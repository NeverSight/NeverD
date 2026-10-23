//===- X87Arithmetic.cpp - Directly rounded finite x87 arithmetic --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact integer/rational arithmetic followed by one rounding operation. PC
/// changes significand precision, never the binary80 exponent range. Tininess
/// is assessed after rounding with unbounded exponent; a bounded rounding is
/// then calculated from the original rational, not that rounded intermediate.
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/X87Arithmetic.h"

#include "neverd/ir/X87Control.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace neverd {
std::optional<X87ComparisonResult>
evaluateFiniteX87Compare(const X87Binary80Bytes &Left,
                         const X87Binary80Bytes &Right) {
  const auto A = *decodeX87Binary80(Left), B = *decodeX87Binary80(Right);
  if (!A.isCanonical() || !B.isCanonical() || A.exponent() == 0x7fff ||
      B.exponent() == 0x7fff)
    return std::nullopt;
  X87Comparison Relation;
  if ((A.isZero() && B.isZero()) ||
      (A.SignExponent == B.SignExponent && A.Significand == B.Significand)) {
    Relation = X87Comparison::Equal;
  } else if (A.negative() != B.negative()) {
    Relation = A.negative() ? X87Comparison::Less : X87Comparison::Greater;
  } else {
    const bool MagnitudeLess = A.exponent() != B.exponent()
                                   ? A.exponent() < B.exponent()
                                   : A.Significand < B.Significand;
    Relation = MagnitudeLess != A.negative() ? X87Comparison::Less
                                             : X87Comparison::Greater;
  }
  return X87ComparisonResult{
      Relation, uint16_t(A.isSubnormal() || B.isSubnormal() ? 2 : 0)};
}

std::optional<X87Binary80Bytes>
convertIntegerToX87(uint64_t Bits, unsigned Bytes, bool Signed) {
  if (Bytes != 1 && Bytes != 2 && Bytes != 4 && Bytes != 8)
    return std::nullopt;
  llvm::APFloat Value(llvm::APFloat::x87DoubleExtended());
  if (Value.convertFromAPInt(llvm::APInt(Bytes * 8, Bits), Signed,
                             llvm::APFloat::rmNearestTiesToEven) !=
      llvm::APFloat::opOK)
    return std::nullopt;
  const auto Encoded = Value.bitcastToAPInt();
  X87Binary80Bytes Result{};
  for (unsigned I = 0; I != Result.size(); ++I)
    Result[I] = Encoded.extractBitsAsZExtValue(8, I * 8);
  return Result;
}

namespace {
// Architectural format and status constants, not resource-policy limits.
constexpr int ExponentBias = 16383;
constexpr int MinimumExponent = -16382;
constexpr int MaximumExponent = 16383;
constexpr unsigned SignificandBits = 64;
constexpr uint64_t IntegerBit = UINT64_C(1) << 63;
constexpr uint16_t Invalid = 1, Denormal = 2, ZeroDivide = 4, Overflow = 8,
                   Underflow = 16, Precision = 32;

struct Finite {
  uint64_t Significand;
  int Scale;
  bool Negative;
  bool Subnormal;
};

std::optional<Finite> decode(const X87Binary80Bytes &Bytes) {
  const auto Raw = *decodeX87Binary80(Bytes);
  if (!Raw.isCanonical() || Raw.exponent() == 0x7fff)
    return std::nullopt;
  return Finite{Raw.Significand,
                int(std::max(Raw.exponent(), 1U)) - ExponentBias -
                    int(SignificandBits - 1),
                Raw.negative(), Raw.isSubnormal()};
}

X87Binary80Bytes encode(uint64_t Significand, unsigned Exponent,
                        bool Negative) {
  return X87Binary80{Significand, uint16_t(Exponent | (Negative ? 0x8000 : 0))}
      .bytes();
}

struct Rounded {
  llvm::APInt Significand;
  bool Inexact;
  bool Incremented;
};

// N / D * 2^Shift rounded to an integer. A wide enough division retains the
// whole remainder, including distant low bits deciding a midpoint tie.
Rounded roundRational(const llvm::APInt &N, const llvm::APInt &D, int Shift,
                      unsigned RC, bool Negative) {
  const unsigned Width =
      std::max(N.getActiveBits() + unsigned(std::max(Shift, 0)),
               D.getActiveBits() + unsigned(std::max(-Shift, 0))) +
      2;
  auto Numerator = N.zextOrTrunc(Width);
  auto Denominator = D.zextOrTrunc(Width);
  if (Shift >= 0)
    Numerator <<= unsigned(Shift);
  else
    Denominator <<= unsigned(-Shift);
  llvm::APInt Quotient(Width, 0), Remainder(Width, 0);
  llvm::APInt::udivrem(Numerator, Denominator, Quotient, Remainder);
  const bool Inexact = !Remainder.isZero();
  bool Increment = false;
  if (Inexact) {
    if (RC == 0) {
      auto Twice = Remainder.shl(1);
      Increment =
          Twice.ugt(Denominator) || (Twice == Denominator && Quotient[0]);
    } else {
      Increment = (RC == 1 && Negative) || (RC == 2 && !Negative);
    }
  }
  if (Increment)
    ++Quotient;
  return {std::move(Quotient), Inexact, Increment};
}

int leadingExponent(const llvm::APInt &N, const llvm::APInt &D) {
  int Candidate = int(N.getActiveBits()) - int(D.getActiveBits());
  const unsigned Width = std::max(N.getActiveBits(), D.getActiveBits()) + 1;
  const auto Numerator = N.zextOrTrunc(Width);
  const auto Denominator = D.zextOrTrunc(Width);
  if (Candidate >= 0 ? Numerator.ult(Denominator.shl(unsigned(Candidate)))
                     : Numerator.shl(unsigned(-Candidate)).ult(Denominator))
    --Candidate;
  return Candidate;
}

X87ArithmeticResult roundResult(const llvm::APInt &N, const llvm::APInt &D,
                                int Scale, bool Negative, unsigned PC,
                                unsigned RC, uint16_t Exceptions) {
  X87ArithmeticResult Result;
  Result.Exceptions = Exceptions;
  if (N.isZero()) {
    Result.Value = encode(0, 0, Negative);
    return Result;
  }
  const int Leading = leadingExponent(N, D) + Scale;
  const int UnboundedUnit = Leading - int(PC - 1);
  const auto Unbounded =
      roundRational(N, D, Scale - UnboundedUnit, RC, Negative);
  const int RoundedLeading =
      Leading + (Unbounded.Significand.getActiveBits() > PC ? 1 : 0);
  if (RoundedLeading > MaximumExponent) {
    const bool Infinity =
        RC == 0 || (RC == 1 && Negative) || (RC == 2 && !Negative);
    const uint64_t Largest = ~UINT64_C(0) << (SignificandBits - PC);
    Result.Value = encode(Infinity ? IntegerBit : Largest,
                          Infinity ? 0x7fff : 0x7ffe, Negative);
    Result.Exceptions |= Overflow | Precision;
    Result.RoundedUp = Infinity;
    return Result;
  }
  const int Unit = std::max(UnboundedUnit, MinimumExponent - int(PC - 1));
  auto Rounded = roundRational(N, D, Scale - Unit, RC, Negative);
  Result.RoundedUp = Rounded.Incremented;
  if (Rounded.Inexact) {
    Result.Exceptions |= Precision;
    if (RoundedLeading < MinimumExponent)
      Result.Exceptions |= Underflow;
  }
  const unsigned Active = Rounded.Significand.getActiveBits();
  if (!Active) {
    Result.Value = encode(0, 0, Negative);
    return Result;
  }
  const int ResultLeading = Unit + int(Active - 1);
  uint64_t Significand = 0;
  unsigned Exponent = 0;
  if (ResultLeading >= MinimumExponent) {
    // A rounding carry can leave 65 bits. Its low bit is zero, so this
    // normalization introduces no additional rounding.
    if (Active > SignificandBits)
      Rounded.Significand = Rounded.Significand.lshr(Active - SignificandBits);
    Significand = Rounded.Significand.getZExtValue();
    if (Active < SignificandBits)
      Significand <<= SignificandBits - Active;
    Exponent = ResultLeading + ExponentBias;
  } else {
    Significand = Rounded.Significand.getZExtValue();
    Significand <<= Unit - (MinimumExponent - int(SignificandBits - 1));
  }
  Result.Value = encode(Significand, Exponent, Negative);
  return Result;
}
} // namespace

