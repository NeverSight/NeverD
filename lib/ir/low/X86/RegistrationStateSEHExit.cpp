//===- RegistrationStateSEHExit.cpp - Physical SEH invocation exits -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Binds ordinary returns and nonlocal exits to their active SEH invocation.

#include "RegistrationControl.h"
#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/lift/X86/X86Regs.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::validateSEHInvocationBlock(size_t I) {
  const auto &Block = Function.Blocks[I];
  if (!charge(Block.Ops.size() + Block.InstructionBoundaries.size() +
              Block.Succs.size() + 1))
    return false;
  if (auto Error = validateLowInstructionBoundaries(
          Block, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    return false;
  }
  if (Block.InstructionBoundaries.empty())
    return false;
  for (const auto &Insn : Block.InstructionBoundaries)
    if (Insn.Mode != InstructionMode::Default ||
        Insn.TargetMode != LowInstructionTargetMode::Preserve ||
        (&Insn != &Block.InstructionBoundaries.back() &&
         Insn.Control != LowInstructionControl::None &&
         (Insn.Control != LowInstructionControl::Call ||
          isUnconditionalNoReturn(Insn) ||
          hasLowInstructionControlFlag(
              Insn.ControlFlags, LowInstructionControlFlag::Conditional) ||
          hasLowInstructionControlFlag(
              Insn.ControlFlags, LowInstructionControlFlag::InstructionGuard))))
      return false;
  // An ordinary returning call can precede the terminator in one decoded
  // block. Its invocation-specific effects are checked by transferBlock;
  // accepting the physical fallthrough here grants no callee memory fact.
  std::set<va_t> Actual, Expected;
  for (int Next : Block.Succs) {
    const auto Successor = Index.find(Next);
    if (Successor == Index.end() ||
        !Actual.insert(Function.Blocks[Successor->second].StartAddr).second)
      return false;
  }
  const auto &Insn = Block.InstructionBoundaries.back();
  if (Insn.Control == LowInstructionControl::Branch && !Block.Ops.empty()) {
    const auto &Branch = Block.Ops.back();
    const bool Conditional = Branch.Opcode == NdOp::COND_BR;
    if ((!Conditional && Branch.Opcode != NdOp::BRANCH) ||
        Branch.NumInputs != (Conditional ? 2 : 1) ||
        !isPE32CodeTarget(Branch.Inputs[0]))
      return false;
    Expected.insert(Branch.Inputs[0].Offset);
    if (Conditional)
      Expected.insert(Block.EndAddr);
  } else if (Insn.Control == LowInstructionControl::None ||
             (Insn.Control == LowInstructionControl::Call &&
              !isUnconditionalNoReturn(Insn)))
    Expected.insert(Block.EndAddr);
  else if (Insn.Control != LowInstructionControl::Return &&
           Insn.Control != LowInstructionControl::Call)
    return false;
  return Actual == Expected;
}

