//===- X86CpuidLeaves.h - CPUID leaves without a sub-leaf -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The CPUID leaves whose results do not depend on the sub-leaf in ECX.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X86CPUIDLEAVES_H
#define NEVERD_IR_X86_X86CPUIDLEAVES_H

#include <cstdint>

namespace neverd {

/// Whether CPUID leaf \p Leaf ignores ECX.  Intel's SDM (CPUID, Vol. 2A)
/// and AMD's APM (Vol. 3, appendix E) give no sub-leaf for the maximum
/// basic leaf and vendor (0), the version and feature flags (1), the cache
/// and TLB descriptors (2), the processor serial number (3), MONITOR/MWAIT
/// (5), thermal and power management (6), and the extended leaves 80000000H
/// to 80000008H.  Every other leaf may read ECX, as 4, 7, 0BH and 0DH do, or
/// is no leaf the manuals give: it keeps its sub-leaf.
constexpr bool x86CpuidLeafIgnoresSubleaf(uint64_t Leaf) {
  switch (Leaf) {
  case 0x0:
  case 0x1:
  case 0x2:
  case 0x3:
  case 0x5:
  case 0x6:
    return true;
  default:
    return Leaf >= 0x80000000u && Leaf <= 0x80000008u;
  }
}

} // namespace neverd

#endif // NEVERD_IR_X86_X86CPUIDLEAVES_H
