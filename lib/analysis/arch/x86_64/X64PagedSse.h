//===- X64PagedSse.h - Legacy SSE paged admission -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Own the fault domain of legacy bitvector steps. LowIR still owns values;
/// the paged memory transaction owns whole-operand access and publication.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDSSE_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDSSE_H

#include "neverd/analysis/arch/x86_64/X64PagedExecution.h"

#include "capstone/x86.h"

#include <cstdint>

namespace neverd::analysis::detail {

// The lifter's complete undefined-output audit already authenticates the
// original legacy encoding. Restrict this fault model to unaligned moves and
// register XOR; memory XOR needs its separate #GP alignment precondition.
inline bool x64PagedSseInstruction(unsigned Id, const cs_x86 &X86) {
  return Id == X86_INS_MOVUPS || Id == X86_INS_MOVDQU ||
         (Id == X86_INS_XORPS && X86.op_count == 2 &&
          X86.operands[1].type == X86_OP_REG);
}

// Intel SDM Vol. 3A Table 15-1: #UD takes precedence over CR0.TS #NM.
// The long-mode target supports SSE/SSE2. Moves/XOR have no SIMD FP exception;
// MXCSR masks and CR4.OSXMMEXCPT therefore do not select an exception here.
constexpr uint8_t x64PagedSseEntryFault(uint64_t CR0, uint64_t CR4) {
  if ((CR0 & 4) || !(CR4 & 0x200))
    return 6;
  return CR0 & 8 ? 7 : 0;
}

constexpr unsigned x64PagedSseAlignmentBytes(X64PagedSseAlignment Profile) {
  switch (Profile) {
  case X64PagedSseAlignment::NoCheck:
    return 1;
  case X64PagedSseAlignment::Qword:
    return 8;
  case X64PagedSseAlignment::FullOperand:
    return 16;
  }
  return 0;
}
} // namespace neverd::analysis::detail
#endif // NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDSSE_H
