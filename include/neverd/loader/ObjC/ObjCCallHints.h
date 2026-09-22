#ifndef NEVERD_LOADER_OBJC_OBJCCALLHINTS_H
#define NEVERD_LOADER_OBJC_OBJCCALLHINTS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <map>
#include <optional>

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Prove that a linked ARM64 Objective-C selector stub overwrites incoming x1
/// before dispatch. This machine fact does not establish a method signature.
bool objcSelectorStubOverwritesCommand(const BinaryImage &Image, va_t Address);

/// Reuse the same selector-stub machine owner while also requiring its decoded
/// selector slot and name to match current metadata. This authenticates only
/// dispatch identity, not a method signature or call effects.
bool objcSelectorStubMatches(const BinaryImage &Image, va_t Address,
                             va_t SelectorReferenceAddress,
                             llvm::StringRef Selector);

/// Authenticate an exact selector-loading objc_msgSend stub and return its
/// declared dynamic-format signature only when the call has no variadic tail.
/// The caller must independently prove that its actual argument count equals
/// the returned fixed parameter count.
std::optional<SourceCallTypeHint>
objcSelectorStubDynamicFormatSourceCallHint(const BinaryImage &Image,
                                            va_t Address);

/// Bind a known ARC runtime routine through an exact imported pointer slot.
/// Register-specific ARM64 entry points retain their machine argument location
/// while TargetName names the corresponding ordinary C runtime operation.
std::optional<SourceCallTypeHint>
objcRuntimeSourceCallHint(const BinaryImage &Image, va_t ImportSlot);

/// Resolve source-only callsite declarations from runtime metadata and exact
/// machine/LowIR evidence. Unknown and conflicting signatures remain unbound.
std::map<va_t, SourceCallTypeHint>
buildObjCSourceCallHints(const BinaryImage &Image, const LowFunc &Function);

} // namespace neverd
#endif
