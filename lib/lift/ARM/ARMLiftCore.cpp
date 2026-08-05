//===- ARMLiftCore.cpp - ARM32 core instruction lifter ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Core ARM32 integer ALU instruction handlers: MOV, arithmetic, logic,
/// shifts, rotates, extensions, bit manipulation, packed arithmetic,
/// saturating operations, and conditional select.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool ARMLifter::liftCore(LiftState &S, const cs_insn *Insn, const cs_arm &ARM) {
  switch (Insn->id) {
  case ARM_INS_HINT:
    S.emit(NdOp::NOP, {}, {});
    break;

  // --- MOV / MVN / MOVW / MOVT ---
  case ARM_INS_MOV:
  case ARM_INS_MOVS: {
    if (ARM.op_count < 2)
      break;
    NdVar Src = operandRead(S, ARM.operands[1]);
    NdVar Dst = operandWrite(ARM.operands[0]);

    // Capstone 6 may decode `MOV{S} Rd, Rm, <shift> #imm` (disassembled with
    // the LSL/LSR/ASR/ROR alias) as ARM_INS_MOV/MOVS with the shift specifier
    // missing from the operand detail.  Recover it from the raw encoding.
    // This affects both the same-register in-place form (`lsls r0,r0,#n`) and
    // the distinct-register form (`lslhs r1,r2,#n`), the latter being common in
    // predicated -O2 code.  We verify the ARM data-processing MOV-register
    // signature first so a Thumb or non-MOV encoding is never misread.
    if (Insn->size == 4 && (ARM.op_count == 2 || ARM.op_count == 3) &&
        ARM.operands[0].type == ARM_OP_REG &&
        ARM.operands[1].type == ARM_OP_REG &&
        ARM.operands[1].shift.type == ARM_SFT_INVALID) {
      uint32_t Enc;
      std::memcpy(&Enc, Insn->bytes, 4);
      // ARM data-processing register MOV:
      //   bits [27:26] = 00 (data processing)
      //   bit  [25]    = 0  (register operand, not rotated immediate)
      //   bits [24:21] = 1101 (MOV opcode)
      //   bits [11:7]  = shift amount (5-bit immediate)
      //   bits [6:5]   = shift type (00=LSL, 01=LSR, 10=ASR, 11=ROR)
      //   bit  [4]     = 0 for immediate shift
      bool IsArmMovReg = ((Enc >> 26) & 0x3) == 0 && ((Enc >> 25) & 0x1) == 0 &&
                         ((Enc >> 21) & 0xF) == 0xD;
      if (IsArmMovReg && (Enc & 0x10) == 0) {
        unsigned ShAmt = (Enc >> 7) & 0x1F;
        unsigned ShType = (Enc >> 5) & 0x3;
        if (ShAmt > 0 || ShType > 0) {
          NdOp ShOp = NdOp::INT_LEFT;
          if (ShType == 1)
            ShOp = NdOp::INT_RIGHT;
          else if (ShType == 2)
            ShOp = NdOp::INT_ASHR;
          if (ShAmt == 0 && (ShType == 1 || ShType == 2))
            ShAmt = 32;
          bool SetFlags = (Insn->id == ARM_INS_MOVS || ARM.update_flags);
          // Capture the shifter carry-out from the *pristine* Src before the
          // shift writes the (commonly aliased, e.g. `lsrs r4,r4,#1`)
          // destination — otherwise the carry would read the already-shifted
          // value and land one bit off.  RRX additionally needs the *old* C for
          // its rotate, which is still intact here; commit the new C only after
          // the shift.
          NdVar NewCarry;
          bool HasNewCarry = false;
          if (SetFlags) {
            if (ShType == 3 && ShAmt == 0) {
              // RRX: shifter carry-out = Src[0].
              NewCarry = S.makeTemp(1);
              S.emit(NdOp::INT_AND, NewCarry, {Src, NdVar::cst(1, 4)});
              HasNewCarry = true;
            } else if (ShAmt > 0) {
              // LSL -> Src[32-n]; LSR/ASR -> Src[n-1]; ROR -> Src[(n-1) mod
              // 32].
              unsigned CarryBit = (ShType == 0)   ? (32 - ShAmt)
                                  : (ShType == 3) ? ((ShAmt - 1) & 31)
                                                  : (ShAmt - 1);
              NdVar CBit = S.makeTemp(4);
              S.emit(NdOp::INT_RIGHT, CBit, {Src, NdVar::cst(CarryBit, 4)});
              NewCarry = S.makeTemp(1);
              S.emit(NdOp::INT_AND, NewCarry, {CBit, NdVar::cst(1, 4)});
              HasNewCarry = true;
            }
          }
          if (ShType == 3 && ShAmt == 0) {
            // RRX: (carry << 31) | (Src >> 1)
            NdVar CfExt = S.makeTemp(4);
            S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(armreg::CFLAG, 1)});
            NdVar CfShifted = S.makeTemp(4);
            S.emit(NdOp::INT_LEFT, CfShifted, {CfExt, NdVar::cst(31, 4)});
            NdVar SrcShifted = S.makeTemp(4);
            S.emit(NdOp::INT_RIGHT, SrcShifted, {Src, NdVar::cst(1, 4)});
            S.emit(NdOp::INT_OR, Dst, {CfShifted, SrcShifted});
          } else if (ShType == 3 && ShAmt > 0) {
            NdVar Lo = S.makeTemp(4), Hi = S.makeTemp(4);
            S.emit(NdOp::INT_RIGHT, Lo, {Src, NdVar::cst(ShAmt, 4)});
            S.emit(NdOp::INT_LEFT, Hi, {Src, NdVar::cst(32 - ShAmt, 4)});
            S.emit(NdOp::INT_OR, Dst, {Lo, Hi});
          } else {
            S.emit(ShOp, Dst, {Src, NdVar::cst(ShAmt, 4)});
          }
          if (SetFlags) {
            if (HasNewCarry)
              S.emit(NdOp::COPY, NdVar::reg(armreg::CFLAG, 1), {NewCarry});
            emitNZ(S, Dst);
          }
          break;
        }
      }
      // Register-shifted MOV (`lsls r0,r1,r2` = `movs r0,r1,lsl r2`): capstone
      // drops the shift detail like the immediate form, so recover the type and
      // Rs from the raw encoding (bit[4]=1, [7]=0, [6:5]=type, [11:8]=Rs).  The
      // plain COPY fallthrough would otherwise lose both the shift and its
      // carry.
      if (IsArmMovReg && (Enc & 0x10) != 0 && (Enc & 0x80) == 0) {
        unsigned ShType = (Enc >> 5) & 0x3;
        unsigned RsNum = (Enc >> 8) & 0xF;
        static const arm_shifter RegShift[4] = {
            ARM_SFT_LSL_REG, ARM_SFT_LSR_REG, ARM_SFT_ASR_REG, ARM_SFT_ROR_REG};
        auto RegId = [](unsigned N) -> arm_reg {
          switch (N) {
          case 13:
            return ARM_REG_SP;
          case 14:
            return ARM_REG_LR;
          case 15:
            return ARM_REG_PC;
          default:
            return static_cast<arm_reg>(ARM_REG_R0 + N);
          }
        };
        cs_arm_op ShOp = ARM.operands[1];
        ShOp.shift.type = RegShift[ShType];
        ShOp.shift.value = static_cast<unsigned>(RegId(RsNum));
        NdVar Shifted = operandRead(S, ShOp);
        if (Insn->id == ARM_INS_MOVS || ARM.update_flags) {
          // Set C before the COPY writes the possibly aliased destination.
          NdVar RawSrc = operandRead(S, ARM.operands[1]);
          cs_arm_op AmtOp{};
          AmtOp.type = ARM_OP_REG;
          AmtOp.reg = RegId(RsNum);
          NdVar Amt = operandRead(S, AmtOp);
          emitRegShifterCarry(S, ShType, RawSrc, Amt);
        }
        S.emit(NdOp::COPY, Dst, {Shifted});
        if (Insn->id == ARM_INS_MOVS || ARM.update_flags)
          emitNZ(S, Dst);
        break;
      }
    }

    // Rotated modified immediate (`movs r0,#0xFF000000`) sets C to bit 31 of
    // the constant; a plain register/immediate leaves C unchanged.
    if (Insn->id == ARM_INS_MOVS || ARM.update_flags)
      emitLogicalOpCarry(S, Insn, ARM.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    if (Insn->id == ARM_INS_MOVS || ARM.update_flags)
      emitNZ(S, Dst);
    break;
  }
  case ARM_INS_MOVW: {
    if (ARM.op_count < 2)
      break;
    NdVar Src = operandRead(S, ARM.operands[1]);
    NdVar Dst = operandWrite(ARM.operands[0]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }
  case ARM_INS_MOVT: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    uint64_t Imm16 =
        static_cast<uint64_t>(static_cast<uint32_t>(ARM.operands[1].imm)) &
        0xFFFF;
    // MOVT writes the upper 16 Bits: Dst = (Dst & 0xFFFF) | (Imm16 << 16)
    NdVar Lo = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Lo,
           {NdVar::reg(Dst.Offset, 4), NdVar::cst(0xFFFF, 4)});
    NdVar Hi = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, Hi, {NdVar::cst(Imm16, 4), NdVar::cst(16, 4)});
    S.emit(NdOp::INT_OR, Dst, {Lo, Hi});
    break;
  }
  case ARM_INS_MVN: {
    if (ARM.op_count < 2)
      break;
    NdVar Src = operandRead(S, ARM.operands[1]);
    NdVar Dst = operandWrite(ARM.operands[0]);
    if (ARM.update_flags)
      emitLogicalOpCarry(S, Insn, ARM.operands[1]);
    S.emit(NdOp::INT_NOT, Dst, {Src});
    if (ARM.update_flags)
      emitNZ(S, Dst);
    break;
  }

  // --- ADD / SUB / RSB ---
  case ARM_INS_ADD:
  case ARM_INS_ADDW: {
    if (ARM.op_count < 3) {
      if (ARM.op_count == 2) {
        NdVar Dst = operandWrite(ARM.operands[0]);
        NdVar A = NdVar::reg(Dst.Offset, 4);
        NdVar B = operandRead(S, ARM.operands[1]);
        if (ARM.update_flags) {
          A = snapForFlags(S, Dst, A);
          B = snapForFlags(S, Dst, B);
        }
        S.emit(NdOp::INT_ADD, Dst, {A, B});
        if (ARM.update_flags)
          emitNZCV(S, Dst, A, B, false);
        break;
      }
      break;
    }
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    if (ARM.update_flags) {
      A = snapForFlags(S, Dst, A);
      B = snapForFlags(S, Dst, B);
    }
    S.emit(NdOp::INT_ADD, Dst, {A, B});
    if (ARM.update_flags)
      emitNZCV(S, Dst, A, B, false);
    break;
  }
  case ARM_INS_SUB:
  case ARM_INS_SUBS:
  case ARM_INS_SUBW: {
    bool SetFlags = (Insn->id == ARM_INS_SUBS) || ARM.update_flags;
    if (ARM.op_count < 3) {
      if (ARM.op_count == 2) {
        NdVar Dst = operandWrite(ARM.operands[0]);
        NdVar B = operandRead(S, ARM.operands[1]);
        NdVar AVal = NdVar::reg(Dst.Offset, 4);
        if (SetFlags) {
          AVal = snapForFlags(S, Dst, AVal);
          B = snapForFlags(S, Dst, B);
        }
        S.emit(NdOp::INT_SUB, Dst, {AVal, B});
        if (SetFlags)
          emitNZCV(S, Dst, AVal, B, true);
        break;
      }
      break;
    }
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    if (SetFlags) {
      A = snapForFlags(S, Dst, A);
      B = snapForFlags(S, Dst, B);
    }
    S.emit(NdOp::INT_SUB, Dst, {A, B});
    if (SetFlags)
      emitNZCV(S, Dst, A, B, true);
    break;
  }
  case ARM_INS_RSB: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    if (ARM.update_flags) {
      A = snapForFlags(S, Dst, A);
      B = snapForFlags(S, Dst, B);
    }
    S.emit(NdOp::INT_SUB, Dst, {B, A});
    if (ARM.update_flags)
      emitNZCV(S, Dst, B, A, true);
    break;
  }
  case ARM_INS_ADC: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    if (ARM.update_flags) {
      A = snapForFlags(S, Dst, A);
      B = snapForFlags(S, Dst, B);
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
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    if (ARM.update_flags) {
      A = snapForFlags(S, Dst, A);
      B = snapForFlags(S, Dst, B);
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
    NdVar A = operandRead(S, ARM.operands[0]);
    NdVar B = operandRead(S, ARM.operands[1]);
    NdVar TmpR = S.makeTemp(4);
    S.emit(NdOp::INT_SUB, TmpR, {A, B});
    emitNZCV(S, TmpR, A, B, true);
    break;
  }
  case ARM_INS_CMN: {
    if (ARM.op_count < 2)
      break;
    NdVar A = operandRead(S, ARM.operands[0]);
    NdVar B = operandRead(S, ARM.operands[1]);
    NdVar TmpR = S.makeTemp(4);
    S.emit(NdOp::INT_ADD, TmpR, {A, B});
    emitNZCV(S, TmpR, A, B, false);
    break;
  }
  case ARM_INS_TST: {
    if (ARM.op_count < 2)
      break;
    NdVar A = operandRead(S, ARM.operands[0]);
    NdVar B = operandRead(S, ARM.operands[1]);
    NdVar TmpR = S.makeTemp(4);
    S.emit(NdOp::INT_AND, TmpR, {A, B});
    emitLogicalOpCarry(S, Insn, ARM.operands[1]);
    emitNZ(S, TmpR);
    break;
  }
  case ARM_INS_TEQ: {
    if (ARM.op_count < 2)
      break;
    NdVar A = operandRead(S, ARM.operands[0]);
    NdVar B = operandRead(S, ARM.operands[1]);
    NdVar TmpR = S.makeTemp(4);
    S.emit(NdOp::INT_XOR, TmpR, {A, B});
    emitLogicalOpCarry(S, Insn, ARM.operands[1]);
    emitNZ(S, TmpR);
    break;
  }

  // --- AND / ORR / EOR / BIC / ORN ---
  case ARM_INS_AND: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    NdVar B = operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    if (ARM.update_flags)
      emitLogicalOpCarry(S, Insn, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    S.emit(NdOp::INT_AND, Dst, {A, B});
    if (ARM.update_flags)
      emitNZ(S, Dst);
    break;
  }
  case ARM_INS_ORR: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    NdVar B = operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    if (ARM.update_flags)
      emitLogicalOpCarry(S, Insn, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    S.emit(NdOp::INT_OR, Dst, {A, B});
    if (ARM.update_flags)
      emitNZ(S, Dst);
    break;
  }
  case ARM_INS_ORN: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    NdVar B = operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    if (ARM.update_flags)
      emitLogicalOpCarry(S, Insn, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    NdVar NB = S.makeTemp(4);
    S.emit(NdOp::INT_NOT, NB, {B});
    S.emit(NdOp::INT_OR, Dst, {A, NB});
    if (ARM.update_flags)
      emitNZ(S, Dst);
    break;
  }
  case ARM_INS_EOR: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    NdVar B = operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    if (ARM.update_flags)
      emitLogicalOpCarry(S, Insn, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    S.emit(NdOp::INT_XOR, Dst, {A, B});
    if (ARM.update_flags)
      emitNZ(S, Dst);
    break;
  }
  case ARM_INS_BIC: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    NdVar B = operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    if (ARM.update_flags)
      emitLogicalOpCarry(S, Insn, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
    NdVar NB = S.makeTemp(4);
    S.emit(NdOp::INT_NOT, NB, {B});
    S.emit(NdOp::INT_AND, Dst, {A, NB});
    if (ARM.update_flags)
      emitNZ(S, Dst);
    break;
  }

  // --- LSL / LSR / ASR / ROR ---
  case ARM_INS_LSL: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    int BIdx = (ARM.op_count >= 3) ? 2 : 1;
    NdVar B = operandRead(S, ARM.operands[BIdx]);
    // Set C (shifter carry-out) before the shift writes the possibly aliased
    // destination, so the carry reads the pre-shift source and amount.
    if (ARM.update_flags)
      emitRegShifterCarry(S, 0, A, B);
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
      emitNZ(S, Dst);
    break;
  }
  case ARM_INS_LSR: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    int BIdx = (ARM.op_count >= 3) ? 2 : 1;
    NdVar B = operandRead(S, ARM.operands[BIdx]);
    if (ARM.update_flags)
      emitRegShifterCarry(S, 1, A, B);
    if (ARM.operands[BIdx].type == ARM_OP_REG) {
      // Rs[7:0] amount; >= 32 yields 0 via the saturating INT_RIGHT (not & 31).
      NdVar Masked = S.makeTemp(4);
      S.emit(NdOp::INT_AND, Masked, {B, NdVar::cst(0xFF, 4)});
      S.emit(NdOp::INT_RIGHT, Dst, {A, Masked});
    } else {
      S.emit(NdOp::INT_RIGHT, Dst, {A, B});
    }
    if (ARM.update_flags)
      emitNZ(S, Dst);
    break;
  }
  case ARM_INS_ASR: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    int BIdx = (ARM.op_count >= 3) ? 2 : 1;
    NdVar B = operandRead(S, ARM.operands[BIdx]);
    if (ARM.update_flags)
      emitRegShifterCarry(S, 2, A, B);
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
      emitNZ(S, Dst);
    break;
  }
  case ARM_INS_ROR: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = (ARM.op_count >= 3) ? operandRead(S, ARM.operands[1])
                                    : NdVar::reg(Dst.Offset, 4);
    int BIdx = (ARM.op_count >= 3) ? 2 : 1;
    NdVar B = operandRead(S, ARM.operands[BIdx]);
    if (ARM.update_flags)
      emitRegShifterCarry(S, 3, A, B);
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
      emitNZ(S, Dst);
    break;
  }

  // --- Extension instructions ---
  case ARM_INS_SXTB: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    NdVar Byte = S.makeTemp(1);
    S.emit(NdOp::SUBBYTES, Byte, {Src, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_SEXT, Dst, {Byte});
    break;
  }
  case ARM_INS_SXTH: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    NdVar Half = S.makeTemp(2);
    S.emit(NdOp::SUBBYTES, Half, {Src, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_SEXT, Dst, {Half});
    break;
  }
  case ARM_INS_UXTB: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    S.emit(NdOp::INT_AND, Dst, {Src, NdVar::cst(0xFF, 4)});
    break;
  }
  case ARM_INS_UXTH: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    S.emit(NdOp::INT_AND, Dst, {Src, NdVar::cst(0xFFFF, 4)});
    break;
  }

  // --- Bit field ---
  case ARM_INS_UBFX: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
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
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
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
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
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
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
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
