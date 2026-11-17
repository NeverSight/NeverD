//===- X86SegmentDescriptorProfile.h - Descriptor faults -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit processor choice for a descriptor's noncanonical trailing bytes.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_X86_X86SEGMENTDESCRIPTORPROFILE_H
#define NEVERD_IR_X86_X86SEGMENTDESCRIPTORPROFILE_H

#include <cstdint>

namespace neverd {

/// A noncanonical descriptor start raises #GP(selector). When only its tail
/// is noncanonical, the bound processor reports either #GP(0) or #GP(selector).
/// Both profiles check the complete extent before page-table access. Event
/// delivery retains EXT in either error code; only the selector part differs.
enum class X86DescriptorCanonicalityProfile : uint8_t {
  SelectorForStart,
  SelectorForRange,
};

constexpr bool
x86DescriptorCanonicalityValid(X86DescriptorCanonicalityProfile Profile) {
  return Profile == X86DescriptorCanonicalityProfile::SelectorForStart ||
         Profile == X86DescriptorCanonicalityProfile::SelectorForRange;
}

} // namespace neverd

#endif // NEVERD_IR_X86_X86SEGMENTDESCRIPTORPROFILE_H
