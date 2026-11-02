//===- RegistrationNestedFinallyFrame.cpp - Suspended finally calls ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Replays ordinary finally invocations inside an active unwind callback.

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/lift/X86/X86Regs.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::runSEHNestedFinallyFrame(
    size_t I, const LowOp &Op, FrameState &State, int32_t ParentStackOffset,
    std::vector<RegistrationObjectExtent> *Writes, unsigned Depth,
    SEHUnwindJump *Jump, SEHFrameOutcomes &Outcomes,
    SEHHandlerOutcomes *Selected) {
  const auto SP =
      State.Registers[x86reg::RSP / x86reg::GeneralRegStride].CallbackAddress;
  if (Depth >=
          ::neverd::limits::ir::
              kRegistrationNestedFinallyFrameRegistrationStateSolverRunSEHNestedFinallyFrameDepthLimit ||
      !State.CallbackEntry || State.SEHFilterEntry || !SP ||
      SP->Entry != State.CallbackEntry || SP->Offset > 0 ||
      SP->Offset < -int64_t(limits::kMaxSourceCallBorrowedBytes) ||
      State.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset != 0 ||
      !validateSEHInvocationBlock(I) ||
      !validateSEHCallContinuation(I, Op, *State.CallbackEntry) ||
      !charge(State.cellCount() + State.OtherRegisterBytes.size() + 10))
    return false;

  FrameState Returned = State;
  SEHFrameOutcomes InnerOutcomes;
  // Ordinary continuation requires this invocation's checked near RET.
  // Selection or a repeated jump retires every suspended invocation. Each
  // nested call shares the global work budget and has a bounded call depth.
  if (!runSEHCallbackFrame(Op.Inputs[0].Offset, Returned, true, Writes, Jump,
                           nullptr, Depth + 1, ParentStackOffset,
                           &InnerOutcomes, Selected))
    return false;
  for (auto &[Level, Outcome] : InnerOutcomes) {
    Outcome.leaveCallback();
    Outcome.CallbackEntry = State.CallbackEntry;
    Outcome.CallbackCells = State.CallbackCells;
    Outcome.InitializedCallbackBytes = State.InitializedCallbackBytes;
    Outcome.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
        FrameValue::callbackFrame(SP->Entry, SP->Offset);
    if (!mergeSEHFrameOutcome(Outcomes, std::move(Outcome)))
      return false;
  }
  return true;
}

} // namespace neverd::registration_state
