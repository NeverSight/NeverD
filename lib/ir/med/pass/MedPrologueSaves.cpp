//===- MedPrologueSaves.cpp - Callee-save prologue storage ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedPrologueSaves.h"

#include "../../AffineFrameState.h"
#include "../abi/MedABIPassDetail.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceStackFrame.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/BlockRegisterConstant.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/ir/med/MedCallLeftovers.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

bool forwardsSourceEntryRegister(const MedOp &Call,
                                 const IntegerArgumentLayout &Layout) {
  if (Call.Opcode != NdOp::CALL || Call.SourceCallHint || !Call.NumInputs ||
      !Call.Inputs[0].isConst())
    return false;
  for (size_t P = 0; P < Layout.Registers.size() && P + 1 < Call.NumInputs;
       ++P) {
    const auto &Input = Call.Inputs[P + 1];
    if (Input.Kind == MedVar::Reg && Input.SSAVer == 0 && Input.Size &&
        Input.Size <= Layout.SlotBytes && Input.RegOff == Layout.Registers[P])
      return true;
  }
  return false;
}

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

/// A complete use proof, rather than a type or an observed access list. A
/// borrowed pointer can move through scalar SSA values and direct synchronous
/// calls; storing it, returning it, or losing its offset cancels the proof.
/// Reads are a union over every path, so an unvisited CFG arm cannot supply a
/// noncapture claim. Distinct offsets at a join and recursive borrows remain
/// conservative. No result outlives the current operation/argument sequence.
class FrameBorrowReads {
public:
  using Range = std::pair<int64_t, int64_t>;
  using Ranges = std::vector<Range>;

  FrameBorrowReads(Arch Architecture, BinaryFormat Format,
                   const std::map<va_t, const MedFunc *> &Callees)
      : TRI(getTargetRegInfo(Architecture)), Format(Format), Callees(Callees) {}

  std::optional<Ranges> frame(const MedFunc &Func) {
    if (Func.Blocks.empty() || !Func.Blocks.front().Preds.empty())
      return std::nullopt;
    // An opaque zero-argument call is harmless only before this invocation
    // has formed any address into its new private frame. After that point,
    // even an omitted argument register could still carry such an address.
    const auto &Entry = Func.Blocks.front();
    std::vector<MedVar> Values;
    std::vector<size_t> Positions;
    for (size_t I = 0; I < Entry.Ops.size(); ++I) {
      const MedOp &Op = Entry.Ops[I];
      if (Op.Output.Kind == MedVar::Reg && Op.Output.Size &&
          Op.Output.RegOff != TRI.StackPointer) {
        Values.push_back(Op.Output);
        Positions.push_back(I);
      }
    }
    const auto Offsets =
        entryStackOffsets(Func, Values, Arch::X64, Format, Func.EntryKind);
    size_t FirstAddress = Entry.Ops.size();
    for (size_t I = 0; I < Offsets.size(); ++I)
      if (Offsets[I])
        FirstAddress = std::min(FirstAddress, Positions[I]);
    for (size_t I = 0; I < Entry.Ops.size(); ++I)
      if (Entry.Ops[I].Opcode == NdOp::STORE && !Entry.Ops[I].PrologueSave)
        FirstAddress = std::min(FirstAddress, I);
    for (size_t I = 0; I < FirstAddress; ++I)
      EmptyPrefixCalls.insert(&Entry.Ops[I]);
    const auto StackPointers = sourceCallStackPointers(Func, Arch::X64, Budget);
    Values.clear();
    for (const auto &[Call, Stack] : StackPointers)
      Values.push_back(Stack);
    const auto CallOffsets =
        entryStackOffsets(Func, Values, Arch::X64, Format, Func.EntryKind);
    size_t I = 0;
    for (const auto &[Call, Stack] : StackPointers) {
      const auto Offset = CallOffsets[I++];
      int64_t End;
      if (!Offset ||
          llvm::AddOverflow(
              *Offset, TRI.integerArgumentLayout(Format).CallStackBase, End))
        return std::nullopt;
      // A call owns its home area and any storage this invocation already
      // released below SP. Neither requires an explicit pointer parameter.
      ImplicitFrameReads.emplace_back(-Func.FrameSize, End);
      ReadSites[Call].emplace_back(-Func.FrameSize, End);
      LocatedCalls.insert(Call);
    }
    return through(Func, roots(Func, TRI.StackPointer, std::nullopt), true);
  }

  /// The complete frame-use proof above supplies every possible reader. A
  /// partial overwrite kills only its bytes on that path; neither a callee
  /// write nor a missing operation is assumed to initialize anything.
  bool unreadAfter(
      const MedFunc &Func, const MedOp &Save, int64_t Offset, uint16_t Bytes,
      const std::map<const MedOp *, std::pair<int64_t, uint16_t>> &Stores) {
    if (!Bytes || Bytes > sizeof(uint64_t))
      return false;
    std::map<int, size_t> Blocks;
    std::optional<std::pair<size_t, size_t>> Start;
    for (size_t B = 0; B < Func.Blocks.size(); ++B) {
      const auto &Block = Func.Blocks[B];
      if (Block.Ops.size() >= Budget)
        return false;
      Budget -= Block.Ops.size() + 1;
      if (!Blocks.emplace(Block.Id, B).second)
        return false;
      for (size_t I = 0; I < Block.Ops.size(); ++I)
        if (&Block.Ops[I] == &Save)
          Start = {B, I + 1};
    }
    if (!Start)
      return false;
    auto overlap = [&](int64_t Begin, int64_t End) {
      uint64_t Mask = 0;
      for (unsigned I = 0; I < Bytes; ++I)
        if (Offset + I >= Begin && Offset + I < End)
          Mask |= uint64_t{1} << I;
      return Mask;
    };
    using Visit = std::tuple<size_t, size_t, uint64_t>;
    std::vector<Visit> Work{
        {Start->first, Start->second, (uint64_t{1} << Bytes) - 1}};
    std::map<std::pair<size_t, size_t>, uint64_t> Seen;
    while (!Work.empty()) {
      auto [B, First, Pending] = Work.back();
      Work.pop_back();
      auto &Visited = Seen[{B, First}];
      Pending &= ~Visited;
      if (!Pending)
        continue;
      Visited |= Pending;
      const auto &Block = Func.Blocks[B];
      bool Returned = false;
      for (size_t I = First; Pending && I < Block.Ops.size(); ++I) {
        if (!Budget)
          return false;
        --Budget;
        const auto &Op = Block.Ops[I];
        if (Op.Dead)
          continue;
        if (const auto At = ReadSites.find(&Op); At != ReadSites.end())
          for (const auto &[Begin, End] : At->second) {
            if (!Budget)
              return false;
            --Budget;
            if (Pending & overlap(Begin, End))
              return false;
          }
        if (!Op.PrologueSave)
          if (const auto At = Stores.find(&Op); At != Stores.end()) {
            int64_t End;
            if (llvm::AddOverflow(At->second.first, int64_t(At->second.second),
                                  End))
              return false;
            Pending &= ~overlap(At->second.first, End);
          }
        if (Op.Opcode == NdOp::RETURN || Op.DoesNotReturn) {
          Returned = true;
          break;
        }
      }
      if (!Pending || Returned)
        continue;
      if (Block.Succs.empty())
        return false;
      for (int Successor : Block.Succs) {
        if (!Budget)
          return false;
        --Budget;
        const auto Next = Blocks.find(Successor);
        if (Next == Blocks.end())
          return false;
        Work.emplace_back(Next->second, 0, Pending);
      }
    }
    return true;
  }

private:
  struct Use {
    const MedOp *Op = nullptr;
    const PhiNode *Phi = nullptr;
    const MedCallInfo *Call = nullptr;
    unsigned Input = 0;
  };
  const TargetRegInfo &TRI;
  BinaryFormat Format;
  const std::map<va_t, const MedFunc *> &Callees;
  size_t Budget = limits::ir::kSourceFrameBorrowWork;
  std::set<const MedOp *> EmptyPrefixCalls;
  std::set<const MedOp *> LocatedCalls;
  Ranges ImplicitFrameReads;
  std::map<const MedOp *, Ranges> ReadSites;
  std::map<std::pair<va_t, uint64_t>, std::optional<Ranges>> Known;

  std::vector<MedVar> roots(const MedFunc &Func, uint64_t Register,
                            std::optional<unsigned> Parameter) const {
    std::map<ValueKey, MedVar> Values;
    std::set<ValueKey> Defined;
    for (const MedBlock &Block : Func.Blocks) {
      for (const PhiNode &Phi : Block.Phis)
        Defined.insert(keyOf(Phi.Output));
      for (const MedOp &Op : Block.Ops)
        if (Op.Output.Size && !Op.Output.isConst())
          Defined.insert(keyOf(Op.Output));
    }
    auto Note = [&](const MedVar &V) {
      // SSA version zero can be the first definition, such as the result of
      // a leaf's load into EAX. Only an unbound value or the entry-seed copy
      // denotes an incoming register; returning the load is not a capture of
      // the register's previous contents.
      if (Defined.count(keyOf(V)) &&
          !(V.Kind == MedVar::Reg && isEntryLiveInValue(Func, V, V.RegOff)))
        return;
      if ((Register != kNoParamReg && V.Kind == MedVar::Reg &&
           V.RegOff / TRI.PointerSize == Register / TRI.PointerSize &&
           V.SSAVer == 0) ||
          (Parameter && V.Kind == MedVar::Param && V.Id == *Parameter))
        Values.emplace(keyOf(V), V);
    };
    for (const MedBlock &Block : Func.Blocks) {
      for (const PhiNode &Phi : Block.Phis)
        for (const auto &[Pred, Value] : Phi.Args)
          Note(Value);
      for (const MedOp &Op : Block.Ops)
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          Note(Op.Inputs[I]);
    }
    for (const MedCallInfo &Call : Func.CallInfos)
      for (const MedVar &Arg : Call.Args)
        Note(Arg);
    std::vector<MedVar> Result;
    for (const auto &[Key, Value] : Values)
      Result.push_back(Value);
    return Result;
  }

  std::optional<Ranges> parameter(const MedCallInfo &Call, unsigned Index) {
    const auto Found = Callees.find(Call.TargetAddr);
    if (Call.IsIndirect || Found == Callees.end())
      return std::nullopt;
    const MedFunc &Func = *Found->second;
    if (Func.SourceParametersBound || Func.IsVariadic ||
        Func.CC != CallingConv::Win64 || !Func.FPParamScalarBytes.empty())
      return std::nullopt;
    const auto Layout = TRI.integerArgumentLayout(Format);
    const llvm::ArrayRef<uint64_t> Registers =
        Call.ArgumentsInCalleeRegisterOrder
            ? llvm::ArrayRef<uint64_t>(Func.IntegerArgumentRegisters)
            : Layout.Registers;
    const uint64_t Register =
        Index < Registers.size() ? Registers[Index] : kNoParamReg;
    // Incoming stack storage has its own alias/overwrite semantics. A
    // missing Param alone does not prove that its bytes are never read.
    if (Register == kNoParamReg) {
      return std::nullopt;
    }
    return inputRegister(Func, Register);
  }

  std::optional<Ranges> inputRegister(const MedFunc &Func, uint64_t Register) {
    const auto Key = std::pair{Func.Entry, Register};
    if (const auto It = Known.find(Key); It != Known.end())
      return It->second;
    const int Index = TRI.integerArgumentLayout(Format).registerIndex(Register);
    auto Result = through(
        Func,
        roots(Func, Register,
              Index < 0 ? std::nullopt : std::optional<unsigned>(Index)),
        false);
    Known.emplace(Key, Result);
    return Result;
  }

