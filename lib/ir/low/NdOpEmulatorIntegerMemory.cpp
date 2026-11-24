//===- NdOpEmulatorIntegerMemory.cpp - Scalar atomic memory execution -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A single hart executing aligned ordinary memory. Its reservation set covers
/// the bytes of the linked access and overlapping writes invalidate it.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/IntegerMemory.h"
#include "neverd/ir/low/NdOpEmulator.h"

#include "llvm/Support/MathExtras.h"

namespace neverd {
bool NdOpEmulator::executeIntegerMemory(const LowOp &Op, Intrinsic Id) {
  using O = integer_memory::Operation;
  if (Op.NumInputs < 2 || !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 1 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None)
    return false;
  auto F = integer_memory::form(Img.Arch, Id, Op.Inputs[1].Offset);
  const unsigned Width = Img.getPointerSize();
  if (!F || !F->matchesSignature(Op.NumInputs, Op.Output.Size, Width))
    return false;
  if (F->Op == O::Fence)
    return true;
  if ((!Op.Output.isReg() && !Op.Output.isTemp()) ||
      Op.Inputs[2].Size != Width || !knownOperandBytes(Op.Inputs[2], 0, Width))
    return false;
  const uint64_t Address = readOperand(Op.Inputs[2]);
  const unsigned Bytes = F->Bytes;
  if (Address & (Bytes - 1))
    return false;
  const Segment *Memory = Img.getSegmentFor(Address);
  if (!Memory || (F->Op != O::Conditional && !Memory->isReadable()) ||
      (F->Op != O::Reserve && !Memory->isWritable()) ||
      Bytes > Memory->Size - (Address - Memory->VA))
    return false;
  uint64_t In = 0;
  if (F->inputs() == 2) {
    if (Op.Inputs[3].Size != Width ||
        !knownOperandBytes(Op.Inputs[3], 0, Bytes))
      return false;
    In =
        readOperand(Op.Inputs[3]) & llvm::maskTrailingOnes<uint64_t>(Bytes * 8);
  }
  if (F->Op == O::Conditional) {
    if (!IntegerReservationKnown)
      return false;
    const bool Success = IntegerReservation &&
                         IntegerReservation->first == Address &&
                         IntegerReservation->second == Bytes;
    if (Success && !storeMemory(Address, Bytes, In))
      return false;
    IntegerReservation.reset();
    writeOutput(Op.Output, isRISCV(Img.Arch) ? !Success : Success);
    return true;
  }
  const auto Old = loadMemory(Address, Bytes);
  if (!Old)
    return false;
  const uint64_t Sign = uint64_t(1) << (Bytes * 8 - 1);
  uint64_t New;
  switch (F->Op) {
  case O::Reserve:
    IntegerReservationKnown = true;
    IntegerReservation.emplace(Address, Bytes);
    break;
  case O::Swap:
    New = In;
    break;
  case O::Add:
    New = *Old + In;
    break;
  case O::And:
    New = *Old & In;
    break;
  case O::Or:
    New = *Old | In;
    break;
  case O::Xor:
    New = *Old ^ In;
    break;
  case O::SMin:
    New = ((*Old ^ Sign) < (In ^ Sign)) ? *Old : In;
    break;
  case O::SMax:
    New = ((*Old ^ Sign) > (In ^ Sign)) ? *Old : In;
    break;
  case O::UMin:
    New = std::min(*Old, In);
    break;
  case O::UMax:
    New = std::max(*Old, In);
    break;
  default:
    return false;
  }
  if (F->Op != O::Reserve && !storeMemory(Address, Bytes, New))
    return false;
  const uint64_t Result = isRISCV(Img.Arch) && Bytes == 4
                              ? uint64_t(llvm::SignExtend64<32>(*Old))
                              : *Old;
  writeOutput(Op.Output, Result);
  return true;
}
} // namespace neverd
