//===- RegistrationStatePrivateCall.cpp - Live capture preservation ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86/X86Regs.h"

namespace neverd::registration_state {

std::optional<FrameValue>
RegistrationStateSolver::transferPrivateSEHCall(Domain &State, const LowOp &Op,
                                                const BlockFacts &Facts) {
  if (!JumpRuntime || !JumpRuntime->PrivateCall || KnownCxx ||
      (EH4 && (!CompleteCookies || !cookiesReady(State.Frame))) ||
      Conditional || CheckCalls || State.Unknown || Facts.Invalid ||
      Facts.InvocationAtExit || !State.Parent || State.Callback ||
      State.Frame.CallbackEntry || !State.Installed || State.Uninstalled ||
      !State.JumpBindingsIntact || !Function.hasCompleteLiftCoverage() ||
      State.JumpCaptures.empty() || Op.Opcode != NdOp::CALL ||
      Chain.RealignedFrame || Chain.RegistrationOffset != -16 ||
      Chain.TryLevelOffset != -4 ||
      State.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset != 0)
    return std::nullopt;
  const auto SP = parentStackOffset(State);
  if (!SP || *SP > *Chain.RegistrationOffset - 8 ||
      !charge(State.JumpCaptures.size()))
    return std::nullopt;
  for (const auto &[Offset, Capture] : State.JumpCaptures)
    if (Offset < *SP)
      return std::nullopt;
  if (!JumpRuntime->PrivateCall(Op, WorkUsed) ||
      !charge(State.Frame.cellCount() + State.InitializedFrameBytes.size()))
    return std::nullopt;
  // The callee owns memory below entry SP, including its physical return
  // address. No parent cell or capture below this boundary is preserved.
  // FrameTransfer still invalidates volatile registers and the call result.
  State.Frame.forgetCellValues(INT32_MIN, uint32_t(int64_t(*SP) - INT32_MIN));
  State.InitializedFrameBytes.erase(
      State.InitializedFrameBytes.begin(),
      State.InitializedFrameBytes.lower_bound(*SP));
  return FrameValue::frame(*SP);
}

} // namespace neverd::registration_state
