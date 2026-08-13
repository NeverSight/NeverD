//===- X86LiftSIMDAVXMask.cpp - x86/x64 AVX-512 opmask register lifter ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AVX-512 opmask (k0-k7) register operations: bitwise
/// logic, shifts, tests, unpack, add and move.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftSIMDAVXMask(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                     const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // P1: AVX-512 k-Mask register operations (opmask k0-k7).
  // ========================================================================

  // KAND{B,W,D,Q} — Mask AND
  case X86_INS_KANDB:
  case X86_INS_KANDW:
  case X86_INS_KANDD:
  case X86_INS_KANDQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_AND, Dst, {A, B});
    break;
  }

  // KANDN{B,W,D,Q} — Mask AND-NOT
  case X86_INS_KANDNB:
  case X86_INS_KANDNW:
  case X86_INS_KANDND:
  case X86_INS_KANDNQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    NdVar NotA = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NotA, {A});
    S.emit(NdOp::INT_AND, Dst, {NotA, B});
    break;
  }

  // KOR{B,W,D,Q} — Mask OR
  case X86_INS_KORB:
  case X86_INS_KORW:
  case X86_INS_KORD:
  case X86_INS_KORQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_OR, Dst, {A, B});
    break;
  }

  // KXOR{B,W,D,Q} — Mask XOR
  case X86_INS_KXORB:
  case X86_INS_KXORW:
  case X86_INS_KXORD:
  case X86_INS_KXORQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_XOR, Dst, {A, B});
    break;
  }

  // KXNOR{B,W,D,Q} — Mask XNOR
  case X86_INS_KXNORB:
  case X86_INS_KXNORW:
  case X86_INS_KXNORD:
  case X86_INS_KXNORQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    NdVar Xored = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_XOR, Xored, {A, B});
    S.emit(NdOp::INT_NOT, Dst, {Xored});
    break;
  }

  // KNOT{B,W,D,Q} — Mask NOT
  case X86_INS_KNOTB:
  case X86_INS_KNOTW:
  case X86_INS_KNOTD:
  case X86_INS_KNOTQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::INT_NOT, Dst, {Src});
    break;
  }

  // KMOV{B,W,D,Q} — Mask move
  case X86_INS_KMOVB:
  case X86_INS_KMOVW:
  case X86_INS_KMOVD:
  case X86_INS_KMOVQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // KSHIFTL{B,W,D,Q} — Mask shift left
  case X86_INS_KSHIFTLB:
  case X86_INS_KSHIFTLW:
  case X86_INS_KSHIFTLD:
  case X86_INS_KSHIFTLQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Cnt = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_LEFT, Dst, {Src, Cnt});
    break;
  }

  // KSHIFTR{B,W,D,Q} — Mask shift right
  case X86_INS_KSHIFTRB:
  case X86_INS_KSHIFTRW:
  case X86_INS_KSHIFTRD:
  case X86_INS_KSHIFTRQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Cnt = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_RIGHT, Dst, {Src, Cnt});
    break;
  }

  // KTEST{B,W,D,Q} — Mask test (set ZF/CF from AND of two Mask regs).
  case X86_INS_KTESTB:
  case X86_INS_KTESTW:
  case X86_INS_KTESTD:
  case X86_INS_KTESTQ: {
    if (X86.op_count < 2)
      break;
    NdVar A = L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[1]);
    NdVar Masked = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, Masked, {A, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Masked, NdVar::cst(0, A.Size)});
    NdVar NotA = S.makeTemp(A.Size);
    S.emit(NdOp::INT_NOT, NotA, {A});
    NdVar AndNot = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, AndNot, {NotA, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::CF, 1),
           {AndNot, NdVar::cst(0, A.Size)});
    break;
  }

  // KORTEST{B,W,D,Q} — Mask OR test (ZF = (src1|src2)==0, CF =
  // (src1|src2)==all1s).
  case X86_INS_KORTESTB:
  case X86_INS_KORTESTW:
  case X86_INS_KORTESTD:
  case X86_INS_KORTESTQ: {
    if (X86.op_count < 2)
      break;
    NdVar A = L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[1]);
    NdVar Ored = S.makeTemp(A.Size);
    S.emit(NdOp::INT_OR, Ored, {A, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Ored, NdVar::cst(0, A.Size)});
    // CF = 1 iff all Bits of (src1|src2) are set, i.e. ~(ored) == 0.
    NdVar NotOred = S.makeTemp(A.Size);
    S.emit(NdOp::INT_NOT, NotOred, {Ored});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::CF, 1),
           {NotOred, NdVar::cst(0, A.Size)});
    break;
  }

  // KUNPCK{BW,WD,DQ} — Mask unpack (concatenate low halves).
  case X86_INS_KUNPCKBW:
  case X86_INS_KUNPCKWD:
  case X86_INS_KUNPCKDQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Kunpck, Dst, {A, B});
    break;
  }

  // KADD{B,W,D,Q} — Mask add
  case X86_INS_KADDB:
  case X86_INS_KADDW:
  case X86_INS_KADDD:
  case X86_INS_KADDQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_ADD, Dst, {A, B});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
