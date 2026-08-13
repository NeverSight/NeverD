//===- ARMLiftCoreLogic.cpp - ARM32 core bitwise logic lifter ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM32 bitwise data processing: AND, ORR, ORN, EOR and BIC.
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

bool liftCoreLogic(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                   const cs_arm &ARM) {
  switch (Insn->id) {
  // --- AND / ORR / EOR / BIC / ORN ---
  case ARM_INS_AND: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? L.operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    NdVar B = L.operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    if (ARM.update_flags)
      L.emitLogicalOpCarry(S, Insn, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    S.emit(NdOp::INT_AND, Dst, {A, B});
    if (ARM.update_flags)
      L.emitNZ(S, Dst);
    break;
  }
  case ARM_INS_ORR: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? L.operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    NdVar B = L.operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    if (ARM.update_flags)
      L.emitLogicalOpCarry(S, Insn, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    S.emit(NdOp::INT_OR, Dst, {A, B});
    if (ARM.update_flags)
      L.emitNZ(S, Dst);
    break;
  }
  case ARM_INS_ORN: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? L.operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    NdVar B = L.operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    if (ARM.update_flags)
      L.emitLogicalOpCarry(S, Insn, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    NdVar NB = S.makeTemp(4);
    S.emit(NdOp::INT_NOT, NB, {B});
    S.emit(NdOp::INT_OR, Dst, {A, NB});
    if (ARM.update_flags)
      L.emitNZ(S, Dst);
    break;
  }
  case ARM_INS_EOR: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? L.operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    NdVar B = L.operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    if (ARM.update_flags)
      L.emitLogicalOpCarry(S, Insn, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    S.emit(NdOp::INT_XOR, Dst, {A, B});
    if (ARM.update_flags)
      L.emitNZ(S, Dst);
    break;
  }
  case ARM_INS_BIC: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? L.operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    NdVar B = L.operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    if (ARM.update_flags)
      L.emitLogicalOpCarry(S, Insn, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    NdVar NB = S.makeTemp(4);
    S.emit(NdOp::INT_NOT, NB, {B});
    S.emit(NdOp::INT_AND, Dst, {A, NB});
    if (ARM.update_flags)
      L.emitNZ(S, Dst);
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
