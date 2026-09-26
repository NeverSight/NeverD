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

#include <optional>

namespace neverd {

namespace {

bool isSelfLowSliceOf(const MedOp &Op, const MedVar &Base) {
  return Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
         Op.Output.Kind == MedVar::Reg && Base.Kind == MedVar::Reg &&
         Op.Inputs[0] == Base && Op.Inputs[0].Size == Base.Size &&
         Op.Output.RegOff == Base.RegOff &&
         Op.Output.Size < Base.Size && Op.Inputs[1].isConst() &&
         Op.Inputs[1].ConstVal == 0;
}

std::optional<int> selfLowSliceDef(const std::vector<MedOp> &Ops,
                                   const MedVar &Value, int Before) {
  for (int I = Before - 1; I >= 0; --I) {
    const MedOp &Op = Ops[I];
    if (Op.Output != Value || Op.Output.Size != Value.Size ||
        Op.Output.RegOff != Value.RegOff)
      continue;
    if (Op.NumInputs < 1 || !isSelfLowSliceOf(Op, Op.Inputs[0]))
      return std::nullopt;
    return I;
  }
  return std::nullopt;
}

bool sameSignedCmpSliceWindow(const std::vector<MedOp> &Ops, int ConsumerIdx,
                              int OverflowIdx, const CmpSource &Cmp,
                              CondCode CC, const TargetRegInfo &TRI) {
  if ((CC != CondCode::SLT && CC != CondCode::SGE &&
      CC != CondCode::SLE && CC != CondCode::SGT) ||
      !Cmp.Valid || !Cmp.FromSub || Cmp.SourceOpIndex < 0 ||
      Cmp.SourceOpIndex >= OverflowIdx || OverflowIdx < 0 ||
      ConsumerIdx < OverflowIdx ||
      static_cast<std::vector<MedOp>::size_type>(ConsumerIdx) >= Ops.size() ||
      Cmp.A.Kind != MedVar::Reg || Cmp.A.Size != 4 ||
      Cmp.B.Size != 4 || Cmp.Result.Size != 4)
    return false;

  const MedOp &Sub = Ops[Cmp.SourceOpIndex];
  const MedOp &Overflow = Ops[OverflowIdx];
  if (Sub.Opcode != NdOp::INT_SUB || Sub.NumInputs < 2 ||
      Sub.Output != Cmp.Result || Sub.Output.Size != Cmp.Result.Size ||
      Sub.Inputs[0] != Cmp.A || Sub.Inputs[1] != Cmp.B ||
      Sub.Inputs[0].Size != Cmp.A.Size ||
      Sub.Inputs[1].Size != Cmp.B.Size ||
      Sub.Inputs[0].RegOff != Cmp.A.RegOff ||
      Overflow.Opcode != NdOp::INT_SBOR || Overflow.NumInputs < 2 ||
      Overflow.Inputs[1] != Cmp.B ||
      Overflow.Inputs[1].Size != Cmp.B.Size ||
      Overflow.Inputs[0].Size != Cmp.A.Size ||
      Sub.Addr != Overflow.Addr)
    return false;
  if (Cmp.B.Kind == MedVar::Reg &&
      (Sub.Inputs[1].RegOff != Cmp.B.RegOff ||
       Overflow.Inputs[1].RegOff != Cmp.B.RegOff))
    return false;

  const auto First = selfLowSliceDef(Ops, Cmp.A, Cmp.SourceOpIndex);
  const auto Second =
      selfLowSliceDef(Ops, Overflow.Inputs[0], OverflowIdx);
  if (!First || !Second || *Second <= Cmp.SourceOpIndex ||
      Ops[*First].Addr != Sub.Addr || Ops[*Second].Addr != Sub.Addr)
    return false;
  const MedVar &Base = Ops[*First].Inputs[0];
  if (Ops[*Second].Inputs[0] != Base ||
      Ops[*Second].Inputs[0].Size != Base.Size ||
      Ops[*Second].Inputs[0].RegOff != Base.RegOff ||
      Ops[*First].Output.Size != Ops[*Second].Output.Size)
    return false;

  // A fresh register write between the two projections ends their shared
  // update window.  This includes the RHS when it overlaps the wide parent.
  // Self-extracts of the exact same wide SSA value are reads.
  for (int I = *First + 1; I < OverflowIdx; ++I) {
    const MedOp &Op = Ops[I];
    if (Op.Addr != Sub.Addr)
      return false;
    if (Op.Output.Kind != MedVar::Reg || Op.Output.Size == 0)
      continue;
    const bool OverlapsBase =
        Op.Output.RegOff < Base.RegOff + Base.Size &&
        Base.RegOff < Op.Output.RegOff + Op.Output.Size;
    const bool OverlapsRight =
        Cmp.B.Kind == MedVar::Reg &&
        Op.Output.RegOff < Cmp.B.RegOff + Cmp.B.Size &&
        Cmp.B.RegOff < Op.Output.RegOff + Op.Output.Size;
    if (OverlapsRight || (OverlapsBase && !isSelfLowSliceOf(Op, Base)))
      return false;
  }

  auto flagFromSubResult = [&](uint64_t Offset, NdOp Opcode) {
    for (int I = ConsumerIdx; I > Cmp.SourceOpIndex; --I) {
      const MedOp &Op = Ops[I];
      if (Op.Output.Kind != MedVar::Flag || Op.Output.RegOff != Offset)
        continue;
      return Op.Opcode == Opcode && Op.Addr == Sub.Addr &&
             Op.NumInputs >= 2 && Op.Inputs[0] == Cmp.Result &&
             Op.Inputs[0].Size == Cmp.Result.Size &&
             Op.Inputs[1].isConst() &&
             Op.Inputs[1].Size == Cmp.Result.Size &&
             Op.Inputs[1].ConstVal == 0;
    }
    return false;
  };
  if (!flagFromSubResult(TRI.FlagNF, NdOp::INT_SLESS))
    return false;
  if ((CC == CondCode::SLE || CC == CondCode::SGT) &&
      !flagFromSubResult(TRI.FlagZF, NdOp::INT_EQUAL))
    return false;
  return true;
}

} // namespace

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
    if ((Def.Inputs[0] == Cmp.A && Def.Inputs[1] == Cmp.B) ||
        (Def.Inputs[0] == Cmp.B && Def.Inputs[1] == Cmp.A))
      return true;
    return sameSignedCmpSliceWindow(Ops, ConsumerIdx, J, Cmp, CC, TRI);
  }
  return false; // flag is live-in/loop-carried — not this block's CMP
}

} // namespace neverd
