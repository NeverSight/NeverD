//===- ARMLiftCore.cpp - ARM32 core instruction lifter ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Core ARM32 integer ALU instruction handlers: MOV, arithmetic, logic,
/// shifts, rotates, extensions, bit manipulation, packed arithmetic,
/// saturating operations, and conditional select.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool ARMLifter::liftCore(LiftState &S, const cs_insn *Insn, const cs_arm &ARM) {
  return liftCoreMove(*this, S, Insn, ARM) ||
         liftCoreArith(*this, S, Insn, ARM) ||
         liftCoreLogic(*this, S, Insn, ARM) ||
         liftCoreShift(*this, S, Insn, ARM) ||
         liftCoreBit(*this, S, Insn, ARM);
}

} // namespace neverd
