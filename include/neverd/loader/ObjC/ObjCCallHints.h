#ifndef NEVERD_LOADER_OBJC_OBJCCALLHINTS_H
#define NEVERD_LOADER_OBJC_OBJCCALLHINTS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <map>

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Prove that a linked ARM64 Objective-C selector stub overwrites incoming x1
/// before dispatch. This machine fact does not establish a method signature.
bool objcSelectorStubOverwritesCommand(const BinaryImage &Image, va_t Address);

/// Resolve source-only callsite declarations from runtime metadata and exact
/// machine/LowIR evidence. Unknown and conflicting signatures remain unbound.
std::map<va_t, SourceCallTypeHint>
buildObjCSourceCallHints(const BinaryImage &Image, const LowFunc &Function);

} // namespace neverd
#endif
