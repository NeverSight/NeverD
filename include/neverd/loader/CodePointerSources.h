//===- CodePointerSources.h - Relocated code coordinates -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared initial code coordinates for relocation-root consumers. A marked
/// pointer does not establish a callable entry, ABI, object boundary or stable
/// runtime value; each consumer retains those separate obligations.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_CODEPOINTERSOURCES_H
#define NEVERD_LOADER_CODEPOINTERSOURCES_H

#include "neverd/Common.h"

#include <optional>

namespace neverd {
struct BinaryImage;

/// The current target of a loader-marked 32- or 64-bit absolute code-pointer
/// slot. Other widths supply no coordinate. ARM/Thumb mode bits are normalized
/// as for CFG relocation roots: this is a code
/// coordinate, never permission to emit a pointer literal or freeze a load.
std::optional<va_t> normalizedRelocatedCodeTarget(const BinaryImage &Image,
                                                  va_t Slot);

} // namespace neverd

#endif // NEVERD_LOADER_CODEPOINTERSOURCES_H