  std::optional<Ranges>
  through(const MedFunc &Func, const std::vector<MedVar> &Roots, bool IsFrame) {
    if (Func.SkippedSSA || Func.AnalystStackFrame || Func.Blocks.empty() ||
        Func.EntryKind != StackEntryKind::Call ||
        llvm::any_of(Func.ModuleAnalysisRoots,
                     [&](va_t Root) { return Root != Func.Entry; }))
      return std::nullopt;
    std::map<ValueKey, std::vector<Use>> Uses;
    std::vector<const MedOp *> OpaquePrefixCalls;
    std::map<std::pair<int, int>, const MedCallInfo *> Calls;
    for (const MedCallInfo &Call : Func.CallInfos)
      Calls.emplace(std::pair{Call.BlockId, Call.OpIdx}, &Call);
    for (const MedBlock &Block : Func.Blocks) {
      if (!Block.ExceptionalSuccs.empty())
        return std::nullopt;
      for (const PhiNode &Phi : Block.Phis) {
        if (Phi.ExceptionalEntry)
          return std::nullopt;
        for (const auto &[Pred, Value] : Phi.Args)
          Uses[keyOf(Value)].push_back({nullptr, &Phi});
      }
      for (size_t I = 0; I < Block.Ops.size(); ++I) {
        if (!Budget)
          return std::nullopt;
        --Budget;
        const MedOp &Op = Block.Ops[I];
        if (Op.Dead)
          continue;
        const auto Call = Calls.find({Block.Id, static_cast<int>(I)});
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
          // Initially admit scalar-register leaf borrows. Inferred source
          // arity alone cannot prove that a nested call ignores stale machine
          // registers, nor that incoming stack storage is noncapturing.
          if (!IsFrame || !LocatedCalls.count(&Op) || Call == Calls.end() ||
              Op.SourceCallHint || Op.CalleeStackArgs != 0 ||
              Op.CalleeVectorArgs > 0 || Op.CalleeVectorSlots ||
              Op.CalleeRegisterArgs < 0 ||
              Call->second->Args.size() != size_t(Op.CalleeRegisterArgs))
            return std::nullopt;
          const auto Callee = Callees.find(Call->second->TargetAddr);
          if (Call->second->IsIndirect || Callee == Callees.end()) {
            if (!EmptyPrefixCalls.count(&Op) || Op.CalleeRegisterArgs != 0)
              return std::nullopt;
            OpaquePrefixCalls.push_back(&Op);
          } else {
            const auto &Body = *Callee->second;
            const auto Registers = TRI.integerArgumentLayout(Format).Registers;
            if (Body.SourceParametersBound || Body.IsVariadic ||
                Body.CC != CallingConv::Win64 ||
                Call->second->ArgumentsInCalleeRegisterOrder ||
                Call->second->Args.size() > Registers.size())
              return std::nullopt;
            // A zero argument count does not rule out an alias manufactured
            // from the callee's entry SP, or a read above its home area.
            const auto StackReads = inputRegister(Body, TRI.StackPointer);
            const int64_t Incoming =
                TRI.integerArgumentLayout(Format).EntryStackBase;
            if (!StackReads ||
                llvm::any_of(*StackReads, [&](const Range &Read) {
                  return Read.second > Incoming;
                }))
              return std::nullopt;
            // Check every implicit GPR input against the body, including
            // results and nonvolatile families. A declaration or a truncated
            // C argument list cannot hide a pointer observation there.
            for (unsigned Family = 0; Family < kX64VectorFamilyBase; ++Family) {
              const uint64_t Register = Family * TRI.PointerSize;
              const int Index =
                  TRI.integerArgumentLayout(Format).registerIndex(Register);
              if (Register == TRI.StackPointer ||
                  (Index >= 0 && size_t(Index) < Call->second->Args.size()))
                continue;
              const auto Implicit = inputRegister(Body, Register);
              if (!Implicit || !Implicit->empty())
                return std::nullopt;
            }
          }
        }
        if (!IsFrame)
          for (unsigned K = 0; K < Op.NumInputs; ++K) {
            const MedVar &Input = Op.Inputs[K];
            if (Input.Kind == MedVar::Reg && Input.SSAVer == 0 &&
                !TRI.isGeneralReg(Input.RegOff) &&
                !TRI.isFlag(Input.RegOff, Input.Size))
              return std::nullopt;
          }
        for (unsigned K = 0; K < Op.NumInputs; ++K) {
          // CallInfos owns recovered arguments, including stack arguments.
          // The operation's first input is still its potentially indirect
          // target. An unbound operation keeps all of its explicit uses.
          if (K && Call != Calls.end() && K <= Call->second->Args.size() &&
              Op.Inputs[K] == Call->second->Args[K - 1])
            continue;
          Uses[keyOf(Op.Inputs[K])].push_back({&Op, nullptr, nullptr, K});
        }
        if (Call != Calls.end())
          for (unsigned K = 0; K < Call->second->Args.size(); ++K)
            Uses[keyOf(Call->second->Args[K])].push_back(
                {&Op, nullptr, Call->second, K});
      }
    }
    // A conditional physical-register preservation is not a simple SSA copy.
    // Such a use needs the clobber owner's proof before a borrow can cross it.
    for (const MedCallClobber &Clobber : Func.CallClobbers)
      Uses[keyOf(Clobber.PreservedInput)].push_back({});
    // A result from an opaque prefix call could itself supply a frame alias.
    // Only an unobserved result can cross this limited prefix exception.
    for (const MedOp *Call : OpaquePrefixCalls)
      if (Call->Output.Size && !Uses[keyOf(Call->Output)].empty())
        return std::nullopt;

    std::vector<std::pair<MedVar, int64_t>> Work;
    for (const MedVar &Root : Roots)
      Work.emplace_back(Root, 0);
    std::map<ValueKey, int64_t> Seen;
    Ranges Reads = IsFrame ? ImplicitFrameReads : Ranges{};
    while (!Work.empty()) {
      const auto [Value, Offset] = Work.back();
      Work.pop_back();
      if (!Budget || Value.Size != TRI.PointerSize)
        return std::nullopt;
      --Budget;
      const auto [At, New] = Seen.emplace(keyOf(Value), Offset);
      if (!New) {
        if (At->second != Offset)
          return std::nullopt;
        continue;
      }
      for (const Use &By : Uses[keyOf(Value)]) {
        if (!Budget)
          return std::nullopt;
        --Budget;
        if (By.Phi) {
          Work.emplace_back(By.Phi->Output, Offset);
          continue;
        }
        if (!By.Op)
          return std::nullopt;
        const MedOp &Op = *By.Op;
        if (By.Call) {
          const auto Borrowed = parameter(*By.Call, By.Input);
          if (!Borrowed || Borrowed->size() > Budget)
            return std::nullopt;
          Budget -= Borrowed->size();
          for (const auto &[Begin, End] : *Borrowed) {
            int64_t First, Last;
            if (llvm::AddOverflow(Offset, Begin, First) ||
                llvm::AddOverflow(Offset, End, Last))
              return std::nullopt;
            Reads.emplace_back(First, Last);
            if (IsFrame)
              ReadSites[By.Op].emplace_back(First, Last);
          }
          continue;
        }
        if (Op.MemoryOrdering != NdMemoryOrdering::None ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          return std::nullopt;
        if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
            By.Input == 0) {
          if (Op.Opcode == NdOp::LOAD) {
            int64_t End;
            if (!Op.Output.Size ||
                llvm::AddOverflow(Offset, int64_t(Op.Output.Size), End))
              return std::nullopt;
            Reads.emplace_back(Offset, End);
            if (IsFrame)
              ReadSites[By.Op].emplace_back(Offset, End);
          }
          continue;
        }
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
          Work.emplace_back(Op.Output, Offset);
          continue;
        }
        if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
            Op.NumInputs == 2 && Op.Inputs[1 - By.Input].isConst() &&
            (Op.Opcode == NdOp::INT_ADD || By.Input == 0)) {
          const int64_t Delta =
              static_cast<int64_t>(Op.Inputs[1 - By.Input].ConstVal);
          int64_t Next;
          const bool Overflow = Op.Opcode == NdOp::INT_ADD
                                    ? llvm::AddOverflow(Offset, Delta, Next)
                                    : llvm::SubOverflow(Offset, Delta, Next);
          if (Overflow)
            return std::nullopt;
          Work.emplace_back(Op.Output, Next);
          continue;
        }
        // Other integer transforms, including pointer comparisons and lossy
        // slices, do not supply a bounded noncapture proof here.
        return std::nullopt;
      }
    }
    // Queries below use disjoint, ordered intervals. Charging the walk alone
    // would still permit a quadratic store-by-read overlap scan afterwards.
    llvm::sort(Reads);
    Ranges Merged;
    for (const auto &Read : Reads) {
      if (Read.first >= Read.second)
        continue;
      if (!Merged.empty() && Merged.back().second >= Read.first)
        Merged.back().second = std::max(Merged.back().second, Read.second);
      else
        Merged.push_back(Read);
    }
    return Merged;
  }
};

/// Explicit and implicit readers, shared by both source spill proofs. A
/// clobber or PHI is not an ordinary operation that a restore can skip.
struct ValueReads {
  std::map<ValueKey, std::vector<const MedOp *>> Ops;
  std::set<ValueKey> Others;
  std::map<ValueKey, std::vector<ValueKey>> PhiOutputs;
  std::set<ValueKey> ImplicitReads;

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
        for (const auto &Incoming : Phi.Args) {
          Note(Incoming.second, nullptr);
          if (!Incoming.second.isConst() && Incoming.second.Id >= 0)
            PhiOutputs[keyOf(Incoming.second)].push_back(keyOf(Phi.Output));
        }
        if (Phi.ExceptionalEntry) {
          Note(*Phi.ExceptionalEntry, nullptr);
          ImplicitReads.insert(keyOf(*Phi.ExceptionalEntry));
        }
      }
      for (const MedOp &Op : Block.Ops)
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          Note(Op.Inputs[I], &Op);
    }
    for (const MedCallClobber &Clobber : Func.CallClobbers) {
      Note(Clobber.Value, nullptr);
      Note(Clobber.PreservedInput, nullptr);
      ImplicitReads.insert(keyOf(Clobber.Value));
      ImplicitReads.insert(keyOf(Clobber.PreservedInput));
    }
  }

  bool unread(const MedVar &V) const {
    return !Ops.count(keyOf(V)) && !Others.count(keyOf(V));
  }

  /// Callee-save restores may flow through dead copies and joins. Scratch
  /// restores still use unread(): later ABI recovery can observe their
  /// register writes implicitly, so they need the separate overwrite proof.
  bool onlyDeadCopies(const MedVar &V, size_t &Budget) const {
    std::vector<ValueKey> Pending{keyOf(V)};
    std::set<ValueKey> Seen;
    while (!Pending.empty()) {
      if (!Budget)
        return false;
      --Budget;
      const ValueKey Key = Pending.back();
      Pending.pop_back();
      if (!Seen.insert(Key).second)
        continue;
      if (ImplicitReads.count(Key))
        return false;
      if (const auto It = Ops.find(Key); It != Ops.end())
        for (const MedOp *Read : It->second) {
          if (!Budget || Read->Opcode != NdOp::COPY || Read->NumInputs != 1 ||
              Read->Output.Id < 0)
            return false;
          --Budget;
          Pending.push_back(keyOf(Read->Output));
        }
      if (const auto It = PhiOutputs.find(Key); It != PhiOutputs.end()) {
        if (It->second.size() > Budget)
          return false;
        Budget -= It->second.size();
        Pending.insert(Pending.end(), It->second.begin(), It->second.end());
      }
    }
    return true;
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

bool isNeutralAtomic(const MedOp &Op, unsigned PointerSize) {
  return Op.Opcode == NdOp::ATOMIC_ADD && Op.NumInputs == 2 &&
         Op.MemoryOrdering == NdMemoryOrdering::SequentiallyConsistent &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         Op.Inputs[0].Size == PointerSize && Op.Inputs[1].isConst() &&
         Op.Inputs[1].ConstVal == 0 && Op.Inputs[1].Size == Op.Output.Size &&
         Op.Output.Id >= 0 && llvm::isPowerOf2_32(Op.Output.Size) &&
         Op.Output.Size <= PointerSize && Op.IntrinsicOutputs.empty();
}

/// Every plain load/store and requested neutral transaction whose address
/// resolves against the entry stack pointer, all by one proof.
void frameAccessesOf(MedFunc &Func, Arch Architecture, BinaryFormat Format,
                     StackEntryKind Entry, std::vector<FrameAccess> &Loads,
                     std::vector<FrameAccess> &Stores,
                     std::vector<FrameAccess> *NeutralAtomics = nullptr) {
  const unsigned PointerSize = getTargetRegInfo(Architecture).PointerSize;
  std::vector<MedOp *> Accesses;
  for (MedBlock &Block : Func.Blocks)
    for (MedOp &Op : Block.Ops)
      if (((Op.Opcode == NdOp::LOAD ||
            (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)) &&
           Op.NumInputs >= 1 && Op.MemoryOrdering == NdMemoryOrdering::None &&
           Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) ||
          (NeutralAtomics && isNeutralAtomic(Op, PointerSize)))
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
    else if (Op.Opcode == NdOp::STORE)
      Stores.push_back({*Offsets[I], Op.Inputs[1].Size, &Op});
    else
      NeutralAtomics->push_back({*Offsets[I], Op.Output.Size, &Op});
  }
}

