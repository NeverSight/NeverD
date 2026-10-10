//===- RegistrationStateInstallation.cpp - x86 node publication ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::registrationInstallationReady(
    const FrameState &Frame) const {
  if (!Chain.RealignedFrame && Chain.hasCxxCallbackStack()) {
    const auto SP =
        Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
    return SP && *SP <= *Chain.RegistrationOffset - 4 &&
           Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset ==
               0 &&
           Frame.load(*Chain.RegistrationOffset - 4, 4).Offset == SP &&
           Frame.load(*Chain.RegistrationOffset, 4).PreviousChain &&
           Frame.load(*Chain.RegistrationOffset + 4, 4).Constant ==
               Chain.HandlerVA &&
           Frame.load(*Chain.TryLevelOffset, 4).Constant ==
               uint32_t(*Chain.SeededTryLevel);
  }
  if (!Chain.RealignedFrame)
    return true;
  const auto &Layout = *Chain.RealignedFrame;
  const auto Parent = Frame.load(Layout.SavedParentFrameOffset, 4);
  const auto Stack = Frame.load(*Chain.RegistrationOffset - 4, 4);
  return Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].EntryOffset ==
             0 &&
         Frame.Registers[x86reg::RSI / x86reg::GeneralRegStride].Offset ==
             Layout.BaseOffset &&
         Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset ==
             Layout.BaseOffset &&
         Parent.EntryOffset == 0 && Stack.Offset == Layout.BaseOffset &&
         Frame.load(*Chain.RegistrationOffset, 4).PreviousChain &&
         Frame.load(*Chain.RegistrationOffset + 4, 4).Constant ==
             Chain.HandlerVA &&
         Frame.load(*Chain.TryLevelOffset, 4).Constant ==
             uint32_t(*Chain.SeededTryLevel);
}

} // namespace neverd::registration_state
