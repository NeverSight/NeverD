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

/// The same necessary condition for x86-64, whose return address the caller's
/// CALL leaves on the stack: every reaching path must leave RSP at its
/// incoming value before this exact branch, or else at nothing derived from
/// it, where the branch resumes saved state on another stack.  PLT0's jump to
/// the lazy-binding resolver, which receives the word PLT0 pushed, qualifies
/// too.  A branch to an address that may derive from this function's own code,
/// or from a read-only table whose entries name it, stays within the function
/// and never qualifies.  OwnedCode is positive ownership metadata, as for
/// AArch64.  It establishes no callee and no jump-table domain; an incomplete
/// proof grants nothing.
std::set<va_t> restoredX64IndirectTailFrames(
    const LowFunc &Function, const BinaryImage &Image,
    ExceptionAddressRange OwnedCode = {},
    size_t MaxWork = limits::kMaxIndirectTailFrameWork);

} // namespace neverd
#endif
