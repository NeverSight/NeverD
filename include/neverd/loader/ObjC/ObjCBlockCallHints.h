#ifndef NEVERD_LOADER_OBJC_OBJCBLOCKCALLHINTS_H
#define NEVERD_LOADER_OBJC_OBJCBLOCKCALLHINTS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <map>

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Recover fixed scalar source dispatch from an exact block+16 target load and
/// the same hidden receiver. Inferred signatures describe observed machine
/// carriers, not original block declarations or authenticated ABI evidence.
std::map<va_t, SourceCallTypeHint>
buildObjCBlockCallHints(const BinaryImage &Image, const LowFunc &Function,
                        const SourceFunctionTypeHint *EntrySignature = nullptr);
} // namespace neverd

#endif