std::optional<X87ArithmeticResult>
evaluateFiniteX87Arithmetic(X87ArithmeticOperation Operation,
                            const X87Binary80Bytes &Left,
                            const X87Binary80Bytes &Right, uint16_t Control) {
  if (!x87ControlWordIsValid(Control))
    return std::nullopt;
  auto A = decode(Left), B = decode(Right);
  if (!A || !B)
    return std::nullopt;
  const unsigned PC = x87PrecisionBits(Control);
  const unsigned RC = (Control >> 10) & 3;
  uint16_t Exceptions = A->Subnormal || B->Subnormal ? Denormal : 0;
  llvm::APInt N(128, 0), D(128, 1);
  int Scale = 0;
  bool Negative = false;
  switch (Operation) {
  case X87ArithmeticOperation::Subtract:
    B->Negative = !B->Negative;
    [[fallthrough]];
  case X87ArithmeticOperation::Add: {
    Scale = std::min(A->Scale, B->Scale);
    const unsigned Width =
        SignificandBits + unsigned(std::abs(A->Scale - B->Scale)) + 1;
    auto L = llvm::APInt(Width, A->Significand).shl(unsigned(A->Scale - Scale));
    auto R = llvm::APInt(Width, B->Significand).shl(unsigned(B->Scale - Scale));
    if (A->Negative == B->Negative) {
      N = L + R;
      Negative = A->Negative;
    } else if (L == R) {
      N = llvm::APInt(Width, 0);
      Negative = RC == 1;
    } else {
      const bool LeftLarger = L.ugt(R);
      N = LeftLarger ? L - R : R - L;
      Negative = LeftLarger ? A->Negative : B->Negative;
    }
    break;
  }
  case X87ArithmeticOperation::Multiply:
    N = llvm::APInt(128, A->Significand) * llvm::APInt(128, B->Significand);
    Scale = A->Scale + B->Scale;
    Negative = A->Negative != B->Negative;
    break;
  case X87ArithmeticOperation::Divide:
    Negative = A->Negative != B->Negative;
    if (B->Significand == 0) {
      if (A->Significand == 0)
        return X87ArithmeticResult{
            encode(UINT64_C(0xc000000000000000), 0x7fff, true), Invalid, false};
      // Zero divide precedes denormal-operand handling, including when both
      // exceptions are masked (Intel SDM volume 1, section 4.9.2).
      return X87ArithmeticResult{encode(IntegerBit, 0x7fff, Negative),
                                 ZeroDivide, false};
    }
    N = llvm::APInt(128, A->Significand);
    D = llvm::APInt(128, B->Significand);
    Scale = A->Scale - B->Scale;
    break;
  default:
    return std::nullopt;
  }
  return roundResult(N, D, Scale, Negative, PC, RC, Exceptions);
}
} // namespace neverd
