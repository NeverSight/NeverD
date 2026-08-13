//===- AArch64DecodeArith.h - Native decode of AArch64 add/sub -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Operand decode of the AArch64 add/subtract classes (immediate, shifted
/// register and extended register) together with the CMP/CMN/NEG/MOV aliases
/// Capstone renders for them.  Every entry point here is reached from
/// tryDecode in neverd/decode/AArch64DecodeDispatch.h, which owns the class
/// masks; each function assumes its class mask has already matched and returns
/// true when \p Insn is fully populated, false to decline to Capstone.
/// Include neverd/decode/AArch64NativeDecode.h instead of this header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_AARCH64DECODEARITH_H
#define NEVERD_DECODE_AARCH64DECODEARITH_H

#include "neverd/Common.h"
#include "neverd/decode/AArch64DecodeOperands.h"

#include <capstone/capstone.h>
#include <cstdint>

namespace neverd {
namespace a64native {

/// Add/subtract (immediate): bits[28:24]==0b10001, bit23==0.
/// sf op S 1 0 0 0 1 0 sh imm12 Rn Rd.  Rd/Rn use the SP interpretation of
/// index 31.  The MOV-to/from-SP, CMP and CMN aliases the lifter recognizes
/// are reproduced exactly; everything else is the plain 3-operand form.
inline bool decodeAddSubImm(uint32_t Word, va_t Addr, cs_insn &Insn,
                            cs_detail &Detail) {
  const unsigned Rd = Word & 0x1F;

  const bool Is64 = (Word >> 31) & 1u;
  const bool IsSub = (Word >> 30) & 1u;
  const bool SetFlags = (Word >> 29) & 1u;
  const bool Sh12 = (Word >> 22) & 1u;
  const uint32_t Imm12 = (Word >> 10) & 0xFFFu;
  const unsigned Rn = (Word >> 5) & 0x1Fu;

  // MOV <Xd|SP>, <Xn|SP>  ==  ADD (imm) #0, no shift, S=0, and SP involved.
  if (!IsSub && !SetFlags && !Sh12 && Imm12 == 0 && (Rd == 31 || Rn == 31)) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_ADD);
    Insn.is_alias = true;
    setMnem(Insn, "mov");
    A.op_count = 2;
    setReg(A.operands[0], gprSP(Rd, Is64));
    A.operands[0].access = CS_AC_WRITE;
    setReg(A.operands[1], gprSP(Rn, Is64));
    A.operands[1].access = CS_AC_READ;
    return true;
  }
  // CMP/CMN (imm): SUBS/ADDS with Rd==31 (result discarded, flags only).
  if (SetFlags && Rd == 31) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                          IsSub ? AARCH64_INS_SUBS : AARCH64_INS_ADDS);
    Insn.is_alias = true;
    setMnem(Insn, IsSub ? "cmp" : "cmn");
    A.update_flags = true;
    A.op_count = 2;
    setReg(A.operands[0], gprSP(Rn, Is64));
    A.operands[0].access = CS_AC_READ;
    setImm(A.operands[1], static_cast<int64_t>(Imm12));
    A.operands[1].access = CS_AC_READ;
    if (Sh12) {
      A.operands[1].shift.type = AARCH64_SFT_LSL;
      A.operands[1].shift.value = 12;
    }
    return true;
  }
  // Plain ADD/ADDS/SUB/SUBS (immediate).
  unsigned Id = IsSub ? (SetFlags ? AARCH64_INS_SUBS : AARCH64_INS_SUB)
                      : (SetFlags ? AARCH64_INS_ADDS : AARCH64_INS_ADD);
  cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Id);
  A.update_flags = SetFlags;
  A.op_count = 3;
  setReg(A.operands[0], gprSP(Rd, Is64));
  A.operands[0].access = CS_AC_WRITE;
  setReg(A.operands[1], gprSP(Rn, Is64));
  A.operands[1].access = CS_AC_READ;
  setImm(A.operands[2], static_cast<int64_t>(Imm12));
  A.operands[2].access = CS_AC_READ;
  if (Sh12) {
    A.operands[2].shift.type = AARCH64_SFT_LSL;
    A.operands[2].shift.value = 12;
  }
  return true;
}

