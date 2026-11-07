//===- RegistrationFinallyFrame.cpp - Returning finally frame effects -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Interprets finally effects under the actual ordinary or runtime invocation.

#include "RegistrationCallbackGraph.h"
#include "RegistrationFrameEffects.h"
#include "RegistrationStateSolver.h"

#include "neverd/lift/X86/X86Regs.h"

#include <utility>

namespace neverd::registration_state {

bool RegistrationStateSolver::runLocalFinally(
    va_t Target, FrameState &Parent, bool OrdinaryInvocation,
    std::vector<RegistrationObjectExtent> *Writes, SEHUnwindJump *Jump,
    SEHFrameOutcomes *Outcomes, SEHHandlerOutcomes *Selected) {
  return runSEHCallbackFrame(Target, Parent, OrdinaryInvocation, Writes, Jump,
                             nullptr, 0, std::nullopt, Outcomes, Selected);
}

bool RegistrationStateSolver::runSEHCallbackFrame(
    va_t Target, FrameState &Parent, bool OrdinaryInvocation,
    std::vector<RegistrationObjectExtent> *Writes, SEHUnwindJump *Jump,
    SEHFilterOutcomes *FilterOutcomes, unsigned Depth,
    std::optional<int32_t> ParentStackOffset, SEHFrameOutcomes *Outcomes,
    SEHHandlerOutcomes *Selected, std::optional<SEHExceptionOrigin> Origin) {
  // A filter may leave through a jump; only its actual returns contribute
  // dispositions and restore the suspended callback frame.
  if (FilterOutcomes && (OrdinaryInvocation || Outcomes || Selected))
    return false;
  const auto Entry = Entries.find(Target);
  // An ordinary CALL has a proved current ESP and its own return PC. SavedESP
  // belongs to a separate runtime restoration protocol; finally-only helpers
  // need not initialize that slot.
  const auto ParentSP =
      ParentStackOffset ? ParentStackOffset
      : OrdinaryInvocation
          ? Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset
          : Parent.load(*Chain.RegistrationOffset - 8, 4).Offset;
  if (Entry == Entries.end() || !ParentSP ||
      *ParentSP >= *Chain.RegistrationOffset - 8 ||
      !charge(Parent.cellCount() + Parent.OtherRegisterBytes.size() + 10))
    return false;
  FrameState Initial;
  if (OrdinaryInvocation)
    Initial = Parent;
  else {
    Initial.Cells = Parent.Cells;
    for (auto &Register : Initial.Registers)
      Register.MayBeFrame = true;
    Initial.OtherRegistersMayBeFrame = true;
    Initial.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
        FrameValue::frame(0);
  }
  if (FilterOutcomes)
    Initial.enterSEHFilter(Target, *Chain.RegistrationOffset - 4, Origin);
  else
    Initial.enterCallback(Target);
  const auto InitialLevel = Initial.load(Chain.TryLevelOffset, 4).Constant;
  if (!InitialLevel || !validLevel(int32_t(*InitialLevel)))
    return false;
  RegistrationCallbackGraph Graph;
  const auto Charge = [&](size_t Work) { return charge(Work); };
  if (!Graph.add(Entry->second, 0, Initial, std::nullopt, Charge))
    return false;
  SEHFrameOutcomes Returned;
  SEHFilterOutcomes FilterReturned;
  SEHHandlerOutcomes SelectedHandlers;
  const auto RecordReturn = [&](const FrameState &State) {
    if (!FilterOutcomes)
      return mergeSEHFrameOutcome(Returned, State);
    const auto &Value = State.Registers[x86reg::RAX / x86reg::GeneralRegStride];
    return !Value.MayBeFrame && Value.Constant &&
           mergeSEHFrameOutcome(FilterReturned[*Value.Constant], State);
  };
  while (!Graph.empty()) {
    const size_t Current = Graph.take();
    const size_t I = Graph.node(Current).Block;
    const size_t FirstOp = Graph.node(Current).FirstOp;
    FrameState State = Graph.node(Current).Frame;
    FrameTransfer Transfer(State, *Chain.RegistrationOffset);
    const auto &Block = Function.Blocks[I];
    if (InvalidBlocks[I] ||
        (OrdinaryInvocation && !validateSEHInvocationBlock(I)))
      return false;
    bool HasReturn = false;
    bool Abandoned = false;
    std::set<va_t> ProvenStores;
    SEHFrameOutcomes NestedOutcomes;
    std::optional<size_t> ResumeAt;
    for (size_t OpIndex = FirstOp; OpIndex < Block.Ops.size(); ++OpIndex) {
      const auto &Op = Block.Ops[OpIndex];
      if (!charge(1 + State.cellCount() + Op.Output.Size) ||
          (Op.Opcode == NdOp::INTRINSIC && !isFramePreservingIntrinsic(Op)) ||
          Op.Opcode == NdOp::INDIR_BR ||
          Op.MemoryOrdering != NdMemoryOrdering::None)
        return false;
      const auto Source = Boundaries.find(Op.Addr);
      if (Op.Seq < 0 || Source == Boundaries.end() ||
          Source->second.first != Block.Id ||
          Source->second.second.FirstOp > OpIndex ||
          Source->second.second.OpCount <=
              OpIndex - Source->second.second.FirstOp ||
          Source->second.second.FirstOp > Block.Ops.size() ||
          Source->second.second.OpCount >
              Block.Ops.size() - Source->second.second.FirstOp)
        return false;
      Transfer.beginInstruction(Op.Addr);
      if (Op.Opcode == NdOp::INDIR_CALL) {
        const auto SP = State.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                            .CallbackAddress;
        SEHFrameOutcomes Continued;
        if (FilterOutcomes || !JumpRuntime || !JumpRuntime->Raise || !SP ||
            SP->Entry != Target ||
            !State.callbackMemoryIsPrivate(*SP, 16, true) ||
            !JumpRuntime->Raise(Op, State, WorkUsed) ||
            State.loadCallback(SP->Offset + 4, 4).Constant != 0 ||
            !runSEHFilterSearch(Op, State, Writes, Continued,
                                /*RequireResume=*/true,
                                Selected ? &SelectedHandlers : nullptr, Jump))
          return false;
        if (Continued.empty()) {
          if (Source->second.second.FirstOp + Source->second.second.OpCount !=
              OpIndex + 1)
            return false;
          // Selection or a jump abandons all instructions after this call.
          // Limit the source-store audit to the actual suspended prefix.
          Abandoned = true;
          ResumeAt = OpIndex + 1;
          break;
        }
        // Continue execution restores this suspended invocation, including
        // its nonvolatile registers and private stack. Search-filter writes
        // to the parent remain visible after the stdcall consumes its args.
        if (Continued.size() != 1) {
          // Resume each state at the next instruction. Lifter temporaries
          // cannot cross this boundary; a split inside one instruction would
          // lose their reaching values and is not a valid continuation.
          if (Source->second.second.FirstOp + Source->second.second.OpCount !=
              OpIndex + 1)
            return false;
          for (auto &[Level, Frame] : Continued) {
            FrameTransfer ContinuedTransfer(Frame, *Chain.RegistrationOffset);
            ContinuedTransfer.write(Op, {});
            Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
                FrameValue::callbackFrame(Target, SP->Offset + 16);
            Frame.trimCallbackCells();
          }
          NestedOutcomes = std::move(Continued);
          ResumeAt = OpIndex + 1;
          break;
        }
        State = std::move(Continued.begin()->second);
        Transfer.write(Op, {});
        State.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
            FrameValue::callbackFrame(Target, SP->Offset + 16);
        State.trimCallbackCells();
        continue;
      }
      if (Op.Opcode == NdOp::CALL) {
        if (Op.NumInputs == 1 && Op.Inputs[0].isConst() &&
            Op.Inputs[0].Size == 4 &&
            FinallyTargets.count(Op.Inputs[0].Offset)) {
          if (FilterOutcomes ||
              !runSEHNestedFinallyFrame(I, Op, State, *ParentSP, Writes, Depth,
                                        Jump, NestedOutcomes,
                                        Selected ? &SelectedHandlers : nullptr))
            return false;
          if (NestedOutcomes.empty()) {
            Abandoned = true;
            ResumeAt = OpIndex + 1;
          }
          // The shared CALL proof permits only the checked tail JMP after
          // this call. Its successors can be resumed once per returned state.
          break;
        }
        if (!Jump ||
            Source->second.second.FirstOp + Source->second.second.OpCount !=
                OpIndex + 1 ||
            !recordSEHUnwindJump(I, Op, State, Target, *Jump))
          return false;
        Abandoned = true;
        ResumeAt = OpIndex + 1;
        break;
      }
      if (Op.Opcode == NdOp::RETURN) {
        if (!callbackReturnInstruction(I, Op) ||
            State.CallbackEntry != Target || !State.callbackStackIsRestored(0))
          return false;
        if (!RecordReturn(State))
          return false;
        HasReturn = true;
        break;
      }
      const auto Memory = lowMemoryOperands(Op);
      if (Memory.Address) {
        if (!Memory.Complete ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            (Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE))
          return false;
        const auto Address = Transfer.read(*Memory.Address);
        if (Conditional && Address.Offset && *Address.Offset >= 4) {
          if (!checkConditionalMemory(State, Op, Transfer))
            return false;
          if (Op.Opcode == NdOp::STORE) {
            State.store(*Address.Offset, Memory.AccessSize,
                        Transfer.read(*Memory.StoredValue));
            if (Writes)
              Writes->push_back(
                  {*Address.Offset,
                   int32_t(int64_t(*Address.Offset) + Memory.AccessSize)});
          }
        } else if (Address.CallbackAddress) {
          if (!charge(Memory.AccessSize) ||
              !State.callbackMemoryIsPrivate(*Address.CallbackAddress,
                                             Memory.AccessSize,
                                             Op.Opcode == NdOp::LOAD))
            return false;
          if (Op.Opcode == NdOp::STORE)
            State.storeCallback(Address.CallbackAddress->Offset,
                                Memory.AccessSize,
                                Transfer.read(*Memory.StoredValue));
        } else if (Address.SEHException) {
          if (Op.Opcode != NdOp::LOAD ||
              !State.sehExceptionLoad(*Address.SEHException, Memory.AccessSize))
            return false;
        } else if (Address.preferredAddress() && !Address.MayBeFrame) {
          const bool Write = Op.Opcode == NdOp::STORE;
          const uint64_t End =
              uint64_t(*Address.preferredAddress()) + Memory.AccessSize;
          const uint64_t TableEnd =
              Chain.ScopeTableVA + (EH4 ? 16 : 0) + Chain.Scopes.size() * 12;
          if (!JumpRuntime || !JumpRuntime->ImageMemory ||
              !JumpRuntime->ImageMemory(*Address.preferredAddress(),
                                        Memory.AccessSize, Write, WorkUsed) ||
              (Write && (*Address.preferredAddress() < TableEnd &&
                         Chain.ScopeTableVA < End)) ||
              (Write && EH4 &&
               *Address.preferredAddress() < SecurityCookieVA + 4 &&
               SecurityCookieVA < End) ||
              (Write && Transfer.read(*Memory.StoredValue).MayBeFrame))
            return false;
          if (!Write) {
            // These runtime bits survive frame relocation. They remain
            // potentially aliasing pointers, but scalar observations do not
            // depend on either regenerated private stack's placement.
            Transfer.write(Op, FrameValue::taint(true, true));
            continue;
          }
        } else if (!Address.Offset || *Address.Offset < *ParentSP ||
                   int64_t(*Address.Offset) + Memory.AccessSize > 0) {
          return false;
        } else if (Op.Opcode == NdOp::STORE) {
          const auto Value = Transfer.read(*Memory.StoredValue);
          if (Address.Offset == Chain.TryLevelOffset) {
            const auto Level = State.load(Chain.TryLevelOffset, 4).Constant;
            std::set<int32_t> Written;
            if (!Level || !ProvenStores.insert(Op.Addr).second ||
                !transferTryLevelStore(I, Op, Address, Value, {int32_t(*Level)},
                                       false, Written) ||
                Written.size() != 1)
              return false;
            State.store(*Address.Offset, Memory.AccessSize, Value);
            State.store(*Chain.TryLevelOffset, 4,
                        FrameValue::constant(uint32_t(*Written.begin())));
          } else {
            // SavedESP, exception pointers and the rest of the registration
            // remain runtime-owned. Only the checked state recipe can write
            // through that administrative boundary.
            if (!frameObjectIsDisjointFromAdministration(
                    *Address.Offset,
                    int64_t(*Address.Offset) + Memory.AccessSize) ||
                Value.CallbackAddress || (Value.MayBeFrame && !Value.Offset))
              return false;
            State.store(*Address.Offset, Memory.AccessSize, Value);
          }
          if (Writes)
            Writes->push_back(
                {*Address.Offset,
                 int32_t(int64_t(*Address.Offset) + Memory.AccessSize)});
        }
      }
      Transfer.write(Op, Transfer.evaluate(Op, true));
      if (Op.Output.isReg() && Op.Output.Offset < x86reg::RSP + 4 &&
          x86reg::RSP < Op.Output.Offset + Op.Output.Size) {
        const auto SP = State.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                            .CallbackAddress;
        if (!SP || SP->Entry != Target || SP->Offset > 0 ||
            !charge(State.CallbackCells.size() +
                    State.InitializedCallbackBytes.size()))
          return false;
        State.trimCallbackCells();
      }
    }
    const va_t Begin = FirstOp ? Block.Ops[FirstOp].Addr : Block.StartAddr;
    const va_t End = ResumeAt && *ResumeAt < Block.Ops.size()
                         ? Block.Ops[*ResumeAt].Addr
                         : Block.EndAddr;
    for (auto Store = Stores.lower_bound(Begin);
         Store != Stores.end() && Store->first < End; ++Store)
      if (!ProvenStores.count(Store->first))
        return false;
    if (Abandoned)
      continue;
    if (ResumeAt && *ResumeAt < Block.Ops.size()) {
      for (const auto &[Level, Frame] : NestedOutcomes)
        if (!Graph.add(I, *ResumeAt, Frame, Current, Charge))
          return false;
      continue;
    }
    if (HasReturn) {
      if (!Block.Succs.empty())
        return false;
      continue;
    }
    if (Block.Succs.empty())
      return false;
    const auto SelectedBranch =
        NestedOutcomes.empty() ? knownSuccessor(Block, Transfer) : std::nullopt;
    if (NestedOutcomes.empty() && !mergeSEHFrameOutcome(NestedOutcomes, State))
      return false;
    for (const auto &[Level, Outcome] : NestedOutcomes)
      for (int Next : Block.Succs) {
        const auto Successor = Index.find(Next);
        if (Successor == Index.end())
          return false;
        if ((!SelectedBranch ||
             Function.Blocks[Successor->second].StartAddr == *SelectedBranch) &&
            !Graph.add(Successor->second, 0, Outcome, Current, Charge))
          return false;
      }
  }
  if (Returned.empty() && FilterReturned.empty() && SelectedHandlers.empty() &&
      (!Jump || Jump->Resumed.empty()))
    return false;
  // A bounded CFG loop advances to different complete frames. A recurrent
  // execution configuration still cannot borrow a sibling's terminal exit.
  if (!Graph.closes(Charge))
    return false;
  if (FilterOutcomes) {
    for (auto &[Disposition, Frames] : FilterReturned)
      for (auto &[Level, Frame] : Frames) {
        // Runtime exception pointers expire with this search invocation.
        // Returning from its filter restores the suspended callback stack.
        Frame.leaveCallback();
        FrameState Projected = Parent;
        Projected.Cells = std::move(Frame.Cells);
        if (!mergeSEHFrameOutcome((*FilterOutcomes)[Disposition],
                                  std::move(Projected)))
          return false;
      }
    return true;
  }
  for (auto &[Scope, Frames] : SelectedHandlers)
    for (auto &[Level, Frame] : Frames)
      if (!Selected ||
          !mergeSEHFrameOutcome((*Selected)[Scope], std::move(Frame)))
        return false;
  std::optional<FrameState> Joined;
  for (auto &[Level, ReturnedFrame] : Returned) {
    FrameState Projected = Parent;
    Projected.Cells = std::move(ReturnedFrame.Cells);
    if (OrdinaryInvocation) {
      // Source finally callbacks retain their actual register effects. The
      // caller separately restores the SP after the checked near RET.
      Projected.Registers = ReturnedFrame.Registers;
      Projected.OtherRegisterBytes =
          std::move(ReturnedFrame.OtherRegisterBytes);
      Projected.OtherRegistersMayBeFrame =
          ReturnedFrame.OtherRegistersMayBeFrame;
    }
    if (Outcomes && !mergeSEHFrameOutcome(*Outcomes, Projected))
      return false;
    if (!Joined)
      Joined = std::move(Projected);
    else if (!charge(Joined->cellCount() + Projected.cellCount() + 10))
      return false;
    else
      Joined->merge(Projected);
  }
  // A consumer without a partitioned result cannot use a merged unknown state
  // to select the next runtime action.
  if (!Outcomes && Returned.size() != 1)
    return false;
  if (Joined)
    Parent = std::move(*Joined);
  return true;
}

} // namespace neverd::registration_state
