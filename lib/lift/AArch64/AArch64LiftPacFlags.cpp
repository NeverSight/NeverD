//===- AArch64LiftPacFlags.cpp - PSTATE flags, MTE and pointer auth -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// PSTATE flag manipulation (CFINV/XAFLAG/RMIF/SETF8/SETF16),
/// MTE tag arithmetic (ADDG/SUBG), the data-key pointer
/// authentication variants (AUTD*/PACD*/XPACD), sub-word
/// sign/zero extend (SXTB/SXTH/UXTB/UXTH), CRC32 and the
/// ARMv8.3+ authenticated branch/return forms.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftPacFlags(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // ========================================================================
  // Flag manipulation: CFINV, XAFLAG, RMIF, SETF.
  // ========================================================================
  case AARCH64_INS_CFINV: {
    NdVar CF = NdVar::reg(a64reg::CFLAG, 1);
    S.emit(NdOp::BOOL_NOT, CF, {CF});
    break;
  }
  case AARCH64_INS_XAFLAG: {
    // FEAT_FlagM2: convert NZCV from the "alternative" (JavaScript) FP-compare
    // encoding back to the Arm encoding.  Depends only on C and Z (per the
    // ARM ARM / QEMU `gen_xaflag`):
    //   N = NOT C AND NOT Z;  Z = C AND Z;  C = C OR Z;  V = NOT C AND Z.
    // Was a bare opaque `A64_Xaflag` intrinsic that left NeverD's modelled
    // flags (NFLAG/ZFLAG/CFLAG/VFLAG) untouched, so the conversion was lost.
    NdVar OldZ = S.makeTemp(1);
    S.emit(NdOp::COPY, OldZ, {NdVar::reg(a64reg::ZFLAG, 1)});
    NdVar OldC = S.makeTemp(1);
    S.emit(NdOp::COPY, OldC, {NdVar::reg(a64reg::CFLAG, 1)});
    NdVar NotC = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, NotC, {OldC});
    NdVar NotZ = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, NotZ, {OldZ});
    S.emit(NdOp::BOOL_AND, NdVar::reg(a64reg::NFLAG, 1), {NotC, NotZ});
    S.emit(NdOp::BOOL_AND, NdVar::reg(a64reg::ZFLAG, 1), {OldC, OldZ});
    S.emit(NdOp::BOOL_AND, NdVar::reg(a64reg::VFLAG, 1), {NotC, OldZ});
    S.emit(NdOp::BOOL_OR, NdVar::reg(a64reg::CFLAG, 1), {OldC, OldZ});
    break;
  }
  case AARCH64_INS_RMIF: {
    // FEAT_FlagM: rotate Xn right by #shift, then insert the low 4 bits of the
    // rotated value into NZCV under the 4-bit #mask — bit3->N, bit2->Z,
    // bit1->C, bit0->V — leaving flags not selected by the mask unchanged.
    // Was a bare opaque `A64_Rmif` intrinsic that dropped all three operands
    // and set no flags at all (the rotate, the mask, and the insert were lost).
    if (ARM64.op_count < 3)
      break;
    NdVar Xn = L.operandRead(S, ARM64.operands[0]);
    uint64_t Shift = static_cast<uint64_t>(ARM64.operands[1].imm) & 63;
    uint64_t Mask = static_cast<uint64_t>(ARM64.operands[2].imm) & 0xF;
    // tmp = ROR(Xn, Shift) over the full 64-bit register.
    NdVar Tmp = Xn;
    if (Shift != 0) {
      NdVar Lo = S.makeTemp(8);
      S.emit(NdOp::INT_RIGHT, Lo, {Xn, NdVar::cst(Shift, 8)});
      NdVar Hi = S.makeTemp(8);
      S.emit(NdOp::INT_LEFT, Hi, {Xn, NdVar::cst(64 - Shift, 8)});
      Tmp = S.makeTemp(8);
      S.emit(NdOp::INT_OR, Tmp, {Lo, Hi});
    }
    const struct {
      unsigned Bit;
      uint64_t Reg;
    } FlagMap[4] = {{3, a64reg::NFLAG},
                    {2, a64reg::ZFLAG},
                    {1, a64reg::CFLAG},
                    {0, a64reg::VFLAG}};
    for (const auto &FM : FlagMap) {
      if (!(Mask & (1u << FM.Bit)))
        continue;
      NdVar Sh = S.makeTemp(8);
      S.emit(NdOp::INT_RIGHT, Sh, {Tmp, NdVar::cst(FM.Bit, 8)});
      NdVar Bit = S.makeTemp(8);
      S.emit(NdOp::INT_AND, Bit, {Sh, NdVar::cst(1, 8)});
      S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(FM.Reg, 1),
             {Bit, NdVar::cst(0, 8)});
    }
    break;
  }
  case AARCH64_INS_SETF8:
  case AARCH64_INS_SETF16: {
    // "Evaluate into flags": set NZV as if an 8-bit (SETF8) or 16-bit (SETF16)
    // signed value had been produced, leaving C unchanged.  Per the ARM ARM /
    // QEMU `disas_evaluate_into_flags` (msb = 7 for SETF8, 15 for SETF16):
    //   N = operand<msb>;  Z = (operand<msb:0> == 0);
    //   V = operand<msb+1> EOR operand<msb>;  C unchanged.
    // The old code took N from bit 31 and Z from the whole 32-bit value and
    // never wrote V, so every byte/halfword flag came out wrong (small inputs
    // masked it: low byte == full word, bit7 == bit31 == 0).
    if (ARM64.op_count < 1)
      break;
    NdVar Src = L.operandRead(S, ARM64.operands[0]);
    bool Is8 = (Insn->id == AARCH64_INS_SETF8);
    unsigned MsbBit = Is8 ? 7 : 15;
    uint16_t NarrowSz = Is8 ? 1 : 2;
    // N and Z come from the low byte / halfword only.
    NdVar Narrow = S.makeTemp(NarrowSz);
    S.emit(NdOp::SUBBYTES, Narrow, {Src, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
           {Narrow, NdVar::cst(0, NarrowSz)});
    S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
           {Narrow, NdVar::cst(0, NarrowSz)});
    // V = bit[msb+1] XOR bit[msb] (signed overflow out of the msb).
    NdVar Sh0 = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_RIGHT, Sh0, {Src, NdVar::cst(MsbBit, Src.Size)});
    NdVar Sh1 = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_RIGHT, Sh1, {Src, NdVar::cst(MsbBit + 1, Src.Size)});
    NdVar Xr = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_XOR, Xr, {Sh0, Sh1});
    NdVar VBit = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_AND, VBit, {Xr, NdVar::cst(1, Src.Size)});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(a64reg::VFLAG, 1),
           {VBit, NdVar::cst(0, Src.Size)});
    break;
  }

  // MTE: ADDG / SUBG
  case AARCH64_INS_ADDG:
  case AARCH64_INS_SUBG: {
    if (ARM64.op_count < 4 || ARM64.operands[2].type != AARCH64_OP_IMM ||
        ARM64.operands[3].type != AARCH64_OP_IMM)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    NdVar AddressOffset = NdVar::cst(
        static_cast<uint64_t>(ARM64.operands[2].imm), Src.Size);
    NdVar TagOffset =
        NdVar::cst(static_cast<uint64_t>(ARM64.operands[3].imm), Src.Size);
    S.emitIntrinsic(Insn->id == AARCH64_INS_ADDG ? Intrinsic::Addg
                                                 : Intrinsic::Subg,
                    Dst, {Src, AddressOffset, TagOffset});
    break;
  }

  // AUTDA / AUTDZA / AUTDZA / AUTDB / AUTDZB — pointer authentication (data).
  case AARCH64_INS_AUTDA:
  case AARCH64_INS_AUTDZA:
  case AARCH64_INS_AUTDB:
  case AARCH64_INS_AUTDZB: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      S.emitIntrinsic(Insn->id == AARCH64_INS_AUTDA ||
                              Insn->id == AARCH64_INS_AUTDZA
                          ? Intrinsic::Autda
                          : Intrinsic::Autdb,
                      Dst);
    }
    break;
  }

  // PACIA/PACIB/PACDA/PACDB — additional PAC variants.
  case AARCH64_INS_PACDA:
  case AARCH64_INS_PACDZA:
  case AARCH64_INS_PACDB:
  case AARCH64_INS_PACDZB: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      S.emitIntrinsic(Insn->id == AARCH64_INS_PACDA ||
                              Insn->id == AARCH64_INS_PACDZA
                          ? Intrinsic::Pacda
                          : Intrinsic::Pacdb,
                      Dst);
    }
    break;
  }

  // XPACD — strip PAC from data pointer.
  case AARCH64_INS_XPACD: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      S.emitIntrinsic(Intrinsic::Xpacd, Dst);
    }
    break;
  }

  // SXTB / SXTH / UXTB / UXTH — extract sub-byte/halfword then extend.
  // Capstone reports the source as a w-register (32-bit), but the instruction
  // only uses bits [7:0] (SXTB/UXTB) or [15:0] (SXTH/UXTH).
  case AARCH64_INS_SXTB:
  case AARCH64_INS_SXTH: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    uint16_t ExtSz = (Insn->id == AARCH64_INS_SXTB) ? 1 : 2;
    NdVar Narrow = S.makeTemp(ExtSz);
    S.emit(NdOp::SUBBYTES, Narrow, {Src, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_SEXT, Dst, {Narrow});
    break;
  }
  case AARCH64_INS_UXTB:
  case AARCH64_INS_UXTH: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    uint16_t ExtSz = (Insn->id == AARCH64_INS_UXTB) ? 1 : 2;
    NdVar Narrow = S.makeTemp(ExtSz);
    S.emit(NdOp::SUBBYTES, Narrow, {Src, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_ZEXT, Dst, {Narrow});
    break;
  }

  // WFE/WFI/YIELD/SEV/SEVL/NOP are encoded as HINT in Capstone 6,
  // already handled at the top of the switch.
  // DC/IC/TLBI/AT are encoded as SYS, already handled above.

  // ========================================================================
  // CRC32 (ARMv8.0-CRC)
  // ========================================================================
  case AARCH64_INS_CRC32B:
  case AARCH64_INS_CRC32H:
  case AARCH64_INS_CRC32W:
  case AARCH64_INS_CRC32X:
  case AARCH64_INS_CRC32CB:
  case AARCH64_INS_CRC32CH:
  case AARCH64_INS_CRC32CW:
  case AARCH64_INS_CRC32CX: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    Intrinsic Id;
    switch (Insn->id) {
    case AARCH64_INS_CRC32B:
      Id = Intrinsic::A64_Crc32b;
      break;
    case AARCH64_INS_CRC32H:
      Id = Intrinsic::A64_Crc32h;
      break;
    case AARCH64_INS_CRC32W:
      Id = Intrinsic::A64_Crc32w;
      break;
    case AARCH64_INS_CRC32X:
      Id = Intrinsic::A64_Crc32x;
      break;
    case AARCH64_INS_CRC32CB:
      Id = Intrinsic::A64_Crc32cb;
      break;
    case AARCH64_INS_CRC32CH:
      Id = Intrinsic::A64_Crc32ch;
      break;
    case AARCH64_INS_CRC32CW:
      Id = Intrinsic::A64_Crc32cw;
      break;
    case AARCH64_INS_CRC32CX:
      Id = Intrinsic::A64_Crc32cx;
      break;
    default:
      Id = Intrinsic::A64_Crc32b;
      break;
    }
    S.emitIntrinsic(Id, Dst, {A, B});
    break;
  }

  // ========================================================================
  // PAC Extended variants (ARMv8.3+)
  // ========================================================================
  case AARCH64_INS_BLRAA:
  case AARCH64_INS_BLRAB: {
    if (ARM64.op_count < 1)
      break;
    NdVar Target = L.operandRead(S, ARM64.operands[0]);
    S.emit(NdOp::COPY, NdVar::reg(a64reg::X30, 8), {NdVar::cst(S.Addr + 4, 8)});
    S.emit(NdOp::INDIR_CALL, NdVar::reg(a64reg::X0, 8), {Target});
    break;
  }
  case AARCH64_INS_BLRAAZ:
  case AARCH64_INS_BLRABZ: {
    if (ARM64.op_count < 1)
      break;
    NdVar Target = L.operandRead(S, ARM64.operands[0]);
    S.emit(NdOp::COPY, NdVar::reg(a64reg::X30, 8), {NdVar::cst(S.Addr + 4, 8)});
    S.emit(NdOp::INDIR_CALL, NdVar::reg(a64reg::X0, 8), {Target});
    break;
  }
  case AARCH64_INS_BRAA:
  case AARCH64_INS_BRAB: {
    if (ARM64.op_count < 1)
      break;
    NdVar Target = L.operandRead(S, ARM64.operands[0]);
    S.emit(NdOp::INDIR_BR, {}, {Target});
    break;
  }
  case AARCH64_INS_BRAAZ:
  case AARCH64_INS_BRABZ: {
    if (ARM64.op_count < 1)
      break;
    NdVar Target = L.operandRead(S, ARM64.operands[0]);
    S.emit(NdOp::INDIR_BR, {}, {Target});
    break;
  }
  case AARCH64_INS_RETAA:
  case AARCH64_INS_RETAB:
  case AARCH64_INS_RETAASPPC:
  case AARCH64_INS_RETABSPPC:
    S.emit(NdOp::RETURN, {}, {NdVar::reg(a64reg::X30, 8)});
    break;
  case AARCH64_INS_ERETAA:
  case AARCH64_INS_ERETAB:
    S.emitIntrinsic(Intrinsic::Eret);
    break;
  case AARCH64_INS_AUTIA171615:
  case AARCH64_INS_AUTIASPPC:
    S.emitIntrinsic(Intrinsic::Autia);
    break;
  case AARCH64_INS_AUTIB171615:
  case AARCH64_INS_AUTIBSPPC:
    S.emitIntrinsic(Intrinsic::Autib);
    break;
  case AARCH64_INS_PACIA171615:
  case AARCH64_INS_PACIASPPC:
  case AARCH64_INS_PACNBIASPPC:
    S.emitIntrinsic(Intrinsic::Pacia);
    break;
  case AARCH64_INS_PACIB171615:
  case AARCH64_INS_PACIBSPPC:
  case AARCH64_INS_PACNBIBSPPC:
    S.emitIntrinsic(Intrinsic::Pacib);
    break;
  case AARCH64_INS_PACGA: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_XOR, Dst, {A, B});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
