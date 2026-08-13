//===- X86LiftSIMDLegacy.cpp - x86/x64 legacy/extension instruction lifter ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dispatches the legacy, vendor-specific and extension instructions to the
/// per-family handlers in X86LiftLegacy*.cpp: BCD, 3DNow!, TBM, CET, VIA
/// PadLock, GFNI, LWP, AMD XOP, SSE4a, and the remaining SSE/MMX/system
/// instructions.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool X86Lifter::liftSIMDLegacy(LiftState &S, const cs_insn *Insn,
                               const cs_x86 &X86) {
  // ========================================================================
  // P2: Remaining instructions — BCD, legacy, 3DNow!, TBM, CET, VIA,
  //     x87 misc, LWP, GFNI, system, xsave64, misc new extensions.
  // ========================================================================
  return liftLegacyBCD(*this, S, Insn, X86) ||
         liftLegacyExt(*this, S, Insn, X86) ||
         liftLegacyXOP(*this, S, Insn, X86) ||
         liftLegacySSEInt(*this, S, Insn, X86) ||
         liftLegacySSEFloat(*this, S, Insn, X86);
}

} // namespace neverd
