//===- X86InstructionRelocations.cpp - x86 relocation fields -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Instruction relocation fields for X86.
///
//===----------------------------------------------------------------------===//

#include "../InstructionRelocationsDetail.h"

#include "neverd/Limits.h"
#include "neverd/loader/BinaryImage.h"

#include <cstring>

namespace neverd {
namespace instruction_relocations_detail {
namespace {

/// In an image that runs at its link address, the displacement of a memory
/// operand that lands in a mapped segment, or in the image's own headers, is
/// that address, as a relocation would have said: `jmp *table(,%rdi,8)` and
/// `movzbl array(%rcx), %eax` index an object of the image.  A field offset
/// added to a pointer (`mov 8(,%rcx,8), %rax` on a packed pointer) lands in
/// no segment, the image being mapped above it, and stays a number.
/// RIP-relative operands and a plain `[disp]` are addresses for the lifter
/// already.
void addFixedImageDisplacement(const BinaryImage &Img, const DecodedInsn &DI,
                               InstructionRelocations &Result) {
  if (!Img.LoadsAtLinkAddress || !DI.Raw || !DI.Raw->detail)
    return;
  const cs_x86 &X86 = DI.Raw->detail->x86;
  const uint8_t Width = X86.encoding.disp_size;
  const uint8_t Offset = X86.encoding.disp_offset;
  if (Width == 0 || Offset == 0 || Offset + Width > DI.Size)
    return;
  const cs_x86_op *Memory = nullptr;
  for (uint8_t I = 0; I < X86.op_count; ++I)
    if (X86.operands[I].type == X86_OP_MEM) {
      Memory = &X86.operands[I];
      break;
    }
  if (!Memory || Memory->mem.base == X86_REG_RIP ||
      Memory->mem.base == X86_REG_EIP ||
      (Memory->mem.base == X86_REG_INVALID &&
       Memory->mem.index == X86_REG_INVALID) ||
      Memory->mem.segment == X86_REG_FS || Memory->mem.segment == X86_REG_GS)
    return;
  const unsigned AddressBytes =
      X86.addr_size ? X86.addr_size : Img.getPointerSize();
  uint64_t Target = static_cast<uint64_t>(Memory->mem.disp);
  if (AddressBytes < 8)
    Target &= (uint64_t(1) << (AddressBytes * 8)) - 1;
  // The image's own headers are its data whether or not they are mapped.
  const Segment *Seg = Img.getSegmentFor(Target);
  if ((!Seg && !Img.imageBaseOffset(Target)) ||
      Target < limits::kMinGlobalDataAddr)
    return;
  // A relocation the loader recorded for the field already owns it.
  const va_t FieldVA = DI.Addr + Offset;
  for (const RelocatedAddressOperand &Existing : Result.Addresses)
    if (Existing.FieldVA == FieldVA)
      return;
  uint64_t Encoded = 0;
  std::memcpy(&Encoded, DI.Raw->bytes + Offset, Width);
  // A memory operand narrower than a pointer cannot read a code pointer, so
  // its base in an executable segment is data among the code (a two-level
  // switch's byte index table), not a code address.
  const bool NarrowerThanPointer =
      Memory->size != 0 && Memory->size < Img.getPointerSize();
  Result.Addresses.push_back(RelocatedAddressOperand{
      FieldVA, Encoded, Target, Width,
      Seg && Seg->isExecutable() && !NarrowerThanPointer
          ? ConstantAddressProvenance::CodeAddress
          : ConstantAddressProvenance::DataAddress,
      InvalidVA, /*PCRelativeFromInstructionEnd=*/false});
}

/// An unlinked ELF object's immediate field no relocation writes: every
/// address of the object's own sections in an instruction needs one, so the
/// field is a number.
void addUnrelocatedImmediate(const BinaryImage &Img, const DecodedInsn &DI,
                             InstructionRelocations &Result) {
  if (!DI.Raw || !DI.Raw->detail)
    return;
  const auto &Encoding = DI.Raw->detail->x86.encoding;
  const uint8_t Width = Encoding.imm_size;
  if (Width == 0 || Width > 8 || Encoding.imm_offset == 0 ||
      Encoding.imm_offset + Width > DI.Size)
    return;
  const va_t FieldVA = DI.Addr + Encoding.imm_offset;
  if (!Img.isUnrelocatedObjectField(FieldVA, Width))
    return;
  uint64_t Encoded = 0;
  for (uint8_t Byte = 0; Byte < Width; ++Byte)
    Encoded |= uint64_t(DI.Raw->bytes[Encoding.imm_offset + Byte])
               << (Byte * 8);
  Result.Scalars.push_back(
      {FieldVA, Encoded, Width,
       RelocatedScalarOperand::Kind::X86ELFUnrelocatedImmediate});
}

/// i386 ELF fields the loader recorded: the GOT-pointer fields of
/// position-independent code.
void addI386ELFFields(const BinaryImage &Img, const DecodedInsn &DI,
                      InstructionRelocations &Result) {
  const va_t Cur = DI.Addr;
  const int Sz = DI.Size;
  const va_t Next = Cur + static_cast<va_t>(Sz);
  std::vector<RelocatedScalarOperand> &RelocatedScalarOperands = Result.Scalars;
  if (Img.getPointerSize() != 4)
    return;
  auto Field = Img.I386GOTPCFields.lower_bound(Cur);
  for (; Field != Img.I386GOTPCFields.end() && Field->first < Next; ++Field)
    RelocatedScalarOperands.push_back(
        {Field->first, Field->second.EncodedValue, 4,
         RelocatedScalarOperand::Kind::I386ELFGOTPC});
  auto Ambiguous = Img.AmbiguousI386GOTPCFields.lower_bound(Cur);
  for (; Ambiguous != Img.AmbiguousI386GOTPCFields.end() && *Ambiguous < Next;
       ++Ambiguous) {
    const uint8_t *EncodedBytes = Img.readVA(*Ambiguous, 4);
    if (!EncodedBytes)
      continue;
    uint32_t Encoded = 0;
    std::memcpy(&Encoded, EncodedBytes, sizeof(Encoded));
    RelocatedScalarOperands.push_back(
        {*Ambiguous, Encoded, 4, RelocatedScalarOperand::Kind::I386ELFGOTPC});
  }
  auto AmbiguousGOTOFF = Img.AmbiguousI386GOTOFFFields.lower_bound(Cur);
  for (; AmbiguousGOTOFF != Img.AmbiguousI386GOTOFFFields.end() &&
         *AmbiguousGOTOFF < Next;
       ++AmbiguousGOTOFF) {
    const uint8_t *EncodedBytes = Img.readVA(*AmbiguousGOTOFF, 4);
    if (!EncodedBytes)
      continue;
    uint32_t Encoded = 0;
    std::memcpy(&Encoded, EncodedBytes, sizeof(Encoded));
    RelocatedScalarOperands.push_back(
        {*AmbiguousGOTOFF, Encoded, 4,
         RelocatedScalarOperand::Kind::I386ELFAmbiguousGOTOFF});
  }
}

} // namespace

bool isX86NarrowMemoryDisplacement(const BinaryImage &Img,
                                   const DecodedInsn &DI, va_t FieldVA) {
  // A sub-pointer memory operand cannot read a code pointer. Its relocated
  // displacement names data even when it points into an executable segment.
  if (!DI.Raw || !DI.Raw->detail)
    return false;
  const cs_x86 &X86 = DI.Raw->detail->x86;
  if (X86.encoding.disp_size == 0 ||
      DI.Addr + X86.encoding.disp_offset != FieldVA)
    return false;
  const unsigned PtrSize = Img.getPointerSize();
  for (uint8_t I = 0; I < X86.op_count; ++I)
    if (X86.operands[I].type == X86_OP_MEM)
      return X86.operands[I].size != 0 && X86.operands[I].size < PtrSize;
  return false;
}

std::optional<unsigned> maxAdjacentX86RelocatedFields(Arch A, unsigned Width) {
  // A four-byte displacement and immediate sit back to back in
  // `mov dword ptr [disp32], imm32`, and no instruction holds a third.  Only
  // x86-64's `mov r64, imm64` and its moffs64 forms hold an eight-byte field,
  // one each.  The next instruction starts with an opcode byte, so none of its
  // fields continues the run.
  if (Width == 4)
    return 2;
  if (Width == 8 && A == Arch::X64)
    return 1;
  return std::nullopt;
}

void addX86Fields(const BinaryImage &Img, const DecodedInsn &DI,
                  InstructionRelocations &Result) {
  if (Img.isELF())
    addUnrelocatedImmediate(Img, DI, Result);
  if (Img.Arch == Arch::X86 && Img.isELF())
    addI386ELFFields(Img, DI, Result);
  addFixedImageDisplacement(Img, DI, Result);
}

} // namespace instruction_relocations_detail
} // namespace neverd
