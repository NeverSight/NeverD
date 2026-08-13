//===- AArch64DecodeOperands.h - Native decode operand builders -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Register-id mapping and `cs_insn` / `cs_aarch64_op` population helpers
/// shared by every instruction class of the Capstone-free AArch64 operand
/// decoder.  Include neverd/decode/AArch64NativeDecode.h instead of this
/// header; it documents the correctness contract these helpers implement.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_AARCH64DECODEOPERANDS_H
#define NEVERD_DECODE_AARCH64DECODEOPERANDS_H

#include "neverd/Common.h"

#include <capstone/capstone.h>
#include <cstdint>
#include <cstring>

namespace neverd {
namespace a64native {

/// Capstone register id for GPR index \p Idx in 0..30 at the given width.
///
/// Capstone's `aarch64_reg` enum is NOT contiguous: X0..X28 run 238..266 but
/// X29/X30 alias the frame/link registers (FP=2, LR=4), and the zero/stack
/// registers sit far away (XZR=10, WZR=9, SP=6, WSP=8).  The lifter's
/// mapCapstoneReg keys on these exact ids, so the mapping must be reproduced
/// rather than computed as a base+index (which mis-decoded x29/x30).  Index 31
/// is handled by the callers (gpr / gprSP) because it means either the zero
/// register or the stack pointer depending on the instruction class.
inline aarch64_reg gpr0to30(unsigned Idx, bool Is64) {
  if (Is64) {
    if (Idx <= 28)
      return static_cast<aarch64_reg>(AARCH64_REG_X0 + Idx);
    if (Idx == 29)
      return AARCH64_REG_X29; // == AARCH64_REG_FP
    return AARCH64_REG_X30;   // 30, == AARCH64_REG_LR
  }
  return static_cast<aarch64_reg>(AARCH64_REG_W0 + Idx);
}

/// GPR id where index 31 is the zero register (the logical/move/branch/most-ALU
/// interpretation of Rd/Rn==31).
inline aarch64_reg gpr(unsigned Idx, bool Is64) {
  if (Idx == 31)
    return Is64 ? AARCH64_REG_XZR : AARCH64_REG_WZR;
  return gpr0to30(Idx, Is64);
}

/// GPR id where index 31 is the stack pointer (the add/sub-immediate and
/// base-register-of-a-memory-operand interpretation of Rd/Rn==31).
inline aarch64_reg gprSP(unsigned Idx, bool Is64) {
  if (Idx == 31)
    return Is64 ? AARCH64_REG_SP : AARCH64_REG_WSP;
  return gpr0to30(Idx, Is64);
}

/// SIMD/FP register id for index \p Idx at a byte width of \p Bytes
/// (1=B,2=H,4=S,8=D,16=Q).  Each bank is 32 contiguous ids in the Capstone
/// enum, so base + index is valid within a bank.
inline aarch64_reg fpReg(unsigned Idx, unsigned Bytes) {
  aarch64_reg Base;
  switch (Bytes) {
  case 1:
    Base = AARCH64_REG_B0;
    break;
  case 2:
    Base = AARCH64_REG_H0;
    break;
  case 4:
    Base = AARCH64_REG_S0;
    break;
  case 8:
    Base = AARCH64_REG_D0;
    break;
  default:
    Base = AARCH64_REG_Q0;
    break; // 16
  }
  return static_cast<aarch64_reg>(Base + Idx);
}

/// Zero every operand field the lifter may read, so a synthesized operand
/// never inherits stale detail from a prior decode into the same buffer.
inline void clearOp(cs_aarch64_op &Op) {
  Op = cs_aarch64_op{};
  Op.vector_index = -1;
  Op.vas = AARCH64LAYOUT_INVALID;
  Op.shift.type = AARCH64_SFT_INVALID;
  Op.shift.value = 0;
  Op.ext = AARCH64_EXT_INVALID;
  Op.type = AARCH64_OP_INVALID;
}

inline void setReg(cs_aarch64_op &Op, aarch64_reg Reg) {
  clearOp(Op);
  Op.type = AARCH64_OP_REG;
  Op.reg = Reg;
}

inline void setImm(cs_aarch64_op &Op, int64_t Imm) {
  clearOp(Op);
  Op.type = AARCH64_OP_IMM;
  Op.imm = Imm;
}

/// A floating-point immediate operand.  Capstone surfaces the decoded FP value
/// as a double in operands[].fp regardless of the instruction's precision, and
/// the lifter re-narrows it to the destination width; \p V must therefore be
/// the exact value Capstone would report.
inline void setFp(cs_aarch64_op &Op, double V) {
  clearOp(Op);
  Op.type = AARCH64_OP_FP;
  Op.fp = V;
}

/// Map the 2-bit shift-type field of a shifted-register operand to the capstone
/// shifter enum (0=LSL,1=LSR,2=ASR,3=ROR).  ROR is only legal for the logical
/// shifted-register class; add/sub callers reject field value 3 before calling.
inline aarch64_shifter shiftKind(unsigned Field) {
  switch (Field & 3u) {
  case 0:
    return AARCH64_SFT_LSL;
  case 1:
    return AARCH64_SFT_LSR;
  case 2:
    return AARCH64_SFT_ASR;
  default:
    return AARCH64_SFT_ROR;
  }
}

/// A shifted register source operand `<Rm>{, <shift> #amount}`.  A zero amount
/// is surfaced with no shift (matches capstone, and the lifter treats a zero
/// shift value as none).
inline void setRegShift(cs_aarch64_op &Op, aarch64_reg Reg, unsigned ShiftField,
                        unsigned Amount) {
  clearOp(Op);
  Op.type = AARCH64_OP_REG;
  Op.reg = Reg;
  if (Amount != 0) {
    Op.shift.type = shiftKind(ShiftField);
    Op.shift.value = Amount;
  }
}

inline void setMnem(cs_insn &Insn, const char *M) {
  std::strncpy(Insn.mnemonic, M, sizeof(Insn.mnemonic) - 1);
  Insn.mnemonic[sizeof(Insn.mnemonic) - 1] = '\0';
}

/// A `[base{, #disp}]` memory operand (no index register).  \p Base is a
/// 64-bit register id.
inline void setMem(cs_aarch64_op &Op, aarch64_reg Base, int64_t Disp) {
  clearOp(Op);
  Op.type = AARCH64_OP_MEM;
  Op.mem.base = Base;
  Op.mem.index = AARCH64_REG_INVALID;
  Op.mem.disp = static_cast<int32_t>(Disp);
}

/// Begin a synthesized instruction: fill the width-independent `cs_insn`
/// scaffolding and reset the aarch64 detail.  \p Detail must outlive \p Insn.
inline cs_aarch64 &begin(cs_insn &Insn, cs_detail &Detail, uint32_t Word,
                         va_t Addr, unsigned Id) {
  Insn.id = Id;
  Insn.alias_id = 0;
  Insn.address = static_cast<uint64_t>(Addr);
  Insn.size = 4;
  Insn.bytes[0] = static_cast<uint8_t>(Word);
  Insn.bytes[1] = static_cast<uint8_t>(Word >> 8);
  Insn.bytes[2] = static_cast<uint8_t>(Word >> 16);
  Insn.bytes[3] = static_cast<uint8_t>(Word >> 24);
  Insn.mnemonic[0] = '\0';
  Insn.op_str[0] = '\0';
  Insn.is_alias = false;
  Insn.detail = &Detail;

  // Reset the cs_detail fields the lifter reads outside the aarch64 sub-struct.
  // Decoder reuses one InsnBuf across decodes, so a prior Capstone decode may
  // have left writeback set — a memory-class lift would then emit a spurious
  // base-register update.
  Detail.writeback = false;

  cs_aarch64 &A = Detail.aarch64;
  // Reset only the header scalar fields, NOT the 16-slot operands[] array.
  // `A = cs_aarch64{}` bulk-zeroed ~1.1 KB per decode (16 * 72-byte operands)
  // for classes that use 2–4 operands — the dominant cost of the fast path
  // (a field-extract with no wide intermediate runs ~60x faster).  Every
  // operand in [0, op_count) is fully initialized by the setReg/setImm/setMem/
  // setRegShift helpers below (each calls clearOp, which zeroes the slot and
  // writes the -1/INVALID sentinels the lifter keys on), and the lifter never
  // reads past op_count, so a stale slot from a previous decode can never leak
  // into the lift.  The strict-subset + lift-parity invariants are unchanged
  // and remain locked by AArch64_NativeDecodeParityTests.
  A.cc = AArch64CC_Invalid;
  A.update_flags = false;
  A.post_index = false;
  A.is_doing_sme = false;
  A.op_count = 0;
  return A;
}

} // namespace a64native
} // namespace neverd

#endif // NEVERD_DECODE_AARCH64DECODEOPERANDS_H