/// Add/subtract (shifted register): bits[28:24]==0b01011, bit21==0.
/// sf op S 0 1 0 1 1 shift(2) 0 Rm imm6 Rn Rd.  Index 31 is the zero register
/// (no SP here).  ROR (shift==3) is reserved, and a 32-bit amount>=32 is
/// UNALLOCATED — both declined to stay a strict Capstone subset.  The
/// extended-register form (bit21==1) is handled by the next block.
inline bool decodeAddSubShiftedReg(uint32_t Word, va_t Addr, cs_insn &Insn,
                                   cs_detail &Detail) {
  const unsigned Rd = Word & 0x1F;

  const unsigned ShType = (Word >> 22) & 3u;
  if (ShType == 3)
    return false; // ROR reserved for add/sub.
  const bool Is64 = (Word >> 31) & 1u;
  const unsigned Imm6 = (Word >> 10) & 0x3Fu;
  if (!Is64 && Imm6 >= 32)
    return false;
  const bool IsSub = (Word >> 30) & 1u;
  const bool SetFlags = (Word >> 29) & 1u;
  const unsigned Rm = (Word >> 16) & 0x1Fu;
  const unsigned Rn = (Word >> 5) & 0x1Fu;

  // CMP/CMN (reg): SUBS/ADDS, Rd==31.  NEG/NEGS: SUB/SUBS, Rn==31.  When both
  // Rd and Rn are 31 the two alias rules overlap and Capstone's choice is not
  // worth reproducing — decline that rare case.
  const bool CmpForm = SetFlags && Rd == 31;
  const bool NegForm = IsSub && Rn == 31;
  if (CmpForm && NegForm)
    return false;
  if (CmpForm) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                          IsSub ? AARCH64_INS_SUBS : AARCH64_INS_ADDS);
    Insn.is_alias = true;
    setMnem(Insn, IsSub ? "cmp" : "cmn");
    A.update_flags = true;
    A.op_count = 2;
    setReg(A.operands[0], gpr(Rn, Is64));
    A.operands[0].access = CS_AC_READ;
    setRegShift(A.operands[1], gpr(Rm, Is64), ShType, Imm6);
    A.operands[1].access = CS_AC_READ;
    return true;
  }
  if (NegForm) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                          SetFlags ? AARCH64_INS_SUBS : AARCH64_INS_SUB);
    Insn.is_alias = true;
    setMnem(Insn, SetFlags ? "negs" : "neg");
    A.update_flags = SetFlags;
    A.op_count = 2;
    setReg(A.operands[0], gpr(Rd, Is64));
    A.operands[0].access = CS_AC_WRITE;
    setRegShift(A.operands[1], gpr(Rm, Is64), ShType, Imm6);
    A.operands[1].access = CS_AC_READ;
    return true;
  }
  unsigned Id = IsSub ? (SetFlags ? AARCH64_INS_SUBS : AARCH64_INS_SUB)
                      : (SetFlags ? AARCH64_INS_ADDS : AARCH64_INS_ADD);
  cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Id);
  A.update_flags = SetFlags;
  A.op_count = 3;
  setReg(A.operands[0], gpr(Rd, Is64));
  A.operands[0].access = CS_AC_WRITE;
  setReg(A.operands[1], gpr(Rn, Is64));
  A.operands[1].access = CS_AC_READ;
  setRegShift(A.operands[2], gpr(Rm, Is64), ShType, Imm6);
  A.operands[2].access = CS_AC_READ;
  return true;
}

