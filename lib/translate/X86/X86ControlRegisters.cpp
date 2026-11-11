//===- X86ControlRegisters.cpp - Intel paging-control checks -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared mode-aware control transitions, independent of physical RAM access.
//===----------------------------------------------------------------------===//

#include "neverd/translate/X86/X86ControlRegisters.h"

namespace neverd::translate {
namespace {
constexpr uint64_t PE = 1, ET = 0x10, WP = 1u << 16, NW = 1u << 29,
                   CD = 1u << 30, PG = 1u << 31, PAE = 1u << 5, LA57 = 1u << 12,
                   PCIDE = 1u << 17, CET = 1u << 23, LME = 1u << 8,
                   LMA = 1u << 10;
}

uint32_t x86SupportedCR4(const X86PagingFeatures &F, uint32_t Other) {
  return Other | (uint32_t(F.PSE) << 4) | (uint32_t(F.PAE) << 5) |
         (uint32_t(F.LA57) << 12) | (uint32_t(F.SMEP) << 20) |
         (uint32_t(F.SMAP) << 21) | (uint32_t(F.PKU) << 22) |
         (uint32_t(F.PKS) << 24);
}

X86PagingControlWrite
planX86PagingControlWrite(const X86PagingState &Old, const X86PagingFeatures &F,
                          uint32_t Other, unsigned Control, uint64_t Value,
                          bool Code64, bool TR16) {
  X86PagingControlWrite R{Old};
  auto &C = R.Controls;
  const auto Fault = [&]() {
    R.Controls = Old;
    R.Exception = X86MemoryException{13, 0, {}};
    return R;
  };
  switch (Control) {
  case 0:
    C.CR0 = (Value & kX86CR0StoredMask) | ET;
    if ((Value >> 32) || ((C.CR0 & PG) && !(C.CR0 & PE)) ||
        ((C.CR0 & NW) && !(C.CR0 & CD)) || (!(C.CR0 & WP) && (C.CR4 & CET)) ||
        (!(C.CR0 & PG) && ((C.CR4 & PCIDE) || Code64)))
      return Fault();
    if (!(Old.CR0 & PG) && (C.CR0 & PG) && (C.EFER & LME) &&
        (!(C.CR4 & PAE) || Code64 || TR16))
      return Fault();
    C.EFER = (C.EFER & ~LMA) | (((C.CR0 & PG) && (C.EFER & LME)) ? LMA : 0);
    break;
  case 3: {
    // The no-flush request is an operand bit, never a stored CR3 bit.
    const uint64_t NoFlush = C.CR4 & PCIDE ? uint64_t{1} << 63 : 0;
    if ((C.EFER & LMA) &&
        (Value & ~(((uint64_t{1} << F.PhysicalAddressBits) - 1) | NoFlush)))
      return Fault();
    C.CR3 = Value & ~NoFlush;
    break;
  }
  case 4:
    if ((Value & ~uint64_t(x86SupportedCR4(F, Other))) ||
        ((Value & PCIDE) && !(C.EFER & LMA)) ||
        ((Value & PCIDE) && !(Old.CR4 & PCIDE) && (C.CR3 & 0xfff)) ||
        ((C.EFER & LMA) && (!(Value & PAE) || ((Value ^ C.CR4) & LA57))) ||
        ((Value & CET) && !(C.CR0 & WP)))
      return Fault();
    C.CR4 = Value;
    break;
  default:
    R.Exception = X86MemoryException{6, 0, {}};
    return R;
  }
  R.ReloadPDPTE = !(C.EFER & LMA) && (C.CR0 & PG) && (C.CR4 & PAE) &&
                  (Control == 3 || ((Old.CR0 ^ C.CR0) & (CD | NW | PG)) ||
                   ((Old.CR4 ^ C.CR4) & (PAE | 0x90 | (1u << 20))));
  return R;
}

} // namespace neverd::translate
