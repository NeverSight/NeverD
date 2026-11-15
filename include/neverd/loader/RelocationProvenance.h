//===- RelocationProvenance.h - Relocated slots in a byte range -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_RELOCATIONPROVENANCE_H
#define NEVERD_LOADER_RELOCATIONPROVENANCE_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/SortedSearch.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <set>
#include <vector>

namespace neverd {

/// The vector-backed slots of an immutable image. Range queries still read
/// the ordered slot maps and validate imports in their owning image. Keep
/// this index only while that image's relocation metadata stays unchanged.
class RelocationProvenanceIndex {
  std::vector<va_t> Slots;

public:
  explicit RelocationProvenanceIndex(const BinaryImage &Img) {
    Slots.reserve(Img.BaseRelocations.size() + Img.Relocations.size());
    for (const auto &Reloc : Img.BaseRelocations)
      Slots.push_back(Reloc.Address);
    for (const auto &Reloc : Img.Relocations)
      Slots.push_back(Reloc.Address);
    std::sort(Slots.begin(), Slots.end());
    Slots.erase(std::unique(Slots.begin(), Slots.end()), Slots.end());
  }

  bool contains(va_t First, va_t End) const {
    const auto It = sorted::lowerBound64(Slots.data(), Slots.size(), First);
    return It != Slots.size() && Slots[It] < End;
  }
};

/// Whether a relocation or fixup slot begins at a byte of [\p First, \p End):
/// BinaryImage::hasRelocationProvenanceAt() of each byte, from one look at
/// each of its sources rather than a pass over its lists for every byte.
/// An optional index must have been built from this unchanged image.
inline bool
hasRelocationProvenanceIn(const BinaryImage &Img, va_t First, va_t End,
                          const RelocationProvenanceIndex *Index = nullptr) {
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
  if (Index)
    return Index->contains(First, End);
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
