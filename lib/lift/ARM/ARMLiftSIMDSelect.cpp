//===- ARMLiftSIMDSelect.cpp - ARM32 VFP select, min/max and round lifter ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The VSEL family, per-lane VMAX/VMIN (and the NaN-suppressing NM
/// variants), the VRINT rounding family and VMRS/VMSR.
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

bool liftSIMDSelect(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                    const cs_arm &ARM) {
  switch (Insn->id) {
  case ARM_INS_VSELEQ:
  case ARM_INS_VSELGE:
  case ARM_INS_VSELGT:
  case ARM_INS_VSELVS: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar Cond = S.makeTemp(1);
    switch (Insn->id) {
    case ARM_INS_VSELEQ:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(armreg::ZFLAG, 1)});
      break;
    case ARM_INS_VSELGE:
      S.emit(NdOp::INT_EQUAL, Cond,
             {NdVar::reg(armreg::NFLAG, 1), NdVar::reg(armreg::VFLAG, 1)});
      break;
    case ARM_INS_VSELGT: {
      NdVar NZ = S.makeTemp(1);
      NdVar NvEq = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(armreg::ZFLAG, 1)});
      S.emit(NdOp::INT_EQUAL, NvEq,
             {NdVar::reg(armreg::NFLAG, 1), NdVar::reg(armreg::VFLAG, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NZ, NvEq});
      break;
    }
    case ARM_INS_VSELVS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(armreg::VFLAG, 1)});
      break;
    default:
      S.emit(NdOp::COPY, Cond, {NdVar::cst(1, 1)});
      break;
    }
    S.emit(NdOp::SELECT, Dst, {Cond, A, B});
    break;
  }
  // VMAX/VMIN and the NaN-aware VMAXNM/VMINNM.  These are per-lane and may be
  // integer (.s8/.u16/.s32/...) or floating-point (.f32/.f64).  The previous
  // implementation unconditionally used a single full-width FLOAT_LESSEQUAL,
  // which is wrong for integer data (e.g. `vmin.s32 q,q,q`) on two counts: it
  // reinterprets integer lanes as floats and it does not isolate the lanes.
  case ARM_INS_VMAXNM:
  case ARM_INS_VMAXNMA:
  case ARM_INS_VMAX:
  case ARM_INS_VMAXA:
  case ARM_INS_VMINNM:
  case ARM_INS_VMINNMA:
  case ARM_INS_VMIN:
  case ARM_INS_VMINA: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    bool IsMin = (Insn->id == ARM_INS_VMIN || Insn->id == ARM_INS_VMINA ||
                  Insn->id == ARM_INS_VMINNM || Insn->id == ARM_INS_VMINNMA);
    bool IsNM = (Insn->id == ARM_INS_VMAXNM || Insn->id == ARM_INS_VMAXNMA ||
                 Insn->id == ARM_INS_VMINNM || Insn->id == ARM_INS_VMINNMA);
    unsigned LaneSz = 0;
    bool IsSigned = false, IsFloat = IsNM;
    switch (ARM.vector_data) {
    case ARM_VECTORDATA_S8:
      LaneSz = 1;
      IsSigned = true;
      break;
    case ARM_VECTORDATA_U8:
    case ARM_VECTORDATA_I8:
      LaneSz = 1;
      break;
    case ARM_VECTORDATA_S16:
      LaneSz = 2;
      IsSigned = true;
      break;
    case ARM_VECTORDATA_U16:
    case ARM_VECTORDATA_I16:
      LaneSz = 2;
      break;
    case ARM_VECTORDATA_S32:
      LaneSz = 4;
      IsSigned = true;
      break;
    case ARM_VECTORDATA_U32:
    case ARM_VECTORDATA_I32:
      LaneSz = 4;
      break;
    case ARM_VECTORDATA_S64:
      LaneSz = 8;
      IsSigned = true;
      break;
    case ARM_VECTORDATA_U64:
    case ARM_VECTORDATA_I64:
      LaneSz = 8;
      break;
    case ARM_VECTORDATA_F32:
      LaneSz = 4;
      IsFloat = true;
      break;
    case ARM_VECTORDATA_F64:
      LaneSz = 8;
      IsFloat = true;
      break;
    default:
      break;
    }
    // Capstone leaves vector_data INVALID for VMINNM/VMAXNM, so the f32/f64
    // element width is only in the mnemonic suffix; a size-based guess wrongly
    // picked f64 (8) for the 16-byte Q-register vminnm.f32 form.
    if (IsFloat && LaneSz == 0)
      LaneSz = llvm::StringRef(Insn->mnemonic).contains(".f64") ? 8 : 4;
    auto cmpOp = [&]() { return IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS; };
    // Float VMIN/VMAX are IEEE minimum/maximum (NaN-propagating, -0 < +0); the
    // NM variants are minNum/maxNum (NaN-suppressing).  A bare
    // FLOAT_LESS+SELECT got both NaN and signed-zero wrong.  Integer lanes keep
    // the compare/select.
    NdOp FMM = IsNM ? (IsMin ? NdOp::FLOAT_MINNUM : NdOp::FLOAT_MAXNUM)
                    : (IsMin ? NdOp::FLOAT_MIN : NdOp::FLOAT_MAX);
    auto emitLane = [&](NdVar Out, NdVar La, NdVar Lb) {
      if (IsFloat) {
        S.emit(FMM, Out, {La, Lb});
      } else {
        NdVar Cond = S.makeTemp(1);
        if (IsMin)
          S.emit(cmpOp(), Cond, {La, Lb});
        else
          S.emit(cmpOp(), Cond, {Lb, La});
        S.emit(NdOp::SELECT, Out, {Cond, La, Lb});
      }
    };
    if (LaneSz > 0 && Dst.Size >= LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Sel = S.makeTemp(LaneSz);
        emitLane(Sel, La, Lb);
        if (I == 0) {
          Acc = Sel;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Sel, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      emitLane(Dst, A, B);
    }
    break;
  }
  // VRINT{A,M,N,P,R,X,Z} — round float to integral float.  A ties away, N/R/X
  // tie to even (R/X follow FPSCR, default even), M floor, P ceil, Z toward
  // zero.  Each had been collapsed to round-half-away, which is wrong for all
  // but VRINTA.
  case ARM_INS_VRINTA:
  case ARM_INS_VRINTM:
  case ARM_INS_VRINTN:
  case ARM_INS_VRINTP:
  case ARM_INS_VRINTR:
  case ARM_INS_VRINTX:
  case ARM_INS_VRINTZ: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    if (Insn->id == ARM_INS_VRINTZ) {
      NdVar Fl = S.makeTemp(Dst.Size), Ce = S.makeTemp(Dst.Size),
              IsNeg = S.makeTemp(1);
      S.emit(NdOp::FLOAT_FLOOR, Fl, {Src});
      S.emit(NdOp::FLOAT_CEIL, Ce, {Src});
      S.emit(NdOp::FLOAT_LESS, IsNeg, {Src, NdVar::cst(0, Dst.Size)});
      S.emit(NdOp::SELECT, Dst, {IsNeg, Ce, Fl});
    } else {
      NdOp Ro;
      switch (Insn->id) {
      case ARM_INS_VRINTM:
        Ro = NdOp::FLOAT_FLOOR;
        break;
      case ARM_INS_VRINTP:
        Ro = NdOp::FLOAT_CEIL;
        break;
      case ARM_INS_VRINTA:
        Ro = NdOp::FLOAT_ROUND;
        break;
      default:
        Ro = NdOp::FLOAT_ROUNDEVEN;
        break;
      }
      S.emit(Ro, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VMRS:
    // `vmrs APSR_nzcv, fpscr` (a.k.a. fmstat) commits the FPSCR flags VCMP
    // produced into the CPSR flags that conditional instructions read.
    S.emit(NdOp::COPY, NdVar::reg(armreg::NFLAG, 1),
           {NdVar::reg(armreg::FP_NFLAG, 1)});
    S.emit(NdOp::COPY, NdVar::reg(armreg::ZFLAG, 1),
           {NdVar::reg(armreg::FP_ZFLAG, 1)});
    S.emit(NdOp::COPY, NdVar::reg(armreg::CFLAG, 1),
           {NdVar::reg(armreg::FP_CFLAG, 1)});
    S.emit(NdOp::COPY, NdVar::reg(armreg::VFLAG, 1),
           {NdVar::reg(armreg::FP_VFLAG, 1)});
    break;
  case ARM_INS_VMSR:
    // FPSCR writes are not modeled separately.
    break;

  default:
    return false;
  }
  return true;
}

} // namespace neverd
