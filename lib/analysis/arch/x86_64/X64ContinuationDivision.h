//===- X64ContinuationDivision.h - Explicit divide errors -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Test the architectural quotient range before division or state publication.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONDIVISION_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONDIVISION_H

#include "neverd/ir/low/LowIR.h"

#include <cstdint>
#include <initializer_list>

namespace neverd::analysis::detail {

/// The caller has admitted the complete X86RequireDivPrecondition shape.
/// Use unsigned magnitudes and multiplication, never a potentially faulting
/// division. The native precondition remains in the successful continuation.
template <class Writer>
NdVar x64DivisionError(Writer &W, const LowOp &Instruction) {
  const NdVar Dividend = Instruction.Inputs[1], Divisor = Instruction.Inputs[2];
  const uint16_t Full = Dividend.Size, Half = Divisor.Size;
  const auto Op = [&](NdOp Code, uint16_t Size,
                      std::initializer_list<NdVar> Inputs) {
    const auto Value = W.temporary(Size);
    W.emit(Code, Value, Inputs);
    return Value;
  };
  const auto Zero = NdVar::scalar(0, Full);
  if (Instruction.Inputs[3].Offset == uint64_t(X86DivKind::Unsigned)) {
    const auto High =
        Op(NdOp::SUBBYTES, Half, {Dividend, NdVar::scalar(Half, 8)});
    // This includes divisor zero without performing any arithmetic division.
    return Op(NdOp::INT_LESSEQUAL, 1, {Divisor, High});
  }
  const auto DividendNegative = Op(NdOp::INT_SLESS, 1, {Dividend, Zero});
  const auto DivisorNegative =
      Op(NdOp::INT_SLESS, 1, {Divisor, NdVar::scalar(0, Half)});
  const auto DividendNegated = Op(NdOp::INT_SUB, Full, {Zero, Dividend});
  const auto Magnitude =
      Op(NdOp::SELECT, Full, {DividendNegative, DividendNegated, Dividend});
  const auto ExtendedDivisor = Op(NdOp::INT_SEXT, Full, {Divisor});
  const auto DivisorNegated = Op(NdOp::INT_SUB, Full, {Zero, ExtendedDivisor});
  const auto DivisorMagnitude = Op(
      NdOp::SELECT, Full, {DivisorNegative, DivisorNegated, ExtendedDivisor});
  const auto NegativeQuotient =
      Op(NdOp::BOOL_XOR, 1, {DividendNegative, DivisorNegative});
  const uint64_t PositiveLimit = UINT64_C(1) << (Half * 8 - 1);
  const auto Limit =
      Op(NdOp::SELECT, Full,
         {NegativeQuotient, NdVar::scalar(PositiveLimit + 1, Full),
          NdVar::scalar(PositiveLimit, Full)});
  const auto Threshold = Op(NdOp::INT_MULT, Full, {DivisorMagnitude, Limit});
  // Truncation toward zero allows magnitude 2^(n-1) only for a negative
  // quotient. The exclusive bounds fit in 2n bits, including signed minima.
  return Op(NdOp::INT_LESSEQUAL, 1, {Threshold, Magnitude});
}

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONDIVISION_H
