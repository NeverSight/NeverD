//===- MedMIPSReturnTargets.cpp - Exact entry link identity --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind source return targets to captured incoming RA through complete SSA.
//===----------------------------------------------------------------------===//

#include "../../../../safety/StackSlotFlow.h"
#include "../../abi/MedABIPassDetail.h"
#include "MedMIPSRecordFrame.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MIPS/MIPSIntrinsicShapes.h"
#include "neverd/ir/med/MIPS/MIPSReturnTargets.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace neverd {

namespace {

/// Incoming loop edges can assume the previous value of a header PHI only
/// when every path to their predecessor has already visited the header.
std::set<int> loopPredecessors(const MedFunc &F, const MedBlock &Header,
                               size_t &Remaining) {
  if (F.Blocks.size() > Remaining)
    return {};
  Remaining -= F.Blocks.size();
  enum : unsigned { FromEntry = 1, FromHeader = 2 };
  std::vector<unsigned> Reached(F.Blocks.size());
  llvm::SmallVector<int, 8> Work;
  auto visit = [&](int Id, unsigned Paths) {
    if ((Reached[Id] | Paths) != Reached[Id]) {
      Reached[Id] |= Paths;
      Work.push_back(Id);
    }
  };
  visit(0, FromEntry);
  visit(Header.Id, FromHeader);
  while (!Work.empty()) {
    const int Id = Work.pop_back_val();
    const auto &Block = F.Blocks[Id];
    if (Block.Id != Id || Remaining < 1 + Block.Succs.size())
      return {};
    Remaining -= 1 + Block.Succs.size();
    for (int Succ : Block.Succs) {
      if (Succ < 0 || size_t(Succ) >= F.Blocks.size())
        return {};
      if (Succ != Header.Id)
        visit(Succ, Reached[Id]);
    }
  }
  std::set<int> Back;
  for (int Pred : Header.Preds) {
    if (Pred < 0 || size_t(Pred) >= Reached.size() || !Reached[Pred])
      return {};
    if (Reached[Pred] == FromHeader)
      Back.insert(Pred);
  }
  return Back.size() != Header.Preds.size() ? Back : std::set<int>{};
}

} // namespace

