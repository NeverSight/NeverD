//===- AArch64DecodeMove.h - Native decode of ADR/ADRP and MOVW -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Operand decode of the AArch64 constant-materialization classes: the
/// PC-relative address forms (ADR/ADRP) and the 16-bit wide-immediate moves
/// (MOVZ/MOVN/MOVK).  Every entry point here is reached from tryDecode in
/// neverd/decode/AArch64DecodeDispatch.h, which owns the class masks; each
/// function assumes its class mask has already matched and returns true when
/// \p Insn is fully populated, false to decline to Capstone.
/// Include neverd/decode/AArch64NativeDecode.h instead of this header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_AARCH64DECODEMOVE_H
#define NEVERD_DECODE_AARCH64DECODEMOVE_H

#include "neverd/Common.h"
#include "neverd/decode/AArch64DecodeOperands.h"

#include <capstone/capstone.h>
#include <cstdint>

namespace neverd {
namespace a64native {

/// ADR / ADRP: op[28:24] == 0b10000, bit31 selects ADR(0)/ADRP(1).
/// Layout: op immlo[30:29] 1 0000 immhi[23:5] Rd[4:0].  No reserved bits, so
/// Capstone accepts the whole (op, immlo, immhi, Rd) space; strict subset is
/// the class mask alone.
inline bool decodeAdr(uint32_t Word, va_t Addr, cs_insn &Insn,
                      cs_detail &Detail) {
  const unsigned Rd = Word & 0x1F;

  const bool IsAdrp = (Word >> 31) & 1u;
  const uint32_t ImmLo = (Word >> 29) & 0x3u;
  const uint32_t ImmHi = (Word >> 5) & 0x7FFFFu; // 19 bits
  int64_t Imm21 = static_cast<int64_t>((ImmHi << 2) | ImmLo);
  Imm21 = (Imm21 ^ 0x100000) - 0x100000; // sign-extend bit 20
  int64_t Target;
  if (IsAdrp)
    Target =
        static_cast<int64_t>(Addr & ~static_cast<va_t>(0xFFF)) + (Imm21 << 12);
  else
    Target = static_cast<int64_t>(Addr) + Imm21;

  cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                        IsAdrp ? AARCH64_INS_ADRP : AARCH64_INS_ADR);
  A.op_count = 2;
  setReg(A.operands[0], gpr(Rd, /*Is64=*/true));
  A.operands[0].access = CS_AC_WRITE;
  setImm(A.operands[1], Target);
  A.operands[1].access = CS_AC_READ;
  return true;
}

/// MOVZ / MOVN / MOVK: bits[28:23] == 0b100101, opc[30:29] selects the
/// variant.  sf=bit31 (0=W,1=X).  hw=bits[22:21] (shift = hw*16); for a
/// 32-bit form hw>=2 is UNALLOCATED (Capstone rejects), so require bit22==0
/// when sf==0 to stay a strict subset.  imm16=bits[20:5].
inline bool decodeMoveWide(uint32_t Word, va_t Addr, cs_insn &Insn,
                           cs_detail &Detail) {
  const unsigned Rd = Word & 0x1F;

  const unsigned Opc = (Word >> 29) & 0x3u; // 00=MOVN,10=MOVZ,11=MOVK
  if (Opc == 1)
    return false; // 01 is UNALLOCATED — let Capstone reject it.
  const bool Is64 = (Word >> 31) & 1u;
  const unsigned Hw = (Word >> 21) & 0x3u;
  if (!Is64 && Hw >= 2)
    return false; // UNALLOCATED 32-bit shift amount.
  const uint32_t Imm16 = (Word >> 5) & 0xFFFFu;
  const unsigned Shift = Hw * 16;

  if (Opc == 3) {
    // MOVK keeps the 16-bit field and shift (Capstone surfaces it non-alias
    // with operands[1] = (imm16, lsl #shift)); the lifter reads imm16 & the
    // shift directly.
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_MOVK);
    A.op_count = 2;
    setReg(A.operands[0], gpr(Rd, Is64));
    A.operands[0].access = CS_AC_READ_WRITE;
    setImm(A.operands[1], static_cast<int64_t>(Imm16));
    A.operands[1].access = CS_AC_READ;
    if (Shift != 0) {
      A.operands[1].shift.type = AARCH64_SFT_LSL;
      A.operands[1].shift.value = Shift;
    }
    return true;
  }

  // MOVZ / MOVN: Capstone folds the shift into the immediate and surfaces an
  // alias (id MOVZ/MOVN, is_alias=1, operands[1] = the final value, no
  // shift), sign-extended to 64 bits *from the operation width* (so a W-form
  // whose top bit is set is stored negative — e.g. `movz w0,#0x8000,lsl#16`
  // is imm -0x80000000).  The lifter's is_alias branch copies that value
  // verbatim, so reproduce it exactly for byte-identical LowIR.
  uint64_t Raw = static_cast<uint64_t>(Imm16) << Shift;
  if (Opc == 0)
    Raw = ~Raw; // MOVN inverts.
  int64_t Final = Is64 ? static_cast<int64_t>(Raw)
                       : static_cast<int64_t>(
                             static_cast<int32_t>(static_cast<uint32_t>(Raw)));
  cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                        Opc == 0 ? AARCH64_INS_MOVN : AARCH64_INS_MOVZ);
  Insn.is_alias = true;
  A.op_count = 2;
  setReg(A.operands[0], gpr(Rd, Is64));
  A.operands[0].access = CS_AC_WRITE;
  setImm(A.operands[1], Final);
  A.operands[1].access = CS_AC_READ;
  return true;
}

} // namespace a64native
} // namespace neverd

#endif // NEVERD_DECODE_AARCH64DECODEMOVE_H
