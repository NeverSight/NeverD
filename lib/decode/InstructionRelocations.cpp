//===- InstructionRelocations.cpp - Relocations inside an instruction -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/decode/InstructionRelocations.h"

#include "InstructionRelocationsDetail.h"

#include "neverd/loader/BinaryImage.h"

namespace neverd {

InstructionRelocations instructionRelocations(const BinaryImage &Img,
                                              const DecodedInsn &DI) {
  const va_t Cur = DI.Addr;
  const int Sz = DI.Size;
  const va_t Next = Cur + static_cast<va_t>(Sz);
  InstructionRelocations Result;
  std::vector<RelocatedAddressOperand> &RelocatedOperands = Result.Addresses;
  auto selectRelocatedOperand =
      [&](const std::map<va_t, RelocatedAddressField> &Occurrences,
          ConstantAddressProvenance Provenance) {
        auto It = Occurrences.lower_bound(Cur);
        for (; It != Occurrences.end() && It->first < Next; ++It) {
          va_t TargetVA = It->second.TargetVA;
          if (It->second.PCRelativeFromInstructionEnd) {
            if (It->second.Width == 0 || It->second.Width > 8)
              continue;
            const unsigned Bits = It->second.Width * 8;
            uint64_t Disp = It->second.EncodedValue;
            if (Bits < 64) {
              const uint64_t Mask = (uint64_t(1) << Bits) - 1;
              Disp &= Mask;
              if (Disp & (uint64_t(1) << (Bits - 1)))
                Disp |= ~Mask;
            }
            TargetVA = Next + Disp;
            if (Img.getPointerSize() == 4)
              TargetVA = static_cast<uint32_t>(TargetVA);
          }
          // A loader-anchored i386 table base lies before its owner whether a
          // GOTOFF or an absolute operand encodes it.
          const bool OwnerMatches =
              It->second.Kind == RelocatedAddressFieldKind::I386ELFGOTOFF ||
                      Img.RodataAnchorSeg.count(TargetVA)
                  ? Img.relocatedI386GOTOFFTargetBelongsToOwner(
                        TargetVA, It->second.TargetOwnerVA)
                  : Img.relocatedTargetBelongsToOwner(TargetVA,
                                                      It->second.TargetOwnerVA);
          if (!OwnerMatches)
            continue;
          ConstantAddressProvenance FieldProvenance = Provenance;
          if (FieldProvenance == ConstantAddressProvenance::CodeAddress &&
              (Img.Arch == Arch::X86 || Img.Arch == Arch::X64) &&
              instruction_relocations_detail::isX86NarrowMemoryDisplacement(
                  Img, DI, It->first))
            FieldProvenance = ConstantAddressProvenance::DataAddress;
          RelocatedOperands.push_back(RelocatedAddressOperand{
              It->first, It->second.EncodedValue, TargetVA, It->second.Width,
              FieldProvenance, It->second.TargetOwnerVA,
              It->second.PCRelativeFromInstructionEnd});
        }
      };
  selectRelocatedOperand(Img.DataAddressRelocOperands,
                         ConstantAddressProvenance::DataAddress);
  selectRelocatedOperand(Img.CodeAddressRelocOperands,
                         ConstantAddressProvenance::CodeAddress);
  switch (Img.Arch) {
  case Arch::X86:
  case Arch::X64:
    instruction_relocations_detail::addX86Fields(Img, DI, Result);
    break;
  case Arch::AArch64:
    instruction_relocations_detail::addAArch64Fields(Img, DI, Result);
    break;
  case Arch::ARM:
    instruction_relocations_detail::addARMFields(Img, DI, Result);
    break;
  default:
    break;
  }
  return Result;
}

std::optional<unsigned> maxAdjacentRelocatedFields(Arch A, unsigned Width) {
  switch (A) {
  case Arch::X86:
  case Arch::X64:
    return instruction_relocations_detail::maxAdjacentX86RelocatedFields(A,
                                                                         Width);
  default:
    return std::nullopt;
  }
}

} // namespace neverd
