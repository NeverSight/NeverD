//===- AArch64DecodeLogical.h - Native decode of AArch64 logic -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Operand decode of the AArch64 bit-manipulation classes: logical
/// shifted-register and logical immediate (with the MOV/MVN/TST aliases),
/// EXTR/ROR, and the bitfield moves SBFM/BFM/UBFM with the full set of
/// preferred aliases Capstone renders for them.  Every entry point here is
/// reached from tryDecode in neverd/decode/AArch64DecodeDispatch.h, which owns
/// the class masks; each function assumes its class mask has already matched
/// and returns true when \p Insn is fully populated, false to decline to
/// Capstone.  Include neverd/decode/AArch64NativeDecode.h instead of this
/// header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_AARCH64DECODELOGICAL_H
#define NEVERD_DECODE_AARCH64DECODELOGICAL_H

#include "neverd/Common.h"
#include "neverd/decode/AArch64DecodeImm.h"
#include "neverd/decode/AArch64DecodeOperands.h"

#include <capstone/capstone.h>
#include <cstdint>

namespace neverd {
namespace a64native {

/// Logical (shifted register): bits[28:24]==0b01010.
/// sf opc(2) 0 1 0 1 0 shift(2) N Rm imm6 Rn Rd.  All four shift types legal.
/// opc/N select AND/BIC/ORR/ORN/EOR/EON/ANDS/BICS.  Index 31 is the zero
/// register.  MOV (ORR,Rn==31,LSL#0), MVN (ORN,Rn==31) and TST (ANDS,Rd==31)
/// are the aliases the lifter recognizes.
inline bool decodeLogicalShiftedReg(uint32_t Word, va_t Addr, cs_insn &Insn,
                                    cs_detail &Detail) {
  const unsigned Rd = Word & 0x1F;

  const bool Is64 = (Word >> 31) & 1u;
  const unsigned Imm6 = (Word >> 10) & 0x3Fu;
  if (!Is64 && Imm6 >= 32)
    return false;
  const unsigned Opc = (Word >> 29) & 3u;
  const unsigned ShType = (Word >> 22) & 3u;
  const bool NotVar = (Word >> 21) & 1u;
  const unsigned Rm = (Word >> 16) & 0x1Fu;
  const unsigned Rn = (Word >> 5) & 0x1Fu;

  // MOV Rd,Rm == ORR Rd,XZR,Rm with LSL #0.
  if (Opc == 1 && !NotVar && Rn == 31 && ShType == 0 && Imm6 == 0) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_ORR);
    Insn.is_alias = true;
    setMnem(Insn, "mov");
    A.op_count = 2;
    setReg(A.operands[0], gpr(Rd, Is64));
    A.operands[0].access = CS_AC_WRITE;
    setReg(A.operands[1], gpr(Rm, Is64));
    A.operands[1].access = CS_AC_READ;
    return true;
  }
  // MVN Rd,Rm{,shift} == ORN Rd,XZR,Rm{,shift}.
  if (Opc == 1 && NotVar && Rn == 31) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_ORN);
    Insn.is_alias = true;
    setMnem(Insn, "mvn");
    A.op_count = 2;
    setReg(A.operands[0], gpr(Rd, Is64));
    A.operands[0].access = CS_AC_WRITE;
    setRegShift(A.operands[1], gpr(Rm, Is64), ShType, Imm6);
    A.operands[1].access = CS_AC_READ;
    return true;
  }
  // TST Rn,Rm{,shift} == ANDS XZR,Rn,Rm{,shift}.
  if (Opc == 3 && !NotVar && Rd == 31) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_ANDS);
    Insn.is_alias = true;
    setMnem(Insn, "tst");
    A.update_flags = true;
    A.op_count = 2;
    setReg(A.operands[0], gpr(Rn, Is64));
    A.operands[0].access = CS_AC_READ;
    setRegShift(A.operands[1], gpr(Rm, Is64), ShType, Imm6);
    A.operands[1].access = CS_AC_READ;
    return true;
  }
  static const unsigned IdTab[4][2] = {{AARCH64_INS_AND, AARCH64_INS_BIC},
                                       {AARCH64_INS_ORR, AARCH64_INS_ORN},
                                       {AARCH64_INS_EOR, AARCH64_INS_EON},
                                       {AARCH64_INS_ANDS, AARCH64_INS_BICS}};
  const bool Flags = (Opc == 3);
  cs_aarch64 &A = begin(Insn, Detail, Word, Addr, IdTab[Opc][NotVar]);
  A.update_flags = Flags;
  A.op_count = 3;
  setReg(A.operands[0], gpr(Rd, Is64));
  A.operands[0].access = CS_AC_WRITE;
  setReg(A.operands[1], gpr(Rn, Is64));
  A.operands[1].access = CS_AC_READ;
  setRegShift(A.operands[2], gpr(Rm, Is64), ShType, Imm6);
  A.operands[2].access = CS_AC_READ;
  return true;
}

