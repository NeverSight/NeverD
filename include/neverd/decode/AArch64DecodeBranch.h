//===- AArch64DecodeBranch.h - Native decode of branches -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Operand decode of the AArch64 control-flow and system classes: the
/// canonical HINT #0 (NOP), the PC-relative and register branches
/// (B/BL/B.cond/CBZ/CBNZ/TBZ/TBNZ/RET/BR/BLR) and BRK.  Reached from tryDecode
/// in neverd/decode/AArch64DecodeDispatch.h; decodeHintNop assumes its class
/// mask has already matched, while decodeControlFlow is the final dispatch
/// step and carries its own per-form masks, returning false when the word
/// belongs to no covered class at all.
/// Include neverd/decode/AArch64NativeDecode.h instead of this header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_AARCH64DECODEBRANCH_H
#define NEVERD_DECODE_AARCH64DECODEBRANCH_H

#include "neverd/Common.h"
#include "neverd/decode/AArch64DecodeOperands.h"

#include <capstone/capstone.h>
#include <cstdint>

namespace neverd {
namespace a64native {

/// NOP (the canonical HINT #0).  Capstone renders it as the HINT alias
/// with no operands; the lifter's HINT case emits a single NOP for any hint it
/// does not special-case (YIELD/WFE/WFI/SEV/SEVL), which this is not.
inline bool decodeHintNop(uint32_t Word, va_t Addr, cs_insn &Insn,
                          cs_detail &Detail) {
  cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_HINT);
  Insn.is_alias = true;
  A.op_count = 0;
  return true;
}

/// Control flow.  Capstone surfaces the branch target as an absolute VA in
/// operands[0] (or [1]/[2] for the compare/test-and-branch forms) and the
/// condition of a `b.cond` in cc; the lifter reads exactly those.  RET/BR/BLR
/// reuse the fixed-width masks already proven bit-exact in the classifier
/// (the PAC-authenticated RETAA/BRAA/... forms have distinct ids and are
/// excluded by the reserved-bit portion of the mask, so Capstone handles
/// them).
///
/// This is the last class tryDecode tries, so falling through every form here
/// means the word is not covered by the native path at all.
inline bool decodeControlFlow(uint32_t Word, va_t Addr, cs_insn &Insn,
                              cs_detail &Detail) {
  auto sext = [](uint32_t V, unsigned Bits) -> int64_t {
    int64_t X = static_cast<int64_t>(V);
    int64_t M = int64_t(1) << (Bits - 1);
    return (X ^ M) - M;
  };
  auto pcRel = [&](uint32_t ImmField, unsigned Bits) -> int64_t {
    return static_cast<int64_t>(Addr) + (sext(ImmField, Bits) << 2);
  };

  // B (unconditional): bits[31:26]==0b000101.
  if ((Word >> 26) == 0x05u) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_B);
    A.op_count = 1;
    setImm(A.operands[0], pcRel(Word & 0x03FFFFFFu, 26));
    A.operands[0].access = CS_AC_READ;
    return true;
  }
  // BL: bits[31:26]==0b100101.
  if ((Word >> 26) == 0x25u) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_BL);
    A.op_count = 1;
    setImm(A.operands[0], pcRel(Word & 0x03FFFFFFu, 26));
    A.operands[0].access = CS_AC_READ;
    return true;
  }
  // B.cond: bits[31:24]==0x54, bit4==0 (bit4==1 is ARMv8.8 BC.cond, id 53,
  // declined).  cc = bits[3:0].
  if ((Word >> 24) == 0x54u && ((Word >> 4) & 1u) == 0u) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_B);
    A.cc = static_cast<AArch64CC_CondCode>(Word & 0xFu);
    A.op_count = 1;
    setImm(A.operands[0], pcRel((Word >> 5) & 0x7FFFFu, 19));
    A.operands[0].access = CS_AC_READ;
    return true;
  }
  // CBZ / CBNZ: bits[30:25]==0b011010, bit24 selects Z(0)/NZ(1).  sf=bit31.
  if (((Word >> 25) & 0x3Fu) == 0x1Au) {
    const bool Is64 = (Word >> 31) & 1u;
    const bool IsNZ = (Word >> 24) & 1u;
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                          IsNZ ? AARCH64_INS_CBNZ : AARCH64_INS_CBZ);
    A.op_count = 2;
    setReg(A.operands[0], gpr(Word & 0x1Fu, Is64));
    A.operands[0].access = CS_AC_READ;
    setImm(A.operands[1], pcRel((Word >> 5) & 0x7FFFFu, 19));
    A.operands[1].access = CS_AC_READ;
    return true;
  }
  // TBZ / TBNZ: bits[30:25]==0b011011, bit24 selects Z(0)/NZ(1).  The tested
  // register is X when the high bit index (b5=bit31) is set, else W; the bit
  // position is b5:b40 (bits[23:19]); the target is a 14-bit PC-rel offset.
  if (((Word >> 25) & 0x3Fu) == 0x1Bu) {
    const bool IsNZ = (Word >> 24) & 1u;
    const unsigned B5 = (Word >> 31) & 1u;
    const unsigned B40 = (Word >> 19) & 0x1Fu;
    const unsigned BitPos = (B5 << 5) | B40;
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                          IsNZ ? AARCH64_INS_TBNZ : AARCH64_INS_TBZ);
    A.op_count = 3;
    setReg(A.operands[0], gpr(Word & 0x1Fu, /*Is64=*/B5 != 0));
    A.operands[0].access = CS_AC_READ;
    setImm(A.operands[1], static_cast<int64_t>(BitPos));
    A.operands[1].access = CS_AC_READ;
    setImm(A.operands[2], pcRel((Word >> 5) & 0x3FFFu, 14));
    A.operands[2].access = CS_AC_READ;
    return true;
  }
  // RET {Xn}: 0xD65F0000 | (Rn<<5), excluding RETAA/RETAB (bits[11:10]).
  if ((Word & 0xFFFFFC1Fu) == 0xD65F0000u) {
    const unsigned Rn = (Word >> 5) & 0x1Fu;
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_RET);
    if (Rn == 30) {
      Insn.is_alias = true; // Capstone renders the Rn==LR form as `ret`.
      A.op_count = 0;
    } else {
      A.op_count = 1;
      setReg(A.operands[0], gpr(Rn, /*Is64=*/true));
      A.operands[0].access = CS_AC_READ;
    }
    return true;
  }
  // BR Xn: 0xD61F0000 | (Rn<<5), excluding BRAA/BRAB.
  if ((Word & 0xFFFFFC1Fu) == 0xD61F0000u) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_BR);
    A.op_count = 1;
    setReg(A.operands[0], gpr((Word >> 5) & 0x1Fu, /*Is64=*/true));
    A.operands[0].access = CS_AC_READ;
    return true;
  }
  // BLR Xn: 0xD63F0000 | (Rn<<5), excluding BLRAA/BLRAB.
  if ((Word & 0xFFFFFC1Fu) == 0xD63F0000u) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_BLR);
    A.op_count = 1;
    setReg(A.operands[0], gpr((Word >> 5) & 0x1Fu, /*Is64=*/true));
    A.operands[0].access = CS_AC_READ;
    return true;
  }
  // BRK #imm16: 0xD4200000 | (imm16<<5).  Exception-generation opc=001,
  // op2=000, LL=00; the lifter emits the Brk intrinsic and reads no operand,
  // but the single imm operand is surfaced to match Capstone's detail.
  if ((Word & 0xFFE0001Fu) == 0xD4200000u) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_BRK);
    A.op_count = 1;
    setImm(A.operands[0], static_cast<int64_t>((Word >> 5) & 0xFFFFu));
    A.operands[0].access = CS_AC_READ;
    return true;
  }

  return false;
}

} // namespace a64native
} // namespace neverd

#endif // NEVERD_DECODE_AARCH64DECODEBRANCH_H
