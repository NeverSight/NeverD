//===- NdOpEmulatorX86FPArith.cpp - Exact x86 SIMD FP arithmetic --------===//

#include "neverd/Limits.h"
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

llvm::APFloat::opStatus
evaluateArithmetic(X86FPArithKind Kind, llvm::APFloat &Value,
                   const llvm::APFloat &Right, const llvm::APFloat &Addend,
                   llvm::APFloat::roundingMode Rounding) {
  switch (Kind) {
  case X86FPArithKind::Add:
    return Value.add(Right, Rounding);
  case X86FPArithKind::Subtract:
    return Value.subtract(Right, Rounding);
  case X86FPArithKind::Multiply:
    return Value.multiply(Right, Rounding);
  case X86FPArithKind::Divide:
    return Value.divide(Right, Rounding);
  case X86FPArithKind::FusedMultiplyAdd:
    return Value.fusedMultiplyAdd(Right, Addend, Rounding);
  default:
    llvm_unreachable("not a binary or fused FP operation");
  }
}

/// APFloat reports only inexact when a directed overflow saturates at the
/// largest finite value. x86 raises overflow as well. Detect it by rounding
/// at the same precision with an extended exponent range: merely testing for
/// an inexact largest result would misclassify additions below the threshold.
bool directedOverflow(X86FPArithKind Kind, const llvm::APFloat &Left,
                      const llvm::APFloat &Right, const llvm::APFloat &Addend,
                      llvm::APFloat::roundingMode Rounding) {
  llvm::fltSemantics Extended = Left.getSemantics();
  const auto MaximumExponent = Extended.maxExponent;
  Extended.maxExponent += 4096;
  Extended.minExponent -= 4096;
  auto Widen = [&](llvm::APFloat Value) {
    bool LosesInfo = false;
    Value.convert(Extended, llvm::APFloat::rmNearestTiesToEven, &LosesInfo);
    assert(!LosesInfo && "widening the exponent range must be exact");
    return Value;
  };
  auto Value = Widen(Left);
  const auto Rhs = Widen(Right);
  const auto Third = Widen(Addend);
  evaluateArithmetic(Kind, Value, Rhs, Third, Rounding);
  return Value.isFiniteNonZero() && llvm::ilogb(Value) > MaximumExponent;
}

