//===- X86LiftSIMD.cpp - x86/x64 SIMD instruction lifter ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dispatches SSE/AVX (VEX) SIMD instructions to the per-family handlers in
/// X86LiftSIMD*.cpp.  AVX-512, FMA and gather live in X86LiftSIMDAVX.cpp;
/// baseline SSE and the vendor extensions live in X86LiftSIMDLegacy.cpp.
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

bool X86Lifter::liftSIMD(LiftState &S, const cs_insn *Insn, const cs_x86 &X86) {
  return liftAMX(*this, S, Insn, X86) ||
         liftSIMDMove(*this, S, Insn, X86) ||
         liftSIMDShuffle(*this, S, Insn, X86) ||
         liftSIMDCompare(*this, S, Insn, X86) ||
         liftSIMDIntArith(*this, S, Insn, X86) ||
         liftSIMDIntMul(*this, S, Insn, X86) ||
         liftSIMDIntHorizontal(*this, S, Insn, X86) ||
         liftSIMDFloatArith(*this, S, Insn, X86) ||
         liftSIMDLogicShift(*this, S, Insn, X86) ||
         liftSIMDConvert(*this, S, Insn, X86) ||
         liftSIMDCrypto(*this, S, Insn, X86);
}

} // namespace neverd
