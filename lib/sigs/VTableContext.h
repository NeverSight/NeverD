//===- VTableContext.h - Source-owned primary table records --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared admission contract for native x64 COFF primary-vftable records.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_VTABLECONTEXT_H
#define NEVERD_SIGS_VTABLECONTEXT_H

#include <cstddef>

namespace neverd::sigs {
struct StoredModule;
inline constexpr size_t VTablePointerBytes = 8;
bool isValidVTableContext(const StoredModule &Module);
} // namespace neverd::sigs

#endif // NEVERD_SIGS_VTABLECONTEXT_H
