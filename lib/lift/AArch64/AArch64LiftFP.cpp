//===- AArch64LiftFP.cpp - AArch64 floating-point lifter dispatch ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dispatches scalar floating-point instructions to the arithmetic and
/// conversion handlers.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool AArch64Lifter::liftFP(LiftState &S, const cs_insn *Insn,
                           const cs_aarch64 &ARM64) {
  return liftFPArith(*this, S, Insn, ARM64) ||
         liftFPConvert(*this, S, Insn, ARM64);
}

} // namespace neverd
