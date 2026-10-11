//===- X86FPPackedConvertState.h - Packed conversion controls -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authoritative SSE/VEX packed conversion widths, rounding and completion.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_X86FPPACKEDCONVERTSTATE_H
#define NEVERD_IR_X86FPPACKEDCONVERTSTATE_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <array>
#include <cstdint>
#include <string>

namespace neverd {
// Low3 bits select the eight SSE conversion operations; bit3 is VEX, bit4
// VL256.
enum class X86FPPackedConvertKind : uint8_t {
  DwordToSingle,
  DwordToDouble,
  SingleToDouble,
  DoubleToSingle,
  SingleToDword,
  DoubleToDword,
  TruncateSingleToDword,
  TruncateDoubleToDword,
};
constexpr unsigned x86FPPackedConvertKind(unsigned Control) {
  return Control & 7;
}
constexpr bool isX86FPPackedConvertStateIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X86FPPackedConvertState ||
         Id == Intrinsic::X86FPPackedConvertMemoryState;
}
constexpr bool x86FPPackedConvertControlIsValid(uint64_t Control) {
  return (Control & ~UINT64_C(31)) == 0 && (!(Control & 16) || (Control & 8));
}
constexpr unsigned x86FPPackedConvertWidth(unsigned Control) {
  return (Control & 16) ? 32 : 16;
}
constexpr bool x86FPPackedConvertIsWidening(unsigned Control) {
  const unsigned Kind = x86FPPackedConvertKind(Control);
  return Kind == 1 || Kind == 2;
}
constexpr bool x86FPPackedConvertIsNarrowing(unsigned Control) {
  const unsigned Kind = x86FPPackedConvertKind(Control);
  return Kind == 3 || Kind == 5 || Kind == 7;
}
constexpr unsigned x86FPPackedConvertSourceBytes(unsigned Control) {
  return x86FPPackedConvertWidth(Control) /
         (x86FPPackedConvertIsWidening(Control) ? 2 : 1);
}
constexpr unsigned x86FPPackedConvertDestinationBytes(unsigned Control) {
  return x86FPPackedConvertIsNarrowing(Control)
             ? 16
             : x86FPPackedConvertWidth(Control);
}
constexpr unsigned x86FPPackedConvertSourceElementBytes(unsigned Control) {
  return x86FPPackedConvertIsNarrowing(Control) ? 8 : 4;
}
constexpr unsigned x86FPPackedConvertDestinationElementBytes(unsigned Control) {
  return x86FPPackedConvertIsWidening(Control) ? 8 : 4;
}
constexpr unsigned x86FPPackedConvertMemoryAlignment(unsigned Control) {
  return !(Control & 8) && x86FPPackedConvertSourceBytes(Control) == 16 ? 16
                                                                        : 1;
}
inline std::string x86FPPackedConvertMnemonic(unsigned Control) {
  constexpr std::array<const char *, 8> Names{
      "cvtdq2ps", "cvtdq2pd", "cvtps2pd",  "cvtpd2ps",
      "cvtps2dq", "cvtpd2dq", "cvttps2dq", "cvttpd2dq"};
  return std::string((Control & 8) ? "v" : "") + Names[Control & 7];
}
inline std::string x86FPPackedConvertAsmMnemonic(unsigned Control,
                                                 bool Memory) {
  return x86FPPackedConvertMnemonic(Control) +
         (Memory && (Control & 8) && x86FPPackedConvertIsNarrowing(Control)
              ? ((Control & 16) ? "y" : "x")
              : "");
}
constexpr uint16_t x86FPPackedConvertEvaluationControl(unsigned Control) {
  const unsigned Kind = Control & 7;
  const bool Truncate = Kind >= 6;
  return makeX86FPConvertControl(
      Kind < 2   ? X86FPConvertKind::SignedIntegerToFloat
      : Kind < 4 ? X86FPConvertKind::FloatToFloat
                 : X86FPConvertKind::FloatToSignedInteger,
      x86FPPackedConvertSourceElementBytes(Control) == 8,
      x86FPPackedConvertDestinationElementBytes(Control) == 8, Truncate, false,
      Truncate ? X86FPRounding::TowardZero : X86FPRounding::MXCSR,
      x86FPPackedConvertSourceBytes(Control) /
          x86FPPackedConvertSourceElementBytes(Control));
}
} // namespace neverd
#endif
