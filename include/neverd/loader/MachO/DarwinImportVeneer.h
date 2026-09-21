#ifndef NEVERD_LOADER_MACHO_DARWINIMPORTVENEER_H
#define NEVERD_LOADER_MACHO_DARWINIMPORTVENEER_H

#include "neverd/Common.h"

#include <optional>

namespace neverd {
struct BinaryImage;

/// Resolve an ordinary Darwin import veneer from current immutable machine
/// bytes, using the same decoder as ObjC/SDK call binding. Selector-loading
/// stubs are excluded. The returned storage address proves neither an import
/// identity nor a call ABI; consumers must authenticate both separately.
std::optional<va_t> darwinImportVeneerSlot(const BinaryImage &Image,
                                           va_t Address);
} // namespace neverd
#endif