/// x86 detects tininess after rounding at full significand precision, before
/// denormalization reduces the precision. A second rounding can produce the
/// smallest normal value while the instruction still raises underflow.
bool tinyBeforeDenormalization(X86FPArithKind Kind, const llvm::APFloat &Left,
                               const llvm::APFloat &Right,
                               const llvm::APFloat &Addend,
                               llvm::APFloat::roundingMode Rounding) {
  llvm::fltSemantics Extended = Left.getSemantics();
  const auto MinimumExponent = Extended.minExponent;
  Extended.maxExponent += 4096;
  Extended.minExponent -= 4096;
  auto Widen = [&](llvm::APFloat Value) {
    bool LosesInfo = false;
    Value.convert(Extended, llvm::APFloat::rmNearestTiesToEven, &LosesInfo);
    assert(!LosesInfo && "widening the exponent range must be exact");
    return Value;
  };
  auto Value = Widen(Left);
  const auto Rhs = Widen(Right);
  const auto Third = Widen(Addend);
  evaluateArithmetic(Kind, Value, Rhs, Third, Rounding);
  return Value.isFiniteNonZero() && llvm::ilogb(Value) < MinimumExponent;
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
  if (!MXCSRKnown)
    return false;
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
      const bool HasSignalingNaN =
          isSignalingNaN(OriginalA, Format) ||
          (UsesRight && isSignalingNaN(OriginalB, Format)) ||
          (IsFma && isSignalingNaN(OriginalC, Format));
      if (IsMinMax || HasSignalingNaN)
        PreRaised |= 1U << 0;
      if (IsMinMax) {
        // MIN/MAX select the second source for every unordered pair.  Intel
        // raises invalid even for a QNaN, but still forwards a second-source
        // SNaN bit-for-bit rather than quieting the selected result.
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
    } else if ((ADenormal || BDenormal || CDenormal) &&
               !(Kind == X86FPArithKind::Divide && !isInfinity(A, Format) &&
                 !isZero(A, Format) && isZero(B, Format))) {
      // A finite nonzero dividend divided by zero raises #Z, including a
      // denormal dividend. Do not add #D for that lane; other active lanes
      // still contribute their independent denormal-input exception.
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
      const llvm::APFloat Addend(Semantics,
                                 llvm::APInt(ElementSize * 8, CValues[Lane]));
      const auto Left = Value;
      const auto Status =
          evaluateArithmetic(Kind, Value, Right, Addend, APFRounding);
      Raised = statusFlags(Status);
      if ((Raised & (1U << 5)) && !(Raised & (1U << 3)) && Value.isLargest() &&
          directedOverflow(Kind, Left, Right, Addend, APFRounding))
        Raised |= 1U << 3;
      Bits = Value.bitcastToAPInt().getZExtValue();
      const uint64_t MinimumNormal = ElementSize == 4
                                         ? UINT64_C(0x00800000)
                                         : UINT64_C(0x0010000000000000);
      if ((Raised & (1U << 5)) && !(Raised & (1U << 4)) &&
          (Bits & ~Format.Sign) == MinimumNormal &&
          tinyBeforeDenormalization(Kind, Left, Right, Addend, APFRounding))
        Raised |= 1U << 4;
    }
    if (isDenormal(Bits, Format) && !UnderflowMasked) {
      Raised |= 1U << 4;
    } else if (FTZ && UnderflowMasked &&
               (isDenormal(Bits, Format) || (Raised & (1U << 4)))) {
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

bool NdOpEmulator::executeX86ScalarFPState(const LowOp &Op) {
  if (Op.NumInputs == 0 || !Op.Inputs[0].isConst())
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (!x86FPStateShapeIsValid(Id, x86FPStateLowShape(Op, Img.Arch)))
    return false;
  for (unsigned Index = 1; Index < Op.NumInputs; ++Index)
    if (!Op.Inputs[Index].isConst() && !getRegister(Op.Inputs[Index].Offset))
      return false;
  if (Id == Intrinsic::X86ReadMXCSR) {
    if (!MXCSRKnown)
      return false;
    writeOutput(Op.Output, MXCSR);
    return true;
  }
  if (Id == Intrinsic::X86WriteMXCSR) {
    const auto State = readOperand(Op.Inputs[1]);
    if ((State & ~UINT64_C(0xffff)) != 0)
      return false;
    setMXCSR(static_cast<uint32_t>(State));
    return true;
  }
  if (isX86FPConversionStateIntrinsic(Id)) {
    const unsigned SourceBytes = Op.Inputs[1].Size;
    const unsigned DestinationBytes =
        static_cast<unsigned>(Op.Inputs[3].Offset);
    const auto State = readOperand(Op.Inputs[2]);
    if ((State & ~UINT64_C(0xffff)) != 0)
      return false;
    NdOpEmulator Evaluation(Img);
    Evaluation.setStrictMode(true);
    Evaluation.setMXCSR(static_cast<uint32_t>(State));
    auto Source = readOperandBytes(Op.Inputs[1]);
    Source.resize(16, 0);
    Evaluation.writeOutputBytes(NdVar::tmp(1, 16), Source);
    const bool Truncate = Id == Intrinsic::X86FPTruncToIntState;
    LowOp Scalar;
    Scalar.Opcode = NdOp::INTRINSIC;
    Scalar.Output = NdVar::tmp(0, 16);
    Scalar.addInput(
        NdVar::cst(static_cast<unsigned>(Intrinsic::X86FPConvert), 2));
    Scalar.addInput(NdVar::cst(
        makeX86FPConvertControl(
            X86FPConvertKind::FloatToSignedInteger, SourceBytes == 8,
            DestinationBytes == 8, Truncate, false,
            Truncate ? X86FPRounding::TowardZero : X86FPRounding::MXCSR, 1),
        2));
    Scalar.addInput(NdVar::tmp(1, 16));
    Scalar.addInput(NdVar::cst(1, 1));
    const bool Complete = Evaluation.executeX86FPConvert(Scalar);
    setMXCSR(Evaluation.getMXCSR());
    if (!Complete)
      return false;
    auto Result = Evaluation.readOperandBytes(Scalar.Output);
    Result.resize(DestinationBytes + 4);
    for (unsigned Index = 0; Index < 4; ++Index)
      Result[DestinationBytes + Index] =
          static_cast<uint8_t>(MXCSR >> (Index * 8));
    writeOutputBytes(Op.Output, Result);
    return true;
  }
  if (isX86FPRoundStateIntrinsic(Id)) {
    const bool Memory = Id == Intrinsic::X86FPRoundMemoryState;
    const unsigned Bytes = Memory ? Op.Output.Size - 4 : Op.Inputs[2].Size;
    const unsigned Control = Op.Inputs[Memory ? 2 : 1].Offset;
    const auto State = readOperand(Op.Inputs[4]);
    if ((State & ~UINT64_C(0xffff)) != 0)
      return false;
    NdOpEmulator Evaluation(Img);
    Evaluation.setStrictMode(true);
    Evaluation.setMXCSR(static_cast<uint32_t>(State));
    std::vector<uint8_t> Source;
    if (Memory) {
      // An unknown architectural address context is a refusal, not a fault
      // prediction based on the image or a permissive external memory reader.
      if ((Img.Arch == Arch::X64 && !X86LinearAddressBits) ||
          (Img.Arch != Arch::X86 && Img.Arch != Arch::X64) ||
          (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default &&
           !MemoryAddressSpaceBases.contains(Op.MemoryAddressSpace)))
        return false;
      // Match the native scope on memory faults: the authenticated incoming
      // CSR is installed before the instruction accesses its source.
      setMXCSR(static_cast<uint32_t>(State));
      const uint64_t Offset = Img.Arch == Arch::X86
                                  ? uint32_t(readOperand(Op.Inputs[1]))
                                  : readOperand(Op.Inputs[1]);
      const auto Address = resolveMemoryAddress(Op, Offset);
      if (!Address || !isX86CanonicalMemoryRange(*Address, Bytes) ||
          (!x86FPRoundStateIsScalar(Control) && !(Control & 8) &&
           (*Address & 15)))
        return false;
      // A write-back cache supplies values, never source-access permission.
      // Check every mapped byte before allowing cached data to satisfy a load.
      for (unsigned Index = 0; Index < Bytes; ++Index) {
        const uint64_t ByteAddress = *Address + Index;
        const Segment *Mapped = Img.getSegmentFor(ByteAddress);
        if (!Mapped || !Mapped->isReadable() || ByteAddress < Mapped->VA ||
            ByteAddress - Mapped->VA >= Mapped->Size)
          return false;
      }
      const auto Loaded = loadMemoryBytes(*Address, Bytes);
      if (!Loaded)
        return false;
      Source = *Loaded;
      if (CollectLoads &&
          static_cast<int>(LoadLog.size()) < limits::kMaxLoadRecords)
        LoadLog.push_back({*Address, static_cast<uint16_t>(Bytes)});
    } else
      Source = readOperandBytes(Op.Inputs[2]);
    const unsigned VectorBytes = std::max(Bytes, 16U);
    Source.resize(VectorBytes, 0);
    Evaluation.writeOutputBytes(NdVar::tmp(1, VectorBytes), Source);
    LowOp Round;
    Round.Opcode = NdOp::INTRINSIC;
    Round.Output = NdVar::tmp(0, VectorBytes);
    Round.addInput(
        NdVar::cst(static_cast<unsigned>(Intrinsic::X86FPRoundTransform), 2));
    // Memory bit 3 selects VEX packed addressing; it is not EVEX SAE.
    Round.addInput(NdVar::cst(Control & 6, 1));
    Round.addInput(NdVar::tmp(1, VectorBytes));
    // Legacy/VEX ROUND ignores bits 7:4; they are not a RNDSCALE scale.
    Round.addInput(NdVar::cst(Op.Inputs[3].Offset & 15, 1));
    Round.addInput(NdVar::cst(0xff, 1));
    const bool Complete = Evaluation.executeX86FPRoundTransform(Round);
    setMXCSR(Evaluation.getMXCSR());
    if (!Complete)
      return false;
    auto Result = Evaluation.readOperandBytes(Round.Output);
    Result.resize(Bytes + 4);
    for (unsigned Index = 0; Index < 4; ++Index)
      Result[Bytes + Index] = static_cast<uint8_t>(MXCSR >> (Index * 8));
    writeOutputBytes(Op.Output, Result);
    return true;
  }
  const unsigned Bytes = Op.Inputs[1].Size;
  const auto State = readOperand(Op.Inputs[3]);
  if ((State & ~UINT64_C(0xffff)) != 0)
    return false;
  // Reuse the authoritative packed evaluator with a single active lane.
  // Scratch values live in a separate evaluator, never in the caller's
  // temporary/register namespace. Preserve its exception evidence on refusal.
  NdOpEmulator Evaluation(Img);
  Evaluation.setStrictMode(true);
  Evaluation.setMXCSR(static_cast<uint32_t>(State));
  std::vector<uint8_t> Left = readOperandBytes(Op.Inputs[1]);
  std::vector<uint8_t> Right = readOperandBytes(Op.Inputs[2]);
  Left.resize(16, 0);
  Right.resize(16, 0);
  Evaluation.writeOutputBytes(NdVar::tmp(1, 16), Left);
  Evaluation.writeOutputBytes(NdVar::tmp(2, 16), Right);
  Evaluation.writeOutputBytes(NdVar::tmp(3, 16), std::vector<uint8_t>(16, 0));
  LowOp Scalar;
  Scalar.Opcode = NdOp::INTRINSIC;
  Scalar.Output = NdVar::tmp(0, 16);
  Scalar.addInput(NdVar::cst(static_cast<unsigned>(Intrinsic::X86FPArith), 2));
  Scalar.addInput(
      NdVar::cst(makeX86FPArithControl(x86ScalarFPStateKind(Id), Bytes == 8,
                                       true, false, X86FPRounding::MXCSR),
                 2));
  Scalar.addInput(NdVar::tmp(1, 16));
  Scalar.addInput(NdVar::tmp(2, 16));
  Scalar.addInput(NdVar::tmp(3, 16));
  Scalar.addInput(NdVar::cst(1, 1));
  const bool Complete = Evaluation.executeX86FPArith(Scalar);
  setMXCSR(Evaluation.getMXCSR());
  if (!Complete)
    return false;
  auto Result = Evaluation.readOperandBytes(Scalar.Output);
  Result.resize(Bytes + 4);
  for (unsigned Index = 0; Index < 4; ++Index)
    Result[Bytes + Index] = static_cast<uint8_t>(MXCSR >> (8 * Index));
  writeOutputBytes(Op.Output, Result);
  return true;
}

} // namespace neverd
