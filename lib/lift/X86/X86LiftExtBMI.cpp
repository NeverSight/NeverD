//===- X86LiftExtBMI.cpp - x86/x64 BMI/BMI2/ADX lifter --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bit-manipulation extensions: TZCNT/LZCNT/POPCNT, the BLS*
/// lowest-set-bit family, ANDN, BEXTR, BZHI, MULX, PDEP/PEXT,
/// RORX, the flag-preserving shifts SARX/SHLX/SHRX, and the
/// ADX carry chains ADCX/ADOX.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftExtBMI(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // Bit counting: TZCNT, LZCNT, POPCNT
  // ========================================================================
  case X86_INS_TZCNT:
  case X86_INS_LZCNT:
  case X86_INS_POPCNT: {
    if (X86.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Dst = L.operandWrite(X86.operands[0]);
    if (InsnId == X86_INS_TZCNT) {
      NdVar NotX = S.makeTemp(Src.Size);
      S.emit(NdOp::INT_NOT, NotX, {Src});
      NdVar XM1 = S.makeTemp(Src.Size);
      S.emit(NdOp::INT_SUB, XM1, {Src, NdVar::scalar(1, Src.Size)});
      NdVar Iso = S.makeTemp(Src.Size);
      S.emit(NdOp::INT_AND, Iso, {NotX, XM1});
      S.emit(NdOp::POPCOUNT, Dst, {Iso});
    } else {
      NdOp Opc = (InsnId == X86_INS_LZCNT) ? NdOp::LZCOUNT : NdOp::POPCOUNT;
      S.emit(Opc, Dst, {Src});
    }
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Dst, NdVar::scalar(0, Dst.Size)});
    if (InsnId == X86_INS_POPCNT) {
      S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NdVar::scalar(0, 1)});
    } else {
      S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::CF, 1),
             {Src, NdVar::scalar(0, Src.Size)});
    }
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::scalar(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::SF, 1), {NdVar::scalar(0, 1)});
    break;
  }

  // ========================================================================
  // BMI1: BLSI, BLSMSK, BLSR, ANDN, BEXTR
  // ========================================================================
  case X86_INS_BLSI: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Neg = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NEG2, Neg, {Src});
    S.emit(NdOp::INT_AND, Dst, {Neg, Src});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Dst, NdVar::scalar(0, Dst.Size)});
    S.emit(NdOp::INT_SLESS, NdVar::reg(x86reg::SF, 1),
           {Dst, NdVar::scalar(0, Dst.Size)});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
           {Src, NdVar::scalar(0, Dst.Size)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::scalar(0, 1)});
    break;
  }
  case X86_INS_BLSMSK: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Dec = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_SUB, Dec, {Src, NdVar::scalar(1, Dst.Size)});
    S.emit(NdOp::INT_XOR, Dst, {Dec, Src});
    S.emit(NdOp::INT_SLESS, NdVar::reg(x86reg::SF, 1),
           {Dst, NdVar::scalar(0, Dst.Size)});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::CF, 1),
           {Src, NdVar::scalar(0, Dst.Size)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::ZF, 1), {NdVar::scalar(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::scalar(0, 1)});
    break;
  }
  case X86_INS_BLSR: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Dec = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_SUB, Dec, {Src, NdVar::scalar(1, Dst.Size)});
    S.emit(NdOp::INT_AND, Dst, {Dec, Src});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Dst, NdVar::scalar(0, Dst.Size)});
    S.emit(NdOp::INT_SLESS, NdVar::reg(x86reg::SF, 1),
           {Dst, NdVar::scalar(0, Dst.Size)});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::CF, 1),
           {Src, NdVar::scalar(0, Dst.Size)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::scalar(0, 1)});
    break;
  }
  case X86_INS_ANDN: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    NdVar NotA = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NotA, {A});
    S.emit(NdOp::INT_AND, Dst, {NotA, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Dst, NdVar::scalar(0, Dst.Size)});
    S.emit(NdOp::INT_SLESS, NdVar::reg(x86reg::SF, 1),
           {Dst, NdVar::scalar(0, Dst.Size)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NdVar::scalar(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::scalar(0, 1)});
    break;
  }

  case X86_INS_BEXTR: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Ctrl = L.operandRead(S, X86.operands[2]);
    uint16_t Sz = Dst.Size;
    NdVar Start = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Start, {Ctrl, NdVar::scalar(0xFF, Sz)});
    NdVar Shifted = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, Shifted, {Src, Start});
    NdVar Len = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, Len, {Ctrl, NdVar::scalar(8, Sz)});
    S.emit(NdOp::INT_AND, Len, {Len, NdVar::scalar(0xFF, Sz)});
    NdVar One = NdVar::scalar(1, Sz);
    NdVar Mask = S.makeTemp(Sz);
    S.emit(NdOp::INT_LEFT, Mask, {One, Len});
    S.emit(NdOp::INT_SUB, Mask, {Mask, One});
    S.emit(NdOp::INT_AND, Dst, {Shifted, Mask});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Dst, NdVar::scalar(0, Sz)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NdVar::scalar(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::scalar(0, 1)});
    break;
  }

  // ========================================================================
  // BMI2: BZHI, MULX, PDEP, PEXT, RORX, SARX, SHLX, SHRX
  // ========================================================================
  case X86_INS_BZHI: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Idx = L.operandRead(S, X86.operands[2]);
    uint16_t Sz = Dst.Size;
    NdVar IdxLow = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, IdxLow, {Idx, NdVar::scalar(0xFF, Sz)});
    NdVar One = NdVar::scalar(1, Sz);
    NdVar Mask = S.makeTemp(Sz);
    S.emit(NdOp::INT_LEFT, Mask, {One, IdxLow});
    S.emit(NdOp::INT_SUB, Mask, {Mask, One});
    S.emit(NdOp::INT_AND, Dst, {Src, Mask});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Dst, NdVar::scalar(0, Sz)});
    S.emit(NdOp::INT_SLESS, NdVar::reg(x86reg::SF, 1),
           {Dst, NdVar::scalar(0, Sz)});
    uint64_t BitWidth = Sz * 8;
    NdVar IdxInRange = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, IdxInRange, {IdxLow, NdVar::scalar(BitWidth, Sz)});
    S.emit(NdOp::BOOL_NOT, NdVar::reg(x86reg::CF, 1), {IdxInRange});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::scalar(0, 1)});
    break;
  }

  case X86_INS_MULX: {
    if (X86.op_count < 3)
      break;
    NdVar DstHi = L.operandWrite(X86.operands[0]);
    NdVar DstLo = L.operandWrite(X86.operands[1]);
    NdVar Src = L.operandRead(S, X86.operands[2]);
    uint16_t Sz = Src.Size;
    NdVar Rdx = NdVar::reg(x86reg::RDX, Sz);
    NdVar ExtA = S.makeTemp(Sz * 2);
    NdVar ExtB = S.makeTemp(Sz * 2);
    S.emit(NdOp::INT_ZEXT, ExtA, {Rdx});
    S.emit(NdOp::INT_ZEXT, ExtB, {Src});
    NdVar Full = S.makeTemp(Sz * 2);
    S.emit(NdOp::INT_MULT, Full, {ExtA, ExtB});
    S.emit(NdOp::SUBBYTES, DstLo, {Full, NdVar::scalar(0, 4)});
    S.emit(NdOp::SUBBYTES, DstHi, {Full, NdVar::scalar(Sz, 4)});
    break;
  }

  case X86_INS_PDEP: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Mask = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Pdep, Dst, {Src, Mask});
    break;
  }
  case X86_INS_PEXT: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Mask = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Pext, Dst, {Src, Mask});
    break;
  }

  case X86_INS_RORX: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar CntRaw = L.operandRead(S, X86.operands[2]);
    if (CntRaw.isConst())
      CntRaw.Provenance = ConstantAddressProvenance::Scalar;
    uint16_t Sz = Dst.Size;
    uint16_t Bits = Sz * 8;
    uint64_t RorxMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar Cnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Cnt, {CntRaw, NdVar::scalar(RorxMask, Sz)});
    NdVar Shr = S.makeTemp(Sz);
    NdVar Comp = S.makeTemp(Sz);
    NdVar Shl = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, Shr, {Src, Cnt});
    S.emit(NdOp::INT_SUB, Comp, {NdVar::scalar(Bits, Sz), Cnt});
    S.emit(NdOp::INT_AND, Comp, {Comp, NdVar::scalar(Bits - 1, Sz)});
    S.emit(NdOp::INT_LEFT, Shl, {Src, Comp});
    S.emit(NdOp::INT_OR, Dst, {Shr, Shl});
    break;
  }

  case X86_INS_SARX:
  case X86_INS_SHLX:
  case X86_INS_SHRX: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar CntRaw = L.operandRead(S, X86.operands[2]);
    if (CntRaw.isConst())
      CntRaw.Provenance = ConstantAddressProvenance::Scalar;
    uint16_t Sz = Dst.Size;
    uint16_t Bits = Sz * 8;
    uint64_t VexMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar Cnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Cnt, {CntRaw, NdVar::scalar(VexMask, Sz)});
    NdOp Opc;
    switch (InsnId) {
    case X86_INS_SHLX:
      Opc = NdOp::INT_LEFT;
      break;
    case X86_INS_SHRX:
      Opc = NdOp::INT_RIGHT;
      break;
    default:
      Opc = NdOp::INT_ASHR;
    }
    S.emit(Opc, Dst, {Src, Cnt});
    break;
  }

  // ========================================================================
  // ADX: ADCX / ADOX — multi-precision addition (reads/writes single flag).
  // ========================================================================
  case X86_INS_ADCX: {
    if (X86.op_count < 2)
      break;
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar CfExt = S.makeTemp(DstR.Size);
    S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(x86reg::CF, 1)});
    NdVar C1 = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, C1, {Src, CfExt});
    NdVar Adj = S.makeTemp(DstR.Size);
    S.emit(NdOp::INT_ADD, Adj, {Src, CfExt});
    // Carry-out reads the pre-write destination; compute it before the result
    // write so DstR does not alias-resolve to the post-add value.
    NdVar C2 = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, C2, {DstR, Adj});
    S.emit(NdOp::INT_ADD, DstW, {DstR, Adj});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::CF, 1), {C1, C2});
    break;
  }

  case X86_INS_ADOX: {
    if (X86.op_count < 2)
      break;
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar OfExt = S.makeTemp(DstR.Size);
    S.emit(NdOp::INT_ZEXT, OfExt, {NdVar::reg(x86reg::OF, 1)});
    NdVar C1 = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, C1, {Src, OfExt});
    NdVar Adj = S.makeTemp(DstR.Size);
    S.emit(NdOp::INT_ADD, Adj, {Src, OfExt});
    // Carry-out reads the pre-write destination; compute it before the result
    // write so DstR does not alias-resolve to the post-add value.
    NdVar C2 = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, C2, {DstR, Adj});
    S.emit(NdOp::INT_ADD, DstW, {DstR, Adj});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::OF, 1), {C1, C2});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
