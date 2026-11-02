//===- RelocationProvenance.h - Relocated slots in a byte range -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_RELOCATIONPROVENANCE_H
#define NEVERD_LOADER_RELOCATIONPROVENANCE_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"

#include <set>

namespace neverd {

/// Whether a relocation or fixup slot begins at a byte of [\p First, \p End):
/// BinaryImage::hasRelocationProvenanceAt() of each byte, from one look at
/// each of its sources rather than a pass over its lists for every byte.
inline bool hasRelocationProvenanceIn(const BinaryImage &Img, va_t First,
                                      va_t End) {
  if (First >= End)
    return false;
  if (const auto It = Img.MachOAuthenticatedPointers.lower_bound(First);
      It != Img.MachOAuthenticatedPointers.end() && It->first < End)
    return true;
  for (const std::set<uint64_t> *Slots :
       {&Img.CodePtrRelocSlots, &Img.DataPtrRelocSlots,
        &Img.RelDataPtrRelocSlots, &Img.RelCodeRelocSlots})
    if (const auto It = Slots->lower_bound(First);
        It != Slots->end() && *It < End)
      return true;
  for (auto It = Img.ImportStorageSlots.lower_bound(First);
       It != Img.ImportStorageSlots.end() && It->first < End; ++It)
    if (Img.isValidImportStorageSlot(It->first, It->second.Name))
      return true;
  for (auto It = Img.ImportPtrSlots.lower_bound(First);
       It != Img.ImportPtrSlots.end() && It->first < End; ++It)
    if (Img.isValidImportStorageSlot(It->first, It->second))
      return true;
  for (auto It = Img.DyldBindSlots.lower_bound(First);
       It != Img.DyldBindSlots.end() && It->first < End; ++It)
    if (Img.isValidImportStorageSlot(It->first, It->second.Name))
      return true;
  const auto Inside = [&](va_t Address) {
    return Address >= First && Address < End;
  };
  return llvm::any_of(Img.BaseRelocations,
                      [&](const BaseRelocation &Reloc) {
                        return Inside(Reloc.Address);
                      }) ||
         llvm::any_of(Img.Relocations, [&](const RelocationEntry &Reloc) {
           return Inside(Reloc.Address);
         });
}

} // namespace neverd

#endif // NEVERD_LOADER_RELOCATIONPROVENANCE_H
