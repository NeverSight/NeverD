//===- ARMLiftCoreBit.cpp - ARM32 core bit-manipulation lifter -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM32 sign/zero extension (SXTB, SXTH, UXTB, UXTH), bitfield
/// extraction (UBFX, SBFX), CLZ, IT and REV.
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

bool liftCoreBit(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                 const cs_arm &ARM) {
  switch (Insn->id) {
  // --- Extension instructions ---
  case ARM_INS_SXTB: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    NdVar Byte = S.makeTemp(1);
    S.emit(NdOp::SUBBYTES, Byte, {Src, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_SEXT, Dst, {Byte});
    break;
  }
  case ARM_INS_SXTH: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    NdVar Half = S.makeTemp(2);
    S.emit(NdOp::SUBBYTES, Half, {Src, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_SEXT, Dst, {Half});
    break;
  }
  case ARM_INS_UXTB: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    S.emit(NdOp::INT_AND, Dst, {Src, NdVar::cst(0xFF, 4)});
    break;
  }
  case ARM_INS_UXTH: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    S.emit(NdOp::INT_AND, Dst, {Src, NdVar::cst(0xFFFF, 4)});
    break;
  }

  // --- Bit field ---
  case ARM_INS_UBFX: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    uint32_t LSB = static_cast<uint32_t>(ARM.operands[2].imm);
    uint32_t Width = static_cast<uint32_t>(ARM.operands[3].imm);
    if (Width == 0 || Width > 32 || LSB >= 32 || Width > 32 - LSB)
      break;
    NdVar Shifted = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, Shifted, {Src, NdVar::cst(LSB, 4)});
    uint32_t Mask = Width == 32 ? ~0u : (1u << Width) - 1;
    S.emit(NdOp::INT_AND, Dst, {Shifted, NdVar::cst(Mask, 4)});
    break;
  }
  case ARM_INS_SBFX: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    uint32_t LSB = static_cast<uint32_t>(ARM.operands[2].imm);
    uint32_t Width = static_cast<uint32_t>(ARM.operands[3].imm);
    if (Width == 0 || Width > 32 || LSB >= 32 || Width > 32 - LSB)
      break;
    NdVar Shifted = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, Shifted, {Src, NdVar::cst(LSB, 4)});
    uint32_t ShiftLeft = 32 - Width;
    NdVar Sl = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, Sl, {Shifted, NdVar::cst(ShiftLeft, 4)});
    S.emit(NdOp::INT_ASHR, Dst, {Sl, NdVar::cst(ShiftLeft, 4)});
    break;
  }

  // --- CLZ ---
  case ARM_INS_CLZ: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    S.emit(NdOp::LZCOUNT, Dst, {Src});
    break;
  }

  // NEG (Thumb alias) → RSB handled above

  // --- IT (If-Then) block ---
  case ARM_INS_IT:
    S.emit(NdOp::NOP, {}, {});
    break;

  // --- REV / REV16 / RBIT ---
  case ARM_INS_REV: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    // Byte swap: ((Src>>24)&0xFF) | ((Src>>8)&0xFF00) | ((Src<<8)&0xFF0000) |
    // ((Src<<24)&0xFF000000)
    NdVar B0 = S.makeTemp(4);
    NdVar B1 = S.makeTemp(4);
    NdVar B2 = S.makeTemp(4);
    NdVar B3 = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, B0, {Src, NdVar::cst(24, 4)});
    S.emit(NdOp::INT_AND, B0, {B0, NdVar::cst(0xFF, 4)});
    NdVar Shr8 = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, Shr8, {Src, NdVar::cst(8, 4)});
    S.emit(NdOp::INT_AND, B1, {Shr8, NdVar::cst(0xFF00, 4)});
    NdVar Shl8 = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, Shl8, {Src, NdVar::cst(8, 4)});
    S.emit(NdOp::INT_AND, B2, {Shl8, NdVar::cst(0xFF0000, 4)});
    S.emit(NdOp::INT_LEFT, B3, {Src, NdVar::cst(24, 4)});
    NdVar T1 = S.makeTemp(4);
    S.emit(NdOp::INT_OR, T1, {B0, B1});
    NdVar T2 = S.makeTemp(4);
    S.emit(NdOp::INT_OR, T2, {B2, B3});
    S.emit(NdOp::INT_OR, Dst, {T1, T2});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
