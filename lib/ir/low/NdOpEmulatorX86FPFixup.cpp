//===- NdOpEmulatorX86FPFixup.cpp - Exact x86 fixup operation -----------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"

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
  uint64_t PositiveOne;
  uint64_t Half;
  uint64_t Ninety;
  uint64_t HalfPi;
  uint64_t Maximum;
};

FloatFormat formatForSize(uint16_t Size) {
  if (Size == 4)
    return {UINT64_C(0x80000000), UINT64_C(0x7f800000),
            UINT64_C(0x007fffff), UINT64_C(0x00400000),
            UINT64_C(0x3f800000), UINT64_C(0x3f000000),
            UINT64_C(0x42b40000), UINT64_C(0x3fc90fdb),
            UINT64_C(0x7f7fffff)};
  return {UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000000),
          UINT64_C(0x000fffffffffffff), UINT64_C(0x0008000000000000),
          UINT64_C(0x3ff0000000000000), UINT64_C(0x3fe0000000000000),
          UINT64_C(0x4056800000000000), UINT64_C(0x3ff921fb54442d18),
          UINT64_C(0x7fefffffffffffff)};
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

unsigned classifyToken(uint64_t Bits, const FloatFormat &Format) {
  if (isNaN(Bits, Format))
    return (Bits & Format.Quiet) != 0 ? 0 : 1;
  const uint64_t Magnitude = Bits & ~Format.Sign;
  if (Magnitude == 0)
    return 2;
  if (Bits == Format.PositiveOne)
    return 3;
  if (Magnitude == Format.Exponent)
    return (Bits & Format.Sign) != 0 ? 4 : 5;
  return (Bits & Format.Sign) != 0 ? 6 : 7;
}

uint64_t quietNaN(uint64_t Bits, const FloatFormat &Format) {
  if (isNaN(Bits, Format))
    return Bits | Format.Quiet;
  return (Bits & Format.Sign) | Format.Exponent | Format.Quiet;
}

uint64_t applyResponse(uint64_t OldDestination, uint64_t Source,
                       unsigned Response, const FloatFormat &Format) {
  switch (Response) {
  case 0:
    return OldDestination;
  case 1:
    return Source;
  case 2:
    return quietNaN(Source, Format);
  case 3:
    return Format.Sign | Format.Exponent | Format.Quiet;
  case 4:
    return Format.Sign | Format.Exponent;
  case 5:
    return Format.Exponent;
  case 6:
    return (Source & Format.Sign) | Format.Exponent;
  case 7:
    return Format.Sign;
  case 8:
    return 0;
  case 9:
    return Format.Sign | Format.PositiveOne;
  case 10:
    return Format.PositiveOne;
  case 11:
    return Format.Half;
  case 12:
    return Format.Ninety;
  case 13:
    return Format.HalfPi;
  case 14:
    return Format.Maximum;
  default:
    return Format.Sign | Format.Maximum;
  }
}

uint32_t requestedExceptions(unsigned Token, uint8_t Immediate) {
  constexpr uint32_t Invalid = 1U << 0;
  constexpr uint32_t DivideByZero = 1U << 2;
  uint32_t Raised = 0;
  switch (Token) {
  case 1:
    if ((Immediate & (1U << 4)) != 0)
      Raised |= Invalid;
    break;
  case 2:
    if ((Immediate & (1U << 0)) != 0)
      Raised |= DivideByZero;
    if ((Immediate & (1U << 1)) != 0)
      Raised |= Invalid;
    break;
  case 3:
    if ((Immediate & (1U << 2)) != 0)
      Raised |= DivideByZero;
    if ((Immediate & (1U << 3)) != 0)
      Raised |= Invalid;
    break;
  case 4:
    if ((Immediate & (1U << 5)) != 0)
      Raised |= Invalid;
    break;
  case 5:
    if ((Immediate & (1U << 7)) != 0)
      Raised |= Invalid;
    break;
  case 6:
    if ((Immediate & (1U << 6)) != 0)
      Raised |= Invalid;
    break;
  default:
    break;
  }
  return Raised;
}

} // namespace

bool NdOpEmulator::executeX86FPFixup(const LowOp &Op) {
  if (Op.NumInputs != 6 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 2 ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) ||
      (Op.Output.Size != 16 && Op.Output.Size != 32 && Op.Output.Size != 64) ||
      Op.Inputs[2].Size != Op.Output.Size ||
      Op.Inputs[3].Size != Op.Output.Size ||
      Op.Inputs[4].Size != Op.Output.Size)
    return false;
  const uint16_t PackedControl =
      static_cast<uint16_t>(readOperand(Op.Inputs[1]));
  const uint8_t Control = static_cast<uint8_t>(PackedControl);
  if (!isValidX86FPFixupControl(Control))
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

  const uint8_t Immediate = static_cast<uint8_t>(PackedControl >> 8);
  const uint64_t ActiveMask = readOperand(Op.Inputs[5]);
  const std::vector<uint8_t> OldDestination =
      readOperandBytes(Op.Inputs[2]);
  const std::vector<uint8_t> Source = readOperandBytes(Op.Inputs[3]);
  const std::vector<uint8_t> Table = readOperandBytes(Op.Inputs[4]);
  if (OldDestination.size() != Op.Output.Size ||
      Source.size() != Op.Output.Size || Table.size() != Op.Output.Size)
    return false;

  const FloatFormat Format = formatForSize(ElementSize);
  const bool DAZ = (MXCSR & (1U << 6)) != 0;
  std::vector<uint8_t> Result = OldDestination;
  uint32_t Raised = 0;
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    if (((ActiveMask >> Lane) & 1U) == 0)
      continue;
    const size_t Offset = static_cast<size_t>(Lane) * ElementSize;
    uint64_t LaneSource = readLane(Source, Offset, ElementSize);
    if (DAZ && (LaneSource & Format.Exponent) == 0)
      LaneSource &= Format.Sign;
    const unsigned Token = classifyToken(LaneSource, Format);
    const uint64_t LaneTable = readLane(Table, Offset, ElementSize);
    const unsigned Response =
        static_cast<unsigned>((LaneTable >> (Token * 4U)) & 0xfU);
    writeLane(Result, Offset, ElementSize,
              applyResponse(readLane(OldDestination, Offset, ElementSize),
                            LaneSource, Response, Format));
    Raised |= requestedExceptions(Token, Immediate);
  }

  // VFIXUPIMM treats MXCSR exception masks as set.  Requested flags can
  // update sticky status but never prevent architectural destination commit.
  if (!SuppressExceptions)
    MXCSR |= Raised;
  writeOutputBytes(Op.Output, Result);
  return true;
}

} // namespace neverd
