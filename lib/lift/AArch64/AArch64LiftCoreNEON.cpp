//===- AArch64LiftCoreNEON.cpp - AArch64 NEON lifter dispatch -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dispatches NEON (Advanced SIMD) instructions to the per-family
/// handlers in AArch64LiftNEON*.cpp, and defines the vector arrangement
/// element-size helper they share.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

// Element size (in bytes) for an AArch64 vector arrangement specifier.
// Returns 0 if the layout is not a recognized packed vector arrangement.
unsigned neonElemSize(AArch64Layout_VectorLayout VAS) {
  switch (VAS) {
  // Bare element layouts (no lane-count prefix): used by the single-element
  // indexed load/store forms `ld1/st1 {v.<T>}[idx]` and scalar forms.  capstone
  // reports e.g. `ld1 {v0.s}[1]` with vas = AARCH64LAYOUT_VL_S (not VL_4S), so
  // these must be recognized or the indexed branch falls back to a full-vector
  // load that clobbers the other lanes.
  case AARCH64LAYOUT_VL_B:
    return 1;
  case AARCH64LAYOUT_VL_H:
    return 2;
  case AARCH64LAYOUT_VL_S:
    return 4;
  case AARCH64LAYOUT_VL_D:
    return 8;
  case AARCH64LAYOUT_VL_Q:
    return 16;
  case AARCH64LAYOUT_VL_16B:
  case AARCH64LAYOUT_VL_8B:
    return 1;
  case AARCH64LAYOUT_VL_8H:
  case AARCH64LAYOUT_VL_4H:
    return 2;
  case AARCH64LAYOUT_VL_4S:
  case AARCH64LAYOUT_VL_2S:
    return 4;
  case AARCH64LAYOUT_VL_2D:
  case AARCH64LAYOUT_VL_1D:
    return 8;
  case AARCH64LAYOUT_VL_1Q:
    return 16;
  default:
    return 0;
  }
}

bool AArch64Lifter::liftCoreNEON(LiftState &S, const cs_insn *Insn,
                                 const cs_aarch64 &ARM64) {
  return liftNEONMove(*this, S, Insn, ARM64) ||
         liftNEONArith(*this, S, Insn, ARM64) ||
         liftNEONMulAcc(*this, S, Insn, ARM64) ||
         liftNEONCompare(*this, S, Insn, ARM64) ||
         liftNEONShift(*this, S, Insn, ARM64) ||
         liftNEONPermute(*this, S, Insn, ARM64) ||
         liftNEONWiden(*this, S, Insn, ARM64) ||
         liftNEONReduce(*this, S, Insn, ARM64) ||
         liftNEONMulLong(*this, S, Insn, ARM64) ||
         liftNEONFloat(*this, S, Insn, ARM64) ||
         liftNEONLdSt(*this, S, Insn, ARM64) ||
         liftNEONMisc(*this, S, Insn, ARM64);
}

} // namespace neverd
