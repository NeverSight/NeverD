//===- X86LegacyTaskTestState.h - Protected task fixtures -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent TSS images and descriptor tables shared by task regressions.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYTASKTESTSTATE_H
#define NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYTASKTESTSTATE_H

#include "X86LegacyTestState.h"

#include <array>
#include <cstdint>

namespace neverd::translate {

class X86LegacyTaskTestState : public X86LegacyTestState {
protected:
  static uint64_t taskDescriptor(uint32_t Base, uint16_t Limit,
                                 uint8_t Attributes) {
    return Limit | (uint64_t(Base & 0xffffff) << 16) |
           (uint64_t(Attributes) << 40) | (uint64_t(Base >> 24) << 56);
  }

  void taskState(bool Wide = true, bool OldWide = true) {
    resetLegacyState();
    O.Tasks = X86LegacyTaskBinding{};
    O.Virtual8086 = X86LegacyVM86Profile::IntelVME;
    O.Tasks->WordGPRHigh = std::array<uint16_t, 8>{};
    O.Debug = X86LegacyDebugProfile::BranchWatchpoints;
    O.FarTransfers = X86LegacyFarTransferProfile::OrderedFrame;
    O.Segments.FullLimitChoice = X86FullSegmentLimitChoice::Fault;
    S.Debug.emplace();
    S.EFlags = 0x202;
    S.TR.Attributes = OldWide ? 0x8b : 0x83;
    S.TR.Limit = OldWide ? 103 : 43;
    put(0x7028, taskDescriptor(S.TR.Base, S.TR.Limit, S.TR.Attributes));
    // CR3 is a static input and must survive the outgoing save.
    put(0x581c, 0x10000, GuestScalarWidth::I32);
    put(0x7020, 0x00cff3000000ffffULL);
    put(0x7030, taskDescriptor(0x8000, Wide ? 103 : 43, Wide ? 0xe9 : 0xe1));
    put(0x8000, 0x55aa, GuestScalarWidth::I16);
    const auto Width = Wide ? GuestScalarWidth::I32 : GuestScalarWidth::I16;
    const unsigned Bytes = Wide ? 4 : 2;
    put(0x8000 + (Wide ? 4 : 2), 0xe000, Width);
    put(0x8000 + (Wide ? 8 : 4), 16, GuestScalarWidth::I16);
    if (Wide)
      put(0x801c, 0x10000, Width);
    put(0x8000 + (Wide ? 32 : 14), 0x3000, Width);
    put(0x8000 + (Wide ? 36 : 16), 0x202, Width);
    for (unsigned I = 0; I != 8; ++I)
      put(0x8000 + (Wide ? 40 : 18) + I * Bytes, I == 4 ? 0xc000 : 0x5100 + I,
          Width);
    const uint16_t Selectors[]{35, 27, 35, 35, 0, 0};
    for (unsigned I = 0; I != (Wide ? 6u : 4u); ++I)
      put(0x8000 + (Wide ? 72 : 34) + I * Bytes, Selectors[I],
          GuestScalarWidth::I16);
  }
};

} // namespace neverd::translate

#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYTASKTESTSTATE_H
