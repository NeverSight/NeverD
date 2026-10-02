#ifndef NEVERD_LOADER_SWIFT_SWIFTACCESSEFFECTS_H
#define NEVERD_LOADER_SWIFT_SWIFTACCESSEFFECTS_H

#include "neverd/ir/low/SourceCallOccurrence.h"
#include "neverd/ir/low/SourceFrameEffects.h"

namespace neverd {
struct BinaryImage;
struct LowFunc;
struct SourceCallTypeHint;

/// Recognize the current import independently of a persisted call declaration.
/// This grants no frame effects or source-publication authority.
bool isSwiftAccessCallTarget(const BinaryImage &Image, va_t Target);

/// Authenticate the original ARM64 BL, strong import and complete current ABI.
/// beginAccess lends private scratch only when the frame analysis proves an
/// untracked Read/Modify flag. endAccess additionally requires the same opaque
/// initialized scratch record, with no intervening writes or expired frame.
/// Tracked-access lifetimes cannot be represented by a synchronous borrow.
std::optional<SourceFrameEffects>
swiftAccessCallEffects(const BinaryImage &Image, const LowFunc &Caller,
                       const SourceCallOccurrenceKey &Site,
                       const SourceCallTypeHint &Binding);
} // namespace neverd
#endif
