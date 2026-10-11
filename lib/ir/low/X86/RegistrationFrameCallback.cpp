//===- RegistrationFrameCallback.cpp - x86 callback stack coordinates -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationFrame.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

void FrameState::enterCallback(va_t Entry) {
  leaveCallback();
  CallbackEntry = Entry;
  Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::callbackFrame(Entry, 0);
}

void FrameState::leaveCallback() {
  auto Forget = [](FrameValue &Value) {
    if (Value.CallbackAddress)
      Value = join(Value, {});
  };
  for (auto &Value : Registers)
    Forget(Value);
  for (auto *Space : {&Cells, &EntryCells})
    for (auto &[Offset, Value] : *Space)
      Forget(Value);
  for (auto &[Offset, Value] : OtherRegisterBytes)
    Forget(Value);
  CallbackCells.clear();
  InitializedCallbackBytes.clear();
  CallbackEntry.reset();
}

bool FrameState::callbackMemoryIsPrivate(const CallbackFrameAddress &Address,
                                         uint16_t Width, bool Read) const {
  const auto &SP =
      Registers[x86reg::RSP / x86reg::GeneralRegStride].CallbackAddress;
  // The runtime return PC and caller storage start at offset zero. Only an
  // explicitly allocated interval below that boundary belongs to this call.
  if (CallbackEntry != Address.Entry || !SP || SP->Entry != Address.Entry ||
      SP->Offset < -int64_t(limits::kMaxRegistrationEHStateWork) || !Width ||
      Address.Offset < SP->Offset || int64_t(Address.Offset) + Width > 0)
    return false;
  if (Read)
    for (int64_t Byte = Address.Offset; Byte < int64_t(Address.Offset) + Width;
         ++Byte)
      if (!InitializedCallbackBytes.count(int32_t(Byte)))
        return false;
  return true;
}

void FrameState::trimCallbackCells() {
  const auto &SP =
      Registers[x86reg::RSP / x86reg::GeneralRegStride].CallbackAddress;
  if (!SP || SP->Entry != CallbackEntry) {
    InitializedCallbackBytes.clear();
    for (auto &[Offset, Value] : CallbackCells)
      Value = join(Value, {});
    return;
  }
  // Dead stack slots cannot prove a later push/load after the runtime or a
  // callee has reused them. A partial pop invalidates the overlapping cell.
  CallbackCells.erase(CallbackCells.begin(),
                      CallbackCells.lower_bound(SP->Offset));
  InitializedCallbackBytes.erase(
      InitializedCallbackBytes.begin(),
      InitializedCallbackBytes.lower_bound(SP->Offset));
}

} // namespace neverd::registration_state
