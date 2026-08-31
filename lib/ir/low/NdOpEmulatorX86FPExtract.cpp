//===- NdOpEmulatorX86FPExtract.cpp - Exact x86 FP extraction -----------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"

#include <bit>
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
  unsigned FractionBits;
  unsigned ExponentBias;
  unsigned MaximumRawExponent;
};

FloatFormat formatForSize(uint16_t Size) {
  if (Size == 4)
    return {UINT64_C(0x80000000), UINT64_C(0x7f800000),
            UINT64_C(0x007fffff), UINT64_C(0x00400000),
            UINT64_C(0xffc00000), 23, 127, 0xff};
  return {UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000000),
          UINT64_C(0x000fffffffffffff), UINT64_C(0x0008000000000000),
          UINT64_C(0xfff8000000000000), 52, 1023, 0x7ff};
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

bool hasUnmaskedException(uint32_t MXCSR, uint32_t Raised) {
  return (Raised & ~(MXCSR >> 7) & 0x3fU) != 0;
}

uint64_t exponentAsFloat(int Exponent, uint16_t ElementSize,
                         const llvm::fltSemantics &Semantics) {
  llvm::APFloat Result(Semantics);
  const llvm::APInt Integer(
      32, static_cast<uint64_t>(static_cast<int64_t>(Exponent)), true);
  const llvm::APFloat::opStatus Status = Result.convertFromAPInt(
      Integer, true, llvm::APFloat::rmNearestTiesToEven);
  if (Status != llvm::APFloat::opOK)
    return ElementSize == 4 ? UINT64_C(0x7fc00000)
                            : UINT64_C(0x7ff8000000000000);
  return Result.bitcastToAPInt().getZExtValue();
}

uint64_t extractExponent(uint64_t Bits, uint16_t ElementSize,
                         const FloatFormat &Format,
                         const llvm::fltSemantics &Semantics, bool DAZ,
                         uint32_t &Raised) {
  const unsigned RawExponent =
      static_cast<unsigned>((Bits & Format.Exponent) >> Format.FractionBits);
  const uint64_t Fraction = Bits & Format.Fraction;
  if (RawExponent == Format.MaximumRawExponent) {
    if (Fraction == 0)
      return Format.Exponent;
    if ((Fraction & Format.Quiet) == 0)
      Raised |= 1U << 0;
    return Bits | Format.Quiet;
  }
  if (RawExponent == 0) {
    if (Fraction == 0 || DAZ)
      return Format.Sign | Format.Exponent;
    Raised |= 1U << 1;
    const int HighestBit =
        static_cast<int>(std::bit_width(Fraction)) - 1;
    const int Unbiased = HighestBit + 1 -
                         static_cast<int>(Format.ExponentBias) -
                         static_cast<int>(Format.FractionBits);
    return exponentAsFloat(Unbiased, ElementSize, Semantics);
  }
  return exponentAsFloat(static_cast<int>(RawExponent) -
                             static_cast<int>(Format.ExponentBias),
                         ElementSize, Semantics);
}

