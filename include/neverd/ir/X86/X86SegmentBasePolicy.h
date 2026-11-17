//===- X86SegmentBasePolicy.h - Null selector base choices ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit processor choice for the architectural base after null FS/GS.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X86SEGMENTBASEPOLICY_H
#define NEVERD_IR_X86_X86SEGMENTBASEPOLICY_H

#include <cstdint>

namespace neverd {

/// No vendor-wide default is inferred. The selector becomes unusable and
/// ignored limit/attribute bytes remain opaque. The complete FS/GS base
/// follows this declared choice, including when later entering code64.
enum class X86NullSegmentBasePolicy : uint8_t { Clear, Preserve };

} // namespace neverd

#endif // NEVERD_IR_X86_X86SEGMENTBASEPOLICY_H
