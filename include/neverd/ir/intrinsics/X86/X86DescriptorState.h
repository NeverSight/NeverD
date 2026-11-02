//===- X86DescriptorState.h - Descriptor state shapes ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One operand contract for descriptor checks, table transfers and selectors.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86DESCRIPTORSTATE_H
#define NEVERD_IR_INTRINSICS_X86_X86DESCRIPTORSTATE_H

#include "neverd/ir/intrinsics/Intrinsics.h"

namespace neverd {

inline constexpr Intrinsic X86DescriptorStateInventory[] = {
    Intrinsic::Lgdt,      Intrinsic::Lidt,      Intrinsic::Sgdt,
    Intrinsic::Sidt,      Intrinsic::Invlpg,    Intrinsic::Lldt,
    Intrinsic::Ltr,       Intrinsic::Lmsw,      Intrinsic::Sldt,
    Intrinsic::Str,       Intrinsic::Smsw,      Intrinsic::Verr,
    Intrinsic::Verw,      Intrinsic::X86Lgdt16, Intrinsic::X86Lidt16,
    Intrinsic::X86Sgdt16, Intrinsic::X86Sidt16};

inline bool isX86DescriptorStateIntrinsic(Intrinsic Id) {
  for (const auto Candidate : X86DescriptorStateInventory)
    if (Id == Candidate)
      return true;
  return false;
}

inline bool isX86DescriptorTable16(Intrinsic Id) {
  return Id == Intrinsic::X86Lgdt16 || Id == Intrinsic::X86Lidt16 ||
         Id == Intrinsic::X86Sgdt16 || Id == Intrinsic::X86Sidt16;
}

inline bool isX86DescriptorTable(Intrinsic Id) {
  return Id == Intrinsic::Lgdt || Id == Intrinsic::Lidt ||
         Id == Intrinsic::Sgdt || Id == Intrinsic::Sidt ||
         isX86DescriptorTable16(Id);
}

inline bool isX86DescriptorVerify(Intrinsic Id) {
  return Id == Intrinsic::Verr || Id == Intrinsic::Verw;
}

inline bool x86DescriptorStateShapeIsValid(
    Intrinsic Id, Arch TargetArch, unsigned Inputs, unsigned OutputBytes,
    unsigned OperandBytes, NdMemoryAddressSpace Space,
    NdMemoryOrdering Ordering = NdMemoryOrdering::None) {
  if (!isX86DescriptorStateIntrinsic(Id) ||
      (TargetArch != Arch::Unknown && TargetArch != Arch::X86 &&
       TargetArch != Arch::X64) ||
      (Space != NdMemoryAddressSpace::Default &&
       Space != NdMemoryAddressSpace::X86FS &&
       Space != NdMemoryAddressSpace::X86GS) ||
      Ordering != NdMemoryOrdering::None ||
      (isX86DescriptorTable16(Id) && TargetArch == Arch::X64))
    return false;
  const bool Memory = Inputs == 2 && OperandBytes == 8;
  if (!Memory && Space != NdMemoryAddressSpace::Default)
    return false;
  if (isX86DescriptorTable(Id) || Id == Intrinsic::Invlpg)
    return Memory && !OutputBytes;
  if (isX86DescriptorVerify(Id))
    return Inputs == 2 && (Memory || OperandBytes == 2) && OutputBytes <= 1;
  if (isX86SystemRegisterStore(Id))
    return Memory ? !OutputBytes
                  : Inputs == 1 && !OperandBytes &&
                        (OutputBytes == 2 || OutputBytes == 4 ||
                         (OutputBytes == 8 && TargetArch != Arch::X86));
  return Inputs == 2 && !OutputBytes && (Memory || OperandBytes == 2);
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86DESCRIPTORSTATE_H
