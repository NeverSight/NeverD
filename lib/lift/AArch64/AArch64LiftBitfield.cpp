//===- AArch64LiftBitfield.cpp - Bitfield move (UBFM/SBFM) ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The unsigned and signed bitfield moves UBFM/SBFM, covering
/// the UBFX/SBFX/UBFIZ/SBFIZ/LSL/LSR/ASR/SXTB/SXTH/SXTW aliases
/// that Capstone resolves to them.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftBitfield(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // --- UBFX / SBFX / UBFM / SBFM ---
  case AARCH64_INS_UBFM: {
    // Capstone 6 alias: LSL/LSR/UXTB/UXTH → id=UBFM, op_count=2
    if (Insn->is_alias && ARM64.op_count < 4 && ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      uint16_t Sz = Dst.Size;
      auto ShT = ARM64.operands[1].shift.type;
      uint32_t ShV = ARM64.operands[1].shift.value;
      bool HasShift = (ShT != AARCH64_SFT_INVALID && ShV != 0);
      // Read raw register value (without shift) to avoid double-shift
      NdVar Src;
      if (HasShift && ARM64.operands[1].type == AARCH64_OP_REG) {
        auto RI =
            mapCapstoneReg(static_cast<aarch64_reg>(ARM64.operands[1].reg));
        Src = (RI.Size == 0)               ? NdVar::cst(0, 8)
              : (RI.Offset == a64reg::XZR) ? NdVar::cst(0, RI.Size)
                                           : NdVar::reg(RI.Offset, RI.Size);
      } else {
        Src = L.operandRead(S, ARM64.operands[1]);
      }
      if (ShT == AARCH64_SFT_LSL && ShV > 0) {
        S.emit(NdOp::INT_LEFT, Dst, {Src, NdVar::cst(ShV, Sz)});
      } else if (ShT == AARCH64_SFT_LSR && ShV > 0) {
        S.emit(NdOp::INT_RIGHT, Dst, {Src, NdVar::cst(ShV, Sz)});
      } else {
        uint16_t ExtSz = Src.Size;
        if (Insn->alias_id == AARCH64_INS_ALIAS_UXTB)
          ExtSz = 1;
        else if (Insn->alias_id == AARCH64_INS_ALIAS_UXTH)
          ExtSz = 2;

        if (ExtSz < Src.Size) {
          NdVar Narrow = S.makeTemp(ExtSz);
          S.emit(NdOp::SUBBYTES, Narrow, {Src, NdVar::cst(0, 4)});
          S.emit(NdOp::INT_ZEXT, Dst, {Narrow});
        } else if (Src.Size > Dst.Size) {
          S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
        } else if (Src.Size < Dst.Size) {
          S.emit(NdOp::INT_ZEXT, Dst, {Src});
        } else {
          S.emit(NdOp::COPY, Dst, {Src});
        }
      }
      break;
    }
    if (ARM64.op_count < 4)
      break;
    {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[1]);
      uint16_t Sz = Dst.Size;
      uint32_t Bits = Sz * 8;

      if (Insn->is_alias) {
        // Alias form: operands are (Rd, Rn, #lsb, #Width) for UBFIZ/UBFX
        uint64_t LSB = static_cast<uint64_t>(ARM64.operands[2].imm);
        uint64_t Width = static_cast<uint64_t>(ARM64.operands[3].imm);
        const char *Mn = Insn->mnemonic;
        if (Mn[0] == 'u' && Mn[1] == 'b' && Mn[2] == 'f' && Mn[3] == 'i') {
          uint64_t Mask = (Width >= 64) ? ~0ULL : ((1ULL << Width) - 1);
          NdVar Masked = S.makeTemp(Sz);
          S.emit(NdOp::INT_AND, Masked, {Src, NdVar::cst(Mask, Sz)});
          S.emit(NdOp::INT_LEFT, Dst, {Masked, NdVar::cst(LSB, Sz)});
        } else {
          NdVar Shifted = S.makeTemp(Sz);
          S.emit(NdOp::INT_RIGHT, Shifted, {Src, NdVar::cst(LSB, Sz)});
          uint64_t Mask = (Width >= 64) ? ~0ULL : ((1ULL << Width) - 1);
          S.emit(NdOp::INT_AND, Dst, {Shifted, NdVar::cst(Mask, Sz)});
        }
      } else {
        uint64_t ImmR = static_cast<uint64_t>(ARM64.operands[2].imm);
        uint64_t ImmS = static_cast<uint64_t>(ARM64.operands[3].imm);
        if (ImmS >= ImmR) {
          uint64_t Width = ImmS - ImmR + 1;
          NdVar Shifted = S.makeTemp(Sz);
          S.emit(NdOp::INT_RIGHT, Shifted, {Src, NdVar::cst(ImmR, Sz)});
          uint64_t MaskVal = (Width >= 64) ? ~0ULL : ((1ULL << Width) - 1);
          S.emit(NdOp::INT_AND, Dst, {Shifted, NdVar::cst(MaskVal, Sz)});
        } else {
          NdVar Lo = S.makeTemp(Sz);
          NdVar Hi = S.makeTemp(Sz);
          S.emit(NdOp::INT_RIGHT, Lo, {Src, NdVar::cst(ImmR, Sz)});
          S.emit(NdOp::INT_LEFT, Hi, {Src, NdVar::cst(Bits - ImmR, Sz)});
          NdVar Rotated = S.makeTemp(Sz);
          S.emit(NdOp::INT_OR, Rotated, {Lo, Hi});
          uint64_t LSBPos = Bits - ImmR;
          uint64_t Width = ImmS + 1;
          uint64_t MaskVal = ((Width >= 64) ? ~0ULL : ((1ULL << Width) - 1))
                             << LSBPos;
          S.emit(NdOp::INT_AND, Dst, {Rotated, NdVar::cst(MaskVal, Sz)});
        }
      }
      break;
    } // end UBFM 4-op scope
  } // end case UBFM
  case AARCH64_INS_SBFM: {
    // Capstone 6 alias: ASR/SXTB/SXTH/SXTW → id=SBFM, op_count=2
    if (Insn->is_alias && ARM64.op_count < 4 && ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      uint16_t Sz = Dst.Size;
      auto ShT = ARM64.operands[1].shift.type;
      uint32_t ShV = ARM64.operands[1].shift.value;
      bool HasShift = (ShT != AARCH64_SFT_INVALID && ShV != 0);
      // Read raw register value (without shift) to avoid double-shift
      NdVar Src;
      if (HasShift && ARM64.operands[1].type == AARCH64_OP_REG) {
        auto RI =
            mapCapstoneReg(static_cast<aarch64_reg>(ARM64.operands[1].reg));
        Src = (RI.Size == 0)               ? NdVar::cst(0, 8)
              : (RI.Offset == a64reg::XZR) ? NdVar::cst(0, RI.Size)
                                           : NdVar::reg(RI.Offset, RI.Size);
      } else {
        Src = L.operandRead(S, ARM64.operands[1]);
      }
      if (ShT == AARCH64_SFT_ASR && ShV > 0) {
        S.emit(NdOp::INT_ASHR, Dst, {Src, NdVar::cst(ShV, Sz)});
      } else {
        // Determine actual extension width from alias_id:
        // SXTB → 1 byte, SXTH → 2 bytes, SXTW → 4 bytes
        uint16_t ExtSz = Src.Size;
        if (Insn->alias_id == AARCH64_INS_ALIAS_SXTB)
          ExtSz = 1;
        else if (Insn->alias_id == AARCH64_INS_ALIAS_SXTH)
          ExtSz = 2;

        if (ExtSz < Src.Size) {
          NdVar Narrow = S.makeTemp(ExtSz);
          S.emit(NdOp::SUBBYTES, Narrow, {Src, NdVar::cst(0, 4)});
          S.emit(NdOp::INT_SEXT, Dst, {Narrow});
        } else if (Src.Size < Dst.Size) {
          S.emit(NdOp::INT_SEXT, Dst, {Src});
        } else {
          S.emit(NdOp::COPY, Dst, {Src});
        }
      }
      break;
    }
    if (ARM64.op_count < 4)
      break;
    {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[1]);
      uint16_t Sz = Dst.Size;
      uint32_t Bits = Sz * 8;

      if (Insn->is_alias) {
        uint64_t LSB = static_cast<uint64_t>(ARM64.operands[2].imm);
        uint64_t Width = static_cast<uint64_t>(ARM64.operands[3].imm);
        const char *Mn = Insn->mnemonic;
        if (Mn[0] == 's' && Mn[1] == 'b' && Mn[2] == 'f' && Mn[3] == 'i') {
          uint32_t ShiftAmt = Bits - static_cast<uint32_t>(Width + LSB);
          NdVar ShlV = S.makeTemp(Sz);
          S.emit(NdOp::INT_LEFT, ShlV, {Src, NdVar::cst(ShiftAmt + LSB, Sz)});
          S.emit(NdOp::INT_ASHR, Dst, {ShlV, NdVar::cst(ShiftAmt, Sz)});
        } else {
          NdVar Shifted = S.makeTemp(Sz);
          S.emit(NdOp::INT_RIGHT, Shifted, {Src, NdVar::cst(LSB, Sz)});
          uint32_t ShiftAmt = Bits - static_cast<uint32_t>(Width);
          NdVar ShlV = S.makeTemp(Sz);
          S.emit(NdOp::INT_LEFT, ShlV, {Shifted, NdVar::cst(ShiftAmt, Sz)});
          S.emit(NdOp::INT_ASHR, Dst, {ShlV, NdVar::cst(ShiftAmt, Sz)});
        }
      } else {
        uint64_t ImmR = static_cast<uint64_t>(ARM64.operands[2].imm);
        uint64_t ImmS = static_cast<uint64_t>(ARM64.operands[3].imm);

        if (ImmS >= ImmR) {
          uint64_t Width = ImmS - ImmR + 1;
          NdVar Shifted = S.makeTemp(Sz);
          S.emit(NdOp::INT_RIGHT, Shifted, {Src, NdVar::cst(ImmR, Sz)});
          uint32_t ShiftAmt = Bits - static_cast<uint32_t>(Width);
          NdVar ShlV = S.makeTemp(Sz);
          S.emit(NdOp::INT_LEFT, ShlV, {Shifted, NdVar::cst(ShiftAmt, Sz)});
          S.emit(NdOp::INT_ASHR, Dst, {ShlV, NdVar::cst(ShiftAmt, Sz)});
        } else {
          // SBFIZ: extract Src[ImmS:0], place at [Bits-ImmR+ImmS : Bits-ImmR],
          // sign-extend from the top of the inserted field.
          uint64_t Width = ImmS + 1;
          uint64_t LSBPos = Bits - ImmR;
          uint64_t ExtractMask = (Width >= 64) ? ~0ULL : ((1ULL << Width) - 1);
          NdVar Extracted = S.makeTemp(Sz);
          S.emit(NdOp::INT_AND, Extracted, {Src, NdVar::cst(ExtractMask, Sz)});
          NdVar Shifted = S.makeTemp(Sz);
          S.emit(NdOp::INT_LEFT, Shifted, {Extracted, NdVar::cst(LSBPos, Sz)});
          uint32_t TopBit = static_cast<uint32_t>(LSBPos + ImmS);
          uint32_t SignShift = Bits - 1 - TopBit;
          NdVar ShlV = S.makeTemp(Sz);
          S.emit(NdOp::INT_LEFT, ShlV, {Shifted, NdVar::cst(SignShift, Sz)});
          S.emit(NdOp::INT_ASHR, Dst, {ShlV, NdVar::cst(SignShift, Sz)});
        }
      } // end !is_alias
      break;
    } // end SBFM 4-op scope
  } // end case SBFM

  default:
    return false;
  }
  return true;
}

} // namespace neverd
