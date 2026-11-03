//===- X86LegacySegments.h - Cached protected-mode segments ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check an operand against an authenticated, already loaded segment cache.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X86LEGACYSEGMENTS_H
#define NEVERD_IR_X86_X86LEGACYSEGMENTS_H

#include "neverd/ir/intrinsics/X86/X86SegmentRegisters.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd {

struct X86LegacySegmentCache {
  uint32_t Base = 0;
  /// Effective byte limit after descriptor granularity scaling.
  uint32_t Limit = 0;
  uint16_t Selector = 0;
  /// Descriptor access byte at bits 7:0 and AVL/L/D/B/G at bits 15:12.
  uint16_t Attributes = 0;
  /// Independent of Selector: a usable zero selector can survive entering
  /// protected mode without reloading the hidden cache.
  bool Unusable = true;
};

enum class X86SegmentAccessKind : uint8_t { Read, Write, Execute };

/// Intel SDM 3A 6.3 leaves an operand crossing a full 4-GiB expand-up limit
/// implementation-dependent, even between executions. Bind this choice for
/// the particular operation; do not infer a universal policy from the host.
enum class X86FullSegmentLimitChoice : uint8_t { Fault, Wrap };

struct X86LegacySegmentAccess {
  X86SegmentRegister Segment;
  X86SegmentAccessKind Kind;
  /// Effective offset, already truncated to the instruction's address size.
  uint32_t Offset;
  uint32_t Bytes;
  std::optional<X86FullSegmentLimitChoice> FullLimitChoice;
};

struct X86LegacySegmentResult {
  uint32_t Linear = 0;
  /// Only #GP(0) or #SS(0). No paging or descriptor-load effects occur here.
  std::optional<uint8_t> FaultVector;
};

/// Validate external cache snapshots. Descriptor loading, privilege checks,
/// present checks, accessed writes and cache invalidation belong to the
/// instruction that installs the cache, not to each subsequent operand.
llvm::Error validateX86LegacySegmentCache(X86SegmentRegister Segment,
                                          const X86LegacySegmentCache &Cache);

/// Protected 16/32-bit operands only. Long mode, real mode and VM86 are not
/// inferred from cache bits. Checks the whole offset extent before base
/// addition; successful linear addition wraps modulo 2^32. The memory owner
/// must preserve that wrap across all operand bytes as well.
llvm::Expected<X86LegacySegmentResult>
checkX86LegacySegmentAccess(const X86LegacySegmentCache &Cache,
                            const X86LegacySegmentAccess &Access);

} // namespace neverd

#endif // NEVERD_IR_X86_X86LEGACYSEGMENTS_H
