//===- ARMInstructionRelocations.cpp - ARM relocation fields -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Instruction relocation fields for ARM.
///
//===----------------------------------------------------------------------===//

#include "../InstructionRelocationsDetail.h"

#include "neverd/loader/BinaryImage.h"

namespace neverd {
namespace instruction_relocations_detail {

void addARMFields(const BinaryImage &Img, const DecodedInsn &DI,
                  InstructionRelocations &Result) {
  // An unlinked object's instruction that no relocation writes keeps the
  // numbers its encoding holds: an address it moved into a register would
  // need one (R_ARM_MOVW_ABS_NC and R_ARM_MOVT_ABS for MOVW and MOVT).
  if (!DI.Raw || (DI.Size != 2 && DI.Size != 4) ||
      !Img.isUnrelocatedObjectField(DI.Addr, DI.Size))
    return;
  uint32_t Word = 0;
  for (unsigned Byte = 0; Byte != DI.Size; ++Byte)
    Word |= uint32_t(DI.Raw->bytes[Byte]) << (Byte * 8);
  Result.Scalars.push_back(
      {DI.Addr, Word, static_cast<uint8_t>(DI.Size),
       RelocatedScalarOperand::Kind::ARMELFUnrelocatedInstruction});
}

} // namespace instruction_relocations_detail
} // namespace neverd
