//===- X86FPMaskCompareState.h - SSE/VEX mask completion -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// CMP predicates, active widths and explicit raw-mask/MXCSR completion.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_X86_X86FPMASKCOMPARESTATE_H
#define NEVERD_IR_X86_X86FPMASKCOMPARESTATE_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <string>

namespace neverd {
constexpr bool isX86FPMaskCompareStateIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X86FPMaskCompareState ||
         Id == Intrinsic::X86FPMaskCompareMemoryState;
}
// bit0: double; bit1: scalar; bit2: VEX. Packed width is retained in the value.
constexpr bool x86FPMaskCompareIsScalar(unsigned Control) {
  return (Control & 2) != 0;
}
constexpr unsigned x86FPMaskCompareElementBytes(unsigned Control) {
  return (Control & 1) ? 8 : 4;
}
constexpr bool x86FPMaskCompareLayoutIsValid(uint64_t Control, unsigned Bytes) {
  return Control < 8 && (x86FPMaskCompareIsScalar(Control)
                             ? Bytes == x86FPMaskCompareElementBytes(Control)
                             : Bytes == 16 || (Bytes == 32 && (Control & 4)));
}
constexpr unsigned x86FPMaskComparePredicate(unsigned Control,
                                             unsigned Immediate) {
  return Immediate & ((Control & 4) ? 31 : 7);
}
constexpr unsigned x86FPMaskCompareStateLayout(
    unsigned Bytes, unsigned Control, unsigned Immediate,
    NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default) {
  return Bytes | (Control << 8) | (Immediate << 16) |
         (static_cast<unsigned>(Space) << 24);
}
constexpr unsigned x86FPMaskCompareStateImmediate(unsigned Layout) {
  return (Layout >> 16) & 255;
}
inline std::string x86FPMaskCompareMnemonic(unsigned Control) {
  return std::string(Control & 4 ? "v" : "") + "cmp" +
         (x86FPMaskCompareIsScalar(Control) ? (Control & 1 ? "sd" : "ss")
                                            : (Control & 1 ? "pd" : "ps"));
}
constexpr unsigned x86FPMaskCompareMemoryAlignment(unsigned Control) {
  return x86FPMaskCompareIsScalar(Control) || (Control & 4) ? 1 : 16;
}
} // namespace neverd
#endif // NEVERD_IR_X86_X86FPMASKCOMPARESTATE_H
