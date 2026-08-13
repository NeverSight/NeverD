//===- AArch64LiftSIMDExt.cpp - AArch64 BF16/SVE/SME lifter dispatch ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dispatches BF16, SVE/SVE2, SME and FEAT_MOPS instructions to the
/// per-family handlers, and defines the complex-arithmetic element size
/// helper they share.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

// Floating-point element size (bytes) of a NEON vector arrangement, for the
// complex (FCADD/FCMLA) per-pair lifting.  Returns 0 for non-FP/unknown.
unsigned complexElemSize(AArch64Layout_VectorLayout Vas) {
  switch (Vas) {
  case AARCH64LAYOUT_VL_8H:
  case AARCH64LAYOUT_VL_4H:
    return 2;
  case AARCH64LAYOUT_VL_4S:
  case AARCH64LAYOUT_VL_2S:
    return 4;
  case AARCH64LAYOUT_VL_2D:
    return 8;
  default:
    return 0;
  }
}

bool AArch64Lifter::liftSIMDExt(LiftState &S, const cs_insn *Insn,
                                const cs_aarch64 &ARM64) {
  return liftBF16(*this, S, Insn, ARM64) ||
         liftSVEPredicate(*this, S, Insn, ARM64) ||
         liftSVELdSt(*this, S, Insn, ARM64) ||
         liftSVEArith(*this, S, Insn, ARM64) ||
         liftSVEWiden(*this, S, Insn, ARM64) ||
         liftSVEFloat(*this, S, Insn, ARM64) || liftSME(*this, S, Insn, ARM64);
}

} // namespace neverd
