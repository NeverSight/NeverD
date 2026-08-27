//===- MedFlagsX86.cpp - x86 EFLAGS compound pattern matching ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86-specific compound flag condition patterns for the MedIR flag
/// elimination pass.  Matches multi-flag BOOL_AND / BOOL_OR chains
/// produced by NdOp lifting of CMP/TEST instructions against the
/// x86 EFLAGS semantics (CF=borrow, ZF=zero, SF=sign, OF=overflow).
///
/// Recognized patterns:
///   !CF && !ZF    → UGT   (JA  / SETA)
///   !ZF && SF==OF → SGT   (JG  / SETG)
///   CF  || ZF     → ULE   (JBE / SETBE)
///   ZF  || SF!=OF → SLE   (JLE / SETLE)
///
//===----------------------------------------------------------------------===//

#include "MedFlagsDetail.h"

namespace neverd {

CondCode resolveCompoundFlagPatternX86(const std::vector<MedOp> &Ops,
                                       const MedOp &Def, int DefIdx,
                                       const TargetRegInfo &TRI) {
  if (Def.NumInputs < 2)
    return CondCode::Invalid;

  auto SP0 = classifySubPat(Ops, Def.Inputs[0], DefIdx - 1);
  auto SP1 = classifySubPat(Ops, Def.Inputs[1], DefIdx - 1);

  if (Def.Opcode == NdOp::BOOL_AND) {
    // !CF && !ZF → UGT (above)
    if (SP0.Pat == FlagSubPat::InvertedFlag &&
        SP1.Pat == FlagSubPat::InvertedFlag) {
      bool HasCF = (SP0.FlagOff == TRI.FlagCF) || (SP1.FlagOff == TRI.FlagCF);
      bool HasZF = (SP0.FlagOff == TRI.FlagZF) || (SP1.FlagOff == TRI.FlagZF);
      if (HasCF && HasZF)
        return CondCode::UGT;
    }
    // !ZF && (SF==OF) → SGT (greater)
    bool HasNotZ =
        (SP0.Pat == FlagSubPat::InvertedFlag && SP0.FlagOff == TRI.FlagZF) ||
        (SP1.Pat == FlagSubPat::InvertedFlag && SP1.FlagOff == TRI.FlagZF);
    bool HasNEqV = (SP0.Pat == FlagSubPat::FlagsEqual) ||
                   (SP1.Pat == FlagSubPat::FlagsEqual);
    if (HasNotZ && HasNEqV)
      return CondCode::SGT;
  }

  if (Def.Opcode == NdOp::BOOL_OR) {
    // CF || ZF → ULE (below or equal)
    if (SP0.Pat == FlagSubPat::DirectFlag &&
        SP1.Pat == FlagSubPat::DirectFlag) {
      bool HasCF = (SP0.FlagOff == TRI.FlagCF) || (SP1.FlagOff == TRI.FlagCF);
      bool HasZF = (SP0.FlagOff == TRI.FlagZF) || (SP1.FlagOff == TRI.FlagZF);
      if (HasCF && HasZF)
        return CondCode::ULE;
    }
    // ZF || (SF!=OF) → SLE (less or equal)
    bool HasZ =
        (SP0.Pat == FlagSubPat::DirectFlag && SP0.FlagOff == TRI.FlagZF) ||
        (SP1.Pat == FlagSubPat::DirectFlag && SP1.FlagOff == TRI.FlagZF);
    bool HasNNeV = (SP0.Pat == FlagSubPat::FlagsNotEqual) ||
                   (SP1.Pat == FlagSubPat::FlagsNotEqual);
    if (HasZ && HasNNeV)
      return CondCode::SLE;
  }

  return CondCode::Invalid;
}

bool carryFlagMatchesCmpX86(const std::vector<MedOp> &Ops, int ConsumerIdx,
                            CondCode CC, const CmpSource &Cmp,
                            const TargetRegInfo &TRI) {
  uint64_t FlagOff;
  NdOp BorrowOp;
  if (CC == CondCode::ULT || CC == CondCode::ULE || CC == CondCode::UGT ||
      CC == CondCode::UGE) {
    FlagOff = TRI.FlagCF;
    BorrowOp = NdOp::INT_LESS;
    // Signed relations consume SF^OF (plus ZF for LE/GT), so their fold is only
    // valid when the nearest OF came directly from this CMP's subtraction.  SBB
    // composes OF from two INT_SBOR terms through BOOL_XOR; collapsing that
    // chain to A <s (B + borrow) is wrong when the addition itself overflows.
  } else if (CC == CondCode::VS || CC == CondCode::SLT || CC == CondCode::SGE ||
             CC == CondCode::SLE || CC == CondCode::SGT) {
    FlagOff = TRI.FlagVF;
    BorrowOp = NdOp::INT_SBOR;
  } else {
    return true; // not a carry/overflow condition
  }
  for (int J = ConsumerIdx; J >= 0; --J) {
    const auto &Def = Ops[J];
    if (Def.Output.Kind != MedVar::Flag || Def.Output.RegOff != FlagOff)
      continue;
    if (Def.Opcode != BorrowOp || Def.NumInputs < 2)
      return false;
    return (Def.Inputs[0] == Cmp.A && Def.Inputs[1] == Cmp.B) ||
           (Def.Inputs[0] == Cmp.B && Def.Inputs[1] == Cmp.A);
  }
  return false; // flag is live-in/loop-carried — not this block's CMP
}

} // namespace neverd
