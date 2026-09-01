//===- NdOpEmulatorX86FPArith.cpp - Exact x86 SIMD FP arithmetic --------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace neverd {
namespace {

struct FloatFormat {
  uint64_t Sign;
  uint64_t Exponent;
  uint64_t Fraction;
  uint64_t Quiet;
  uint64_t Indefinite;
};

FloatFormat formatForSize(uint16_t Size) {
  if (Size == 4)
    return {UINT64_C(0x80000000), UINT64_C(0x7f800000), UINT64_C(0x007fffff),
            UINT64_C(0x00400000), UINT64_C(0xffc00000)};
  return {UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000000),
          UINT64_C(0x000fffffffffffff), UINT64_C(0x0008000000000000),
          UINT64_C(0xfff8000000000000)};
}

uint64_t readLane(llvm::ArrayRef<uint8_t> Bytes, size_t Offset, uint16_t Size) {
  uint64_t Value = 0;
  std::memcpy(&Value, Bytes.data() + Offset, Size);
  return Value;
}

void writeLane(std::vector<uint8_t> &Bytes, size_t Offset, uint16_t Size,
               uint64_t Value) {
  std::memcpy(Bytes.data() + Offset, &Value, Size);
}

bool isNaN(uint64_t Bits, const FloatFormat &Format) {
  return (Bits & Format.Exponent) == Format.Exponent &&
         (Bits & Format.Fraction) != 0;
}

bool isSignalingNaN(uint64_t Bits, const FloatFormat &Format) {
  return isNaN(Bits, Format) && (Bits & Format.Quiet) == 0;
}

bool isInfinity(uint64_t Bits, const FloatFormat &Format) {
  return (Bits & ~Format.Sign) == Format.Exponent;
}

bool isZero(uint64_t Bits, const FloatFormat &Format) {
  return (Bits & ~Format.Sign) == 0;
}

bool isDenormal(uint64_t Bits, const FloatFormat &Format) {
  return (Bits & Format.Exponent) == 0 && (Bits & Format.Fraction) != 0;
}

llvm::APFloat::roundingMode roundingMode(X86FPRounding Rounding,
                                         uint32_t MXCSR) {
  unsigned Mode = static_cast<unsigned>(Rounding);
  if (Rounding == X86FPRounding::MXCSR)
    Mode = (MXCSR >> 13) & 3U;
  switch (Mode) {
  case 1:
    return llvm::APFloat::rmTowardNegative;
  case 2:
    return llvm::APFloat::rmTowardPositive;
  case 3:
    return llvm::APFloat::rmTowardZero;
  default:
    return llvm::APFloat::rmNearestTiesToEven;
  }
}

uint32_t statusFlags(llvm::APFloat::opStatus Status) {
  const unsigned Raw = static_cast<unsigned>(Status);
  uint32_t Flags = 0;
  if (Raw & llvm::APFloat::opInvalidOp)
    Flags |= 1U << 0;
  if (Raw & llvm::APFloat::opDivByZero)
    Flags |= 1U << 2;
  if (Raw & llvm::APFloat::opOverflow)
    Flags |= 1U << 3;
  if (Raw & llvm::APFloat::opUnderflow)
    Flags |= 1U << 4;
  if (Raw & llvm::APFloat::opInexact)
    Flags |= 1U << 5;
  return Flags;
}

bool hasUnmaskedException(uint32_t MXCSR, uint32_t Raised) {
  return (Raised & ~(MXCSR >> 7) & 0x3fU) != 0;
}

struct ExactSqrtResult {
  uint64_t Bits = 0;
  bool Inexact = false;
};

int floorDivideByTwo(int Value) {
  return Value >= 0 ? Value / 2 : -((-Value + 1) / 2);
}