bool hasMIPSSourceReturnTargets(const LowFunc &Low, const MedFunc &Function,
                                Arch Target, BinaryFormat Format,
                                unsigned SourceResultOperands) {
  if (auto Error = validateLowInstructionBoundaries(
          Low, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    return false;
  }
  if (Function.SkippedSSA || !Low.hasCompleteLiftCoverage() ||
      Low.ModuleAnalysisRoots.size() > 1 ||
      Low.OrdinaryModuleAnalysisRoots.size() > 1)
    return false;
  using Site = std::pair<va_t, int>;
  std::set<Site> Required;
  std::map<Site, Site> TailReturns;
  for (const auto &Block : Low.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries) {
      if (Boundary.Control == LowInstructionControl::TailCall) {
        // CFG lowering appends a synthetic result return. Its value is not
        // the physical link. First establish a complete, unframed leaf tail:
        // no earlier call, memory, opaque effect, or write of RA/SP can have
        // changed the incoming return context before the terminal transfer.
        if (Low.Blocks.size() != 1 || Boundary.OpCount < 2 ||
            Boundary.FirstOp + Boundary.OpCount != Block.Ops.size())
          return false;
        const auto &Call = Block.Ops[Block.Ops.size() - 2];
        const auto &Return = Block.Ops.back();
        if ((Call.Opcode != NdOp::CALL && Call.Opcode != NdOp::INDIR_CALL) ||
            Return.Opcode != NdOp::RETURN)
          return false;
        const unsigned Width = isMIPS64(Target) ? 8 : 4;
        for (size_t I = 0; I < Block.Ops.size(); ++I) {
          const auto &Op = Block.Ops[I];
          if (Op.Output.isReg() && Op.Output.Size)
            for (uint64_t Register : {mipsreg::RA, mipsreg::SP})
              if (Op.Output.Offset < Register + Width &&
                  Register < Op.Output.Offset + Op.Output.Size)
                return false;
          if (I + 2 >= Block.Ops.size())
            continue;
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
              Op.Opcode == NdOp::RETURN || Op.Opcode == NdOp::BRANCH ||
              Op.Opcode == NdOp::COND_BR || Op.Opcode == NdOp::INDIR_BR ||
              Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE ||
              Op.Opcode == NdOp::INTRINSIC)
            return false;
        }
        TailReturns.emplace(Site{Return.Addr, Return.Seq},
                            Site{Call.Addr, Call.Seq});
      } else {
        for (size_t I = Boundary.FirstOp;
             I < Boundary.FirstOp + Boundary.OpCount; ++I)
          if (const auto &Op = Block.Ops[I]; Op.Opcode == NdOp::RETURN)
            Required.emplace(Op.Addr, Op.Seq);
      }
    }
  if (Required.empty() && TailReturns.empty())
    return true;
  std::set<Site> QualifiedTails;
  using Key = std::tuple<int, int, int, uint64_t, uint16_t>;
  const auto key = [](const MedVar &V) -> Key {
    return {int(V.Kind), V.Id, V.SSAVer, V.RegOff, V.Size};
  };
  std::vector<MedVar> MemoryAddresses;
  std::map<const MedBlock *, int> BlockIndices;
  bool QualifiedMemory = true;
  size_t CallCount = 0;
  llvm::SmallDenseMap<const MedBlock *, size_t, 4> NativePrefixes;
  struct CallFrame {
    llvm::SmallVector<SourceABIValueLocation, 8> Inputs;
    unsigned FloatingResultBytes = 0;
  };
  std::map<const MedOp *, CallFrame> Calls;
  size_t Remaining = limits::ir::kSourceFrameAnalysisRemaining;
  for (size_t I = 0; I < Function.Blocks.size(); ++I) {
    const auto &Block = Function.Blocks[I];
    if (!Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty())
      return false;
    BlockIndices.emplace(&Block, I);
    for (const auto &Op : Block.Ops) {
      const bool NoDataWrite =
          Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
          Op.Inputs[0].isConst() &&
          (Op.Inputs[0].ConstVal == uint64_t(Intrinsic::MIPSRequireWord) ||
           mips::isBarrier(static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) ||
           mips::isDSPControlIntrinsic(
               static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) ||
           mips::isPureScalarValue(
               static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) ||
           mips::isScalarFPArithmetic(
               static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) ||
           mips::isScalarFPControl(
               static_cast<Intrinsic>(Op.Inputs[0].ConstVal))) &&
          mipsScalarMedShapeIsValid(Op, Target);
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) && Op.NumInputs)
        MemoryAddresses.push_back(Op.Inputs[0]);
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        ++CallCount;
        if (Op.SourceCallHint && Op.SourceCallHint->Signature.MIPSRecords) {
          const auto &Signature = Op.SourceCallHint->Signature;
          const auto Pieces = sourceABIParameters(Signature);
          if (Op.NumInputs != Pieces.size() + 1)
            return false;
          auto &Frame = Calls[&Op];
          for (const auto &P : Pieces)
            Frame.Inputs.push_back(P.Location);
          if (Signature.ReturnType->Kind == NdTypeKind::Float ||
              (!Signature.ReturnComponents.empty() &&
               llvm::all_of(Signature.ReturnComponents, [](const auto &P) {
                 return P.Kind == SourceABICarrierKind::FloatingRegister;
               })))
            Frame.FloatingResultBytes = Signature.ReturnType->Size;
        } else if (const auto Signature =
                       boundFloatingCallArguments(Op, Target, Format)) {
          auto &Frame = Calls[&Op];
          for (const auto &P : Signature->Scalars)
            Frame.Inputs.push_back(
                {P.Kind, P.Register, P.StackOffset, P.Bytes});
          if (Signature->ReturnKind == NdTypeKind::Float)
            Frame.FloatingResultBytes = Signature->ReturnBytes;
        } else {
          NativePrefixes[&Block] = &Op - Block.Ops.data();
        }
        if (Op.IsTailCall && Calls.contains(&Op))
          QualifiedTails.emplace(Op.Addr, Op.OriginSeq);
      }
      if ((Op.Opcode == NdOp::INTRINSIC && !NoDataWrite &&
           (Op.NumInputs != 3 || !Op.Inputs[0].isConst() ||
            Op.Inputs[0].ConstVal != uint64_t(Intrinsic::RequireAligned))) ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Op.MemoryOrdering != NdMemoryOrdering::None)
        QualifiedMemory = false;
    }
  }
  const bool HasCalls = CallCount != 0;
  const auto CallSP = HasCalls && QualifiedMemory
                          ? sourceCallStackPointers(Function, Target, Remaining)
                          : std::map<const MedOp *, MedVar>{};
  for (const auto &[Call, SP] : CallSP)
    MemoryAddresses.push_back(SP);
  std::map<Key, std::optional<int64_t>> Offsets;
  if (QualifiedMemory) {
    const auto Values =
        entryStackOffsets(Function, MemoryAddresses, Target, Format);
    for (size_t I = 0; I < Values.size(); ++I)
      Offsets.emplace(key(MemoryAddresses[I]), Values[I]);
  }
  const auto Offset = [&](const MedVar &V) -> std::optional<int64_t> {
    const auto It = Offsets.find(key(V));
    return It == Offsets.end() ? std::nullopt : It->second;
  };
  MedDefSiteIndex Definitions(Function);
  safety::detail::ExactPhiGraph Graph(Function);
  // A native call can observe earlier register writes. A predecessor also
  // reaches it through its entire body, including a later loop iteration.
  llvm::SmallVector<const MedBlock *, 8> PendingCalls;
  if (QualifiedMemory)
    for (const auto &Prefix : NativePrefixes)
      PendingCalls.push_back(Prefix.first);
  while (!PendingCalls.empty()) {
    const MedBlock *Block = PendingCalls.pop_back_val();
    if (Remaining < 1 + Block->Preds.size() ||
        !Graph.hasCompleteNormalPredecessors(*Block)) {
      QualifiedMemory = false;
      break;
    }
    Remaining -= 1 + Block->Preds.size();
    for (int Id : Block->Preds) {
      const MedBlock *Pred = Graph.blockById(Id);
      auto [Prefix, New] = NativePrefixes.try_emplace(Pred, Pred->Ops.size());
      if (New)
        PendingCalls.push_back(Pred);
      else
        Prefix->second = Pred->Ops.size();
    }
  }
  const auto BeforeFrameEscape = [&](const MedBlock &LoadBlock,
                                     size_t LoadIndex) {
    // An external read cannot recover this function's frame address before
    // any operation publishes it. Private stack saves are harmless here;
    // external stores and calls need a stronger memory contract. Visit every
    // predecessor, including a loop's later operations on its back edge.
    llvm::SmallPtrSet<const MedBlock *, 16> Seen;
    std::vector<std::pair<const MedBlock *, size_t>> Pending{
        {&LoadBlock, LoadIndex}};
    bool ReachesEntry = false;
    while (!Pending.empty()) {
      const auto [Block, End] = Pending.back();
      Pending.pop_back();
      if (End == Block->Ops.size() && !Seen.insert(Block).second)
        continue;
      if (!Graph.hasCompleteNormalPredecessors(*Block))
        return false;
      for (size_t I = 0; I < End; ++I) {
        if (!Remaining)
          return false;
        --Remaining;
        const auto &Op = Block->Ops[I];
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
          return false;
        if (Op.Opcode != NdOp::STORE)
          continue;
        const auto *Address = safety::detail::memoryAddress(Op);
        const auto *Value = safety::detail::storedValue(Op);
        const auto At = Address ? Offset(*Address) : std::nullopt;
        if (!Value || !At || *At >= 0 ||
            *At < -limits::ir::kSourceFrameAnalysisMaxFrame ||
            *At + Value->Size > 0)
          return false;
      }
      if (Block->isFunctionEntry(Function.Entry))
        ReachesEntry = true;
      else if (Block->Preds.empty())
        return false;
      for (int Id : Block->Preds) {
        const auto *Pred = Graph.blockById(Id);
        Pending.emplace_back(Pred, Pred->Ops.size());
      }
    }
    return ReachesEntry;
  };
  std::map<Key, bool> Results;
  std::set<Key> Active;
  const auto &TRI = getTargetRegInfo(Target);
  using SliceKey = std::tuple<Key, unsigned, unsigned>;
  std::set<SliceKey> ProvenNonFrame;
  std::set<std::pair<SliceKey, bool>> NonFrameActive;
  std::optional<Key> AssumedPhi;
  std::map<int, std::set<int>> LoopEdges;
  size_t Cycles = 0;
  std::function<bool(const MedVar &, unsigned, unsigned)> NonFrameBytes;
  const auto IsNonFrame = [&](const MedVar &V) {
    return NonFrameBytes(V, 0, V.Size);
  };
  NonFrameBytes = [&](const MedVar &V, unsigned First, unsigned Bytes) {
    if (!Bytes || First + Bytes > V.Size)
      return false;
    if (V.isConst())
      return true;
    const SliceKey K{key(V), First, Bytes};
    if (AssumedPhi == key(V) || ProvenNonFrame.count(K))
      return true;
    if (!Remaining ||
        NonFrameActive.size() >= limits::ir::kMIPSReturnIdentityDepth)
      return false;
    const auto Query = std::pair(K, AssumedPhi.has_value());
    if (!NonFrameActive.insert(Query).second) {
      ++Cycles;
      return false;
    }
    --Remaining;
    const auto D = Definitions.find(V);
    bool Safe = false;
    if (!D.Ambiguous && safety::detail::isAuthenticatedNonFrameEntryRegister(
                            Function, V, TRI.StackPointer, TRI.FramePointer)) {
      Safe = true;
    } else if (!D.Ambiguous && D.Phi &&
               Graph.hasCompleteIncoming(BlockIndices.at(D.Block), *D.Phi)) {
      const size_t Before = Cycles;
      Safe = llvm::all_of(D.Phi->Args, [&](const auto &A) {
        return NonFrameBytes(A.second, First, Bytes);
      });
      if (!Safe && !AssumedPhi && Cycles != Before &&
          D.Block->Id == BlockIndices.at(D.Block)) {
        auto [It, New] = LoopEdges.try_emplace(D.Block->Id);
        if (New)
          It->second = loopPredecessors(Function, *D.Block, Remaining);
        const auto &Back = It->second;
        // The initial value must be non-frame in full before an update may
        // use any of its bytes, even when the original query was a slice.
        if (!Back.empty() && llvm::all_of(D.Phi->Args, [&](const auto &A) {
              return Back.contains(A.first) || IsNonFrame(A.second);
            })) {
          AssumedPhi = key(V);
          Safe = llvm::all_of(D.Phi->Args, [&](const auto &A) {
            return !Back.contains(A.first) || IsNonFrame(A.second);
          });
          AssumedPhi.reset();
        }
      }
    } else if (!D.Ambiguous && D.Op) {
      const auto &Op = *D.Op;
      // External memory cannot contain a pointer into this newly allocated
      // frame unless it escaped. PreservesCall separately requires that no
      // store or call operand exposes it, including before the RA save.
      switch (Op.Opcode) {
      case NdOp::COPY:
      case NdOp::CAST:
        Safe = Op.NumInputs == 1 && NonFrameBytes(Op.Inputs[0], First, Bytes);
        break;
      case NdOp::SUBBYTES:
        Safe =
            Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
            Op.Inputs[1].ConstVal <= Op.Inputs[0].Size &&
            NonFrameBytes(Op.Inputs[0], First + Op.Inputs[1].ConstVal, Bytes);
        break;
      case NdOp::CONCAT:
        if (Op.NumInputs == 2) {
          const auto Low = Op.Inputs[1].Size;
          const auto End = First + Bytes;
          Safe = (First >= Low ||
                  NonFrameBytes(Op.Inputs[1], First,
                                std::min<unsigned>(End, Low) - First)) &&
                 (End <= Low ||
                  NonFrameBytes(Op.Inputs[0], First > Low ? First - Low : 0,
                                End - std::max<unsigned>(First, Low)));
        }
        break;
      case NdOp::LOAD:
        if (Op.NumInputs == 1 && QualifiedMemory) {
          if (HasCalls && IsNonFrame(Op.Inputs[0]))
            Safe = true;
          else if (const auto At = Offset(Op.Inputs[0]);
                   At && *At < 0 && *At + Op.Output.Size <= 0) {
            // Fixed stack arguments are loaded into explicit call operands.
            // Trace their exact reaching stores; a frame address stored there
            // is still an escape, and an intervening unknown call invalidates
            // the proof. Only the bytes this operand reads matter.
            const auto FromStores = [&](auto &&Self, unsigned Begin,
                                        unsigned Size) -> bool {
              const unsigned Start = std::max(Begin, First);
              const unsigned End = std::min(Begin + Size, First + Bytes);
              if (Start >= End)
                return true;
              if (!Remaining)
                return false;
              --Remaining;
              const unsigned MemoryByte = archLittleEndian(Target)
                                              ? Begin
                                              : Op.Output.Size - Begin - Size;
              const auto Reaching = safety::detail::reachingStackValues(
                  Function, D.Block->Id, int(&Op - D.Block->Ops.data()),
                  *At + MemoryByte, Size, Offset,
                  [&](const MedVar &Address) { return !IsNonFrame(Address); },
                  [](const MedVar &) { return true; }, true);
              if (!Reaching.Reachable)
                return false;
              if (Reaching.Complete)
                return llvm::all_of(Reaching.Values, [&](const MedVar &Stored) {
                  return NonFrameBytes(Stored, Start - Begin, End - Start);
                });
              // GCC can assemble one double from separate word stores.
              // Prove every requested byte on every predecessor, retaining
              // the existing rejection of unknown writes and frame escapes.
              if (Size == 1)
                return false;
              const unsigned Half = Size / 2;
              return Self(Self, Begin, Half) &&
                     Self(Self, Begin + Half, Size - Half);
            };
            Safe = FromStores(FromStores, 0, Op.Output.Size);
          } else if (!At && IsNonFrame(Op.Inputs[0])) {
            Safe = BeforeFrameEscape(*D.Block, &Op - D.Block->Ops.data());
          }
        }
        break;
      case NdOp::CALL:
      case NdOp::INDIR_CALL:
        // Follow a bound numerical result only when the target and every
        // argument have no provenance in this frame. Spilling that result
        // beside the saved link does not escape the link slot. Unknown and
        // pointer-returning calls still provide no such proof.
        if (const auto Call = Calls.find(&Op);
            Call != Calls.end() &&
            First + Bytes <= Call->second.FloatingResultBytes) {
          Safe = IsNonFrame(Op.Inputs[0]);
          for (size_t I = 0; Safe && I < Call->second.Inputs.size(); ++I)
            Safe = NonFrameBytes(Op.Inputs[I + 1], 0,
                                 Call->second.Inputs[I].ValueBytes);
        }
        break;
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
      case NdOp::INT_ADD:
      case NdOp::INT_SUB:
      case NdOp::INT_AND:
      case NdOp::INT_OR:
      case NdOp::INT_XOR:
      case NdOp::INT_MULT:
      case NdOp::INT_LEFT:
      case NdOp::SELECT:
        Safe = Op.NumInputs &&
               llvm::all_of(llvm::ArrayRef(Op.Inputs.data(), Op.NumInputs),
                            IsNonFrame);
        break;
      case NdOp::INTRINSIC:
        Safe =
            Op.NumInputs && Op.Inputs[0].isConst() &&
            mips::isScalarFPArithmetic(
                static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) &&
            mipsScalarMedShapeIsValid(Op, Target) &&
            llvm::all_of(llvm::ArrayRef(Op.Inputs.data() + 1, Op.NumInputs - 1),
                         IsNonFrame);
        break;
      default:
        break;
      }
    }
    NonFrameActive.erase(Query);
    // A reaching-store walk can visit a later call-result spill while that
    // call's argument is still being proved. Such a recursive failure depends
    // on the active query; only completed positive proofs are reusable.
    if (Safe && !AssumedPhi)
      ProvenNonFrame.insert(K);
    return Safe;
  };
  const auto IntegerLayout = TRI.integerArgumentLayout(Format);
  const auto CallPrivateStart =
      [&](const MedOp &Call) -> std::optional<int64_t> {
    const auto P = CallSP.find(&Call);
    if (P == CallSP.end())
      return std::nullopt;
    const auto SP = Offset(P->second);
    if (!SP || *SP % (isMIPSO32(Target) ? 8 : 16) ||
        *SP < -limits::ir::kSourceFrameAnalysisMaxFrame)
      return std::nullopt;
    const int64_t SlotBytes = isMIPSO32(Target) ? 4 : 8;
    int64_t Extent = isMIPSO32(Target) ? 16 : 0;
    if (const auto S = Calls.find(&Call); S != Calls.end()) {
      for (const auto &A : S->second.Inputs)
        if (A.Kind == SourceABICarrierKind::Stack) {
          const auto End = A.EntryStackOffset + A.ValueBytes;
          // A big-endian n64 float occupies the first word, but the callee
          // still owns its complete eight-byte argument slot.
          Extent =
              std::max(Extent, (End + SlotBytes - 1) / SlotBytes * SlotBytes);
        }
    } else if (Call.CalleeStackArgs > int(IntegerLayout.Registers.size())) {
      Extent = std::max<int64_t>(
          Extent, IntegerLayout.CallStackBase +
                      (Call.CalleeStackArgs - IntegerLayout.Registers.size()) *
                          SlotBytes);
    }
    return *SP + Extent;
  };
  // A saved frame pointer does not publish its value when every call leaves
  // the complete spill slot private. Use the same outgoing-area boundary as
  // the saved-link proof, but require a complete argument extent before
  // admitting pointer spills. An unknown or open variadic call can read more
  // stack arguments than the ABI's minimum outgoing area.
  std::optional<int64_t> SharedPrivateStart;
  if (CallSP.size() == CallCount)
    for (const auto &Entry : CallSP) {
      const auto &Call = *Entry.first;
      const auto Start = CallPrivateStart(Call);
      if (!Start ||
          (!Calls.contains(&Call) &&
           (Call.CalleeStackArgs < 0 || Call.CalleeVariadicFrom >= 0 ||
            Call.CalleeVariadicRegisterFrom >= 0))) {
        SharedPrivateStart.reset();
        break;
      }
      SharedPrivateStart =
          SharedPrivateStart ? std::max(*SharedPrivateStart, *Start) : *Start;
    }
  const auto PrivateFrameStore = [&](const MedOp &Op) {
    const auto At = Offset(Op.Inputs[0]);
    return SharedPrivateStart && At && *At >= *SharedPrivateStart &&
           *At + Op.Inputs[1].Size <= 0;
  };
  // Native calls preserve the caller's allocated private frame under the
  // platform ABI even when the callee has no fixed declaration. Check every
  // possible argument definition that can reach a native call. Registers
  // merged after the last call cannot expose the frame to that callee.
  // Check all stores and calls so an escape before the RA save cannot be
  // forgotten when reachingStackValues sees the later exact store. Reloading
  // a private pointer into an argument still has to prove non-frame provenance.
  bool UnexposedFrame = QualifiedMemory;
  bool NativeArgumentsNonFrame = true;
  const auto FPArguments = TRI.floatingParamRegs(Format);
  const auto CheckNativeArgument = [&](const MedVar &Value) {
    if (Value.Kind != MedVar::Reg || !Value.Size)
      return true;
    const auto Overlaps = [&](uint64_t Register, uint16_t Bytes) {
      return Value.RegOff < Register + Bytes &&
             Register < Value.RegOff + Value.Size;
    };
    const bool Argument =
        llvm::any_of(IntegerLayout.Registers,
                     [&](uint64_t Register) {
                       return Overlaps(Register, TRI.FullRegWidth);
                     }) ||
        llvm::any_of(FPArguments, [&](uint64_t Register) {
          return Overlaps(Register, TRI.FPABIRegWidth);
        });
    return !Argument || IsNonFrame(Value);
  };
  if (HasCalls && QualifiedMemory)
    for (const auto &Block : Function.Blocks) {
      const auto Prefix = NativePrefixes.find(&Block);
      if (Prefix != NativePrefixes.end())
        for (const auto &Phi : Block.Phis)
          NativeArgumentsNonFrame &= CheckNativeArgument(Phi.Output);
      const size_t End = Prefix == NativePrefixes.end() ? 0 : Prefix->second;
      for (size_t OpIndex = 0; OpIndex < Block.Ops.size(); ++OpIndex) {
        const auto &Op = Block.Ops[OpIndex];
        if (OpIndex < End) {
          NativeArgumentsNonFrame &= CheckNativeArgument(Op.Output);
          for (const auto &Value : Op.IntrinsicOutputs)
            NativeArgumentsNonFrame &= CheckNativeArgument(Value);
        }
        if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2)
          UnexposedFrame &= PrivateFrameStore(Op) || IsNonFrame(Op.Inputs[1]);
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
          if (!Op.NumInputs) {
            UnexposedFrame = false;
          } else {
            UnexposedFrame &= IsNonFrame(Op.Inputs[0]);
            const auto Signature = Calls.find(&Op);
            for (unsigned I = 1; I < Op.NumInputs; ++I)
              UnexposedFrame &= NonFrameBytes(
                  Op.Inputs[I], 0,
                  Signature == Calls.end()
                      ? Op.Inputs[I].Size
                      : Signature->second.Inputs[I - 1].ValueBytes);
          }
        }
      }
    }
  UnexposedFrame &= NativeArgumentsNonFrame;
  const auto PreservesCall = [&](const MedOp &Call, int64_t At, uint16_t Size) {
    if (!UnexposedFrame)
      return false;
    const auto Start = CallPrivateStart(Call);
    return Start && At >= *Start && At + Size <= 0;
  };
  const uint16_t LinkBytes = isMIPS64(Target) ? 8 : 4;
  std::function<bool(const MedVar &)> Prove = [&](const MedVar &Value) {
    if (!Remaining || Value.isConst() || Value.Size != LinkBytes ||
        Active.size() >= limits::ir::kMIPSReturnIdentityDepth)
      return false;
    --Remaining;
    const Key Identity = key(Value);
    if (const auto It = Results.find(Identity); It != Results.end())
      return It->second;
    if (!Active.insert(Identity).second)
      return false;
    const auto Definition = Definitions.find(Value);
    bool Valid = false;
    if (!Definition.Ambiguous && Value.Kind == MedVar::Reg &&
        Value.RegOff == mipsreg::RA &&
        safety::detail::isAuthenticatedEntryRegisterLiveIn(Function, Value)) {
      Valid = true;
    } else if (!Definition.Ambiguous && Definition.Phi &&
               Graph.hasCompleteIncoming(BlockIndices.at(Definition.Block),
                                         *Definition.Phi)) {
      Valid = llvm::all_of(Definition.Phi->Args,
                           [&](const auto &Arg) { return Prove(Arg.second); });
    } else if (!Definition.Ambiguous && Definition.Op) {
      const auto &Op = *Definition.Op;
      const auto Zero = [&](unsigned I) {
        return Op.Inputs[I].isConst() && Op.Inputs[I].ConstVal == 0;
      };
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
        Valid = Prove(Op.Inputs[0]);
      else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_OR ||
                Op.Opcode == NdOp::INT_XOR) &&
               Op.NumInputs == 2)
        Valid = (Zero(0) && Prove(Op.Inputs[1])) ||
                (Zero(1) && Prove(Op.Inputs[0]));
      else if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
               QualifiedMemory) {
        const auto At = Offset(Op.Inputs[0]);
        if (At && *At <= -int64_t(LinkBytes) &&
            *At >= -limits::ir::kSourceFrameAnalysisMaxFrame) {
          const auto Reaching = safety::detail::reachingStackValues(
              Function, Definition.Block->Id,
              int(&Op - Definition.Block->Ops.data()), *At, LinkBytes, Offset,
              [&](const MedVar &V) {
                return HasCalls ? !IsNonFrame(V) : !V.isConst();
              },
              // The complete function proof already distinguishes private
              // pointer spills from escapes. Do not invalidate the same
              // slot merely because one of those private stores contains FP
              // or SP. Exact writes, overlaps and each call's outgoing area
              // are still checked by this walk and PreservesCall.
              [](const MedVar &) { return true; }, true,
              HasCalls && !UnexposedFrame, PreservesCall);
          // Exact stores and complete calls must preserve this private slot.
          // Escaped frame addresses, overlap and unresolved writes reject it.
          Valid = Reaching.Complete && Reaching.Reachable &&
                  !Reaching.MayBeUninitialized && !Reaching.HasUnknownWrites &&
                  !Reaching.Values.empty() &&
                  llvm::all_of(Reaching.Values, Prove);
        }
      }
    }
    Active.erase(Identity);
    Results.emplace(Identity, Valid);
    return Valid;
  };
  const auto RecordCall =
      [&](const MedOp &Call) -> std::optional<mips::RecordFrameCall> {
    const auto It = Calls.find(&Call);
    if (It == Calls.end() || !Call.SourceCallHint ||
        !Call.SourceCallHint->Signature.MIPSRecords)
      return std::nullopt;
    if (const auto Start = CallPrivateStart(Call))
      return mips::RecordFrameCall{*Start, It->second.FloatingResultBytes};
    return std::nullopt;
  };
  const bool SingleRecordExit =
      QualifiedMemory && Required.size() == 1 && TailReturns.empty() &&
      llvm::any_of(Calls, [](const auto &C) {
        return C.first->SourceCallHint &&
               C.first->SourceCallHint->Signature.MIPSRecords;
      });
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::RETURN) {
        const Site Origin{Op.Addr, Op.OriginSeq};
        if (const auto Tail = TailReturns.find(Origin);
            Tail != TailReturns.end()) {
          if (!QualifiedTails.erase(Tail->second))
            return false;
          TailReturns.erase(Tail);
          continue;
        }
        if (!Required.erase(Origin) ||
            Op.NumInputs != SourceResultOperands + 1 ||
            (!Prove(Op.Inputs[SourceResultOperands]) &&
             (!SingleRecordExit ||
              !mips::preservesRecordFrameLink(Function, Op, Target, RecordCall,
                                              Remaining))))
          return false;
      }
  return Required.empty() && TailReturns.empty() && QualifiedTails.empty();
}

} // namespace neverd
