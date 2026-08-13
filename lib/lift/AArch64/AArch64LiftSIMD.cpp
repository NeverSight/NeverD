//===- AArch64LiftSIMD.cpp - AArch64 crypto/system lifter dispatch --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dispatches the cryptographic extension, PSTATE/MTE/pointer-auth,
/// ordered load/store, system, NEON narrowing-shift and floating-point
/// extension handlers.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool AArch64Lifter::liftSIMD(LiftState &S, const cs_insn *Insn,
                             const cs_aarch64 &ARM64) {
  return liftCrypto(*this, S, Insn, ARM64) ||
         liftPacFlags(*this, S, Insn, ARM64) ||
         liftLdStVariant(*this, S, Insn, ARM64) ||
         liftSysMisc(*this, S, Insn, ARM64) ||
         liftNarrowShift(*this, S, Insn, ARM64) ||
         liftFPCondCompare(*this, S, Insn, ARM64) ||
         liftFPRound(*this, S, Insn, ARM64);
}

} // namespace neverd
