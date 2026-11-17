#ifndef NEVERD_IR_LOW_INDIRECTTAILFRAME_H
#define NEVERD_IR_LOW_INDIRECTTAILFRAME_H

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/ExceptionCommon.h"

namespace neverd {

struct BinaryImage;

/// Necessary frame condition for the existing AArch64 indirect-tail heuristic.
/// Under the native call ABI, every reaching path must restore the incoming SP
/// and link word before this exact branch. This does not establish a callee,
/// source declaration, or jump-table domain. An incomplete proof grants
/// nothing. OwnedCode is positive function ownership metadata, not a guessed
/// next-entry bound; it retains local targets that the graph has not decoded.
/// Image retains local-label table provenance through indexed loads, without
/// proving their selector domain or publishing any switch targets.
std::set<va_t> restoredAArch64IndirectTailFrames(
    const LowFunc &Function, BinaryFormat Format,
    ExceptionAddressRange OwnedCode = {},
    size_t MaxWork = limits::kMaxIndirectTailFrameWork,
    const BinaryImage *Image = nullptr);

} // namespace neverd
#endif
