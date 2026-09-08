#ifndef NEVERD_LOADER_OBJC_OBJCCALLHINTS_H
#define NEVERD_LOADER_OBJC_OBJCCALLHINTS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <map>

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Resolve source-only callsite declarations from runtime metadata and exact
/// machine/LowIR evidence. Unknown and conflicting signatures remain unbound.
std::map<va_t, SourceCallTypeHint>
buildObjCSourceCallHints(const BinaryImage &Image, const LowFunc &Function);

} // namespace neverd
#endif