/// EXTR (extract register): bits[30:23]==0b00100111, N==sf, o0(bit21)==0.
/// Capstone renders the Rn==Rm case as the ROR-immediate alias.
inline bool decodeExtr(uint32_t Word, va_t Addr, cs_insn &Insn,
                       cs_detail &Detail) {
  const bool Is64 = (Word >> 31) & 1u;
  const unsigned Imms = (Word >> 10) & 0x3Fu;
  if (!Is64 && Imms >= 32)
    return false; // 32-bit EXTR with imms>=32 is UNALLOCATED.
  const unsigned Rm = (Word >> 16) & 0x1Fu;
  const unsigned Rn = (Word >> 5) & 0x1Fu;
  const unsigned Rd = Word & 0x1Fu;
  if (Rn == Rm) {
    // ROR <Rd>, <Rn>, #imms.
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_EXTR);
    Insn.is_alias = true;
    setMnem(Insn, "ror");
    A.op_count = 2;
    setReg(A.operands[0], gpr(Rd, Is64));
    A.operands[0].access = CS_AC_WRITE;
    setReg(A.operands[1], gpr(Rn, Is64));
    A.operands[1].access = CS_AC_READ;
    A.operands[1].shift.type = AARCH64_SFT_ROR;
    A.operands[1].shift.value = Imms;
    return true;
  }
  cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_EXTR);
  A.op_count = 4;
  setReg(A.operands[0], gpr(Rd, Is64));
  A.operands[0].access = CS_AC_WRITE;
  setReg(A.operands[1], gpr(Rn, Is64));
  A.operands[1].access = CS_AC_READ;
  setReg(A.operands[2], gpr(Rm, Is64));
  A.operands[2].access = CS_AC_READ;
  setImm(A.operands[3], static_cast<int64_t>(Imms));
  A.operands[3].access = CS_AC_READ;
  return true;
}

