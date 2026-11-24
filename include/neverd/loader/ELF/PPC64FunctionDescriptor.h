//===- PPC64FunctionDescriptor.h - Native descriptor identity -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resolve the linked PPC64 descriptors a native function value can represent.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ELF_PPC64FUNCTIONDESCRIPTOR_H
#define NEVERD_LOADER_ELF_PPC64FUNCTIONDESCRIPTOR_H

#include "neverd/loader/BinaryImageModel.h"

#include <optional>

namespace neverd {

inline std::optional<va_t> nativePPC64DescriptorEntry(const BinaryImage &Image,
                                                      va_t Address) {
  if (!isPowerPC64ELFv1(Image.Arch))
    return std::nullopt;
  const auto It = Image.PPC64Descriptors.find(Address);
  if (It == Image.PPC64Descriptors.end() ||
      It->second.Environment.value_or(0) ||
      Image.PPC64EntryContexts.at(It->second.Entry).Descriptor != Address)
    return std::nullopt;
  return It->second.Entry;
}

} // namespace neverd

#endif // NEVERD_LOADER_ELF_PPC64FUNCTIONDESCRIPTOR_H
