//===- NdOpEmulatorX86FPScale.cpp - Exact x86 scale operation -----------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APSInt.h"

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
    return {UINT64_C(0x80000000), UINT64_C(0x7f800000),
            UINT64_C(0x007fffff), UINT64_C(0x00400000),
            UINT64_C(0xffc00000)};
  return {UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000000),
          UINT64_C(0x000fffffffffffff), UINT64_C(0x0008000000000000),
          UINT64_C(0xfff8000000000000)};
}

uint64_t readLane(llvm::ArrayRef<uint8_t> Bytes, size_t Offset,
                  uint16_t Size) {
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

bool hasUnmaskedException(uint32_t MXCSR, uint32_t Raised) {
  return (Raised & ~(MXCSR >> 7) & 0x3fU) != 0;
}

int floorScaleExponent(uint64_t Bits, uint16_t ElementSize,
                       const llvm::fltSemantics &Semantics,
                       const FloatFormat &Format) {
  const llvm::APFloat Value(Semantics,
                            llvm::APInt(ElementSize * 8, Bits));
  llvm::APSInt Integer(32, false);
  bool Exact = false;
  const llvm::APFloat::opStatus Status = Value.convertToInteger(
      Integer, llvm::APFloat::rmTowardNegative, &Exact);
  if ((static_cast<unsigned>(Status) & llvm::APFloat::opInvalidOp) != 0)
    return (Bits & Format.Sign) != 0 ? -32768 : 32768;
  return static_cast<int>(Integer.getSExtValue());
}

} // namespace

