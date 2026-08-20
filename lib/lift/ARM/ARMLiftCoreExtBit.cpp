//===- ARMLiftCoreExtBit.cpp - ARM32 extended bit-manipulation lifter ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ADR, the bitfield insert/clear pair BFC and BFI, the byte/bit
/// reversals REV16, REVSH and RBIT, plus RRX and RSC.
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

bool liftCoreExtBit(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                    const cs_arm &ARM) {
  switch (Insn->id) {
  // ========================================================================
  // Additional integer: ADR, BFC, BFI, REV16, REVSH, RBIT, RRX, CLZ variants
  // ========================================================================
  case ARM_INS_ADR: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    if (Src.isConst()) {
      Src.Size = Dst.Size;
      Src.Provenance = ConstantAddressProvenance::Address;
    }
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }
  case ARM_INS_BFC: {
    // BFC Rd, #LSB, #width — clear Bits [LSB+width-1:LSB] in Rd
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    uint32_t LSB = static_cast<uint32_t>(ARM.operands[1].imm);
    uint32_t Width = static_cast<uint32_t>(ARM.operands[2].imm);
    if (Width == 0 || Width > 32 || LSB >= 32 || Width > 32 - LSB)
      break;
    uint32_t FieldMask = Width == 32 ? ~0u : ((1u << Width) - 1) << LSB;
    uint32_t Mask = ~FieldMask;
    S.emit(NdOp::INT_AND, Dst,
           {NdVar::reg(Dst.Offset, 4), NdVar::cst(Mask, 4)});
    break;
  }
  case ARM_INS_BFI: {
    // BFI Rd, Rn, #LSB, #width — insert Width Bits from Rn[width-1:0] into
    // Rd[LSB+width-1:LSB]
    if (ARM.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    uint32_t LSB = static_cast<uint32_t>(ARM.operands[2].imm);
    uint32_t Width = static_cast<uint32_t>(ARM.operands[3].imm);
    if (Width == 0 || Width > 32 || LSB >= 32 || Width > 32 - LSB)
      break;
    uint32_t FieldMask = Width == 32 ? ~0u : ((1u << Width) - 1) << LSB;
    NdVar ShiftedSrc = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, ShiftedSrc, {Src, NdVar::cst(LSB, 4)});
    NdVar MaskedSrc = S.makeTemp(4);
    S.emit(NdOp::INT_AND, MaskedSrc, {ShiftedSrc, NdVar::cst(FieldMask, 4)});
    NdVar ClearedDst = S.makeTemp(4);
    S.emit(NdOp::INT_AND, ClearedDst,
           {NdVar::reg(Dst.Offset, 4), NdVar::cst(~FieldMask, 4)});
    S.emit(NdOp::INT_OR, Dst, {ClearedDst, MaskedSrc});
    break;
  }
  case ARM_INS_REV16: {
    // REV16 — reverse bytes within each 16-bit halfword
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    // Low halfword: swap bytes 0,1
    NdVar LoHi = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, LoHi, {Src, NdVar::cst(8, 4)});
    S.emit(NdOp::INT_AND, LoHi, {LoHi, NdVar::cst(0xFF00, 4)});
    NdVar LoLo = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, LoLo, {Src, NdVar::cst(8, 4)});
    S.emit(NdOp::INT_AND, LoLo, {LoLo, NdVar::cst(0x00FF, 4)});
    NdVar Lo = S.makeTemp(4);
    S.emit(NdOp::INT_OR, Lo, {LoHi, LoLo});
    // High halfword: swap bytes 2,3
    NdVar HiHi = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, HiHi, {Src, NdVar::cst(8, 4)});
    S.emit(NdOp::INT_AND, HiHi, {HiHi, NdVar::cst(0xFF000000u, 4)});
    NdVar HiLo = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, HiLo, {Src, NdVar::cst(8, 4)});
    S.emit(NdOp::INT_AND, HiLo, {HiLo, NdVar::cst(0x00FF0000u, 4)});
    NdVar Hi = S.makeTemp(4);
    S.emit(NdOp::INT_OR, Hi, {HiHi, HiLo});
    S.emit(NdOp::INT_OR, Dst, {Lo, Hi});
    break;
  }
  case ARM_INS_REVSH: {
    // REVSH — reverse bytes in low halfword, sign-extend to 32 Bits
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    NdVar Byte0 = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Byte0, {Src, NdVar::cst(0xFF, 4)});
    NdVar Byte0_shifted = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, Byte0_shifted, {Byte0, NdVar::cst(8, 4)});
    NdVar Byte1 = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, Byte1, {Src, NdVar::cst(8, 4)});
    S.emit(NdOp::INT_AND, Byte1, {Byte1, NdVar::cst(0xFF, 4)});
    NdVar Swapped16 = S.makeTemp(4);
    S.emit(NdOp::INT_OR, Swapped16, {Byte0_shifted, Byte1});
    NdVar Trunc = S.makeTemp(2);
    S.emit(NdOp::SUBBYTES, Trunc, {Swapped16, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_SEXT, Dst, {Trunc});
    break;
  }
  case ARM_INS_RBIT: {
    // RBIT — reverse all 32 Bits (use intrinsic for exact semantics)
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    S.emitIntrinsic(Intrinsic::ArmRbit, Dst, {Src});
    break;
  }
  case ARM_INS_RRX: {
    // RRX — rotate right by 1 through carry: Result = (CF << 31) | (Src >> 1)
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    NdVar CfExt = S.makeTemp(4);
    S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(armreg::CFLAG, 1)});
    NdVar CfShifted = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, CfShifted, {CfExt, NdVar::cst(31, 4)});
    NdVar SrcShifted = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, SrcShifted, {Src, NdVar::cst(1, 4)});
    S.emit(NdOp::INT_OR, Dst, {CfShifted, SrcShifted});
    if (ARM.update_flags) {
      NdVar Bit0Word = S.makeTemp(4);
      S.emit(NdOp::INT_AND, Bit0Word, {Src, NdVar::cst(1, 4)});
      S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(armreg::CFLAG, 1),
             {Bit0Word, NdVar::cst(0, 4)});
    }
    break;
  }
  case ARM_INS_RSC: {
    // RSC — Reverse Subtract with Carry: Dst = NOT(a) + b + C
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    if (ARM.update_flags) {
      // Snapshot sources before the result write so the carry/overflow flags do
      // not alias-resolve to the post-write Dst (rscs rD,rN,rD).
      A = L.snapForFlags(S, Dst, A);
      B = L.snapForFlags(S, Dst, B);
    }
    NdVar NotA = S.makeTemp(4);
    S.emit(NdOp::INT_NOT, NotA, {A});
    NdVar Sum = S.makeTemp(4);
    S.emit(NdOp::INT_ADD, Sum, {B, NotA});
    NdVar CfExt = S.makeTemp(4);
    S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(armreg::CFLAG, 1)});
    S.emit(NdOp::INT_ADD, Dst, {Sum, CfExt});
    if (ARM.update_flags) {
      S.emit(NdOp::INT_SLESS, NdVar::reg(armreg::NFLAG, 1),
             {Dst, NdVar::cst(0, 4)});
      S.emit(NdOp::INT_EQUAL, NdVar::reg(armreg::ZFLAG, 1),
             {Dst, NdVar::cst(0, 4)});
      NdVar C1 = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, C1, {B, NotA});
      NdVar C2 = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, C2, {Sum, CfExt});
      S.emit(NdOp::BOOL_OR, NdVar::reg(armreg::CFLAG, 1), {C1, C2});
      NdVar V1 = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, V1, {B, NotA});
      NdVar V2 = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, V2, {Sum, CfExt});
      S.emit(NdOp::BOOL_XOR, NdVar::reg(armreg::VFLAG, 1), {V1, V2});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
