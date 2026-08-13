//===- AArch64DecodeLoadStore.h - Native decode of AArch64 ldst -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Operand decode of every AArch64 load/store class the native path covers:
/// the scaled unsigned-offset form, the register-offset and unscaled /
/// pre-index / post-index imm9 forms, and load/store pair — each in both the
/// integer and the FP/SIMD (V==1) flavour.  Every entry point here is reached
/// from tryDecode in neverd/decode/AArch64DecodeDispatch.h, which owns the
/// class masks; each function assumes its class mask has already matched and
/// returns true when \p Insn is fully populated, false to decline to Capstone.
/// Include neverd/decode/AArch64NativeDecode.h instead of this header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_AARCH64DECODELOADSTORE_H
#define NEVERD_DECODE_AARCH64DECODELOADSTORE_H

#include "neverd/Common.h"
#include "neverd/decode/AArch64DecodeOperands.h"

#include <capstone/capstone.h>
#include <cstdint>

namespace neverd {
namespace a64native {

/// Decode the (size, opc) fields of a single integer load/store register into
/// the Capstone instruction id, whether it stores, and whether the transfer
/// register is 64-bit.  Returns false for the UNALLOCATED / PRFM slots the
/// native path must decline.  Shared by every addressing mode (unsigned
/// offset, register offset, pre/post-index) since they differ only in how the
/// address operand is formed, not in the transfer register or width.
inline bool ldstIntId(unsigned Size, unsigned Opc, unsigned &Id, bool &IsStore,
                      bool &RtIs64) {
  IsStore = false;
  RtIs64 = false;
  switch (Size) {
  case 0:
    if (Opc == 0) {
      Id = AARCH64_INS_STRB;
      IsStore = true;
    } else if (Opc == 1)
      Id = AARCH64_INS_LDRB;
    else {
      Id = AARCH64_INS_LDRSB;
      RtIs64 = (Opc == 2);
    }
    return true;
  case 1:
    if (Opc == 0) {
      Id = AARCH64_INS_STRH;
      IsStore = true;
    } else if (Opc == 1)
      Id = AARCH64_INS_LDRH;
    else {
      Id = AARCH64_INS_LDRSH;
      RtIs64 = (Opc == 2);
    }
    return true;
  case 2:
    if (Opc == 0) {
      Id = AARCH64_INS_STR;
      IsStore = true;
    } else if (Opc == 1)
      Id = AARCH64_INS_LDR;
    else if (Opc == 2) {
      Id = AARCH64_INS_LDRSW;
      RtIs64 = true;
    } else
      return false;
    return true;
  default: // size == 3
    if (Opc == 0) {
      Id = AARCH64_INS_STR;
      IsStore = true;
      RtIs64 = true;
    } else if (Opc == 1) {
      Id = AARCH64_INS_LDR;
      RtIs64 = true;
    } else
      return false; // PRFM / UNALLOCATED
    return true;
  }
}

/// Load/store register (unsigned immediate offset): the dominant memory
/// form.  bits[29:27]==0b111, bits[25:24]==0b01, V(bit26)==0 (integer only;
/// FP/SIMD V==1 is declined so Capstone maps the S/D/Q register).  Layout:
/// size(2) 111 V 01 opc(2) imm12 Rn Rt.  The byte displacement is the encoded
/// imm12 scaled by the access size (Capstone surfaces the scaled value); Rn is
/// the SP-form base, Rt the zero-register-form transfer register.
inline bool decodeLdStUnsignedImm(uint32_t Word, va_t Addr, cs_insn &Insn,
                                  cs_detail &Detail) {
  const unsigned Size = (Word >> 30) & 3u;
  const unsigned Opc = (Word >> 22) & 3u;
  const uint32_t Imm12 = (Word >> 10) & 0xFFFu;
  const unsigned Rn = (Word >> 5) & 0x1Fu;

  unsigned Id = 0;
  bool IsStore = false, RtIs64 = false;
  if (!ldstIntId(Size, Opc, Id, IsStore, RtIs64))
    return false;

  const int64_t Disp = static_cast<int64_t>(Imm12) << Size;
  cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Id);
  Insn.is_alias = (Imm12 == 0); // Capstone aliases the zero-offset form.
  A.op_count = 2;
  setReg(A.operands[0], gpr(Word & 0x1Fu, RtIs64));
  A.operands[0].access = IsStore ? CS_AC_READ : CS_AC_WRITE;
  setMem(A.operands[1], gprSP(Rn, /*Is64=*/true), Disp);
  A.operands[1].access = IsStore ? CS_AC_WRITE : CS_AC_READ;
  return true;
}

/// FP/SIMD load/store register (unsigned immediate offset): same shape as
/// the integer form but V(bit26)==1, transferring a B/H/S/D/Q register.  The
/// access width is 1<<size for the scalar forms; opc bit23 selects the 128-bit
/// Q form (size must be 00).  The lifter's LDR/STR handler reads the FP
/// destination register and load size straight from this, so it lifts
/// identically to Capstone.
inline bool decodeLdStUnsignedImmFP(uint32_t Word, va_t Addr, cs_insn &Insn,
                                    cs_detail &Detail) {
  const unsigned Size = (Word >> 30) & 3u;
  const unsigned Opc = (Word >> 22) & 3u;
  const bool IsQ = (Opc >> 1) & 1u; // bit23 extends the width to 128
  const bool IsLoad = Opc & 1u;     // bit22
  unsigned Width, Scale;
  if (IsQ) {
    if (Size != 0)
      return false; // size!=00 with the Q bit is UNALLOCATED.
    Width = 16;
    Scale = 4;
  } else {
    Width = 1u << Size; // 1,2,4,8 -> B,H,S,D
    Scale = Size;
  }
  const uint32_t Imm12 = (Word >> 10) & 0xFFFu;
  const int64_t Disp = static_cast<int64_t>(Imm12) << Scale;
  const unsigned Rn = (Word >> 5) & 0x1Fu;
  cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                        IsLoad ? AARCH64_INS_LDR : AARCH64_INS_STR);
  Insn.is_alias = (Imm12 == 0);
  A.op_count = 2;
  setReg(A.operands[0], fpReg(Word & 0x1Fu, Width));
  A.operands[0].access = IsLoad ? CS_AC_WRITE : CS_AC_READ;
  setMem(A.operands[1], gprSP(Rn, /*Is64=*/true), Disp);
  A.operands[1].access = IsLoad ? CS_AC_READ : CS_AC_WRITE;
  return true;
}

