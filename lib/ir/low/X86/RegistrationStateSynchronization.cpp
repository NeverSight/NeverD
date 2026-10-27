//===- RegistrationStateSynchronization.cpp - Bound PE32 runtime words ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind atomic image storage and opaque scalar inputs at their actual calls.
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"
#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

#include "llvm/ADT/ScopeExit.h"

#include <cstdint>
#include <optional>

namespace neverd::registration_state {

std::optional<FrameValue> RegistrationStateSolver::transferSynchronization(
    const Domain &State, const LowOp &Op, const BlockFacts &Facts,
    std::optional<FrameValue> &CallResult) {
  if (!TrackRuntimeArguments || !JumpRuntime || Op.Opcode != NdOp::INDIR_CALL)
    return std::nullopt;
  const bool Atomic =
      JumpRuntime->Interlocked && JumpRuntime->Interlocked(Op, WorkUsed);
  if (!Atomic && (!JumpRuntime->Sleep || !JumpRuntime->Sleep(Op, WorkUsed)))
    return std::nullopt;
  if (!charge(1))
    return std::nullopt;
  bool Accepted = false;
  auto Revoke = llvm::scope_exit([&] {
    // A receipt from an earlier path cannot conceal a later uninitialized or
    // differently borrowed invocation at the same physical CALL.
    if (!Accepted)
      InvalidRuntimeBindings.emplace(Op.Addr, Op.Seq);
  });
  if (Conditional || KnownCxx || State.Unknown || Facts.Invalid ||
      !State.Parent || State.Callback || !State.Installed ||
      State.Uninstalled || !State.JumpBindingsIntact ||
      !State.Frame.ImportBindingsIntact || Chain.RealignedFrame ||
      !State.JumpCaptures.empty() ||
      (EH4 && (!CompleteCookies || !cookiesReady(State.Frame))))
    return std::nullopt;
  const auto SP = State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  const uint32_t Count = Atomic ? 3 : 1;
  if (!SP.Offset || int64_t(*SP.Offset) + Count * 4 > 0 ||
      !frameObjectIsDisjointFromAdministration(*SP.Offset,
                                               int64_t(*SP.Offset) + Count * 4))
    return std::nullopt;
  va_t Destination = 0;
  for (uint32_t I = 0; I != Count; ++I) {
    const auto Value = registration_abi::readCallerFrameWord(
        State.Frame, *SP.Offset + int32_t(I * 4), &State.InitializedFrameBytes,
        WorkUsed);
    if (!Value)
      return std::nullopt;
    if (!Atomic || I) {
      if (!Value->isFrameIndependentScalar())
        return std::nullopt;
      continue;
    }
    if (!Value->ImageAddress ||
        *Value != FrameValue::imageAddress(*Value->ImageAddress))
      return std::nullopt;
    Destination = *Value->ImageAddress;
  }
  if (Atomic) {
    const auto Overlaps = [&](va_t Begin, uint64_t Bytes) {
      return Bytes && (Begin <= Destination ? Destination - Begin < Bytes
                                            : Begin - Destination < 4);
    };
    if (!Destination || Destination % 4 ||
        (SecurityCookieVA && Overlaps(SecurityCookieVA, 4)) ||
        Overlaps(Chain.ScopeTableVA,
                 (EH4 ? 16u : 0u) + uint64_t(Chain.Scopes.size()) * 12) ||
        !JumpRuntime->ImageMemory ||
        !JumpRuntime->ImageMemory(Destination, 4, true, WorkUsed) ||
        Op.Output.Size != 4 ||
        (!Op.Output.isTemp() &&
         (!Op.Output.isReg() || Op.Output.Offset != x86reg::RAX)))
      return std::nullopt;
    ImageReads.emplace(Destination, Destination + 4);
    // The old image word remains a real, opaque EAX result. Its possible
    // address bits grant no frame coordinate or dereference authority.
    CallResult = FrameValue::taint(true, true);
  }
  recordSEHRuntimeCall(
      State, Op,
      Atomic ? RegistrationSEHBoundCall::Kind::InterlockedCompareExchange
             : RegistrationSEHBoundCall::Kind::Sleep,
      std::nullopt, Destination);
  Accepted = true;
  return FrameValue::frame(int32_t(int64_t(*SP.Offset) + Count * 4));
}

} // namespace neverd::registration_state