bool RegistrationStateSolver::enterOrdinaryFinally(size_t I,
                                                   const Domain &Before,
                                                   const LowOp &Op,
                                                   BlockFacts &Facts) {
  const auto Refuse = [&] {
    Facts.Invalid = true;
    Result.SEHInvocationExitsComplete = false;
    return true;
  };
  const auto Entry = Entries.find(Op.Inputs[0].Offset);
  const auto SP =
      Before.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  const bool ParentCall = Before.Parent && !Before.Callback &&
                          !Before.Frame.CallbackEntry && SP.Offset &&
                          *SP.Offset < int64_t(*Chain.RegistrationOffset) - 8 &&
                          *SP.Offset >= INT32_MIN + 4;
  const bool CallbackCall =
      !Before.Parent && Before.Callback &&
      Before.Activation.Entry == Before.Frame.CallbackEntry &&
      !Before.Activation.SearchFilter && SP.CallbackAddress &&
      SP.CallbackAddress->Entry == Before.Frame.CallbackEntry &&
      SP.CallbackAddress->Offset <= 0 &&
      SP.CallbackAddress->Offset >=
          -int64_t(limits::kMaxSourceCallBorrowedBytes) &&
      Before.SEHCallers.size() < 16;
  if (Before.Unknown || Facts.Invalid || (!ParentCall && !CallbackCall) ||
      Chain.RealignedFrame ||
      Before.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset !=
          0 ||
      Entry == Entries.end() ||
      !validateSEHCallContinuation(
          I, Op, CallbackCall ? *Before.Activation.Entry : 0) ||
      !charge(Before.Frame.cellCount() + 10))
    return Refuse();
  Domain Root = Before;
  Root.Activation = {Op.Inputs[0].Offset, false, Op.Addr};
  if (CallbackCall) {
    if (!charge(Before.sehCallerCellCount() +
                Before.Frame.CallbackCells.size() +
                Before.Frame.InitializedCallbackBytes.size() + 10))
      return Refuse();
    Root.Activation.Callers = Before.Activation.Callers;
    Root.Activation.Callers.emplace_back(*Before.Activation.Entry,
                                         Before.Activation.OrdinaryCall);
    Domain::SEHCaller Caller;
    Caller.Activation = Before.Activation;
    Caller.Stack.CallbackEntry = Before.Frame.CallbackEntry;
    Caller.Stack.CallbackCells = Before.Frame.CallbackCells;
    Caller.Stack.InitializedCallbackBytes =
        Before.Frame.InitializedCallbackBytes;
    Caller.Stack.Registers[x86reg::RSP / x86reg::GeneralRegStride] = SP;
    Caller.OtherCallback = Before.OtherCallback;
    Caller.OrdinaryCallStack = Before.OrdinaryCallStack;
    Caller.StackRestore = Before.StackRestore;
    Root.SEHCallers.push_back(std::move(Caller));
  }
  Root.Parent = false;
  Root.Callback = true;
  Root.OtherCallback = false;
  Root.OrdinaryCallStack = SP.Offset;
  Root.StackRestore.reset();
  Root.Frame.enterCallback(Op.Inputs[0].Offset);
  merge(Entry->second, Root);
  // This specific activation enters the body. Its ordinary CALL successor is
  // supplied only by an inspected return, never by a target-wide noreturn tag.
  Facts.InvocationAtExit = true;
  return true;
}