/// Load/store register (register offset and unscaled/pre/post-index):
/// bits[29:27]==0b111, bits[25:24]==0b00, V(bit26)==0.  Sub-form by bit21 and
/// bits[11:10]:
///   * bit21==1, [11:10]==0b10 : register offset  (scaled by option/S)
///   * bit21==0, [11:10]==0b00 : unscaled signed imm9  (LDUR/STUR family)
///   * bit21==0, [11:10]==0b01 : post-index  (LDR family, writeback)
///   * bit21==0, [11:10]==0b11 : pre-index   (LDR family, writeback)
/// The unprivileged ([11:10]==0b10, bit21==0) and atomic/PAC (bit21==1,
/// [11:10]!=0b10) forms are declined.
inline bool decodeLdStImm9OrReg(uint32_t Word, va_t Addr, cs_insn &Insn,
                                cs_detail &Detail) {
  const unsigned Size = (Word >> 30) & 3u;
  const unsigned Opc = (Word >> 22) & 3u;
  const unsigned Rn = (Word >> 5) & 0x1Fu;
  const unsigned Op4 = (Word >> 10) & 3u; // bits[11:10]
  const bool Bit21 = (Word >> 21) & 1u;

  if (Bit21) {
    // Register offset: option=bits[15:13], S=bit12, Rm=bits[20:16].
    if (Op4 != 2)
      return false; // only [11:10]==10 is the register-offset form.
    const unsigned Option = (Word >> 13) & 0x7u;
    if (((Option >> 1) & 1u) == 0u)
      return false; // option 0b0x0 / 0b0x1 with bit1==0 is reserved.
    unsigned Id = 0;
    bool IsStore = false, RtIs64 = false;
    if (!ldstIntId(Size, Opc, Id, IsStore, RtIs64))
      return false;
    const bool IdxIs64 = (Option & 1u) != 0u; // UXTW/SXTW use the W view.
    const unsigned Rm = (Word >> 16) & 0x1Fu;
    const bool S = (Word >> 12) & 1u;

    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Id);
    // Capstone renders the plain `[Xn, Xm]` (option 0b011, S==0) as an alias.
    Insn.is_alias = (Option == 3 && !S);
    A.op_count = 2;
    setReg(A.operands[0], gpr(Word & 0x1Fu, RtIs64));
    A.operands[0].access = IsStore ? CS_AC_READ : CS_AC_WRITE;
    clearOp(A.operands[1]);
    A.operands[1].type = AARCH64_OP_MEM;
    A.operands[1].mem.base = gprSP(Rn, /*Is64=*/true);
    A.operands[1].mem.index = gpr(Rm, IdxIs64);
    A.operands[1].mem.disp = 0;
    // ext: UXTW(010)->UXTW, SXTW(110)->SXTW, SXTX(111)->SXTX; the plain LSL
    // form (011, UXTX) carries no extend.  Capstone surfaces the LSL scale
    // shift only when S is set (shift amount = size).
    switch (Option) {
    case 2:
      A.operands[1].ext = AARCH64_EXT_UXTW;
      break;
    case 6:
      A.operands[1].ext = AARCH64_EXT_SXTW;
      break;
    case 7:
      A.operands[1].ext = AARCH64_EXT_SXTX;
      break;
    default:
      A.operands[1].ext = AARCH64_EXT_INVALID;
      break; // option 3 (LSL)
    }
    if (S) {
      A.operands[1].shift.type = AARCH64_SFT_LSL;
      A.operands[1].shift.value = Size; // scale = log2(access size)
    }
    A.operands[1].access = IsStore ? CS_AC_WRITE : CS_AC_READ;
    return true;
  }

  // imm9-addressed forms (unscaled / pre / post).  imm9=bits[20:12] signed.
  if (Op4 == 2)
    return false; // unprivileged LDTR/STTR — declined.
  int64_t Imm9 = static_cast<int64_t>((Word >> 12) & 0x1FFu);
  Imm9 = (Imm9 ^ 0x100) - 0x100; // sign-extend bit 8

  if (Op4 == 0) {
    // Unscaled (LDUR/STUR family): the store/width logic mirrors the scaled
    // family exactly (LDURSB/SH have 64/32-bit forms, LDURSW is 64-bit), only
    // the instruction id differs and there is no writeback.
    unsigned Scaled = 0;
    bool IsStore = false, RtIs64 = false;
    if (!ldstIntId(Size, Opc, Scaled, IsStore, RtIs64))
      return false; // PRFUM / UNALLOCATED
    static const struct {
      unsigned Scaled, Unscaled;
    } Map[] = {{AARCH64_INS_STRB, AARCH64_INS_STURB},
               {AARCH64_INS_LDRB, AARCH64_INS_LDURB},
               {AARCH64_INS_LDRSB, AARCH64_INS_LDURSB},
               {AARCH64_INS_STRH, AARCH64_INS_STURH},
               {AARCH64_INS_LDRH, AARCH64_INS_LDURH},
               {AARCH64_INS_LDRSH, AARCH64_INS_LDURSH},
               {AARCH64_INS_STR, AARCH64_INS_STUR},
               {AARCH64_INS_LDR, AARCH64_INS_LDUR},
               {AARCH64_INS_LDRSW, AARCH64_INS_LDURSW}};
    unsigned Id = Scaled;
    for (const auto &E : Map)
      if (E.Scaled == Scaled) {
        Id = E.Unscaled;
        break;
      }
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Id);
    A.op_count = 2;
    setReg(A.operands[0], gpr(Word & 0x1Fu, RtIs64));
    A.operands[0].access = IsStore ? CS_AC_READ : CS_AC_WRITE;
    setMem(A.operands[1], gprSP(Rn, /*Is64=*/true), Imm9);
    A.operands[1].access = IsStore ? CS_AC_WRITE : CS_AC_READ;
    return true;
  }

  // Pre-index (0b11) / post-index (0b01): same ids as the scaled family,
  // with writeback (post_index for 0b01).
  unsigned Id = 0;
  bool IsStore = false, RtIs64 = false;
  if (!ldstIntId(Size, Opc, Id, IsStore, RtIs64))
    return false;
  cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Id);
  A.op_count = 2;
  setReg(A.operands[0], gpr(Word & 0x1Fu, RtIs64));
  A.operands[0].access = IsStore ? CS_AC_READ : CS_AC_WRITE;
  setMem(A.operands[1], gprSP(Rn, /*Is64=*/true), Imm9);
  A.operands[1].access = IsStore ? CS_AC_WRITE : CS_AC_READ;
  Detail.writeback = true;
  A.post_index = (Op4 == 1);
  return true;
}

