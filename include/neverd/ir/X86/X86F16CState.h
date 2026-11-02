//===- X86F16CState.h - F16C operand and result layout -*- C++ -*-===//
// NeverD Decompiler
#ifndef NEVERD_IR_X86_X86F16CSTATE_H
#define NEVERD_IR_X86_X86F16CSTATE_H
#include "neverd/ir/intrinsics/Intrinsics.h"
namespace neverd {
constexpr bool isX86F16CStateIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X86F16CState || Id == Intrinsic::X86F16CReadState ||
         Id == Intrinsic::X86F16CStoreState;
}
// bit0: PS to PH; bit1: 256-bit single-precision operand.
constexpr bool x86F16CIsNarrow(unsigned Control) { return Control & 1; }
constexpr unsigned x86F16CSingleBytes(unsigned Control) {
  return Control & 2 ? 32 : 16;
}
constexpr unsigned x86F16CSourceBytes(unsigned Control) {
  return x86F16CSingleBytes(Control) / (x86F16CIsNarrow(Control) ? 1 : 2);
}
constexpr unsigned x86F16CDestinationBytes(unsigned Control) {
  return x86F16CIsNarrow(Control) ? 16 : x86F16CSingleBytes(Control);
}
constexpr unsigned x86F16CStoreBytes(unsigned Control) {
  return x86F16CSingleBytes(Control) / 2;
}
constexpr const char *x86F16CMnemonic(unsigned Control) {
  return x86F16CIsNarrow(Control) ? "vcvtps2ph" : "vcvtph2ps";
}
} // namespace neverd
#endif
