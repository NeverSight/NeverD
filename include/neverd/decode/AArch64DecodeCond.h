//===- AArch64DecodeCond.h - Native decode of AArch64 cond ops -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Operand decode of the AArch64 flag-consuming classes: conditional compare
/// (CCMP/CCMN, register and immediate) and conditional select (CSEL/CSINC/
/// CSINV/CSNEG) with the cset/csetm/cinc/cinv/cneg aliases.  Every entry point
/// here is reached from tryDecode in neverd/decode/AArch64DecodeDispatch.h,
/// which owns the class masks; each function assumes its class mask has
/// already matched and returns true when \p Insn is fully populated, false to
/// decline to Capstone.
/// Include neverd/decode/AArch64NativeDecode.h instead of this header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_AARCH64DECODECOND_H
#define NEVERD_DECODE_AARCH64DECODECOND_H

#include "neverd/Common.h"
#include "neverd/decode/AArch64DecodeOperands.h"

#include <capstone/capstone.h>
#include <cstdint>

namespace neverd {
namespace a64native {

/// Conditional compare (register & immediate): bits[28:21]==0b11010010,
/// S(bit29)==1, o3(bit10)==0, o1(bit4)==0 (else UNALLOCATED).  op(bit30)
/// selects CCMN(0)/CCMP(1); o2(bit11) selects register(0)/immediate(1).
/// Layout: sf op 1 11010010 Rm/imm5 cond 0 0 Rn 0 nzcv.  The lifter reads
/// operands[0]=Rn, operands[1]=Rm|#imm5, operands[2]=#nzcv and cc.
inline bool decodeCondCompare(uint32_t Word, va_t Addr, cs_insn &Insn,
                              cs_detail &Detail) {
  if (((Word >> 10) & 1u) != 0u || (Word & 0x10u) != 0u)
    return false; // o3/o1 reserved bits set -> UNALLOCATED.
  const bool Is64 = (Word >> 31) & 1u;
  const bool IsCmp = (Word >> 30) & 1u;
  const bool ImmForm = (Word >> 11) & 1u;
  const unsigned Cond = (Word >> 12) & 0xFu;
  const unsigned Rn = (Word >> 5) & 0x1Fu;
  const unsigned Nzcv = Word & 0xFu;
  cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                        IsCmp ? AARCH64_INS_CCMP : AARCH64_INS_CCMN);
  A.update_flags = true;
  A.cc = static_cast<AArch64CC_CondCode>(Cond);
  A.op_count = 3;
  setReg(A.operands[0], gpr(Rn, Is64));
  A.operands[0].access = CS_AC_READ;
  if (ImmForm) {
    setImm(A.operands[1], static_cast<int64_t>((Word >> 16) & 0x1Fu));
  } else {
    setReg(A.operands[1], gpr((Word >> 16) & 0x1Fu, Is64));
  }
  A.operands[1].access = CS_AC_READ;
  setImm(A.operands[2], static_cast<int64_t>(Nzcv));
  A.operands[2].access = CS_AC_READ;
  return true;
}

/// Conditional select: bits[28:21]==0b11010100, S(bit29)==0, bit11==0.
/// op(bit30):op2(bit10) select CSEL/CSINC/CSINV/CSNEG.  Capstone renders the
/// CSET/CSETM (Rm==Rn==31), CINC/CINV (Rm==Rn!=31) and CNEG (Rm==Rn) aliases
/// when the condition is not AL/NV, reporting the *inverted* condition; the
/// lifter reads is_alias + op_count + cc to reconstruct them, so reproduce
/// them exactly.
inline bool decodeCondSelect(uint32_t Word, va_t Addr, cs_insn &Insn,
                             cs_detail &Detail) {
  const bool Is64 = (Word >> 31) & 1u;
  const unsigned Variant = (((Word >> 30) & 1u) << 1) | ((Word >> 10) & 1u);
  const unsigned Cond = (Word >> 12) & 0xFu;
  const unsigned Rm = (Word >> 16) & 0x1Fu;
  const unsigned Rn = (Word >> 5) & 0x1Fu;
  const unsigned Rd = Word & 0x1Fu;
  static const unsigned Ids[4] = {AARCH64_INS_CSEL, AARCH64_INS_CSINC,
                                  AARCH64_INS_CSINV, AARCH64_INS_CSNEG};
  const bool CondAliasable = (Cond >> 1) != 0x7u; // not AL(1110)/NV(1111)
  const unsigned InvCond = Cond ^ 1u;

  // CSET/CSETM: CSINC/CSINV, Rm==Rn==31.
  if (CondAliasable && Rm == 31 && Rn == 31 && (Variant == 1 || Variant == 2)) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Ids[Variant]);
    Insn.is_alias = true;
    setMnem(Insn, Variant == 1 ? "cset" : "csetm");
    A.cc = static_cast<AArch64CC_CondCode>(InvCond);
    A.op_count = 1;
    setReg(A.operands[0], gpr(Rd, Is64));
    A.operands[0].access = CS_AC_WRITE;
    return true;
  }
  // CINC/CINV: CSINC/CSINV, Rm==Rn (not 31).  CNEG: CSNEG, Rm==Rn.
  const bool PairEq = (Rm == Rn);
  if (CondAliasable && PairEq &&
      ((Variant == 1 && Rn != 31) || (Variant == 2 && Rn != 31) ||
       Variant == 3)) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Ids[Variant]);
    Insn.is_alias = true;
    setMnem(Insn, Variant == 1 ? "cinc" : Variant == 2 ? "cinv" : "cneg");
    A.cc = static_cast<AArch64CC_CondCode>(InvCond);
    A.op_count = 2;
    setReg(A.operands[0], gpr(Rd, Is64));
    A.operands[0].access = CS_AC_WRITE;
    setReg(A.operands[1], gpr(Rn, Is64));
    A.operands[1].access = CS_AC_READ;
    return true;
  }
  // Canonical 4-field, 3-operand form.
  cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Ids[Variant]);
  A.cc = static_cast<AArch64CC_CondCode>(Cond);
  A.op_count = 3;
  setReg(A.operands[0], gpr(Rd, Is64));
  A.operands[0].access = CS_AC_WRITE;
  setReg(A.operands[1], gpr(Rn, Is64));
  A.operands[1].access = CS_AC_READ;
  setReg(A.operands[2], gpr(Rm, Is64));
  A.operands[2].access = CS_AC_READ;
  return true;
}

} // namespace a64native
} // namespace neverd

#endif // NEVERD_DECODE_AARCH64DECODECOND_H
