//===- NdOpEmulatorX86FPRoundTransform.cpp - Exact x86 FP transforms ----===//

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
};

FloatFormat formatForSize(uint16_t Size) {
  if (Size == 4)
    return {UINT64_C(0x80000000), UINT64_C(0x7f800000),
            UINT64_C(0x007fffff)};
  return {UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000000),
          UINT64_C(0x000fffffffffffff)};
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

bool isDenormal(uint64_t Bits, const FloatFormat &Format) {
  return (Bits & Format.Exponent) == 0 && (Bits & Format.Fraction) != 0;
}

llvm::APFloat::roundingMode immediateRoundingMode(uint8_t Immediate,
                                                  uint32_t MXCSR) {
  const unsigned Mode = (Immediate & 4U) ? ((MXCSR >> 13) & 3U)
                                         : (Immediate & 3U);
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

bool collectStatus(llvm::APFloat::opStatus Status, uint32_t &Raised) {
  const unsigned Raw = static_cast<unsigned>(Status);
  const unsigned Supported = llvm::APFloat::opInvalidOp |
                             llvm::APFloat::opInexact;
  if ((Raw & ~Supported) != 0)
    return false;
  if ((Raw & llvm::APFloat::opInvalidOp) != 0)
    Raised |= 1U << 0;
  if ((Raw & llvm::APFloat::opInexact) != 0)
    Raised |= 1U << 5;
  return true;
}

bool hasUnmaskedException(uint32_t MXCSR, uint32_t Raised) {
  return (Raised & ~(MXCSR >> 7) & 0x3fU) != 0;
}

/// Round Input at the binary point selected by Scale.  Intel specifies the
/// input multiplication as having an unlimited exponent range.  A finite
/// value whose exponent already leaves no fractional significand bits at that
/// point is therefore returned directly; every remaining value can safely be
/// scaled in its native APFloat format without artificial overflow.
bool roundAtScale(const llvm::APFloat &Input, unsigned Scale,
                  llvm::APFloat::roundingMode Rounding,
                  llvm::APFloat &Rounded, uint32_t &Raised) {
  Rounded = Input;
  if (Input.isFiniteNonZero() &&
      llvm::ilogb(Input) + static_cast<int>(Scale) + 1 >=
          static_cast<int>(
              llvm::APFloat::semanticsPrecision(Input.getSemantics())))
    return true;

  if (Input.isFiniteNonZero())
    Rounded = llvm::scalbn(Input, static_cast<int>(Scale),
                           llvm::APFloat::rmNearestTiesToEven);
  const llvm::APFloat::opStatus Status = Rounded.roundToIntegral(Rounding);
  if (!collectStatus(Status, Raised))
    return false;
  if (Input.isFiniteNonZero())
    Rounded = llvm::scalbn(Rounded, -static_cast<int>(Scale),
                           llvm::APFloat::rmNearestTiesToEven);

  // The architectural precision condition is stated directly as source !=
  // destination.  Preserve APFloat's status and also enforce that definition
  // explicitly so implementation details of roundToIntegral cannot weaken it.
  if (Input.isFinite() && !Input.isNaN() &&
      Input.compare(Rounded) != llvm::APFloat::cmpEqual)
    Raised |= 1U << 5;
  return true;
}

} // namespace

bool NdOpEmulator::executeX86FPRoundTransform(const LowOp &Op) {
  if (Op.NumInputs != 5 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 1 ||
      !Op.Inputs[3].isConst() || Op.Inputs[3].Size != 1 ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) ||
      (Op.Output.Size != 16 && Op.Output.Size != 32 && Op.Output.Size != 64) ||
      Op.Inputs[2].Size != Op.Output.Size)
    return false;

  const uint8_t Control = static_cast<uint8_t>(readOperand(Op.Inputs[1]));
  if (!isValidX86FPRoundTransformControl(Control))
    return false;
  const auto Kind = static_cast<X86FPRoundTransformKind>(Control & 1U);
  const bool IsF64 = (Control & (UINT8_C(1) << 1)) != 0;
  const bool Scalar = (Control & (UINT8_C(1) << 2)) != 0;
  const bool SuppressExceptions = (Control & (UINT8_C(1) << 3)) != 0;
  const uint16_t ElementSize = IsF64 ? 8 : 4;
  const unsigned LaneCount = Scalar ? 1 : Op.Output.Size / ElementSize;
  const uint16_t MaskSize = static_cast<uint16_t>((LaneCount + 7U) / 8U);
  if ((Scalar && Op.Output.Size != 16) || Op.Output.Size % ElementSize != 0 ||
      Op.Inputs[4].Size != MaskSize)
    return false;

  const uint8_t Immediate = static_cast<uint8_t>(readOperand(Op.Inputs[3]));
  const unsigned Scale = Immediate >> 4;
  const llvm::APFloat::roundingMode Rounding =
      immediateRoundingMode(Immediate, MXCSR);
  const uint64_t ActiveMask = readOperand(Op.Inputs[4]);
  const std::vector<uint8_t> Source = readOperandBytes(Op.Inputs[2]);
  if (Source.size() != Op.Output.Size)
    return false;

  const llvm::fltSemantics &Semantics =
      IsF64 ? llvm::APFloat::IEEEdouble() : llvm::APFloat::IEEEsingle();
  const FloatFormat Format = formatForSize(ElementSize);
  const bool DAZ = (MXCSR & (1U << 6)) != 0;
  std::vector<uint8_t> Result(Op.Output.Size, 0);
  uint32_t Raised = 0;

  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    if (((ActiveMask >> Lane) & 1U) == 0)
      continue;
    const size_t Offset = static_cast<size_t>(Lane) * ElementSize;
    uint64_t Bits = readLane(Source, Offset, ElementSize);
    if (DAZ && isDenormal(Bits, Format))
      Bits &= Format.Sign;

    const llvm::APFloat Input(Semantics,
                              llvm::APInt(ElementSize * 8, Bits));
    llvm::APFloat Rounded = Input;
    if (!roundAtScale(Input, Scale, Rounding, Rounded, Raised))
      return false;

    uint64_t ResultBits = Rounded.bitcastToAPInt().getZExtValue();
    if (Kind == X86FPRoundTransformKind::Reduce && !Input.isNaN()) {
      if (Input.isInfinity()) {
        Raised |= 1U << 0;
        ResultBits = 0;
      } else {
        llvm::APFloat Reduced = Input;
        if (!collectStatus(Reduced.subtract(Rounded, Rounding), Raised))
          return false;
        ResultBits = Reduced.bitcastToAPInt().getZExtValue();
      }
    }
    writeLane(Result, Offset, ElementSize, ResultBits);
  }

  // imm8.SPE suppresses only precision.  EVEX SAE suppresses every status
  // update and exception, including invalid from signaling NaNs or infinity
  // reduction.  Destination writeback remains atomic for unmasked faults.
  if ((Immediate & 8U) != 0)
    Raised &= ~(1U << 5);
  if (!SuppressExceptions) {
    MXCSR |= Raised;
    if (hasUnmaskedException(MXCSR, Raised))
      return false;
  }
  writeOutputBytes(Op.Output, Result);
  return true;
}

} // namespace neverd
