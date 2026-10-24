//===- MedPrologueSaves.cpp - Callee-save prologue storage ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedPrologueSaves.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedStackAlignment.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"

#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

bool isEntryLiveInValue(const MedFunc &Func, const MedVar &V, uint64_t RegOff) {
  if (Func.Blocks.empty())
    return false;
  // An entry seed copies the incoming register.  SSA may give the copy a new
  // version (`COPY RCX.1 = RCX` when CL is seeded too) while later reads
  // still name the incoming one; both sides are the incoming value.
  for (const MedOp &Op : Func.Blocks.front().Ops) {
    if (Op.Opcode != NdOp::COPY)
      break;
    if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == RegOff &&
        Op.NumInputs >= 1 && Op.Inputs[0].Kind == MedVar::Reg &&
        Op.Inputs[0].Id == Op.Output.Id && Op.Inputs[0].SSAVer == 0 &&
        (Op.Output == V || Op.Inputs[0] == V))
      return true;
  }
  return false;
}

namespace {

using ValueKey = std::tuple<MedVar::VarKind, int, int>;

ValueKey keyOf(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }

/// Explicit and implicit readers, shared by both source spill proofs. A
/// clobber or PHI is not an ordinary operation that a restore can skip.
struct ValueReads {
  std::map<ValueKey, std::vector<const MedOp *>> Ops;
  std::set<ValueKey> Others;

  explicit ValueReads(const MedFunc &Func) {
    auto Note = [&](const MedVar &V, const MedOp *By) {
      if (V.isConst() || V.Id < 0)
        return;
      if (By)
        Ops[keyOf(V)].push_back(By);
      else
        Others.insert(keyOf(V));
    };
    for (const MedBlock &Block : Func.Blocks) {
      for (const PhiNode &Phi : Block.Phis) {
        for (const auto &Incoming : Phi.Args)
          Note(Incoming.second, nullptr);
        if (Phi.ExceptionalEntry)
          Note(*Phi.ExceptionalEntry, nullptr);
      }
      for (const MedOp &Op : Block.Ops)
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          Note(Op.Inputs[I], &Op);
    }
    for (const MedCallClobber &Clobber : Func.CallClobbers) {
      Note(Clobber.Value, nullptr);
      Note(Clobber.PreservedInput, nullptr);
    }
  }

  bool unread(const MedVar &V) const {
    return !Ops.count(keyOf(V)) && !Others.count(keyOf(V));
  }

  const MedOp *singleReader(const MedVar &V) const {
    const auto It = Ops.find(keyOf(V));
    return !Others.count(keyOf(V)) && It != Ops.end() && It->second.size() == 1
               ? It->second.front()
               : nullptr;
  }
};

/// A load or store of the bytes at Offset from the entry stack pointer.
struct FrameAccess {
  int64_t Offset = 0;
  uint16_t Size = 0;
  MedOp *Op = nullptr;

