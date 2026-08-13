//===- ARMLiftCoreShift.cpp - ARM32 core shift lifter --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM32 shift and rotate data processing: LSL, LSR, ASR and ROR,
/// including the shifter carry-out for the flag-setting forms.
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

bool liftCoreShift(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                   const cs_arm &ARM) {
  switch (Insn->id) {
  // --- LSL / LSR / ASR / ROR ---
  case ARM_INS_LSL: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? L.operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    int BIdx = (ARM.op_count >= 3) ? 2 : 1;
    NdVar B = L.operandRead(S, ARM.operands[BIdx]);
    // Set C (shifter carry-out) before the shift writes the possibly aliased
    // destination, so the carry reads the pre-shift source and amount.
    if (ARM.update_flags)
      L.emitRegShifterCarry(S, 0, A, B);
    if (ARM.operands[BIdx].type == ARM_OP_REG) {
      // ARM32 register shift uses Rs[7:0] (0-255); amounts >= 32 yield 0 via
      // the saturating INT_LEFT.  Masking to 5 bits (& 31) would wrongly wrap
      // 40→8.
      NdVar Masked = S.makeTemp(4);
      S.emit(NdOp::INT_AND, Masked, {B, NdVar::cst(0xFF, 4)});
      S.emit(NdOp::INT_LEFT, Dst, {A, Masked});
    } else {
      S.emit(NdOp::INT_LEFT, Dst, {A, B});
    }
    if (ARM.update_flags)
      L.emitNZ(S, Dst);
    break;
  }
  case ARM_INS_LSR: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? L.operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    int BIdx = (ARM.op_count >= 3) ? 2 : 1;
    NdVar B = L.operandRead(S, ARM.operands[BIdx]);
    if (ARM.update_flags)
      L.emitRegShifterCarry(S, 1, A, B);
    if (ARM.operands[BIdx].type == ARM_OP_REG) {
      // Rs[7:0] amount; >= 32 yields 0 via the saturating INT_RIGHT (not & 31).
      NdVar Masked = S.makeTemp(4);
      S.emit(NdOp::INT_AND, Masked, {B, NdVar::cst(0xFF, 4)});
      S.emit(NdOp::INT_RIGHT, Dst, {A, Masked});
    } else {
      S.emit(NdOp::INT_RIGHT, Dst, {A, B});
    }
    if (ARM.update_flags)
      L.emitNZ(S, Dst);
    break;
  }
  case ARM_INS_ASR: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? L.operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    int BIdx = (ARM.op_count >= 3) ? 2 : 1;
    NdVar B = L.operandRead(S, ARM.operands[BIdx]);
    if (ARM.update_flags)
      L.emitRegShifterCarry(S, 2, A, B);
    if (ARM.operands[BIdx].type == ARM_OP_REG) {
      // Rs[7:0] amount; >= 32 sign-replicates via the clamping INT_ASHR (the
      // op clamps the amount to width-1), not & 31 which would wrap 40→8.
      NdVar Masked = S.makeTemp(4);
      S.emit(NdOp::INT_AND, Masked, {B, NdVar::cst(0xFF, 4)});
      S.emit(NdOp::INT_ASHR, Dst, {A, Masked});
    } else {
      S.emit(NdOp::INT_ASHR, Dst, {A, B});
    }
    if (ARM.update_flags)
      L.emitNZ(S, Dst);
    break;
  }
  case ARM_INS_ROR: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? L.operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    int BIdx = (ARM.op_count >= 3) ? 2 : 1;
    NdVar B = L.operandRead(S, ARM.operands[BIdx]);
    if (ARM.update_flags)
      L.emitRegShifterCarry(S, 3, A, B);
    NdVar MaskedB = B;
    if (ARM.operands[BIdx].type == ARM_OP_REG) {
      MaskedB = S.makeTemp(4);
      S.emit(NdOp::INT_AND, MaskedB, {B, NdVar::cst(31, 4)});
    }
    NdVar Lo = S.makeTemp(4);
    NdVar Hi = S.makeTemp(4);
    NdVar Comp = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, Lo, {A, MaskedB});
    S.emit(NdOp::INT_SUB, Comp, {NdVar::cst(32, 4), MaskedB});
    S.emit(NdOp::INT_LEFT, Hi, {A, Comp});
    S.emit(NdOp::INT_OR, Dst, {Lo, Hi});
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
