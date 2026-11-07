//===- PEHeaderMapping.h - Loaded PE header identity ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Identify the loader's additional header region without changing native
/// COFF section numbering.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_PEHEADERMAPPING_H
#define NEVERD_LOADER_COFF_PEHEADERMAPPING_H

#include <cstddef>

namespace neverd {
struct BinaryImage;

namespace coff_loader {
/// The only additional section is the checked, original PE header mapping
/// following NativeSections actual COFF sections. Bind both normalized
/// regions to the header bytes and their format-native section count; a name
/// alone supplies no identity. Restricted loads retain bytes in the segment.
bool hasCheckedPEHeaderMapping(const BinaryImage &Image, size_t NativeSections);
} // namespace coff_loader
} // namespace neverd

#endif // NEVERD_LOADER_COFF_PEHEADERMAPPING_H
