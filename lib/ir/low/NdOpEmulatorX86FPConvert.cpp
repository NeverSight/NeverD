//===- NdOpEmulatorX86FPConvert.cpp - Exact x86 SIMD conversions --------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APSInt.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace neverd {
namespace {

const llvm::fltSemantics &floatSemantics(uint16_t Size) {
  return Size == 8 ? llvm::APFloat::IEEEdouble() : llvm::APFloat::IEEEsingle();
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

uint64_t readLane(llvm::ArrayRef<uint8_t> Bytes, size_t Offset, uint16_t Size) {
  uint64_t Value = 0;
  std::memcpy(&Value, Bytes.data() + Offset, Size);
  return Value;
}

void writeLane(std::vector<uint8_t> &Bytes, size_t Offset, uint16_t Size,
               uint64_t Value) {
  std::memcpy(Bytes.data() + Offset, &Value, Size);
}

bool isDenormal(uint64_t Bits, uint16_t Size) {
  const uint64_t Exponent =
      Size == 4 ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
  const uint64_t Fraction =
      Size == 4 ? UINT64_C(0x007fffff) : UINT64_C(0x000fffffffffffff);
  return (Bits & Exponent) == 0 && (Bits & Fraction) != 0;
}

bool isSignalingNaN(uint64_t Bits, uint16_t Size) {
  const uint64_t Exponent =
      Size == 4 ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
  const uint64_t Fraction =
      Size == 4 ? UINT64_C(0x007fffff) : UINT64_C(0x000fffffffffffff);
  const uint64_t Quiet =
      Size == 4 ? UINT64_C(0x00400000) : UINT64_C(0x0008000000000000);
  return (Bits & Exponent) == Exponent && (Bits & Fraction) != 0 &&
         (Bits & Quiet) == 0;
}

uint64_t signMask(uint16_t Size) {
  return Size == 4 ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);
}

} // namespace

bool NdOpEmulator::executeX86FPConvert(const LowOp &Op) {
  if (Op.NumInputs != 4 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 2 ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) ||
      (Op.Output.Size != 16 && Op.Output.Size != 32 && Op.Output.Size != 64) ||
      (Op.Inputs[2].Size != 16 && Op.Inputs[2].Size != 32 &&
       Op.Inputs[2].Size != 64))
    return false;

  const uint16_t Control = static_cast<uint16_t>(readOperand(Op.Inputs[1]));
  if (!isValidX86FPConvertControl(Control))
    return false;
  const auto Kind = static_cast<X86FPConvertKind>(Control & 7U);
  const uint16_t SourceElementSize = (Control & (UINT16_C(1) << 3)) ? 8 : 4;
  const uint16_t DestinationElementSize =
      (Control & (UINT16_C(1) << 4)) ? 8 : 4;
  const bool SuppressExceptions = (Control & (UINT16_C(1) << 6)) != 0;
  const auto Rounding = static_cast<X86FPRounding>((Control >> 7) & 7U);
  const unsigned LaneCount = ((Control >> 10) & 15U) + 1;
  const uint16_t SourceSize = static_cast<uint16_t>(
      std::max<unsigned>(16, LaneCount * SourceElementSize));
  const uint16_t DestinationSize = static_cast<uint16_t>(
      std::max<unsigned>(16, LaneCount * DestinationElementSize));
  const uint16_t MaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7U) / 8U));
  if (Op.Inputs[2].Size != SourceSize || Op.Output.Size != DestinationSize ||
      Op.Inputs[3].Size != MaskSize)
    return false;

  const bool IntegerToFloat = Kind == X86FPConvertKind::SignedIntegerToFloat ||
                              Kind == X86FPConvertKind::UnsignedIntegerToFloat;
  const bool FloatToFloat = Kind == X86FPConvertKind::FloatToFloat;
  const bool Unsigned = Kind == X86FPConvertKind::UnsignedIntegerToFloat ||
                        Kind == X86FPConvertKind::FloatToUnsignedInteger;
  const llvm::APFloat::roundingMode APFRounding = roundingMode(Rounding, MXCSR);
  const bool DAZ = (MXCSR & (1U << 6)) != 0;
  const bool UnderflowMasked = SuppressExceptions || (MXCSR & (1U << 11)) != 0;
  const bool FTZ = (MXCSR & (1U << 15)) != 0;
  const uint64_t ActiveMask = readOperand(Op.Inputs[3]);
  const std::vector<uint8_t> Source = readOperandBytes(Op.Inputs[2]);
  if (Source.size() != SourceSize)
    return false;

  std::vector<uint8_t> Result(DestinationSize, 0);
  uint32_t Raised = 0;
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    if (((ActiveMask >> Lane) & 1U) == 0)
      continue;
    uint64_t Bits =
        readLane(Source, static_cast<size_t>(Lane) * SourceElementSize,
                 SourceElementSize);
    llvm::APFloat::opStatus Status = llvm::APFloat::opOK;
    uint64_t Converted = 0;
    if (IntegerToFloat) {
      llvm::APFloat Value(floatSemantics(DestinationElementSize));
      Status = Value.convertFromAPInt(llvm::APInt(SourceElementSize * 8, Bits),
                                      !Unsigned, APFRounding);
      Converted = Value.bitcastToAPInt().getZExtValue();
    } else {
      if (isSignalingNaN(Bits, SourceElementSize))
        Raised |= 1U << 0;
      if (isDenormal(Bits, SourceElementSize)) {
        if (DAZ)
          Bits &= signMask(SourceElementSize);
        else
          Raised |= 1U << 1;
      }
      llvm::APFloat Value(floatSemantics(SourceElementSize),
                          llvm::APInt(SourceElementSize * 8, Bits));
      if (FloatToFloat) {
        bool LosesInfo = false;
        Status = Value.convert(floatSemantics(DestinationElementSize),
                               APFRounding, &LosesInfo);
        Converted = Value.bitcastToAPInt().getZExtValue();
        if (isDenormal(Converted, DestinationElementSize) && !UnderflowMasked) {
          Raised |= 1U << 4;
        } else if (isDenormal(Converted, DestinationElementSize) && FTZ) {
          Converted &= signMask(DestinationElementSize);
          Raised |= (1U << 4) | (1U << 5);
        }
      } else {
        llvm::APSInt Integer(DestinationElementSize * 8, Unsigned);
        bool IsExact = false;
        Status = Value.convertToInteger(Integer, APFRounding, &IsExact);
        if ((static_cast<unsigned>(Status) &
             static_cast<unsigned>(llvm::APFloat::opInvalidOp)) != 0)
          Integer =
              Unsigned
                  ? llvm::APInt::getAllOnes(DestinationElementSize * 8)
                  : llvm::APInt::getSignedMinValue(DestinationElementSize * 8);
        Converted = Integer.getZExtValue();
      }
    }
    Raised |= statusFlags(Status);
    writeLane(Result, static_cast<size_t>(Lane) * DestinationElementSize,
              DestinationElementSize, Converted);
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