/// Bitfield move (SBFM/BFM/UBFM): bits[28:23]==0b100110.  Capstone almost
/// always renders one of the many preferred aliases (lsl/lsr/asr, uxtb/uxth,
/// sxtb/sxth/sxtw, ubfx/ubfiz, sbfx/sbfiz, bfi/bfxil/bfc), and the lifter's
/// decode depends on exactly which alias + operands it sees, so the selection
/// is reproduced bit-for-bit here.  N must equal sf and, for the 32-bit form,
/// immr/imms must be < 32 (else UNALLOCATED — declined).
inline bool decodeBitfield(uint32_t Word, va_t Addr, cs_insn &Insn,
                           cs_detail &Detail) {
  const bool Sf = (Word >> 31) & 1u;
  const unsigned Opc = (Word >> 29) & 3u;
  const unsigned NBit = (Word >> 22) & 1u;
  unsigned Immr = (Word >> 16) & 0x3Fu;
  unsigned Imms = (Word >> 10) & 0x3Fu;
  const unsigned Rn = (Word >> 5) & 0x1Fu;
  const unsigned Rd = Word & 0x1Fu;
  if (Opc == 3 || (unsigned)Sf != NBit)
    return false; // opc 11 reserved; N must mirror sf.
  if (!Sf && (Immr >= 32 || Imms >= 32))
    return false; // 32-bit form with a 6-bit field set -> UNALLOCATED.
  const unsigned DataSize = Sf ? 64u : 32u;

  // 2-operand register-plus-shift alias (lsl/lsr/asr) shared by UBFM/SBFM.
  auto shiftAlias = [&](unsigned Id, aarch64_shifter Sft, unsigned Amt) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Id);
    Insn.is_alias = true;
    setMnem(Insn, Sft == AARCH64_SFT_ASR   ? "asr"
                  : Sft == AARCH64_SFT_LSR ? "lsr"
                                           : "lsl");
    A.op_count = 2;
    setReg(A.operands[0], gpr(Rd, Sf));
    A.operands[0].access = CS_AC_WRITE;
    setReg(A.operands[1], gpr(Rn, Sf));
    A.operands[1].access = CS_AC_READ;
    A.operands[1].shift.type = Sft;
    A.operands[1].shift.value = Amt;
  };
  // 2-operand extend alias (uxtb/uxth/sxtb/sxth/sxtw): the source is the
  // W view; the destination width follows sf (sxtw is always X).  Capstone
  // marks operands[1] with the corresponding extender, and the lifter's
  // operandRead applies it, so set it here too for identical lifted IR.
  auto extendAlias = [&](unsigned Id, const char *Mn, unsigned AliasId,
                         aarch64_extender Ext, bool DstIs64) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Id);
    Insn.is_alias = true;
    Insn.alias_id = AliasId;
    setMnem(Insn, Mn);
    A.op_count = 2;
    setReg(A.operands[0], gpr(Rd, DstIs64));
    A.operands[0].access = CS_AC_WRITE;
    setReg(A.operands[1], gpr(Rn, /*Is64=*/false));
    A.operands[1].ext = Ext;
    A.operands[1].access = CS_AC_READ;
  };
  // 4-operand extract/insert alias (ubfx/ubfiz/sbfx/sbfiz/bfi/bfxil).
  auto fieldAlias = [&](unsigned Id, const char *Mn, unsigned Lsb,
                        unsigned Width) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Id);
    Insn.is_alias = true;
    setMnem(Insn, Mn);
    A.op_count = 4;
    setReg(A.operands[0], gpr(Rd, Sf));
    A.operands[0].access = CS_AC_WRITE;
    setReg(A.operands[1], gpr(Rn, Sf));
    A.operands[1].access = CS_AC_READ;
    setImm(A.operands[2], static_cast<int64_t>(Lsb));
    setImm(A.operands[3], static_cast<int64_t>(Width));
  };

  if (Opc == 2) { // UBFM
    if (Imms == DataSize - 1)
      shiftAlias(AARCH64_INS_UBFM, AARCH64_SFT_LSR, Immr);
    else if (Imms + 1 == Immr)
      shiftAlias(AARCH64_INS_UBFM, AARCH64_SFT_LSL, DataSize - Immr);
    else if (!Sf && Immr == 0 && Imms == 7)
      extendAlias(AARCH64_INS_UBFM, "uxtb", AARCH64_INS_ALIAS_UXTB,
                  AARCH64_EXT_UXTB, false);
    else if (!Sf && Immr == 0 && Imms == 15)
      extendAlias(AARCH64_INS_UBFM, "uxth", AARCH64_INS_ALIAS_UXTH,
                  AARCH64_EXT_UXTH, false);
    else if (Immr <= Imms)
      fieldAlias(AARCH64_INS_UBFM, "ubfx", Immr, Imms - Immr + 1);
    else
      fieldAlias(AARCH64_INS_UBFM, "ubfiz", DataSize - Immr, Imms + 1);
    return true;
  }
  if (Opc == 0) { // SBFM
    if (Imms == DataSize - 1)
      shiftAlias(AARCH64_INS_SBFM, AARCH64_SFT_ASR, Immr);
    else if (Immr == 0 && Imms == 7)
      extendAlias(AARCH64_INS_SBFM, "sxtb", AARCH64_INS_ALIAS_SXTB,
                  AARCH64_EXT_SXTB, Sf);
    else if (Immr == 0 && Imms == 15)
      extendAlias(AARCH64_INS_SBFM, "sxth", AARCH64_INS_ALIAS_SXTH,
                  AARCH64_EXT_SXTH, Sf);
    else if (Sf && Immr == 0 && Imms == 31)
      extendAlias(AARCH64_INS_SBFM, "sxtw", AARCH64_INS_ALIAS_SXTW,
                  AARCH64_EXT_SXTW, true);
    else if (Immr <= Imms)
      fieldAlias(AARCH64_INS_SBFM, "sbfx", Immr, Imms - Immr + 1);
    else
      fieldAlias(AARCH64_INS_SBFM, "sbfiz", DataSize - Immr, Imms + 1);
    return true;
  }
  // Opc == 1: BFM (Rn!=31 only).  When Rn==31 Capstone chooses between BFC
  // and BFXIL-with-zr by a heuristic that is not the clean architectural
  // rule, so that rare sub-case (bitfield clears) is declined to Capstone.
  // BFI (imms<immr) / BFXIL (imms>=immr) with a real source are unambiguous.
  // All BFM forms read-modify-write Rd -> writeback=true (matches Capstone).
  if (Rn == 31)
    return false;
  if (Imms >= Immr)
    fieldAlias(AARCH64_INS_BFM, "bfxil", Immr, Imms - Immr + 1);
  else
    fieldAlias(AARCH64_INS_BFM, "bfi", DataSize - Immr, Imms + 1);
  Detail.writeback = true;
  return true;
}

