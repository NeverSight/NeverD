//===- NdOpEmulatorX86FPCompare.cpp - Exact EVEX FP comparisons --------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"

#include <array>
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

uint64_t readLane(const std::vector<uint8_t> &Bytes, size_t Offset,
                  uint16_t Size) {
  uint64_t Value = 0;
  std::memcpy(&Value, Bytes.data() + Offset, Size);
  return Value;
}

bool isNaN(uint64_t Bits, const FloatFormat &Format) {
  return (Bits & Format.Exponent) == Format.Exponent &&
         (Bits & Format.Fraction) != 0;
}

bool isSignalingNaN(uint64_t Bits, const FloatFormat &Format) {
  return isNaN(Bits, Format) && (Bits & Format.Quiet) == 0;
}

bool isDenormal(uint64_t Bits, const FloatFormat &Format) {
  return (Bits & Format.Exponent) == 0 && (Bits & Format.Fraction) != 0;
}

bool hasUnmaskedException(uint32_t MXCSR, uint32_t Raised) {
  return (Raised & ~(MXCSR >> 7) & 0x3fU) != 0;
}

// Bits are ordered LT, EQ, GT, UNORD.  Keeping the architectural table
// explicit prevents predicate suffixes with identical truth values from also
// being mistaken for identical exception behavior.
constexpr std::array<uint8_t, 32> PredicateResults = {
    0x2, 0x1, 0x3, 0x8, 0xd, 0xe, 0xc, 0x7,
    0xa, 0x9, 0xb, 0x0, 0x5, 0x6, 0x4, 0xf,
    0x2, 0x1, 0x3, 0x8, 0xd, 0xe, 0xc, 0x7,
    0xa, 0x9, 0xb, 0x0, 0x5, 0x6, 0x4, 0xf,
};

constexpr std::array<bool, 32> SignalingPredicates = {
    false, true,  true,  false, false, true,  true,  false,
    false, true,  true,  false, false, true,  true,  false,
    true,  false, false, true,  true,  false, false, true,
    true,  false, false, true,  true,  false, false, true,
};

} // namespace

bool NdOpEmulator::executeX86FPCompare(const LowOp &Op) {
  if (Op.NumInputs != 6 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 1 ||
      !Op.Inputs[5].isConst() || Op.Inputs[5].Size != 1 ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) ||
      (Op.Inputs[2].Size != 16 && Op.Inputs[2].Size != 32 &&
       Op.Inputs[2].Size != 64) ||
      Op.Inputs[3].Size != Op.Inputs[2].Size)
    return false;

  const uint8_t Control = static_cast<uint8_t>(readOperand(Op.Inputs[1]));
  if (!isValidX86FPCompareControl(Control))
    return false;
  const bool IsF64 = (Control & 1U) != 0;
  const bool Scalar = (Control & 2U) != 0;
  const bool SuppressExceptions = (Control & 4U) != 0;
  const uint16_t ElementSize = IsF64 ? 8 : 4;
  const unsigned LaneCount = Scalar ? 1 : Op.Inputs[2].Size / ElementSize;
  const uint16_t MaskSize =
      static_cast<uint16_t>((LaneCount + 7U) / 8U);
  if ((Scalar && Op.Inputs[2].Size != 16) ||
      Op.Inputs[2].Size % ElementSize != 0 || Op.Output.Size != MaskSize ||
      Op.Inputs[4].Size != MaskSize)
    return false;

  const uint8_t Predicate =
      static_cast<uint8_t>(readOperand(Op.Inputs[5])) & UINT8_C(0x1f);
  const uint64_t ActiveMask = readOperand(Op.Inputs[4]);
  const std::vector<uint8_t> Left = readOperandBytes(Op.Inputs[2]);
  const std::vector<uint8_t> Right = readOperandBytes(Op.Inputs[3]);
  if (Left.size() != Op.Inputs[2].Size || Right.size() != Op.Inputs[3].Size)
    return false;

  const FloatFormat Format = formatForSize(ElementSize);
  const llvm::fltSemantics &Semantics =
      IsF64 ? llvm::APFloat::IEEEdouble() : llvm::APFloat::IEEEsingle();
  const bool DAZ = (MXCSR & (1U << 6)) != 0;
  uint64_t Result = 0;
  uint32_t Raised = 0;
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    if (((ActiveMask >> Lane) & 1U) == 0)
      continue;

    const size_t Offset = static_cast<size_t>(Lane) * ElementSize;
    uint64_t LeftBits = readLane(Left, Offset, ElementSize);
    uint64_t RightBits = readLane(Right, Offset, ElementSize);
    const bool LeftNaN = isNaN(LeftBits, Format);
    const bool RightNaN = isNaN(RightBits, Format);
    unsigned Relation = 3;
    if (LeftNaN || RightNaN) {
      if (isSignalingNaN(LeftBits, Format) ||
          isSignalingNaN(RightBits, Format) ||
          SignalingPredicates[Predicate])
        Raised |= 1U << 0;
    } else {
      const bool LeftDenormal = isDenormal(LeftBits, Format);
      const bool RightDenormal = isDenormal(RightBits, Format);
      if (DAZ) {
        if (LeftDenormal)
          LeftBits &= Format.Sign;
        if (RightDenormal)
          RightBits &= Format.Sign;
      } else if (LeftDenormal || RightDenormal) {
        Raised |= 1U << 1;
      }

      const llvm::APFloat LeftValue(
          Semantics, llvm::APInt(ElementSize * 8, LeftBits));
      const llvm::APFloat RightValue(
          Semantics, llvm::APInt(ElementSize * 8, RightBits));
      switch (LeftValue.compare(RightValue)) {
      case llvm::APFloat::cmpLessThan:
        Relation = 0;
        break;
      case llvm::APFloat::cmpEqual:
        Relation = 1;
        break;
      case llvm::APFloat::cmpGreaterThan:
        Relation = 2;
        break;
      case llvm::APFloat::cmpUnordered:
        return false;
      }
    }

    if ((PredicateResults[Predicate] & (UINT8_C(1) << Relation)) != 0)
      Result |= UINT64_C(1) << Lane;
  }

  if (!SuppressExceptions) {
    MXCSR |= Raised;
    if (hasUnmaskedException(MXCSR, Raised))
      return false;
  }
  writeOutput(Op.Output, Result);
  return true;
}

} // namespace neverd
