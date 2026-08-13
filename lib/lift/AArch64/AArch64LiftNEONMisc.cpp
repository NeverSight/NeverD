//===- AArch64LiftNEONMisc.cpp - NEON misc and scalar carry ops -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Element reverse (REV64), SQXTNB, saturating mixed-sign
/// accumulate (SUQADD/USQADD), shift-left-long by element size
/// (SHLL/SHLL2), the scalar add/subtract-with-carry ADC/ADCS/
/// SBC/SBCS, long multiply-subtract (SMSUBL/UMSUBL) and the
/// non-temporal pair LDNP/STNP.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftNEONMisc(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // NEON misc single-operand
  // REV64 (vector) — reverse the order of elements within each 64-bit group.
  // Element width from the arrangement (.16b/.8h/.4s).  Was an intrinsic
  // intrinsic with no backend handler -> silently returned 0.
  case AARCH64_INS_REV64: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    unsigned ElemSz = neonElemSize(ARM64.operands[0].vas);
    if (ElemSz == 0 || ElemSz >= 8 || Src.Size < 8) {
      S.emit(NdOp::COPY, Dst, {Src});
      break;
    }
    unsigned ElemsPerGroup = 8 / ElemSz;
    unsigned NGroups = Src.Size / 8;
    NdVar Acc = S.makeTemp(0);
    bool First = true;
    for (unsigned G = 0; G < NGroups; ++G) {
      for (unsigned E = 0; E < ElemsPerGroup; ++E) {
        unsigned SrcIdx = G * ElemsPerGroup + (ElemsPerGroup - 1 - E);
        NdVar El = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, El, {Src, NdVar::cst(SrcIdx * ElemSz, 4)});
        if (First) {
          Acc = El;
          First = false;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + ElemSz);
          S.emit(NdOp::CONCAT, Next, {El, Acc});
          Acc = Next;
        }
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case AARCH64_INS_SQXTNB: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Intrinsic::A64_Sqxtn, Dst, {Src});
    break;
  }
  // SUQADD / USQADD — saturating accumulate of an opposite-signedness operand.
  //   SUQADD: signed accumulator Vd + unsigned addend Vn, signed-saturating.
  //   USQADD: unsigned accumulator Vd + signed addend Vn, unsigned-saturating.
  // Vd (operands[0]) is both destination and first source.  Per-lane: extend
  // each operand by its own signedness to a wider lane, add, clamp to the
  // destination signedness range.  Was a full-width INT_ADD placeholder (no
  // saturation, cross-lane carry, wrong signedness).
  case AARCH64_INS_SUQADD:
  case AARCH64_INS_USQADD: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Vd = NdVar::reg(Dst.Offset, Dst.Size);
    NdVar Vn = L.operandRead(S, ARM64.operands[1]);

    bool DstSigned = (Insn->id == AARCH64_INS_SUQADD);
    unsigned LaneSz = neonElemSize(ARM64.operands[0].vas);
    if (LaneSz == 0)
      LaneSz = Dst.Size; // scalar b/h/s/d form: whole register is one lane
    unsigned NLanes = (LaneSz && Dst.Size >= LaneSz) ? Dst.Size / LaneSz : 1;
    unsigned WideSz = (LaneSz <= 4) ? LaneSz * 2 : 16;

    NdVar Acc = S.makeTemp(0);
    for (unsigned Idx = 0; Idx < NLanes; ++Idx) {
      NdVar La = S.makeTemp(LaneSz);
      NdVar Lb = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, La, {Vd, NdVar::cst(Idx * LaneSz, 4)});
      S.emit(NdOp::SUBBYTES, Lb, {Vn, NdVar::cst(Idx * LaneSz, 4)});
      NdVar Wa = S.makeTemp(WideSz);
      NdVar Wb = S.makeTemp(WideSz);
      S.emit(DstSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Wa, {La});
      S.emit(DstSigned ? NdOp::INT_ZEXT : NdOp::INT_SEXT, Wb, {Lb});
      NdVar Sum = S.makeTemp(WideSz);
      S.emit(NdOp::INT_ADD, Sum, {Wa, Wb});

      // Narrowing saturate via trunc+extend overflow detect (avoids fork's
      // InstCombine mis-fold on INT_SLESS+SELECT clamp chains).
      NdVar Trunc = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, Trunc, {Sum, NdVar::cst(0, 4)});
      NdVar BackWide = S.makeTemp(WideSz);
      S.emit(DstSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, BackWide, {Trunc});
      NdVar Fits = S.makeTemp(1);
      S.emit(NdOp::INT_EQUAL, Fits, {Sum, BackWide});
      // The scalar form takes LaneSz from the destination register, so a `d`
      // lane puts the sign bit at position 63 — clamp the shift instead of
      // running it off the end of the type.  ~MaxVal is the same bit pattern
      // as -(1 << (bits - 1)) without the signed-overflow.
      uint64_t MaxVal, MinVal;
      if (DstSigned) {
        MaxVal = (LaneSz == 0 || LaneSz >= 8)
                     ? (~0ULL >> 1)
                     : ((1ULL << (LaneSz * 8 - 1)) - 1);
        MinVal = ~MaxVal;
      } else {
        MaxVal = (LaneSz < 8) ? ((1ULL << (LaneSz * 8)) - 1) : ~0ULL;
        MinVal = 0;
      }
      NdVar IsPos = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, IsPos, {NdVar::cst(0, WideSz), Sum});
      NdVar OvfVal = S.makeTemp(LaneSz);
      S.emit(NdOp::SELECT, OvfVal,
             {IsPos, NdVar::cst(MaxVal, LaneSz), NdVar::cst(MinVal, LaneSz)});
      NdVar Result = S.makeTemp(LaneSz);
      S.emit(NdOp::SELECT, Result, {Fits, Trunc, OvfVal});

      if (Idx == 0) {
        Acc = Result;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {Result, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // SHLL / SHLL2 — shift left long by element size: each source element is
  // zero-extended to twice its width and shifted left by the source element's
  // bit-width (source bits land in the high half, low half zero).  SHLL2 reads
  // the high half of the source.  Was a full-width INT_ZEXT placeholder (no
  // shift, not per-lane).
  case AARCH64_INS_SHLL:
  case AARCH64_INS_SHLL2: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    unsigned SrcLaneSz = neonElemSize(ARM64.operands[1].vas);
    if (SrcLaneSz == 0 || SrcLaneSz > 4) {
      S.emit(NdOp::INT_ZEXT, Dst, {Src});
      break;
    }
    unsigned DstLaneSz = SrcLaneSz * 2;
    unsigned NLanes = Dst.Size / DstLaneSz;
    unsigned SrcOff = (Insn->id == AARCH64_INS_SHLL2) ? NLanes * SrcLaneSz : 0;
    unsigned ShiftBits = SrcLaneSz * 8;
    NdVar Acc = S.makeTemp(0);
    for (unsigned Idx = 0; Idx < NLanes; ++Idx) {
      NdVar L = S.makeTemp(SrcLaneSz);
      S.emit(NdOp::SUBBYTES, L, {Src, NdVar::cst(SrcOff + Idx * SrcLaneSz, 4)});
      NdVar W = S.makeTemp(DstLaneSz);
      S.emit(NdOp::INT_ZEXT, W, {L});
      NdVar Sh = S.makeTemp(DstLaneSz);
      S.emit(NdOp::INT_LEFT, Sh, {W, NdVar::cst(ShiftBits, DstLaneSz)});
      if (Idx == 0) {
        Acc = Sh;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
        S.emit(NdOp::CONCAT, Next, {Sh, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // ADC / SBC (add/subtract with carry)
  case AARCH64_INS_ADC:
  case AARCH64_INS_ADCS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar AB = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_ADD, AB, {A, B});
    NdVar CfExt = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(a64reg::CFLAG, 1)});
    bool SetFlags = (Insn->id == AARCH64_INS_ADCS);
    // carry/overflow from the source operands must be computed before the
    // result write, else A/b alias-resolve to the post-write Dst (adcs
    // xD,xD,xM).
    NdVar C1, V1;
    if (SetFlags) {
      C1 = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, C1, {A, B});
      V1 = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, V1, {A, B});
    }
    S.emit(NdOp::INT_ADD, Dst, {AB, CfExt});
    if (SetFlags) {
      S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      // C_out = carry(A,b) | carry(a+b, cin)
      NdVar C2 = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, C2, {AB, CfExt});
      S.emit(NdOp::BOOL_OR, NdVar::reg(a64reg::CFLAG, 1), {C1, C2});
      // V_out = scarry(A,b) ^ scarry(a+b, cin)
      NdVar V2 = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, V2, {AB, CfExt});
      S.emit(NdOp::BOOL_XOR, NdVar::reg(a64reg::VFLAG, 1), {V1, V2});
    }
    break;
  }
  case AARCH64_INS_SBC:
  case AARCH64_INS_SBCS: {
    // Capstone 6 alias: NGC Rd, Rm → id=SBC, op_count=2 (Src1=XZR implicit)
    // NGC: Dst = NOT(Rm) + C = ~Rm + CF
    if (Insn->is_alias && ARM64.op_count == 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar B = L.operandRead(S, ARM64.operands[1]);
      uint16_t Sz = Dst.Size;
      NdVar NotB = S.makeTemp(Sz);
      S.emit(NdOp::INT_NOT, NotB, {B});
      NdVar CfExt = S.makeTemp(Sz);
      S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(a64reg::CFLAG, 1)});
      S.emit(NdOp::INT_ADD, Dst, {NotB, CfExt});
      if (Insn->id == AARCH64_INS_SBCS) {
        // NGCS sets NZCV.  First operand is XZR=0:
        // carry(0,~Rm)=scarry(0,~Rm)=0, so the ~Rm + C addition alone
        // determines C/V.
        S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
               {Dst, NdVar::cst(0, Sz)});
        S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
               {Dst, NdVar::cst(0, Sz)});
        S.emit(NdOp::INT_CARRY, NdVar::reg(a64reg::CFLAG, 1), {NotB, CfExt});
        S.emit(NdOp::INT_SOVF, NdVar::reg(a64reg::VFLAG, 1), {NotB, CfExt});
      }
      break;
    }
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar NotB = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NotB, {B});
    NdVar AB = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_ADD, AB, {A, NotB});
    NdVar CfExt = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(a64reg::CFLAG, 1)});
    bool SetFlags = (Insn->id == AARCH64_INS_SBCS);
    // carry/overflow from the source operands must be computed before the
    // result write, else A alias-resolves to the post-write Dst (sbcs
    // xD,xD,xM).
    NdVar C1, V1;
    if (SetFlags) {
      C1 = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, C1, {A, NotB});
      V1 = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, V1, {A, NotB});
    }
    S.emit(NdOp::INT_ADD, Dst, {AB, CfExt});
    if (SetFlags) {
      S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      // C_out = carry(A, ~b) | carry(a+~b, cin)
      NdVar C2 = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, C2, {AB, CfExt});
      S.emit(NdOp::BOOL_OR, NdVar::reg(a64reg::CFLAG, 1), {C1, C2});
      // V_out = scarry(A, ~b) ^ scarry(a+~b, cin)
      NdVar V2 = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, V2, {AB, CfExt});
      S.emit(NdOp::BOOL_XOR, NdVar::reg(a64reg::VFLAG, 1), {V1, V2});
    }
    break;
  }
  // SMSUBL / UMSUBL
  // Capstone 6 alias: SMNEGL/UMNEGL → id=SMSUBL/UMSUBL, op_count=3
  case AARCH64_INS_SMSUBL:
  case AARCH64_INS_UMSUBL: {
    if (Insn->is_alias && ARM64.op_count == 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar N = L.operandRead(S, ARM64.operands[1]);
      NdVar M = L.operandRead(S, ARM64.operands[2]);
      NdVar ExtN = S.makeTemp(8), ExtM = S.makeTemp(8);
      if (Insn->id == AARCH64_INS_SMSUBL) {
        S.emit(NdOp::INT_SEXT, ExtN, {N});
        S.emit(NdOp::INT_SEXT, ExtM, {M});
      } else {
        S.emit(NdOp::INT_ZEXT, ExtN, {N});
        S.emit(NdOp::INT_ZEXT, ExtM, {M});
      }
      NdVar Prod = S.makeTemp(8);
      S.emit(NdOp::INT_MULT, Prod, {ExtN, ExtM});
      S.emit(NdOp::INT_NEG2, Dst, {Prod});
      break;
    }
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar N = L.operandRead(S, ARM64.operands[1]);
    NdVar M = L.operandRead(S, ARM64.operands[2]);
    NdVar A = L.operandRead(S, ARM64.operands[3]);
    NdVar ExtN = S.makeTemp(8);
    NdVar ExtM = S.makeTemp(8);
    if (Insn->id == AARCH64_INS_SMSUBL) {
      S.emit(NdOp::INT_SEXT, ExtN, {N});
      S.emit(NdOp::INT_SEXT, ExtM, {M});
    } else {
      S.emit(NdOp::INT_ZEXT, ExtN, {N});
      S.emit(NdOp::INT_ZEXT, ExtM, {M});
    }
    NdVar Prod = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod, {ExtN, ExtM});
    S.emit(NdOp::INT_SUB, Dst, {A, Prod});
    break;
  }
  // MNEG → MSUB Xd, Xn, Xm, XZR (handled by MSUB case above)
  // LDNP / STNP (non-temporal pair)
  case AARCH64_INS_LDNP: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst1 = L.operandWrite(ARM64.operands[0]);
    NdVar Dst2 = L.operandWrite(ARM64.operands[1]);
    // Use the effective address (base+disp); operandRead would dereference the
    // `[Xn, #imm]` memory operand and use the loaded value as the address.
    NdVar EA = L.operandEffAddr(S, ARM64.operands[2]);
    S.emit(NdOp::LOAD, Dst1, {EA});
    NdVar EA2 = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(Dst1.Size, 8)});
    S.emit(NdOp::LOAD, Dst2, {EA2});
    break;
  }
  case AARCH64_INS_STNP: {
    if (ARM64.op_count < 3)
      break;
    NdVar Src1 = L.operandRead(S, ARM64.operands[0]);
    NdVar Src2 = L.operandRead(S, ARM64.operands[1]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[2]);
    S.emit(NdOp::STORE, {}, {EA, Src1});
    NdVar EA2 = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(Src1.Size, 8)});
    S.emit(NdOp::STORE, {}, {EA2, Src2});
    break;
  }
  default:
    return false;
  }
  return true;
}

} // namespace neverd
