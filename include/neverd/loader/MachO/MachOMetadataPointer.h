//===- MachOMetadataPointer.h - Analysis-only pointer targets ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Follow loader-owned signing recipes for metadata discovery. This is never
/// evidence of a runtime pointer value or permission to materialize storage.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_MACHO_MACHOMETADATAPOINTER_H
#define NEVERD_LOADER_MACHO_MACHOMETADATAPOINTER_H

#include "neverd/Common.h"

#include <optional>

namespace neverd {
struct BinaryImage;

namespace macho_loader {
/// Metadata-only bytes, including immutable shared-cache mappings outside the
/// selected image. Local owned bytes take precedence so edits remain visible.
const uint8_t *readMetadataBytes(const BinaryImage &Image, va_t Address,
                                 uint64_t Size);
/// Read a local metadata target from a resolved plain slot or a validated
/// signing recipe. Imports, stale recipes and ambiguous storage fail closed.
std::optional<va_t> readMetadataPointerTarget(const BinaryImage &Image,
                                              va_t Slot);
} // namespace macho_loader
} // namespace neverd

#endif // NEVERD_LOADER_MACHO_MACHOMETADATAPOINTER_H
