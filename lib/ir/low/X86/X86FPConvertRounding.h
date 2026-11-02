//===- X86FPConvertRounding.h - Narrow precision exponent boundary -*- C++
//-*-===//
// NeverD Decompiler
#ifndef NEVERD_LIB_IR_LOW_X86_X86FPCONVERTROUNDING_H
#define NEVERD_LIB_IR_LOW_X86_X86FPCONVERTROUNDING_H
#include "llvm/ADT/APFloat.h"

#include <cstdint>
namespace neverd {
// Test exponent range after precision rounding, before denormalization.
inline int x86NarrowRoundedExponent(uint64_t Bits, unsigned ExponentBits,
                                    unsigned FractionBits,
                                    unsigned ResultFractionBits,
                                    llvm::APFloat::roundingMode Rounding) {
  const bool Negative = (Bits >> (ExponentBits + FractionBits)) != 0;
  const unsigned Bias = (1U << (ExponentBits - 1)) - 1;
  const unsigned Shift = FractionBits - ResultFractionBits;
  const unsigned Exponent = (Bits >> FractionBits) & ((1U << ExponentBits) - 1);
  if (!Exponent)
    return 1 - int(Bias);
  const uint64_t Mantissa = (UINT64_C(1) << FractionBits) |
                            (Bits & ((UINT64_C(1) << FractionBits) - 1));
  uint64_t Rounded = Mantissa >> Shift;
  const uint64_t Remainder = Mantissa & ((UINT64_C(1) << Shift) - 1);
  bool Up = false;
  if (Remainder) {
    if (Rounding == llvm::APFloat::rmNearestTiesToEven)
      Up = Remainder > (UINT64_C(1) << (Shift - 1)) ||
           (Remainder == (UINT64_C(1) << (Shift - 1)) && (Rounded & 1));
    else if (Rounding == llvm::APFloat::rmTowardPositive)
      Up = !Negative;
    else if (Rounding == llvm::APFloat::rmTowardNegative)
      Up = Negative;
  }
  Rounded += Up;
  return int(Exponent) - int(Bias) +
         (Rounded == (UINT64_C(1) << (ResultFractionBits + 1)));
}

} // namespace neverd
#endif
