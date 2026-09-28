#ifndef NEVERD_LOADER_SWIFT_SWIFTVIRTUALCALLS_H
#define NEVERD_LOADER_SWIFT_SWIFTVIRTUALCALLS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <map>

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Bind only arm64 Swift CGFloat, Double, or Bool accessors and exact no-arg
/// void method thunks with a masked-isa virtual target and preserved Swift
/// self carrier.
std::map<va_t, SourceCallTypeHint>
buildSwiftVirtualCallHints(const BinaryImage &Image, const LowFunc &Function);

/// Recheck the immutable declaration and import evidence carried by a bound
/// virtual call. The LowIR target and context paths are proved at binding.
bool isSwiftVirtualSourceCallHint(const BinaryImage &Image,
                                  const SourceCallTypeHint &Hint);
} // namespace neverd

#endif