uint64_t extractMantissa(uint64_t Bits, uint8_t Immediate,
                         const FloatFormat &Format, bool DAZ,
                         uint32_t &Raised) {
  const unsigned RawExponent =
      static_cast<unsigned>((Bits & Format.Exponent) >> Format.FractionBits);
  uint64_t Fraction = Bits & Format.Fraction;
  const bool Negative = (Bits & Format.Sign) != 0;
  const unsigned SignControl = (Immediate >> 2) & 3U;
  const unsigned Interval = Immediate & 3U;
  const uint64_t DestinationSign =
      (SignControl & 1U) != 0 ? 0 : (Bits & Format.Sign);

  if (RawExponent == Format.MaximumRawExponent && Fraction != 0) {
    if ((Fraction & Format.Quiet) == 0)
      Raised |= 1U << 0;
    return Bits | Format.Quiet;
  }
  if (RawExponent == 0 && Fraction == 0)
    return DestinationSign |
           (static_cast<uint64_t>(Format.ExponentBias)
            << Format.FractionBits);
  if (RawExponent == Format.MaximumRawExponent) {
    if (Negative && (SignControl & 2U) != 0) {
      Raised |= 1U << 0;
      return Format.Indefinite;
    }
    return DestinationSign |
           (static_cast<uint64_t>(Format.ExponentBias)
            << Format.FractionBits);
  }
  if (Negative && (SignControl & 2U) != 0) {
    Raised |= 1U << 0;
    return Format.Indefinite;
  }

  int NormalizedExponent = static_cast<int>(RawExponent);
  if (RawExponent == 0 && Fraction != 0) {
    if (DAZ) {
      Fraction = 0;
    } else {
      Raised |= 1U << 1;
      const unsigned HighestBit = std::bit_width(Fraction) - 1;
      const unsigned Shift = Format.FractionBits - HighestBit;
      Fraction = (Fraction << Shift) & Format.Fraction;
      NormalizedExponent =
          static_cast<int>(Format.ExponentBias) - static_cast<int>(Shift);
    }
  }

  const int UnbiasedExponent =
      NormalizedExponent - static_cast<int>(Format.ExponentBias);
  unsigned DestinationExponent = Format.ExponentBias;
  switch (Interval) {
  case 1:
    if ((UnbiasedExponent & 1) != 0)
      --DestinationExponent;
    break;
  case 2:
    --DestinationExponent;
    break;
  case 3:
    if ((Fraction & (UINT64_C(1) << (Format.FractionBits - 1))) != 0)
      --DestinationExponent;
    break;
  default:
    break;
  }
  return DestinationSign |
         (static_cast<uint64_t>(DestinationExponent) << Format.FractionBits) |
         Fraction;
}

} // namespace

bool NdOpEmulator::executeX86FPExtract(const LowOp &Op) {
  if (Op.NumInputs != 5 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 1 ||
      !Op.Inputs[3].isConst() || Op.Inputs[3].Size != 1 ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) ||
      (Op.Output.Size != 16 && Op.Output.Size != 32 && Op.Output.Size != 64) ||
      Op.Inputs[2].Size != Op.Output.Size)
    return false;

  const uint8_t Control = static_cast<uint8_t>(readOperand(Op.Inputs[1]));
  if (!isValidX86FPExtractControl(Control))
    return false;
  const auto Kind = static_cast<X86FPExtractKind>(Control & 1U);
  const bool IsF64 = (Control & (UINT8_C(1) << 1)) != 0;
  const bool Scalar = (Control & (UINT8_C(1) << 2)) != 0;
  const bool SuppressExceptions = (Control & (UINT8_C(1) << 3)) != 0;
  const uint8_t Immediate = static_cast<uint8_t>(readOperand(Op.Inputs[3]));
  if (Kind == X86FPExtractKind::Exponent && Immediate != 0)
    return false;

  const uint16_t ElementSize = IsF64 ? 8 : 4;
  const unsigned LaneCount = Scalar ? 1 : Op.Output.Size / ElementSize;
  const uint16_t MaskSize = static_cast<uint16_t>((LaneCount + 7U) / 8U);
  if ((Scalar && Op.Output.Size != 16) || Op.Output.Size % ElementSize != 0 ||
      Op.Inputs[4].Size != MaskSize)
    return false;
  const uint64_t ActiveMask = readOperand(Op.Inputs[4]);
  const std::vector<uint8_t> Source = readOperandBytes(Op.Inputs[2]);
  if (Source.size() != Op.Output.Size)
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
    const uint64_t Bits = readLane(Source, Offset, ElementSize);
    const uint64_t Extracted =
        Kind == X86FPExtractKind::Exponent
            ? extractExponent(Bits, ElementSize, Format, Semantics, DAZ,
                              Raised)
            : extractMantissa(Bits, Immediate, Format, DAZ, Raised);
    writeLane(Result, Offset, ElementSize, Extracted);
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
