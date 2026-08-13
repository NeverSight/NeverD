//===- X86LiftLegacyXOP.cpp - AMD XOP and AVX-512 leftover lifter ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AMD XOP: VPCOM* compares, VPCMOV/VPPERM/VPERMIL2*,
/// horizontal VPHADD*/VPHSUB*, VPMACS* multiply-accumulate,
/// VPROT*/VPSHA*/VPSHL* and VFRCZ*.  Also the AVX-512
/// VPANDN, align/shuffle, VPCMPxSTRx, VTESTPD/PS and
/// VP4DPWSSD handlers that share this dispatcher.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftLegacyXOP(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                   const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // AVX-512 VPANDN — ~Dst & Src (bulk 128/256/512b).
  // ========================================================================
  case X86_INS_VPANDN: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    NdVar Inv = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, Inv, {A});
    S.emit(NdOp::INT_AND, Dst, {Inv, B});
    break;
  }

  // ========================================================================
  // AVX-512 shuffles / blends / aligns — COPY-based dataflow.
  // ========================================================================
  case X86_INS_VALIGND:
  case X86_INS_VALIGNQ:
  case X86_INS_VSHUFF32X4:
  case X86_INS_VSHUFF64X2:
  case X86_INS_VSHUFI32X4:
  case X86_INS_VSHUFI64X2: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // ========================================================================
  // AVX VPCMPESTRI/M, VPCMPISTRI/M → intrinsic (sets RCX + EFLAGS).
  // ========================================================================
  case X86_INS_VPCMPESTRI:
    S.emitIntrinsic(Intrinsic::Pcmpestri);
    break;
  case X86_INS_VPCMPESTRM:
    S.emitIntrinsic(Intrinsic::Pcmpestrm);
    break;
  case X86_INS_VPCMPISTRI:
    S.emitIntrinsic(Intrinsic::Pcmpistri);
    break;
  case X86_INS_VPCMPISTRM:
    S.emitIntrinsic(Intrinsic::Pcmpistrm);
    break;

  // ========================================================================
  // AVX-512 VTESTPD/PS — set ZF/CF from AND/ANDN of packed floats.
  // ========================================================================
  case X86_INS_VTESTPD:
  case X86_INS_VTESTPS: {
    if (X86.op_count < 2)
      break;
    NdVar A = L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[1]);
    NdVar AndR = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, AndR, {A, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {AndR, NdVar::cst(0, AndR.Size)});
    NdVar InvA = S.makeTemp(A.Size);
    S.emit(NdOp::INT_NOT, InvA, {A});
    NdVar AndnR = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, AndnR, {InvA, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::CF, 1),
           {AndnR, NdVar::cst(0, AndnR.Size)});
    break;
  }

  // ========================================================================
  // AVX-512 VP4DPWSSD / VP4DPWSSDS — dot product accumulate.
  // ========================================================================
  case X86_INS_VP4DPWSSD:
  case X86_INS_VP4DPWSSDS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::INT_ADD, Dst, {Dst, Src});
    break;
  }

  // ========================================================================
  // AMD XOP: VPCOM* (integer compare) — Result is all-1s or all-0s Mask.
  // ========================================================================
  case X86_INS_VPCOM:
  case X86_INS_VPCOMB:
  case X86_INS_VPCOMD:
  case X86_INS_VPCOMQ:
  case X86_INS_VPCOMUB:
  case X86_INS_VPCOMUD:
  case X86_INS_VPCOMUQ:
  case X86_INS_VPCOMUW:
  case X86_INS_VPCOMW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_EQUAL, Dst, {A, B});
    break;
  }

  // ========================================================================
  // AMD XOP: VPCMOV — conditional move; VPERMIL2PD/PS — permute.
  // ========================================================================
  case X86_INS_VPCMOV:
  case X86_INS_VPERMIL2PD:
  case X86_INS_VPERMIL2PS:
  case X86_INS_VPPERM: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // ========================================================================
  // AMD XOP: VPHADD* / VPHSUB* — horizontal add/sub variants.
  // ========================================================================
  case X86_INS_VPHADDBD:
  case X86_INS_VPHADDBQ:
  case X86_INS_VPHADDBW:
  case X86_INS_VPHADDDQ:
  case X86_INS_VPHADDUBD:
  case X86_INS_VPHADDUBQ:
  case X86_INS_VPHADDUBW:
  case X86_INS_VPHADDUDQ:
  case X86_INS_VPHADDUWD:
  case X86_INS_VPHADDUWQ:
  case X86_INS_VPHADDWD:
  case X86_INS_VPHADDWQ:
  case X86_INS_VPHSUBBW:
  case X86_INS_VPHSUBDQ:
  case X86_INS_VPHSUBWD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::INT_ADD, Dst, {Dst, Src});
    break;
  }

  // ========================================================================
  // AMD XOP: VPMACS* / VPMADCS* — multiply-accumulate.
  // ========================================================================
  case X86_INS_VPMACSDD:
  case X86_INS_VPMACSDQH:
  case X86_INS_VPMACSDQL:
  case X86_INS_VPMACSSDD:
  case X86_INS_VPMACSSDQH:
  case X86_INS_VPMACSSDQL:
  case X86_INS_VPMACSSWD:
  case X86_INS_VPMACSSWW:
  case X86_INS_VPMACSWD:
  case X86_INS_VPMACSWW:
  case X86_INS_VPMADCSSWD:
  case X86_INS_VPMADCSWD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    S.emit(NdOp::INT_ADD, Dst, {Dst, Prod});
    break;
  }

  // ========================================================================
  // AMD XOP: VPROT* — packed rotate; VPSHA/VPSHL* — packed shift.
  // ========================================================================
  case X86_INS_VPROTB:
  case X86_INS_VPROTD:
  case X86_INS_VPROTQ:
  case X86_INS_VPROTW:
  case X86_INS_VPSHAB:
  case X86_INS_VPSHAD:
  case X86_INS_VPSHAQ:
  case X86_INS_VPSHAW:
  case X86_INS_VPSHLB:
  case X86_INS_VPSHLD:
  case X86_INS_VPSHLQ:
  case X86_INS_VPSHLW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_LEFT, Dst, {A, B});
    break;
  }

  // ========================================================================
  // AMD XOP: VFRCZ* — approximate reciprocal.
  // ========================================================================
  case X86_INS_VFRCZPD:
  case X86_INS_VFRCZPS:
  case X86_INS_VFRCZSD:
  case X86_INS_VFRCZSS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
