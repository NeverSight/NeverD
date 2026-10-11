//===- RegistrationStateResume.cpp - Suspended PE32 catch frames ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Restore the invocation snapshot owned by an exact runtime catch return.
//===----------------------------------------------------------------------===//
#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {
void RegistrationStateSolver::resumeCxxCatch(
    const Domain &After, const RegistrationCxxContinuation &Return) {
  const auto Resume = Entries.find(Return.TargetVA);
  if (Resume == Entries.end() ||
      !charge(After.Frame.cellCount() + After.RuntimeObject.cellCount() +
              After.catchCellCount() + 2))
    return;
  Domain Continued = After;
  auto Stack = *Continued.CxxCatchStacks.begin();
  const auto Context = Stack.back();
  Stack.pop_back();
  Continued.Parent = Stack.empty();
  Continued.Callback = !Stack.empty();
  Continued.OtherCallback = false;
  Continued.CxxCatchStacks.clear();
  if (!Stack.empty())
    Continued.CxxCatchStacks.insert(std::move(Stack));
  const auto Saved = Return.SavedCallbackVA
                         ? FrameValue::callbackFrame(Return.SavedCallbackVA,
                                                     Return.SavedStackOffset)
                         : FrameValue::frame(Return.SavedStackOffset);
  if (Chain.hasCxxCallbackStack()) {
    Continued.Frame.leaveCallback();
    if (Return.SavedCallbackVA) {
      Continued.Frame.CallbackEntry = Context.SuspendedCallback;
      Continued.Frame.CallbackCells = Context.SuspendedCells;
      Continued.Frame.InitializedCallbackBytes =
          Context.SuspendedInitializedBytes;
    }
    // The CRT does not promise the departing callback's register values at
    // the continuation. The source restore block must reload what it uses.
    for (auto &Register : Continued.Frame.Registers)
      Register = {{}, {}, false, true};
    Continued.Frame.OtherRegisterBytes.clear();
    Continued.Frame.OtherRegistersMayBeFrame = true;
    for (auto &Register : Continued.RuntimeObject.Registers)
      Register = registration_state::join(Register, {});
  }
  const int32_t SavedSlot = *Chain.RegistrationOffset - 4;
  Continued.Frame.store(SavedSlot, 4, Saved);
  Continued.RuntimeObject.store(SavedSlot, 4, {});
  Continued.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
      FrameValue::frame(*Chain.cxxRuntimeFrameOffset());
  Continued.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] = Saved;
  if (Return.SavedCallbackVA)
    Continued.Frame.trimCallbackCells();
  merge(Resume->second, Continued);
}
} // namespace neverd::registration_state
