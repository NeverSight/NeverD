//===- ARMLiftCoreArith.cpp - ARM32 core arithmetic lifter ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM32 integer add/subtract with and without carry (ADD, SUB, RSB,
/// ADC, SBC) and the flag-only compares CMP, CMN, TST and TEQ.
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

bool liftCoreArith(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                   const cs_arm &ARM) {
  switch (Insn->id) {
  // --- ADD / SUB / RSB ---
  case ARM_INS_ADD:
  case ARM_INS_ADDW: {
    if (ARM.op_count < 3) {
      if (ARM.op_count == 2) {
        NdVar Dst = L.operandWrite(ARM.operands[0]);
        NdVar A = NdVar::reg(Dst.Offset, 4);
        NdVar B = L.operandRead(S, ARM.operands[1]);
        if (ARM.update_flags) {
          A = L.snapForFlags(S, Dst, A);
          B = L.snapForFlags(S, Dst, B);
        }
        S.emit(NdOp::INT_ADD, Dst, {A, B});
        if (ARM.update_flags)
          L.emitNZCV(S, Dst, A, B, false);
        break;
      }
      break;
    }
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    if (ARM.update_flags) {
      A = L.snapForFlags(S, Dst, A);
      B = L.snapForFlags(S, Dst, B);
    }
    S.emit(NdOp::INT_ADD, Dst, {A, B});
    if (ARM.update_flags)
      L.emitNZCV(S, Dst, A, B, false);
    break;
  }
  case ARM_INS_SUB:
  case ARM_INS_SUBS:
  case ARM_INS_SUBW: {
    bool SetFlags = (Insn->id == ARM_INS_SUBS) || ARM.update_flags;
    if (ARM.op_count < 3) {
      if (ARM.op_count == 2) {
        NdVar Dst = L.operandWrite(ARM.operands[0]);
        NdVar B = L.operandRead(S, ARM.operands[1]);
        NdVar AVal = NdVar::reg(Dst.Offset, 4);
        if (SetFlags) {
          AVal = L.snapForFlags(S, Dst, AVal);
          B = L.snapForFlags(S, Dst, B);
        }
        S.emit(NdOp::INT_SUB, Dst, {AVal, B});
        if (SetFlags)
          L.emitNZCV(S, Dst, AVal, B, true);
        break;
      }
      break;
    }
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    if (SetFlags) {
      A = L.snapForFlags(S, Dst, A);
      B = L.snapForFlags(S, Dst, B);
    }
    S.emit(NdOp::INT_SUB, Dst, {A, B});
    if (SetFlags)
      L.emitNZCV(S, Dst, A, B, true);
    break;
  }
  case ARM_INS_RSB: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    if (ARM.update_flags) {
      A = L.snapForFlags(S, Dst, A);
      B = L.snapForFlags(S, Dst, B);
    }
    S.emit(NdOp::INT_SUB, Dst, {B, A});
    if (ARM.update_flags)
      L.emitNZCV(S, Dst, B, A, true);
    break;
  }
  case ARM_INS_ADC: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    if (ARM.update_flags) {
      A = L.snapForFlags(S, Dst, A);
      B = L.snapForFlags(S, Dst, B);
    }
    NdVar Sum = S.makeTemp(4);
    S.emit(NdOp::INT_ADD, Sum, {A, B});
    NdVar CfExt = S.makeTemp(4);
    S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(armreg::CFLAG, 1)});
    S.emit(NdOp::INT_ADD, Dst, {Sum, CfExt});
    if (ARM.update_flags) {
      S.emit(NdOp::INT_SLESS, NdVar::reg(armreg::NFLAG, 1),
             {Dst, NdVar::cst(0, 4)});
      S.emit(NdOp::INT_EQUAL, NdVar::reg(armreg::ZFLAG, 1),
             {Dst, NdVar::cst(0, 4)});
      NdVar C1 = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, C1, {A, B});
      NdVar C2 = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, C2, {Sum, CfExt});
      S.emit(NdOp::BOOL_OR, NdVar::reg(armreg::CFLAG, 1), {C1, C2});
      NdVar V1 = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, V1, {A, B});
      NdVar V2 = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, V2, {Sum, CfExt});
      S.emit(NdOp::BOOL_XOR, NdVar::reg(armreg::VFLAG, 1), {V1, V2});
    }
    break;
  }
  case ARM_INS_SBC: {
    // SBC: Dst = a - b - NOT(C) = a + ~b + C
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    if (ARM.update_flags) {
      A = L.snapForFlags(S, Dst, A);
      B = L.snapForFlags(S, Dst, B);
    }
    NdVar NotB = S.makeTemp(4);
    S.emit(NdOp::INT_NOT, NotB, {B});
    NdVar Sum = S.makeTemp(4);
    S.emit(NdOp::INT_ADD, Sum, {A, NotB});
    NdVar CfExt = S.makeTemp(4);
    S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(armreg::CFLAG, 1)});
    S.emit(NdOp::INT_ADD, Dst, {Sum, CfExt});
    if (ARM.update_flags) {
      S.emit(NdOp::INT_SLESS, NdVar::reg(armreg::NFLAG, 1),
             {Dst, NdVar::cst(0, 4)});
      S.emit(NdOp::INT_EQUAL, NdVar::reg(armreg::ZFLAG, 1),
             {Dst, NdVar::cst(0, 4)});
      NdVar C1 = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, C1, {A, NotB});
      NdVar C2 = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, C2, {Sum, CfExt});
      S.emit(NdOp::BOOL_OR, NdVar::reg(armreg::CFLAG, 1), {C1, C2});
      NdVar V1 = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, V1, {A, NotB});
      NdVar V2 = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, V2, {Sum, CfExt});
      S.emit(NdOp::BOOL_XOR, NdVar::reg(armreg::VFLAG, 1), {V1, V2});
    }
    break;
  }

  // --- CMP / CMN / TST / TEQ ---
  case ARM_INS_CMP: {
    if (ARM.op_count < 2)
      break;
    NdVar A = L.operandRead(S, ARM.operands[0]);
    NdVar B = L.operandRead(S, ARM.operands[1]);
    NdVar TmpR = S.makeTemp(4);
    S.emit(NdOp::INT_SUB, TmpR, {A, B});
    L.emitNZCV(S, TmpR, A, B, true);
    break;
  }
  case ARM_INS_CMN: {
    if (ARM.op_count < 2)
      break;
    NdVar A = L.operandRead(S, ARM.operands[0]);
    NdVar B = L.operandRead(S, ARM.operands[1]);
    NdVar TmpR = S.makeTemp(4);
    S.emit(NdOp::INT_ADD, TmpR, {A, B});
    L.emitNZCV(S, TmpR, A, B, false);
    break;
  }
  case ARM_INS_TST: {
    if (ARM.op_count < 2)
      break;
    NdVar A = L.operandRead(S, ARM.operands[0]);
    NdVar B = L.operandRead(S, ARM.operands[1]);
    NdVar TmpR = S.makeTemp(4);
    S.emit(NdOp::INT_AND, TmpR, {A, B});
    L.emitLogicalOpCarry(S, Insn, ARM.operands[1]);
    L.emitNZ(S, TmpR);
    break;
  }
  case ARM_INS_TEQ: {
    if (ARM.op_count < 2)
      break;
    NdVar A = L.operandRead(S, ARM.operands[0]);
    NdVar B = L.operandRead(S, ARM.operands[1]);
    NdVar TmpR = S.makeTemp(4);
    S.emit(NdOp::INT_XOR, TmpR, {A, B});
    L.emitLogicalOpCarry(S, Insn, ARM.operands[1]);
    L.emitNZ(S, TmpR);
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
