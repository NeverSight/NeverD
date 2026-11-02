//===- AArch64FloatConversion.cpp - ARM64 scalar narrowing ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// FPCR rounding, NaN/denormal handling and FPSR effects of FCVT S,D.
//===----------------------------------------------------------------------===//
#include "AArch64FloatConversion.h"

#include "AArch64Machine.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"

namespace neverd::emulation {
std::optional<AArch64FloatConversion> narrowAArch64Double(uint64_t Bits,
                                                          uint64_t FPCR) {
  if (FPCR & ~aarch64::AllowedFPCR)
    return std::nullopt;
  const uint64_t Magnitude = Bits & 0x7fffffffffffffffull;
  const uint32_t Sign = uint32_t(Bits >> 32) & 0x80000000u;
  const bool Tiny = Magnitude && Magnitude < 0x3810000000000000ull;
  if (FPCR & (1u << 24)) {
    if (Magnitude && Magnitude < 0x0010000000000000ull)
      return AArch64FloatConversion{Sign, 1u << 7}; // Input denormal.
    // Output flushing uses tininess before rounding, even when the rounded
    // value would be normal. It raises UFC without adding IXC.
    if (Tiny)
      return AArch64FloatConversion{Sign, 1u << 3};
  }
  constexpr llvm::APFloat::roundingMode Modes[] = {
      llvm::APFloat::rmNearestTiesToEven, llvm::APFloat::rmTowardPositive,
      llvm::APFloat::rmTowardNegative, llvm::APFloat::rmTowardZero};
  llvm::APFloat Value(llvm::APFloat::IEEEdouble(), llvm::APInt(64, Bits));
  bool LosesInfo;
  const auto Status = Value.convert(llvm::APFloat::IEEEsingle(),
                                    Modes[(FPCR >> 22) & 3], &LosesInfo);
  uint32_t Raised = 0;
  if (Status & llvm::APFloat::opInvalidOp)
    Raised |= 1u;
  // Directed rounding can saturate to max-finite with APFloat reporting only
  // inexact. A finite input at or above 2^128 still raises ARM64 overflow.
  if ((Status & llvm::APFloat::opOverflow) ||
      (Magnitude >= 0x47f0000000000000ull && Magnitude < 0x7ff0000000000000ull))
    Raised |= 1u << 2;
  if (Status & llvm::APFloat::opInexact) {
    Raised |= 1u << 4;
    // APFloat detects tininess after rounding. ARM64's baseline conversion
    // also raises underflow when an inexact tiny value rounds up to normal.
    if (Tiny)
      Raised |= 1u << 3;
  }
  const uint32_t Result = Value.isNaN() && (FPCR & (1u << 25))
                              ? 0x7fc00000u
                              : uint32_t(Value.bitcastToAPInt().getZExtValue());
  return AArch64FloatConversion{Result, Raised};
}
} // namespace neverd::emulation