void markCalleeSavePrologueSaves(
    MedFunc &Func, Arch Architecture, BinaryFormat Format, StackEntryKind Entry,
    std::vector<FrameAccess> &Loads, std::vector<FrameAccess> &Stores,
    const ValueReads &Reads,
    std::vector<FrameAccess> *NeutralAtomics = nullptr) {
  const auto &TRI = getTargetRegInfo(Architecture);
  const BinaryFormat Convention =
      Func.CC == CallingConv::Win64 ? BinaryFormat::COFF : BinaryFormat::ELF;
  // Win64 also hands the callee its register arguments' home slots, above the
  // return address: MSVC saves nonvolatile registers there.
  const IntegerArgumentLayout Layout =
      TRI.integerArgumentLayout(Convention == BinaryFormat::COFF);

  frameAccessesOf(Func, Architecture, Format, Entry, Loads, Stores,
                  NeutralAtomics);

  size_t ReadBudget = limits::ir::kSourceEntrySpillWork;
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
      if (Reads.onlyDeadCopies(Out, ReadBudget))
        return true;
      const MedOp *Reader = Reads.singleReader(Out);
      if (SameRegister(Out) || !Reader)
        return false;
      const MedOp &Copy = *Reader;
      return Copy.Opcode == NdOp::COPY && Copy.NumInputs == 1 &&
             SameRegister(Copy.Output) &&
             Reads.onlyDeadCopies(Copy.Output, ReadBudget);
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

/// The compiler owns the relocated CALL return slot. Only an unchanged
/// transaction with no value observers can use that different address and
/// contents; flags derived from the old bytes count as ordinary readers too.
void markNeutralReturnSlotTransactions(MedFunc &Func, Arch Architecture,
                                       StackEntryKind Entry,
                                       llvm::ArrayRef<FrameAccess> Atomics,
                                       const ValueReads &Reads) {
  if (Atomics.empty() ||
      (Architecture != Arch::X86 && Architecture != Arch::X64) ||
      Entry != StackEntryKind::Call || Func.SkippedSSA ||
      Func.AnalystStackFrame || Func.Blocks.empty() ||
      Func.Blocks.front().StartAddr != Func.Entry ||
      !Func.Blocks.front().Preds.empty() ||
      !Func.Blocks.front().ExceptionalPreds.empty())
    return;
  // A disconnected or exceptional entry has its own stack context. The
  // ordinary entry-SP proof cannot grant that context the caller's slot.
  if (llvm::any_of(Func.Blocks, [&](const MedBlock &Block) {
        return !Block.ExceptionalPreds.empty() ||
               (Block.Id != Func.Blocks.front().Id && Block.Preds.empty());
      }))
    return;
  const unsigned PointerSize = getTargetRegInfo(Architecture).PointerSize;
  for (const FrameAccess &Access : Atomics)
    if (Access.Offset >= 0 && Access.Offset < PointerSize &&
        Access.Size <= PointerSize - Access.Offset &&
        Reads.unread(Access.Op->Output))
      Access.Op->SourceReturnSlotOffset = uint16_t(Access.Offset);
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
  case NdOp::POPCOUNT:
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
                               const ValueReads &Reads,
                               const BinaryImage *Image,
                               std::set<const MedOp *> *Overwritten) {
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
  // Folding an address calculation may leave one named COPY after the
  // earlier substitution phase. Preserve its exact width and provenance;
  // numeric equality or a truncated address is not an image-object proof.
  std::map<ValueKey, const MedVar *> ConstantAddresses;
  if (Image)
    for (const auto &Block : Func.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            Op.Output.Size == TRI.PointerSize && Op.Inputs[0].isConst() &&
            Op.Inputs[0].Size == TRI.PointerSize)
          ConstantAddresses.emplace(keyOf(Op.Output), &Op.Inputs[0]);
  std::map<int, size_t> Blocks;
  std::set<va_t> BlockEntries;
  std::set<const MedOp *> ImageReads;
  std::map<const MedOp *, std::pair<size_t, size_t>> Positions;
  for (size_t B = 0; B < Func.Blocks.size(); ++B) {
    Blocks.emplace(Func.Blocks[B].Id, B);
    BlockEntries.insert(Func.Blocks[B].StartAddr);
    for (size_t I = 0; I < Func.Blocks[B].Ops.size(); ++I) {
      const MedOp &Op = Func.Blocks[B].Ops[I];
      Positions.emplace(&Op, std::pair{B, I});
      if (!Image || Op.Opcode != NdOp::LOAD || !Op.NumInputs ||
          !Op.Output.Size || Op.Inputs[0].Size != TRI.PointerSize)
        continue;
      const MedVar *Pointer = &Op.Inputs[0];
      if (!Pointer->isConst()) {
        const auto Found = ConstantAddresses.find(keyOf(*Pointer));
        if (Found == ConstantAddresses.end())
          continue;
        Pointer = Found->second;
      }
      if (!isExactAddressProvenance(Pointer->Provenance) ||
          isCodeAddressProvenance(Pointer->Provenance))
        continue;
      const va_t Address = Pointer->ConstVal;
      const va_t Owner = Pointer->AddressOwnerVA == InvalidVA
                             ? Image->layoutAddressOwner(Address)
                             : Pointer->AddressOwnerVA;
      const auto End = Image->mappedObjectOwnerEnd(Address);
      if (End && Address < *End && Op.Output.Size <= *End - Address &&
          Owner != InvalidVA && Image->layoutAddressOwner(Address) == Owner &&
          Image->layoutAddressOwner(Address + Op.Output.Size - 1) == Owner &&
          Image->hasObjectDataProvenance(Address) &&
          Image->hasObjectDataProvenance(Address + Op.Output.Size - 1) &&
          Image->readVA(Address, Op.Output.Size))
        ImageReads.insert(&Op);
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
    // cannot prove a physical scratch register dead before that call. Only
    // the separately checked input-observation proof can cross it.
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
        if (Op.Opcode == NdOp::CALL) {
          const auto Proof = Func.CallUnreadRegisters.find(Op.CallSiteId);
          if (Proof == Func.CallUnreadRegisters.end() ||
              Architecture != Arch::X64 || !Op.CallSiteId || !Op.NumInputs ||
              !Op.Inputs[0].isConst() || Op.SourceCallHint ||
              Op.Addr != Proof->second.Instruction ||
              Op.OriginSeq != Proof->second.OriginSeq ||
              Op.Inputs[0].ConstVal != Proof->second.Target ||
              !callPreservesRegisterBytes(Architecture, Proof->second.Families,
                                          Value.RegOff, Value.Size))
            return false;
          if (Op.DoesNotReturn ||
              callClobbersRegister(Op, Value.RegOff, Value.Size, TRI, Format))
            return true;
        } else if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) {
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
    if (Save.Op->Dead || Save.Op->PrologueSave || Value.isConst() ||
        Value.Size > sizeof(uint64_t) || !Private(Save.Offset, Value.Size))
      continue;
    const auto [FirstBlock, FirstOp] = Positions.at(Save.Op);
    const uint64_t All = (uint64_t{1} << Value.Size) - 1;
    using State = std::tuple<size_t, size_t, uint64_t>;
    std::vector<State> Work{{FirstBlock, FirstOp + 1, All}};
    std::map<std::pair<size_t, size_t>, uint64_t> Seen;
    bool Safe = true, FullyOverwritten = true;
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
        if (Op.Dead) {
          // Source omission does not certify that an original memory access
          // or opaque effect could not retain these bits in physical state.
          FullyOverwritten &= isSpillScalarStep(Op.Opcode);
          continue;
        }
        if (Op.MemoryOrdering != NdMemoryOrdering::None ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
          Safe = false;
          break;
        }
        if (Op.Opcode == NdOp::STORE || Op.Opcode == NdOp::LOAD) {
          // This exact image object is separate from the invocation's source
          // frame. Its read stays observable, including its loaded value;
          // neither an arbitrary numeric pointer nor an external store gets
          // this permission to cross the private-byte overwrite proof.
          if (ImageReads.count(&Op))
            continue;
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
          if (Op.Opcode == NdOp::LOAD && (Pending & Overlap)) {
            // A source-dead restore may still carry the original value out
            // in a physical register. It cannot certify input destruction.
            FullyOverwritten = false;
            if (!UnusedRestore(Op)) {
              Safe = false;
              break;
            }
          }
          if (Op.Opcode == NdOp::STORE && !Op.PrologueSave)
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
          FullyOverwritten = false;
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
    if (Safe) {
      Save.Op->PrologueSave = true;
      if (Overwritten && FullyOverwritten)
        Overwritten->insert(Save.Op);
    }
    if (!Budget)
      return;
  }
}

/// An absorbed read-modify-write may leave no MedIR LOAD. Original instruction
/// temporaries may only flow through copies and a shared scalar annihilator;
/// flags, register writes and memory uses of the old value remain observations.
bool hasUnobservedInstructionLoad(llvm::ArrayRef<LowOp> Ops, size_t Index,
                                  const std::set<const LowOp *> &Absorbed,
                                  size_t &Budget) {
  const auto &Load = Ops[Index];
  if (!Load.Output.isTemp() || !Load.Output.Size ||
      Load.Output.Offset > UINT64_MAX - Load.Output.Size)
    return false;
  std::map<uint64_t, uint16_t> Tainted{{Load.Output.Offset, Load.Output.Size}};
  for (size_t I = Index + 1; I < Ops.size() && Ops[I].Addr == Load.Addr; ++I) {
    const auto &Op = Ops[I];
    const size_t Cost = 1 + Tainted.size() * (Op.NumInputs + 1);
    if (Cost > Budget)
      return false;
    Budget -= Cost;
    if (Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        (Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE &&
         !isSpillScalarStep(Op.Opcode)))
      return false;
    bool Read = false;
    for (unsigned K = 0; K < Op.NumInputs; ++K) {
      const auto &Input = Op.Inputs[K];
      if (Input.isTemp() && Input.Size)
        for (const auto &[At, Bytes] : Tainted)
          Read |= Input.Offset >= At ? Input.Offset - At < Bytes
                                     : At - Input.Offset < Input.Size;
    }
    if (Read &&
        (lowAbsorbingConstant(Op) || isLowSelfXor(Op) || Absorbed.count(&Op)))
      Read = false;
    if (Read && (Op.Opcode != NdOp::COPY || Op.NumInputs != 1 ||
                 !Op.Output.isTemp() || Op.Output.Size != Op.Inputs[0].Size))
      return false;
    if (Op.Output.isTemp() && Op.Output.Size) {
      const uint64_t Begin = Op.Output.Offset;
      if (Begin > UINT64_MAX - Op.Output.Size)
        return false;
      const uint64_t End = Begin + Op.Output.Size;
      // A partial overwrite removes just those bytes. Reads of a subrange
      // may start inside an older temporary instead of at its original key.
      for (auto At = Tainted.begin(); At != Tainted.end();) {
        const auto [First, Bytes] = *At;
        const uint64_t Last = First + Bytes;
        if (First >= End || Last <= Begin) {
          ++At;
          continue;
        }
        At = Tainted.erase(At);
        if (First < Begin)
          Tainted.emplace(First, static_cast<uint16_t>(Begin - First));
        if (Last > End)
          Tainted.emplace(End, static_cast<uint16_t>(Last - End));
      }
      if (Read)
        Tainted[Begin] = Op.Output.Size;
    }
    if (Tainted.empty())
      return true;
  }
  // Remaining values are instruction-local temporaries, never physical state.
  return true;
}

/// A standalone, unaligned x64 GS handler reads its cookie above the current
/// establisher SP; it does not read the interrupted volatile register bank.
/// With no frame register, a suspended ordinary call supplies that SP. Keep
/// the cookie beyond the callee's home area and private frame, so a descendant
/// may discard its own temporary input spill without hiding a parent observer.
/// This admits only register proofs that never discard a spill in this frame.
bool hasSeparatedGSInputObserver(const MedFunc &Func, size_t &Budget) {
  if (!Func.ExceptionMetadata)
    return false;
  const auto &EH = *Func.ExceptionMetadata;
  if (EH.ParseStatus != ExceptionParseStatus::Complete ||
      EH.Personality != ExceptionPersonality::GSHandlerCheck ||
      !EH.PersonalityVA || !EH.HandlerDataVA || !EH.GSCookie ||
      EH.Kind != RuntimeFunctionKind::Primary ||
      EH.CodeRange.Begin != Func.Entry || !EH.CodeRange.isValid() ||
      EH.PrimaryFunctionIndex || EH.ChainedPrimaryRange ||
      EH.ChainedUnwindInfoRVA || EH.FrameRegister || EH.FrameOffset ||
      EH.UnwindFlags != 3 ||
      !((EH.Encoding == ExceptionEncoding::X64UnwindV1 &&
         EH.UnwindVersion == 1) ||
        (EH.Encoding == ExceptionEncoding::X64UnwindV2 &&
         EH.UnwindVersion == 2)) ||
      EH.SEH || EH.Cxx || EH.Dwarf || EH.Itanium || EH.ARMEHABI || EH.Compact ||
      EH.Registration || EH.Delphi || EH.DelphiScopes || EH.Go || EH.Rust ||
      EH.ObjC)
    return false;
  if (EH.DecodeProvenance && (EH.DecodeProvenance->Structural.ParseStatus !=
                                  ExceptionParseStatus::Complete ||
                              EH.DecodeProvenance->Language.ParseStatus !=
                                  ExceptionParseStatus::Complete))
    return false;
  if (EH.UnwindOperations.size() > Budget) {
    Budget = 0;
    return false;
  }
  Budget -= EH.UnwindOperations.size();
  for (const auto &Op : EH.UnwindOperations)
    switch (Op.Kind) {
    case UnwindOperationKind::PushNonVolatile:
    case UnwindOperationKind::AllocateLarge:
    case UnwindOperationKind::AllocateSmall:
    case UnwindOperationKind::SaveNonVolatile:
    case UnwindOperationKind::SaveNonVolatileFar:
    case UnwindOperationKind::SaveXMM128:
    case UnwindOperationKind::SaveXMM128Far:
    case UnwindOperationKind::Epilog:
      break;
    default:
      return false;
    }
  const auto &Cookie = *EH.GSCookie;
  const auto Layout =
      getTargetRegInfo(Arch::X64).integerArgumentLayout(BinaryFormat::COFF);
  return Cookie.ParseStatus == ExceptionParseStatus::Complete &&
         !Cookie.HasExceptionHandler && !Cookie.HasUnwindHandler &&
         !Cookie.HasAlignment && !Cookie.AlignmentBaseOffset &&
         !Cookie.Alignment && Cookie.CookieOffset >= Layout.CallStackBase &&
         uint32_t(Cookie.CookieOffset) % Layout.SlotBytes == 0 &&
         Cookie.Payload.size() == sizeof(uint32_t) &&
         readLE<uint32_t>(Cookie.Payload.data()) ==
             uint32_t(Cookie.CookieOffset);
}

bool hasSourceRegisterInputBody(const MedFunc &Func,
                                bool HasSeparatedGSObserver = false) {
  return !(
      Func.CC != CallingConv::Win64 || Func.SourceTypeHint ||
      Func.SourceParametersBound || Func.IsVariadic || Func.SkippedSSA ||
      Func.AnalystStackFrame || Func.Blocks.empty() ||
      !Func.Blocks.front().Preds.empty() ||
      Func.EntryKind != StackEntryKind::Call ||
      !Func.IntegerArgumentRegisters.empty() ||
      (Func.ExceptionMetadata &&
       (Func.ExceptionMetadata->Personality != ExceptionPersonality::None ||
        Func.ExceptionMetadata->ParseStatus !=
            ExceptionParseStatus::Complete) &&
       !HasSeparatedGSObserver) ||
      llvm::any_of(Func.ModuleAnalysisRoots,
                   [&](va_t Root) { return Root != Func.Entry; }));
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

/// Whether, on every path from \p Store, which \p Positions places, a call
/// receives an address within the bytes the store writes before anything
/// may read them: a local a routine fills through its address.  \p Loads
/// holds the function's loads at proven entry offsets, \p Offsets the proven
/// entry offsets of call operands and returned values, and \p Calls each
/// call's record.  A load whose address is neither proven nor a constant, an
/// earlier call that receives no such address, an intrinsic, a return that
/// may pass a frame address on, or a path the walk cannot follow within
/// \p Budget proves nothing.
bool slotAddressReachesCallFirst(
    const MedFunc &Func, Arch Architecture, const FrameAccess &Store,
    const std::map<const MedOp *, std::pair<size_t, size_t>> &Positions,
    const std::map<const MedOp *, const FrameAccess *> &Loads,
    const std::map<ValueKey, int64_t> &Offsets,
    const std::map<std::pair<int, int>, const MedCallInfo *> &Calls,
    size_t &Budget) {
  const auto SlotAddress = [&](const MedVar &V) {
    const auto It = Offsets.find(keyOf(V));
    return It != Offsets.end() && It->second >= Store.Offset &&
           It->second < Store.Offset + Store.Size;
  };
  std::map<int, size_t> BlockIndex;
  for (size_t B = 0; B < Func.Blocks.size(); ++B)
    BlockIndex.emplace(Func.Blocks[B].Id, B);
  const auto [StoreBlock, StoreIndex] = Positions.at(Store.Op);
  std::vector<std::pair<size_t, size_t>> Work{{StoreBlock, StoreIndex + 1}};
  std::set<std::pair<size_t, size_t>> Seen;
  while (!Work.empty()) {
    const auto [B, Begin] = Work.back();
    Work.pop_back();
    if (!Seen.insert({B, Begin}).second)
      continue;
    const MedBlock &Block = Func.Blocks[B];
    if (!Block.ExceptionalSuccs.empty())
      return false;
    bool Ended = false;
    for (size_t I = Begin; I < Block.Ops.size() && !Ended; ++I) {
      if (!Budget)
        return false;
      --Budget;
      const MedOp &Op = Block.Ops[I];
      if (Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return false;
      switch (Op.Opcode) {
      case NdOp::LOAD: {
        // An image address is no frame byte.
        const auto Load = Loads.find(&Op);
        if (Load == Loads.end() ? !Op.NumInputs || !Op.Inputs[0].isConst()
                                : Load->second->overlaps(
                                      Store.Offset, Store.Offset + Store.Size))
          return false;
        break;
      }
      case NdOp::STORE:
      case NdOp::BRANCH:
      case NdOp::COND_BR:
        break;
      case NdOp::CALL:
      case NdOp::INDIR_CALL: {
        bool Receives = false;
        for (unsigned K = 0; K < Op.NumInputs; ++K)
          Receives |= SlotAddress(Op.Inputs[K]);
        if (const auto Call = Calls.find({Block.Id, static_cast<int>(I)});
            Call != Calls.end())
          Receives |= llvm::any_of(Call->second->Args, SlotAddress);
        if (!Receives)
          return false;
        Ended = true;
        break;
      }
      case NdOp::RETURN:
        for (unsigned K = 0; K < Op.NumInputs; ++K)
          if (Offsets.count(keyOf(Op.Inputs[K])))
            return false;
        Ended = true;
        break;
      case NdOp::INTRINSIC:
        if (!isExplicitFlagsTransfer(Op, Architecture))
          return false;
        break;
      default:
        if (!isSpillScalarStep(Op.Opcode))
          return false;
        break;
      }
    }
    if (Ended)
      continue;
    if (Block.Succs.empty())
      return false;
    for (const int Successor : Block.Succs) {
      const auto Next = BlockIndex.find(Successor);
      if (Next == BlockIndex.end())
        return false;
      Work.emplace_back(Next->second, 0);
    }
  }
  return true;
}

} // namespace

bool isSourcePrivateFrameAccess(const MedFunc &Func, Arch Architecture,
                                BinaryFormat Format, int64_t Offset,
                                uint16_t Bytes) {
  return isPrivateFrameAccess(
      Func, getTargetRegInfo(Architecture).integerArgumentLayout(Format),
      Offset, Bytes);
}

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
                             BinaryFormat Format, StackEntryKind Entry,
                             const BinaryImage *Image,
                             std::set<const MedOp *> *Overwritten) {
  if (Overwritten)
    Overwritten->clear();
  // These source-only annotations are derived from the current body. An
  // edited store or load must not retain a proof from an earlier preparation.
  for (MedBlock &Block : Func.Blocks)
    for (MedOp &Op : Block.Ops) {
      Op.PrologueSave = false;
      Op.ReturnAddressRead = false;
      Op.SourceReturnSlotOffset.reset();
    }
  std::vector<FrameAccess> Loads, Stores, NeutralAtomics;
  const ValueReads Reads(Func);
  markCalleeSavePrologueSaves(Func, Architecture, Format, Entry, Loads, Stores,
                              Reads, &NeutralAtomics);
  // Annotate before source-only replacement. No access proof survives the
  // replacements at the end of this preparation.
  if (hasStackReturnAddress(Architecture, Entry))
    markReturnAddressReads(getTargetRegInfo(Architecture).PointerSize, Loads,
                           Stores);
  markNeutralReturnSlotTransactions(Func, Architecture, Entry, NeutralAtomics,
                                    Reads);
  markUnobservedStackSpills(Func, Architecture, Format, Loads, Stores, Reads,
                            Image, Overwritten);
  replaceUnwrittenCallFrameReads(Func, Architecture, Loads, Stores);
}

std::set<unsigned> sourceRegisterInputOverwrites(
    const MedFunc &Func, llvm::ArrayRef<LowBlock> OriginalBlocks,
    const SourceRegisterParameterReads &Reads,
    const SourceCalleeParameterOverwrites &Callees,
    const std::set<const MedOp *> &Overwritten, size_t &Budget) {
  std::set<unsigned> Result;
  const bool GSObserver = hasSeparatedGSInputObserver(Func, Budget);
  if (!hasSourceRegisterInputBody(Func, GSObserver) || OriginalBlocks.empty() ||
      OriginalBlocks.front().StartAddr != Func.Entry ||
      !OriginalBlocks.front().Preds.empty() ||
      OriginalBlocks.front().Ops.empty() ||
      OriginalBlocks.front().Ops.front().Addr != Func.Entry ||
      (!GSObserver &&
       llvm::none_of(Reads, [](const auto &Read) { return Read.second == 0; })))
    return Result;
  std::map<int, size_t> ById;
  std::map<va_t, size_t> ByAddress;
  for (size_t I = 0; I < OriginalBlocks.size(); ++I) {
    if (!Budget)
      return Result;
    --Budget;
    const LowBlock &Block = OriginalBlocks[I];
    if (GSObserver && !Func.ExceptionMetadata->ownsCode(Block.StartAddr))
      return Result;
    if (!ById.emplace(Block.Id, I).second ||
        !ByAddress.emplace(Block.StartAddr, I).second)
      return Result;
  }
  using Occurrence = std::pair<va_t, int>;
  std::map<Occurrence, const MedOp *> Current;
  for (const auto &Block : Func.Blocks)
    for (const auto &Op : Block.Ops) {
      if (!Budget)
        return Result;
      --Budget;
      if (Op.OriginSeq < 0 ||
          (Op.Opcode != NdOp::STORE && Op.Opcode != NdOp::LOAD &&
           Op.Opcode != NdOp::CALL))
        continue;
      const auto [At, New] =
          Current.emplace(Occurrence{Op.Addr, Op.OriginSeq}, &Op);
      if (!New)
        At->second = nullptr;
    }
  const auto &TRI = getTargetRegInfo(Arch::X64);
  const auto Layout = TRI.integerArgumentLayout(BinaryFormat::COFF);
  std::set<const LowOp *> ParameterLoads;
  // Promoting a stack argument can remove the original LOAD. Its exact entry
  // coordinate is outside every private spill eligible for Overwritten, so
  // it cannot resurrect an ignored register save. Only the original prefix
  // before a call or control transfer supplies this independent address fact.
  if (Func.Params.size() > Layout.Registers.size()) {
    std::map<Occurrence, const LowOp *> EntryLoads;
    for (const auto &Op : OriginalBlocks.front().Ops) {
      if (!Budget)
        return Result;
      --Budget;
      if (Op.Opcode == NdOp::LOAD) {
        const auto [At, New] =
            EntryLoads.emplace(Occurrence{Op.Addr, Op.Seq}, &Op);
        if (!New)
          At->second = nullptr;
      }
    }
    for (const auto &Access : sourceEntryStackAccesses(
             OriginalBlocks.front().Ops, Arch::X64, Budget)) {
      if (Access.Write || !Access.Bytes ||
          Access.Offset < Layout.EntryStackBase)
        continue;
      const uint64_t Distance = Access.Offset - Layout.EntryStackBase;
      const uint64_t P = Layout.Registers.size() + Distance / Layout.SlotBytes;
      const uint64_t Byte = Distance % Layout.SlotBytes;
      if (P < Func.Params.size() && Func.Params[P].Kind == MedVar::Param &&
          Func.Params[P].Id == int(P) && Func.Params[P].RegOff == kNoParamReg &&
          Func.Params[P].Size <= Layout.SlotBytes &&
          Byte < Func.Params[P].Size &&
          Access.Bytes <= Func.Params[P].Size - Byte)
        if (const auto At =
                EntryLoads.find({Access.Instruction, Access.Sequence});
            At != EntryLoads.end() && At->second)
          ParameterLoads.insert(At->second);
    }
  }
  std::optional<std::set<const LowOp *>> Absorbed;
  auto Candidates = Reads;
  if (GSObserver)
    for (unsigned P = 0; P < Func.Params.size() && P < Layout.Registers.size();
         ++P)
      if (Func.Params[P].RegOff == Layout.Registers[P] && Func.Params[P].Size &&
          Func.Params[P].Size <= TRI.PointerSize)
        Candidates[P] = 0;
  for (const auto &[P, Bytes] : Candidates) {
    if (Bytes || P >= Layout.Registers.size())
      continue;
    const uint64_t Register = Layout.Registers[P];
    const auto Overlaps = [&](const NdVar &V) {
      return V.isReg() && V.Size &&
             (V.Offset >= Register ? V.Offset - Register < TRI.PointerSize
                                   : Register - V.Offset < V.Size);
    };
    // true closes this path, false continues over its exact outgoing edges,
    // and no value refuses a read, lost binding, or unsupported operation.
    const auto DestroysInBlock =
        [&](const LowBlock &Block) -> std::optional<bool> {
      if (Block.Ops.empty() || !Block.ExceptionalPreds.empty() ||
          !Block.ExceptionalSuccs.empty())
        return std::nullopt;
      for (size_t I = 0; I < Block.Ops.size(); ++I) {
        const LowOp &Op = Block.Ops[I];
        if (!Budget)
          return std::nullopt;
        --Budget;
        if (GSObserver && !Func.ExceptionMetadata->ownsCode(Op.Addr))
          return std::nullopt;
        const auto At = Current.find({Op.Addr, Op.Seq});
        const MedOp *Bound = At == Current.end() ? nullptr : At->second;
        if (Op.MemoryOrdering != NdMemoryOrdering::None ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          return std::nullopt;
        if (Op.Opcode == NdOp::CALL) {
          if (!Op.NumInputs || !Op.Inputs[0].isConst() || !Bound ||
              Bound->Opcode != NdOp::CALL || Bound->SourceCallHint ||
              !Bound->NumInputs || !Bound->Inputs[0].isConst() ||
              Bound->Inputs[0].ConstVal != Op.Inputs[0].Offset)
            return std::nullopt;
          if (const auto Known = Callees.find(Op.Inputs[0].Offset);
              Known != Callees.end() && Known->second.count(P))
            return true;
          const auto Unread = Func.CallUnreadRegisters.find(Bound->CallSiteId);
          if (!Bound->CallSiteId || Unread == Func.CallUnreadRegisters.end() ||
              Unread->second.Instruction != Op.Addr ||
              Unread->second.OriginSeq != Op.Seq ||
              Unread->second.Target != Op.Inputs[0].Offset ||
              !callPreservesRegisterBytes(Arch::X64, Unread->second.Families,
                                          Register, TRI.PointerSize))
            return std::nullopt;
          continue;
        }
        if (Op.Opcode == NdOp::LOAD &&
            (!Bound || Bound->Opcode != NdOp::LOAD) &&
            !ParameterLoads.count(&Op)) {
          if (!Absorbed)
            Absorbed = lowBlockAbsorbingConstants(OriginalBlocks, Budget);
          if (!hasUnobservedInstructionLoad(Block.Ops, I, *Absorbed, Budget))
            return std::nullopt;
        }
        const bool Branch =
            Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR;
        if (Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE && !Branch &&
            !isSpillScalarStep(Op.Opcode))
          return std::nullopt;
        bool Read = false;
        for (unsigned K = 0; K < Op.NumInputs; ++K)
          Read |= Overlaps(Op.Inputs[K]);
        if (lowAbsorbingConstant(Op) || isLowSelfXor(Op))
          Read = false;
        if (Read) {
          // A discarded restore is insufficient: its physical destination may
          // outlive its source statement. Only complete byte overwrites
          // qualify.
          if (GSObserver || Op.Opcode != NdOp::STORE || Op.NumInputs != 2 ||
              Overlaps(Op.Inputs[0]) || !Op.Inputs[1].isReg() ||
              Op.Inputs[1].Offset != Register || !Bound ||
              Bound->Opcode != NdOp::STORE || Bound->NumInputs != 2 ||
              Bound->Inputs[1].Kind != MedVar::Reg ||
              Bound->Inputs[1].RegOff != Register ||
              Bound->Inputs[1].Size != Op.Inputs[1].Size ||
              !Overwritten.count(Bound))
            return std::nullopt;
          continue;
        }
        if (Branch) {
          if (I + 1 != Block.Ops.size() ||
              Op.NumInputs != (Op.Opcode == NdOp::COND_BR ? 2 : 1) ||
              !Op.Inputs[0].isConst())
            return std::nullopt;
          return false;
        }
        // A complete independent definition ends this input. Earlier reads
        // could not retain it in temporaries, other registers or live storage.
        // The x64 32-bit destination also clears its high half.
        if (Op.Output.isReg() && Op.Output.Offset == Register &&
            (Op.Output.Size == sizeof(uint32_t) ||
             Op.Output.Size == TRI.PointerSize))
          return true;
      }
      return false;
    };
    // Prove all successors, with an explicit postorder stack. A visited node
    // is reusable only after its whole suffix was proved; an active node is a
    // cycle that could retain the input indefinitely.
    std::vector<uint8_t> State(OriginalBlocks.size());
    std::vector<std::pair<size_t, bool>> Work{{0, false}};
    bool Safe = true;
    while (Safe && !Work.empty()) {
      const auto [Index, Finished] = Work.back();
      Work.pop_back();
      if (Finished) {
        State[Index] = 2;
        continue;
      }
      if (State[Index] == 2)
        continue;
      if (State[Index] == 1) {
        Safe = false;
        break;
      }
      State[Index] = 1;
      const LowBlock &Block = OriginalBlocks[Index];
      const auto Destroyed = DestroysInBlock(Block);
      if (!Destroyed || (Index && Block.Preds.empty())) {
        Safe = false;
        break;
      }
      if (*Destroyed) {
        State[Index] = 2;
        continue;
      }
      if (Block.Succs.size() > Budget)
        return Result;
      Budget -= Block.Succs.size();
      std::set<size_t> Expected;
      const auto Include = [&](va_t Address) {
        const auto At = ByAddress.find(Address);
        if (At == ByAddress.end())
          return false;
        Expected.insert(At->second);
        return true;
      };
      const LowOp &Last = Block.Ops.back();
      if (Last.Opcode == NdOp::BRANCH || Last.Opcode == NdOp::COND_BR)
        Safe = Include(Last.Inputs[0].Offset);
      if (Last.Opcode != NdOp::BRANCH)
        Safe &= Include(Block.EndAddr);
      std::set<size_t> Actual;
      for (int Id : Block.Succs) {
        const auto At = ById.find(Id);
        if (At == ById.end() ||
            !llvm::is_contained(OriginalBlocks[At->second].Preds, Block.Id) ||
            !Actual.insert(At->second).second) {
          Safe = false;
          break;
        }
      }
      if (!Safe || Expected.empty() || Actual != Expected) {
        Safe = false;
        break;
      }
      Work.emplace_back(Index, true);
      for (size_t Next : Actual)
        Work.emplace_back(Next, false);
    }
    if (Safe)
      Result.insert(P);
  }
  return Result;
}

std::set<unsigned> unobservedSourceStackParameters(
    const MedFunc &Func, llvm::ArrayRef<LowBlock> OriginalBlocks,
    const std::set<va_t> &DeclaredCallees, size_t &Budget) {
  if (!hasSourceRegisterInputBody(Func) || OriginalBlocks.empty() ||
      OriginalBlocks.front().StartAddr != Func.Entry ||
      !OriginalBlocks.front().Preds.empty() ||
      !Func.FPParamScalarBytes.empty() || Func.DoesNotReturn ||
      Func.Params.size() > limits::ir::kMaxSourceABIParameters)
    return {};
  const auto &TRI = getTargetRegInfo(Arch::X64);
  const auto Layout = TRI.integerArgumentLayout(BinaryFormat::COFF);
  if (Func.Params.size() <= Layout.Registers.size())
    return {};
  using Occurrence = std::pair<va_t, int>;
  std::map<Occurrence, const MedOp *> Calls;
  for (const auto &Block : Func.Blocks) {
    if (!Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty() ||
        (&Block != &Func.Blocks.front() && Block.Preds.empty()))
      return {};
    for (const auto &Phi : Block.Phis) {
      if (!Budget || Phi.ExceptionalEntry)
        return {};
      --Budget;
    }
    for (const auto &Op : Block.Ops) {
      if (!Budget)
        return {};
      --Budget;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        if (Op.OriginSeq < 0 ||
            !Calls.emplace(Occurrence{Op.Addr, Op.OriginSeq}, &Op).second)
          return {};
      }
    }
  }
  // Bind every original edge to its instruction target or fallthrough. A
  // missing arm, independent entry or overlapping block cannot prove absence
  // of a stack read. Block order is not control flow.
  std::map<int, size_t> ById;
  std::map<va_t, size_t> ByAddress;
  std::set<std::pair<int, int>> Incoming, Outgoing;
  for (size_t I = 0; I < OriginalBlocks.size(); ++I) {
    const auto &Block = OriginalBlocks[I];
    const size_t Cost = 1 + Block.Preds.size() + Block.Succs.size();
    if (Cost > Budget)
      return {};
    Budget -= Cost;
    if (Block.Ops.empty() || Block.Ops.front().Addr != Block.StartAddr ||
        Block.EndAddr <= Block.StartAddr || !Block.ExceptionalPreds.empty() ||
        !Block.ExceptionalSuccs.empty() || (I && Block.Preds.empty()) ||
        !ById.emplace(Block.Id, I).second ||
        !ByAddress.emplace(Block.StartAddr, I).second)
      return {};
    for (int Pred : Block.Preds)
      if (!Incoming.emplace(Pred, Block.Id).second)
        return {};
    for (int Succ : Block.Succs)
      if (!Outgoing.emplace(Block.Id, Succ).second)
        return {};
  }
  if (Incoming != Outgoing)
    return {};
  va_t PreviousEnd = 0;
  std::vector<std::vector<size_t>> Successors(OriginalBlocks.size());
  for (const auto &[Address, Index] : ByAddress) {
    const auto &Block = OriginalBlocks[Index];
    if (Address < PreviousEnd)
      return {};
    PreviousEnd = Block.EndAddr;
    std::set<size_t> Expected, Actual;
    const auto Include = [&](va_t Target) {
      const auto At = ByAddress.find(Target);
      if (At == ByAddress.end())
        return false;
      Expected.insert(At->second);
      return true;
    };
    const auto &Last = Block.Ops.back();
    if (Last.Opcode == NdOp::BRANCH || Last.Opcode == NdOp::COND_BR) {
      if (Last.NumInputs != (Last.Opcode == NdOp::COND_BR ? 2 : 1) ||
          !Last.Inputs[0].isConst() || !Include(Last.Inputs[0].Offset))
        return {};
    }
    if (Last.Opcode != NdOp::BRANCH && Last.Opcode != NdOp::RETURN &&
        !Include(Block.EndAddr))
      return {};
    for (int Id : Block.Succs) {
      const auto At = ById.find(Id);
      if (At == ById.end())
        return {};
      Actual.insert(At->second);
    }
    if (Actual != Expected)
      return {};
    Successors[Index].assign(Actual.begin(), Actual.end());
  }
  // A wrapper may retain instruction-local address temporaries, but no frame
  // address may enter another physical register or stored value. Keep taint
  // after lossy arithmetic as well as exact affine coordinates.
  struct FrameValue {
    std::optional<int64_t> Offset;
    uint16_t Bytes = 0;
    bool Derived = false;
  };
  struct FrameState {
    int64_t Stack = 0;
    std::set<uint64_t> Flags;
  };
  std::vector<std::optional<FrameState>> Entries(OriginalBlocks.size());
  Entries[0] = FrameState{};
  std::vector<size_t> Work{0};
  std::vector<std::pair<int64_t, int64_t>> Reads;
  std::map<Occurrence, const LowOp *> MatchedCalls;
  bool Bounded = true;
  while (!Work.empty()) {
    const size_t Index = Work.back();
    Work.pop_back();
    const auto &Block = OriginalBlocks[Index];
    if (1 + Entries[Index]->Flags.size() > Budget)
      return {};
    Budget -= 1 + Entries[Index]->Flags.size();
    int64_t Stack = Entries[Index]->Stack;
    auto FrameFlags = Entries[Index]->Flags;
    std::map<uint64_t, FrameValue> Temps;
    va_t Instruction = InvalidVA;
    // Keep nonoverlapping temporary intervals. A subrange read still carries
    // frame taint; a partial write cannot retain the old exact address.
    const auto FirstOverlap = [&](const NdVar &V) {
      auto At = Temps.lower_bound(V.Offset);
      if (At != Temps.begin()) {
        auto Before = At;
        --Before;
        if (V.Offset - Before->first < Before->second.Bytes)
          At = Before;
      }
      return At;
    };
    const auto Read = [&](const NdVar &V) -> FrameValue {
      if (V.isReg() && V.Offset == TRI.StackPointer)
        return {V.Size == TRI.PointerSize ? std::optional(Stack) : std::nullopt,
                V.Size, true};
      if (V.isReg() && FrameFlags.count(V.Offset))
        return {std::nullopt, V.Size, true};
      FrameValue Value;
      if (V.isTemp())
        for (auto At = FirstOverlap(V);
             At != Temps.end() &&
             (At->first < V.Offset || At->first - V.Offset < V.Size);
             ++At) {
          if (!Budget) {
            Bounded = false;
            return {};
          }
          --Budget;
          Value.Derived |= At->second.Derived;
          if (At->first == V.Offset && At->second.Bytes == V.Size)
            Value.Offset = At->second.Offset;
        }
      return Value;
    };
    for (const auto &Op : Block.Ops) {
      if (!Budget || Op.Addr < Block.StartAddr || Op.Addr >= Block.EndAddr)
        return {};
      --Budget;
      if (Op.Addr != Instruction) {
        Temps.clear();
        Instruction = Op.Addr;
      }
      if (Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return {};
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        const auto At = Calls.find({Op.Addr, Op.Seq});
        if (Op.NumInputs != 1 || !Op.Inputs[0].isConst() || At == Calls.end())
          return {};
        const auto &Call = *At->second;
        if (Call.Opcode != Op.Opcode || !Call.NumInputs ||
            !Call.Inputs[0].isConst() ||
            Call.Inputs[0].ConstVal != Op.Inputs[0].Offset ||
            !DeclaredCallees.count(Op.Inputs[0].Offset) ||
            Call.SourceCallHint || Call.IsTailCall || Call.DoesNotReturn ||
            Call.CalleeStackArgs < 0 || Call.CalleeVectorArgs != 0 ||
            Call.CalleeVectorSlots || Stack > 0)
          return {};
        const int64_t Arguments = std::max<int64_t>(
            int64_t(Call.CalleeStackArgs) - int64_t(Layout.Registers.size()),
            0);
        const auto End = detail::checkedAffineOffset(
            Stack, Layout.CallStackBase + Arguments * Layout.SlotBytes);
        if (!End)
          return {};
        // Declared callees own their outgoing argument/home area and storage
        // below call SP. A shallow frame can expose incoming slots to that
        // area.
        Reads.emplace_back(std::min<int64_t>(Stack, 0), *End);
        const auto [Matched, New] =
            MatchedCalls.emplace(Occurrence{Op.Addr, Op.Seq}, &Op);
        if (!New && Matched->second != &Op)
          return {};
        // A call may overwrite flags, but does not prove destruction of an old
        // frame-derived value. Retain that taint until an original definition.
        continue;
      }
      if (Op.Opcode == NdOp::RETURN) {
        if (&Op != &Block.Ops.back() || Stack || Op.NumInputs != 1 ||
            Op.Inputs[0] != NdVar::reg(TRI.IntReturnReg, TRI.PointerSize))
          return {};
        continue;
      }
      FrameValue Value;
      for (unsigned K = 0; K < Op.NumInputs; ++K)
        Value.Derived |= Read(Op.Inputs[K]).Derived;
      if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) {
        if (&Op != &Block.Ops.back() || Value.Derived)
          return {};
        continue;
      }
      if (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) {
        if (Op.NumInputs != (Op.Opcode == NdOp::LOAD ? 1 : 2))
          return {};
        const auto Address = Read(Op.Inputs[0]).Offset;
        const auto Bytes =
            Op.Opcode == NdOp::LOAD ? Op.Output.Size : Op.Inputs[1].Size;
        const auto End = Address ? detail::checkedAffineOffset(*Address, Bytes)
                                 : std::nullopt;
        if (!Address || !Bytes || !End ||
            (Op.Opcode == NdOp::STORE && Read(Op.Inputs[1]).Derived))
          return {};
        if (Op.Opcode == NdOp::STORE)
          continue;
        Reads.emplace_back(*Address, *End);
        Value.Derived = false;
      } else if (!isSpillScalarStep(Op.Opcode)) {
        return {};
      } else if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                 Op.Output.Size == Op.Inputs[0].Size) {
        Value.Offset = Read(Op.Inputs[0]).Offset;
      } else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
                 Op.NumInputs == 2 && Op.Output.Size == TRI.PointerSize &&
                 Op.Inputs[1].isConst()) {
        if (const auto Base = Read(Op.Inputs[0]).Offset)
          Value.Offset = detail::checkedAffineOffset(
              *Base, static_cast<int64_t>(Op.Inputs[1].Offset),
              Op.Opcode == NdOp::INT_SUB);
      }
      Value.Bytes = Op.Output.Size;
      if (Op.Output.isReg()) {
        if (Op.Output.Offset == TRI.StackPointer) {
          if (Op.Output.Size != TRI.PointerSize || !Value.Offset)
            return {};
          Stack = *Value.Offset;
        } else if (TRI.isFlag(Op.Output.Offset, Op.Output.Size)) {
          if (Value.Derived)
            FrameFlags.insert(Op.Output.Offset);
          else
            FrameFlags.erase(Op.Output.Offset);
        } else if (Value.Derived) {
          return {};
        }
      } else if (Op.Output.isTemp()) {
        for (auto At = FirstOverlap(Op.Output);
             At != Temps.end() &&
             (At->first < Op.Output.Offset ||
              At->first - Op.Output.Offset < Op.Output.Size);) {
          if (!Budget)
            return {};
          --Budget;
          if (At->second.Derived &&
              (At->first < Op.Output.Offset ||
               At->second.Bytes >
                   Op.Output.Size - (At->first - Op.Output.Offset)))
            return {};
          At = Temps.erase(At);
        }
        Temps.emplace(Op.Output.Offset, Value);
      } else if (Value.Derived) {
        return {};
      }
    }
    // Offsets must agree at joins. Flag taint is a may-fact: retain it if
    // any predecessor carries it, and revisit users when the union grows.
    // Stable loops terminate without assuming that a backedge is unread.
    for (size_t Next : Successors[Index]) {
      if (1 + FrameFlags.size() > Budget)
        return {};
      Budget -= 1 + FrameFlags.size();
      auto &Entry = Entries[Next];
      if (!Entry) {
        Entry = FrameState{Stack, FrameFlags};
        Work.push_back(Next);
        continue;
      }
      if (Entry->Stack != Stack)
        return {};
      bool Changed = false;
      for (uint64_t Flag : FrameFlags)
        Changed |= Entry->Flags.insert(Flag).second;
      if (Changed)
        Work.push_back(Next);
    }
  }
  if (!Bounded || MatchedCalls.size() != Calls.size() ||
      llvm::any_of(Entries, [](const auto &Entry) { return !Entry; }))
    return {};
  std::set<unsigned> Observed;
  const auto Observe = [&](const MedVar &Value) {
    if (!Budget)
      return false;
    --Budget;
    if (Value.Kind == MedVar::Param && Value.Id >= 0)
      Observed.insert(unsigned(Value.Id));
    return true;
  };
  for (const auto &Block : Func.Blocks) {
    for (const auto &Phi : Block.Phis)
      for (const auto &[Pred, Value] : Phi.Args)
        if (!Observe(Value))
          return {};
    for (const auto &Op : Block.Ops)
      for (unsigned K = 0; K < Op.NumInputs; ++K)
        if (!Observe(Op.Inputs[K]))
          return {};
  }
  for (const auto &Call : Func.CallInfos)
    for (const auto &Arg : Call.Args)
      if (!Observe(Arg))
        return {};
  for (const auto &Clobber : Func.CallClobbers)
    if (!Observe(Clobber.Value) || !Observe(Clobber.PreservedInput))
      return {};
  std::set<unsigned> Result;
  for (unsigned P = Layout.Registers.size(); P < Func.Params.size(); ++P) {
    if (Func.Params[P].Kind != MedVar::Param || Func.Params[P].Id != int(P) ||
        Func.Params[P].RegOff != kNoParamReg || Observed.count(P))
      continue;
    const int64_t Begin = Layout.EntryStackBase +
                          (P - Layout.Registers.size()) * Layout.SlotBytes;
    if (Reads.size() > Budget)
      return {};
    Budget -= Reads.size();
    if (llvm::none_of(Reads, [&](const auto &Range) {
          return Range.first < Begin + Layout.SlotBytes && Begin < Range.second;
        }))
      Result.insert(P);
  }
  return Result;
}

