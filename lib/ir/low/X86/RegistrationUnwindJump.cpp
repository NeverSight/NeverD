//===- RegistrationUnwindJump.cpp - Repeated nonlocal callback jumps -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Separates nonlocal callback exits from ordinary returns and selections.

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86/X86Regs.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::recordSEHUnwindJump(size_t I, const LowOp &Op,
                                                  const FrameState &State,
                                                  va_t Callback,
                                                  SEHUnwindJump &Jump) {
  const auto SP =
      State.Registers[x86reg::RSP / x86reg::GeneralRegStride].CallbackAddress;
  if (!JumpRuntime || !JumpRuntime->Resume ||
      !JumpRuntime->Resume(Op, WorkUsed) || !validateSEHInvocationBlock(I) ||
      !SP || SP->Entry != Callback || SP->Offset > -8 ||
      !State.callbackMemoryIsPrivate(*SP, 8, true))
    return false;
  const auto Buffer = State.loadCallback(SP->Offset, 4).Offset;
  const auto Capture =
      Buffer ? Jump.Captures.find(*Buffer) : Jump.Captures.end();
  if (Capture == Jump.Captures.end() ||
      !hasSEHJumpCapture(State, Capture->second))
    return false;
  const auto Value = State.loadCallback(SP->Offset + 4, 4);
  if (!Value.isFrameIndependentScalar())
    return false;
  const SEHUnwindJumpValue Transfer{
      *Buffer, Value, {Op.Addr, Op.Seq, Callback, Capture->second.Call}};
  return mergeSEHFrameOutcome(Jump.Resumed[Transfer], State);
}

} // namespace neverd::registration_state
