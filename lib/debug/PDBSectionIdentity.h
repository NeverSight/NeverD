//===- PDBSectionIdentity.h - Link-time section identity -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared section identity checks for complete PDB loading and name hints.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_PDBSECTIONIDENTITY_H
#define NEVERD_DEBUG_PDBSECTIONIDENTITY_H

#include "neverd/debug/PDBLoader.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Object/COFF.h"

#include <cstddef>

namespace neverd::pdb_loader_detail {

inline RecordedSection
sectionIdentity(const llvm::object::coff_section &Header) {
  return {llvm::StringRef(Header.Name, sizeof(Header.Name)).split('\0').first,
          Header.VirtualAddress,
          Header.VirtualSize,
          Header.PointerToRawData,
          Header.SizeOfRawData,
          Header.Characteristics};
}

/// Only a trailing resource/relocation suffix permits post-link relayout.
template <typename NameAt>
size_t firstPostLinkSection(size_t Count, NameAt GetName) {
  while (Count > 0) {
    const llvm::StringRef Name = GetName(Count - 1);
    if (Name != ".rsrc" && Name != ".reloc")
      break;
    --Count;
  }
  return Count;
}

inline bool sectionIdentityMatches(const RecordedSection &Loaded,
                                   const RecordedSection &Recorded,
                                   bool MayRelayout) {
  return Loaded.Name == Recorded.Name &&
         Loaded.Characteristics == Recorded.Characteristics &&
         (MayRelayout ||
          (Loaded.VirtualAddress == Recorded.VirtualAddress &&
           Loaded.VirtualSize == Recorded.VirtualSize &&
           Loaded.PointerToRawData == Recorded.PointerToRawData &&
           Loaded.SizeOfRawData == Recorded.SizeOfRawData));
}

} // namespace neverd::pdb_loader_detail

#endif // NEVERD_DEBUG_PDBSECTIONIDENTITY_H
