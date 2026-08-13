//===- MedFlagsDetail.h - Shared flag pattern helpers ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal types and utilities shared between the architecture-generic
/// flag elimination framework (MedFlags.cpp) and per-target compound
/// flag pattern matchers (MedFlagsX86.cpp, etc.).
///
/// This header is an implementation detail of the med/ library and
/// should NOT be included by code outside lib/ir/med/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDFLAGSDETAIL_H
#define NEVERD_IR_MED_MEDFLAGSDETAIL_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

#include <vector>

namespace neverd {

/// Classification of a single sub-expression in a compound flag pattern.
enum class FlagSubPat {
  None,
  DirectFlag,
  InvertedFlag,
  FlagsEqual,
  FlagsNotEqual,
};

/// Result of classifying one operand of a BOOL_AND / BOOL_OR expression.
struct SubPatInfo {
  FlagSubPat Pat = FlagSubPat::None;
  uint64_t FlagOff = 0;
};

/// Classify a sub-expression within a compound flag pattern.
/// Walks backward from \p SearchStart looking for the definition of \p Var
/// and determines whether it is a direct flag read, inverted flag, or a
/// flag-equality test.
SubPatInfo classifySubPat(const std::vector<MedOp> &Ops, const MedVar &Var,
                          int SearchStart);

/// Check whether both inputs of \p Op are flag or constant variables.
bool areBothFlags(const MedOp &Op);

/// Whether \p Op is any floating-point comparison opcode.  A scalar FP compare
/// (COMISS/COMISD/UCOMISx/FCOMI/FUCOMI/FCOM/FTST) writes EFLAGS through a
/// BOOL_OR over these temps, so the flag-folding passes must treat *any* of
/// them sitting between a flag consumer and an earlier integer CMP as evidence
/// the flag is FP-defined and leave the condition unfolded.  Listed in one
/// place so a new FP-compare lowering can never silently slip past the guard.
inline bool isFloatCompareOpcode(NdOp Opcode) {
  return Opcode == NdOp::FLOAT_EQUAL || Opcode == NdOp::FLOAT_NOTEQUAL ||
         Opcode == NdOp::FLOAT_LESS || Opcode == NdOp::FLOAT_LESSEQUAL ||
         Opcode == NdOp::FLOAT_ISNAN;
}

/// Operands of the comparison (CMP/TEST/SUB/AND) that produced a flag chain,
/// recovered by findCmpSource().
struct CmpSource {
  MedVar A, B;
  bool Valid = false;
  /// True only when the operands came from a genuine INT_SUB (a real CMP/SUB).
  /// Carry/overflow conditions (ULT/ULE/UGT/UGE/VS) can only be reconstructed
  /// as a comparison from a real subtraction; for any other flag source they
  /// must be left unfolded.
  bool FromSub = false;
};

// ---- Architecture-specific compound flag pattern resolvers ----

/// x86 EFLAGS compound patterns (MedFlagsX86.cpp).
///
/// Recognizes multi-flag conditions:
///   !CF && !ZF    → UGT   (above)
///   !ZF && SF==OF → SGT   (greater)
///   CF  || ZF     → ULE   (below or equal)
///   ZF  || SF!=OF → SLE   (less or equal)
CondCode resolveCompoundFlagPatternX86(const std::vector<MedOp> &Ops,
                                       const MedOp &Def, int DefIdx,
                                       const TargetRegInfo &TRI);

/// x86 carry/overflow conditions (CF for ULT/ULE/UGT/UGE, OF for VS) may only
/// be folded against a CMP that actually produced that flag.  A real x86 CMP
/// lifts CF as the unsigned borrow INT_LESS(A,B) and OF as INT_SBOR(A,B);
/// `bt`, shifts and similar overwrite the same flag bit from unrelated
/// operands.  Returns true when the condition's flag may be folded against
/// \p Cmp, false when it must be left for the emitter.  (MedFlagsX86.cpp)
bool carryFlagMatchesCmpX86(const std::vector<MedOp> &Ops, int ConsumerIdx,
                            CondCode CC, const CmpSource &Cmp,
                            const TargetRegInfo &TRI);

/// ARM family (ARM32 + AArch64) NZCV compound patterns (MedFlagsARM.cpp).
///
/// ARM C flag has inverted polarity vs x86 CF (C=1 → unsigned >=).
/// Recognizes:
///    C && !Z   → UGT   (HI)
///   !Z && N==V → SGT   (GT)
///   !C ||  Z   → ULE   (LS)
///    Z || N!=V → SLE   (LE)
CondCode resolveCompoundFlagPatternARM(const std::vector<MedOp> &Ops,
                                       const MedOp &Def, int DefIdx,
                                       const TargetRegInfo &TRI);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDFLAGSDETAIL_H