/// Logical (immediate): bits[28:23]==0b100100.  sf opc 100100 N immr imms
/// Rn Rd.  opc selects AND/ORR/EOR/ANDS; the bitmask immediate is decoded to
/// its value (Capstone reports the decoded value).  Rd is the SP-form for
/// AND/ORR/EOR and the ZR-form for ANDS; Rn is always the ZR-form.  The
/// ORR-with-Rn==31 MOV(bitmask) alias is reproduced via the MoveWidePreferred
/// predicate (isAnyMovwMovAlias); the TST (ANDS, Rd==31) alias is reproduced
/// too.
inline bool decodeLogicalImm(uint32_t Word, va_t Addr, cs_insn &Insn,
                             cs_detail &Detail) {
  const bool Is64 = (Word >> 31) & 1u;
  const unsigned Opc = (Word >> 29) & 3u;
  const unsigned NBit = (Word >> 22) & 1u;
  const unsigned Immr = (Word >> 16) & 0x3Fu;
  const unsigned Imms = (Word >> 10) & 0x3Fu;
  const unsigned Rn = (Word >> 5) & 0x1Fu;
  const unsigned Rd = Word & 0x1Fu;
  uint64_t Imm = 0;
  if (!decodeBitMasks(Is64, NBit, Imms, Immr, Imm))
    return false; // UNDEFINED bitmask -> let Capstone reject.

  // ORR with Rn==31: `orr Rd, xzr, #imm` is the canonical constant-build.
  // Capstone renders it as the `mov Rd, #imm` alias exactly when the value is
  // NOT movz/movn-preferred (the MoveWidePreferred predicate); otherwise it
  // keeps the plain three-operand orr.  The alias sign-extends the value from
  // the register width (32-bit `mov w`, top bit set -> negative int64), which
  // the lifter's is_alias ORR copy reproduces verbatim, so match it here.
  if (Opc == 1 && Rn == 31) {
    const unsigned RegWidth = Is64 ? 64u : 32u;
    if (!isAnyMovwMovAlias(Imm, RegWidth)) {
      int64_t MovImm = Is64 ? static_cast<int64_t>(Imm)
                            : static_cast<int64_t>(static_cast<int32_t>(
                                  static_cast<uint32_t>(Imm)));
      cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_ORR);
      Insn.is_alias = true;
      setMnem(Insn, "mov");
      A.op_count = 2;
      setReg(A.operands[0], gprSP(Rd, Is64));
      A.operands[0].access = CS_AC_WRITE;
      setImm(A.operands[1], MovImm);
      A.operands[1].access = CS_AC_READ;
      return true;
    }
    // movz/movn-preferred value -> plain `orr` form (fall through).
  }

  if (Opc == 3 && Rd == 31) {
    // TST <Rn>, #imm  ==  ANDS ZR, Rn, #imm.
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_ANDS);
    Insn.is_alias = true;
    setMnem(Insn, "tst");
    A.update_flags = true;
    A.op_count = 2;
    setReg(A.operands[0], gpr(Rn, Is64));
    A.operands[0].access = CS_AC_READ;
    setImm(A.operands[1], static_cast<int64_t>(Imm));
    A.operands[1].access = CS_AC_READ;
    return true;
  }
  unsigned Id = (Opc == 0)   ? AARCH64_INS_AND
                : (Opc == 1) ? AARCH64_INS_ORR
                : (Opc == 2) ? AARCH64_INS_EOR
                             : AARCH64_INS_ANDS;
  const bool Flags = (Opc == 3);
  cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Id);
  A.update_flags = Flags;
  A.op_count = 3;
  // AND/ORR/EOR write the SP-form Rd; ANDS writes a GP register (Rd==31 was
  // the TST alias handled above).
  setReg(A.operands[0], Flags ? gpr(Rd, Is64) : gprSP(Rd, Is64));
  A.operands[0].access = CS_AC_WRITE;
  setReg(A.operands[1], gpr(Rn, Is64));
  A.operands[1].access = CS_AC_READ;
  setImm(A.operands[2], static_cast<int64_t>(Imm));
  A.operands[2].access = CS_AC_READ;
  return true;
}

} // namespace a64native
} // namespace neverd

#endif // NEVERD_DECODE_AARCH64DECODELOGICAL_H
