#ifndef NEVERD_LOADER_MACHO_MACHOLOCALFUNCTION_H
#define NEVERD_LOADER_MACHO_MACHOLOCALFUNCTION_H

#include "neverd/Common.h"

namespace neverd {
struct BinaryImage;
enum class MachOLocalFunctionAliases { Reject, SameAddress };

/// Authenticate one local, non-exported function range in the current linked
/// image. BinaryImage::Exports also contains ordinary nlist function names,
/// so it cannot supply linkage evidence. Missing file ranges, malformed
/// metadata, interior symbols and dynamic exports fail closed. The default
/// rejects aliases; SameAddress permits multiple strictly local names for the
/// same entry when the consumer independently proves the complete machine body.
bool isMachOLocalFunctionRange(
    const BinaryImage &Image, va_t Entry, uint64_t Size,
    MachOLocalFunctionAliases Aliases = MachOLocalFunctionAliases::Reject);
} // namespace neverd
#endif