  bool overlaps(int64_t Begin, int64_t End) const {
    return Offset < End && Begin < Offset + Size;
  }
};

/// Validate the start and remaining space before computing an end offset.
/// An extreme positive offset must not wrap into the private frame or a home
/// slot. A zero-width access cannot establish private storage either.
bool isPrivateFrameAccess(const MedFunc &Func,
                          const IntegerArgumentLayout &Layout, int64_t Offset,
                          uint16_t Bytes) {
  if (!Bytes)
    return false;
  if (Func.FrameSize > 0 && Offset >= -Func.FrameSize && Offset < 0 &&
      Bytes <= -Offset)
    return true;
  const int64_t HomeBegin = Layout.EntryStackBase - Layout.CallStackBase;
  return Layout.CallStackBase && Offset >= HomeBegin &&
         Offset < Layout.EntryStackBase &&
         Bytes <= Layout.EntryStackBase - Offset;
}

/// Every plain load and store of \p Func whose address resolves against the
/// entry stack pointer, all by one proof.
void frameAccessesOf(MedFunc &Func, Arch Architecture, BinaryFormat Format,
                     StackEntryKind Entry, std::vector<FrameAccess> &Loads,
                     std::vector<FrameAccess> &Stores) {
  std::vector<MedOp *> Accesses;
  for (MedBlock &Block : Func.Blocks)
    for (MedOp &Op : Block.Ops)
      if ((Op.Opcode == NdOp::LOAD ||
           (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)) &&
          Op.NumInputs >= 1 && Op.MemoryOrdering == NdMemoryOrdering::None &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
        Accesses.push_back(&Op);
  std::vector<MedVar> Addresses;
  Addresses.reserve(Accesses.size());
  for (const MedOp *Op : Accesses)
    Addresses.push_back(Op->Inputs[0]);
  const std::vector<std::optional<int64_t>> Offsets =
      entryStackOffsets(Func, Addresses, Architecture, Format, Entry);
  for (size_t I = 0; I < Accesses.size(); ++I) {
    if (!Offsets[I])
      continue;
    MedOp &Op = *Accesses[I];
    if (Op.Opcode == NdOp::LOAD)
      Loads.push_back({*Offsets[I], Op.Output.Size, &Op});
    else
      Stores.push_back({*Offsets[I], Op.Inputs[1].Size, &Op});
  }
}

void markCalleeSavePrologueSaves(MedFunc &Func, Arch Architecture,
                                 BinaryFormat Format, StackEntryKind Entry,
                                 std::vector<FrameAccess> &Loads,
                                 std::vector<FrameAccess> &Stores,
                                 const ValueReads &Reads) {
  const auto &TRI = getTargetRegInfo(Architecture);
  const BinaryFormat Convention =
      Func.CC == CallingConv::Win64 ? BinaryFormat::COFF : BinaryFormat::ELF;
  // Win64 also hands the callee its register arguments' home slots, above the
  // return address: MSVC saves nonvolatile registers there.
  const IntegerArgumentLayout Layout =
      TRI.integerArgumentLayout(Convention == BinaryFormat::COFF);

  frameAccessesOf(Func, Architecture, Format, Entry, Loads, Stores);

  std::vector<MedOp *> Saves;
  for (const FrameAccess &Save : Stores) {
    const MedVar &Saved = Save.Op->Inputs[1];
    if (Saved.Kind != MedVar::Reg || !Saved.Size ||
        !isEntryLiveInValue(Func, Saved, Saved.RegOff) ||
        !(TRI.isCallPreserved(Saved.RegOff, Saved.Size, Convention) ||
          TRI.isFramePointer(Saved.RegOff) || TRI.isLinkRegister(Saved.RegOff)))
      continue;
    if (!isPrivateFrameAccess(Func, Layout, Save.Offset, Saved.Size))
      continue;
    const int64_t End = Save.Offset + Saved.Size;
    // A later store into the same bytes makes them storage the body uses.
    if (llvm::any_of(Stores, [&](const FrameAccess &Other) {
          return Other.Op != Save.Op && Other.overlaps(Save.Offset, End);
        }))
      continue;
    auto SameRegister = [&](const MedVar &V) {
      return V.Kind == MedVar::Reg && V.RegOff == Saved.RegOff &&
             V.Size == Saved.Size;
    };
    // Each read of the bytes is a restore nothing uses: a load into that
    // register, or into a value only a copy into it takes, or a load no one
    // reads.  Those loads read nothing a source statement uses either.
    const bool OnlyRestored = llvm::all_of(Loads, [&](const FrameAccess &Read) {
      if (!Read.overlaps(Save.Offset, End))
        return true;
      if (Read.Offset != Save.Offset || Read.Size != Saved.Size)
        return false;
      const MedVar &Out = Read.Op->Output;
      if (Reads.unread(Out))
        return true;
      const MedOp *Reader = Reads.singleReader(Out);
      if (SameRegister(Out) || !Reader)
        return false;
      const MedOp &Copy = *Reader;
      return Copy.Opcode == NdOp::COPY && Copy.NumInputs == 1 &&
             SameRegister(Copy.Output) && Reads.unread(Copy.Output);
    });
    if (OnlyRestored)
      Saves.push_back(Save.Op);
  }
  for (MedOp *Save : Saves)
    Save->PrologueSave = true;
}

bool hasStackReturnAddress(Arch Architecture, StackEntryKind Entry) {
  const auto &TRI = getTargetRegInfo(Architecture);
  return TRI.CallPushesReturnAddress && Entry == StackEntryKind::Call &&
         TRI.PointerSize;
}

void markReturnAddressReads(unsigned PointerSize,
                            llvm::ArrayRef<FrameAccess> Loads,
                            llvm::ArrayRef<FrameAccess> Stores) {
  // A store into the slot replaces the return address, as a function that
  // returns somewhere else does.
  if (llvm::any_of(Stores, [&](const FrameAccess &Write) {
        return Write.overlaps(0, PointerSize);
      }))
    return;
  for (const FrameAccess &Read : Loads)
    if (Read.Offset == 0 && Read.Size == PointerSize)
      Read.Op->ReturnAddressRead = true;
}

/// A status-word transfer has no implicit access to the lifted frame: the x86
/// lifter already makes PUSHF's store and POPF's load explicit. Keep the
/// intrinsic and its architectural effects; this is not a purity contract.
bool isExplicitFlagsTransfer(const MedOp &Op, Arch Architecture) {
  if ((Architecture != Arch::X86 && Architecture != Arch::X64) ||
      Op.NumInputs == 0 || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      !Op.IntrinsicOutputs.empty())
    return false;
  const auto ValidWidth = [&](uint16_t Size) {
    return Size == 2 || Size == getTargetRegInfo(Architecture).PointerSize;
  };
  const auto Scalar = [](const MedVar &V) {
    return V.isConst() || V.Kind == MedVar::Temp || V.Kind == MedVar::Reg;
  };
  switch (static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) {
  case Intrinsic::Pushf:
    return Op.NumInputs == 1 && !Op.Output.isConst() && Scalar(Op.Output) &&
           Op.Output.Id >= 0 && ValidWidth(Op.Output.Size);
  case Intrinsic::Popf:
    return Op.NumInputs == 2 && Op.Output.Size == 0 && Scalar(Op.Inputs[1]) &&
           ValidWidth(Op.Inputs[1].Size);
  default:
    return false;
  }
}

/// Scalar moves, address calculations and flag updates with no implicit
/// observer or trapping operation. Both scratch proofs use this boundary.
bool isSpillScalarStep(NdOp Opcode) {
  switch (Opcode) {
  case NdOp::NOP:
  case NdOp::COPY:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_MULT:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESS:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NEG2:
  case NdOp::INT_NOT:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::BOOL_NOT:
  case NdOp::CONCAT:
  case NdOp::SUBBYTES:
  case NdOp::SELECT:
    return true;
  default:
    return false;
  }
}

/// Saving a scalar into scratch storage does not read its value
/// when every path overwrites the bytes before observing them. Keep the proof
/// in source preparation so HighC and LLVMC discard the same transport. Calls,
/// opaque memory accesses and exceptional edges stop the proof; a partial
/// overwrite kills only its own bytes. Operation positions remain unchanged.
void markUnobservedStackSpills(MedFunc &Func, Arch Architecture,
                               BinaryFormat Format,
                               llvm::ArrayRef<FrameAccess> Loads,
                               llvm::ArrayRef<FrameAccess> Stores,
                               const ValueReads &Reads) {
  if (Func.Blocks.empty() || Func.SkippedSSA || Func.AnalystStackFrame)
    return;
  const auto &TRI = getTargetRegInfo(Architecture);
  const auto Layout = TRI.integerArgumentLayout(Format);
  const auto Private = [&](int64_t At, uint16_t Bytes) {
    return isPrivateFrameAccess(Func, Layout, At, Bytes);
  };
  std::map<const MedOp *, const FrameAccess *> Accesses;
  for (const FrameAccess &Access : Loads)
    Accesses.emplace(Access.Op, &Access);
  for (const FrameAccess &Access : Stores)
    Accesses.emplace(Access.Op, &Access);
  std::map<int, size_t> Blocks;
  std::set<va_t> BlockEntries;
  std::map<const MedOp *, std::pair<size_t, size_t>> Positions;
  for (size_t B = 0; B < Func.Blocks.size(); ++B) {
    Blocks.emplace(Func.Blocks[B].Id, B);
    BlockEntries.insert(Func.Blocks[B].StartAddr);
    for (size_t I = 0; I < Func.Blocks[B].Ops.size(); ++I) {
      const MedOp &Op = Func.Blocks[B].Ops[I];
      Positions.emplace(&Op, std::pair{B, I});
    }
  }
  size_t Budget = limits::ir::kSourceEntrySpillWork;
  auto UnusedRestore = [&](const MedOp &Load) {
    const MedOp *Definition = &Load;
    if (!Reads.unread(Load.Output)) {
      Definition = Reads.singleReader(Load.Output);
      if (!Definition || Definition->Opcode != NdOp::COPY ||
          Definition->NumInputs != 1 || !Reads.unread(Definition->Output))
        return false;
    }
    const MedVar &Value = Definition->Output;
    if (Value.Kind == MedVar::Temp)
      return true;
    // Return and preserved registers have implicit exit observers. A call's
    // arguments may also be completed later: lack of an SSA reader alone
    // cannot prove a physical scratch register dead before that call.
    if (Value.Kind != MedVar::Reg || !TRI.isGeneralReg(Value.RegOff) ||
        TRI.isReturnReg(Value.RegOff) || TRI.isReturnControlReg(Value.RegOff) ||
        llvm::is_contained(TRI.IntReturnRegs, Value.RegOff) ||
        TRI.isCallPreserved(Value.RegOff, Value.Size, Format) ||
        (Func.CC == CallingConv::Win64 &&
         TRI.isCallPreserved(Value.RegOff, Value.Size, BinaryFormat::COFF)) ||
        llvm::any_of(Func.MultiReturn, [&](const MedReturnReg &Return) {
          return Return.RegOff == Value.RegOff;
        }))
      return false;
    auto Overlaps = [&](const MedVar &Other) {
      return Other.Kind == MedVar::Reg && Other.Size &&
             (Other.RegOff >= Value.RegOff
                  ? Other.RegOff - Value.RegOff < Value.Size
                  : Value.RegOff - Other.RegOff < Other.Size);
    };
    auto [B, Position] = Positions.at(Definition);
    ++Position;
    std::set<size_t> Seen;
    while (Seen.insert(B).second) {
      const MedBlock &Block = Func.Blocks[B];
      if (!Block.ExceptionalSuccs.empty() ||
          (Position == 0 && !Block.Phis.empty()))
        return false;
      for (size_t I = Position; I < Block.Ops.size(); ++I) {
        if (!Budget)
          return false;
        --Budget;
        const MedOp &Op = Block.Ops[I];
        if (Op.MemoryOrdering != NdMemoryOrdering::None ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          return false;
        for (unsigned K = 0; K < Op.NumInputs; ++K)
          if (Overlaps(Op.Inputs[K]))
            return false;
        if (Op.Opcode == NdOp::RETURN)
          return true;
        if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) {
          if (!Op.NumInputs || !Op.Inputs[0].isConst() ||
              !BlockEntries.count(Op.Inputs[0].ConstVal))
            return false;
        } else if (!isSpillScalarStep(Op.Opcode) && Op.Opcode != NdOp::LOAD &&
                   Op.Opcode != NdOp::STORE &&
                   !(Op.Opcode == NdOp::INTRINSIC &&
                     isExplicitFlagsTransfer(Op, Architecture))) {
          return false;
        }
        if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == Value.RegOff &&
            Op.Output.Size >= Value.Size)
          return true;
      }
      // Follow only straight continuations; a join, branch, cycle or missing
      // block keeps the restore. All work shares the stack-spill budget.
      if (Block.Succs.size() != 1)
        return false;
      const auto Next = Blocks.find(Block.Succs.front());
      if (Next == Blocks.end())
        return false;
      B = Next->second;
      Position = 0;
    }
    return false;
  };
  for (const FrameAccess &Save : Stores) {
    const MedVar &Value = Save.Op->Inputs[1];
    if (Save.Op->PrologueSave || Value.isConst() ||
        Value.Size > sizeof(uint64_t) || !Private(Save.Offset, Value.Size))
      continue;
    const auto [FirstBlock, FirstOp] = Positions.at(Save.Op);
    const uint64_t All = (uint64_t{1} << Value.Size) - 1;
    using State = std::tuple<size_t, size_t, uint64_t>;
    std::vector<State> Work{{FirstBlock, FirstOp + 1, All}};
    std::map<std::pair<size_t, size_t>, uint64_t> Seen;
    bool Safe = true;
    while (Safe && !Work.empty()) {
      auto [B, Begin, Pending] = Work.back();
      Work.pop_back();
      auto &Visited = Seen[{B, Begin}];
      Pending &= ~Visited;
      if (!Pending)
        continue;
      Visited |= Pending;
      const MedBlock &Block = Func.Blocks[B];
      if (!Block.ExceptionalSuccs.empty()) {
        Safe = false;
        break;
      }
      bool Returned = false;
      for (size_t I = Begin; Pending && I < Block.Ops.size(); ++I) {
        if (!Budget) {
          Safe = false;
          break;
        }
        --Budget;
        const MedOp &Op = Block.Ops[I];
        if (Op.MemoryOrdering != NdMemoryOrdering::None ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
          Safe = false;
          break;
        }
        if (Op.Opcode == NdOp::STORE || Op.Opcode == NdOp::LOAD) {
          const auto At = Accesses.find(&Op);
          if (At == Accesses.end() ||
              !Private(At->second->Offset, At->second->Size)) {
            Safe = false;
            break;
          }
          uint64_t Overlap = 0;
          for (unsigned Byte = 0; Byte < Value.Size; ++Byte)
            if (At->second->overlaps(Save.Offset + Byte,
                                     Save.Offset + Byte + 1))
              Overlap |= uint64_t{1} << Byte;
          if (Op.Opcode == NdOp::LOAD && (Pending & Overlap) &&
              !UnusedRestore(Op)) {
            Safe = false;
            break;
          }
          if (Op.Opcode == NdOp::STORE)
            Pending &= ~Overlap;
        } else if (Op.Opcode == NdOp::INTRINSIC &&
                   isExplicitFlagsTransfer(Op, Architecture)) {
          continue;
        } else if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
                   Op.Opcode == NdOp::INTRINSIC ||
                   Op.Opcode == NdOp::INDIR_BR ||
                   Op.Opcode == NdOp::ATOMIC_XCHG ||
                   Op.Opcode == NdOp::ATOMIC_ADD ||
                   Op.Opcode == NdOp::ATOMIC_CMPXCHG) {
          Safe = false;
          break;
        } else if (Op.Opcode == NdOp::RETURN) {
          // An unresolved result may return a frame pointer, including a PHI
          // of different offsets. Only a scalar constant or void return proves
          // the remaining bytes cannot escape through the result.
          for (unsigned K = 0; K < Op.NumInputs; ++K)
            Safe &= Op.Inputs[K].isConst();
          Returned = true;
          break;
        } else if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) {
          if (!Op.NumInputs || !Op.Inputs[0].isConst() ||
              !BlockEntries.count(Op.Inputs[0].ConstVal)) {
            Safe = false;
            break;
          }
        } else if (!isSpillScalarStep(Op.Opcode)) {
          Safe = false;
          break;
        }
      }
      if (!Safe || !Pending || Returned)
        continue;
      if (Block.Succs.empty()) {
        Safe = false;
        break;
      }
      for (int Successor : Block.Succs) {
        const auto Next = Blocks.find(Successor);
        if (Next == Blocks.end()) {
          Safe = false;
          break;
        }
        Work.emplace_back(Next->second, 0, Pending);
      }
    }
    if (Safe)
      Save.Op->PrologueSave = true;
    if (!Budget)
      return;
  }
}

