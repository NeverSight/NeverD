//===- RegistrationStateCRTCall.cpp - Invocation-bound CRT output words ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Transfers authenticated CRT effects through PE32 registration states.

#include "RegistrationStateSolver.h"

#include "neverd/ir/RegistrationCRTControl.h"
#include "neverd/lift/X86/X86Regs.h"

namespace neverd::registration_state {

std::optional<FrameValue> RegistrationStateSolver::transferControlFP(
    const Domain &Before, Domain &After, const LowOp &Op,
    const BlockFacts &Facts, std::optional<Domain> &CallException) {
  if (!JumpRuntime || !JumpRuntime->ControlFP || KnownCxx || EH4 ||
      After.Unknown || Facts.Invalid || !After.Parent || After.Callback ||
      !After.Installed || After.Uninstalled || !After.JumpBindingsIntact)
    return std::nullopt;
  const auto SP = After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  if (!SP.Offset || *SP.Offset > int64_t(*Chain.RegistrationOffset) - 20)
    return std::nullopt;
  const auto Output = After.Frame.load(*SP.Offset, 4);
  const auto Control = After.Frame.load(*SP.Offset + 4, 4);
  const auto Mask = After.Frame.load(*SP.Offset + 8, 4);
  const bool NullOutput = Output == FrameValue::constant(0);
  // Exception masks cannot invoke a parameter-error hook. Null-output calls
  // additionally admit a complete, valid PE32 precision setting. Keep its
  // actual scalar arguments in the native recipe and retain the FP effects.
  if (Control.MayBeFrame || Mask.MayBeFrame || !Mask.Constant ||
      ((*Mask.Constant & ~registration_crt::kControlFPExceptionMask) &&
       !(NullOutput && Control.Constant &&
         registration_crt::isX86ControlFPPrecisionSetting(*Control.Constant,
                                                          *Mask.Constant))) ||
      (!NullOutput &&
       (!Output.Offset || Output.ReturnPC || Output.CallbackAddress ||
        Output.EntryOffset || Output.SEHException ||
        *Output.Offset < int64_t(*SP.Offset) + 12 ||
        !frameObjectIsDisjointFromAdministration(*Output.Offset,
                                                 int64_t(*Output.Offset) + 4) ||
        *Output.Offset > -4)) ||
      !JumpRuntime->ControlFP(Op, WorkUsed) ||
      !charge(Before.Frame.cellCount() + After.Frame.cellCount() + 4))
    return std::nullopt;
  if (!CallException)
    CallException = Before;
  recordSEHRuntimeCall(After, Op, RegistrationSEHBoundCall::Kind::ControlFP,
                       Output.Offset);
  if (!NullOutput) {
    invalidateJumpCaptures(After, Output, 4);
    invalidateJumpCaptures(*CallException, Output, 4);
    After.Frame.store(*Output.Offset, 4, {});
    CallException->Frame.store(*Output.Offset, 4, {});
  }
  Result.HasBoundSEHCallees = true;
  // The original CALL remains in LowIR, including its floating-point state
  // effects. This returning memory fact is distinct from native admission.
  return SP;
}

} // namespace neverd::registration_state
