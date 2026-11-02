//===- X86FPComiState.h - Scalar comparison completion --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// COMI/UCOMI raw flags and explicit MXCSR completion controls.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_X86_X86FPCOMISTATE_H
#define NEVERD_IR_X86_X86FPCOMISTATE_H
#include "neverd/ir/intrinsics/Intrinsics.h"
namespace neverd {
constexpr bool isX86FPComiStateIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X86FPComiState ||
         Id == Intrinsic::X86FPComiMemoryState;
}
constexpr bool x86FPComiControlIsValid(unsigned Control) { return Control < 8; }
constexpr unsigned x86FPComiElementBytes(unsigned Control) {
  return Control & 2 ? 8 : 4;
}
constexpr const char *x86FPComiMnemonic(unsigned Control) {
  if (Control & 4)
    return Control & 1 ? (Control & 2 ? "vucomisd" : "vucomiss")
                       : (Control & 2 ? "vcomisd" : "vcomiss");
  return Control & 1 ? (Control & 2 ? "ucomisd" : "ucomiss")
                     : (Control & 2 ? "comisd" : "comiss");
}
/// LAHF's defined low status byte: SF/AF=0 and bit one always set.
constexpr uint32_t x86FPComiFlags(unsigned Relation) {
  return Relation == 0   ? 0x03
         : Relation == 1 ? 0x42
         : Relation == 2 ? 0x02
                         : 0x47;
}
} // namespace neverd
#endif
