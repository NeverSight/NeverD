//===- PointerRelocation.h - Shared absolute-pointer provenance -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Normalizes container-specific full-width pointer relocations into the
/// format-neutral slot and target provenance carried by BinaryImage.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_POINTERRELOCATION_H
#define NEVERD_LOADER_POINTERRELOCATION_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/object/SectionNames.h"

namespace neverd {

/// Whether storage is writable after loader relocation processing completes.
/// ELF `.data.rel.ro` and equivalent container sections carry a writable file
/// flag only for relocation, then become immutable at runtime; downstream
/// mutation analysis must use this semantic view rather than raw SHF_WRITE /
/// section flags.
inline bool isRuntimeWritableAddress(const BinaryImage &Img, va_t Address) {
  if (const Section *Sec = Img.getSectionFor(Address))
    return Sec->isWritable() &&
           !section_names::isReadOnlyAfterRelocSectionName(Sec->Name) &&
           !section_names::isReadOnlyAfterRelocSectionName(Sec->SegmentName);
  if (const Segment *Seg = Img.getSegmentFor(Address))
    return Seg->isWritable();
  // Unknown ownership is not evidence of immutability.
  return true;
}

/// Record the provenance carried by one full-width absolute pointer
/// relocation.  Container formats spell these differently (Mach-O rebases,
/// PE base relocations, ELF absolute/RELATIVE relocations), but downstream
/// code needs one invariant: a pointer stored in data is a relocatable slot,
/// while the same relocation embedded in code proves the materialized target
/// address without turning instruction bytes into a pointer table.
inline bool recordAbsolutePointerRelocation(BinaryImage &Img, va_t SlotVA,
                                            va_t TargetVA,
                                            va_t TargetOwnerVA = InvalidVA) {
  if (Img.ImportStorageSlots.count(SlotVA) ||
      Img.ConflictingImportStorageSlots.count(SlotVA) ||
      Img.ImportPtrSlots.count(SlotVA) || Img.DyldBindSlots.count(SlotVA))
    return false;

  if (TargetOwnerVA == InvalidVA)
    TargetOwnerVA = TargetVA;

  struct AddressClass {
    bool Mapped = false;
    bool Readable = false;
    bool Writable = false;
    bool Executable = false;
    bool HasOwnerRange = false;
    va_t OwnerBegin = 0;
    va_t OwnerEnd = 0;
  };
  auto Classify = [&](va_t Addr) {
    AddressClass C;
    const Segment *Seg = Img.getSegmentFor(Addr);
    if (!Seg)
      return C;
    C.Mapped = true;
    if (const Section *Sec = Img.getSectionFor(Addr)) {
      C.Readable = Sec->isReadable();
      C.Writable = isRuntimeWritableAddress(Img, Addr);
      // Linked Mach-O sections inherit their enclosing segment protection, so
      // __TEXT,__cstring appears executable in Section::Flags. Its instruction
      // attributes are the exact authority. ELF/COFF sections carry their own
      // execute bit and can likewise override a coarse executable load segment.
      C.Executable = Img.hasExecutableCodeOwnerAt(Addr);
      if (Sec->Size <= InvalidVA - Sec->VA) {
        C.HasOwnerRange = true;
        C.OwnerBegin = Sec->VA;
        C.OwnerEnd = Sec->VA + Sec->Size;
      }
    } else {
      C.Readable = Seg->isReadable();
      C.Writable = Seg->isWritable();
      // Keep absolute-relocation provenance aligned with BinaryImage's
      // format-aware code classifier. In particular, a mapped header or
      // alignment gap is relocatable data identity when section metadata
      // exists.
      C.Executable = Img.hasExecutableCodeOwnerAt(Addr);
      if (Seg->Size <= InvalidVA - Seg->VA) {
        C.HasOwnerRange = true;
        C.OwnerBegin = Seg->VA;
        C.OwnerEnd = Seg->VA + Seg->Size;
      }
    }
    return C;
  };

  AddressClass Slot = Classify(SlotVA);
  AddressClass Target = Classify(TargetOwnerVA);
  if (!Slot.Mapped || !Target.Mapped)
    return false;
  // The relocation symbol/section owns the semantic role; the resolved addend
  // may legally name its one-past data address, which has no mapped byte and
  // may numerically coincide with the next section.  Code targets must still
  // name an owned byte, while data targets admit that one-past endpoint.
  if (!Target.HasOwnerRange || TargetVA < Target.OwnerBegin ||
      (Target.Executable ? TargetVA >= Target.OwnerEnd
                         : TargetVA > Target.OwnerEnd))
    return false;

  bool Changed = false;
  auto RecordOperand = [&](auto &Occurrences) {
    const RelocatedAddressField Field{
        TargetVA, TargetVA, static_cast<uint8_t>(Img.getPointerSize()),
        Target.OwnerBegin};
    auto It = Occurrences.find(SlotVA);
    if (It == Occurrences.end() ||
        It->second.EncodedValue != Field.EncodedValue ||
        It->second.TargetVA != Field.TargetVA ||
        It->second.Width != Field.Width ||
        It->second.TargetOwnerVA != Field.TargetOwnerVA) {
      Occurrences[SlotVA] = Field;
      Changed = true;
    }
  };
  if (Target.Executable) {
    if (Slot.Executable) {
      Changed |= Img.DataAddressRelocOperands.erase(SlotVA) != 0;
      Changed |= Img.CodeRefTargets
                     .insert(normalizeCodeAddress(TargetVA, Img.Arch, Img.Mode))
                     .second;
      RecordOperand(Img.CodeAddressRelocOperands);
    } else if (Slot.Readable) {
      Changed |= Img.DataPtrRelocSlots.erase(SlotVA) != 0;
      Changed |= Img.DataPtrRelocTargetOwners.erase(SlotVA) != 0;
      Changed |= Img.CodePtrRelocSlots.insert(SlotVA).second;
    }
    return Changed;
  }

  if (!Target.Readable || Target.Executable)
    return false;
  if (Target.Writable)
    Changed |= Img.WritableRelocDataAddrs.insert(TargetVA).second;
  else
    Changed |= Img.RelocDataAddrs.insert(TargetVA).second;

  if (Slot.Executable) {
    Changed |= Img.CodeAddressRelocOperands.erase(SlotVA) != 0;
    RecordOperand(Img.DataAddressRelocOperands);
  } else if (Slot.Readable) {
    Changed |= Img.CodePtrRelocSlots.erase(SlotVA) != 0;
    Changed |= Img.DataPtrRelocSlots.insert(SlotVA).second;
    auto OwnerIt = Img.DataPtrRelocTargetOwners.find(SlotVA);
    if (OwnerIt == Img.DataPtrRelocTargetOwners.end() ||
        OwnerIt->second != Target.OwnerBegin) {
      Img.DataPtrRelocTargetOwners[SlotVA] = Target.OwnerBegin;
      Changed = true;
    }
  }
  return Changed;
}

} // namespace neverd

#endif // NEVERD_LOADER_POINTERRELOCATION_H