SourceRegisterParameterReads sourceRegisterParameterReads(
    const MedFunc &Func, const SourceCalleeParameterReads &Callees,
    size_t &Budget, const SourceCalleeParameterOverwrites &Overwrites) {
  SourceRegisterParameterReads Result;
  if (!Budget || !hasSourceRegisterInputBody(Func))
    return Result;
  const auto &TRI = getTargetRegInfo(Arch::X64);
  const auto Layout = TRI.integerArgumentLayout(BinaryFormat::COFF);
  std::map<int, const MedBlock *> Blocks;
  std::set<va_t> Entries;
  std::map<ValueKey, const MedVar *> Values;
  std::map<const MedOp *, const MedCallInfo *> Calls;
  std::map<ValueKey, uint16_t> CallReads;
  for (const auto &Block : Func.Blocks) {
    const size_t Cost = 1 + Block.Ops.size() + Block.Phis.size();
    if (Cost > Budget)
      return Result;
    Budget -= Cost;
    if (!Block.ExceptionalSuccs.empty() || !Block.ExceptionalPreds.empty())
      return Result;
    if (!Blocks.emplace(Block.Id, &Block).second ||
        (Block.Id != Func.Blocks.front().Id && Block.Preds.empty()))
      return Result;
    Entries.insert(Block.StartAddr);
    for (const auto &Phi : Block.Phis) {
      if (Phi.ExceptionalEntry ||
          llvm::any_of(Phi.Args, [&](const auto &Input) {
            return Input.second.Size != Phi.Output.Size;
          }))
        return Result;
      Values.emplace(keyOf(Phi.Output), &Phi.Output);
    }
    for (const auto &Op : Block.Ops)
      if (!Op.Output.isConst() && Op.Output.Size)
        Values.emplace(keyOf(Op.Output), &Op.Output);
  }
  for (const auto &Call : Func.CallInfos) {
    const auto Block = Blocks.find(Call.BlockId);
    const MedOp *Op = nullptr;
    if (Block != Blocks.end() && Call.OpIdx >= 0 &&
        size_t(Call.OpIdx) < Block->second->Ops.size()) {
      const auto &Candidate = Block->second->Ops[Call.OpIdx];
      if (!Call.IsIndirect && !Call.SourceCallHint &&
          !Call.ArgumentsInCalleeRegisterOrder &&
          Candidate.Opcode == NdOp::CALL && Candidate.NumInputs &&
          Candidate.Inputs[0].isConst() && !Candidate.SourceCallHint &&
          Candidate.Inputs[0].ConstVal == Call.TargetAddr)
        Op = &Candidate;
    }
    if (Block != Blocks.end() && Call.OpIdx >= 0 &&
        size_t(Call.OpIdx) < Block->second->Ops.size())
      if (!Calls.emplace(&Block->second->Ops[Call.OpIdx], Op ? &Call : nullptr)
               .second)
        return Result;
    const auto Callee = Op ? Callees.find(Call.TargetAddr) : Callees.end();
    for (unsigned P = 0; P < Call.Args.size(); ++P) {
      const auto &Argument = Call.Args[P];
      if (Argument.isConst())
        continue;
      uint16_t Bytes = Argument.Size;
      if (Callee != Callees.end())
        if (const auto At = Callee->second.find(P); At != Callee->second.end())
          Bytes = std::min(Bytes, At->second);
      auto &Read = CallReads[keyOf(Argument)];
      Read = std::max(Read, Bytes);
    }
  }
  const auto SourceRead = [&](const MedOp &Op,
                              unsigned P) -> std::optional<uint16_t> {
    const auto Call = Calls.find(&Op);
    if (Op.Opcode != NdOp::CALL || !Op.NumInputs || !Op.Inputs[0].isConst() ||
        Op.SourceCallHint || (Call != Calls.end() && !Call->second))
      return std::nullopt;
    const auto Callee = Callees.find(Op.Inputs[0].ConstVal);
    if (Callee == Callees.end())
      return std::nullopt;
    const auto Read = Callee->second.find(P);
    return Read == Callee->second.end() ? std::nullopt
                                        : std::optional(Read->second);
  };
  const ValueReads Reads(Func);
  for (unsigned P = 0; P < Func.Params.size() && P < Layout.Registers.size();
       ++P) {
    const uint64_t Register = Layout.Registers[P];
    if (Func.Params[P].RegOff != Register ||
        Func.Params[P].Size > TRI.PointerSize)
      continue;
    for (uint16_t Prefix : {0, 1, 2, 4}) {
      if (Prefix >= Func.Params[P].Size)
        break;
      std::set<ValueKey> Aliases;
      std::vector<ValueKey> Pending;
      bool Seeded = false, NarrowSeed = false;
      for (const auto &Op : Func.Blocks.front().Ops) {
        if (Op.Opcode != NdOp::COPY)
          break;
        if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == Register &&
            Op.NumInputs == 1 && Op.Inputs[0].Kind == MedVar::Reg &&
            Op.Inputs[0].RegOff == Register && Op.Inputs[0].SSAVer == 0 &&
            Op.Output.Size == Op.Inputs[0].Size) {
          Seeded = true;
          if (Op.Output.Size <= Prefix) {
            NarrowSeed = true;
            continue;
          }
          Pending.push_back(keyOf(Op.Inputs[0]));
          Pending.push_back(keyOf(Op.Output));
        }
      }
      bool Safe = Seeded, Saved = false;
      const auto Follow = [&](ValueKey Key) {
        const auto At = Values.find(Key);
        if (At == Values.end())
          return false;
        const MedVar &Value = *At->second;
        if (Value.Size <= Prefix)
          return true;
        // Moving an incoming value to another physical register can make it an
        // implicit argument there. Only this register and temporaries qualify.
        if (Value.Kind != MedVar::Temp &&
            (Value.Kind != MedVar::Reg || Value.RegOff != Register))
          return false;
        Pending.push_back(Key);
        return true;
      };
      while (Safe && !Pending.empty()) {
        if (!Budget)
          return Result;
        --Budget;
        const ValueKey Key = Pending.back();
        Pending.pop_back();
        if (!Aliases.insert(Key).second)
          continue;
        if (const auto At = CallReads.find(Key);
            At != CallReads.end() && At->second > Prefix) {
          Safe = false;
          break;
        }
        if (Reads.ImplicitReads.count(Key)) {
          Safe = false;
          break;
        }
        if (const auto At = Reads.Ops.find(Key); At != Reads.Ops.end())
          for (const MedOp *Use : At->second) {
            if (!Budget)
              return Result;
            --Budget;
            if (Use->Opcode == NdOp::STORE && Use->PrologueSave &&
                Use->NumInputs == 2 && keyOf(Use->Inputs[1]) == Key &&
                keyOf(Use->Inputs[0]) != Key) {
              Saved = true;
              continue;
            }
            if (Use->Opcode == NdOp::CALL) {
              const auto Call = Calls.find(Use);
              bool Bound = Call != Calls.end() && Call->second;
              // Omitted conversion bodies retain their original positional
              // call operands. Recovered source bodies use CallInfos instead.
              if (Call == Calls.end()) {
                Bound = Use->NumInputs && Use->Inputs[0].isConst() &&
                        !Use->SourceCallHint;
                for (unsigned K = 1; K < Use->NumInputs; ++K)
                  if (keyOf(Use->Inputs[K]) == Key) {
                    const auto Read = SourceRead(*Use, K - 1);
                    Bound &= Read && *Read <= Prefix;
                  }
              }
              if (Bound) {
                Saved |= SourceRead(*Use, P) == 0;
                continue;
              }
            }
            if (Use->Opcode == NdOp::SUBBYTES && Use->NumInputs == 2 &&
                Use->Inputs[1].isConst() && Use->Inputs[1].ConstVal == 0 &&
                Use->Output.Size <= Prefix &&
                Use->Output.Size <= Use->Inputs[0].Size &&
                Use->MemoryOrdering == NdMemoryOrdering::None &&
                Use->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
                !Use->SourceCallHint && Use->IntrinsicOutputs.empty()) {
              NarrowSeed = true;
              continue;
            }
            if (Use->Opcode != NdOp::COPY || Use->NumInputs != 1 ||
                Use->MemoryOrdering != NdMemoryOrdering::None ||
                Use->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
                Use->SourceCallHint || !Use->IntrinsicOutputs.empty() ||
                Use->Output.Size != Use->Inputs[0].Size ||
                !Follow(keyOf(Use->Output))) {
              Safe = false;
              break;
            }
          }
        if (const auto At = Reads.PhiOutputs.find(Key);
            Safe && At != Reads.PhiOutputs.end()) {
          if (At->second.size() > Budget)
            return Result;
          Budget -= At->second.size();
          for (const auto &Output : At->second)
            if (!Follow(Output)) {
              Safe = false;
              break;
            }
        }
      }
      if (!Safe || (!Saved && !NarrowSeed))
        continue;
      // Source operands do not describe an opaque call's implicit registers.
      // Follow physical liveness on every path, including later setups that
      // restore the entry value. A may-write or partial write cannot kill it.
      std::vector<std::pair<int, bool>> Work{{Func.Blocks.front().Id, true}};
      std::set<std::pair<int, bool>> Seen;
      while (Safe && !Work.empty()) {
        auto [Id, Live] = Work.back();
        Work.pop_back();
        if (!Seen.emplace(Id, Live).second)
          continue;
        const auto At = Blocks.find(Id);
        if (At == Blocks.end()) {
          Safe = false;
          break;
        }
        const MedBlock &Block = *At->second;
        for (const auto &Phi : Block.Phis)
          if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == Register &&
              Aliases.count(keyOf(Phi.Output)))
            Live = true;
        bool Closed = false;
        for (const auto &Op : Block.Ops) {
          if (!Budget)
            return Result;
          --Budget;
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
            // Recovered calls can substitute away a setup COPY. An explicit
            // old operand still reintroduces that input into its physical slot.
            if (P + 1 < Op.NumInputs && Aliases.count(keyOf(Op.Inputs[P + 1])))
              Live = true;
            if (const auto Call = Calls.find(&Op);
                Call != Calls.end() && Call->second &&
                P < Call->second->Args.size() &&
                Aliases.count(keyOf(Call->second->Args[P])))
              Live = true;
            if (!Live)
              continue;
            if (const auto Read = SourceRead(Op, P); Read && *Read <= Prefix) {
              if (const auto At = Overwrites.find(Op.Inputs[0].ConstVal);
                  At != Overwrites.end() && At->second.count(P)) {
                Live = false;
              }
              continue;
            }
            const auto Proof = Func.CallUnreadRegisters.find(Op.CallSiteId);
            if (Proof == Func.CallUnreadRegisters.end() || !Op.CallSiteId ||
                !Op.NumInputs || !Op.Inputs[0].isConst() || Op.SourceCallHint ||
                Op.Addr != Proof->second.Instruction ||
                Op.OriginSeq != Proof->second.OriginSeq ||
                Op.Inputs[0].ConstVal != Proof->second.Target ||
                !callPreservesRegisterBytes(Arch::X64, Proof->second.Families,
                                            Register, TRI.PointerSize)) {
              Safe = false;
              break;
            }
          } else if (Op.Opcode == NdOp::RETURN) {
            Closed = true;
            break;
          } else if (Live && (Op.Opcode == NdOp::INDIR_BR ||
                              (Op.Opcode == NdOp::INTRINSIC &&
                               !isExplicitFlagsTransfer(Op, Arch::X64)))) {
            Safe = false;
            break;
          } else if (Op.Output.Kind == MedVar::Reg &&
                     Op.Output.RegOff == Register) {
            if (Aliases.count(keyOf(Op.Output)))
              Live = true;
            else if (Op.Output.Size >= TRI.PointerSize)
              Live = false;
          } else if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) {
            if (!Op.NumInputs || !Op.Inputs[0].isConst() ||
                !Entries.count(Op.Inputs[0].ConstVal)) {
              Safe = false;
              break;
            }
          }
        }
        if (!Closed) {
          if (Block.Succs.empty() && Live)
            Safe = false;
          if (Block.Succs.size() > Budget)
            return Result;
          Budget -= Block.Succs.size();
          for (int Successor : Block.Succs)
            Work.emplace_back(Successor, Live);
        }
      }
      if (Safe) {
        Result.emplace(P, Prefix);
        break;
      }
    }
  }
  return Result;
}

