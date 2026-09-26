//===- X87PartialRemainder.cpp - Exact concrete FPREM semantics ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/X87PartialRemainder.h"

#include "neverd/lift/X86Regs.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace neverd {

namespace {

struct X87Encoding {
  uint64_t Significand = 0;
  uint16_t SignExponent = 0;

  unsigned exponent() const { return SignExponent & 0x7fffU; }
  bool integerBit() const { return (Significand >> 63) != 0; }
  bool isZero() const { return exponent() == 0 && Significand == 0; }
  bool isSubnormal() const {
    return exponent() == 0 && Significand != 0 && !integerBit();
  }
  bool isInfinity() const {
    return exponent() == 0x7fffU && Significand == UINT64_C(0x8000000000000000);
  }
  bool isNaN() const {
    return exponent() == 0x7fffU && Significand != UINT64_C(0x8000000000000000);
  }
  bool isCanonical() const {
    if (exponent() == 0)
      return !integerBit();
    return integerBit();
  }
};

X87Encoding decodeX87(llvm::ArrayRef<uint8_t> Bytes) {
  X87Encoding Value;
  for (unsigned Index = 0; Index != 8; ++Index)
    Value.Significand |= uint64_t(Bytes[Index]) << (Index * 8);
  Value.SignExponent = uint16_t(Bytes[8]) | (uint16_t(Bytes[9]) << 8);
  return Value;
}

llvm::APInt x87Bits(llvm::ArrayRef<uint8_t> Bytes) {
  llvm::APInt Bits(80, 0);
  for (unsigned Index = 0; Index != 10; ++Index)
    Bits |= llvm::APInt(80, Bytes[Index]) << (Index * 8);
  return Bits;
}

std::array<uint8_t, 10> encodeX87(const llvm::APFloat &Value) {
  const llvm::APInt Bits = Value.bitcastToAPInt();
  std::array<uint8_t, 10> Bytes{};
  for (unsigned Index = 0; Index != Bytes.size(); ++Index)
    Bytes[Index] =
        static_cast<uint8_t>(Bits.extractBitsAsZExtValue(8, Index * 8));
  return Bytes;
}

/// Return the low three bits of the absolute integral quotient. The x87
/// reports magnitude bits, matching Intel-compatible hardware and SoftFloat.
uint8_t completedQuotientBits(const X87Encoding &Dividend,
                              const X87Encoding &Divisor,
                              bool RoundNearestEven) {
  const int ExponentDifference = static_cast<int>(Dividend.exponent()) -
                                 static_cast<int>(Divisor.exponent());

  if (ExponentDifference < 0) {
    if (!RoundNearestEven || ExponentDifference < -1)
      return 0;
    return Dividend.Significand > Divisor.Significand ? 1U : 0U;
  }

  llvm::APInt Numerator(128, Dividend.Significand);
  llvm::APInt Denominator(128, Divisor.Significand);
  Numerator <<= static_cast<unsigned>(ExponentDifference);

  llvm::APInt Quotient = Numerator.udiv(Denominator);
  if (RoundNearestEven) {
    const llvm::APInt Remainder = Numerator.urem(Denominator);
    const llvm::APInt TwiceRemainder = Remainder.shl(1);
    if (TwiceRemainder.ugt(Denominator) ||
        (TwiceRemainder == Denominator && Quotient[0]))
      ++Quotient;
  }
  return static_cast<uint8_t>(Quotient.trunc(3).getZExtValue());
}

uint16_t quotientConditionCodes(uint8_t Quotient) {
  uint16_t Status = 0;
  if ((Quotient & 4U) != 0)
    Status |= UINT16_C(1) << x86reg::FPU_SW_C0_BIT;
  if ((Quotient & 1U) != 0)
    Status |= UINT16_C(1) << x86reg::FPU_SW_C1_BIT;
  if ((Quotient & 2U) != 0)
    Status |= UINT16_C(1) << x86reg::FPU_SW_C3_BIT;
  return Status;
}

} // namespace

std::optional<X87PartialRemainderResult>
evaluateX87PartialRemainder(llvm::ArrayRef<uint8_t> DividendBytes,
                            llvm::ArrayRef<uint8_t> DivisorBytes,
                            bool RoundNearestEven) {
  if (DividendBytes.size() != 10 || DivisorBytes.size() != 10)
    return std::nullopt;
  const X87Encoding Dividend = decodeX87(DividendBytes);
  const X87Encoding Divisor = decodeX87(DivisorBytes);

  // Invalid encodings and operands that raise x87 exceptions need the tag
  // word, exception masks, and sticky exception fields to decide whether the
  // destination commits. Reject them instead of returning a plausible NaN.
  if (!Dividend.isCanonical() || !Divisor.isCanonical() ||
      Dividend.isSubnormal() || Divisor.isSubnormal() || Dividend.isNaN() ||
      Divisor.isNaN() || Dividend.isInfinity() || Divisor.isZero())
    return std::nullopt;

  X87PartialRemainderResult Result;
  std::copy(DividendBytes.begin(), DividendBytes.end(), Result.Value.begin());
  Result.Complete = true;
  if (Dividend.isZero() || Divisor.isInfinity())
    return Result;

  llvm::APFloat Remainder(llvm::APFloat::x87DoubleExtended(),
                          x87Bits(DividendBytes));
  const llvm::APFloat DivisorValue(llvm::APFloat::x87DoubleExtended(),
                                   x87Bits(DivisorBytes));
  const int ExponentDifference = static_cast<int>(Dividend.exponent()) -
                                 static_cast<int>(Divisor.exponent());
  llvm::APFloat::opStatus ArithmeticStatus = llvm::APFloat::opOK;
  if (ExponentDifference < 64) {
    ArithmeticStatus = RoundNearestEven ? Remainder.remainder(DivisorValue)
                                        : Remainder.mod(DivisorValue);
    Result.ConditionCodes = quotientConditionCodes(
        completedQuotientBits(Dividend, Divisor, RoundNearestEven));
  } else {
    // Intel permits an implementation-selected N in [32,63]. Use N=63 to
    // match this project's Unicorn CPU model; the partial-result bytes and
    // non-C2 condition codes are not architecture-invariant.
    constexpr int PartialReductionBits = 63;
    llvm::APFloat ScaledDivisor =
        llvm::scalbn(DivisorValue, ExponentDifference - PartialReductionBits,
                     llvm::APFloat::rmNearestTiesToEven);
    if (!ScaledDivisor.isFinite() || ScaledDivisor.isZero())
      return std::nullopt;
    ArithmeticStatus = Remainder.mod(ScaledDivisor);
    Result.ConditionCodes = X87PartialRemainderC2Mask;
    Result.Complete = false;
  }
  if (ArithmeticStatus != llvm::APFloat::opOK)
    return std::nullopt;
  Result.Value = encodeX87(Remainder);
  const X87Encoding EncodedResult = decodeX87(Result.Value);
  if (!EncodedResult.isCanonical() || EncodedResult.isSubnormal() ||
      EncodedResult.isNaN() || EncodedResult.isInfinity())
    return std::nullopt;
  return Result;
}

} // namespace neverd