void RegistrationStateSolver::recordSEHStackRestore(
    const LowOp &Op, Domain &After, const FrameTransfer &Transfer,
    const FrameValue &Value) {
  if (KnownCxx || !After.Callback || After.Parent ||
      After.Activation.SearchFilter || !After.Activation.Entry ||
      !FinallyTargets.count(*After.Activation.Entry) || !Op.Output.isReg() ||
      Op.Output.Offset >= x86reg::RSP + 4 ||
      x86reg::RSP >= Op.Output.Offset + Op.Output.Size)
    return;
  // Arithmetic on the restored parent coordinate may allocate argument slots;
  // the exit still has to recover exactly the authenticated SavedESP snapshot.
  if (After.StackRestore && Value.Offset)
    return;
  // Pushes and pops within the active callback do not restore the parent's
  // SavedESP. Keep any suspended ordinary finally invocation until its exact
  // near return, even when the inner callback allocates private scratch.
  if (Value.CallbackAddress &&
      Value.CallbackAddress->Entry == After.Activation.Entry) {
    After.StackRestore.reset();
    return;
  }
  After.StackRestore.reset();
  After.SEHCallers.clear();
  const auto Source = Boundaries.find(Op.Addr);
  if ((Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::COPY) ||
      Op.NumInputs != 1 || Op.Output.Size != 4 ||
      Op.Output.Offset != x86reg::RSP ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None || Op.Seq < 0 ||
      !Value.Offset || *Value.Offset >= *Chain.RegistrationOffset - 8 ||
      After.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset !=
          0 ||
      Source == Boundaries.end() || !Index.count(Source->second.first) ||
      Source->second.second.Control != LowInstructionControl::None ||
      Source->second.second.ControlFlags != LowInstructionControlFlag::None)
    return;
  const LowOp *Load = &Op;
  if (Op.Opcode == NdOp::COPY) {
    const auto &Block = Function.Blocks[Index.at(Source->second.first)];
    const auto &Insn = Source->second.second;
    if (!Op.Inputs[0].isTemp() || Op.Inputs[0].Size != 4 || Op.Seq == 0 ||
        Insn.FirstOp > Block.Ops.size() ||
        Insn.OpCount > Block.Ops.size() - Insn.FirstOp ||
        !charge(Insn.OpCount + 1))
      return;
    const size_t End = Insn.FirstOp + Insn.OpCount;
    size_t Current = Insn.FirstOp;
    while (Current < End && &Block.Ops[Current] != &Op)
      ++Current;
    if (Current == Insn.FirstOp || Current == End)
      return;
    Load = &Block.Ops[Current - 1];
    // The PE32 lifter loads the memory operand into a temporary, then copies
    // it into ESP. Bind that exact same-instruction producer; an equal frame
    // value in an unrelated temporary cannot authenticate the source restore.
    if (Load->Addr != Op.Addr || Load->Seq != Op.Seq - 1 ||
        !Load->Output.isTemp() || Load->Output.Size != 4 ||
        Load->Output.Offset != Op.Inputs[0].Offset)
      return;
  }
  if (Load->Opcode != NdOp::LOAD || Load->NumInputs != 1 ||
      Load->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Load->MemoryOrdering != NdMemoryOrdering::None ||
      Transfer.read(Load->Inputs[0]).Offset != *Chain.RegistrationOffset - 8)
    return;
  After.StackRestore =
      Domain::ParentStackRestore{Op.Addr, Op.Seq, *Value.Offset};
}

