//===- ARMLiftSIMD.cpp - ARM32 VFP/NEON instruction lifter --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// VFP and NEON (Advanced SIMD) instruction handlers for ARM32.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool ARMLifter::liftSIMD(LiftState &S, const cs_insn *Insn, const cs_arm &ARM) {
  return liftSIMDArith(*this, S, Insn, ARM) ||
         liftSIMDMove(*this, S, Insn, ARM) ||
         liftSIMDConvert(*this, S, Insn, ARM) ||
         liftSIMDFMA(*this, S, Insn, ARM) ||
         liftSIMDSelect(*this, S, Insn, ARM) ||
         liftSIMDMem(*this, S, Insn, ARM);
}

} // namespace neverd