bool exactPositiveSquareRoot(uint64_t Bits, uint16_t ElementSize,
                             llvm::APFloat::roundingMode Rounding,
                             ExactSqrtResult &Result) {
  const FloatFormat Format = formatForSize(ElementSize);
  if (isZero(Bits, Format) || isInfinity(Bits, Format)) {
    Result.Bits = Bits;
    return true;
  }
  if ((Bits & Format.Sign) != 0 || isNaN(Bits, Format))
    return false;

  const unsigned FractionBits = ElementSize == 4 ? 23 : 52;
  const int ExponentBias = ElementSize == 4 ? 127 : 1023;
  const int MinimumExponent = ElementSize == 4 ? -126 : -1022;
  const uint64_t FractionMask = (UINT64_C(1) << FractionBits) - UINT64_C(1);
  const uint64_t Fraction = Bits & FractionMask;
  const uint64_t RawExponent = (Bits & Format.Exponent) >> FractionBits;

  uint64_t Mantissa = Fraction;
  int BinaryExponent = MinimumExponent - static_cast<int>(FractionBits);
  if (RawExponent != 0) {
    Mantissa |= UINT64_C(1) << FractionBits;
    BinaryExponent = static_cast<int>(RawExponent) - ExponentBias -
                     static_cast<int>(FractionBits);
  }
  if (Mantissa == 0)
    return false;

  const int MantissaExponent =
      static_cast<int>(llvm::APInt(64, Mantissa).logBase2());
  int ResultExponent = floorDivideByTwo(MantissaExponent + BinaryExponent);
  const int Scale =
      BinaryExponent + 2 * (static_cast<int>(FractionBits) - ResultExponent);
  if (Scale < 0 || Scale >= 128)
    return false;

  llvm::APInt Radicand(128, Mantissa);
  Radicand <<= static_cast<unsigned>(Scale);
  llvm::APInt Root = Radicand.sqrt();
  llvm::APInt RootSquared = Root * Root;
  // APInt::sqrt() selects the nearest integer.  Normalize that result to the
  // mathematical floor before applying the guest rounding mode ourselves.
  if (RootSquared.ugt(Radicand)) {
    --Root;
    RootSquared = Root * Root;
  }
  const llvm::APInt NextRoot = Root + 1;
  if (RootSquared.ugt(Radicand) || (NextRoot * NextRoot).ule(Radicand))
    return false;
  const llvm::APInt Remainder = Radicand - RootSquared;
  Result.Inexact = !Remainder.isZero();

  bool RoundUp = false;
  if (Result.Inexact) {
    switch (Rounding) {
    case llvm::APFloat::rmNearestTiesToEven:
      // N and q are integers, so N can never equal (q + 1/2)^2.  The
      // nearest midpoint comparison therefore reduces exactly to N-q^2 > q.
      RoundUp = Remainder.ugt(Root);
      break;
    case llvm::APFloat::rmTowardPositive:
      RoundUp = true;
      break;
    case llvm::APFloat::rmTowardNegative:
    case llvm::APFloat::rmTowardZero:
      break;
    default:
      return false;
    }
  }
  if (RoundUp)
    ++Root;
  if (Root.getActiveBits() > FractionBits + 1) {
    Root = Root.lshr(1);
    ++ResultExponent;
  }

  const int BiasedExponent = ResultExponent + ExponentBias;
  if (BiasedExponent <= 0 ||
      BiasedExponent >= (ElementSize == 4 ? 0xff : 0x7ff) ||
      Root.getActiveBits() > FractionBits + 1)
    return false;
  Result.Bits = (static_cast<uint64_t>(BiasedExponent) << FractionBits) |
                (Root.getZExtValue() & FractionMask);
  return true;
}

} // namespace

