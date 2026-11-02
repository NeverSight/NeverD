//===- NdOpEmulatorX86FPCompare.cpp - Exact EVEX FP comparisons --------===//

#include "neverd/ir/X86/X86FPComparison.h"
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

uint64_t readLane(const std::vector<uint8_t> &Bytes, size_t Offset,
                  uint16_t Size) {
  uint64_t Value = 0;
  std::memcpy(&Value, Bytes.data() + Offset, Size);
  return Value;
}

bool hasUnmaskedException(uint32_t MXCSR, uint32_t Raised) {
  return (Raised & ~(MXCSR >> 7) & 0x3fU) != 0;
}

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
  const uint16_t MaskSize = static_cast<uint16_t>((LaneCount + 7U) / 8U);
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

  uint64_t Result = 0;
  uint32_t Raised = 0;
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    if (((ActiveMask >> Lane) & 1U) == 0)
      continue;

    const size_t Offset = static_cast<size_t>(Lane) * ElementSize;
    uint64_t LeftBits = readLane(Left, Offset, ElementSize);
    uint64_t RightBits = readLane(Right, Offset, ElementSize);
    const auto Compared = evaluateX86FPComparison(
        LeftBits, RightBits, ElementSize,
        X86FPComparisonSignalingPredicates[Predicate], MXCSR);
    if (!Compared)
      return false;
    const unsigned Relation = Compared->Relation;
    Raised |= Compared->Raised;

    if ((X86FPComparisonPredicateResults[Predicate] &
         (UINT8_C(1) << Relation)) != 0)
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
