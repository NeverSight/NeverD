//===- RegistrationFrameCallback.cpp - x86 callback stack coordinates -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationFrame.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>

namespace neverd::registration_state {

void FrameState::enterCallback(va_t Entry) {
  leaveCallback();
  CallbackEntry = Entry;
  Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::callbackFrame(Entry, 0);
}

void FrameState::leaveCallback() {
  auto Forget = [](FrameValue &Value) {
    if (Value.CallbackAddress || Value.SEHException)
      Value = join(Value, {});
  };
  for (auto &Value : Registers)
    Forget(Value);
  for (auto *Space : {&Cells, &EntryCells})
    if (std::any_of(Space->begin(), Space->end(), [](const auto &Cell) {
          return Cell.second.CallbackAddress || Cell.second.SEHException;
        }))
      Space->edit([&](FrameCells::Map &Cells) {
        for (auto &[Offset, Value] : Cells)
          Forget(Value);
      });
  for (auto &[Offset, Value] : OtherRegisterBytes)
    Forget(Value);
  CallbackCells.clear();
  InitializedCallbackBytes.clear();
  CallbackEntry.reset();
  SEHFilterEntry.reset();
  SEHOrigin.reset();
}

bool FrameState::callbackMemoryIsPrivate(const CallbackFrameAddress &Address,
                                         uint16_t Width, bool Read) const {
  const auto &SP =
      Registers[x86reg::RSP / x86reg::GeneralRegStride].CallbackAddress;
  // The runtime return PC and caller storage start at offset zero. Only an
  // explicitly allocated interval below that boundary belongs to this call.
  if (CallbackEntry != Address.Entry || !SP || SP->Entry != Address.Entry ||
      SP->Offset < -int64_t(limits::kMaxSourceCallBorrowedBytes) || !Width ||
      Address.Offset < SP->Offset || int64_t(Address.Offset) + Width > 0)
    return false;
  if (Read)
    for (int64_t Byte = Address.Offset; Byte < int64_t(Address.Offset) + Width;
         ++Byte)
      if (!InitializedCallbackBytes.count(int32_t(Byte)))
        return false;
  return true;
}

bool FrameState::callbackStackIsRestored(int32_t ParentFrameOffset) const {
  const auto &SP =
      Registers[x86reg::RSP / x86reg::GeneralRegStride].CallbackAddress;
  return CallbackEntry && SP && SP->Entry == *CallbackEntry &&
         SP->Offset == 0 &&
         Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset ==
             ParentFrameOffset;
}

void FrameState::trimCallbackCells() {
  const auto &SP =
      Registers[x86reg::RSP / x86reg::GeneralRegStride].CallbackAddress;
  if (!SP || SP->Entry != CallbackEntry) {
    InitializedCallbackBytes.clear();
    if (std::any_of(CallbackCells.begin(), CallbackCells.end(),
                    [](const auto &Cell) {
                      return join(Cell.second, {}) != Cell.second;
                    }))
      CallbackCells.edit([](FrameCells::Map &Cells) {
        for (auto &[Offset, Value] : Cells)
          Value = join(Value, {});
      });
    return;
  }
  // Deallocated bytes lose initialization. A partial pop also loses the
  // complete word's identity, while its surviving pointer bits remain tainted.
  discardFrameCellsBelow(CallbackCells, SP->Offset);
  InitializedCallbackBytes.erase(
      InitializedCallbackBytes.begin(),
      InitializedCallbackBytes.lower_bound(SP->Offset));
}

} // namespace neverd::registration_state
