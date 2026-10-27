//===- RegistrationStateHeap.cpp - Exact PE32 heap policy invocation ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind only the process-wide terminate-on-corruption request with no borrow.
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::registration_state {

std::optional<FrameValue> RegistrationStateSolver::transferHeapTermination(
    const Domain &State, const LowOp &Op, const BlockFacts &Facts) {
  if (!JumpRuntime || !JumpRuntime->HeapTermination || Conditional ||
      KnownCxx || Op.Opcode != NdOp::INDIR_CALL || State.Unknown ||
      Facts.Invalid || !State.Parent || State.Callback || !State.Installed ||
      State.Uninstalled || !State.JumpBindingsIntact ||
      !State.Frame.ImportBindingsIntact || Chain.RealignedFrame ||
      (EH4 && (!CompleteCookies || !cookiesReady(State.Frame))))
    return std::nullopt;
  const auto SP = State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  const auto &Arguments = RegistrationSEHBoundCall::HeapTerminationArguments;
  constexpr uint32_t Bytes =
      RegistrationSEHBoundCall::HeapTerminationArguments.size() * 4;
  if (!SP.Offset || int64_t(*SP.Offset) + Bytes > 0 ||
      !frameObjectIsDisjointFromAdministration(*SP.Offset,
                                               int64_t(*SP.Offset) + Bytes) ||
      !charge(State.Frame.cellCount() + Arguments.size()) ||
      !JumpRuntime->HeapTermination(Op, WorkUsed))
    return std::nullopt;
  for (size_t I = 0; I != Arguments.size(); ++I)
    if (State.Frame.load(*SP.Offset + int32_t(I * 4), 4) !=
        FrameValue::constant(Arguments[I]))
      return std::nullopt;
  // This mode changes a process heap policy and takes no caller storage. It
  // supplies only a normal-return frame/import fact, never an exact BOOL,
  // no-unwind, no-callback, purity or unconditional-return guarantee.
  recordSEHRuntimeCall(State, Op,
                       RegistrationSEHBoundCall::Kind::HeapTermination);
  return FrameValue::frame(int32_t(int64_t(*SP.Offset) + Bytes));
}

} // namespace neverd::registration_state
