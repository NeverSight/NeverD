//===- X86LiftCore.cpp - x86/x64 core instruction lifter ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Core x86/x64 integer ALU instruction handlers: MOV, arithmetic, logic,
/// shifts, rotates, multiply/divide, bit manipulation, flags, and sign
/// extension.  Control flow, atomics, and string/system ops are in separate
/// files.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool X86Lifter::liftCore(LiftState &S, const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // --- NOP ---
  case X86_INS_NOP:
  case X86_INS_FNOP:
    S.emit(NdOp::NOP, {}, {});
    break;

  // --- MOV (+ MOVABS, plus SSE/SIMD 128-bit register-to-register moves).
  // MOVAPS/MOVUPS/MOVDQA/MOVDQU/MOVD/MOVQ semantically are just "copy bytes"
  // for our purposes — the lift doesn't model float arithmetic separately
  // unless we later promote to FLOAT_* S.Ops. Treating them as bulk COPY at
  // least preserves dataflow (so the destination XMM reg has a defined
  // value) instead of dropping the instruction to NOP.
  case X86_INS_MOV:
  case X86_INS_MOVABS:
  case X86_INS_MOVZX:
  case X86_INS_MOVSX:
  case X86_INS_MOVSXD:
  case X86_INS_MOVAPS:
  case X86_INS_MOVAPD:
  case X86_INS_MOVUPS:
  case X86_INS_MOVUPD:
  case X86_INS_MOVDQA:
  case X86_INS_MOVDQU:
  case X86_INS_MOVD:
  case X86_INS_MOVQ:
  case X86_INS_MOVSS: {
    if (X86.op_count < 2)
      break;
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar DstV = operandWrite(X86.operands[0]);

    if (InsnId == X86_INS_MOVZX) {
      NdVar Ext = S.makeTemp(DstV.Size);
      S.emit(NdOp::INT_ZEXT, Ext, {Src});
      Src = Ext;
    } else if (InsnId == X86_INS_MOVSX || InsnId == X86_INS_MOVSXD) {
      NdVar Ext = S.makeTemp(DstV.Size);
      S.emit(NdOp::INT_SEXT, Ext, {Src});
      Src = Ext;
    }

    if (X86.operands[0].type == X86_OP_MEM) {
      // Scalar FP stores: truncate XMM to actual operand width
      if (InsnId == X86_INS_MOVSD && Src.Size > 8) {
        NdVar Lo = S.makeTemp(8);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        Src = Lo;
      } else if ((InsnId == X86_INS_MOVSS || InsnId == X86_INS_MOVD) &&
                 Src.Size > 4) {
        NdVar Lo = S.makeTemp(4);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        Src = Lo;
      } else if (InsnId == X86_INS_MOVQ && Src.Size > 8) {
        NdVar Lo = S.makeTemp(8);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        Src = Lo;
      }
      S.storeToMem(X86.operands[0], Src);
    } else {
      if (Src.Size > DstV.Size) {
        NdVar Lo = S.makeTemp(DstV.Size);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        Src = Lo;
      }

      // A narrow source moved into a wider XMM/MMX register zero-extends the
      // upper lanes for MOVD/MOVQ and for the MEMORY forms of MOVSS/MOVSD
      // (Intel SDM); the register forms of MOVSS/MOVSD merge instead (handled
      // below).  Make the zero extension explicit so the destination is a true
      // full-width def — a plain COPY of the narrow value leaves the upper
      // bytes stale, which a later full-width read (e.g. a movdqa spill that is
      // reloaded by psadbw) would otherwise expose as garbage.
      // (MOVSD reaches the dedicated handler in X86LiftString.cpp, not here.)
      bool ZeroExtendsUpper =
          (InsnId == X86_INS_MOVD || InsnId == X86_INS_MOVQ) ||
          (InsnId == X86_INS_MOVSS && X86.operands[1].type == X86_OP_MEM);
      if (Src.Size < DstV.Size && ZeroExtendsUpper) {
        NdVar Ext = S.makeTemp(DstV.Size);
        S.emit(NdOp::INT_ZEXT, Ext, {Src});
        Src = Ext;
      }

      // MOVSS/MOVSD reg-to-reg: merges only the low scalar element into
      // the destination, preserving its upper bits.  Other MOV variants
      // (MOVAPS, MOVDQA, etc.) are full copies.
      bool NeedsMerge = (InsnId == X86_INS_MOVSS || InsnId == X86_INS_MOVSD) &&
                        X86.operands[1].type == X86_OP_REG &&
                        DstV.Size > Src.Size;
      if (NeedsMerge) {
        // This is MOVSS/MOVSD from xmm to xmm — scalar merge.
        // But if Src was already truncated above, Src.Size == DstV.Size,
        // so NeedsMerge wouldn't trigger.  Handle the case where
        // both operands report full XMM size.
      }

      if ((InsnId == X86_INS_MOVSS || InsnId == X86_INS_MOVSD) &&
          X86.operands[0].type == X86_OP_REG &&
          X86.operands[1].type == X86_OP_REG && DstV.Size >= 16) {
        uint16_t ElemSz = (InsnId == X86_INS_MOVSS) ? 4 : 8;
        NdVar DstR = operandRead(S, X86.operands[0]);
        NdVar SrcLo = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, SrcLo, {Src, NdVar::cst(0, 4)});
        NdVar DstHi = S.makeTemp(DstV.Size - ElemSz);
        S.emit(NdOp::SUBBYTES, DstHi, {DstR, NdVar::cst(ElemSz, 4)});
        S.emit(NdOp::CONCAT, DstV, {DstHi, SrcLo});
      } else {
        S.emit(NdOp::COPY, DstV, {Src});
      }
    }
    break;
  }

  // MOVLPS/MOVLPD/MOVHPS/MOVHPD — partial 64-bit moves: each touches ONE half
  // of the XMM and must preserve / select the correct other half.
  //   store: movlps m64,xmm → m64 = xmm[63:0];  movhps m64,xmm → m64 =
  //   xmm[127:64] load:  movlps xmm,m64 → xmm[63:0]=m64, [127:64] kept
  //          movhps xmm,m64 → xmm[127:64]=m64, [63:0] kept
  // The old code shared the generic MOV path: stores always took the LOW qword
  // (so MOVHPS/MOVHPD stored the wrong half) and loads did a flat COPY of the
  // 8-byte memory value into the 16-byte XMM (clobbering the preserved half).
  case X86_INS_MOVLPS:
  case X86_INS_MOVLPD:
  case X86_INS_MOVHPS:
  case X86_INS_MOVHPD: {
    if (X86.op_count < 2)
      break;
    bool IsHigh = (InsnId == X86_INS_MOVHPS || InsnId == X86_INS_MOVHPD);
    if (X86.operands[0].type == X86_OP_MEM) {
      NdVar Src = operandRead(S, X86.operands[1]);
      NdVar Half = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Half, {Src, NdVar::cst(IsHigh ? 8 : 0, 4)});
      S.storeToMem(X86.operands[0], Half);
    } else {
      NdVar DstV = operandWrite(X86.operands[0]);
      NdVar DstR = operandRead(S, X86.operands[0]);
      NdVar Mem = operandRead(S, X86.operands[1]);
      NdVar MemLo = Mem;
      if (Mem.Size != 8) {
        MemLo = S.makeTemp(8);
        S.emit(NdOp::SUBBYTES, MemLo, {Mem, NdVar::cst(0, 4)});
      }
      if (IsHigh) {
        // high = m64, low preserved → {lo, m64}.
        NdVar Lo = S.makeTemp(8);
        S.emit(NdOp::SUBBYTES, Lo, {DstR, NdVar::cst(0, 4)});
        S.emit(NdOp::CONCAT, DstV, {MemLo, Lo});
      } else {
        // low = m64, high preserved → {m64, hi}.
        NdVar Hi = S.makeTemp(8);
        S.emit(NdOp::SUBBYTES, Hi, {DstR, NdVar::cst(8, 4)});
        S.emit(NdOp::CONCAT, DstV, {Hi, MemLo});
      }
    }
    break;
  }

  // --- FP conversions (scalar) ---
  case X86_INS_CVTTSD2SI:
  case X86_INS_CVTTSS2SI: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    unsigned FPSz = (Insn->id == X86_INS_CVTTSS2SI) ? 4 : 8;
    if (Src.Size > FPSz) {
      NdVar Narrow = S.makeTemp(FPSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Src, NdVar::cst(0, 4)});
      Src = Narrow;
    }
    S.emit(NdOp::FLOAT_TRUNC, Dst, {Src});
    break;
  }
  case X86_INS_CVTSD2SI:
  case X86_INS_CVTSS2SI: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    unsigned FPSz = (Insn->id == X86_INS_CVTSS2SI) ? 4 : 8;
    if (Src.Size > FPSz) {
      NdVar Narrow = S.makeTemp(FPSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Src, NdVar::cst(0, 4)});
      Src = Narrow;
    }
    // CVTSD2SI/CVTSS2SI round using MXCSR (default: nearest, ties to even),
    // unlike the truncating CVTTSD2SI/CVTTSS2SI.  Round first, then convert.
    NdVar Rounded = S.makeTemp(FPSz);
    S.emit(NdOp::FLOAT_ROUNDEVEN, Rounded, {Src});
    S.emit(NdOp::FLOAT_FLOAT2INT, Dst, {Rounded});
    break;
  }
  case X86_INS_CVTSI2SD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Tmp = S.makeTemp(8);
    S.emit(NdOp::FLOAT_INT2FLOAT, Tmp, {Src});
    // NOTE: this writes the low double as a narrow (8-byte) scalar value and
    // does NOT rebuild the destination's upper 64 bits.  That is deliberate and
    // matches the codebase-wide scalar-SSE convention: a scalar XMM value is
    // kept narrow-typed so downstream scalar ops (sqrtss/mulss/comiss/...)
    // infer the right FP width.  Forcing a 16-byte CONCAT here to preserve
    // xmm[127:64] re-types the register as i128, and inferFloatTy() then
    // mis-selects double for those consumers.  Real
    // compilers never read the upper lane after CVTSI2*, so the divergence is
    // unobservable in practice; only hand-written asm can see it.
    S.emit(NdOp::COPY, Dst, {Tmp});
    break;
  }
  case X86_INS_CVTSI2SS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Tmp = S.makeTemp(4);
    S.emit(NdOp::FLOAT_INT2FLOAT, Tmp, {Src});
    // See CVTSI2SD above: narrow scalar write by design; upper lanes are not
    // rebuilt to keep the value float-typed for downstream scalar consumers.
    S.emit(NdOp::COPY, Dst, {Tmp});
    break;
  }
  case X86_INS_CVTSD2SS:
  case X86_INS_CVTSS2SD: {
    if (X86.op_count < 2)
      break;
    bool ToDouble = (Insn->id == X86_INS_CVTSS2SD);
    unsigned SrcFPSz = ToDouble ? 4 : 8;
    unsigned DstFPSz = ToDouble ? 8 : 4;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    // Narrow the source to its real scalar FP width: a full XMM (16 bytes)
    // would make the emitter mis-infer the source type and pick the wrong
    // conversion direction.
    if (Src.Size > SrcFPSz) {
      NdVar N = S.makeTemp(SrcFPSz);
      S.emit(NdOp::SUBBYTES, N, {Src, NdVar::cst(0, 4)});
      Src = N;
    }
    NdVar Tmp = S.makeTemp(DstFPSz);
    S.emit(NdOp::FLOAT_FLOAT2FLOAT, Tmp, {Src});
    if (X86.operands[0].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[0], Tmp);
    } else if (Dst.Size > DstFPSz) {
      // Scalar SSE conversions preserve the upper lanes of the destination.
      NdVar Hi = S.makeTemp(Dst.Size - DstFPSz);
      S.emit(NdOp::SUBBYTES, Hi, {Dst, NdVar::cst(DstFPSz, 4)});
      S.emit(NdOp::CONCAT, Dst, {Hi, Tmp});
    } else {
      S.emit(NdOp::COPY, Dst, {Tmp});
    }
    break;
  }

  // --- LEA ---
  case X86_INS_LEA: {
    if (X86.op_count < 2)
      break;
    NdVar DstV = operandWrite(X86.operands[0]);
    NdVar EA = S.computeEA(X86.operands[1]);
    if (EA.Size > DstV.Size) {
      NdVar Trunc = S.makeTemp(DstV.Size);
      S.emit(NdOp::SUBBYTES, Trunc, {EA, NdVar::cst(0, 4)});
      EA = Trunc;
    }
    S.emit(NdOp::COPY, DstV, {EA});
    break;
  }

  // --- ADD ---
  case X86_INS_ADD: {
    if (X86.op_count < 2)
      break;
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar DstW = operandWrite(X86.operands[0]);
    // Snapshot operands into temps before INT_ADD writes to DstW, so that
    // sub-register aliasing in LowToMed cannot redirect the flag operands
    // to the post-add value (fixes byte-level addb OF bug).
    NdVar FlagA = S.makeTemp(DstR.Size);
    S.emit(NdOp::COPY, FlagA, {DstR});
    NdVar FlagB = S.makeTemp(Src.Size);
    S.emit(NdOp::COPY, FlagB, {Src});
    NdVar Result =
        (X86.operands[0].type == X86_OP_MEM) ? S.makeTemp(DstR.Size) : DstW;
    S.emit(NdOp::INT_ADD, Result, {DstR, Src});
    emitFlagsArith(S, Result, FlagA, FlagB, false);
    if (X86.operands[0].type == X86_OP_MEM)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // --- SUB ---
  case X86_INS_SUB: {
    if (X86.op_count < 2)
      break;
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar DstW = operandWrite(X86.operands[0]);
    NdVar FlagA = S.makeTemp(DstR.Size);
    S.emit(NdOp::COPY, FlagA, {DstR});
    NdVar FlagB = S.makeTemp(Src.Size);
    S.emit(NdOp::COPY, FlagB, {Src});
    NdVar Result =
        (X86.operands[0].type == X86_OP_MEM) ? S.makeTemp(DstR.Size) : DstW;
    S.emit(NdOp::INT_SUB, Result, {DstR, Src});
    emitFlagsArith(S, Result, FlagA, FlagB, true);
    if (X86.operands[0].type == X86_OP_MEM)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // --- CMP ---
  case X86_INS_CMP: {
    if (X86.op_count < 2)
      break;
    NdVar A = operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[1]);
    NdVar TmpR = S.makeTemp(A.Size);
    S.emit(NdOp::INT_SUB, TmpR, {A, B});
    emitFlagsArith(S, TmpR, A, B, true);
    break;
  }

  // --- TEST ---
  case X86_INS_TEST: {
    if (X86.op_count < 2)
      break;
    NdVar A = operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[1]);
    NdVar TmpR = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, TmpR, {A, B});
    emitFlagsLogic(S, TmpR);
    break;
  }

  // --- AND / OR / XOR ---
  case X86_INS_AND:
  case X86_INS_OR:
  case X86_INS_XOR: {
    if (X86.op_count < 2)
      break;

    // Idiom: xor reg, reg → COPY reg = 0 (avoids false live-in)
    if (InsnId == X86_INS_XOR && X86.operands[0].type == X86_OP_REG &&
        X86.operands[1].type == X86_OP_REG &&
        X86.operands[0].reg == X86.operands[1].reg) {
      NdVar DstW = operandWrite(X86.operands[0]);
      S.emit(NdOp::COPY, DstW, {NdVar::cst(0, DstW.Size)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::ZF, 1), {NdVar::cst(1, 1)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::SF, 1), {NdVar::cst(0, 1)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::PF, 1), {NdVar::cst(1, 1)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NdVar::cst(0, 1)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::cst(0, 1)});
      break;
    }

    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar DstW = operandWrite(X86.operands[0]);

    // Idiom: test reg, reg (AND with itself) → just set flags, don't overwrite
    // reg
    if (InsnId == X86_INS_AND && X86.operands[0].type == X86_OP_REG &&
        X86.operands[1].type == X86_OP_REG &&
        X86.operands[0].reg == X86.operands[1].reg) {
      NdVar TmpV = S.makeTemp(DstR.Size);
      S.emit(NdOp::INT_AND, TmpV, {DstR, Src});
      emitFlagsLogic(S, TmpV);
      break;
    }

    NdOp Opc = NdOp::INT_AND;
    if (InsnId == X86_INS_OR)
      Opc = NdOp::INT_OR;
    if (InsnId == X86_INS_XOR)
      Opc = NdOp::INT_XOR;

    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    NdVar Result = MemDst ? S.makeTemp(DstR.Size) : DstW;
    S.emit(Opc, Result, {DstR, Src});
    emitFlagsLogic(S, Result);
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // --- SHL / SHR / SAR ---
  case X86_INS_SHL:
  case X86_INS_SAL:
  case X86_INS_SHR:
  case X86_INS_SAR: {
    if (X86.op_count < 2)
      break;
    NdVar Cnt = operandRead(S, X86.operands[1]);
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar DstW = operandWrite(X86.operands[0]);

    if (Cnt.isReg() && Cnt.Size == 1)
      Cnt.Size = 4;

    uint16_t Sz = DstR.Size;
    uint16_t Bits = Sz * 8;

    // x86 masks the shift count: 0x1F for 8/16/32-bit, 0x3F for 64-bit
    uint64_t ShiftMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar MaskedCnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, MaskedCnt, {Cnt, NdVar::cst(ShiftMask, Sz)});

    // Snapshot the source before the shift writes the (aliased) destination, so
    // SHR's OF (= MSB of the original operand) reads the pre-shift value.
    NdVar PreSrc = S.makeTemp(Sz);
    S.emit(NdOp::COPY, PreSrc, {DstR});

    // Snapshot the flags so a zero count can restore them (x86 leaves every
    // flag unchanged when the masked shift count is 0).
    NdVar OldCF = S.makeTemp(1);
    S.emit(NdOp::COPY, OldCF, {NdVar::reg(x86reg::CF, 1)});
    NdVar OldZF = S.makeTemp(1);
    S.emit(NdOp::COPY, OldZF, {NdVar::reg(x86reg::ZF, 1)});
    NdVar OldSF = S.makeTemp(1);
    S.emit(NdOp::COPY, OldSF, {NdVar::reg(x86reg::SF, 1)});
    NdVar OldPF = S.makeTemp(1);
    S.emit(NdOp::COPY, OldPF, {NdVar::reg(x86reg::PF, 1)});

    // CF = last bit Shifted out (valid when Cnt >= 1).
    // SHL: bit (Bits - Cnt); SHR/SAR: bit (Cnt - 1)
    {
      NdVar CfIdx = S.makeTemp(Sz);
      if (InsnId == X86_INS_SHL || InsnId == X86_INS_SAL)
        S.emit(NdOp::INT_SUB, CfIdx, {NdVar::cst(Bits, Sz), MaskedCnt});
      else
        S.emit(NdOp::INT_SUB, CfIdx, {MaskedCnt, NdVar::cst(1, Sz)});
      NdVar CfTmp = S.makeTemp(Sz);
      S.emit(NdOp::INT_RIGHT, CfTmp, {DstR, CfIdx});
      NdVar CfBit = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, CfBit, {CfTmp, NdVar::cst(0, 4)});
      S.emit(NdOp::INT_AND, CfBit, {CfBit, NdVar::cst(1, 1)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {CfBit});
    }

    NdOp Opc = NdOp::INT_LEFT;
    if (InsnId == X86_INS_SHR)
      Opc = NdOp::INT_RIGHT;
    if (InsnId == X86_INS_SAR)
      Opc = NdOp::INT_ASHR;

    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    NdVar Result = MemDst ? S.makeTemp(DstR.Size) : DstW;
    S.emit(Opc, Result, {DstR, MaskedCnt});
    emitZSPF(S, Result);
    // OF (1-bit shifts only): SHL = MSB(result) ^ CF, SHR = MSB(source),
    // SAR = 0.  emitShiftRotateOF leaves OF unchanged for any other count.
    NdVar OfBit = S.makeTemp(1);
    if (InsnId == X86_INS_SHR) {
      S.emit(NdOp::COPY, OfBit, {extractBit(S, PreSrc, Bits - 1)});
    } else if (InsnId == X86_INS_SAR) {
      S.emit(NdOp::COPY, OfBit, {NdVar::cst(0, 1)});
    } else {
      S.emit(NdOp::BOOL_XOR, OfBit,
             {extractBit(S, Result, Bits - 1), NdVar::reg(x86reg::CF, 1)});
    }
    emitShiftRotateOF(S, MaskedCnt, OfBit);
    emitZeroCountFlagGuard(S, MaskedCnt,
                           {{x86reg::CF, OldCF},
                            {x86reg::ZF, OldZF},
                            {x86reg::SF, OldSF},
                            {x86reg::PF, OldPF}});
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // --- INC / DEC ---
  case X86_INS_INC:
  case X86_INS_DEC: {
    if (X86.op_count < 1)
      break;
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar DstW = operandWrite(X86.operands[0]);
    NdVar One = NdVar::cst(1, DstR.Size);
    bool IsInc = (InsnId == X86_INS_INC);
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    // Snapshot the source before INT_ADD/INT_SUB writes DstW: for a register
    // operand DstR and DstW alias the same reg, so a later AF read of DstR
    // would see the post-update value (sub-register aliasing, cf. ADD).
    NdVar PreVal = S.makeTemp(DstR.Size);
    S.emit(NdOp::COPY, PreVal, {DstR});
    NdVar Result = MemDst ? S.makeTemp(DstR.Size) : DstW;
    S.emit(IsInc ? NdOp::INT_ADD : NdOp::INT_SUB, Result, {DstR, One});
    emitZSPF(S, Result);
    emitAF(S, Result, PreVal, One);
    // OF must use the pre-update source (PreVal); for a register operand a bare
    // DstR read here is redirected to the post-update value, so e.g. incb 0x7F
    // computed OF from 0x80 (no overflow) instead of 0x7F (overflow).
    if (IsInc)
      S.emit(NdOp::INT_SOVF, NdVar::reg(x86reg::OF, 1), {PreVal, One});
    else
      S.emit(NdOp::INT_SBOR, NdVar::reg(x86reg::OF, 1), {PreVal, One});
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // --- NEG / NOT ---
  case X86_INS_NEG: {
    if (X86.op_count < 1)
      break;
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar DstW = operandWrite(X86.operands[0]);
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    // Snapshot before INT_NEG2 overwrites DstW (register NEG aliases DstR).
    NdVar PreVal = S.makeTemp(DstR.Size);
    S.emit(NdOp::COPY, PreVal, {DstR});
    NdVar Result = MemDst ? S.makeTemp(DstR.Size) : DstW;
    S.emit(NdOp::INT_NEG2, Result, {DstR});
    // CF/OF use the pre-update source (PreVal): a register NEG aliases
    // DstR/DstW so a post-2COMP read of DstR would be the negated value.
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
           {PreVal, NdVar::cst(0, DstR.Size)});
    emitZSPF(S, Result);
    emitAF(S, Result, NdVar::cst(0, DstR.Size), PreVal);
    S.emit(NdOp::INT_SBOR, NdVar::reg(x86reg::OF, 1),
           {NdVar::cst(0, DstR.Size), PreVal});
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }
  case X86_INS_NOT: {
    if (X86.op_count < 1)
      break;
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar DstW = operandWrite(X86.operands[0]);
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    NdVar Result = MemDst ? S.makeTemp(DstR.Size) : DstW;
    S.emit(NdOp::INT_NOT, Result, {DstR});
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // --- IMUL ---
  case X86_INS_IMUL: {
    if (X86.op_count == 1) {
      NdVar Src = operandRead(S, X86.operands[0]);
      uint16_t Sz = Src.Size;
      if (Sz == 1) {
        // 8-bit: AX = AL * r/m8 (signed, Result in AX)
        NdVar Al = NdVar::reg(x86reg::RAX, 1);
        NdVar Ax = NdVar::reg(x86reg::RAX, 2);
        NdVar ExtA = S.makeTemp(2);
        NdVar ExtB = S.makeTemp(2);
        S.emit(NdOp::INT_SEXT, ExtA, {Al});
        S.emit(NdOp::INT_SEXT, ExtB, {Src});
        S.emit(NdOp::INT_MULT, Ax, {ExtA, ExtB});
        NdVar LowSext = S.makeTemp(2);
        S.emit(NdOp::INT_SEXT, LowSext, {NdVar::reg(x86reg::RAX, 1)});
        S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1), {LowSext, Ax});
        S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1),
               {NdVar::reg(x86reg::CF, 1)});
      } else {
        NdVar Rax = NdVar::reg(x86reg::RAX, Sz);
        NdVar Rdx = NdVar::reg(x86reg::RDX, Sz);
        NdVar ExtA = S.makeTemp(Sz * 2);
        NdVar ExtB = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_SEXT, ExtA, {Rax});
        S.emit(NdOp::INT_SEXT, ExtB, {Src});
        NdVar Full = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_MULT, Full, {ExtA, ExtB});
        S.emit(NdOp::SUBBYTES, Rax, {Full, NdVar::cst(0, 4)});
        S.emit(NdOp::SUBBYTES, Rdx, {Full, NdVar::cst(Sz, 4)});
        NdVar LowSext = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_SEXT, LowSext, {Rax});
        S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
               {LowSext, Full});
        S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1),
               {NdVar::reg(x86reg::CF, 1)});
      }
      break;
    }
    NdVar Dst{}, MulA{}, MulB{};
    if (X86.op_count == 2) {
      MulA = operandRead(S, X86.operands[0]);
      MulB = operandRead(S, X86.operands[1]);
      Dst = operandWrite(X86.operands[0]);
    } else if (X86.op_count == 3) {
      MulA = operandRead(S, X86.operands[1]);
      MulB = operandRead(S, X86.operands[2]);
      Dst = operandWrite(X86.operands[0]);
    } else {
      break;
    }
    // CF=OF: widen both operands to double-size, multiply, then compare the
    // sign-extended truncated Result against the Full product.  The operands
    // are sign-extended (and the full product formed) BEFORE Dst is written, so
    // the flags stay correct when Dst aliases a source (`imul r,r/m`/`imul
    // r,r,imm`). The result keeps a native-width INT_MULT — only the flag path
    // uses the double-width product, so it is dropped when flags are dead (no
    // i128 lib call for a 64-bit imul whose flags are unused).
    NdVar ExtA = S.makeTemp(Dst.Size * 2);
    NdVar ExtB = S.makeTemp(Dst.Size * 2);
    S.emit(NdOp::INT_SEXT, ExtA, {MulA});
    S.emit(NdOp::INT_SEXT, ExtB, {MulB});
    NdVar Full = S.makeTemp(Dst.Size * 2);
    S.emit(NdOp::INT_MULT, Full, {ExtA, ExtB});
    S.emit(NdOp::INT_MULT, Dst, {MulA, MulB});
    NdVar ExtRes = S.makeTemp(Dst.Size * 2);
    S.emit(NdOp::INT_SEXT, ExtRes, {Dst});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1), {ExtRes, Full});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1),
           {NdVar::reg(x86reg::CF, 1)});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Dst, NdVar::cst(0, Dst.Size)});
    S.emit(NdOp::INT_SLESS, NdVar::reg(x86reg::SF, 1),
           {Dst, NdVar::cst(0, Dst.Size)});
    break;
  }

  // --- XCHG ---
  case X86_INS_XCHG: {
    if (X86.op_count < 2)
      break;
    NdVar A = operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[1]);
    NdVar TmpV = S.makeTemp(A.Size);
    S.emit(NdOp::COPY, TmpV, {A});
    // A MEMORY operand must be written with an explicit STORE — operandWrite()
    // returns a discarded ram(0) placeholder, so `xchg [mem],reg` previously
    // updated only the register and silently dropped the memory half.  Do the
    // store FIRST (using the original register values for its address) before
    // the register write, so a register that also indexes the address (e.g.
    // `xchg rax,[rax]`) is not clobbered first.
    if (X86.operands[0].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[0], B);
      S.emit(NdOp::COPY, operandWrite(X86.operands[1]), {TmpV});
    } else if (X86.operands[1].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[1], TmpV);
      S.emit(NdOp::COPY, operandWrite(X86.operands[0]), {B});
    } else {
      S.emit(NdOp::COPY, operandWrite(X86.operands[0]), {B});
      S.emit(NdOp::COPY, operandWrite(X86.operands[1]), {TmpV});
    }
    break;
  }

  // --- CDQ/CDQE/CQO ---
  case X86_INS_CDQ:
    S.emit(NdOp::INT_ASHR, NdVar::reg(x86reg::RDX, 4),
           {NdVar::reg(x86reg::RAX, 4), NdVar::cst(31, 4)});
    break;
  case X86_INS_CQO:
    S.emit(NdOp::INT_ASHR, NdVar::reg(x86reg::RDX, 8),
           {NdVar::reg(x86reg::RAX, 8), NdVar::cst(63, 8)});
    break;
  case X86_INS_CDQE:
    S.emit(NdOp::INT_SEXT, NdVar::reg(x86reg::RAX, 8),
           {NdVar::reg(x86reg::RAX, 4)});
    break;

  // --- MUL / DIV ---
  case X86_INS_MUL: {
    if (X86.op_count < 1)
      break;
    NdVar Src = operandRead(S, X86.operands[0]);
    uint16_t Sz = Src.Size;
    if (Sz == 1) {
      // 8-bit: AX = AL * r/m8 (Result in AX, not DL:AL)
      NdVar Al = NdVar::reg(x86reg::RAX, 1);
      NdVar Ax = NdVar::reg(x86reg::RAX, 2);
      NdVar ExtA = S.makeTemp(2);
      NdVar ExtB = S.makeTemp(2);
      S.emit(NdOp::INT_ZEXT, ExtA, {Al});
      S.emit(NdOp::INT_ZEXT, ExtB, {Src});
      S.emit(NdOp::INT_MULT, Ax, {ExtA, ExtB});
      NdVar Ah = NdVar::reg(x86reg::RAX + 1, 1);
      S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
             {Ah, NdVar::cst(0, 1)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1),
             {NdVar::reg(x86reg::CF, 1)});
    } else {
      NdVar Rax = NdVar::reg(x86reg::RAX, Sz);
      NdVar Rdx = NdVar::reg(x86reg::RDX, Sz);
      NdVar ExtA = S.makeTemp(Sz * 2);
      NdVar ExtB = S.makeTemp(Sz * 2);
      S.emit(NdOp::INT_ZEXT, ExtA, {Rax});
      S.emit(NdOp::INT_ZEXT, ExtB, {Src});
      NdVar Full = S.makeTemp(Sz * 2);
      S.emit(NdOp::INT_MULT, Full, {ExtA, ExtB});
      S.emit(NdOp::SUBBYTES, Rax, {Full, NdVar::cst(0, 4)});
      S.emit(NdOp::SUBBYTES, Rdx, {Full, NdVar::cst(Sz, 4)});
      S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
             {Rdx, NdVar::cst(0, Sz)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1),
             {NdVar::reg(x86reg::CF, 1)});
    }
    break;
  }
  case X86_INS_DIV: {
    if (X86.op_count < 1)
      break;
    NdVar Src = operandRead(S, X86.operands[0]);
    uint16_t Sz = Src.Size;
    if (Sz == 1) {
      NdVar Ax = NdVar::reg(x86reg::RAX, 2);
      NdVar ExtSrc = S.makeTemp(2);
      S.emit(NdOp::INT_ZEXT, ExtSrc, {Src});
      NdVar Quot = S.makeTemp(2);
      NdVar Rem = S.makeTemp(2);
      S.emit(NdOp::INT_DIV, Quot, {Ax, ExtSrc});
      S.emit(NdOp::INT_REM, Rem, {Ax, ExtSrc});
      S.emit(NdOp::SUBBYTES, NdVar::reg(x86reg::RAX, 1),
             {Quot, NdVar::cst(0, 4)});
      S.emit(NdOp::SUBBYTES, NdVar::reg(x86reg::RAX + 1, 1),
             {Rem, NdVar::cst(0, 4)});
    } else {
      NdVar Rax = NdVar::reg(x86reg::RAX, Sz);
      NdVar Rdx = NdVar::reg(x86reg::RDX, Sz);

      bool IsZeroRdx = (LastRdxState == RdxState::Zero && LastRdxSize >= Sz);

      if (IsZeroRdx) {
        NdVar OrigRax = S.makeTemp(Sz);
        S.emit(NdOp::COPY, OrigRax, {Rax});
        S.emit(NdOp::INT_DIV, Rax, {OrigRax, Src});
        S.emit(NdOp::INT_REM, Rdx, {OrigRax, Src});
      } else {
        // The double-width INT_DIV/INT_REM below is recognized by the value
        // emitter and lowered back to a single `divq` via inline-asm
        // passthrough (no __udivti3/__umodti3 libcall) — original binary stays
        // binary.
        LLVM_DEBUG(if (Sz * 2 > 8) llvm::dbgs()
                   << "DIV wide fallback: i" << (Sz * 2 * 8)
                   << " udiv/urem at 0x" << llvm::utohexstr(S.Addr)
                   << " — emitter lowers to inline-asm div\n");
        NdVar ExtRAX = S.makeTemp(Sz * 2);
        NdVar ExtRDX = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_ZEXT, ExtRAX, {Rax});
        S.emit(NdOp::INT_ZEXT, ExtRDX, {Rdx});
        NdVar HiShifted = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_LEFT, HiShifted,
               {ExtRDX, NdVar::cst(Sz * 8, Sz * 2)});
        NdVar Dividend = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_OR, Dividend, {HiShifted, ExtRAX});
        NdVar ExtSrc = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_ZEXT, ExtSrc, {Src});
        NdVar Quot = S.makeTemp(Sz * 2);
        NdVar Rem = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_DIV, Quot, {Dividend, ExtSrc});
        S.emit(NdOp::INT_REM, Rem, {Dividend, ExtSrc});
        S.emit(NdOp::SUBBYTES, Rax, {Quot, NdVar::cst(0, 4)});
        S.emit(NdOp::SUBBYTES, Rdx, {Rem, NdVar::cst(0, 4)});
      }
    }
    break;
  }
  case X86_INS_IDIV: {
    if (X86.op_count < 1)
      break;
    NdVar Src = operandRead(S, X86.operands[0]);
    uint16_t Sz = Src.Size;
    if (Sz == 1) {
      NdVar Ax = NdVar::reg(x86reg::RAX, 2);
      NdVar ExtSrc = S.makeTemp(2);
      S.emit(NdOp::INT_SEXT, ExtSrc, {Src});
      NdVar Quot = S.makeTemp(2);
      NdVar Rem = S.makeTemp(2);
      S.emit(NdOp::INT_SDIV, Quot, {Ax, ExtSrc});
      S.emit(NdOp::INT_SREM, Rem, {Ax, ExtSrc});
      S.emit(NdOp::SUBBYTES, NdVar::reg(x86reg::RAX, 1),
             {Quot, NdVar::cst(0, 4)});
      S.emit(NdOp::SUBBYTES, NdVar::reg(x86reg::RAX + 1, 1),
             {Rem, NdVar::cst(0, 4)});
    } else {
      NdVar Rax = NdVar::reg(x86reg::RAX, Sz);
      NdVar Rdx = NdVar::reg(x86reg::RDX, Sz);

      bool IsCqoIdiom =
          (LastRdxState == RdxState::SignExtRAX && LastRdxSize == Sz);

      if (IsCqoIdiom) {
        NdVar OrigRax = S.makeTemp(Sz);
        S.emit(NdOp::COPY, OrigRax, {Rax});
        S.emit(NdOp::INT_SDIV, Rax, {OrigRax, Src});
        S.emit(NdOp::INT_SREM, Rdx, {OrigRax, Src});
      } else {
        // The double-width INT_SDIV/INT_SREM below is recognized by the value
        // emitter and lowered back to a single `idiv` via inline-asm
        // passthrough (no __divti3/__modti3 libcall) — original binary stays
        // binary.
        LLVM_DEBUG(if (Sz * 2 > 8) llvm::dbgs()
                   << "IDIV wide fallback: i" << (Sz * 2 * 8)
                   << " sdiv/srem at 0x" << llvm::utohexstr(S.Addr)
                   << " — emitter lowers to inline-asm idiv\n");
        NdVar ExtRAX = S.makeTemp(Sz * 2);
        NdVar ExtRDX = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_ZEXT, ExtRAX, {Rax});
        S.emit(NdOp::INT_SEXT, ExtRDX, {Rdx});
        NdVar HiShifted = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_LEFT, HiShifted,
               {ExtRDX, NdVar::cst(Sz * 8, Sz * 2)});
        NdVar Dividend = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_OR, Dividend, {HiShifted, ExtRAX});
        NdVar ExtSrc = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_SEXT, ExtSrc, {Src});
        NdVar Quot = S.makeTemp(Sz * 2);
        NdVar Rem = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_SDIV, Quot, {Dividend, ExtSrc});
        S.emit(NdOp::INT_SREM, Rem, {Dividend, ExtSrc});
        S.emit(NdOp::SUBBYTES, Rax, {Quot, NdVar::cst(0, 4)});
        S.emit(NdOp::SUBBYTES, Rdx, {Rem, NdVar::cst(0, 4)});
      }
    }
    break;
  }

  // --- ROL / ROR ---
  case X86_INS_ROL:
  case X86_INS_ROR: {
    if (X86.op_count < 2)
      break;
    NdVar Cnt = operandRead(S, X86.operands[1]);
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar DstW = operandWrite(X86.operands[0]);
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    NdVar Result = MemDst ? S.makeTemp(DstR.Size) : DstW;
    uint16_t Sz = DstR.Size;
    uint16_t Bits = Sz * 8;

    // x86 masks rotate count: 0x1F for 8/16/32-bit, 0x3F for 64-bit
    uint64_t RotMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar MaskedCnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, MaskedCnt, {Cnt, NdVar::cst(RotMask, Sz)});
    // BYTE/WORD rotates take a SECOND reduction mod the operand size (Intel
    // SDM: tempCOUNT = (COUNT AND 1Fh) MOD size).  Since size is a power of two
    // this is `& (Bits-1)`.  Without it, e.g. `rolb $9` feeds x<<9 into the
    // saturating INT_LEFT (over-shift -> 0), dropping the high half.  32/64-bit
    // need no step (the 5/6-bit mask already yields a count < size).
    if (Bits < 32)
      S.emit(NdOp::INT_AND, MaskedCnt, {MaskedCnt, NdVar::cst(Bits - 1, Sz)});

    // Rotates affect only CF and OF; snapshot CF so a zero count preserves it.
    NdVar OldCF = S.makeTemp(1);
    S.emit(NdOp::COPY, OldCF, {NdVar::reg(x86reg::CF, 1)});

    if (InsnId == X86_INS_ROL) {
      NdVar Shl = S.makeTemp(Sz);
      NdVar Comp = S.makeTemp(Sz);
      NdVar Shr = S.makeTemp(Sz);
      S.emit(NdOp::INT_LEFT, Shl, {DstR, MaskedCnt});
      S.emit(NdOp::INT_SUB, Comp, {NdVar::cst(Bits, Sz), MaskedCnt});
      S.emit(NdOp::INT_AND, Comp, {Comp, NdVar::cst(Bits - 1, Sz)});
      S.emit(NdOp::INT_RIGHT, Shr, {DstR, Comp});
      S.emit(NdOp::INT_OR, Result, {Shl, Shr});
      NdVar CfTmp = S.makeTemp(Sz);
      S.emit(NdOp::INT_AND, CfTmp, {Result, NdVar::cst(1, Sz)});
      S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
             {CfTmp, NdVar::cst(0, Sz)});
    } else {
      NdVar Shr = S.makeTemp(Sz);
      NdVar Comp = S.makeTemp(Sz);
      NdVar Shl = S.makeTemp(Sz);
      S.emit(NdOp::INT_RIGHT, Shr, {DstR, MaskedCnt});
      S.emit(NdOp::INT_SUB, Comp, {NdVar::cst(Bits, Sz), MaskedCnt});
      S.emit(NdOp::INT_AND, Comp, {Comp, NdVar::cst(Bits - 1, Sz)});
      S.emit(NdOp::INT_LEFT, Shl, {DstR, Comp});
      S.emit(NdOp::INT_OR, Result, {Shr, Shl});
      NdVar CfTmp = S.makeTemp(Sz);
      S.emit(NdOp::INT_RIGHT, CfTmp, {Result, NdVar::cst(Bits - 1, Sz)});
      S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
             {CfTmp, NdVar::cst(0, Sz)});
    }
    // OF (1-bit rotates only): ROL = MSB(result) ^ LSB(result),
    // ROR = MSB(result) ^ next-MSB(result).
    NdVar OfBit = S.makeTemp(1);
    if (InsnId == X86_INS_ROL)
      S.emit(NdOp::BOOL_XOR, OfBit,
             {extractBit(S, Result, Bits - 1), extractBit(S, Result, 0)});
    else
      S.emit(
          NdOp::BOOL_XOR, OfBit,
          {extractBit(S, Result, Bits - 1), extractBit(S, Result, Bits - 2)});
    emitShiftRotateOF(S, MaskedCnt, OfBit);
    emitZeroCountFlagGuard(S, MaskedCnt, {{x86reg::CF, OldCF}});
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // --- BSF / BSR ---
  // When the source is zero, ZF=1 and the destination is left UNCHANGED.  The
  // Intel manual labels the destination "undefined" in that case, but real
  // hardware (and QEMU/Unicorn, via a conditional move) preserve the prior
  // destination value, and real programs depend on it.  The old code wrote the
  // computed value unconditionally — which for a zero source is (Bits-1)-clz(0)
  // = -1, clobbering the destination.  Select the old destination on src==0.
  case X86_INS_BSF:
  case X86_INS_BSR: {
    if (X86.op_count < 2)
      break;
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar OldDst = operandRead(S, X86.operands[0]);
    NdVar DstW = operandWrite(X86.operands[0]);
    NdVar SrcZero = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, SrcZero, {Src, NdVar::cst(0, Src.Size)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::ZF, 1), {SrcZero});
    NdVar Computed = S.makeTemp(DstW.Size);
    if (InsnId == X86_INS_BSR) {
      // BSR: index of highest set bit = (Bits-1) - CLZ(Src)
      NdVar Clz = S.makeTemp(DstW.Size);
      S.emit(NdOp::LZCOUNT, Clz, {Src});
      S.emit(NdOp::INT_SUB, Computed,
             {NdVar::cst(DstW.Size * 8 - 1, DstW.Size), Clz});
    } else {
      // BSF: index of lowest set bit = (Bits-1) - CLZ(Src & -Src)
      // isolate lowest bit: low = Src & (-Src)
      NdVar Neg = S.makeTemp(Src.Size);
      S.emit(NdOp::INT_NEG2, Neg, {Src});
      NdVar LowBit = S.makeTemp(Src.Size);
      S.emit(NdOp::INT_AND, LowBit, {Src, Neg});
      NdVar Clz = S.makeTemp(DstW.Size);
      S.emit(NdOp::LZCOUNT, Clz, {LowBit});
      S.emit(NdOp::INT_SUB, Computed,
             {NdVar::cst(DstW.Size * 8 - 1, DstW.Size), Clz});
    }
    // Preserve the destination when the source is zero (matches hardware/QEMU).
    S.emit(NdOp::SELECT, DstW, {SrcZero, OldDst, Computed});
    break;
  }

  // --- SBB (subtract with borrow): Dst = Dst - Src - CF; CF = combined borrow.
  case X86_INS_SBB: {
    if (X86.op_count < 2)
      break;
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar DstW = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    // Snapshot operands before the result overwrites DstW, so the borrow/OF
    // flags read the pre-write values (DstW aliases operand[0]).  Without this
    // a multi-limb sbb chain consumes a borrow computed from the result.
    NdVar A = S.makeTemp(DstR.Size);
    S.emit(NdOp::COPY, A, {DstR});
    NdVar B = S.makeTemp(Src.Size);
    S.emit(NdOp::COPY, B, {Src});
    NdVar Result = MemDst ? S.makeTemp(A.Size) : DstW;
    NdVar CfExt = S.makeTemp(A.Size);
    S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(x86reg::CF, 1)});
    NdVar CarryInner = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, CarryInner, {B, CfExt});
    NdVar Adj = S.makeTemp(A.Size);
    S.emit(NdOp::INT_ADD, Adj, {B, CfExt});
    S.emit(NdOp::INT_SUB, Result, {A, Adj});
    NdVar BorrowOuter = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, BorrowOuter, {A, Adj});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::CF, 1),
           {CarryInner, BorrowOuter});
    emitZSPF(S, Result);
    // OF: two-stage sborrow XOR — sborrow(Dst,Src) ^ sborrow(Dst-Src, Cf).
    // Using INT_SBOR(Dst, Adj) alone is WRONG when Src+CF wraps
    // (e.g. Src=0x7F, CF=1 → Adj=0x80 flips the sign of the subtrahend).
    {
      NdVar Temp = S.makeTemp(A.Size);
      S.emit(NdOp::INT_SUB, Temp, {A, B});
      NdVar SB1 = S.makeTemp(1);
      S.emit(NdOp::INT_SBOR, SB1, {A, B});
      NdVar SB2 = S.makeTemp(1);
      S.emit(NdOp::INT_SBOR, SB2, {Temp, CfExt});
      S.emit(NdOp::BOOL_XOR, NdVar::reg(x86reg::OF, 1), {SB1, SB2});
    }
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // --- ADC (add with carry): mirror of SBB.
  case X86_INS_ADC: {
    if (X86.op_count < 2)
      break;
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar DstW = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    // Snapshot operands before the result overwrites DstW, so the carry/OF
    // flags read the pre-write values (DstW aliases operand[0]).  Without this
    // a multi-limb adc chain consumes a carry computed from the result.
    NdVar A = S.makeTemp(DstR.Size);
    S.emit(NdOp::COPY, A, {DstR});
    NdVar B = S.makeTemp(Src.Size);
    S.emit(NdOp::COPY, B, {Src});
    NdVar Result = MemDst ? S.makeTemp(A.Size) : DstW;
    NdVar CfExt = S.makeTemp(A.Size);
    S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(x86reg::CF, 1)});
    NdVar CarryInner = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, CarryInner, {B, CfExt});
    NdVar Adj = S.makeTemp(A.Size);
    S.emit(NdOp::INT_ADD, Adj, {B, CfExt});
    S.emit(NdOp::INT_ADD, Result, {A, Adj});
    emitZSPF(S, Result);
    NdVar CarryOuter = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, CarryOuter, {A, Adj});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::CF, 1),
           {CarryInner, CarryOuter});
    NdVar V1 = S.makeTemp(1);
    S.emit(NdOp::INT_SOVF, V1, {B, CfExt});
    NdVar V2 = S.makeTemp(1);
    S.emit(NdOp::INT_SOVF, V2, {A, Adj});
    S.emit(NdOp::BOOL_XOR, NdVar::reg(x86reg::OF, 1), {V1, V2});
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // --- Bit test/set/reset/complement ---
  // The bit offset's meaning depends on the bit-base operand and offset kind:
  //   * register base, or any immediate offset: the offset is taken modulo the
  //     operand width (16/32/64), so `bt eax,33` tests bit 1.
  //   * register offset on a MEMORY base: the offset is a signed bit index into
  //     a bit string — the accessed operand-size chunk is at
  //     EA + ((sext(idx) >> log2(bits)) << log2(bytes)), and the in-chunk bit
  //     is idx & (bits-1).  (Matches the QEMU/x86 reference for `bt mem,reg`.)
  case X86_INS_BT:
  case X86_INS_BTS:
  case X86_INS_BTR:
  case X86_INS_BTC: {
    if (X86.op_count < 2)
      break;
    uint16_t Sz = static_cast<uint16_t>(X86.operands[0].size);
    if (Sz == 0)
      Sz = (TargetArch == Arch::X64) ? 8 : 4;
    uint16_t Bits = Sz * 8;
    uint64_t LogSz = (Sz == 8) ? 3 : (Sz == 4) ? 2 : (Sz == 2) ? 1 : 0;
    uint16_t PtrSz = (TargetArch == Arch::X64) ? 8 : 4;
    bool MemBase = (X86.operands[0].type == X86_OP_MEM);
    bool RegOffset = (X86.operands[1].type == X86_OP_REG);

    NdVar IdxRaw = operandRead(S, X86.operands[1]);

    // Locate the operand-size value containing the target bit, and (for memory)
    // the byte address to load/store.
    NdVar Base;
    NdVar ByteAddr;
    if (MemBase) {
      ByteAddr = S.computeEA(X86.operands[0]);
      if (RegOffset) {
        NdVar IdxExt = S.makeTemp(PtrSz);
        S.emit(NdOp::INT_SEXT, IdxExt, {IdxRaw});
        NdVar ChunkOff = S.makeTemp(PtrSz);
        S.emit(NdOp::INT_ASHR, ChunkOff,
               {IdxExt, NdVar::cst(LogSz + 3, PtrSz)});
        NdVar ByteOff = S.makeTemp(PtrSz);
        S.emit(NdOp::INT_LEFT, ByteOff, {ChunkOff, NdVar::cst(LogSz, PtrSz)});
        NdVar Adj = S.makeTemp(PtrSz);
        S.emit(NdOp::INT_ADD, Adj, {ByteAddr, ByteOff});
        ByteAddr = Adj;
      }
      Base = S.makeTemp(Sz);
      S.emit(NdOp::LOAD, Base, {ByteAddr});
    } else {
      Base = operandRead(S, X86.operands[0]);
    }

    // In-chunk bit position, taken modulo the operand width.
    NdVar Idx = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Idx, {IdxRaw, NdVar::cst(Bits - 1, Sz)});

    // CF = (Base >> Idx) & 1
    NdVar Shifted = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, Shifted, {Base, Idx});
    NdVar Masked = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Masked, {Shifted, NdVar::cst(1, Sz)});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
           {Masked, NdVar::cst(0, Sz)});

    if (InsnId != X86_INS_BT) {
      NdVar Mask = S.makeTemp(Sz);
      S.emit(NdOp::INT_LEFT, Mask, {NdVar::cst(1, Sz), Idx});
      NdVar Result = (MemBase || X86.operands[0].type == X86_OP_MEM)
                           ? S.makeTemp(Sz)
                           : operandWrite(X86.operands[0]);
      if (InsnId == X86_INS_BTS)
        S.emit(NdOp::INT_OR, Result, {Base, Mask});
      else if (InsnId == X86_INS_BTR) {
        NdVar Inv = S.makeTemp(Sz);
        S.emit(NdOp::INT_NOT, Inv, {Mask});
        S.emit(NdOp::INT_AND, Result, {Base, Inv});
      } else { // BTC
        S.emit(NdOp::INT_XOR, Result, {Base, Mask});
      }
      if (MemBase)
        S.emit(NdOp::STORE, {}, {ByteAddr, Result});
    }
    break;
  }

  // --- BSWAP (byte swap): emit as a chain of shifts+ORs. For 32/64-bit
  //     register operand, swap all bytes. Implementation here is a
  //     pragmatic shift-sequence; downstream NdOp → LLVM IR doesn't have
  //     a native bswap opcode anyway, so a sequence is fine.
  case X86_INS_BSWAP: {
    if (X86.op_count < 1)
      break;
    NdVar Src = operandRead(S, X86.operands[0]);
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Swapped = S.emitByteSwap(Src);
    S.emit(NdOp::COPY, Dst, {Swapped});
    break;
  }

  // ========================================================================
  // Double-precision shifts (SHLD / SHRD)
  // ========================================================================
  case X86_INS_SHLD: {
    if (X86.op_count < 3)
      break;
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar DstW = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar CntRaw = operandRead(S, X86.operands[2]);
    uint16_t Sz = DstR.Size;
    uint16_t Bits = Sz * 8;
    // A MEMORY destination must be written with an explicit STORE:
    // operandWrite() of a mem operand yields a discarded ram(0) placeholder, so
    // the prior code dropped the write-back for `shld [mem],reg,cnt` (value
    // computed, flags set, memory left unchanged).  Compute into a temp and
    // store it at the end.
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    NdVar Result = MemDst ? S.makeTemp(Sz) : DstW;
    uint64_t ShldMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar Cnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Cnt, {CntRaw, NdVar::cst(ShldMask, Sz)});
    // Snapshot flags so a zero (post-mask) count restores them: x86 leaves all
    // flags unchanged when SHLD/SHRD shift by 0 (same rule as the single
    // shifts).
    NdVar OldCF = S.makeTemp(1);
    S.emit(NdOp::COPY, OldCF, {NdVar::reg(x86reg::CF, 1)});
    NdVar OldZF = S.makeTemp(1);
    S.emit(NdOp::COPY, OldZF, {NdVar::reg(x86reg::ZF, 1)});
    NdVar OldSF = S.makeTemp(1);
    S.emit(NdOp::COPY, OldSF, {NdVar::reg(x86reg::SF, 1)});
    NdVar OldPF = S.makeTemp(1);
    S.emit(NdOp::COPY, OldPF, {NdVar::reg(x86reg::PF, 1)});
    NdVar CfIdx = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, CfIdx, {NdVar::cst(Bits, Sz), Cnt});
    NdVar CfTmp = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, CfTmp, {DstR, CfIdx});
    NdVar CfBit = S.makeTemp(1);
    S.emit(NdOp::SUBBYTES, CfBit, {CfTmp, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_AND, CfBit, {CfBit, NdVar::cst(1, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {CfBit});
    NdVar Hi = S.makeTemp(Sz);
    S.emit(NdOp::INT_LEFT, Hi, {DstR, Cnt});
    NdVar Rem = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, Rem, {NdVar::cst(Bits, Sz), Cnt});
    NdVar Lo = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, Lo, {Src, Rem});
    S.emit(NdOp::INT_OR, Result, {Hi, Lo});
    emitZSPF(S, Result);
    // OF (1-bit only): MSB(result) ^ CF (the SHL rule, since QEMU folds SHLD's
    // flags through CC_OP_SHL).  emitShiftRotateOF leaves OF unchanged
    // otherwise.
    NdVar OfBit = S.makeTemp(1);
    S.emit(NdOp::BOOL_XOR, OfBit,
           {extractBit(S, Result, Bits - 1), NdVar::reg(x86reg::CF, 1)});
    emitShiftRotateOF(S, Cnt, OfBit);
    emitZeroCountFlagGuard(S, Cnt,
                           {{x86reg::CF, OldCF},
                            {x86reg::ZF, OldZF},
                            {x86reg::SF, OldSF},
                            {x86reg::PF, OldPF}});
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }
  case X86_INS_SHRD: {
    if (X86.op_count < 3)
      break;
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar DstW = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar CntRaw = operandRead(S, X86.operands[2]);
    uint16_t Sz = DstR.Size;
    uint16_t Bits = Sz * 8;
    // Memory destination: store the result explicitly (see SHLD note above).
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    NdVar Result = MemDst ? S.makeTemp(Sz) : DstW;
    uint64_t ShrdMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar Cnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Cnt, {CntRaw, NdVar::cst(ShrdMask, Sz)});
    // Snapshot the original destination MSB (for OF) and the flags (for a zero
    // count) before the result write aliases the destination register.
    NdVar PreMsb = S.makeTemp(1);
    S.emit(NdOp::COPY, PreMsb, {extractBit(S, DstR, Bits - 1)});
    NdVar OldCF = S.makeTemp(1);
    S.emit(NdOp::COPY, OldCF, {NdVar::reg(x86reg::CF, 1)});
    NdVar OldZF = S.makeTemp(1);
    S.emit(NdOp::COPY, OldZF, {NdVar::reg(x86reg::ZF, 1)});
    NdVar OldSF = S.makeTemp(1);
    S.emit(NdOp::COPY, OldSF, {NdVar::reg(x86reg::SF, 1)});
    NdVar OldPF = S.makeTemp(1);
    S.emit(NdOp::COPY, OldPF, {NdVar::reg(x86reg::PF, 1)});
    NdVar CfIdx = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, CfIdx, {Cnt, NdVar::cst(1, Sz)});
    NdVar CfTmp = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, CfTmp, {DstR, CfIdx});
    NdVar CfBit = S.makeTemp(1);
    S.emit(NdOp::SUBBYTES, CfBit, {CfTmp, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_AND, CfBit, {CfBit, NdVar::cst(1, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {CfBit});
    NdVar Lo = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, Lo, {DstR, Cnt});
    NdVar Rem = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, Rem, {NdVar::cst(Bits, Sz), Cnt});
    NdVar Hi = S.makeTemp(Sz);
    S.emit(NdOp::INT_LEFT, Hi, {Src, Rem});
    S.emit(NdOp::INT_OR, Result, {Lo, Hi});
    emitZSPF(S, Result);
    // OF (1-bit only): sign change = MSB(original dst) ^ MSB(result) (QEMU
    // folds SHRD's flags through CC_OP_SAR, whose OF is MSB(shm1) ^
    // MSB(result)).
    NdVar OfBit = S.makeTemp(1);
    S.emit(NdOp::BOOL_XOR, OfBit, {PreMsb, extractBit(S, Result, Bits - 1)});
    emitShiftRotateOF(S, Cnt, OfBit);
    emitZeroCountFlagGuard(S, Cnt,
                           {{x86reg::CF, OldCF},
                            {x86reg::ZF, OldZF},
                            {x86reg::SF, OldSF},
                            {x86reg::PF, OldPF}});
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  case X86_INS_RCR: {
    if (X86.op_count < 2)
      break;
    NdVar SrcR = operandRead(S, X86.operands[0]);
    NdVar DstW = operandWrite(X86.operands[0]);
    NdVar CntRaw = operandRead(S, X86.operands[1]);
    uint16_t Sz = SrcR.Size;
    uint16_t Bits = Sz * 8;
    // Memory destination: store the result explicitly (see SHLD note above).
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    NdVar Result = MemDst ? S.makeTemp(Sz) : DstW;
    uint64_t RcrMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar Cnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Cnt, {CntRaw, NdVar::cst(RcrMask, Sz)});
    // RCR affects only CF and OF; snapshot CF so a zero count preserves it.
    NdVar OldCF = S.makeTemp(1);
    S.emit(NdOp::COPY, OldCF, {NdVar::reg(x86reg::CF, 1)});
    // Rotate-through-carry cycles through Bits+1 positions (operand bits + CF),
    // so BYTE/WORD counts reduce mod 9/17 (Intel SDM), not just the 5-bit mask.
    // 32/64-bit need no step (the masked count is already < Bits+1).
    if (Bits < 32) {
      NdVar Modded = S.makeTemp(Sz);
      S.emit(NdOp::INT_REM, Modded,
             {Cnt, NdVar::cst((uint64_t)Bits + 1, Sz)});
      Cnt = Modded;
    }
    NdVar CfExt = S.makeTemp(Sz);
    S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(x86reg::CF, 1)});
    NdVar CfIdx = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, CfIdx, {Cnt, NdVar::cst(1, Sz)});
    NdVar CfShifted = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, CfShifted, {SrcR, CfIdx});
    NdVar NewCf = S.makeTemp(1);
    S.emit(NdOp::SUBBYTES, NewCf, {CfShifted, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_AND, NewCf, {NewCf, NdVar::cst(1, 1)});
    NdVar Lower = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, Lower, {SrcR, Cnt});
    NdVar CfPos = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, CfPos, {NdVar::cst(Bits, Sz), Cnt});
    NdVar CfIn = S.makeTemp(Sz);
    S.emit(NdOp::INT_LEFT, CfIn, {CfExt, CfPos});
    // WrapAmt = Bits+1-Cnt.  Clamp to avoid UB shift when Cnt==1.
    NdVar WrapAmtRaw = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, WrapAmtRaw, {NdVar::cst(Bits + 1, Sz), Cnt});
    NdVar WrapOk = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, WrapOk, {WrapAmtRaw, NdVar::cst(Bits, Sz)});
    NdVar WrapSafe = S.makeTemp(Sz);
    S.emit(NdOp::INT_LEFT, WrapSafe, {SrcR, WrapAmtRaw});
    NdVar Wrapped = S.makeTemp(Sz);
    S.emit(NdOp::SELECT, Wrapped, {WrapOk, WrapSafe, NdVar::cst(0, Sz)});
    NdVar Tmp1 = S.makeTemp(Sz);
    S.emit(NdOp::INT_OR, Tmp1, {Lower, CfIn});
    S.emit(NdOp::INT_OR, Result, {Tmp1, Wrapped});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NewCf});
    // OF (1-bit only, right rotate): XOR of the two most-significant result
    // bits.
    NdVar RcrOf = S.makeTemp(1);
    S.emit(NdOp::BOOL_XOR, RcrOf,
           {extractBit(S, Result, Bits - 1), extractBit(S, Result, Bits - 2)});
    emitShiftRotateOF(S, Cnt, RcrOf);
    emitZeroCountFlagGuard(S, Cnt, {{x86reg::CF, OldCF}});
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // ========================================================================
  // RCL — rotate left through carry
  // ========================================================================
  case X86_INS_RCL: {
    if (X86.op_count < 2)
      break;
    NdVar SrcR = operandRead(S, X86.operands[0]);
    NdVar DstW = operandWrite(X86.operands[0]);
    NdVar CntRaw = operandRead(S, X86.operands[1]);
    uint16_t Sz = SrcR.Size;
    uint16_t Bits = Sz * 8;
    // Memory destination: store the result explicitly (see SHLD note above).
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    NdVar Result = MemDst ? S.makeTemp(Sz) : DstW;
    uint64_t RclMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar Cnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Cnt, {CntRaw, NdVar::cst(RclMask, Sz)});
    // RCL affects only CF and OF; snapshot CF so a zero count preserves it.
    NdVar OldCF = S.makeTemp(1);
    S.emit(NdOp::COPY, OldCF, {NdVar::reg(x86reg::CF, 1)});
    // Rotate-through-carry cycles through Bits+1 positions (operand bits + CF),
    // so BYTE/WORD counts reduce mod 9/17 (Intel SDM), not just the 5-bit mask.
    // 32/64-bit need no step (the masked count is already < Bits+1).
    if (Bits < 32) {
      NdVar Modded = S.makeTemp(Sz);
      S.emit(NdOp::INT_REM, Modded,
             {Cnt, NdVar::cst((uint64_t)Bits + 1, Sz)});
      Cnt = Modded;
    }
    NdVar CfExt = S.makeTemp(Sz);
    S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(x86reg::CF, 1)});
    NdVar CfBitPos = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, CfBitPos, {NdVar::cst(Bits, Sz), Cnt});
    NdVar CfShifted = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, CfShifted, {SrcR, CfBitPos});
    NdVar NewCf = S.makeTemp(1);
    S.emit(NdOp::SUBBYTES, NewCf, {CfShifted, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_AND, NewCf, {NewCf, NdVar::cst(1, 1)});
    NdVar Upper = S.makeTemp(Sz);
    S.emit(NdOp::INT_LEFT, Upper, {SrcR, Cnt});
    NdVar CfPos = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, CfPos, {Cnt, NdVar::cst(1, Sz)});
    NdVar CfIn = S.makeTemp(Sz);
    S.emit(NdOp::INT_LEFT, CfIn, {CfExt, CfPos});
    // WrapAmt = Bits+1-Cnt.  When Cnt==1, WrapAmt==Bits which is UB for
    // a shift-right of Bits-wide value.  Guard with a saturating clamp.
    NdVar WrapAmtRaw = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, WrapAmtRaw, {NdVar::cst(Bits + 1, Sz), Cnt});
    NdVar WrapOk = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, WrapOk, {WrapAmtRaw, NdVar::cst(Bits, Sz)});
    NdVar WrapSafe = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, WrapSafe, {SrcR, WrapAmtRaw});
    NdVar Wrapped = S.makeTemp(Sz);
    S.emit(NdOp::SELECT, Wrapped, {WrapOk, WrapSafe, NdVar::cst(0, Sz)});
    NdVar Tmp1 = S.makeTemp(Sz);
    S.emit(NdOp::INT_OR, Tmp1, {Upper, CfIn});
    S.emit(NdOp::INT_OR, Result, {Tmp1, Wrapped});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NewCf});
    // OF (1-bit only, left rotate): new CF XOR most-significant result bit.
    NdVar RclOf = S.makeTemp(1);
    S.emit(NdOp::BOOL_XOR, RclOf, {extractBit(S, Result, Bits - 1), NewCf});
    emitShiftRotateOF(S, Cnt, RclOf);
    emitZeroCountFlagGuard(S, Cnt, {{x86reg::CF, OldCF}});
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // ========================================================================
  // Flag manipulation
  // ========================================================================
  case X86_INS_CLC:
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NdVar::cst(0, 1)});
    break;
  case X86_INS_STC:
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NdVar::cst(1, 1)});
    break;
  case X86_INS_CMC: {
    NdVar NC = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, NC, {NdVar::reg(x86reg::CF, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NC});
    break;
  }
  case X86_INS_CLD:
    S.emit(NdOp::COPY, NdVar::reg(x86reg::DF, 1), {NdVar::cst(0, 1)});
    break;
  case X86_INS_STD:
    S.emit(NdOp::COPY, NdVar::reg(x86reg::DF, 1), {NdVar::cst(1, 1)});
    break;
  case X86_INS_SAHF: {
    NdVar Ah = NdVar::reg(x86reg::RAX + 1, 1);
    NdVar BitCf = S.makeTemp(1);
    S.emit(NdOp::INT_AND, BitCf, {Ah, NdVar::cst(0x01, 1)});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
           {BitCf, NdVar::cst(0, 1)});
    NdVar BitPf = S.makeTemp(1);
    S.emit(NdOp::INT_AND, BitPf, {Ah, NdVar::cst(0x04, 1)});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::PF, 1),
           {BitPf, NdVar::cst(0, 1)});
    NdVar BitZf = S.makeTemp(1);
    S.emit(NdOp::INT_AND, BitZf, {Ah, NdVar::cst(0x40, 1)});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::ZF, 1),
           {BitZf, NdVar::cst(0, 1)});
    NdVar BitSf = S.makeTemp(1);
    S.emit(NdOp::INT_AND, BitSf, {Ah, NdVar::cst(0x80, 1)});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::SF, 1),
           {BitSf, NdVar::cst(0, 1)});
    NdVar BitAf = S.makeTemp(1);
    S.emit(NdOp::INT_AND, BitAf, {Ah, NdVar::cst(0x10, 1)});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::AF, 1),
           {BitAf, NdVar::cst(0, 1)});
    break;
  }
  case X86_INS_LAHF: {
    NdVar Ah = NdVar::reg(x86reg::RAX + 1, 1);
    NdVar Sf = S.makeTemp(1);
    S.emit(NdOp::INT_ZEXT, Sf, {NdVar::reg(x86reg::SF, 1)});
    NdVar SfSh = S.makeTemp(1);
    S.emit(NdOp::INT_LEFT, SfSh, {Sf, NdVar::cst(7, 1)});
    NdVar Zf = S.makeTemp(1);
    S.emit(NdOp::INT_ZEXT, Zf, {NdVar::reg(x86reg::ZF, 1)});
    NdVar ZfSh = S.makeTemp(1);
    S.emit(NdOp::INT_LEFT, ZfSh, {Zf, NdVar::cst(6, 1)});
    NdVar Pf = S.makeTemp(1);
    S.emit(NdOp::INT_ZEXT, Pf, {NdVar::reg(x86reg::PF, 1)});
    NdVar PfSh = S.makeTemp(1);
    S.emit(NdOp::INT_LEFT, PfSh, {Pf, NdVar::cst(2, 1)});
    NdVar Af = S.makeTemp(1);
    S.emit(NdOp::INT_ZEXT, Af, {NdVar::reg(x86reg::AF, 1)});
    NdVar AfSh = S.makeTemp(1);
    S.emit(NdOp::INT_LEFT, AfSh, {Af, NdVar::cst(4, 1)});
    NdVar Cf = S.makeTemp(1);
    S.emit(NdOp::INT_ZEXT, Cf, {NdVar::reg(x86reg::CF, 1)});
    NdVar M1 = S.makeTemp(1);
    S.emit(NdOp::INT_OR, M1, {SfSh, ZfSh});
    NdVar M2 = S.makeTemp(1);
    S.emit(NdOp::INT_OR, M2, {M1, PfSh});
    NdVar M2a = S.makeTemp(1);
    S.emit(NdOp::INT_OR, M2a, {M2, AfSh});
    NdVar M3 = S.makeTemp(1);
    S.emit(NdOp::INT_OR, M3, {M2a, NdVar::cst(0x02, 1)});
    S.emit(NdOp::INT_OR, Ah, {M3, Cf});
    break;
  }

  // ========================================================================
  // Sign-extension siblings of CDQ/CQO/CDQE
  // ========================================================================
  case X86_INS_CWD:
    S.emit(NdOp::INT_ASHR, NdVar::reg(x86reg::RDX, 2),
           {NdVar::reg(x86reg::RAX, 2), NdVar::cst(15, 2)});
    break;
  case X86_INS_CWDE:
    S.emit(NdOp::INT_SEXT, NdVar::reg(x86reg::RAX, 4),
           {NdVar::reg(x86reg::RAX, 2)});
    break;
  case X86_INS_CBW:
    S.emit(NdOp::INT_SEXT, NdVar::reg(x86reg::RAX, 2),
           {NdVar::reg(x86reg::RAX, 1)});
    break;

  default:
    return false;
  }
  return true;
}

} // namespace neverd
