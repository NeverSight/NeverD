//===- LinkedGlobalPointer.h - Linked ELF global pointer bases -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Identify the storage that gives a linked ABI global pointer its value.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ELF_LINKEDGLOBALPOINTER_H
#define NEVERD_LOADER_ELF_LINKEDGLOBALPOINTER_H

#include "neverd/loader/ELF/MIPSGPAnchor.h"
#include "neverd/loader/ELF/PowerPCABI.h"

#include <optional>

namespace neverd {

inline std::optional<va_t> linkedGlobalPointerAnchor(const BinaryImage &Image,
                                                     va_t Value) {
  if (!isPowerPC64ELFv1(Image.Arch))
    return mipsPICGPAnchor(Image, Value);
  if (Value < ppc_elf::TOCBias)
    return std::nullopt;
  const va_t Base = Value - ppc_elf::TOCBias;
  for (auto Name : {section_names::elf::Got, ppc_elf::TOCSection})
    if (const auto *S = Image.getSectionByName(Name);
        S && S->VA == Base && Image.isDataAddress(Base))
      return Base;
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_LOADER_ELF_LINKEDGLOBALPOINTER_H
