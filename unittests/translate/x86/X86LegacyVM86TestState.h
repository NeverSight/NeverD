//===- X86LegacyVM86TestState.h - VM86 execution fixtures -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent VM86 state and protected return frames for mode regressions.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYVM86TESTSTATE_H
#define NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYVM86TESTSTATE_H

#include "X86LegacyTaskTestState.h"

#include <array>
#include <cstdint>

namespace neverd::translate {

class X86LegacyVM86TestState : public X86LegacyTaskTestState {
protected:
  void vm86State(uint32_t Flags = 0x23202) {
    resetLegacyState();
    O.Virtual8086 = X86LegacyVM86Profile::IntelVME;
    S.EFlags = Flags;
    S.EIP = 0x100;
    S.Core.GPR[4] = 0xabcd4000;
    const uint16_t Selectors[]{0x300, 0x100, 0x800, 0x400, 0x500, 0x600};
    for (unsigned I = 0; I != 6; ++I)
      S.Core.Segments[I] = {uint32_t(Selectors[I]) * 16, 65535, Selectors[I],
                            0xf3, false};
    O.FarTransfers = X86LegacyFarTransferProfile::OrderedFrame;
  }

  void vm86ReturnFrame(uint32_t Flags = 0x23202) {
    resetLegacyState();
    O.Virtual8086 = X86LegacyVM86Profile::IntelVME;
    S.Core.CPL = 0;
    for (unsigned I = 0; I != 6; ++I)
      S.Core.Segments[I] = {0, UINT32_MAX, uint16_t(I == 1 ? 8 : 16),
                            uint16_t(I == 1 ? 0xc09b : 0xc093), false};
    S.EFlags = 2;
    const uint32_t Slots[]{0x100, 0x100, Flags, 0xabcd4000, 0x800,
                           0x300, 0x400, 0x500, 0x600};
    for (unsigned I = 0; I != 9; ++I)
      put(S.Core.GPR[4] + I * 4, Slots[I], GuestScalarWidth::I32);
  }

  X86LegacyExecutionResult vmRun(llvm::ArrayRef<uint8_t> Bytes) {
    for (unsigned I = 0; I != Bytes.size(); ++I)
      put(S.Core.Segments[1].Base + S.EIP + I, Bytes[I], GuestScalarWidth::I8);
    return llvm::cantFail(executeX86LegacyInstruction(S, *M, Bytes, O));
  }

  void vme(bool Enabled = true) {
    auto C = M->controls();
    C.CR4 = Enabled ? 1 : 2;
    llvm::cantFail(M->bindControls(C));
  }
};

} // namespace neverd::translate

#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYVM86TESTSTATE_H
