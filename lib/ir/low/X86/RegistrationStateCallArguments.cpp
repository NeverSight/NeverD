//===- RegistrationStateCallArguments.cpp - PE32 scalar call words --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"

namespace neverd::registration_state {
bool RegistrationStateSolver::transferScalarArguments(
    Domain &After, const RegistrationCalleeFrameContract &Callee,
    FrameValue &StackPointer, std::optional<int32_t> Object) {
  if (Callee.StackPopBytes > UINT16_MAX || Callee.StackPopBytes % 4 ||
      (!Callee.isInitializer() &&
       Callee.StackPopBytes / 4 >= limits::kMaxCallArgs))
    return false;
  const auto PointerFree = [&](const auto &Cells, int32_t Begin, int64_t End) {
    for (const auto &[Offset, Value] : Cells)
      if (Value.MayBeFrame && int64_t(Offset) < End &&
          Begin < int64_t(Offset) + 4)
        return false;
    return true;
  };
  if (StackPointer.CallbackAddress) {
    const auto Stack = *StackPointer.CallbackAddress;
    const int64_t End = int64_t(Stack.Offset) + Callee.StackPopBytes;
    if (!KnownCxx || After.Parent || !After.Callback || After.OtherCallback ||
        After.CxxCatchStacks.size() != 1 ||
        After.CxxCatchStacks.begin()->empty() ||
        After.Frame.CallbackEntry != Stack.Entry || End > 0 ||
        !charge(Callee.StackPopBytes + After.Frame.cellCount() +
                After.RuntimeObject.cellCount()))
      return false;
    if (Callee.StackPopBytes &&
        !After.Frame.callbackMemoryIsPrivate(
            Stack, uint16_t(Callee.StackPopBytes), true))
      return false;
    for (auto *Frame : {&After.Frame, &After.RuntimeObject}) {
      if (!PointerFree(Frame->CallbackCells, Stack.Offset, End))
        return false;
    }
    for (auto *Frame : {&After.Frame, &After.RuntimeObject}) {
      discardFrameCellsBelow(Frame->CallbackCells, int32_t(End));
      Frame->InitializedCallbackBytes.erase(
          Frame->InitializedCallbackBytes.begin(),
          Frame->InitializedCallbackBytes.lower_bound(int32_t(End)));
    }
    StackPointer = FrameValue::callbackFrame(Stack.Entry, int32_t(End));
    return true;
  }
  const auto SP = StackPointer.Offset;
  if (!SP || After.Callback || !After.Parent ||
      int64_t(*SP) + Callee.StackPopBytes > INT32_MAX)
    return false;
  const int32_t AfterSP = int32_t(int64_t(*SP) + Callee.StackPopBytes);
  std::vector<RegistrationObjectExtent> Arguments;
  if (Callee.StackPopBytes &&
      !projectFrameObject(After, *SP, SP, {{0, int32_t(Callee.StackPopBytes)}},
                          Arguments, true))
    return false;
  if (!charge(After.RuntimeObject.cellCount()) ||
      !PointerFree(After.RuntimeObject.Cells, *SP, AfterSP))
    return false;
  if (Object)
    for (const auto *Ranges : {&Callee.ECXReads, &Callee.ECXWrites})
      for (const auto &Range : *Ranges)
        if (!charge(1) || int64_t(*Object) + Range.Begin < AfterSP)
          return false;
  if (!charge(After.InitializedFrameBytes.size() + After.Frame.cellCount() +
              After.RuntimeObject.cellCount()))
    return false;
  After.InitializedFrameBytes.erase(
      After.InitializedFrameBytes.begin(),
      After.InitializedFrameBytes.lower_bound(AfterSP));
  for (auto *Frame : {&After.Frame, &After.RuntimeObject})
    discardFrameCellsBelow(Frame->Cells, AfterSP);
  StackPointer = FrameValue::frame(AfterSP);
  return true;
}
} // namespace neverd::registration_state
