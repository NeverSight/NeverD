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

/// Record the provenance carried by one full-width absolute pointer
/// relocation.  Container formats spell these differently (Mach-O rebases,
/// PE base relocations, ELF absolute/RELATIVE relocations), but downstream
/// code needs one invariant: a pointer stored in data is a relocatable slot,
/// while the same relocation embedded in code proves the materialized target
/// address without turning instruction bytes into a pointer table.
inline bool recordAbsolutePointerRelocation(BinaryImage &Img, va_t SlotVA,
                                            va_t TargetVA) {
  if (TargetVA == 0 || Img.ImportPtrSlots.count(SlotVA) ||
      Img.DyldBindSlots.count(SlotVA))
    return false;

  struct AddressClass {
    bool Mapped = false;
    bool Readable = false;
    bool Writable = false;
    bool Executable = false;
  };
  auto Classify = [&](va_t Addr) {
    AddressClass C;
    const Segment *Seg = Img.getSegmentFor(Addr);
    if (!Seg)
      return C;
    C.Mapped = true;
    if (const Section *Sec = Img.getSectionFor(Addr)) {
      C.Readable = Sec->isReadable();
      C.Writable =
          Sec->isWritable() &&
          !section_names::isReadOnlyAfterRelocSectionName(Sec->Name) &&
          !section_names::isReadOnlyAfterRelocSectionName(Sec->SegmentName);
      // Linked Mach-O sections inherit their enclosing segment protection, so
      // __TEXT,__cstring appears executable in Section::Flags.  Its instruction
      // attributes are the exact authority.  ELF/COFF sections carry their own
      // execute bit and can likewise override a coarse executable load segment.
      C.Executable =
          Img.isMachO() ? Img.isCodeAddress(Addr) : Sec->isExecutable();
    } else {
      C.Readable = Seg->isReadable();
      C.Writable = Seg->isWritable();
      C.Executable = Seg->isExecutable();
    }
    return C;
  };

  AddressClass Slot = Classify(SlotVA);
  AddressClass Target = Classify(TargetVA);
  if (!Slot.Mapped || !Target.Mapped)
    return false;

  bool Changed = false;
  if (Target.Executable) {
    if (Slot.Executable) {
      Changed |= Img.CodeRefTargets
                     .insert(normalizeCodeAddress(TargetVA, Img.Arch, Img.Mode))
                     .second;
    } else if (Slot.Readable) {
      Changed |= Img.DataPtrRelocSlots.erase(SlotVA) != 0;
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

  if (!Slot.Executable && Slot.Readable) {
    Changed |= Img.CodePtrRelocSlots.erase(SlotVA) != 0;
    Changed |= Img.DataPtrRelocSlots.insert(SlotVA).second;
  }
  return Changed;
}

} // namespace neverd

#endif // NEVERD_LOADER_POINTERRELOCATION_H
