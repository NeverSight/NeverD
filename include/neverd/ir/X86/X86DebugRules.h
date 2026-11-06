//===- X86DebugRules.h - Instruction debug status -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared nontransactional TF/ICEBP status with inactive debug comparators.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_X86_X86DEBUGRULES_H
#define NEVERD_IR_X86_X86DEBUGRULES_H

#include <cstdint>

namespace neverd {

inline bool x86InstructionDebugValid(uint64_t DR6, uint64_t DR7,
                                     uint64_t DebugCTL) {
  return DR7 == 0x400 && DebugCTL == 0 && (DR6 & 0xffffffff00000000ULL) == 0 &&
         (DR6 & 0xfffe07f0u) == 0xfffe07f0u && !(DR6 & 0x1000);
}

/// Inactive B0..B3 clear, RTM becomes one, sticky status survives. ICEBP does
/// not set BS merely because entry TF was set. Historical BLD/RTM inputs
/// require a target capable of representing those statuses.
inline uint64_t x86InstructionDebugStatus(uint64_t DR6, bool SingleStep) {
  return (DR6 & ~uint64_t{0xf}) | 0x10000 | (SingleStep ? 0x4000 : 0);
}

} // namespace neverd
#endif // NEVERD_IR_X86_X86DEBUGRULES_H