std::set<unsigned> unobservedSourceSpillParameters(const MedFunc &Func) {
  size_t Remaining = limits::ir::kSourceEntrySpillWork;
  std::set<unsigned> Result;
  for (const auto &[Parameter, Bytes] :
       sourceRegisterParameterReads(Func, {}, Remaining))
    if (!Bytes)
      Result.insert(Parameter);
  return Result;
}

void replaceKnownSourceCallArguments(
    MedFunc &Func, const SourceCalleeParameterReads &Callees) {
  if (llvm::none_of(Func.CallInfos, [&](const MedCallInfo &Call) {
        const auto At = Callees.find(Call.TargetAddr);
        return !Call.IsIndirect && !Call.SourceCallHint &&
               !Call.ArgumentsInCalleeRegisterOrder && At != Callees.end() &&
               llvm::any_of(At->second, [&](const auto &Read) {
                 return Read.first < Call.Args.size() &&
                        Read.second < Call.Args[Read.first].Size;
               });
      }))
    return;
  std::map<ValueKey, const MedOp *> Definitions;
  std::map<int, const MedBlock *> Blocks;
  for (const auto &Block : Func.Blocks) {
    Blocks.emplace(Block.Id, &Block);
    for (const auto &Op : Block.Ops) {
      if (Op.Output.isConst() || !Op.Output.Size || Op.Output.Id < 0)
        continue;
      auto [At, Added] = Definitions.emplace(keyOf(Op.Output), &Op);
      if (!Added)
        At->second = nullptr;
    }
  }
  size_t Remaining = limits::ir::kSourceEntrySpillWork;
  const auto ConstantPrefix = [&](const MedVar &Value,
                                  uint16_t Bytes) -> std::optional<uint64_t> {
    struct Slice {
      MedVar Value;
      unsigned Offset, Bytes, Destination;
    };
    std::vector<Slice> Pending{{Value, 0, Bytes, 0}};
    std::set<std::tuple<ValueKey, unsigned, unsigned>> Seen;
    uint64_t Constant = 0;
    while (!Pending.empty()) {
      if (!Remaining)
        return std::nullopt;
      --Remaining;
      const auto [V, Offset, Count, Destination] = Pending.back();
      Pending.pop_back();
      if (!V.Size || V.Size > sizeof(uint64_t) || !Count ||
          Offset + Count > V.Size || Destination + Count > sizeof(uint64_t))
        return std::nullopt;
      if (V.isConst()) {
        if (V.Provenance != ConstantAddressProvenance::Scalar &&
            V.Provenance != ConstantAddressProvenance::Unknown)
          return std::nullopt;
        Constant |= ((V.ConstVal >> (8 * Offset)) &
                     llvm::maskTrailingOnes<uint64_t>(8 * Count))
                    << (8 * Destination);
        continue;
      }
      if (!Seen.emplace(keyOf(V), Offset, Count).second)
        return std::nullopt;
      const auto At = Definitions.find(keyOf(V));
      if (At == Definitions.end() || !At->second)
        return std::nullopt;
      const auto &Op = *At->second;
      if (Op.Output.Size != V.Size ||
          Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Op.SourceCallHint || !Op.IntrinsicOutputs.empty())
        return std::nullopt;
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
          Op.Inputs[0].Size == V.Size) {
        Pending.push_back({Op.Inputs[0], Offset, Count, Destination});
      } else if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
                 Op.Inputs[1].isConst() &&
                 Op.Inputs[1].ConstVal <= Op.Inputs[0].Size &&
                 V.Size <= Op.Inputs[0].Size - Op.Inputs[1].ConstVal) {
        Pending.push_back({Op.Inputs[0],
                           Offset + unsigned(Op.Inputs[1].ConstVal), Count,
                           Destination});
      } else if (Op.Opcode == NdOp::CONCAT && Op.NumInputs == 2 &&
                 Op.Inputs[0].Size + Op.Inputs[1].Size == V.Size) {
        const unsigned Low = Op.Inputs[1].Size;
        const unsigned Taken = Offset < Low ? std::min(Count, Low - Offset) : 0;
        if (Taken)
          Pending.push_back({Op.Inputs[1], Offset, Taken, Destination});
        if (Taken != Count)
          Pending.push_back({Op.Inputs[0], Offset + Taken - Low, Count - Taken,
                             Destination + Taken});
      } else if (Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs == 1 &&
                 Op.Inputs[0].Size <= V.Size) {
        if (Offset < Op.Inputs[0].Size)
          Pending.push_back(
              {Op.Inputs[0], Offset,
               std::min(Count, unsigned(Op.Inputs[0].Size) - Offset),
               Destination});
      } else {
        return std::nullopt;
      }
    }
    return Constant;
  };
  for (auto &Call : Func.CallInfos) {
    if (Call.IsIndirect || Call.SourceCallHint ||
        Call.ArgumentsInCalleeRegisterOrder)
      continue;
    const auto Callee = Callees.find(Call.TargetAddr);
    const auto Block = Blocks.find(Call.BlockId);
    if (Callee == Callees.end() || Block == Blocks.end() || Call.OpIdx < 0 ||
        size_t(Call.OpIdx) >= Block->second->Ops.size())
      continue;
    const auto &Op = Block->second->Ops[Call.OpIdx];
    if (Op.Opcode != NdOp::CALL || !Op.NumInputs || !Op.Inputs[0].isConst() ||
        Op.SourceCallHint || Op.Inputs[0].ConstVal != Call.TargetAddr)
      continue;
    for (const auto &[P, Bytes] : Callee->second) {
      if (P >= Call.Args.size() || Bytes >= Call.Args[P].Size)
        continue;
      if (const auto Value = Bytes ? ConstantPrefix(Call.Args[P], Bytes)
                                   : std::optional<uint64_t>(0))
        Call.Args[P] = MedVar::makeConst(*Value, Call.Args[P].Size,
                                         ConstantAddressProvenance::Scalar);
    }
  }
}

