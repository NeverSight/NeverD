//===- RegistrationStateStartupInfo.cpp - Private PE32 startup output -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bound GetStartupInfoW's possible writes without inventing initialization.

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86/X86Regs.h"

namespace neverd::registration_state {

std::optional<FrameValue> RegistrationStateSolver::transferStartupInfo(
    const Domain &Before, Domain &After, const LowOp &Op,
    const BlockFacts &Facts, std::optional<Domain> &CallException) {
  if (!JumpRuntime || !JumpRuntime->StartupInfo || Conditional || KnownCxx ||
      After.Unknown || Facts.Invalid || !After.Parent || After.Callback ||
      !After.Installed || After.Uninstalled || !After.JumpBindingsIntact ||
      Chain.RealignedFrame ||
      (EH4 && (!CompleteCookies || !cookiesReady(After.Frame))))
    return std::nullopt;
  const auto SP = After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  if (!SP.Offset || *SP.Offset > int64_t(*Chain.RegistrationOffset) - 12)
    return std::nullopt;
  constexpr uint32_t Bytes = RegistrationSEHBoundCall::StartupInfoBytes;
  const auto Output = After.Frame.load(*SP.Offset, 4);
  if (!Output.Offset || Output.ReturnPC || Output.CallbackAddress ||
      Output.EntryOffset || Output.SEHException ||
      *Output.Offset < int64_t(*SP.Offset) + 4 ||
      int64_t(*Output.Offset) + Bytes > 0 ||
      !frameObjectIsDisjointFromAdministration(
          *Output.Offset, int64_t(*Output.Offset) + Bytes) ||
      !JumpRuntime->StartupInfo(Op, WorkUsed) ||
      !charge(Before.Frame.cellCount() + After.Frame.cellCount() + Bytes))
    return std::nullopt;
  if (!CallException)
    CallException = Before;
  recordSEHRuntimeCall(After, Op,
                       RegistrationSEHBoundCall::Kind::GetStartupInfoW,
                       Output.Offset);
  for (auto *State : {&After, &*CallException}) {
    invalidateJumpCaptures(*State, Output, Bytes);
    // Unused standard handles can retain their pre-call contents. Possible
    // writes forget exact values but keep pointer taint and grant no newly
    // initialized bytes, on normal return or partial exceptional execution.
    State->Frame.forgetCellValues(*Output.Offset, Bytes);
  }
  return FrameValue::frame(*SP.Offset + 4);
}

} // namespace neverd::registration_state
