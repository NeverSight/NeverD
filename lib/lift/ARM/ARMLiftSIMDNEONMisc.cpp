//===- ARMLiftSIMDNEONMisc.cpp - ARM32 NEON reduction and MVE lifter -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pairwise VPMAX/VPMIN, the across-vector reductions, the MVE
/// predicate ops, complex multiply-accumulate, the dot products and
/// the custom-datapath VCX instructions.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool liftSIMDNEONMisc(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                      const cs_arm &ARM) {
  switch (Insn->id) {
  // NEON pairwise min/max: d[i]=op(a[2i],a[2i+1]), d[N/2+i]=op(b[2i],b[2i+1])
  case ARM_INS_VPMAX:
  case ARM_INS_VPMIN: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    bool IsMin = (Insn->id == ARM_INS_VPMIN);
    if (LI.LaneSz > 0 && A.Size >= 2 * LI.LaneSz) {
      unsigned NPairs = A.Size / LI.LaneSz / 2;
      NdOp CmpOp = LI.IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS;
      // VPMIN/VPMAX float lanes are IEEE minimum/maximum (NaN-propagating,
      // -0 < +0); a FLOAT_LESS+SELECT got NaN and signed zeros wrong.
      NdOp FMM = IsMin ? NdOp::FLOAT_MIN : NdOp::FLOAT_MAX;
      NdVar Acc = NdVar::cst(0, 0);
      auto doCmp = [&](const NdVar &Src, unsigned SetIdx) {
        for (unsigned P = 0; P < NPairs; ++P) {
          NdVar Lo = S.makeTemp(LI.LaneSz);
          S.emit(
              NdOp::SUBBYTES, Lo,
              {Src, NdVar::cst(static_cast<uint64_t>(P) * 2 * LI.LaneSz, 4)});
          NdVar Hi = S.makeTemp(LI.LaneSz);
          S.emit(NdOp::SUBBYTES, Hi,
                 {Src, NdVar::cst(
                           (static_cast<uint64_t>(P) * 2 + 1) * LI.LaneSz, 4)});
          NdVar Sel = S.makeTemp(LI.LaneSz);
          if (LI.IsFloat) {
            S.emit(FMM, Sel, {Lo, Hi});
          } else {
            NdVar Cond = S.makeTemp(1);
            if (IsMin)
              S.emit(CmpOp, Cond, {Lo, Hi});
            else
              S.emit(CmpOp, Cond, {Hi, Lo});
            S.emit(NdOp::SELECT, Sel, {Cond, Lo, Hi});
          }
          if (SetIdx == 0 && P == 0) {
            Acc = Sel;
          } else {
            NdVar Prev = S.makeTemp(Acc.Size + LI.LaneSz);
            S.emit(NdOp::CONCAT, Prev, {Sel, Acc});
            Acc = Prev;
          }
        }
      };
      doCmp(A, 0);
      doCmp(B, 1);
      if (Acc.Size == Dst.Size)
        S.emit(NdOp::COPY, Dst, {Acc});
      else if (Acc.Size < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {Acc});
      else
        S.emit(NdOp::SUBBYTES, Dst, {Acc, NdVar::cst(0, 4)});
    } else {
      S.emit(NdOp::COPY, Dst, {A});
    }
    break;
  }

  // NEON reduction
  case ARM_INS_VMAXAV:
  case ARM_INS_VMAXNMAV:
  case ARM_INS_VMAXNMV:
  case ARM_INS_VMAXV:
  case ARM_INS_VMINAV:
  case ARM_INS_VMINNMAV:
  case ARM_INS_VMINNMV:
  case ARM_INS_VMINV:
  case ARM_INS_VADDV:
  case ARM_INS_VADDVA:
  case ARM_INS_VADDLV:
  case ARM_INS_VADDLVA:
  case ARM_INS_VMLADAV:
  case ARM_INS_VMLADAVA:
  case ARM_INS_VMLADAVAX:
  case ARM_INS_VMLADAVX:
  case ARM_INS_VMLALDAV:
  case ARM_INS_VMLALDAVA:
  case ARM_INS_VMLALDAVAX:
  case ARM_INS_VMLALDAVX:
  case ARM_INS_VMLSDAV:
  case ARM_INS_VMLSDAVA:
  case ARM_INS_VMLSDAVAX:
  case ARM_INS_VMLSDAVX:
  case ARM_INS_VMLSLDAV:
  case ARM_INS_VMLSLDAVA:
  case ARM_INS_VMLSLDAVAX:
  case ARM_INS_VMLSLDAVX:
  case ARM_INS_VRMLALDAVH:
  case ARM_INS_VRMLALDAVHA:
  case ARM_INS_VRMLALDAVHAX:
  case ARM_INS_VRMLALDAVHX:
  case ARM_INS_VRMLSLDAVH:
  case ARM_INS_VRMLSLDAVHA:
  case ARM_INS_VRMLSLDAVHAX:
  case ARM_INS_VRMLSLDAVHX: {
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar Src = L.operandRead(S, ARM.operands[ARM.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  // MVE predicate
  case ARM_INS_VPST:
    S.emitIntrinsic(Intrinsic::ArmVpst);
    break;
  case ARM_INS_VPT:
    S.emitIntrinsic(Intrinsic::ArmVpt);
    break;
  case ARM_INS_VPSEL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    S.emit(NdOp::COPY, Dst, {A});
    break;
  }
  case ARM_INS_VCMUL:
  // VCMLA — rotated complex floating-point multiply-accumulate (AArch32
  // FEAT_FCMA).  Same #rot table as AArch64 FCMLA; the old code was a whole-
  // register INT_MULT+INT_ADD (integer ops on FP, no rotation, no per-lane).
  case ARM_INS_VCMLA: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Vn = L.operandRead(S, ARM.operands[1]);
    NdVar Vm = L.operandRead(S, ARM.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    int64_t Rot = ARM.operands[3].imm;
    int VecIdx = ARM.operands[2].neon_lane >= 0 ? ARM.operands[2].neon_lane
                                                : ARM.operands[2].vector_index;
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned ES = LI.LaneSz;
    if (!LI.IsFloat || (ES != 4 && ES != 8) || Dst.Size < 2 * ES) {
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {Vn, Vm});
      S.emit(NdOp::INT_ADD, Dst, {OldDst, Prod});
      break;
    }
    bool UseVnIm = (Rot == 90 || Rot == 270);
    bool ReUseVmIm = (Rot == 90 || Rot == 270);
    bool ImUseVmIm = (Rot == 0 || Rot == 180);
    bool ReSub = (Rot == 90 || Rot == 180);
    bool ImSub = (Rot == 180 || Rot == 270);
    auto lane = [&](NdVar V, unsigned Idx) {
      NdVar T = S.makeTemp(ES);
      S.emit(NdOp::SUBBYTES, T,
             {V, NdVar::cst(static_cast<uint64_t>(Idx) * ES, 4)});
      return T;
    };
    NdVar Acc = NdVar::cst(0, 0);
    bool First = true;
    auto append = [&](NdVar L) {
      if (First) {
        Acc = L;
        First = false;
        return;
      }
      NdVar N = S.makeTemp(Acc.Size + ES);
      S.emit(NdOp::CONCAT, N, {L, Acc});
      Acc = N;
    };
    unsigned NLanes = Dst.Size / ES;
    for (unsigned K = 0; K < NLanes / 2; ++K) {
      unsigned BPair = (VecIdx >= 0) ? (unsigned)VecIdx : K;
      NdVar VnEl = lane(Vn, 2 * K + (UseVnIm ? 1 : 0));
      NdVar VmRe = lane(Vm, 2 * BPair);
      NdVar VmIm = lane(Vm, 2 * BPair + 1);
      NdVar AccRe = lane(OldDst, 2 * K);
      NdVar AccIm = lane(OldDst, 2 * K + 1);
      NdVar PRe = S.makeTemp(ES), PIm = S.makeTemp(ES);
      S.emit(NdOp::FLOAT_MULT, PRe, {VnEl, ReUseVmIm ? VmIm : VmRe});
      S.emit(NdOp::FLOAT_MULT, PIm, {VnEl, ImUseVmIm ? VmIm : VmRe});
      NdVar OutRe = S.makeTemp(ES), OutIm = S.makeTemp(ES);
      S.emit(ReSub ? NdOp::FLOAT_SUB : NdOp::FLOAT_ADD, OutRe, {AccRe, PRe});
      S.emit(ImSub ? NdOp::FLOAT_SUB : NdOp::FLOAT_ADD, OutIm, {AccIm, PIm});
      append(OutRe);
      append(OutIm);
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case ARM_INS_VBRSR: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    S.emit(NdOp::INT_MULT, Dst, {A, B});
    break;
  }
  case ARM_INS_VDDUP:
  case ARM_INS_VDWDUP:
  case ARM_INS_VIDUP:
  case ARM_INS_VIWDUP: {
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar Src = L.operandRead(S, ARM.operands[1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VADC:
  case ARM_INS_VADCI: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    S.emit(NdOp::INT_ADD, Dst, {A, B});
    break;
  }
  case ARM_INS_VSBC:
  case ARM_INS_VSBCI: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    S.emit(NdOp::INT_SUB, Dst, {A, B});
    break;
  }
  // VSDOT/VUDOT — byte dot-product accumulate: each 32-bit lane adds the sum of
  // four byte products (signed/unsigned).  The indexed form broadcasts one
  // 4-byte group of B.  Was a full-width INT_MULT+INT_ADD placeholder (whole
  // register as one integer — no per-lane reduction, no byte widening).
  case ARM_INS_VSDOT:
  case ARM_INS_VUDOT: {
    if (ARM.op_count < 3)
      break;
    bool IsSigned = (Insn->id == ARM_INS_VSDOT);
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    unsigned NLanes = Dst.Size / 4;
    int VecIdx = ARM.operands[2].neon_lane >= 0 ? ARM.operands[2].neon_lane
                                                : ARM.operands[2].vector_index;
    auto ExtOp = IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
    NdVar Acc = NdVar::cst(0, 0);
    for (unsigned I = 0; I < NLanes; ++I) {
      unsigned BBase = (VecIdx >= 0 ? (unsigned)VecIdx : I) * 4;
      NdVar Lane = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Lane, {OldDst, NdVar::cst(I * 4, 4)});
      for (unsigned K = 0; K < 4; ++K) {
        NdVar Ba = S.makeTemp(1), Bb = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, Ba, {A, NdVar::cst(I * 4 + K, 4)});
        S.emit(NdOp::SUBBYTES, Bb, {B, NdVar::cst(BBase + K, 4)});
        NdVar Ea = S.makeTemp(4), Eb = S.makeTemp(4);
        S.emit(ExtOp, Ea, {Ba});
        S.emit(ExtOp, Eb, {Bb});
        NdVar Pr = S.makeTemp(4);
        S.emit(NdOp::INT_MULT, Pr, {Ea, Eb});
        NdVar Nl = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Nl, {Lane, Pr});
        Lane = Nl;
      }
      if (I == 0) {
        Acc = Lane;
      } else {
        NdVar P = S.makeTemp(Acc.Size + 4);
        S.emit(NdOp::CONCAT, P, {Lane, Acc});
        Acc = P;
      }
    }
    if (Acc.Size < Dst.Size)
      S.emit(NdOp::INT_ZEXT, Dst, {Acc});
    else
      S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // VUSDOT/VSUDOT (mixed sign, i8mm) and VDOT (alias) — keep the simple
  // placeholder; not validated against Unicorn.
  case ARM_INS_VUSDOT:
  case ARM_INS_VSUDOT:
  case ARM_INS_VDOT: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    S.emit(NdOp::INT_ADD, Dst, {NdVar::reg(Dst.Offset, Dst.Size), Prod});
    break;
  }
  case ARM_INS_VMMLA:
  case ARM_INS_VSMMLA:
  case ARM_INS_VUMMLA:
  case ARM_INS_VUSMMLA: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    S.emit(NdOp::INT_ADD, Dst, {NdVar::reg(Dst.Offset, Dst.Size), Prod});
    break;
  }
  case ARM_INS_VCX1:
    S.emitIntrinsic(Intrinsic::ArmVcx1);
    break;
  case ARM_INS_VCX1A:
    S.emitIntrinsic(Intrinsic::ArmVcx1a);
    break;
  case ARM_INS_VCX2:
    S.emitIntrinsic(Intrinsic::ArmVcx2);
    break;
  case ARM_INS_VCX2A:
    S.emitIntrinsic(Intrinsic::ArmVcx2a);
    break;
  case ARM_INS_VCX3:
    S.emitIntrinsic(Intrinsic::ArmVcx3);
    break;
  case ARM_INS_VCX3A:
    S.emitIntrinsic(Intrinsic::ArmVcx3a);
    break;

  case ARM_INS___BRKDIV0:
    S.emitIntrinsic(Intrinsic::ArmBkpt);
    break;

  default:
    return false;
  }
  return true;
}

} // namespace neverd