void markUnobservedBorrowedFrameStores(
    MedFunc &Func, Arch Architecture, BinaryFormat Format,
    const std::map<va_t, const MedFunc *> &Callees) {
  if (Architecture != Arch::X64 || Format != BinaryFormat::COFF ||
      Func.CC != CallingConv::Win64 || Func.SkippedSSA ||
      Func.AnalystStackFrame || Func.SourceParametersBound ||
      Func.EntryKind != StackEntryKind::Call)
    return;
  std::vector<FrameAccess> Loads, Stores;
  frameAccessesOf(Func, Architecture, Format, Func.EntryKind, Loads, Stores);
  const auto Layout =
      getTargetRegInfo(Architecture).integerArgumentLayout(Format);
  const auto Candidate = [&](const FrameAccess &Store) {
    return !Store.Op->PrologueSave && !Store.Op->Inputs[1].isConst() &&
           isPrivateFrameAccess(Func, Layout, Store.Offset, Store.Size);
  };
  if (llvm::none_of(Stores, Candidate))
    return;
  FrameBorrowReads Proof(Architecture, Format, Callees);
  const auto Reads = Proof.frame(Func);
  if (!Reads)
    return;
  std::map<const MedOp *, std::pair<int64_t, uint16_t>> Writes;
  for (const FrameAccess &Store : Stores)
    Writes.emplace(Store.Op, std::pair{Store.Offset, Store.Size});
  for (const FrameAccess &Store : Stores) {
    if (!Candidate(Store))
      continue;
    const auto Read = std::partition_point(
        Reads->begin(), Reads->end(), [&](const FrameBorrowReads::Range &R) {
          return R.second <= Store.Offset;
        });
    if (Read == Reads->end() || !Store.overlaps(Read->first, Read->second) ||
        Proof.unreadAfter(Func, *Store.Op, Store.Offset, Store.Size, Writes))
      Store.Op->PrologueSave = true;
  }
}

