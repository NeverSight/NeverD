#ifndef NEVERD_LOADER_SWIFT_SWIFTVALUEBUFFEREFFECTS_H
#define NEVERD_LOADER_SWIFT_SWIFTVALUEBUFFEREFFECTS_H

#include "neverd/ir/low/SourceCallOccurrence.h"
#include "neverd/ir/low/SourceFrameEffects.h"

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Recognize the authenticated physical body independently of a saved ABI.
/// Recognition requires consumers to obtain the complete call proof below;
/// it grants neither an effect certificate nor source publication authority.
bool isSwiftValueBufferProjection(const BinaryImage &Image, va_t Entry);

/// Authenticate a complete immutable ARM64 value-buffer projector and its
/// strong swift_makeBoxUnique import, without using the helper's name.
/// The three-word buffer may change and the result may alias its base or heap
/// storage. This does not authorize dropping or reordering any call effects.
std::optional<SourceFrameEffects>
swiftValueBufferProjectionEffects(const BinaryImage &Image, va_t Entry,
                                  const SourceFunctionTypeHint &Signature);

/// Additionally authenticate this caller's original BL and LowIR occurrence.
/// Loader target proofs and pipeline frame proofs must use this same owner.
std::optional<SourceFrameEffects>
swiftValueBufferCallEffects(const BinaryImage &Image, const LowFunc &Caller,
                            const SourceCallOccurrenceKey &Site,
                            const SourceFunctionTypeHint &Signature);
} // namespace neverd
#endif
