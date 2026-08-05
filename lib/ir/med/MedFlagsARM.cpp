//===- MedFlagsARM.cpp - ARM/AArch64 NZCV compound pattern matching ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM family (ARM32 + AArch64) compound flag condition patterns for the
/// MedIR flag elimination pass.  Both architectures share identical NZCV
/// (Negative, Zero, Carry, oVerflow) condition semantics.
///
/// ARM condition flags differ from x86 EFLAGS in that C=1 indicates
/// "no borrow" (unsigned >=) rather than x86's CF=1 meaning "borrow"
/// (unsigned <).  This inverts the polarity of compound unsigned
/// conditions relative to x86.
///
/// Recognized patterns:
///    C && !Z     → UGT   (HI: unsigned higher)
///   !Z && N==V   → SGT   (GT: signed greater than)
///   !C ||  Z     → ULE   (LS: unsigned lower or same)
///    Z || N!=V   → SLE   (LE: signed less or equal)
///
//===----------------------------------------------------------------------===//

#include "MedFlagsDetail.h"

namespace neverd {

CondCode resolveCompoundFlagPatternARM(const std::vector<MedOp> &Ops,
                                       const MedOp &Def, int DefIdx,
                                       const TargetRegInfo &TRI) {
  if (Def.NumInputs < 2)
    return CondCode::Invalid;

  auto SP0 = classifySubPat(Ops, Def.Inputs[0], DefIdx - 1);
  auto SP1 = classifySubPat(Ops, Def.Inputs[1], DefIdx - 1);

  if (Def.Opcode == NdOp::BOOL_AND) {
    // C && !Z → UGT (HI)
    auto IsCDirect = [&](const SubPatInfo &S) {
      return S.Pat == FlagSubPat::DirectFlag && S.FlagOff == TRI.FlagCF;
    };
    auto IsZInverted = [&](const SubPatInfo &S) {
      return S.Pat == FlagSubPat::InvertedFlag && S.FlagOff == TRI.FlagZF;
    };
    if ((IsCDirect(SP0) && IsZInverted(SP1)) ||
        (IsCDirect(SP1) && IsZInverted(SP0)))
      return CondCode::UGT;

    // !Z && (N==V) → SGT (GT)
    bool HasNotZ =
        (SP0.Pat == FlagSubPat::InvertedFlag && SP0.FlagOff == TRI.FlagZF) ||
        (SP1.Pat == FlagSubPat::InvertedFlag && SP1.FlagOff == TRI.FlagZF);
    bool HasNEqV = (SP0.Pat == FlagSubPat::FlagsEqual) ||
                   (SP1.Pat == FlagSubPat::FlagsEqual);
    if (HasNotZ && HasNEqV)
      return CondCode::SGT;
  }

  if (Def.Opcode == NdOp::BOOL_OR) {
    // !C || Z → ULE (LS)
    auto IsCInverted = [&](const SubPatInfo &S) {
      return S.Pat == FlagSubPat::InvertedFlag && S.FlagOff == TRI.FlagCF;
    };
    auto IsZDirect = [&](const SubPatInfo &S) {
      return S.Pat == FlagSubPat::DirectFlag && S.FlagOff == TRI.FlagZF;
    };
    if ((IsCInverted(SP0) && IsZDirect(SP1)) ||
        (IsCInverted(SP1) && IsZDirect(SP0)))
      return CondCode::ULE;

    // Z || (N!=V) → SLE (LE)
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

} // namespace neverd
