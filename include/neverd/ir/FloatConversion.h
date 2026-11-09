//===- FloatConversion.h - Scalar FP conversion policy ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_FLOATCONVERSION_H
#define NEVERD_IR_FLOATCONVERSION_H

#include "neverd/Common.h"

#include <cstdint>

namespace neverd {

enum class FPToIntegerPolicy { Saturate, X86Indefinite };

/// FLD/FST precision conversions quiet signaling NaNs and stores obey the
/// x87 rounding mode. They are not reversible bit views, even when the final
/// IEEE width equals the initial one.
constexpr bool isX87PrecisionConversion(Arch Target, uint16_t InputBytes,
                                        uint16_t OutputBytes) {
  return (Target == Arch::X86 || Target == Arch::X64) &&
         ((InputBytes == 10 && (OutputBytes == 4 || OutputBytes == 8)) ||
          (OutputBytes == 10 && (InputBytes == 4 || InputBytes == 8)));
}

/// Result policy for scalar FLOAT_FLOAT2INT/FLOAT_FLOAT2UINT/FLOAT_TRUNC.
/// Floating control/status effects belong to their architecture intrinsics.
constexpr FPToIntegerPolicy fpToIntegerPolicy(Arch Target) {
  return Target == Arch::X86 || Target == Arch::X64
             ? FPToIntegerPolicy::X86Indefinite
             : FPToIntegerPolicy::Saturate;
}

} // namespace neverd

#endif
