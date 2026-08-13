//===- X86LiftCoreShift.cpp - x86/x64 shift and rotate lifter -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SHL/SAL/SHR/SAR, ROL/ROR, the double-precision shifts
/// SHLD/SHRD, and the rotate-through-carry RCL/RCR.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftCoreShift(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                   const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // --- SHL / SHR / SAR ---
  case X86_INS_SHL:
  case X86_INS_SAL:
  case X86_INS_SHR:
  case X86_INS_SAR: {
    if (X86.op_count < 2)
      break;
    NdVar Cnt = L.operandRead(S, X86.operands[1]);
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);

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
    L.emitZSPF(S, Result);
    // OF (1-bit shifts only): SHL = MSB(result) ^ CF, SHR = MSB(source),
    // SAR = 0.  emitShiftRotateOF leaves OF unchanged for any other count.
    NdVar OfBit = S.makeTemp(1);
    if (InsnId == X86_INS_SHR) {
      S.emit(NdOp::COPY, OfBit, {L.extractBit(S, PreSrc, Bits - 1)});
    } else if (InsnId == X86_INS_SAR) {
      S.emit(NdOp::COPY, OfBit, {NdVar::cst(0, 1)});
    } else {
      S.emit(NdOp::BOOL_XOR, OfBit,
             {L.extractBit(S, Result, Bits - 1), NdVar::reg(x86reg::CF, 1)});
    }
    L.emitShiftRotateOF(S, MaskedCnt, OfBit);
    L.emitZeroCountFlagGuard(S, MaskedCnt,
                             {{x86reg::CF, OldCF},
                              {x86reg::ZF, OldZF},
                              {x86reg::SF, OldSF},
                              {x86reg::PF, OldPF}});
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // --- ROL / ROR ---
  case X86_INS_ROL:
  case X86_INS_ROR: {
    if (X86.op_count < 2)
      break;
    NdVar Cnt = L.operandRead(S, X86.operands[1]);
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
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
             {L.extractBit(S, Result, Bits - 1), L.extractBit(S, Result, 0)});
    else
      S.emit(NdOp::BOOL_XOR, OfBit,
             {L.extractBit(S, Result, Bits - 1),
              L.extractBit(S, Result, Bits - 2)});
    L.emitShiftRotateOF(S, MaskedCnt, OfBit);
    L.emitZeroCountFlagGuard(S, MaskedCnt, {{x86reg::CF, OldCF}});
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // ========================================================================
  // Double-precision shifts (SHLD / SHRD)
  // ========================================================================
  case X86_INS_SHLD: {
    if (X86.op_count < 3)
      break;
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar CntRaw = L.operandRead(S, X86.operands[2]);
    uint16_t Sz = DstR.Size;
    uint16_t Bits = Sz * 8;
    // A MEMORY destination must be written with an explicit STORE:
    // L.operandWrite() of a mem operand yields a discarded ram(0) placeholder,
    // so the prior code dropped the write-back for `shld [mem],reg,cnt` (value
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
    L.emitZSPF(S, Result);
    // OF (1-bit only): MSB(result) ^ CF (the SHL rule, since QEMU folds SHLD's
    // flags through CC_OP_SHL).  emitShiftRotateOF leaves OF unchanged
    // otherwise.
    NdVar OfBit = S.makeTemp(1);
    S.emit(NdOp::BOOL_XOR, OfBit,
           {L.extractBit(S, Result, Bits - 1), NdVar::reg(x86reg::CF, 1)});
    L.emitShiftRotateOF(S, Cnt, OfBit);
    L.emitZeroCountFlagGuard(S, Cnt,
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
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar CntRaw = L.operandRead(S, X86.operands[2]);
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
    S.emit(NdOp::COPY, PreMsb, {L.extractBit(S, DstR, Bits - 1)});
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
    L.emitZSPF(S, Result);
    // OF (1-bit only): sign change = MSB(original dst) ^ MSB(result) (QEMU
    // folds SHRD's flags through CC_OP_SAR, whose OF is MSB(shm1) ^
    // MSB(result)).
    NdVar OfBit = S.makeTemp(1);
    S.emit(NdOp::BOOL_XOR, OfBit, {PreMsb, L.extractBit(S, Result, Bits - 1)});
    L.emitShiftRotateOF(S, Cnt, OfBit);
    L.emitZeroCountFlagGuard(S, Cnt,
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
    NdVar SrcR = L.operandRead(S, X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar CntRaw = L.operandRead(S, X86.operands[1]);
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
      S.emit(NdOp::INT_REM, Modded, {Cnt, NdVar::cst((uint64_t)Bits + 1, Sz)});
      Cnt = Modded;
    }
    NdVar CfExt = snapshotCarryAtWidth(S, Sz);
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
    S.emit(
        NdOp::BOOL_XOR, RcrOf,
        {L.extractBit(S, Result, Bits - 1), L.extractBit(S, Result, Bits - 2)});
    L.emitShiftRotateOF(S, Cnt, RcrOf);
    L.emitZeroCountFlagGuard(S, Cnt, {{x86reg::CF, OldCF}});
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
    NdVar SrcR = L.operandRead(S, X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar CntRaw = L.operandRead(S, X86.operands[1]);
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
      S.emit(NdOp::INT_REM, Modded, {Cnt, NdVar::cst((uint64_t)Bits + 1, Sz)});
      Cnt = Modded;
    }
    NdVar CfExt = snapshotCarryAtWidth(S, Sz);
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
    S.emit(NdOp::BOOL_XOR, RclOf, {L.extractBit(S, Result, Bits - 1), NewCf});
    L.emitShiftRotateOF(S, Cnt, RclOf);
    L.emitZeroCountFlagGuard(S, Cnt, {{x86reg::CF, OldCF}});
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
