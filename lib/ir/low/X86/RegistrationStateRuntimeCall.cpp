//===- RegistrationStateRuntimeCall.cpp - Native SEH runtime arguments ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Publish native projections only after the runtime's source effect proof.

#include "RegistrationStateSolver.h"

#include "neverd/ir/RegistrationCRTControl.h"
#include "neverd/lift/X86Regs.h"

#include <cstddef>
#include <utility>

namespace neverd::registration_state {

void RegistrationStateSolver::recordSEHRuntimeCall(
    const Domain &State, const LowOp &Op, RegistrationSEHBoundCall::Kind Kind,
    std::optional<int32_t> Output, va_t ImageOutput) {
  if (Conditional || KnownCxx || State.Unknown || !State.Installed ||
      (EH4 &&
       (!CompleteCookies ||
        (Kind != RegistrationSEHBoundCall::Kind::RaiseException &&
         Kind != RegistrationSEHBoundCall::Kind::GetStartupInfoW &&
         Kind != RegistrationSEHBoundCall::Kind::HeapTermination &&
         Kind != RegistrationSEHBoundCall::Kind::InterlockedCompareExchange &&
         Kind != RegistrationSEHBoundCall::Kind::Sleep))) ||
      State.Uninstalled)
    return;
  const auto &SP =
      State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  if (!charge(5))
    return;
  RegistrationSEHBoundCall Call;
  if (State.Parent && !State.Callback && SP.Offset)
    Call.StackOffset = *SP.Offset;
  else if (!State.Parent && State.Callback && State.Activation.Entry &&
           !State.Activation.SearchFilter &&
           Kind == RegistrationSEHBoundCall::Kind::RaiseException &&
           SP.CallbackAddress &&
           SP.CallbackAddress->Entry == *State.Activation.Entry &&
           State.Frame.callbackMemoryIsPrivate(*SP.CallbackAddress, 16, true)) {
    Call.EntryVA = *State.Activation.Entry;
    Call.StackOffset = SP.CallbackAddress->Offset;
  } else
    return;
  Call.CalleeKind = Kind;
  Call.Address = Op.Addr;
  Call.OpSeq = Op.Seq;
  Call.TargetVA = Op.Inputs[0].Offset;
  using Argument = RegistrationSEHBoundArgument::Kind;
  const unsigned Count =
      Kind == RegistrationSEHBoundCall::Kind::GetStartupInfoW ||
              Kind == RegistrationSEHBoundCall::Kind::Sleep
          ? 1
      : Kind == RegistrationSEHBoundCall::Kind::ControlFP ||
              Kind == RegistrationSEHBoundCall::Kind::InterlockedCompareExchange
          ? 3
          : 4;
  for (unsigned I = 0; I != Count; ++I)
    Call.Arguments.push_back(
        {Argument::Scalar, Call.StackOffset + int32_t(I * 4), 0});
  if (Kind == RegistrationSEHBoundCall::Kind::RaiseException)
    Call.Arguments.back() = {Argument::NullPointer, 0, 0};
  else if (Kind == RegistrationSEHBoundCall::Kind::HeapTermination) {
    for (size_t I = 0; I != Call.Arguments.size(); ++I)
      Call.Arguments[I] =
          I == 0 || I == 2
              ? RegistrationSEHBoundArgument{Argument::NullPointer}
              : RegistrationSEHBoundArgument{
                    Argument::ConstantScalar, 0, 0,
                    RegistrationSEHBoundCall::HeapTerminationArguments[I]};
  } else if (Kind ==
                 RegistrationSEHBoundCall::Kind::InterlockedCompareExchange &&
             ImageOutput)
    Call.Arguments.front() = {Argument::ImagePointer, 0, 0, 0, ImageOutput};
  else if (Kind == RegistrationSEHBoundCall::Kind::Sleep) {
    // The duration is sampled at the actual source invocation.
  } else if (Kind == RegistrationSEHBoundCall::Kind::ControlFP) {
    Call.Arguments.front() =
        Output ? RegistrationSEHBoundArgument{Argument::Object, *Output, 0}
               : RegistrationSEHBoundArgument{Argument::NullPointer};
    const auto Mask = State.Frame.load(Call.StackOffset + 8, 4);
    if (!Output &&
        Mask.Constant == registration_crt::kX86ControlFPPrecisionMask) {
      const auto Control = State.Frame.load(Call.StackOffset + 4, 4);
      if (!Control.Constant ||
          !registration_crt::isX86ControlFPPrecisionSetting(*Control.Constant,
                                                            *Mask.Constant))
        return;
      Call.Arguments[1] = {Argument::ConstantScalar, 0, 0, *Control.Constant};
      Call.Arguments[2] = {Argument::ConstantScalar, 0, 0, *Mask.Constant};
    }
  } else if (Kind == RegistrationSEHBoundCall::Kind::GetStartupInfoW && Output)
    Call.Arguments.front() = {Argument::Object, *Output, 0};
  else
    return;
  const auto [It, Added] =
      BoundCalls.emplace(std::make_pair(Op.Addr, Op.Seq), Call);
  if (!Added && It->second != Call)
    Result.SEHBoundCallsComplete = false;
  Result.HasBoundSEHCallees = true;
}

} // namespace neverd::registration_state
