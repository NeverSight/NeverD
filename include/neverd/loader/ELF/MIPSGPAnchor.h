//===- MIPSGPAnchor.h - Linked MIPS global pointer storage -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ELF_MIPSGPANCHOR_H
#define NEVERD_LOADER_ELF_MIPSGPANCHOR_H

#include "neverd/loader/BinaryImageModel.h"
#include "neverd/object/SectionNames.h"

#include <optional>

namespace neverd {

/// GP may lie beyond the last mapped byte. Its signed GOT displacements
/// still address the same table after the linked image moves.
inline std::optional<va_t> mipsPICGPAnchor(const BinaryImage &Image,
                                           uint64_t Value) {
  if (!Image.MIPS || Image.MIPS->PICGP != Value)
    return std::nullopt;
  if (Image.DynInfo.PltGotAddr)
    return Image.DynInfo.PltGotAddr;
  if (const auto *GOT = Image.getSectionByName(section_names::elf::Got);
      GOT && GOT->isReadable() && !GOT->isExecutable() &&
      GOT->Size >= Image.getPointerSize())
    return GOT->VA;
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_LOADER_ELF_MIPSGPANCHOR_H
