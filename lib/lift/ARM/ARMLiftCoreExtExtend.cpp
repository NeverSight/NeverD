//===- ARMLiftCoreExtExtend.cpp - ARM32 extend-and-add lifter ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The extend-with-accumulate family: SXTAB, SXTAH, SXTAB16, SXTB16,
/// UXTAB, UXTAH, UXTAB16 and UXTB16.
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

bool liftCoreExtExtend(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                       const cs_arm &ARM) {
  switch (Insn->id) {
  // SXTAB: Rd = Rn + SignExtend(Byte(Rm)); SXTAH: Rd = Rn +
  // SignExtend(Half(Rm))
  case ARM_INS_SXTAB: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Base = L.operandRead(S, ARM.operands[1]);
    NdVar Src = L.operandRead(S, ARM.operands[2]);
    NdVar ByteV = S.makeTemp(1);
    S.emit(NdOp::SUBBYTES, ByteV, {Src, NdVar::cst(0, 4)});
    NdVar Ext = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, Ext, {ByteV});
    S.emit(NdOp::INT_ADD, Dst, {Base, Ext});
    break;
  }
  case ARM_INS_SXTAH: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Base = L.operandRead(S, ARM.operands[1]);
    NdVar Src = L.operandRead(S, ARM.operands[2]);
    NdVar HalfV = S.makeTemp(2);
    S.emit(NdOp::SUBBYTES, HalfV, {Src, NdVar::cst(0, 4)});
    NdVar Ext = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, Ext, {HalfV});
    S.emit(NdOp::INT_ADD, Dst, {Base, Ext});
    break;
  }
  case ARM_INS_SXTAB16:
  case ARM_INS_SXTB16: {
    // SXTB16: sign-extend Byte[0]→halfword[0], Byte[2]→halfword[1]
    // SXTAB16: Rd = Rn + SXTB16(Rm) (packed halfword add)
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[ARM.op_count - 1]);
    // Extract Byte[0], sign-extend to 16 Bits, pack into low halfword
    NdVar B0 = S.makeTemp(1);
    S.emit(NdOp::SUBBYTES, B0, {Src, NdVar::cst(0, 4)});
    NdVar Ext0 = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, Ext0, {B0});
    NdVar Lo = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Lo, {Ext0, NdVar::cst(0xFFFF, 4)});
    // Extract Byte[2], sign-extend to 16 Bits, pack into high halfword
    NdVar Shifted = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, Shifted, {Src, NdVar::cst(16, 4)});
    NdVar B2 = S.makeTemp(1);
    S.emit(NdOp::SUBBYTES, B2, {Shifted, NdVar::cst(0, 4)});
    NdVar Ext2 = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, Ext2, {B2});
    NdVar Hi = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, Hi, {Ext2, NdVar::cst(16, 4)});
    NdVar Result = S.makeTemp(4);
    S.emit(NdOp::INT_OR, Result, {Lo, Hi});
    if (Insn->id == ARM_INS_SXTAB16 && ARM.op_count >= 3) {
      NdVar Base = L.operandRead(S, ARM.operands[1]);
      // Packed halfword add: each halfword independently
      NdVar BaseLo = S.makeTemp(4);
      S.emit(NdOp::INT_AND, BaseLo, {Base, NdVar::cst(0xFFFF, 4)});
      NdVar BaseHi = S.makeTemp(4);
      S.emit(NdOp::INT_AND, BaseHi, {Base, NdVar::cst(0xFFFF0000u, 4)});
      NdVar SumLo = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, SumLo, {BaseLo, Lo});
      S.emit(NdOp::INT_AND, SumLo, {SumLo, NdVar::cst(0xFFFF, 4)});
      NdVar SumHi = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, SumHi, {BaseHi, Hi});
      S.emit(NdOp::INT_AND, SumHi, {SumHi, NdVar::cst(0xFFFF0000u, 4)});
      S.emit(NdOp::INT_OR, Dst, {SumLo, SumHi});
    } else {
      S.emit(NdOp::COPY, Dst, {Result});
    }
    break;
  }
  // UXTAB: Rd = Rn + ZeroExtend(Byte(Rm)); UXTAH: Rd = Rn +
  // ZeroExtend(Half(Rm))
  case ARM_INS_UXTAB: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Base = L.operandRead(S, ARM.operands[1]);
    NdVar Src = L.operandRead(S, ARM.operands[2]);
    NdVar Masked = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Masked, {Src, NdVar::cst(0xFF, 4)});
    S.emit(NdOp::INT_ADD, Dst, {Base, Masked});
    break;
  }
  case ARM_INS_UXTAH: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Base = L.operandRead(S, ARM.operands[1]);
    NdVar Src = L.operandRead(S, ARM.operands[2]);
    NdVar Masked = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Masked, {Src, NdVar::cst(0xFFFF, 4)});
    S.emit(NdOp::INT_ADD, Dst, {Base, Masked});
    break;
  }
  case ARM_INS_UXTAB16:
  case ARM_INS_UXTB16: {
    // UXTB16: Zero-extend Byte[0]→halfword[0], Byte[2]→halfword[1]
    // UXTAB16: Rd = Rn + UXTB16(Rm) (packed halfword add)
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[ARM.op_count - 1]);
    NdVar Lo = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Lo, {Src, NdVar::cst(0xFF, 4)});
    NdVar HiByte = S.makeTemp(4);
    S.emit(NdOp::INT_AND, HiByte, {Src, NdVar::cst(0x00FF0000u, 4)});
    NdVar Result = S.makeTemp(4);
    S.emit(NdOp::INT_OR, Result, {Lo, HiByte});
    if (Insn->id == ARM_INS_UXTAB16 && ARM.op_count >= 3) {
      NdVar Base = L.operandRead(S, ARM.operands[1]);
      NdVar BaseLo = S.makeTemp(4);
      S.emit(NdOp::INT_AND, BaseLo, {Base, NdVar::cst(0xFFFF, 4)});
      NdVar BaseHi = S.makeTemp(4);
      S.emit(NdOp::INT_AND, BaseHi, {Base, NdVar::cst(0xFFFF0000u, 4)});
      NdVar SumLo = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, SumLo, {BaseLo, Lo});
      S.emit(NdOp::INT_AND, SumLo, {SumLo, NdVar::cst(0xFFFF, 4)});
      NdVar SumHi = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, SumHi, {BaseHi, HiByte});
      S.emit(NdOp::INT_AND, SumHi, {SumHi, NdVar::cst(0xFFFF0000u, 4)});
      S.emit(NdOp::INT_OR, Dst, {SumLo, SumHi});
    } else {
      S.emit(NdOp::COPY, Dst, {Result});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
