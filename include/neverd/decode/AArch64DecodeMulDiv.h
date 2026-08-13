//===- AArch64DecodeMulDiv.h - Native decode of AArch64 DP ops -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Operand decode of the AArch64 register-only data-processing classes: the
/// 3-source multiply-accumulate forms, the 2-source divides and variable
/// shifts, and the 1-source bit/byte reversal and count-leading forms.  Every
/// entry point here is reached from tryDecode in
/// neverd/decode/AArch64DecodeDispatch.h, which owns the class masks; each
/// function assumes its class mask has already matched and returns true when
/// \p Insn is fully populated, false to decline to Capstone.
/// Include neverd/decode/AArch64NativeDecode.h instead of this header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_AARCH64DECODEMULDIV_H
#define NEVERD_DECODE_AARCH64DECODEMULDIV_H

#include "neverd/Common.h"
#include "neverd/decode/AArch64DecodeOperands.h"

#include <capstone/capstone.h>
#include <cstdint>

namespace neverd {
namespace a64native {

/// Data-processing (3 source): bits[30:24]==0b0011011 (op54==00).  Only
/// the non-widening multiply-accumulate forms are taken natively: MADD/MSUB
/// (op31==0) with their MUL/MNEG aliases (Ra==31), and the high-half
/// SMULH/UMULH (op31==010/110, o0==0), and the widening long forms
/// SMADDL/UMADDL/SMSUBL/UMSUBL (op31==001/101) with their
/// SMULL/UMULL/SMNEGL/UMNEGL aliases (Ra==31).  The remaining 3-source forms
/// are left to Capstone.
inline bool decodeDataProc3Src(uint32_t Word, va_t Addr, cs_insn &Insn,
                               cs_detail &Detail) {
  const bool Is64 = (Word >> 31) & 1u;
  const unsigned Op31 = (Word >> 21) & 0x7u;
  const unsigned O0 = (Word >> 15) & 1u;
  const unsigned Ra = (Word >> 10) & 0x1Fu;
  const unsigned Rm = (Word >> 16) & 0x1Fu;
  const unsigned Rn = (Word >> 5) & 0x1Fu;
  const unsigned Rd = Word & 0x1Fu;
  if (Op31 == 0) {
    // MADD (o0==0) / MSUB (o0==1); Ra==31 gives the MUL / MNEG alias.
    const bool Sub = (O0 != 0);
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                          Sub ? AARCH64_INS_MSUB : AARCH64_INS_MADD);
    if (Ra == 31) {
      Insn.is_alias = true;
      setMnem(Insn, Sub ? "mneg" : "mul");
      A.op_count = 3;
    } else {
      A.op_count = 4;
    }
    setReg(A.operands[0], gpr(Rd, Is64));
    A.operands[0].access = CS_AC_WRITE;
    setReg(A.operands[1], gpr(Rn, Is64));
    A.operands[1].access = CS_AC_READ;
    setReg(A.operands[2], gpr(Rm, Is64));
    A.operands[2].access = CS_AC_READ;
    if (Ra != 31) {
      setReg(A.operands[3], gpr(Ra, Is64));
      A.operands[3].access = CS_AC_READ;
    }
    return true;
  }
  if (O0 == 0 && (Op31 == 2 || Op31 == 6) && Ra == 31) {
    // SMULH (010) / UMULH (110): 64x64->high-64, only the X form exists.
    if (!Is64)
      return false;
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                          Op31 == 2 ? AARCH64_INS_SMULH : AARCH64_INS_UMULH);
    A.op_count = 3;
    setReg(A.operands[0], gpr(Rd, true));
    A.operands[0].access = CS_AC_WRITE;
    setReg(A.operands[1], gpr(Rn, true));
    A.operands[1].access = CS_AC_READ;
    setReg(A.operands[2], gpr(Rm, true));
    A.operands[2].access = CS_AC_READ;
    return true;
  }
  // Widening multiply-add long: op31==001 signed / 101 unsigned, o0 picks
  // add(0)/sub(1).  sf is fixed 1 (sf==0 is UNALLOCATED).  Rd/Rn/Rm are the
  // zero-register form; Rn/Rm are the 32-bit W views, Rd/Ra the 64-bit X.
  // Ra==31 gives the SMULL/UMULL (add) or SMNEGL/UMNEGL (sub) alias, which
  // Capstone surfaces with the same id, is_alias=1, op_count=3.
  if ((Op31 == 1 || Op31 == 5) && Is64) {
    const bool Unsigned = (Op31 == 5);
    const bool Sub = (O0 != 0);
    unsigned Id = Sub ? (Unsigned ? AARCH64_INS_UMSUBL : AARCH64_INS_SMSUBL)
                      : (Unsigned ? AARCH64_INS_UMADDL : AARCH64_INS_SMADDL);
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Id);
    if (Ra == 31) {
      Insn.is_alias = true;
      A.op_count = 3;
    } else {
      A.op_count = 4;
    }
    setReg(A.operands[0], gpr(Rd, /*Is64=*/true));
    A.operands[0].access = CS_AC_WRITE;
    setReg(A.operands[1], gpr(Rn, /*Is64=*/false));
    A.operands[1].access = CS_AC_READ;
    setReg(A.operands[2], gpr(Rm, /*Is64=*/false));
    A.operands[2].access = CS_AC_READ;
    if (Ra != 31) {
      setReg(A.operands[3], gpr(Ra, /*Is64=*/true));
      A.operands[3].access = CS_AC_READ;
    }
    return true;
  }
  return false; // remaining 3-source forms -> Capstone.
}