bool NdOpEmulator::executeX86FPScale(const LowOp &Op) {
  if (Op.NumInputs != 5 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 1 ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) ||
      (Op.Output.Size != 16 && Op.Output.Size != 32 && Op.Output.Size != 64) ||
      Op.Inputs[2].Size != Op.Output.Size ||
      Op.Inputs[3].Size != Op.Output.Size)
    return false;
  const uint8_t Control = static_cast<uint8_t>(readOperand(Op.Inputs[1]));
  if (!isValidX86FPScaleControl(Control))
    return false;
  const bool IsF64 = (Control & 1U) != 0;
  const bool Scalar = (Control & 2U) != 0;
  const bool SuppressExceptions = (Control & 4U) != 0;
  const auto Rounding = static_cast<X86FPRounding>((Control >> 3) & 7U);
  const uint16_t ElementSize = IsF64 ? 8 : 4;
  const unsigned LaneCount = Scalar ? 1 : Op.Output.Size / ElementSize;
  const uint16_t MaskSize = static_cast<uint16_t>((LaneCount + 7U) / 8U);
  if ((Scalar && Op.Output.Size != 16) || Op.Output.Size % ElementSize != 0 ||
      Op.Inputs[4].Size != MaskSize)
    return false;

  const uint64_t ActiveMask = readOperand(Op.Inputs[4]);
  const std::vector<uint8_t> LeftBytes = readOperandBytes(Op.Inputs[2]);
  const std::vector<uint8_t> RightBytes = readOperandBytes(Op.Inputs[3]);
  if (LeftBytes.size() != Op.Output.Size ||
      RightBytes.size() != Op.Output.Size)
    return false;

  constexpr uint32_t Invalid = 1U << 0;
  constexpr uint32_t Denormal = 1U << 1;
  constexpr uint32_t Overflow = 1U << 3;
  constexpr uint32_t Underflow = 1U << 4;
  constexpr uint32_t Precision = 1U << 5;
  const FloatFormat Format = formatForSize(ElementSize);
  const llvm::fltSemantics &Semantics =
      IsF64 ? llvm::APFloat::IEEEdouble() : llvm::APFloat::IEEEsingle();
  const llvm::APFloat::roundingMode APFRounding = roundingMode(Rounding, MXCSR);
  const bool DAZ = (MXCSR & (1U << 6)) != 0;
  const bool UnderflowMasked =
      SuppressExceptions || (MXCSR & (1U << 11)) != 0;
  const bool FTZ = (MXCSR & (1U << 15)) != 0;

  std::vector<uint8_t> Result(Op.Output.Size, 0);
  std::vector<uint64_t> LeftValues(LaneCount), RightValues(LaneCount);
  std::vector<uint8_t> NeedsScale(LaneCount, 0);
  uint32_t PreRaised = 0;
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    if (((ActiveMask >> Lane) & 1U) == 0)
      continue;
    const size_t Offset = static_cast<size_t>(Lane) * ElementSize;
    const uint64_t OriginalLeft = readLane(LeftBytes, Offset, ElementSize);
    const uint64_t OriginalRight = readLane(RightBytes, Offset, ElementSize);
    const bool LeftDenormal = isDenormal(OriginalLeft, Format);
    const bool RightDenormal = isDenormal(OriginalRight, Format);
    if (LeftDenormal && !DAZ)
      PreRaised |= Denormal;
    uint64_t Left = OriginalLeft;
    uint64_t Right = OriginalRight;
    if (DAZ) {
      if (LeftDenormal)
        Left &= Format.Sign;
      if (RightDenormal)
        Right &= Format.Sign;
    }

    const bool LeftNaN = isNaN(Left, Format);
    const bool RightNaN = isNaN(Right, Format);
    const bool LeftSignaling = isSignalingNaN(Left, Format);
    const bool RightSignaling = isSignalingNaN(Right, Format);
    if (LeftSignaling || RightSignaling)
      PreRaised |= Invalid;
    if (LeftSignaling) {
      writeLane(Result, Offset, ElementSize, Left | Format.Quiet);
      continue;
    }
    if (RightNaN) {
      writeLane(Result, Offset, ElementSize,
                LeftNaN ? Left | Format.Quiet : Right | Format.Quiet);
      continue;
    }
    if (LeftNaN) {
      if (isInfinity(Right, Format))
        writeLane(Result, Offset, ElementSize,
                  (Right & Format.Sign) != 0 ? 0 : Format.Exponent);
      else
        writeLane(Result, Offset, ElementSize, Left | Format.Quiet);
      continue;
    }

    const bool LeftInfinity = isInfinity(Left, Format);
    const bool LeftZero = isZero(Left, Format);
    if (isInfinity(Right, Format)) {
      const bool NegativeScale = (Right & Format.Sign) != 0;
      if ((!NegativeScale && LeftZero) ||
          (NegativeScale && LeftInfinity)) {
        PreRaised |= Invalid;
        writeLane(Result, Offset, ElementSize, Format.Indefinite);
      } else if (NegativeScale) {
        writeLane(Result, Offset, ElementSize,
                  LeftZero ? Left : Left & Format.Sign);
      } else {
        writeLane(Result, Offset, ElementSize,
                  LeftInfinity ? Left
                               : (Left & Format.Sign) | Format.Exponent);
      }
      continue;
    }
    if (LeftInfinity || LeftZero) {
      writeLane(Result, Offset, ElementSize, Left);
      continue;
    }
    LeftValues[Lane] = Left;
    RightValues[Lane] = Right;
    NeedsScale[Lane] = 1;
  }

  if (!SuppressExceptions) {
    MXCSR |= PreRaised;
    if (hasUnmaskedException(MXCSR, PreRaised))
      return false;
  }

  uint32_t PostRaised = 0;
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    if (!NeedsScale[Lane])
      continue;
    const int Scale = floorScaleExponent(RightValues[Lane], ElementSize,
                                         Semantics, Format);
    const llvm::APFloat Original(
        Semantics, llvm::APInt(ElementSize * 8, LeftValues[Lane]));
    llvm::APFloat Scaled = llvm::scalbn(Original, Scale, APFRounding);
    uint64_t Bits = Scaled.bitcastToAPInt().getZExtValue();
    uint32_t Raised = 0;
    bool Exact = false;
    if (!isInfinity(Bits, Format)) {
      const llvm::APFloat Reversed = llvm::scalbn(
          Scaled, -Scale, llvm::APFloat::rmNearestTiesToEven);
      Exact = Reversed.bitcastToAPInt().getZExtValue() == LeftValues[Lane];
    }
    if (!Exact) {
      if (isZero(Bits, Format) || isDenormal(Bits, Format))
        Raised |= Underflow | Precision;
      else
        Raised |= Overflow | Precision;
    }
    if ((Raised & Underflow) != 0 && isDenormal(Bits, Format) && FTZ &&
        UnderflowMasked)
      Bits &= Format.Sign;
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