bool RegistrationStateSolver::transferSEHExit(size_t I, Domain &After,
                                              const LowOp &Op,
                                              BlockFacts &Facts) {
  if (!SEHControls || KnownCxx || !After.Callback || After.Parent ||
      !After.Activation.Entry || After.Activation.SearchFilter ||
      !FinallyTargets.count(*After.Activation.Entry) ||
      (Op.Opcode != NdOp::RETURN && Op.Opcode != NdOp::BRANCH))
    return false;
  const bool OrdinaryReturn =
      Op.Opcode == NdOp::RETURN && After.Activation.OrdinaryCall;
  const bool ParentJump = Op.Opcode == NdOp::BRANCH && After.StackRestore;
  if (!OrdinaryReturn && !ParentJump)
    return false;
  const auto Identity = SEHExitKey{
      *After.Activation.Entry, After.Activation.OrdinaryCall, Op.Addr, Op.Seq};
  const auto Refuse = [&] {
    SEHExits.erase(Identity);
    InvalidSEHExits.insert(Identity);
    Result.SEHInvocationExitsComplete = false;
    Facts.Invalid = After.Unknown = true;
    After.Levels.clear();
    return true;
  };
  const auto &Block = Function.Blocks[I];
  const auto Source = Boundaries.find(Op.Addr);
  if (After.Unknown || Facts.Invalid || Chain.RealignedFrame ||
      !After.Installed || After.Uninstalled || Op.Seq < 0 ||
      After.Frame.CallbackEntry != After.Activation.Entry ||
      After.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset !=
          0 ||
      Source == Boundaries.end() || Source->second.first != Block.Id ||
      Block.Ops.empty() || &Op != &Block.Ops.back() ||
      Source->second.second.Address + Source->second.second.Size !=
          Block.EndAddr ||
      Source->second.second.Mode != InstructionMode::Default ||
      Source->second.second.TargetMode != LowInstructionTargetMode::Preserve ||
      hasLowInstructionControlFlag(Source->second.second.ControlFlags,
                                   LowInstructionControlFlag::Conditional) ||
      hasLowInstructionControlFlag(Source->second.second.ControlFlags,
                                   LowInstructionControlFlag::InstructionGuard))
    return Refuse();
  RegistrationSEHInvocationExit Exit;
  Exit.EntryVA = *After.Activation.Entry;
  Exit.CallVA = After.Activation.OrdinaryCall;
  Exit.Address = Op.Addr;
  Exit.EndAddress = Block.EndAddr;
  Exit.OpSeq = Op.Seq;
  if (OrdinaryReturn && !After.SEHCallers.empty())
    return returnNestedFinally(After, Op, Facts, Exit);
  if (OrdinaryReturn) {
    const auto Control = SEHControlIndex.find(Op.Addr);
    const auto Call = SEHControlIndex.find(Exit.CallVA);
    if (!After.OrdinaryCallStack || !callbackCanReturn(After) ||
        !callbackReturnInstruction(I, Op) || Control == SEHControlIndex.end() ||
        Control->second.Control !=
            RegistrationSEHControlContract::Kind::NearReturn ||
        Control->second.EndAddress != Block.EndAddr ||
        Call == SEHControlIndex.end())
      return Refuse();
    const auto Resume = sehCallContinuation(Exit.CallVA);
    if (!Resume)
      return Refuse();
    Exit.TargetVA = *Resume;
    Exit.StackOffset = *After.OrdinaryCallStack;
  } else {
    const auto &SP =
        After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
    if (Op.NumInputs != 1 || !isPE32CodeTarget(Op.Inputs[0]) ||
        Source->second.second.Control != LowInstructionControl::Branch ||
        Source->second.second.Immediate != Op.Inputs[0].Offset || !SP.Offset ||
        SP.Offset != After.StackRestore->StackOffset ||
        After.Frame.load(*Chain.RegistrationOffset - 8, 4).Offset !=
            SP.Offset ||
        Block.Succs.size() != 1 || !Index.count(Block.Succs.front()) ||
        Function.Blocks[Index.at(Block.Succs.front())].StartAddr !=
            Op.Inputs[0].Offset ||
        !EH.CodeRange.contains(Op.Inputs[0].Offset) ||
        (!SEHParentEntries.count(Op.Inputs[0].Offset) &&
         !SEHParentContinuations.count(Op.Inputs[0].Offset)) ||
        FinallyTargets.count(Op.Inputs[0].Offset))
      return Refuse();
    for (const auto &Scope : Chain.Scopes)
      if (Scope.FilterVA == Op.Inputs[0].Offset ||
          Scope.HandlerVA == Op.Inputs[0].Offset)
        return Refuse();
    Exit.Exit = RegistrationSEHInvocationExit::Kind::ParentJump;
    Exit.TargetVA = Op.Inputs[0].Offset;
    Exit.StackOffset = *SP.Offset;
    Exit.StackRestoreVA = After.StackRestore->Address;
    Exit.StackRestoreSeq = After.StackRestore->Seq;
  }
  if (!Entries.count(Exit.TargetVA) || InvalidSEHExits.count(Identity) ||
      !charge(After.Frame.cellCount() + 10))
    return Refuse();
  auto [It, New] = SEHExits.emplace(Identity, Exit);
  if (!New && It->second != Exit)
    return Refuse();
  if (!OrdinaryReturn) {
    After.AbandonedSEHGuard |=
        !After.Activation.OrdinaryCall ||
        llvm::any_of(After.SEHCallers, [](const auto &Caller) {
          return !Caller.Activation.OrdinaryCall;
        });
    After.SEHCallers.clear();
  }
  After.Frame.leaveCallback();
  After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::frame(Exit.StackOffset);
  After.Activation = {};
  After.OrdinaryCallStack.reset();
  After.StackRestore.reset();
  After.Parent = true;
  After.Callback = After.OtherCallback = false;
  if (OrdinaryReturn) {
    merge(Entries.at(Exit.TargetVA), After);
    Facts.InvocationAtExit = true;
  }
  return true;
}

} // namespace neverd::registration_state
