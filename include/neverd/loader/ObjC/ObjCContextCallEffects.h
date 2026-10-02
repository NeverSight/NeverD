#ifndef NEVERD_LOADER_OBJC_OBJCCONTEXTCALLEFFECTS_H
#define NEVERD_LOADER_OBJC_OBJCCONTEXTCALLEFFECTS_H

#include "neverd/ir/low/SourceCallOccurrence.h"
#include "neverd/ir/low/SourceFrameEffects.h"

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Complete immutable ARM64 context-load/tail-dispatch body, independently of
/// a saved native ABI. Recognition requires the full effect proof below.
bool isObjCContextProjection(const BinaryImage &Image, va_t Entry);

/// Prove a nonescaping eight-byte context read followed by a strong declared
/// Objective-C message. Every forwarded carrier must match the current native
/// ABI. An optional counter update must address unique writable image storage.
/// The loaded object, message, and counter retain all their external effects;
/// the caller must separately exclude frame-derived values in borrowed bytes.
std::optional<SourceFrameEffects>
objcContextProjectionEffects(const BinaryImage &Image, va_t Entry,
                             const SourceFunctionTypeHint &Signature);

/// Recheck the original direct BL or immutable indirect-call occurrence as
/// well as the current body, import, declaration, and complete native ABI.
std::optional<SourceFrameEffects> objcContextCallEffects(
    const BinaryImage &Image, const LowFunc &Caller,
    const SourceCallOccurrenceKey &Site,
    const SourceFunctionTypeHint &Signature,
    const std::map<va_t, SourceFunctionTypeHint> *NativeCallees = nullptr);
} // namespace neverd
#endif