/// FP/SIMD load/store register (register-offset, unscaled, pre/post-index):
/// same shape as the integer forms above but V(bit26)==1, transferring a
/// B/H/S/D/Q register.  Access width is 1<<size, or 16 when the Q bit
/// (opc<23>) is set (size must be 00).  Unscaled uses the LDUR/STUR ids; the
/// register-offset and pre/post-index forms use LDR/STR (with writeback), just
/// as Capstone reports them; the lifter reads the FP register and load size
/// straight from this, so it lifts identically.
inline bool decodeLdStImm9OrRegFP(uint32_t Word, va_t Addr, cs_insn &Insn,
                                  cs_detail &Detail) {
  const unsigned Size = (Word >> 30) & 3u;
  const unsigned Opc = (Word >> 22) & 3u;
  const bool IsQ = (Opc >> 1) & 1u; // bit23 extends the width to 128
  const bool IsLoad = Opc & 1u;     // bit22
  unsigned Width, Scale;
  if (IsQ) {
    if (Size != 0)
      return false; // size!=00 with the Q bit is UNALLOCATED.
    Width = 16;
    Scale = 4;
  } else {
    Width = 1u << Size;
    Scale = Size;
  }
  const unsigned Rn = (Word >> 5) & 0x1Fu;
  const unsigned Op4 = (Word >> 10) & 3u; // bits[11:10]
  const bool Bit21 = (Word >> 21) & 1u;

  if (Bit21) {
    // Register offset (option=bits[15:13], S=bit12, Rm=bits[20:16]).
    if (Op4 != 2)
      return false;
    const unsigned Option = (Word >> 13) & 0x7u;
    if (((Option >> 1) & 1u) == 0u)
      return false; // reserved option.
    const bool IdxIs64 = (Option & 1u) != 0u;
    const unsigned Rm = (Word >> 16) & 0x1Fu;
    const bool S = (Word >> 12) & 1u;
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                          IsLoad ? AARCH64_INS_LDR : AARCH64_INS_STR);
    Insn.is_alias = (Option == 3 && !S); // plain `[Xn, Xm]`
    A.op_count = 2;
    setReg(A.operands[0], fpReg(Word & 0x1Fu, Width));
    A.operands[0].access = IsLoad ? CS_AC_WRITE : CS_AC_READ;
    clearOp(A.operands[1]);
    A.operands[1].type = AARCH64_OP_MEM;
    A.operands[1].mem.base = gprSP(Rn, /*Is64=*/true);
    A.operands[1].mem.index = gpr(Rm, IdxIs64);
    A.operands[1].mem.disp = 0;
    switch (Option) {
    case 2:
      A.operands[1].ext = AARCH64_EXT_UXTW;
      break;
    case 6:
      A.operands[1].ext = AARCH64_EXT_SXTW;
      break;
    case 7:
      A.operands[1].ext = AARCH64_EXT_SXTX;
      break;
    default:
      A.operands[1].ext = AARCH64_EXT_INVALID;
      break;
    }
    if (S) {
      A.operands[1].shift.type = AARCH64_SFT_LSL;
      A.operands[1].shift.value = Scale;
    }
    A.operands[1].access = IsLoad ? CS_AC_READ : CS_AC_WRITE;
    return true;
  }

  if (Op4 == 2)
    return false; // no unprivileged FP form.
  int64_t Imm9 = static_cast<int64_t>((Word >> 12) & 0x1FFu);
  Imm9 = (Imm9 ^ 0x100) - 0x100; // sign-extend bit 8

  if (Op4 == 0) {
    // Unscaled LDUR/STUR (FP).
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                          IsLoad ? AARCH64_INS_LDUR : AARCH64_INS_STUR);
    A.op_count = 2;
    setReg(A.operands[0], fpReg(Word & 0x1Fu, Width));
    A.operands[0].access = IsLoad ? CS_AC_WRITE : CS_AC_READ;
    setMem(A.operands[1], gprSP(Rn, /*Is64=*/true), Imm9);
    A.operands[1].access = IsLoad ? CS_AC_READ : CS_AC_WRITE;
    return true;
  }

  // Pre-index (0b11) / post-index (0b01): LDR/STR with writeback.
  cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                        IsLoad ? AARCH64_INS_LDR : AARCH64_INS_STR);
  A.op_count = 2;
  setReg(A.operands[0], fpReg(Word & 0x1Fu, Width));
  A.operands[0].access = IsLoad ? CS_AC_WRITE : CS_AC_READ;
  setMem(A.operands[1], gprSP(Rn, /*Is64=*/true), Imm9);
  A.operands[1].access = IsLoad ? CS_AC_READ : CS_AC_WRITE;
  Detail.writeback = true;
  A.post_index = (Op4 == 1);
  return true;
}

