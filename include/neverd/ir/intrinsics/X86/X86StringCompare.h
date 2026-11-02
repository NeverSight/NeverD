//===- X86StringCompare.h - SSE4.2 string compare intrinsics ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// How the PcmpistrFlag and PcmpestrFlag intrinsics name the status flag of
/// an SSE4.2 string compare they read: the instruction's control byte is the
/// low byte of their immediate operand, and the flag's selector
/// (X86StringCompareFlags.def) is above it.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86STRINGCOMPARE_H
#define NEVERD_IR_INTRINSICS_X86_X86STRINGCOMPARE_H

#include <cstdint>

namespace neverd {

/// The bit where a status-flag intrinsic's selector starts.
constexpr unsigned kX86StringCompareFlagShift = 8;

/// The bits of a status-flag intrinsic's immediate that are the
/// instruction's control byte.
constexpr uint64_t kX86StringCompareControlMask = 0xFF;

/// The letter that ends the C intrinsic reading the flag \p Selector names
/// (`c` for _mm_cmpistrc), or null.
constexpr const char *x86StringCompareFlagSuffix(uint64_t Selector) {
#define X86_STRING_COMPARE_FLAG(SELECTOR, FLAG, SUFFIX)                        \
  if (Selector == SELECTOR)                                                    \
    return SUFFIX;
#include "neverd/ir/intrinsics/X86/X86StringCompareFlags.def"
  return nullptr;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86STRINGCOMPARE_H
