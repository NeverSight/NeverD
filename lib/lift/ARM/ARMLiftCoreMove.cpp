//===- ARMLiftCoreMove.cpp - ARM32 core move lifter ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// HINT and the ARM32 register/immediate move family: MOV, MOVS,
/// MOVW, MOVT and MVN.
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

bool liftCoreMove(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                  const cs_arm &ARM) {
  switch (Insn->id) {
  case ARM_INS_HINT:
    S.emit(NdOp::NOP, {}, {});
    break;

  // --- MOV / MVN / MOVW / MOVT ---
  case ARM_INS_MOV:
  case ARM_INS_MOVS: {
    if (ARM.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    NdVar Dst = L.operandWrite(ARM.operands[0]);

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
            L.emitNZ(S, Dst);
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
        NdVar Shifted = L.operandRead(S, ShOp);
        if (Insn->id == ARM_INS_MOVS || ARM.update_flags) {
          // Set C before the COPY writes the possibly aliased destination.
          NdVar RawSrc = L.operandRead(S, ARM.operands[1]);
          cs_arm_op AmtOp{};
          AmtOp.type = ARM_OP_REG;
          AmtOp.reg = RegId(RsNum);
          NdVar Amt = L.operandRead(S, AmtOp);
          L.emitRegShifterCarry(S, ShType, RawSrc, Amt);
        }
        S.emit(NdOp::COPY, Dst, {Shifted});
        if (Insn->id == ARM_INS_MOVS || ARM.update_flags)
          L.emitNZ(S, Dst);
        break;
      }
    }

    // Rotated modified immediate (`movs r0,#0xFF000000`) sets C to bit 31 of
    // the constant; a plain register/immediate leaves C unchanged.
    if (Insn->id == ARM_INS_MOVS || ARM.update_flags)
      L.emitLogicalOpCarry(S, Insn, ARM.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    if (Insn->id == ARM_INS_MOVS || ARM.update_flags)
      L.emitNZ(S, Dst);
    break;
  }
  case ARM_INS_MOVW: {
    if (ARM.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }
  case ARM_INS_MOVT: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
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
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    if (ARM.update_flags)
      L.emitLogicalOpCarry(S, Insn, ARM.operands[1]);
    S.emit(NdOp::INT_NOT, Dst, {Src});
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