/// Load/store register pair: bits[29:27]==0b101, V(bit26)==0 (integer).
/// The [25:23] class selects post-index(001)/offset(010)/pre-index(011); the
/// no-allocate STNP/LDNP form(000) is declined.  opc(2) picks 32-bit(00) or
/// 64-bit(10); LDPSW/STGP(01) and the reserved(11) opc are declined (the
/// lifter has no LDPSW path, so leaving it to Capstone preserves behavior).
/// Layout: opc 101 V mode L imm7 Rt2 Rn Rt; disp = SignExtend(imm7)*regsize,
/// exactly as Capstone reports it.
inline bool decodeLdStPair(uint32_t Word, va_t Addr, cs_insn &Insn,
                           cs_detail &Detail) {
  const unsigned Mode = (Word >> 23) & 0x7u; // 001 post, 010 offset, 011 pre
  const unsigned Opc = (Word >> 30) & 0x3u;
  if ((Mode != 1 && Mode != 2 && Mode != 3) || (Opc != 0 && Opc != 2))
    return false;
  const bool Is64 = (Opc == 2);
  const bool IsLoad = (Word >> 22) & 1u;
  int64_t Imm7 = static_cast<int64_t>((Word >> 15) & 0x7Fu);
  Imm7 = (Imm7 ^ 0x40) - 0x40; // sign-extend bit 6
  const unsigned Scale = Is64 ? 3 : 2;
  const int64_t Disp = Imm7 << Scale;
  const unsigned Rt = Word & 0x1Fu;
  const unsigned Rt2 = (Word >> 10) & 0x1Fu;
  const unsigned Rn = (Word >> 5) & 0x1Fu;

  cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                        IsLoad ? AARCH64_INS_LDP : AARCH64_INS_STP);
  // Capstone aliases only the zero-displacement offset form.
  Insn.is_alias = (Mode == 2 && Imm7 == 0);
  const cs_ac_type RtAcc = IsLoad ? CS_AC_WRITE : CS_AC_READ;
  A.op_count = 3;
  setReg(A.operands[0], gpr(Rt, Is64));
  A.operands[0].access = RtAcc;
  setReg(A.operands[1], gpr(Rt2, Is64));
  A.operands[1].access = RtAcc;
  setMem(A.operands[2], gprSP(Rn, /*Is64=*/true), Disp);
  A.operands[2].access = IsLoad ? CS_AC_READ : CS_AC_WRITE;
  if (Mode != 2) {
    Detail.writeback = true;
    A.post_index = (Mode == 1);
  }
  return true;
}

