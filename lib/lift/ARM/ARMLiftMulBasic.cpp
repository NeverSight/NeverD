//===- ARMLiftMulBasic.cpp - ARM32 word multiply and divide lifter -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The 32-bit multiply/divide core (MUL, MLA, MLS, SDIV, UDIV) and
/// the 64-bit long forms SMULL, UMULL, SMLAL, UMLAL and UMAAL.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool liftMulBasic(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                  const cs_arm &ARM) {
  switch (Insn->id) {
  // --- MUL / MLA / MLS ---
  case ARM_INS_MUL: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? L.operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    NdVar B = L.operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    S.emit(NdOp::INT_MULT, Dst, {A, B});
    if (ARM.update_flags)
      L.emitNZ(S, Dst);
    break;
  }
  case ARM_INS_MLA: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar C = L.operandRead(S, ARM.operands[3]);
    NdVar Prod = S.makeTemp(4);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    S.emit(NdOp::INT_ADD, Dst, {Prod, C});
    break;
  }
  case ARM_INS_MLS: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar C = L.operandRead(S, ARM.operands[3]);
    NdVar Prod = S.makeTemp(4);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    S.emit(NdOp::INT_SUB, Dst, {C, Prod});
    break;
  }

  // --- SDIV / UDIV ---
  case ARM_INS_SDIV: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? L.operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    NdVar B = L.operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    S.emit(NdOp::INT_SDIV, Dst, {A, B});
    break;
  }
  case ARM_INS_UDIV: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? L.operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    NdVar B = L.operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    S.emit(NdOp::INT_DIV, Dst, {A, B});
    break;
  }

  // --- SMULL / UMULL ---
  case ARM_INS_SMULL: {
    if (ARM.op_count < 4)
      break;
    NdVar DstLo = L.operandWrite(ARM.operands[0]);
    NdVar DstHi = L.operandWrite(ARM.operands[1]);
    NdVar A = L.operandRead(S, ARM.operands[2]);
    NdVar B = L.operandRead(S, ARM.operands[3]);
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, AExt, {A});
    S.emit(NdOp::INT_SEXT, BExt, {B});
    NdVar Prod = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod, {AExt, BExt});
    S.emit(NdOp::SUBBYTES, DstLo, {Prod, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, DstHi, {Prod, NdVar::cst(4, 4)});
    break;
  }
  case ARM_INS_UMULL: {
    if (ARM.op_count < 4)
      break;
    NdVar DstLo = L.operandWrite(ARM.operands[0]);
    NdVar DstHi = L.operandWrite(ARM.operands[1]);
    NdVar A = L.operandRead(S, ARM.operands[2]);
    NdVar B = L.operandRead(S, ARM.operands[3]);
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, AExt, {A});
    S.emit(NdOp::INT_ZEXT, BExt, {B});
    NdVar Prod = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod, {AExt, BExt});
    S.emit(NdOp::SUBBYTES, DstLo, {Prod, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, DstHi, {Prod, NdVar::cst(4, 4)});
    break;
  }

  // SMLAL (signed multiply-accumulate long): {RdHi:RdLo} += Rn * Rm
  case ARM_INS_SMLAL: {
    if (ARM.op_count < 4)
      break;
    NdVar DstLo = L.operandWrite(ARM.operands[0]);
    NdVar DstHi = L.operandWrite(ARM.operands[1]);
    NdVar A = L.operandRead(S, ARM.operands[2]);
    NdVar B = L.operandRead(S, ARM.operands[3]);
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, AExt, {A});
    S.emit(NdOp::INT_SEXT, BExt, {B});
    NdVar Prod = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod, {AExt, BExt});
    NdVar OldLoExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, OldLoExt, {NdVar::reg(DstLo.Offset, 4)});
    NdVar OldHiExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, OldHiExt, {NdVar::reg(DstHi.Offset, 4)});
    NdVar OldHiShifted = S.makeTemp(8);
    S.emit(NdOp::INT_LEFT, OldHiShifted, {OldHiExt, NdVar::cst(32, 8)});
    NdVar Accum = S.makeTemp(8);
    S.emit(NdOp::INT_OR, Accum, {OldHiShifted, OldLoExt});
    NdVar Result = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, Result, {Accum, Prod});
    S.emit(NdOp::SUBBYTES, DstLo, {Result, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, DstHi, {Result, NdVar::cst(4, 4)});
    break;
  }
  // UMLAL (unsigned multiply-accumulate long): {RdHi:RdLo} += Rn * Rm
  case ARM_INS_UMLAL: {
    if (ARM.op_count < 4)
      break;
    NdVar DstLo = L.operandWrite(ARM.operands[0]);
    NdVar DstHi = L.operandWrite(ARM.operands[1]);
    NdVar A = L.operandRead(S, ARM.operands[2]);
    NdVar B = L.operandRead(S, ARM.operands[3]);
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, AExt, {A});
    S.emit(NdOp::INT_ZEXT, BExt, {B});
    NdVar Prod = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod, {AExt, BExt});
    NdVar OldLoExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, OldLoExt, {NdVar::reg(DstLo.Offset, 4)});
    NdVar OldHiExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, OldHiExt, {NdVar::reg(DstHi.Offset, 4)});
    NdVar OldHiShifted = S.makeTemp(8);
    S.emit(NdOp::INT_LEFT, OldHiShifted, {OldHiExt, NdVar::cst(32, 8)});
    NdVar Accum = S.makeTemp(8);
    S.emit(NdOp::INT_OR, Accum, {OldHiShifted, OldLoExt});
    NdVar Result = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, Result, {Accum, Prod});
    S.emit(NdOp::SUBBYTES, DstLo, {Result, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, DstHi, {Result, NdVar::cst(4, 4)});
    break;
  }
  // UMAAL: {RdHi:RdLo} = Rn * Rm + RdHi + RdLo (double accumulate)
  case ARM_INS_UMAAL: {
    if (ARM.op_count < 4)
      break;
    NdVar DstLo = L.operandWrite(ARM.operands[0]);
    NdVar DstHi = L.operandWrite(ARM.operands[1]);
    NdVar A = L.operandRead(S, ARM.operands[2]);
    NdVar B = L.operandRead(S, ARM.operands[3]);
    NdVar AExt = S.makeTemp(8);
    NdVar BExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, AExt, {A});
    S.emit(NdOp::INT_ZEXT, BExt, {B});
    NdVar Prod = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod, {AExt, BExt});
    NdVar AddLo = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, AddLo, {NdVar::reg(DstLo.Offset, 4)});
    NdVar AddHi = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, AddHi, {NdVar::reg(DstHi.Offset, 4)});
    NdVar Sum1 = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, Sum1, {Prod, AddLo});
    NdVar Result = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, Result, {Sum1, AddHi});
    S.emit(NdOp::SUBBYTES, DstLo, {Result, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, DstHi, {Result, NdVar::cst(4, 4)});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
