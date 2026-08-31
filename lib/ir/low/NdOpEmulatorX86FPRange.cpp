//===- NdOpEmulatorX86FPRange.cpp - Exact x86 range operation -----------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"

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
};

FloatFormat formatForSize(uint16_t Size) {
  if (Size == 4)
    return {UINT64_C(0x80000000), UINT64_C(0x7f800000),
            UINT64_C(0x007fffff), UINT64_C(0x00400000)};
  return {UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000000),
          UINT64_C(0x000fffffffffffff), UINT64_C(0x0008000000000000)};
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

bool isQuietNaN(uint64_t Bits, const FloatFormat &Format) {
  return isNaN(Bits, Format) && (Bits & Format.Quiet) != 0;
}

bool isDenormal(uint64_t Bits, const FloatFormat &Format) {
  return (Bits & Format.Exponent) == 0 && (Bits & Format.Fraction) != 0;
}

bool isZero(uint64_t Bits, const FloatFormat &Format) {
  return (Bits & ~Format.Sign) == 0;
}

bool hasUnmaskedException(uint32_t MXCSR, uint32_t Raised) {
  return (Raised & ~(MXCSR >> 7) & 0x3fU) != 0;
}

uint64_t rangeLane(uint64_t LeftBits, uint64_t RightBits, uint8_t Immediate,
                   uint16_t ElementSize, const FloatFormat &Format,
                   const llvm::fltSemantics &Semantics, bool DAZ,
                   uint32_t &Raised) {
  // Signaling NaNs have strict first-source priority and bypass sign control.
  if (isSignalingNaN(LeftBits, Format)) {
    Raised |= 1U << 0;
    return LeftBits | Format.Quiet;
  }
  if (isSignalingNaN(RightBits, Format)) {
    Raised |= 1U << 0;
    return RightBits | Format.Quiet;
  }

  const bool LeftQNaN = isQuietNaN(LeftBits, Format);
  const bool RightQNaN = isQuietNaN(RightBits, Format);
  uint64_t Left = LeftBits;
  uint64_t Right = RightBits;
  if (isDenormal(Left, Format)) {
    if (DAZ)
      Left &= Format.Sign;
    else if (!RightQNaN)
      Raised |= 1U << 1;
  }
  if (isDenormal(Right, Format)) {
    if (DAZ)
      Right &= Format.Sign;
    else if (!LeftQNaN)
      Raised |= 1U << 1;
  }

  const unsigned CompareControl = Immediate & 3U;
  const unsigned SignControl = (Immediate >> 2) & 3U;
  uint64_t Selected = 0;
  if (RightQNaN) {
    Selected = Left;
  } else if (LeftQNaN) {
    Selected = Right;
  } else {
    const uint64_t LeftMagnitude = Left & ~Format.Sign;
    const uint64_t RightMagnitude = Right & ~Format.Sign;
    const bool OppositeSigns = ((Left ^ Right) & Format.Sign) != 0;
    if (isZero(Left, Format) && isZero(Right, Format) && OppositeSigns) {
      Selected = CompareControl == 0 || CompareControl == 2 ? Format.Sign : 0;
    } else if (CompareControl > 1 && LeftMagnitude == RightMagnitude &&
               OppositeSigns) {
      Selected = CompareControl == 2
                     ? ((Left & Format.Sign) != 0 ? Left : Right)
                     : ((Left & Format.Sign) == 0 ? Left : Right);
    } else if (CompareControl < 2) {
      const llvm::APFloat LeftValue(
          Semantics, llvm::APInt(ElementSize * 8, Left));
      const llvm::APFloat RightValue(
          Semantics, llvm::APInt(ElementSize * 8, Right));
      const llvm::APFloat::cmpResult Comparison =
          LeftValue.compare(RightValue);
      const bool LessOrEqual = Comparison == llvm::APFloat::cmpLessThan ||
                               Comparison == llvm::APFloat::cmpEqual;
      Selected = CompareControl == 0 ? (LessOrEqual ? Left : Right)
                                     : (LessOrEqual ? Right : Left);
    } else {
      const bool MagnitudeLessOrEqual = LeftMagnitude <= RightMagnitude;
      Selected = CompareControl == 2
                     ? (MagnitudeLessOrEqual ? Left : Right)
                     : (MagnitudeLessOrEqual ? Right : Left);
    }
  }

  const uint64_t SelectedMagnitude = Selected & ~Format.Sign;
  switch (SignControl) {
  case 0:
    return (Left & Format.Sign) | SelectedMagnitude;
  case 1:
    return Selected;
  case 2:
    return SelectedMagnitude;
  default:
    return Format.Sign | SelectedMagnitude;
  }
}

} // namespace

bool NdOpEmulator::executeX86FPRange(const LowOp &Op) {
  if (Op.NumInputs != 6 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 1 ||
      !Op.Inputs[4].isConst() || Op.Inputs[4].Size != 1 ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) ||
      (Op.Output.Size != 16 && Op.Output.Size != 32 && Op.Output.Size != 64) ||
      Op.Inputs[2].Size != Op.Output.Size ||
      Op.Inputs[3].Size != Op.Output.Size)
    return false;
  const uint8_t Control = static_cast<uint8_t>(readOperand(Op.Inputs[1]));
  const uint8_t Immediate = static_cast<uint8_t>(readOperand(Op.Inputs[4]));
  if (!isValidX86FPRangeControl(Control) || (Immediate & 0xf0U) != 0)
    return false;
  const bool IsF64 = (Control & 1U) != 0;
  const bool Scalar = (Control & 2U) != 0;
  const bool SuppressExceptions = (Control & 4U) != 0;
  const uint16_t ElementSize = IsF64 ? 8 : 4;
  const unsigned LaneCount = Scalar ? 1 : Op.Output.Size / ElementSize;
  const uint16_t MaskSize = static_cast<uint16_t>((LaneCount + 7U) / 8U);
  if ((Scalar && Op.Output.Size != 16) || Op.Output.Size % ElementSize != 0 ||
      Op.Inputs[5].Size != MaskSize)
    return false;

  const uint64_t ActiveMask = readOperand(Op.Inputs[5]);
  const std::vector<uint8_t> Left = readOperandBytes(Op.Inputs[2]);
  const std::vector<uint8_t> Right = readOperandBytes(Op.Inputs[3]);
  if (Left.size() != Op.Output.Size || Right.size() != Op.Output.Size)
    return false;
  const FloatFormat Format = formatForSize(ElementSize);
  const llvm::fltSemantics &Semantics =
      IsF64 ? llvm::APFloat::IEEEdouble() : llvm::APFloat::IEEEsingle();
  const bool DAZ = (MXCSR & (1U << 6)) != 0;
  std::vector<uint8_t> Result(Op.Output.Size, 0);
  uint32_t Raised = 0;
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    if (((ActiveMask >> Lane) & 1U) == 0)
      continue;
    const size_t Offset = static_cast<size_t>(Lane) * ElementSize;
    const uint64_t LaneResult = rangeLane(
        readLane(Left, Offset, ElementSize),
        readLane(Right, Offset, ElementSize), Immediate, ElementSize, Format,
        Semantics, DAZ, Raised);
    writeLane(Result, Offset, ElementSize, LaneResult);
  }

  if (!SuppressExceptions) {
    MXCSR |= Raised;
    if (hasUnmaskedException(MXCSR, Raised))
      return false;
  }
  writeOutputBytes(Op.Output, Result);
  return true;
}

} // namespace neverd