/// Add/subtract (extended register): bits[28:24]==0b01011, bit21==1.
/// sf op S 0 1 0 1 1 opt(2) 1 Rm option(3) imm3(3) Rn Rd.  opt(23:22) must be
/// 0 and imm3<=4 (else UNALLOCATED).  Rd/Rn use the SP interpretation of index
/// 31; Rm is the zero-register form.  The shifted-register class (bit21==0) is
/// handled above; the register letter for Rm is X only for the UXTX/SXTX
/// options of a 64-bit op, else W (matching Capstone's operand detail).
inline bool decodeAddSubExtendedReg(uint32_t Word, va_t Addr, cs_insn &Insn,
                                    cs_detail &Detail) {
  const unsigned Rd = Word & 0x1F;

  if (((Word >> 22) & 3u) != 0u)
    return false; // opt!=00 is UNALLOCATED.
  const unsigned Imm3 = (Word >> 10) & 0x7u;
  if (Imm3 > 4)
    return false; // amount>4 is UNALLOCATED.
  const bool Is64 = (Word >> 31) & 1u;
  const bool IsSub = (Word >> 30) & 1u;
  const bool SetFlags = (Word >> 29) & 1u;
  const unsigned Rm = (Word >> 16) & 0x1Fu;
  const unsigned Option = (Word >> 13) & 0x7u;
  const unsigned Rn = (Word >> 5) & 0x1Fu;

  // The register letter: UXTX(011)/SXTX(111) of a 64-bit op use the X view;
  // everything else uses the W view.
  const bool RmIs64 = Is64 && (Option == 3u || Option == 7u);
  static const aarch64_extender ExtTab[8] = {
      AARCH64_EXT_UXTB, AARCH64_EXT_UXTH, AARCH64_EXT_UXTW, AARCH64_EXT_UXTX,
      AARCH64_EXT_SXTB, AARCH64_EXT_SXTH, AARCH64_EXT_SXTW, AARCH64_EXT_SXTX};
  // Capstone drops the extend to a plain register (ext INVALID, no shift)
  // only for the fully-natural LSL#0 form with SP: amount==0, the natural
  // option (UXTX for 64-bit, UXTW for 32-bit), and either the destination is
  // SP (non-flag form, Rd==31) or the first source is SP (Rn==31).
  const bool NaturalOpt = (Option == (Is64 ? 3u : 2u));
  const bool DropExtend =
      Imm3 == 0 && NaturalOpt && ((!SetFlags && Rd == 31) || Rn == 31);

  // Populate the extended Rm operand (or the plain register when dropped).
  auto setExtReg = [&](cs_aarch64_op &Op) {
    if (DropExtend) {
      setReg(Op, gpr(Rm, Is64));
    } else {
      clearOp(Op);
      Op.type = AARCH64_OP_REG;
      Op.reg = gpr(Rm, RmIs64);
      Op.ext = ExtTab[Option];
      Op.shift.type = AARCH64_SFT_LSL; // Capstone always tags arith-extend LSL
      Op.shift.value = Imm3;
    }
    Op.access = CS_AC_READ;
  };

  // CMP/CMN (extended): SUBS/ADDS with Rd==31 (flags only), 2-operand alias.
  if (SetFlags && Rd == 31) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                          IsSub ? AARCH64_INS_SUBS : AARCH64_INS_ADDS);
    Insn.is_alias = true;
    setMnem(Insn, IsSub ? "cmp" : "cmn");
    A.update_flags = true;
    A.op_count = 2;
    setReg(A.operands[0], gprSP(Rn, Is64));
    A.operands[0].access = CS_AC_READ;
    setExtReg(A.operands[1]);
    return true;
  }
  unsigned Id = IsSub ? (SetFlags ? AARCH64_INS_SUBS : AARCH64_INS_SUB)
                      : (SetFlags ? AARCH64_INS_ADDS : AARCH64_INS_ADD);
  cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Id);
  A.update_flags = SetFlags;
  A.op_count = 3;
  setReg(A.operands[0], gprSP(Rd, Is64));
  A.operands[0].access = CS_AC_WRITE;
  setReg(A.operands[1], gprSP(Rn, Is64));
  A.operands[1].access = CS_AC_READ;
  setExtReg(A.operands[2]);
  return true;
}

} // namespace a64native
} // namespace neverd

#endif // NEVERD_DECODE_AARCH64DECODEARITH_H