/// Data-processing (2 source): bits[30:21]==0b0011010110.  Integer divides
/// (SDIV opcode 000011, UDIV 000010) and the variable shifts LSLV/LSRV/ASRV/
/// RORV (opcodes 001000..001011, which Capstone renders as LSL/LSR/ASR/ROR
/// with a register shift); CRC/PACGA and the rest are left to Capstone.
inline bool decodeDataProc2Src(uint32_t Word, va_t Addr, cs_insn &Insn,
                               cs_detail &Detail) {
  const unsigned Op2 = (Word >> 10) & 0x3Fu;
  const bool Is64 = (Word >> 31) & 1u;
  const unsigned Rm = (Word >> 16) & 0x1Fu;
  const unsigned Rn = (Word >> 5) & 0x1Fu;
  if (Op2 == 2 || Op2 == 3) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                          Op2 == 3 ? AARCH64_INS_SDIV : AARCH64_INS_UDIV);
    A.op_count = 3;
    setReg(A.operands[0], gpr(Word & 0x1Fu, Is64));
    A.operands[0].access = CS_AC_WRITE;
    setReg(A.operands[1], gpr(Rn, Is64));
    A.operands[1].access = CS_AC_READ;
    setReg(A.operands[2], gpr(Rm, Is64));
    A.operands[2].access = CS_AC_READ;
    return true;
  }
  if (Op2 >= 8 && Op2 <= 11) {
    // LSLV/LSRV/ASRV/RORV.  The lifter re-derives Rn/Rm from the raw bytes
    // for the 2-operand register-shift form, so only the id and a matching
    // register-shift descriptor on operand[1] are needed.
    static const unsigned Id[4] = {AARCH64_INS_LSL, AARCH64_INS_LSR,
                                   AARCH64_INS_ASR, AARCH64_INS_ROR};
    static const aarch64_shifter Sft[4] = {
        AARCH64_SFT_LSL_REG, AARCH64_SFT_LSR_REG, AARCH64_SFT_ASR_REG,
        AARCH64_SFT_ROR_REG};
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Id[Op2 - 8]);
    A.op_count = 2;
    setReg(A.operands[0], gpr(Word & 0x1Fu, Is64));
    A.operands[0].access = CS_AC_WRITE;
    setReg(A.operands[1], gpr(Rn, Is64));
    A.operands[1].access = CS_AC_READ;
    A.operands[1].shift.type = Sft[Op2 - 8];
    A.operands[1].shift.value = static_cast<unsigned>(gpr(Rm, Is64));
    return true;
  }
  return false;
}

/// Data-processing (1 source): bits[30:21]==0b1011010110, opcode2(20:16)==0
/// (else UNALLOCATED).  opcode(15:10) selects RBIT/REV16/REV32/REV/CLZ/CLS;
/// the FEAT_CSSC additions (CTZ/CNT/ABS, opcode>=6) and the PAC forms
/// (opcode2!=0) are left to Capstone.  All are 2-operand `Rd, Rn`.
inline bool decodeDataProc1Src(uint32_t Word, va_t Addr, cs_insn &Insn,
                               cs_detail &Detail) {
  const bool Is64 = (Word >> 31) & 1u;
  const unsigned Opcode = (Word >> 10) & 0x3Fu;
  const unsigned Rn = (Word >> 5) & 0x1Fu;
  unsigned Id;
  switch (Opcode) {
  case 0:
    Id = AARCH64_INS_RBIT;
    break;
  case 1:
    Id = AARCH64_INS_REV16;
    break;
  case 2:
    Id = Is64 ? AARCH64_INS_REV32 : AARCH64_INS_REV;
    break;
  case 3:
    if (!Is64)
      return false; // opcode 000011 is UNALLOCATED for the 32-bit form.
    Id = AARCH64_INS_REV;
    break;
  case 4:
    Id = AARCH64_INS_CLZ;
    break;
  case 5:
    Id = AARCH64_INS_CLS;
    break;
  default:
    return false; // CTZ/CNT/ABS (FEAT_CSSC) etc. -> Capstone.
  }
  cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Id);
  A.op_count = 2;
  setReg(A.operands[0], gpr(Word & 0x1Fu, Is64));
  A.operands[0].access = CS_AC_WRITE;
  setReg(A.operands[1], gpr(Rn, Is64));
  A.operands[1].access = CS_AC_READ;
  return true;
}

} // namespace a64native
} // namespace neverd

#endif // NEVERD_DECODE_AARCH64DECODEMULDIV_H
