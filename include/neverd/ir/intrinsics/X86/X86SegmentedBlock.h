//===- X86SegmentedBlock.h - i386 segmented block transactions -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// MOVDIR64B and ENQCMD/S consume offsets, not host pointers. The native
/// instruction owns both source segment checks and the implicit ES destination.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86SEGMENTEDBLOCK_H
#define NEVERD_IR_INTRINSICS_X86_X86SEGMENTEDBLOCK_H

#include <cstdint>

namespace neverd {

enum class X86SegmentedBlockKind : uint16_t {
  DirectStore,
  Enqueue,
  EnqueueSupervisor
};
enum class X86BlockSegment : uint16_t { ES, CS, SS, DS, FS, GS };

// Serialized selector: operation [1:0], source segment [4:2], addr16 [5].
inline constexpr uint16_t X86SegmentedBlockSelectorLimit = 64;
inline constexpr uint16_t X86SegmentedBlockAddress16 = 32;

constexpr uint16_t x86SegmentedBlockSelector(X86SegmentedBlockKind Kind,
                                             X86BlockSegment Segment,
                                             bool Address16) {
  return static_cast<uint16_t>(Kind) | (static_cast<uint16_t>(Segment) << 2) |
         (Address16 ? X86SegmentedBlockAddress16 : 0);
}

constexpr bool x86SegmentedBlockSelectorIsValid(uint16_t Selector) {
  return Selector < X86SegmentedBlockSelectorLimit && (Selector & 3) < 3 &&
         ((Selector >> 2) & 7) < 6;
}

constexpr bool x86SegmentedBlockHasFlag(uint16_t Selector) {
  return (Selector & 3) != 0;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86SEGMENTEDBLOCK_H
