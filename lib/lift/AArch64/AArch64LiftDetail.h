//===- AArch64LiftDetail.h - AArch64 lifter internals -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal declarations shared by the AArch64 instruction-lifter
/// translation units: the per-instruction-family handlers that the
/// AArch64Lifter::lift* members chain through, plus the few helpers used
/// by more than one of them.
///
/// Each handler has exactly the shape of the switch it was carved out of:
/// it returns true when it recognized (and lifted) the instruction and
/// false when the instruction is not one of its cases, so the dispatchers
/// can chain them with `||` and keep the original dispatch order.
///
/// This header is an implementation detail of lib/lift/AArch64/ and must
/// NOT be included by code outside that directory.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_AARCH64_AARCH64LIFTDETAIL_H
#define NEVERD_LIFT_AARCH64_AARCH64LIFTDETAIL_H

#include "neverd/lift/AArch64Lifter.h"

#include <capstone/capstone.h>

namespace neverd {

//===----------------------------------------------------------------------===//
// Helpers shared by more than one handler translation unit
//===----------------------------------------------------------------------===//

/// Element size (in bytes) for an AArch64 vector arrangement specifier.
/// Returns 0 if the layout is not a recognized packed vector arrangement.
/// Defined in AArch64LiftCoreNEON.cpp.
unsigned neonElemSize(AArch64Layout_VectorLayout VAS);

/// Byte widths of the IEEE formats a NEON floating-point lane can hold:
/// half (FEAT_FP16), single and double.  Integer arrangements and
/// unrecognized specifiers must not be reinterpreted as a float lane.
inline bool isFPLaneSize(unsigned LaneSz) {
  return LaneSz == 2 || LaneSz == 4 || LaneSz == 8;
}

/// Floating-point element size (bytes) of a NEON vector arrangement, for
/// the complex (FCADD/FCMLA) per-pair lifting.  Returns 0 for
/// non-FP/unknown.  Defined in AArch64LiftSIMDExt.cpp.
unsigned complexElemSize(AArch64Layout_VectorLayout Vas);

/// Broadcast a SIMD logical immediate across every lane and OR / AND-NOT
/// it into the destination register (`orr vD.<T>, #imm` / `bic vD.<T>,
/// #imm`).  Defined in AArch64LiftCore.cpp.
void emitSimdImmLogic(AArch64Lifter::LiftState &S, const cs_aarch64 &ARM64,
                      bool IsBic);

//===----------------------------------------------------------------------===//
// AArch64Lifter::liftCoreNEON handlers (AArch64LiftCoreNEON.cpp)
//===----------------------------------------------------------------------===//

/// NEON move, duplicate and lane transfer.
bool liftNEONMove(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64);

/// NEON integer arithmetic.
bool liftNEONArith(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                   const cs_insn *Insn, const cs_aarch64 &ARM64);

/// NEON multiply-accumulate.
bool liftNEONMulAcc(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                    const cs_insn *Insn, const cs_aarch64 &ARM64);

/// NEON vector compare.
bool liftNEONCompare(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                     const cs_insn *Insn, const cs_aarch64 &ARM64);

/// NEON vector shift.
bool liftNEONShift(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                   const cs_insn *Insn, const cs_aarch64 &ARM64);

/// NEON select, permute and table lookup.
bool liftNEONPermute(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                     const cs_insn *Insn, const cs_aarch64 &ARM64);

/// NEON widening and narrowing.
bool liftNEONWiden(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                   const cs_insn *Insn, const cs_aarch64 &ARM64);

/// NEON unary ops and reductions.
bool liftNEONReduce(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                    const cs_insn *Insn, const cs_aarch64 &ARM64);

/// NEON widening multiply.
bool liftNEONMulLong(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                     const cs_insn *Insn, const cs_aarch64 &ARM64);

/// NEON floating-point ops.
bool liftNEONFloat(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                   const cs_insn *Insn, const cs_aarch64 &ARM64);

/// NEON structured load/store.
bool liftNEONLdSt(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64);

/// NEON misc and scalar carry ops.
bool liftNEONMisc(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64);

//===----------------------------------------------------------------------===//
// AArch64Lifter::liftSIMD handlers (AArch64LiftSIMD.cpp)
//===----------------------------------------------------------------------===//

/// AArch64 cryptographic extension lifter.
bool liftCrypto(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                const cs_insn *Insn, const cs_aarch64 &ARM64);

/// PSTATE flags, MTE and pointer auth.
bool liftPacFlags(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64);

/// Ordered and unprivileged load/store.
bool liftLdStVariant(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                     const cs_insn *Insn, const cs_aarch64 &ARM64);

/// Memory tagging and system ops.
bool liftSysMisc(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                 const cs_insn *Insn, const cs_aarch64 &ARM64);

/// NEON narrowing shifts and pairwise.
bool liftNarrowShift(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                     const cs_insn *Insn, const cs_aarch64 &ARM64);

/// Floating-point conditional compare.
bool liftFPCondCompare(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                       const cs_insn *Insn, const cs_aarch64 &ARM64);

/// Floating-point convert and rounding.
bool liftFPRound(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                 const cs_insn *Insn, const cs_aarch64 &ARM64);

//===----------------------------------------------------------------------===//
// AArch64Lifter::liftSIMDExt handlers (AArch64LiftSIMDExt.cpp)
//===----------------------------------------------------------------------===//

/// BF16 and FP8 conversion lifter.
bool liftBF16(AArch64Lifter &L, AArch64Lifter::LiftState &S,
              const cs_insn *Insn, const cs_aarch64 &ARM64);

/// SVE predicate and length control.
bool liftSVEPredicate(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                      const cs_insn *Insn, const cs_aarch64 &ARM64);

/// SVE load and store.
bool liftSVELdSt(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                 const cs_insn *Insn, const cs_aarch64 &ARM64);

/// SVE integer arithmetic and compare.
bool liftSVEArith(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64);

/// SVE2 widening and narrowing ops.
bool liftSVEWiden(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64);

/// SVE permute and floating-point ops.
bool liftSVEFloat(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64);

/// SME tile ops and FEAT_MOPS copy/set.
bool liftSME(AArch64Lifter &L, AArch64Lifter::LiftState &S, const cs_insn *Insn,
             const cs_aarch64 &ARM64);

//===----------------------------------------------------------------------===//
// AArch64Lifter::liftCore handlers (AArch64LiftCore.cpp)
//===----------------------------------------------------------------------===//

/// AArch64 hint and register move.
bool liftMove(AArch64Lifter &L, AArch64Lifter::LiftState &S,
              const cs_insn *Insn, const cs_aarch64 &ARM64);

/// AArch64 integer add/sub and logic.
bool liftArith(AArch64Lifter &L, AArch64Lifter::LiftState &S,
               const cs_insn *Insn, const cs_aarch64 &ARM64);

/// Shift, multiply/divide and address.
bool liftShiftMulDiv(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                     const cs_insn *Insn, const cs_aarch64 &ARM64);

/// Conditional select and negate.
bool liftCondSel(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                 const cs_insn *Insn, const cs_aarch64 &ARM64);

/// Bitfield move (UBFM/SBFM).
bool liftBitfield(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64);

/// Multiply-accumulate and reversal.
bool liftBitManip(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64);

/// Bit clear, rotate and bitfield insert.
bool liftRotate(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                const cs_insn *Insn, const cs_aarch64 &ARM64);

/// Long multiply and leading sign bits.
bool liftMulLong(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                 const cs_insn *Insn, const cs_aarch64 &ARM64);

/// Conditional compare (CCMP/CCMN).
bool liftCondCompare(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                     const cs_insn *Insn, const cs_aarch64 &ARM64);

//===----------------------------------------------------------------------===//
// AArch64Lifter::liftFP handlers (AArch64LiftFP.cpp)
//===----------------------------------------------------------------------===//

/// Scalar floating-point arithmetic.
bool liftFPArith(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                 const cs_insn *Insn, const cs_aarch64 &ARM64);

/// Float conversion and rounding.
bool liftFPConvert(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                   const cs_insn *Insn, const cs_aarch64 &ARM64);

} // namespace neverd

#endif // NEVERD_LIFT_AARCH64_AARCH64LIFTDETAIL_H
