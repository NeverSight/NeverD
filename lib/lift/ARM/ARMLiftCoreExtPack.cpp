//===- ARMLiftCoreExtPack.cpp - ARM32 halfword pack and byte select lifter ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Halfword packing (PKHBT, PKHTB), the GE-flag byte select SEL and
/// the sum-of-absolute-differences pair USAD8 and USADA8.
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

bool liftCoreExtPack(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                     const cs_arm &ARM) {
  switch (Insn->id) {
  // PKHBT: pack bottom Half from Rn, top Half from Rm (Shifted)
  case ARM_INS_PKHBT: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar Lo = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Lo, {A, NdVar::cst(0xFFFF, 4)});
    NdVar Hi = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Hi, {B, NdVar::cst(0xFFFF0000u, 4)});
    S.emit(NdOp::INT_OR, Dst, {Lo, Hi});
    break;
  }
  // PKHTB: pack top Half from Rn, bottom Half from Rm (Shifted)
  case ARM_INS_PKHTB: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar Hi = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Hi, {A, NdVar::cst(0xFFFF0000u, 4)});
    NdVar Lo = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Lo, {B, NdVar::cst(0xFFFF, 4)});
    S.emit(NdOp::INT_OR, Dst, {Hi, Lo});
    break;
  }
  // SEL — per-byte select on the APSR.GE flags: Rd.byte[i] = GE[i] ? Rn : Rm.
  // The GE flags are produced by a preceding GE-setting parallel add/sub (the
  // plain SADD8/UADD16/SSUB8/SASX/... handled above).  The old code emitted an
  // unhandled `ArmSel` intrinsic which the emitter turned into a bare `sel`
  // inline-asm string (too few operands) that aborted codegen.
  case ARM_INS_SEL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < 4; ++I) {
      NdVar ByteA = S.makeTemp(1), ByteB = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, ByteA, {A, NdVar::cst(I, 4)});
      S.emit(NdOp::SUBBYTES, ByteB, {B, NdVar::cst(I, 4)});
      NdVar Sel = S.makeTemp(1);
      S.emit(NdOp::SELECT, Sel,
             {NdVar::reg(armreg::GEFLAG(I), 1), ByteA, ByteB});
      if (I == 0) {
        Acc = Sel;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + 1);
        S.emit(NdOp::CONCAT, Next, {Sel, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case ARM_INS_USAD8: {
    // USAD8: |a[7:0]-b[7:0]| + |a[15:8]-b[15:8]| + |a[23:16]-b[23:16]| +
    // |a[31:24]-b[31:24]|
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar Acc = S.makeTemp(4);
    S.emit(NdOp::COPY, Acc, {NdVar::cst(0, 4)});
    for (int I = 0; I < 4; I++) {
      NdVar ByteA = S.makeTemp(4);
      NdVar ByteB = S.makeTemp(4);
      S.emit(NdOp::INT_RIGHT, ByteA, {A, NdVar::cst(I * 8, 4)});
      S.emit(NdOp::INT_AND, ByteA, {ByteA, NdVar::cst(0xFF, 4)});
      S.emit(NdOp::INT_RIGHT, ByteB, {B, NdVar::cst(I * 8, 4)});
      S.emit(NdOp::INT_AND, ByteB, {ByteB, NdVar::cst(0xFF, 4)});
      NdVar GE = S.makeTemp(1);
      S.emit(NdOp::INT_LESSEQUAL, GE, {ByteB, ByteA});
      NdVar DiffPos = S.makeTemp(4);
      S.emit(NdOp::INT_SUB, DiffPos, {ByteA, ByteB});
      NdVar DiffNeg = S.makeTemp(4);
      S.emit(NdOp::INT_SUB, DiffNeg, {ByteB, ByteA});
      NdVar AbsDiff = S.makeTemp(4);
      S.emit(NdOp::SELECT, AbsDiff, {GE, DiffPos, DiffNeg});
      S.emit(NdOp::INT_ADD, Acc, {Acc, AbsDiff});
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case ARM_INS_USADA8: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar C = L.operandRead(S, ARM.operands[3]);
    NdVar Acc = S.makeTemp(4);
    S.emit(NdOp::COPY, Acc, {NdVar::cst(0, 4)});
    for (int I = 0; I < 4; I++) {
      NdVar ByteA = S.makeTemp(4);
      NdVar ByteB = S.makeTemp(4);
      S.emit(NdOp::INT_RIGHT, ByteA, {A, NdVar::cst(I * 8, 4)});
      S.emit(NdOp::INT_AND, ByteA, {ByteA, NdVar::cst(0xFF, 4)});
      S.emit(NdOp::INT_RIGHT, ByteB, {B, NdVar::cst(I * 8, 4)});
      S.emit(NdOp::INT_AND, ByteB, {ByteB, NdVar::cst(0xFF, 4)});
      NdVar GE = S.makeTemp(1);
      S.emit(NdOp::INT_LESSEQUAL, GE, {ByteB, ByteA});
      NdVar DiffPos = S.makeTemp(4);
      S.emit(NdOp::INT_SUB, DiffPos, {ByteA, ByteB});
      NdVar DiffNeg = S.makeTemp(4);
      S.emit(NdOp::INT_SUB, DiffNeg, {ByteB, ByteA});
      NdVar AbsDiff = S.makeTemp(4);
      S.emit(NdOp::SELECT, AbsDiff, {GE, DiffPos, DiffNeg});
      S.emit(NdOp::INT_ADD, Acc, {Acc, AbsDiff});
    }
    S.emit(NdOp::INT_ADD, Dst, {Acc, C});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
