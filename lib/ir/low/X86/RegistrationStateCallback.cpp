//===- RegistrationStateCallback.cpp - x86 callback return contracts ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

std::optional<int32_t>
RegistrationStateSolver::parentStackOffset(const Domain &State) const {
  const auto &SP =
      State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  if (SP.Offset)
    return SP.Offset;
  if (!Chain.hasCxxCallbackStack() || !SP.CallbackAddress ||
      SP.CallbackAddress->Entry != State.Frame.CallbackEntry ||
      SP.CallbackAddress->Offset > 0 ||
      SP.CallbackAddress->Offset <
          -int64_t(limits::kMaxRegistrationEHStateWork) ||
      State.Parent || !State.Callback || State.OtherCallback ||
      State.CxxCatchStacks.size() != 1 || State.CxxCatchStacks.begin()->empty())
    return std::nullopt;
  // Callback allocation cannot release parent locals. SavedESP may already
  // name this private stack; only the CRT's pre-dispatch snapshot bounds the
  // still-live parent allocation used by a checked object borrow.
  return State.CxxCatchStacks.begin()->front().SavedStackOffset;
}

bool RegistrationStateSolver::callbackCanReturn(const Domain &State) const {
  if (!Chain.hasCxxCallbackStack())
    return true;
  if (State.CxxCatchStacks.size() != 1 || State.CxxCatchStacks.begin()->empty())
    return false;
  const auto &Context = State.CxxCatchStacks.begin()->back();
  const auto Entry = EH.Cxx->TryBlocks[Context.TryIndex]
                         .Handlers[Context.CatchIndex]
                         .HandlerVA;
  const auto &Frame = State.Frame;
  const auto &SP =
      Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].CallbackAddress;
  return Frame.CallbackEntry == Entry && SP && SP->Entry == Entry &&
         SP->Offset == 0 &&
         Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset ==
             Chain.cxxRuntimeFrameOffset() &&
         (!Chain.RealignedFrame ||
          Frame.load(Chain.RealignedFrame->SavedParentFrameOffset, 4)
                  .EntryOffset == 0);
}

} // namespace neverd::registration_state
