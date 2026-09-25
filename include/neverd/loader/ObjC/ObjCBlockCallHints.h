#ifndef NEVERD_LOADER_OBJC_OBJCBLOCKCALLHINTS_H
#define NEVERD_LOADER_OBJC_OBJCBLOCKCALLHINTS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <map>
#include <set>

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Descriptor-backed capture words for one block invoke entry. BlockWords
/// additionally have a validated copy-helper assignment with block flag 7.
/// These facts describe storage and machine carriers, not callback types.
struct ObjCBlockCaptureCallFields {
  std::set<uint64_t> ScalarWords;
  std::set<uint64_t> BlockWords;
  bool operator==(const ObjCBlockCaptureCallFields &) const = default;
};

/// Recover fixed scalar source dispatch from an exact block+16 target load and
/// the same hidden receiver. Inferred signatures describe observed machine
/// carriers, not original block declarations or authenticated ABI evidence.
std::map<va_t, SourceCallTypeHint> buildObjCBlockCallHints(
    const BinaryImage &Image, const LowFunc &Function,
    const SourceFunctionTypeHint *EntrySignature = nullptr,
    const std::map<va_t, SourceCallTypeHint> *BoundCalls = nullptr,
    const ObjCBlockCaptureCallFields *Captures = nullptr);
} // namespace neverd

#endif
