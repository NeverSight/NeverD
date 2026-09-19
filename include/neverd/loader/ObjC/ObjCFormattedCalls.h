#ifndef NEVERD_LOADER_OBJC_OBJCFORMATTEDCALLS_H
#define NEVERD_LOADER_OBJC_OBJCFORMATTEDCALLS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace neverd {
struct BinaryImage;

/// Promoted scalar arguments of a bounded declared format. NSString positional
/// slots must agree; predicate substitutions exclude quoted literals. Malformed
/// and unsupported forms fail instead of supplying partial argument lists.
std::optional<std::vector<TypeRef>>
objcFormatArgumentTypes(llvm::ArrayRef<uint16_t> Format,
                        SourceCallTypeHint::FormatSyntax Syntax =
                            SourceCallTypeHint::FormatSyntax::NSString);

/// Complete a declared message or C call using its format-language contract.
/// The caller supplies the independently validated fixed signature.
std::optional<SourceCallTypeHint>
bindObjCFormatArguments(const BinaryImage &Image, SourceCallTypeHint Call,
                        unsigned FormatParameter, va_t FormatAddress,
                        SourceCallTypeHint::FormatSyntax Syntax =
                            SourceCallTypeHint::FormatSyntax::NSString);

/// Complete a declared C call using a uniquely mapped immutable NUL-terminated
/// printf format string and the caller-supplied fixed signature.
std::optional<SourceCallTypeHint> bindCFormatArguments(const BinaryImage &Image,
                                                       SourceCallTypeHint Call,
                                                       unsigned FormatParameter,
                                                       va_t FormatAddress);

/// Bind a dynamic message using an immutable format object and a declaration
/// agreed by the SDK and every matching runtime/protocol method.
std::optional<SourceCallTypeHint>
objcFormattedSourceCallHint(const BinaryImage &Image, llvm::StringRef Selector,
                            va_t FormatAddress);

/// Bind one control-flow join whose exact immutable format-object candidates
/// all require the same variadic ABI. A differing, duplicate, malformed, or
/// oversized candidate set is rejected rather than approximated.
std::optional<SourceCallTypeHint>
objcFormattedSourceCallHint(const BinaryImage &Image, llvm::StringRef Selector,
                            llvm::ArrayRef<va_t> FormatAddresses);
} // namespace neverd
#endif