bool NdOpEmulator::executeX86FPArith(const LowOp &Op) {
  if (Op.NumInputs != 6 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 2 ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) ||
      (Op.Output.Size != 16 && Op.Output.Size != 32 && Op.Output.Size != 64) ||
      Op.Inputs[2].Size != Op.Output.Size ||
      Op.Inputs[3].Size != Op.Output.Size ||
      Op.Inputs[4].Size != Op.Output.Size)
    return false;

  const uint16_t Control = static_cast<uint16_t>(readOperand(Op.Inputs[1]));
  if (!isValidX86FPArithControl(Control))
    return false;
  const auto Kind = static_cast<X86FPArithKind>(Control & 7U);
  const bool IsF64 = (Control & (UINT16_C(1) << 3)) != 0;
  const bool Scalar = (Control & (UINT16_C(1) << 4)) != 0;
  const bool SuppressExceptions = (Control & (UINT16_C(1) << 5)) != 0;
  const bool NegateProduct = (Control & (UINT16_C(1) << 6)) != 0;
  const bool SubtractAddend = (Control & (UINT16_C(1) << 7)) != 0;
  const auto Rounding = static_cast<X86FPRounding>((Control >> 8) & 7U);
  const bool AlternatingAddend = (Control & (UINT16_C(1) << 11)) != 0;
  const bool SubtractEven = (Control & (UINT16_C(1) << 12)) != 0;
  const uint16_t ElementSize = IsF64 ? 8 : 4;
  const unsigned LaneCount = Scalar ? 1 : Op.Output.Size / ElementSize;
  const uint16_t MaskSize = static_cast<uint16_t>((LaneCount + 7U) / 8U);
  if ((Scalar && Op.Output.Size != 16) || Op.Output.Size % ElementSize != 0 ||
      Op.Inputs[5].Size != MaskSize)
    return false;

  const uint64_t ActiveMask = readOperand(Op.Inputs[5]);
  const std::vector<uint8_t> LeftBytes = readOperandBytes(Op.Inputs[2]);
  const std::vector<uint8_t> RightBytes = readOperandBytes(Op.Inputs[3]);
  const std::vector<uint8_t> AddendBytes = readOperandBytes(Op.Inputs[4]);
  if (LeftBytes.size() < Op.Output.Size || RightBytes.size() < Op.Output.Size ||
      AddendBytes.size() < Op.Output.Size)
    return false;

  const FloatFormat Format = formatForSize(ElementSize);
  const llvm::fltSemantics &Semantics =
      IsF64 ? llvm::APFloat::IEEEdouble() : llvm::APFloat::IEEEsingle();
  const llvm::APFloat::roundingMode APFRounding = roundingMode(Rounding, MXCSR);
  const bool DAZ = (MXCSR & (1U << 6)) != 0;
  const bool UnderflowMasked = SuppressExceptions || (MXCSR & (1U << 11)) != 0;
  const bool FTZ = (MXCSR & (1U << 15)) != 0;

  std::vector<uint8_t> Result(Op.Output.Size, 0);
  std::vector<uint64_t> AValues(LaneCount), BValues(LaneCount),
      CValues(LaneCount);
  std::vector<uint8_t> NeedsArithmetic(LaneCount, 0);
  uint32_t PreRaised = 0;

  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    if (((ActiveMask >> Lane) & 1) == 0)
      continue;
    const size_t Offset = static_cast<size_t>(Lane) * ElementSize;
    const uint64_t OriginalA = readLane(LeftBytes, Offset, ElementSize);
    const uint64_t OriginalB = readLane(RightBytes, Offset, ElementSize);
    const uint64_t OriginalC = readLane(AddendBytes, Offset, ElementSize);

    const bool IsFma = Kind == X86FPArithKind::FusedMultiplyAdd;
    const bool UsesRight = Kind != X86FPArithKind::SquareRoot;
    const bool IsMinMax =
        Kind == X86FPArithKind::Minimum || Kind == X86FPArithKind::Maximum;
    const bool ANaN = isNaN(OriginalA, Format);
    const bool BNaN = UsesRight && isNaN(OriginalB, Format);
    const bool CNaN = IsFma && isNaN(OriginalC, Format);
    if (ANaN || BNaN || CNaN) {
      if (isSignalingNaN(OriginalA, Format) ||
          (UsesRight && isSignalingNaN(OriginalB, Format)) ||
          (IsFma && isSignalingNaN(OriginalC, Format)))
        PreRaised |= 1U << 0;
      if (IsMinMax) {
        // MIN/MAX select the second source for every unordered pair.  Intel
        // also requires an SNaN in that source to be forwarded bit-for-bit;
        // the invalid exception is raised above without quieting the result.
        writeLane(Result, Offset, ElementSize, OriginalB);
      } else {
        const uint64_t Selected =
            ANaN ? OriginalA : (BNaN ? OriginalB : OriginalC);
        writeLane(Result, Offset, ElementSize, Selected | Format.Quiet);
      }
      continue;
    }

    uint64_t A = OriginalA;
    uint64_t B = OriginalB;
    uint64_t C = OriginalC;
    const bool ADenormal = isDenormal(A, Format);
    const bool BDenormal = UsesRight && isDenormal(B, Format);
    const bool CDenormal = IsFma && isDenormal(C, Format);
    if (DAZ) {
      if (ADenormal)
        A &= Format.Sign;
      if (BDenormal)
        B &= Format.Sign;
      if (CDenormal)
        C &= Format.Sign;
    } else if (ADenormal || BDenormal || CDenormal) {
      PreRaised |= 1U << 1;
    }

    if (Kind == X86FPArithKind::Add || Kind == X86FPArithKind::Subtract) {
      const uint64_t ArithmeticB =
          Kind == X86FPArithKind::Subtract ? B ^ Format.Sign : B;
      if (isInfinity(A, Format) && isInfinity(ArithmeticB, Format) &&
          ((A ^ ArithmeticB) & Format.Sign) != 0) {
        PreRaised |= 1U << 0;
        writeLane(Result, Offset, ElementSize, Format.Indefinite);
        continue;
      }
    } else if (Kind == X86FPArithKind::Multiply) {
      if ((isZero(A, Format) && isInfinity(B, Format)) ||
          (isInfinity(A, Format) && isZero(B, Format))) {
        PreRaised |= 1U << 0;
        writeLane(Result, Offset, ElementSize, Format.Indefinite);
        continue;
      }
    } else if (Kind == X86FPArithKind::Divide) {
      if ((isZero(A, Format) && isZero(B, Format)) ||
          (isInfinity(A, Format) && isInfinity(B, Format))) {
        PreRaised |= 1U << 0;
        writeLane(Result, Offset, ElementSize, Format.Indefinite);
        continue;
      }
      if (!isInfinity(A, Format) && !isZero(A, Format) && isZero(B, Format)) {
        PreRaised |= 1U << 2;
        writeLane(Result, Offset, ElementSize,
                  Format.Exponent | ((A ^ B) & Format.Sign));
        continue;
      }
    } else if (Kind == X86FPArithKind::FusedMultiplyAdd) {
      const bool LaneSubtractAddend = AlternatingAddend
                                          ? (((Lane & 1U) == 0) == SubtractEven)
                                          : SubtractAddend;
      uint64_t ArithmeticA = NegateProduct ? A ^ Format.Sign : A;
      uint64_t ArithmeticC = LaneSubtractAddend ? C ^ Format.Sign : C;
      if ((isZero(ArithmeticA, Format) && isInfinity(B, Format)) ||
          (isInfinity(ArithmeticA, Format) && isZero(B, Format))) {
        PreRaised |= 1U << 0;
        writeLane(Result, Offset, ElementSize, Format.Indefinite);
        continue;
      }
      if ((isInfinity(ArithmeticA, Format) || isInfinity(B, Format)) &&
          isInfinity(ArithmeticC, Format)) {
        const uint64_t ProductSign = (ArithmeticA ^ B) & Format.Sign;
        if (ProductSign != (ArithmeticC & Format.Sign)) {
          PreRaised |= 1U << 0;
          writeLane(Result, Offset, ElementSize, Format.Indefinite);
          continue;
        }
      }
      A = ArithmeticA;
      C = ArithmeticC;
    } else if (Kind == X86FPArithKind::SquareRoot && (A & Format.Sign) != 0 &&
               !isZero(A, Format)) {
      PreRaised |= 1U << 0;
      writeLane(Result, Offset, ElementSize, Format.Indefinite);
      continue;
    }

    AValues[Lane] = A;
    BValues[Lane] = B;
    CValues[Lane] = C;
    NeedsArithmetic[Lane] = 1;
  }

  if (!SuppressExceptions) {
    MXCSR |= PreRaised;
    if (hasUnmaskedException(MXCSR, PreRaised))
      return false;
  }

  uint32_t PostRaised = 0;
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    if (!NeedsArithmetic[Lane])
      continue;
    uint32_t Raised = 0;
    uint64_t Bits = 0;
    if (Kind == X86FPArithKind::SquareRoot) {
      ExactSqrtResult Sqrt;
      if (!exactPositiveSquareRoot(AValues[Lane], ElementSize, APFRounding,
                                   Sqrt))
        return false;
      Bits = Sqrt.Bits;
      if (Sqrt.Inexact)
        Raised |= 1U << 5;
    } else if (Kind == X86FPArithKind::Minimum ||
               Kind == X86FPArithKind::Maximum) {
      const uint64_t A = AValues[Lane];
      const uint64_t B = BValues[Lane];
      if (isZero(A, Format) && isZero(B, Format)) {
        Bits = B;
      } else {
        const llvm::APFloat Left(Semantics, llvm::APInt(ElementSize * 8, A));
        const llvm::APFloat Right(Semantics, llvm::APInt(ElementSize * 8, B));
        const llvm::APFloat::cmpResult Comparison = Left.compare(Right);
        const bool SelectLeft =
            Kind == X86FPArithKind::Minimum
                ? Comparison == llvm::APFloat::cmpLessThan
                : Comparison == llvm::APFloat::cmpGreaterThan;
        Bits = SelectLeft ? A : B;
      }
    } else {
      llvm::APFloat Value(Semantics,
                          llvm::APInt(ElementSize * 8, AValues[Lane]));
      const llvm::APFloat Right(Semantics,
                                llvm::APInt(ElementSize * 8, BValues[Lane]));
      llvm::APFloat::opStatus Status = llvm::APFloat::opOK;
      switch (Kind) {
      case X86FPArithKind::Add:
        Status = Value.add(Right, APFRounding);
        break;
      case X86FPArithKind::Subtract:
        Status = Value.subtract(Right, APFRounding);
        break;
      case X86FPArithKind::Multiply:
        Status = Value.multiply(Right, APFRounding);
        break;
      case X86FPArithKind::Divide:
        Status = Value.divide(Right, APFRounding);
        break;
      case X86FPArithKind::FusedMultiplyAdd: {
        const llvm::APFloat Addend(Semantics,
                                   llvm::APInt(ElementSize * 8, CValues[Lane]));
        Status = Value.fusedMultiplyAdd(Right, Addend, APFRounding);
        break;
      }
      case X86FPArithKind::SquareRoot:
      case X86FPArithKind::Minimum:
      case X86FPArithKind::Maximum:
        return false;
      }
      Raised = statusFlags(Status);
      Bits = Value.bitcastToAPInt().getZExtValue();
    }
    if (isDenormal(Bits, Format) && !UnderflowMasked) {
      Raised |= 1U << 4;
    } else if (isDenormal(Bits, Format) && FTZ) {
      Bits &= Format.Sign;
      Raised |= (1U << 4) | (1U << 5);
    }
    writeLane(Result, static_cast<size_t>(Lane) * ElementSize, ElementSize,
              Bits);
    PostRaised |= Raised;
  }

  if (!SuppressExceptions) {
    MXCSR |= PostRaised;
    if (hasUnmaskedException(MXCSR, PostRaised))
      return false;
  }
  writeOutputBytes(Op.Output, Result);
  return true;
}

} // namespace neverd