/// An exact declared call operand observes every loaded value byte. A source
/// frame has no initial contents below its entry SP. Prove absence
/// of a write, rather than mistaking absence of a known producer for that
/// proof. Unioning writes from every predecessor is deliberately conservative:
/// one path writing a byte prevents this proof from calling it unwritten.
void replaceUnwrittenCallFrameReads(MedFunc &Func, Arch Architecture,
                                    llvm::ArrayRef<FrameAccess> Loads,
                                    llvm::ArrayRef<FrameAccess> Stores) {
  if (Func.Blocks.empty() || Func.FrameSize <= 0 || Func.SkippedSSA ||
      Func.AnalystStackFrame)
    return;
  size_t Remaining = limits::ir::kUnwrittenSourceFrameReadWork;
  std::set<std::pair<ValueKey, uint16_t>> Required;
  for (const auto &Block : Func.Blocks) {
    if (Remaining < Block.Ops.size())
      return;
    Remaining -= Block.Ops.size();
    for (const auto &Op : Block.Ops) {
      if (!hasBoundIntegerCallArguments(Op))
        continue;
      if (Remaining < Op.NumInputs)
        return;
      Remaining -= Op.NumInputs;
      for (size_t I = 0; I < Op.IntegerCallSignature->Parameters.size(); ++I) {
        const auto &Parameter = Op.IntegerCallSignature->Parameters[I];
        const auto &Value = Op.Inputs[I + 1];
        if (Parameter.Kind == SourceABICarrierKind::Stack && !Value.isConst() &&
            Value.Id >= 0 && Value.Size == Parameter.Bytes)
          Required.emplace(keyOf(Value), Value.Size);
      }
    }
  }
  // General loads may have only their initialized bits observed later. Their
  // partial-byte source model is separate from a full declared argument read.
  if (Required.empty() || Remaining < Stores.size())
    return;
  Remaining -= Stores.size();
  std::map<int, const MedBlock *> Blocks;
  std::map<const MedOp *, std::pair<int, size_t>> Positions;
  std::map<const MedOp *, const FrameAccess *> Writes;
  std::set<std::pair<int, int>> IncomingEdges, OutgoingEdges;
  for (const auto &Write : Stores)
    Writes.emplace(Write.Op, &Write);
  for (const auto &Block : Func.Blocks) {
    if (Remaining <
            1 + Block.Ops.size() + Block.Preds.size() + Block.Succs.size() ||
        !Blocks.emplace(Block.Id, &Block).second)
      return;
    Remaining -= 1 + Block.Ops.size() + Block.Preds.size() + Block.Succs.size();
    for (size_t I = 0; I < Block.Ops.size(); ++I)
      Positions.emplace(&Block.Ops[I], std::pair{Block.Id, I});
    for (int Pred : Block.Preds)
      IncomingEdges.emplace(Pred, Block.Id);
    for (int Succ : Block.Succs)
      OutgoingEdges.emplace(Block.Id, Succ);
  }
  // Exceptional or incomplete graph roots cannot inherit the ordinary frame.
  if (IncomingEdges != OutgoingEdges)
    return;
  for (const auto &[From, To] : IncomingEdges)
    if (!Blocks.count(From) || !Blocks.count(To))
      return;
  for (const auto &Block : Func.Blocks)
    if (!Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty())
      return;
  for (const auto &Read : Loads) {
    if (!Required.count({keyOf(Read.Op->Output), Read.Size}) || Read.Op->Dead ||
        Read.Op->ReturnAddressRead || !Read.Size ||
        Read.Offset < -Func.FrameSize || Read.Offset > -int64_t(Read.Size))
      continue;
    if (Remaining < Read.Size)
      return;
    Remaining -= Read.Size;
    std::vector<bool> Written(Read.Size);
    const auto Position = Positions.at(Read.Op);
    std::vector<std::pair<int, size_t>> Pending{Position};
    std::set<int> Visited;
    bool Complete = true, ReachedEntry = false;
    while (Complete && !Pending.empty()) {
      const auto [Id, End] = Pending.back();
      Pending.pop_back();
      if (!Remaining)
        return;
      --Remaining;
      const auto &Block = *Blocks.at(Id);
      if (!Visited.insert(Id).second) {
        // A backedge to the query also executes its suffix before another
        // observation. Do not publish a first-iteration-only conclusion.
        if (Id == Position.first)
          Complete = false;
        continue;
      }
      ReachedEntry |= Id == Func.Blocks.front().Id;
      if (Block.Preds.empty() && Id != Func.Blocks.front().Id) {
        Complete = false;
        break;
      }
      if (Remaining < End + Block.Preds.size())
        return;
      Remaining -= End + Block.Preds.size();
      for (size_t I = 0; I < End; ++I) {
        const auto &Op = Block.Ops[I];
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
            Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::ATOMIC_XCHG ||
            Op.Opcode == NdOp::ATOMIC_ADD ||
            Op.Opcode == NdOp::ATOMIC_CMPXCHG) {
          Complete = false;
          break;
        }
        if (Op.Opcode != NdOp::STORE)
          continue;
        const auto Write = Writes.find(&Op);
        if (Write == Writes.end()) {
          Complete = false; // An unresolved address might alias this frame.
          break;
        }
        if (Remaining < Read.Size)
          return;
        Remaining -= Read.Size;
        for (size_t Byte = 0; Byte < Read.Size; ++Byte)
          Written[Byte] = Written[Byte] || Write->second->overlaps(
                                               Read.Offset + int64_t(Byte),
                                               Read.Offset + int64_t(Byte) + 1);
      }
      for (int Pred : Block.Preds)
        Pending.emplace_back(Pred, Blocks.at(Pred)->Ops.size());
    }
    if (Complete && ReachedEntry && llvm::is_contained(Written, false)) {
      // Keep the same definition and operation position: call records name
      // both. Native lifting never invokes this source-only preparation.
      Read.Op->Opcode = NdOp::COPY;
      Read.Op->NumInputs = 1;
      Read.Op->Inputs[0] = MedVar::makeUnspecified(Read.Size, Architecture);
    }
  }
}

} // namespace

