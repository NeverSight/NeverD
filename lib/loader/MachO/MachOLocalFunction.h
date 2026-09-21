#ifndef NEVERD_LOADER_MACHO_MACHOLOCALFUNCTION_H
#define NEVERD_LOADER_MACHO_MACHOLOCALFUNCTION_H

#include "neverd/Common.h"

namespace neverd {
struct BinaryImage;

/// Authenticate one local, non-exported function range in the current linked
/// image. BinaryImage::Exports also contains ordinary nlist function names,
/// so it cannot supply linkage evidence. Missing file ranges, malformed
/// metadata, aliases, interior symbols and dynamic exports fail closed.
bool isMachOLocalFunctionRange(const BinaryImage &Image, va_t Entry,
                               uint64_t Size);
} // namespace neverd
#endif