void markUndefinedScratchStores(MedFunc &Func, Arch Architecture,
                                BinaryFormat Format) {
  // A variadic prologue spills what its callers pass in argument registers,
  // AL's vector count among them, and an exception dispatcher enters blocks
  // with registers no predecessor sets.
  if (Func.Blocks.empty() || Func.SkippedSSA || Func.AnalystStackFrame ||
      Func.FrameSize <= 0 || Func.IsVariadic || Func.ExceptionMetadata ||
      llvm::any_of(Func.Blocks, [](const MedBlock &Block) {
        return !Block.ExceptionalPreds.empty() ||
               !Block.ExceptionalSuccs.empty();
      }))
    return;
  const CallArgumentConvention *Convention =
      callArgumentConvention(Architecture, Format);
  if (!Convention || !Convention->UndefinedIncomingScratchRegisters)
    return;
  const auto UndefinedStore = [&](const MedOp &Op) {
    return Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 && !Op.PrologueSave &&
           holdsUndefinedIncomingScratch(Func, Architecture, Format,
                                         Op.Inputs[1]);
  };
  if (llvm::none_of(Func.Blocks, [&](const MedBlock &Block) {
        return llvm::any_of(Block.Ops, UndefinedStore);
      }))
    return;
  std::vector<FrameAccess> Loads, Stores;
  frameAccessesOf(Func, Architecture, Format, Func.EntryKind, Loads, Stores);
  // The bytes a load or a stack variable of the function reads.
  std::vector<std::pair<int64_t, int64_t>> Reads;
  for (const FrameAccess &Load : Loads)
    Reads.emplace_back(Load.Offset, Load.Offset + Load.Size);
  const auto StackRead = [&](const MedVar &V) {
    if (V.Kind == MedVar::Stack && V.Size)
      Reads.emplace_back(V.StackOff, V.StackOff + V.Size);
  };
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &Incoming : Phi.Args)
        StackRead(Incoming.second);
    for (const MedOp &Op : Block.Ops)
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        StackRead(Op.Inputs[I]);
  }
  const auto ReadBack = [&](const FrameAccess &Store) {
    return llvm::any_of(Reads, [&](const std::pair<int64_t, int64_t> &Read) {
      return Store.overlaps(Read.first, Read.second);
    });
  };
  const auto Private = [&](const FrameAccess &Store) {
    return Store.Offset >= -Func.FrameSize && Store.Offset < 0 &&
           int64_t(Store.Size) <= -Store.Offset && UndefinedStore(*Store.Op);
  };
  // The variadic count register (System V's RAX) carries nothing into a
  // function that is not variadic, unlike R10, a nested function's static
  // chain.  Where a routine receives the slot's address before anything may
  // read the bytes, a read sees what the routine wrote or bytes the program
  // never defined, as a local's would.  A slot read back before then holds a
  // spill, which only a convention recovery does not model (`__regcall`)
  // gives a value: that store stays.
  const std::optional<unsigned> CountFamily =
      Convention->VariadicVectorCountRegister
          ? gprFamilyOf(Architecture, *Convention->VariadicVectorCountRegister)
          : std::nullopt;
  const auto CountStore = [&](const FrameAccess &Store) {
    return CountFamily &&
           gprFamilyOf(Architecture, Store.Op->Inputs[1].RegOff) == CountFamily;
  };
  std::map<const MedOp *, std::pair<size_t, size_t>> Positions;
  std::map<const MedOp *, const FrameAccess *> LoadAt;
  std::map<ValueKey, int64_t> Offsets;
  std::map<std::pair<int, int>, const MedCallInfo *> Calls;
  if (llvm::any_of(Stores, [&](const FrameAccess &Store) {
        return Private(Store) && CountStore(Store) && ReadBack(Store);
      })) {
    for (size_t B = 0; B < Func.Blocks.size(); ++B)
      for (size_t I = 0; I < Func.Blocks[B].Ops.size(); ++I)
        Positions.emplace(&Func.Blocks[B].Ops[I], std::pair{B, I});
    for (const FrameAccess &Load : Loads)
      LoadAt.emplace(Load.Op, &Load);
    for (const MedCallInfo &Call : Func.CallInfos)
      Calls.emplace(std::pair{Call.BlockId, Call.OpIdx}, &Call);
    // The call operands and returned values that may be frame addresses,
    // all by one proof.
    std::vector<MedVar> Values;
    for (const MedBlock &Block : Func.Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
            Op.Opcode == NdOp::RETURN)
          for (unsigned K = 0; K < Op.NumInputs; ++K)
            if (!Op.Inputs[K].isConst())
              Values.push_back(Op.Inputs[K]);
    for (const MedCallInfo &Call : Func.CallInfos)
      for (const MedVar &Arg : Call.Args)
        if (!Arg.isConst())
          Values.push_back(Arg);
    const std::vector<std::optional<int64_t>> Found =
        entryStackOffsets(Func, Values, Architecture, Format, Func.EntryKind);
    for (size_t I = 0; I < Values.size(); ++I)
      if (Found[I])
        Offsets.emplace(keyOf(Values[I]), *Found[I]);
  }
  size_t Budget = limits::ir::kUndefinedScratchEscapeWork;
  for (const FrameAccess &Store : Stores)
    if (Private(Store) &&
        (!ReadBack(Store) ||
         (CountStore(Store) &&
          slotAddressReachesCallFirst(Func, Architecture, Store, Positions,
                                      LoadAt, Offsets, Calls, Budget))))
      Store.Op->PrologueSave = true;
}

} // namespace neverd