void markCalleeSavePrologueSaves(MedFunc &Func, Arch Architecture,
                                 BinaryFormat Format, StackEntryKind Entry) {
  std::vector<FrameAccess> Loads, Stores;
  const ValueReads Reads(Func);
  markCalleeSavePrologueSaves(Func, Architecture, Format, Entry, Loads, Stores,
                              Reads);
}

void markReturnAddressReads(MedFunc &Func, Arch Architecture,
                            BinaryFormat Format, StackEntryKind Entry) {
  if (!hasStackReturnAddress(Architecture, Entry))
    return;
  std::vector<FrameAccess> Loads, Stores;
  frameAccessesOf(Func, Architecture, Format, Entry, Loads, Stores);
  markReturnAddressReads(getTargetRegInfo(Architecture).PointerSize, Loads,
                         Stores);
}

void markSourceStackAccesses(MedFunc &Func, Arch Architecture,
                             BinaryFormat Format, StackEntryKind Entry) {
  std::vector<FrameAccess> Loads, Stores;
  const ValueReads Reads(Func);
  markCalleeSavePrologueSaves(Func, Architecture, Format, Entry, Loads, Stores,
                              Reads);
  // Annotate before source-only replacement. No access proof survives the
  // replacements at the end of this preparation.
  if (hasStackReturnAddress(Architecture, Entry))
    markReturnAddressReads(getTargetRegInfo(Architecture).PointerSize, Loads,
                           Stores);
  markUnobservedStackSpills(Func, Architecture, Format, Loads, Stores, Reads);
  replaceUnwrittenCallFrameReads(Func, Architecture, Loads, Stores);
}

} // namespace neverd
