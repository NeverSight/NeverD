//===- AArch64LiftRotate.cpp - Bit clear, rotate and bitfield insert ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// BIC/BICS, EON, the ROR rotate (including the Capstone RORV
/// alias), the EXTR pair extract and the BFM bitfield insert
/// (BFI/BFXIL).
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/decode/AArch64NativeDecode.h"
#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftRotate(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // --- BIC (bit clear: AND NOT) ---
  case AARCH64_INS_BIC:
  case AARCH64_INS_BICS: {
    // SIMD BIC (vector, immediate): `bic vD.<T>, #imm{, lsl #s}` -> vD &=
    // ~bcast.
    if (ARM64.op_count == 2 && ARM64.operands[0].type == AARCH64_OP_REG &&
        ARM64.operands[0].vas != AARCH64LAYOUT_INVALID &&
        ARM64.operands[1].type == AARCH64_OP_IMM) {
      emitSimdImmLogic(S, ARM64, /*IsBic=*/true);
      break;
    }
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar Inv = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, Inv, {B});
    S.emit(NdOp::INT_AND, Dst, {A, Inv});
    if (Insn->id == AARCH64_INS_BICS) {
      S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      // BICS, like ANDS, clears C and V (only N/Z reflect the result).
      S.emit(NdOp::COPY, NdVar::reg(a64reg::CFLAG, 1), {NdVar::cst(0, 1)});
      S.emit(NdOp::COPY, NdVar::reg(a64reg::VFLAG, 1), {NdVar::cst(0, 1)});
    }
    break;
  }

  // --- EON (exclusive OR NOT) ---
  case AARCH64_INS_EON: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar Inv = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, Inv, {B});
    S.emit(NdOp::INT_XOR, Dst, {A, Inv});
    break;
  }

  // ORN is handled above (Merged with former MVN case)

  // --- ROR (rotate right) ---
  // Capstone 6 alias: RORV Xd,Xn,Xm may be reported as ROR with 2
  // operands — extract Rn and Rm from the encoding.
  case AARCH64_INS_ROR: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src, Amt;
    bool IsRegRot = false;
    if (ARM64.op_count >= 3) {
      Src = L.operandRead(S, ARM64.operands[1]);
      Amt = L.operandRead(S, ARM64.operands[2]);
      IsRegRot = ARM64.operands[2].type == AARCH64_OP_REG;
    } else if (ARM64.op_count == 2 &&
               ARM64.operands[0].type == AARCH64_OP_REG &&
               ARM64.operands[1].type == AARCH64_OP_REG && Insn->size == 4) {
      uint32_t Enc;
      std::memcpy(&Enc, Insn->bytes, 4);
      bool Is64 = (Enc >> 31) & 1;
      unsigned RnIdx = (Enc >> 5) & 0x1F;
      unsigned RmIdx = (Enc >> 16) & 0x1F;
      auto RnReg = a64native::gpr(RnIdx, Is64);
      auto RmReg = a64native::gpr(RmIdx, Is64);
      auto RnRI = mapCapstoneReg(RnReg);
      auto RmRI = mapCapstoneReg(RmReg);
      Src = (RnRI.Size > 0) ? NdVar::reg(RnRI.Offset, RnRI.Size)
                            : L.operandRead(S, ARM64.operands[0]);
      Amt = (RmRI.Size > 0) ? NdVar::reg(RmRI.Offset, RmRI.Size)
                            : NdVar::cst(0, Dst.Size);
      IsRegRot = true;
    } else {
      Src = L.operandRead(S, ARM64.operands[0]);
      Amt = L.operandRead(S, ARM64.operands[1]);
    }
    uint16_t Bits = Dst.Size * 8;
    NdVar MaskedAmt = Amt;
    if (IsRegRot) {
      uint64_t Mask = (Dst.Size == 8) ? 63 : 31;
      MaskedAmt = S.makeTemp(Amt.Size);
      S.emit(NdOp::INT_AND, MaskedAmt, {Amt, NdVar::cst(Mask, Amt.Size)});
    }
    NdVar RightPart = S.makeTemp(Dst.Size);
    NdVar LeftPart = S.makeTemp(Dst.Size);
    NdVar Comp = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_RIGHT, RightPart, {Src, MaskedAmt});
    S.emit(NdOp::INT_SUB, Comp, {NdVar::cst(Bits, Dst.Size), MaskedAmt});
    S.emit(NdOp::INT_LEFT, LeftPart, {Src, Comp});
    S.emit(NdOp::INT_OR, Dst, {RightPart, LeftPart});
    break;
  }

  // --- EXTR (extract from pair) ---
  case AARCH64_INS_EXTR: {
    // Capstone 6 alias: ROR Xd, Xn, #shift → id=EXTR, op_count=2
    if (Insn->is_alias && ARM64.op_count < 4 && ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      uint16_t Sz = Dst.Size;
      uint32_t Bits = Sz * 8;
      auto ShT = ARM64.operands[1].shift.type;
      uint32_t ShV = ARM64.operands[1].shift.value;
      bool HasShift = (ShT != AARCH64_SFT_INVALID && ShV != 0);
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
      if (ShV > 0 && ShV < Bits) {
        NdVar Lo = S.makeTemp(Sz);
        NdVar Hi = S.makeTemp(Sz);
        S.emit(NdOp::INT_RIGHT, Lo, {Src, NdVar::cst(ShV, Sz)});
        S.emit(NdOp::INT_LEFT, Hi, {Src, NdVar::cst(Bits - ShV, Sz)});
        S.emit(NdOp::INT_OR, Dst, {Lo, Hi});
      } else {
        S.emit(NdOp::COPY, Dst, {Src});
      }
      break;
    }
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Hi = L.operandRead(S, ARM64.operands[1]);
    NdVar Lo = L.operandRead(S, ARM64.operands[2]);
    NdVar LSB = L.operandRead(S, ARM64.operands[3]);
    uint16_t Bits = Dst.Size * 8;
    NdVar LoShifted = S.makeTemp(Dst.Size);
    NdVar HiShifted = S.makeTemp(Dst.Size);
    NdVar Comp = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_RIGHT, LoShifted, {Lo, LSB});
    S.emit(NdOp::INT_SUB, Comp, {NdVar::cst(Bits, Dst.Size), LSB});
    S.emit(NdOp::INT_LEFT, HiShifted, {Hi, Comp});
    S.emit(NdOp::INT_OR, Dst, {LoShifted, HiShifted});
    break;
  }

  // --- BFM (BFI/BFXIL resolved to BFM in Capstone 6) ---
  case AARCH64_INS_BFM: {
    // BFC Xd,#lsb,#width is a BFM alias whose source is the implicit zero
    // register, so Capstone surfaces only THREE operands (Xd,#lsb,#width) with
    // no Rn.  It clears `width` bits starting at `lsb`.  Without this the
    // generic
    // >=4-operand path below `break`s out on op_count==3 and leaves Rd
    // unchanged (BFC was silently a no-op).
    if (Insn->is_alias && Insn->mnemonic[0] == 'b' &&
        Insn->mnemonic[1] == 'f' && Insn->mnemonic[2] == 'c' &&
        ARM64.op_count >= 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      uint16_t Sz = Dst.Size;
      uint32_t Bits = Sz * 8;
      uint64_t Lsb =
          static_cast<uint64_t>(ARM64.operands[ARM64.op_count - 2].imm);
      uint64_t Width =
          static_cast<uint64_t>(ARM64.operands[ARM64.op_count - 1].imm);
      uint64_t FieldMask = (Width >= 64) ? ~0ULL : ((1ULL << Width) - 1);
      uint64_t ClearMask = (Lsb >= Bits) ? 0 : (FieldMask << Lsb);
      S.emit(NdOp::INT_AND, Dst,
             {NdVar::reg(Dst.Offset, Sz), NdVar::cst(~ClearMask, Sz)});
      break;
    }
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    uint16_t Sz = Dst.Size;
    uint32_t Bits = Sz * 8;

    uint64_t ImmR, ImmS;
    if (Insn->is_alias) {
      // Capstone 6 alias: BFI/BFXIL provide (lsb, width) not (ImmR, ImmS).
      uint64_t Lsb = static_cast<uint64_t>(ARM64.operands[2].imm);
      uint64_t Width = static_cast<uint64_t>(ARM64.operands[3].imm);
      const char *Mn = Insn->mnemonic;
      if (Mn[0] == 'b' && Mn[1] == 'f' && Mn[2] == 'i') {
        // BFI Xd,Xn,#lsb,#width → BFM Xd,Xn,#(-lsb MOD Bits),#(width-1)
        ImmR = (Bits - Lsb) % Bits;
        ImmS = Width - 1;
      } else {
        // BFXIL Xd,Xn,#lsb,#width → BFM Xd,Xn,#lsb,#(lsb+width-1)
        ImmR = Lsb;
        ImmS = Lsb + Width - 1;
      }
    } else {
      ImmR = static_cast<uint64_t>(ARM64.operands[2].imm);
      ImmS = static_cast<uint64_t>(ARM64.operands[3].imm);
    }

    {
      // Both cases use ROR(Src, ImmR) then Mask at the correct position.
      // ARM pseudocode: bot = (Xd & ~wmask) | (ROR(Xn, ImmR) & wmask)
      NdVar Rotated;
      if (ImmR == 0) {
        Rotated = Src;
      } else {
        NdVar Lo = S.makeTemp(Sz);
        NdVar Hi = S.makeTemp(Sz);
        S.emit(NdOp::INT_RIGHT, Lo, {Src, NdVar::cst(ImmR, Sz)});
        S.emit(NdOp::INT_LEFT, Hi, {Src, NdVar::cst(Bits - ImmR, Sz)});
        Rotated = S.makeTemp(Sz);
        S.emit(NdOp::INT_OR, Rotated, {Lo, Hi});
      }

      uint64_t Mask;
      if (ImmS >= ImmR) {
        // BFXIL: extract Src[ImmS:ImmR] → Dst[Width-1:0]
        uint64_t Width = ImmS - ImmR + 1;
        Mask = (Width >= 64) ? ~0ULL : ((1ULL << Width) - 1);
      } else {
        // BFI: insert Src[ImmS:0] → Dst[lsb+ImmS:lsb] where lsb=Bits-ImmR
        uint64_t LSBPos = Bits - ImmR;
        uint64_t Width = ImmS + 1;
        Mask = ((Width >= 64) ? ~0ULL : ((1ULL << Width) - 1)) << LSBPos;
      }
      NdVar MaskedSrc = S.makeTemp(Sz);
      S.emit(NdOp::INT_AND, MaskedSrc, {Rotated, NdVar::cst(Mask, Sz)});
      NdVar ClearedDst = S.makeTemp(Sz);
      S.emit(NdOp::INT_AND, ClearedDst,
             {NdVar::reg(Dst.Offset, Sz), NdVar::cst(~Mask, Sz)});
      S.emit(NdOp::INT_OR, Dst, {ClearedDst, MaskedSrc});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
