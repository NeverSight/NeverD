//===- AArch64DecodeFP.h - Native decode of AArch64 scalar FP -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Operand decode of the AArch64 scalar floating-point classes sharing the
/// bits[30:24]==0b0011110 / bit21==1 encoding space: FMOV (register, general
/// GPR<->FP, 8-bit immediate), the 1-source FABS/FNEG/FSQRT and FCVT, the
/// 2-source arithmetic forms, and FCMP/FCMPE.  Reached from tryDecode in
/// neverd/decode/AArch64DecodeDispatch.h, which owns the class mask; the
/// function assumes the mask has already matched and returns true when \p Insn
/// is fully populated, false to decline to Capstone.
/// Include neverd/decode/AArch64NativeDecode.h instead of this header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_AARCH64DECODEFP_H
#define NEVERD_DECODE_AARCH64DECODEFP_H

#include "neverd/Common.h"
#include "neverd/decode/AArch64DecodeImm.h"
#include "neverd/decode/AArch64DecodeOperands.h"

#include <capstone/capstone.h>
#include <cstdint>

namespace neverd {
namespace a64native {

/// FMOV (scalar): bits[30:24]==0b0011110, bit21==1.  Three sub-forms are
/// taken natively; the top-half (Vn.D[1]) forms (rmode!=0) and every other FP
/// scalar op are declined.  ptype(23:22) selects S(00)/D(01)/H(11); ptype 10
/// is UNALLOCATED here.  Register letters map to the V bank (B/H/S/D/Q).
inline bool decodeScalarFP(uint32_t Word, va_t Addr, cs_insn &Insn,
                           cs_detail &Detail) {
  const unsigned Rd = Word & 0x1F;

  if (((Word >> 29) & 1u) != 0u)
    return false; // S bit must be 0.
  const unsigned Ptype = (Word >> 22) & 3u;
  unsigned FpBytes;
  switch (Ptype) {
  case 0:
    FpBytes = 4;
    break; // S
  case 1:
    FpBytes = 8;
    break; // D
  case 3:
    FpBytes = 2;
    break; // H
  default:
    return false; // ptype 10 UNALLOCATED
  }
  const bool M = (Word >> 31) & 1u;
  const unsigned Rn = (Word >> 5) & 0x1Fu;

  // FMOV (register): M==0, opcode(20:15)==000000, bits[14:10]==0b10000.
  if (!M && ((Word >> 15) & 0x3Fu) == 0u && ((Word >> 10) & 0x1Fu) == 0x10u) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_FMOV);
    A.op_count = 2;
    setReg(A.operands[0], fpReg(Rd, FpBytes));
    A.operands[0].access = CS_AC_WRITE;
    setReg(A.operands[1], fpReg(Rn, FpBytes));
    A.operands[1].access = CS_AC_READ;
    return true;
  }
  // FMOV (scalar immediate): M==0, bits[12:10]==0b100, bits[9:5]==0.
  if (!M && ((Word >> 10) & 0x7u) == 0x4u && ((Word >> 5) & 0x1Fu) == 0u) {
    const unsigned Imm8 = (Word >> 13) & 0xFFu;
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_FMOV);
    A.op_count = 2;
    setReg(A.operands[0], fpReg(Rd, FpBytes));
    A.operands[0].access = CS_AC_WRITE;
    setFp(A.operands[1], vfpExpandImmToDouble(Imm8));
    A.operands[1].access = CS_AC_READ;
    return true;
  }
  // FMOV (general, GPR<->FP): bits[15:10]==0, rmode(20:19)==0, opcode(18:16)
  // in {110 FP->GPR, 111 GPR->FP}.  The valid (sf, ptype) combos are S<->W,
  // D<->X and H<->W/H<->X; the others are UNALLOCATED (declined).
  if (((Word >> 10) & 0x3Fu) == 0u && ((Word >> 19) & 3u) == 0u) {
    const unsigned Opcode = (Word >> 16) & 0x7u;
    if (Opcode != 6u && Opcode != 7u)
      return false;
    const bool Sf = M;
    if ((Ptype == 0 && Sf) || (Ptype == 1 && !Sf))
      return false; // S needs W, D needs X.
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_FMOV);
    A.op_count = 2;
    if (Opcode == 7u) { // GPR -> FP
      setReg(A.operands[0], fpReg(Rd, FpBytes));
      A.operands[0].access = CS_AC_WRITE;
      setReg(A.operands[1], gpr(Rn, Sf));
      A.operands[1].access = CS_AC_READ;
    } else { // FP -> GPR
      setReg(A.operands[0], gpr(Rd, Sf));
      A.operands[0].access = CS_AC_WRITE;
      setReg(A.operands[1], fpReg(Rn, FpBytes));
      A.operands[1].access = CS_AC_READ;
    }
    return true;
  }

  // The remaining scalar FP forms are all M==0 (bit31 is not sf here).
  if (M)
    return false;
  const unsigned Rm = (Word >> 16) & 0x1Fu;

  // FP data-processing (1 source): bits[14:10]==0b10000, opcode(20:15).
  // FMOV(000000) handled above; FABS/FNEG/FSQRT and FCVT are taken here.
  if (((Word >> 10) & 0x1Fu) == 0x10u) {
    const unsigned Opcode = (Word >> 15) & 0x3Fu;
    if (Opcode == 1u || Opcode == 2u || Opcode == 3u) {
      unsigned Id = Opcode == 1u   ? AARCH64_INS_FABS
                    : Opcode == 2u ? AARCH64_INS_FNEG
                                   : AARCH64_INS_FSQRT;
      cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Id);
      A.op_count = 2;
      setReg(A.operands[0], fpReg(Rd, FpBytes));
      A.operands[0].access = CS_AC_WRITE;
      setReg(A.operands[1], fpReg(Rn, FpBytes));
      A.operands[1].access = CS_AC_READ;
      return true;
    }
    // FCVT (precision convert): opcode==0001TT, TT the target ptype
    // (00=S,01=D,11=H) which must differ from the source and not be 10.
    if (((Opcode >> 2) & 0xFu) == 1u) {
      const unsigned Tgt = Opcode & 3u;
      if (Tgt == Ptype || Tgt == 2u)
        return false;
      const unsigned TgtBytes = (Tgt == 0) ? 4u : (Tgt == 1) ? 8u : 2u;
      cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_FCVT);
      A.op_count = 2;
      setReg(A.operands[0], fpReg(Rd, TgtBytes));
      A.operands[0].access = CS_AC_WRITE;
      setReg(A.operands[1], fpReg(Rn, FpBytes));
      A.operands[1].access = CS_AC_READ;
      return true;
    }
    return false; // FRINT* and the rest -> Capstone.
  }

  // FP data-processing (2 source): bits[11:10]==0b10, opcode(15:12).
  if (((Word >> 10) & 0x3u) == 0x2u) {
    const unsigned Opcode = (Word >> 12) & 0xFu;
    static const unsigned Ids[9] = {
        AARCH64_INS_FMUL,   AARCH64_INS_FDIV,   AARCH64_INS_FADD,
        AARCH64_INS_FSUB,   AARCH64_INS_FMAX,   AARCH64_INS_FMIN,
        AARCH64_INS_FMAXNM, AARCH64_INS_FMINNM, AARCH64_INS_FNMUL};
    if (Opcode > 8u)
      return false; // opcodes 9..15 UNALLOCATED.
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Ids[Opcode]);
    A.op_count = 3;
    setReg(A.operands[0], fpReg(Rd, FpBytes));
    A.operands[0].access = CS_AC_WRITE;
    setReg(A.operands[1], fpReg(Rn, FpBytes));
    A.operands[1].access = CS_AC_READ;
    setReg(A.operands[2], fpReg(Rm, FpBytes));
    A.operands[2].access = CS_AC_READ;
    return true;
  }

  // FP compare: bits[15:14]==0b00, bits[13:10]==0b1000, opcode2(4:0) with
  // bits[2:0]==0.  opcode2<3> selects the compare-with-zero form (Capstone
  // surfaces #0.0 as XZR), opcode2<4> the signalling (E) variant.
  if (((Word >> 14) & 0x3u) == 0u && ((Word >> 10) & 0xFu) == 0x8u) {
    const unsigned Opcode2 = Word & 0x1Fu;
    if ((Opcode2 & 0x7u) != 0u)
      return false; // bits[2:0] reserved.
    const bool IsE = (Opcode2 >> 4) & 1u;
    const bool IsZero = (Opcode2 >> 3) & 1u;
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                          IsE ? AARCH64_INS_FCMPE : AARCH64_INS_FCMP);
    A.update_flags = true;
    A.op_count = 2;
    setReg(A.operands[0], fpReg(Rn, FpBytes));
    A.operands[0].access = CS_AC_READ;
    if (IsZero)
      setReg(A.operands[1], AARCH64_REG_XZR); // Capstone reports #0.0 as XZR
    else
      setReg(A.operands[1], fpReg(Rm, FpBytes));
    A.operands[1].access = CS_AC_READ;
    return true;
  }

  return false; // conditional select/compare, FRINT*, etc. -> Capstone.
}

} // namespace a64native
} // namespace neverd

#endif // NEVERD_DECODE_AARCH64DECODEFP_H
