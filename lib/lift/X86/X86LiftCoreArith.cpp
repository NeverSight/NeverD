//===- X86LiftCoreArith.cpp - x86/x64 integer arithmetic lifter -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Integer arithmetic and the EFLAGS it defines: ADD, SUB,
/// CMP, TEST, AND/OR/XOR, INC/DEC, NEG, NOT, the
/// carry-propagating ADC/SBB, IMUL/MUL, and the
/// sign-extension pairs (CDQ/CQO/CDQE).
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftCoreArith(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                   const cs_x86 &X86) {
  auto readArithmeticOperand = [&](const cs_x86_op &Operand) {
    NdVar Value = L.operandRead(S, Operand);
    // Encoded arithmetic immediates are scalar bit patterns unless the loader
    // attached provenance to this exact immediate field.  Keeping the exact
    // occurrence override lets `cmp $symbol,%reg` remain relocatable while a
    // loop bound that merely equals a low image VA stays numeric.
    if (Operand.type == X86_OP_IMM && Value.isConst() &&
        Value.Provenance == ConstantAddressProvenance::Unknown)
      Value.Provenance = ConstantAddressProvenance::Scalar;
    return Value;
  };
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // --- ADD ---
  case X86_INS_ADD: {
    if (X86.op_count < 2)
      break;
    NdVar Src = readArithmeticOperand(X86.operands[1]);
    NdVar DstR = readArithmeticOperand(X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
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
    L.emitFlagsArith(S, Result, FlagA, FlagB, false);
    if (X86.operands[0].type == X86_OP_MEM)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // --- SUB ---
  case X86_INS_SUB: {
    if (X86.op_count < 2)
      break;
    NdVar Src = readArithmeticOperand(X86.operands[1]);
    NdVar DstR = readArithmeticOperand(X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar FlagA = S.makeTemp(DstR.Size);
    S.emit(NdOp::COPY, FlagA, {DstR});
    NdVar FlagB = S.makeTemp(Src.Size);
    S.emit(NdOp::COPY, FlagB, {Src});
    NdVar Result =
        (X86.operands[0].type == X86_OP_MEM) ? S.makeTemp(DstR.Size) : DstW;
    S.emit(NdOp::INT_SUB, Result, {DstR, Src});
    L.emitFlagsArith(S, Result, FlagA, FlagB, true);
    if (X86.operands[0].type == X86_OP_MEM)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // --- CMP ---
  case X86_INS_CMP: {
    if (X86.op_count < 2)
      break;
    NdVar A = readArithmeticOperand(X86.operands[0]);
    NdVar B = readArithmeticOperand(X86.operands[1]);
    NdVar TmpR = S.makeTemp(A.Size);
    S.emit(NdOp::INT_SUB, TmpR, {A, B});
    L.emitFlagsArith(S, TmpR, A, B, true);
    break;
  }

  // --- TEST ---
  case X86_INS_TEST: {
    if (X86.op_count < 2)
      break;
    NdVar A = readArithmeticOperand(X86.operands[0]);
    NdVar B = readArithmeticOperand(X86.operands[1]);
    NdVar TmpR = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, TmpR, {A, B});
    L.emitFlagsLogic(S, TmpR);
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
      NdVar DstW = L.operandWrite(X86.operands[0]);
      S.emit(NdOp::COPY, DstW, {NdVar::scalar(0, DstW.Size)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::ZF, 1), {NdVar::scalar(1, 1)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::SF, 1), {NdVar::scalar(0, 1)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::PF, 1), {NdVar::scalar(1, 1)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NdVar::scalar(0, 1)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::scalar(0, 1)});
      break;
    }

    NdVar Src = readArithmeticOperand(X86.operands[1]);
    NdVar DstR = readArithmeticOperand(X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);

    // Idiom: test reg, reg (AND with itself) → just set flags, don't overwrite
    // reg
    if (InsnId == X86_INS_AND && X86.operands[0].type == X86_OP_REG &&
        X86.operands[1].type == X86_OP_REG &&
        X86.operands[0].reg == X86.operands[1].reg) {
      NdVar TmpV = S.makeTemp(DstR.Size);
      S.emit(NdOp::INT_AND, TmpV, {DstR, Src});
      L.emitFlagsLogic(S, TmpV);
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
    L.emitFlagsLogic(S, Result);
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // --- INC / DEC ---
  case X86_INS_INC:
  case X86_INS_DEC: {
    if (X86.op_count < 1)
      break;
    NdVar DstR = readArithmeticOperand(X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar One = NdVar::scalar(1, DstR.Size);
    bool IsInc = (InsnId == X86_INS_INC);
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    // Snapshot the source before INT_ADD/INT_SUB writes DstW: for a register
    // operand DstR and DstW alias the same reg, so a later AF read of DstR
    // would see the post-update value (sub-register aliasing, cf. ADD).
    NdVar PreVal = S.makeTemp(DstR.Size);
    S.emit(NdOp::COPY, PreVal, {DstR});
    NdVar Result = MemDst ? S.makeTemp(DstR.Size) : DstW;
    S.emit(IsInc ? NdOp::INT_ADD : NdOp::INT_SUB, Result, {DstR, One});
    L.emitZSPF(S, Result);
    L.emitAF(S, Result, PreVal, One);
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
    NdVar DstR = readArithmeticOperand(X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    // Snapshot before INT_NEG2 overwrites DstW (register NEG aliases DstR).
    NdVar PreVal = S.makeTemp(DstR.Size);
    S.emit(NdOp::COPY, PreVal, {DstR});
    NdVar Result = MemDst ? S.makeTemp(DstR.Size) : DstW;
    S.emit(NdOp::INT_NEG2, Result, {DstR});
    // CF/OF use the pre-update source (PreVal): a register NEG aliases
    // DstR/DstW so a post-2COMP read of DstR would be the negated value.
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
           {PreVal, NdVar::scalar(0, DstR.Size)});
    L.emitZSPF(S, Result);
    L.emitAF(S, Result, NdVar::scalar(0, DstR.Size), PreVal);
    S.emit(NdOp::INT_SBOR, NdVar::reg(x86reg::OF, 1),
           {NdVar::scalar(0, DstR.Size), PreVal});
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }
  case X86_INS_NOT: {
    if (X86.op_count < 1)
      break;
    NdVar DstR = readArithmeticOperand(X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
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
      NdVar Src = readArithmeticOperand(X86.operands[0]);
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
        S.emit(NdOp::SUBBYTES, Rax, {Full, NdVar::scalar(0, 4)});
        S.emit(NdOp::SUBBYTES, Rdx, {Full, NdVar::scalar(Sz, 4)});
        NdVar LowSext = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_SEXT, LowSext, {Rax});
        S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1), {LowSext, Full});
        S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1),
               {NdVar::reg(x86reg::CF, 1)});
      }
      break;
    }
    NdVar Dst{}, MulA{}, MulB{};
    if (X86.op_count == 2) {
      MulA = readArithmeticOperand(X86.operands[0]);
      MulB = readArithmeticOperand(X86.operands[1]);
      Dst = L.operandWrite(X86.operands[0]);
    } else if (X86.op_count == 3) {
      MulA = readArithmeticOperand(X86.operands[1]);
      MulB = readArithmeticOperand(X86.operands[2]);
      Dst = L.operandWrite(X86.operands[0]);
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
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::reg(x86reg::CF, 1)});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Dst, NdVar::scalar(0, Dst.Size)});
    S.emit(NdOp::INT_SLESS, NdVar::reg(x86reg::SF, 1),
           {Dst, NdVar::scalar(0, Dst.Size)});
    break;
  }

  // --- CDQ/CDQE/CQO ---
  case X86_INS_CDQ:
    S.emit(NdOp::INT_ASHR, NdVar::reg(x86reg::RDX, 4),
           {NdVar::reg(x86reg::RAX, 4), NdVar::scalar(31, 4)});
    break;
  case X86_INS_CQO:
    S.emit(NdOp::INT_ASHR, NdVar::reg(x86reg::RDX, 8),
           {NdVar::reg(x86reg::RAX, 8), NdVar::scalar(63, 8)});
    break;
  case X86_INS_CDQE:
    S.emit(NdOp::INT_SEXT, NdVar::reg(x86reg::RAX, 8),
           {NdVar::reg(x86reg::RAX, 4)});
    break;

  // --- MUL / DIV ---
  case X86_INS_MUL: {
    if (X86.op_count < 1)
      break;
    NdVar Src = readArithmeticOperand(X86.operands[0]);
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
             {Ah, NdVar::scalar(0, 1)});
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
      S.emit(NdOp::SUBBYTES, Rax, {Full, NdVar::scalar(0, 4)});
      S.emit(NdOp::SUBBYTES, Rdx, {Full, NdVar::scalar(Sz, 4)});
      S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
             {Rdx, NdVar::scalar(0, Sz)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1),
             {NdVar::reg(x86reg::CF, 1)});
    }
    break;
  }

  // --- SBB (subtract with borrow): Dst = Dst - Src - CF; CF = combined borrow.
  case X86_INS_SBB: {
    if (X86.op_count < 2)
      break;
    NdVar DstR = readArithmeticOperand(X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar Src = readArithmeticOperand(X86.operands[1]);
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    // Snapshot operands before the result overwrites DstW, so the borrow/OF
    // flags read the pre-write values (DstW aliases operand[0]).  Without this
    // a multi-limb sbb chain consumes a borrow computed from the result.
    NdVar A = S.makeTemp(DstR.Size);
    S.emit(NdOp::COPY, A, {DstR});
    NdVar B = S.makeTemp(Src.Size);
    S.emit(NdOp::COPY, B, {Src});
    NdVar Result = MemDst ? S.makeTemp(A.Size) : DstW;
    NdVar CfExt = snapshotCarryAtWidth(S, A.Size);
    NdVar CarryInner = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, CarryInner, {B, CfExt});
    NdVar Adj = S.makeTemp(A.Size);
    S.emit(NdOp::INT_ADD, Adj, {B, CfExt});
    S.emit(NdOp::INT_SUB, Result, {A, Adj});
    NdVar BorrowOuter = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, BorrowOuter, {A, Adj});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::CF, 1), {CarryInner, BorrowOuter});
    L.emitZSPF(S, Result);
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
    NdVar DstR = readArithmeticOperand(X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar Src = readArithmeticOperand(X86.operands[1]);
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    // Snapshot operands before the result overwrites DstW, so the carry/OF
    // flags read the pre-write values (DstW aliases operand[0]).  Without this
    // a multi-limb adc chain consumes a carry computed from the result.
    NdVar A = S.makeTemp(DstR.Size);
    S.emit(NdOp::COPY, A, {DstR});
    NdVar B = S.makeTemp(Src.Size);
    S.emit(NdOp::COPY, B, {Src});
    NdVar Result = MemDst ? S.makeTemp(A.Size) : DstW;
    NdVar CfExt = snapshotCarryAtWidth(S, A.Size);
    NdVar CarryInner = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, CarryInner, {B, CfExt});
    NdVar Adj = S.makeTemp(A.Size);
    S.emit(NdOp::INT_ADD, Adj, {B, CfExt});
    S.emit(NdOp::INT_ADD, Result, {A, Adj});
    L.emitZSPF(S, Result);
    NdVar CarryOuter = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, CarryOuter, {A, Adj});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::CF, 1), {CarryInner, CarryOuter});
    NdVar V1 = S.makeTemp(1);
    S.emit(NdOp::INT_SOVF, V1, {B, CfExt});
    NdVar V2 = S.makeTemp(1);
    S.emit(NdOp::INT_SOVF, V2, {A, Adj});
    S.emit(NdOp::BOOL_XOR, NdVar::reg(x86reg::OF, 1), {V1, V2});
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