/// FP/SIMD load/store pair: bits[29:27]==0b101, V(bit26)==1.  Same mode
/// encoding as the integer pair (STNP/LDNP mode 000 declined); opc(2) picks
/// the S(00)/D(01)/Q(10) register width (opc 11 reserved).  disp =
/// SignExtend(imm7) << (opc+2), exactly as Capstone reports it.
inline bool decodeLdStPairFP(uint32_t Word, va_t Addr, cs_insn &Insn,
                             cs_detail &Detail) {
  const unsigned Mode = (Word >> 23) & 0x7u;
  const unsigned Opc = (Word >> 30) & 0x3u;
  if ((Mode != 1 && Mode != 2 && Mode != 3) || Opc == 3)
    return false;
  const unsigned Width = (Opc == 0) ? 4u : (Opc == 1) ? 8u : 16u;
  const unsigned Scale = Opc + 2u; // 2,3,4
  const bool IsLoad = (Word >> 22) & 1u;
  int64_t Imm7 = static_cast<int64_t>((Word >> 15) & 0x7Fu);
  Imm7 = (Imm7 ^ 0x40) - 0x40; // sign-extend bit 6
  const int64_t Disp = Imm7 << Scale;
  const unsigned Rt = Word & 0x1Fu;
  const unsigned Rt2 = (Word >> 10) & 0x1Fu;
  const unsigned Rn = (Word >> 5) & 0x1Fu;

  cs_aarch64 &A = begin(Insn, Detail, Word, Addr,
                        IsLoad ? AARCH64_INS_LDP : AARCH64_INS_STP);
  Insn.is_alias = (Mode == 2 && Imm7 == 0);
  const cs_ac_type RtAcc = IsLoad ? CS_AC_WRITE : CS_AC_READ;
  A.op_count = 3;
  setReg(A.operands[0], fpReg(Rt, Width));
  A.operands[0].access = RtAcc;
  setReg(A.operands[1], fpReg(Rt2, Width));
  A.operands[1].access = RtAcc;
  setMem(A.operands[2], gprSP(Rn, /*Is64=*/true), Disp);
  A.operands[2].access = IsLoad ? CS_AC_READ : CS_AC_WRITE;
  if (Mode != 2) {
    Detail.writeback = true;
    A.post_index = (Mode == 1);
  }
  return true;
}

} // namespace a64native
} // namespace neverd

#endif // NEVERD_DECODE_AARCH64DECODELOADSTORE_H
