//===- ARMLiftMul.cpp - ARM32 multiply/divide instruction lifter ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM32 multiply and divide instruction handlers: MUL, MLA, MLS, SDIV, UDIV,
/// SMULL, UMULL, and all signed/unsigned multiply-accumulate long, halfword
/// multiply, dual multiply, and saturating multiply variants.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool ARMLifter::liftMul(LiftState &S, const cs_insn *Insn, const cs_arm &ARM) {
  return liftMulBasic(*this, S, Insn, ARM) ||
         liftMulHalfAccum(*this, S, Insn, ARM) ||
         liftMulHalfLong(*this, S, Insn, ARM) ||
         liftMulHalfProduct(*this, S, Insn, ARM) ||
         liftMulHigh(*this, S, Insn, ARM);
}

} // namespace neverd
