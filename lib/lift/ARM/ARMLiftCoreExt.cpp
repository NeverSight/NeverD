//===- ARMLiftCoreExt.cpp - ARM32 extended integer instruction lifter -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Additional ARM32 integer instruction handlers: ADR, BFC, BFI, REV16,
/// REVSH, RBIT, RRX, PKH*, SDIV, UDIV, SEL, and remaining
/// integer/system instructions.
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

bool ARMLifter::liftCoreExt(LiftState &S, const cs_insn *Insn,
                            const cs_arm &ARM) {
  return liftCoreExtBit(*this, S, Insn, ARM) ||
         liftCoreExtExtend(*this, S, Insn, ARM) ||
         liftCoreExtSat(*this, S, Insn, ARM) ||
         liftCoreExtPack(*this, S, Insn, ARM) ||
         liftCoreExtMisc(*this, S, Insn, ARM);
}

} // namespace neverd
