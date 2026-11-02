//===- RegistrationJumpContinuation.cpp - Restored nonlocal frame --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Restores only paths which completed local unwind and resume their capture.

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86/X86Regs.h"

#include <iterator>
#include <utility>

namespace neverd::registration_state {

bool RegistrationStateSolver::mergeSEHJumpContinuation(
    const Domain &Source, const SEHJumpCapture &Saved, const FrameValue &Value,
    SEHFrameOutcomes Outcomes) {
  std::optional<FrameState> Joined;
  std::set<int32_t> Stopped;
  for (auto &[Level, Frame] : Outcomes) {
    Stopped.insert(Level);
    if (!Joined)
      Joined = std::move(Frame);
    else if (!charge(Joined->cellCount() + Frame.cellCount() + 10))
      return false;
    else
      Joined->merge(Frame);
  }
  if (!Joined)
    return false;
  Domain Resume = Source;
  Resume.Frame = std::move(*Joined);
  Resume.Frame.leaveCallback();
  Resume.Levels = std::move(Stopped);
  Resume.Activation = {};
  Resume.Parent = true;
  Resume.Callback = Resume.OtherCallback = false;
  Resume.CanDispatch = true;
  Resume.OrdinaryCallStack.reset();
  Resume.StackRestore.reset();
  Resume.SEHCallers.clear();
  constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                    x86reg::RSI};
  for (unsigned R = 0; R != std::size(Registers); ++R)
    Resume.Frame.Registers[Registers[R] / x86reg::GeneralRegStride] =
        Saved.Nonvolatile[R];
  Resume.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::frame(Saved.StackOffset);
  Resume.Frame.Registers[x86reg::RAX / x86reg::GeneralRegStride] =
      Value.Constant == 0 ? FrameValue::constant(1) : Value;
  for (uint64_t R : {x86reg::RCX, x86reg::RDX})
    Resume.Frame.Registers[R / x86reg::GeneralRegStride] = {
        {}, {}, false, true, true};
  Resume.Frame.OtherRegisterBytes.clear();
  Resume.Frame.OtherRegistersMayBeFrame = true;
  merge(Entries.at(Saved.ReturnPC), Resume);
  return !Exhausted;
}

} // namespace neverd::registration_state
