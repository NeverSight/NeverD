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

/// Prove that an exact, strong libobjc selector stub dispatches through
/// objc_msgSend. Its machine call preserves Darwin nonvolatile registers even
/// when the selector's source-level return and parameter types are unknown.
bool objcSelectorStubPreservesNonvolatileRegisters(const BinaryImage &Image,
                                                   va_t Address);

/// Reuse the same selector-stub machine owner while also requiring its decoded
/// selector slot and name to match current metadata. This authenticates only
/// dispatch identity, not a method signature or call effects.
bool objcSelectorStubMatches(const BinaryImage &Image, va_t Address,
                             va_t SelectorReferenceAddress,
                             llvm::StringRef Selector);

/// Authenticate a strong selector stub and the complete selector-wide agreed
/// declaration. This is a dispatch/ABI fact, not a receiver or effect proof.
std::optional<SourceCallTypeHint>
objcSelectorStubSourceCallHint(const BinaryImage &Image, va_t Address);

struct ObjCArgumentTailCall {
  va_t SelectorStub = 0;
  uint64_t SourceRegister = 0;
  uint64_t DestinationRegister = 0;
};
/// Exact local MOV x0/x2..x7, x19..x28 followed by B to a strong selector stub.
/// Only one physical argument moves; its type still needs a declaration.
std::optional<ObjCArgumentTailCall>
objcArgumentTailCall(const BinaryImage &Image, va_t Address);

/// Authenticate an exact selector-loading objc_msgSend stub and return its
/// declared dynamic-format signature only when the call has no variadic tail.
/// The caller must independently prove that its actual argument count equals
/// the returned fixed parameter count.
std::optional<SourceCallTypeHint>
objcSelectorStubDynamicFormatSourceCallHint(const BinaryImage &Image,
                                            va_t Address);

/// Bind a known ARC runtime routine through an exact imported pointer slot or
/// a complete local MOV x0, x19..x28 / B bridge to a strong objc_release
/// veneer. Register-specific ARM64 entry points retain their machine argument
/// location while TargetName names the corresponding ordinary C runtime
/// operation.
std::optional<SourceCallTypeHint>
objcRuntimeSourceCallHint(const BinaryImage &Image, va_t ImportSlot);

/// Resolve source-only callsite declarations from runtime metadata and exact
/// machine/LowIR evidence. Unknown and conflicting signatures remain unbound.
std::map<va_t, SourceCallTypeHint> buildObjCSourceCallHints(
    const BinaryImage &Image, const LowFunc &Function,
    const std::map<unsigned, ObjCReceiverTypeHint> *BlockParameters = nullptr,
    const std::map<uint64_t, ObjCReceiverTypeHint> *BlockCaptures = nullptr);

} // namespace neverd
#endif
