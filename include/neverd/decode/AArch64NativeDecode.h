//===- AArch64NativeDecode.h - Native fixed-width operand decode -*- C++ -*-//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Capstone-free operand-level decode of a single 32-bit AArch64 instruction
/// word into a `cs_insn` the existing AArch64Lifter can consume unchanged.
///
/// Capstone's decode DFA is the throughput ceiling of the front end: on this
/// machine it sustains only ~1.6M insn/s single-threaded and — being
/// table-driven with a large working set — it does not scale across cores
/// (aggregate throughput peaks at ~4 threads and then regresses), while a
/// fixed-width native scan over the very same bytes runs at ~150M insn/s and
/// scales ~9x to 12 threads.  AArch64 is fixed 4-byte width and 4-byte
/// aligned, so the common instruction classes can be decoded with a handful of
/// mask+shift extractions and the operands written straight into a `cs_insn`,
/// bypassing the DFA entirely.
///
/// \b Correctness \b contract.  This decoder never has to be a full AArch64
/// disassembler.  It handles a fixed, growing set of high-frequency classes
/// and \e declines everything else (returns false), so the caller falls back
/// to Capstone.  Two invariants make the fast path safe, and both are locked
/// by a differential test (AArch64_NativeDecodeParityTests) that uses Capstone
/// as the oracle over exhaustive field sweeps, millions of random words, and
/// whole real binaries:
///
///   1. \b Strict \b subset.  For every word `tryDecode` accepts, Capstone
///      also accepts it (every reserved-bit constraint of the class is
///      checked here), so routing a word through the fast path never turns a
///      Capstone \e reject into an accept — the property function-entry
///      verification relies on when walking untrusted bytes.
///   2. \b Lift \b parity.  Feeding the `cs_insn` produced here to
///      AArch64Lifter yields byte-identical LowIR to feeding the `cs_insn`
///      Capstone would have produced for the same word.  The synthesized form
///      is chosen to lift identically (e.g. the canonical MOVZ form lifts the
///      same as Capstone's `mov` alias), so exact reproduction of Capstone's
///      mnemonic text / alias flags is unnecessary — only the operand detail
///      the lifter reads must match.
///
/// The produced `cs_insn` intentionally leaves `mnemonic`/`op_str` empty: the
/// lift and CFG paths read operands and the instruction id, never the rendered
/// text (the SDK disassembly path, which does read the text, keeps using
/// Capstone).  `id` is set to the canonical instruction id for the class.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_AARCH64NATIVEDECODE_H
#define NEVERD_DECODE_AARCH64NATIVEDECODE_H

#include "neverd/Common.h"

#include <capstone/capstone.h>

#include <bit>
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
  case 1: Base = AARCH64_REG_B0; break;
  case 2: Base = AARCH64_REG_H0; break;
  case 4: Base = AARCH64_REG_S0; break;
  case 8: Base = AARCH64_REG_D0; break;
  default: Base = AARCH64_REG_Q0; break; // 16
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

/// Expand an 8-bit AArch64 FP immediate to the real value it denotes, returned
/// as a double.  The 8-bit format encodes the same real number for every
/// precision (H/S/D), and all such values are exactly representable in double,
/// so the width-64 expansion equals the value Capstone reports in operands[].fp
/// for the H/S/D forms alike (VFPExpandImm with N=64).
inline double vfpExpandImmToDouble(unsigned Imm8) {
  const uint64_t Sign = (Imm8 >> 7) & 1u;
  const uint64_t B = (Imm8 >> 6) & 1u;
  // exp[10]=NOT(b); exp[9:2]=b replicated 8x; exp[1:0]=imm8<5:4>.
  const uint64_t Exp =
      ((~B & 1ULL) << 10) | ((B ? 0xFFULL : 0ULL) << 2) | ((Imm8 >> 4) & 3ULL);
  const uint64_t Frac = static_cast<uint64_t>(Imm8 & 0xFu) << 48;
  const uint64_t Bits = (Sign << 63) | (Exp << 52) | Frac;
  double D;
  std::memcpy(&D, &Bits, 8);
  return D;
}

/// Would \p Value be produced by some MOVZ/MOVN 16-bit wide-immediate move?
/// This is the ARM \c MoveWidePreferred predicate (mirrored from LLVM's
/// AArch64AddressingModes): when true, Capstone keeps the 3-operand `orr Rd,
/// xzr, #imm` form; when false it renders the `mov Rd, #imm` (bitmask) alias.
inline bool isAnyMovwMovAlias(uint64_t Value, unsigned RegWidth) {
  auto isMovz = [](uint64_t V, unsigned Shift) {
    return V == (V & (0xFFFFULL << Shift));
  };
  auto isMovn = [RegWidth](uint64_t V, unsigned Shift) {
    uint64_t NotV = ~V;
    if (RegWidth == 32)
      NotV &= 0xFFFFFFFFULL;
    return NotV == (NotV & (0xFFFFULL << Shift));
  };
  for (unsigned Shift = 0; Shift + 16 <= RegWidth; Shift += 16)
    if (isMovz(Value, Shift) || isMovn(Value, Shift))
      return true;
  return false;
}

/// Decode an AArch64 logical bitmask immediate (N:immr:imms) to its 64/32-bit
/// value, returning false for the UNDEFINED encodings Capstone rejects, so the
/// class stays a strict subset.  This mirrors the architectural DecodeBitMasks
/// with immediate=TRUE; Capstone reports exactly this value in operands[].imm.
inline bool decodeBitMasks(bool Sf, unsigned N, unsigned Imms, unsigned Immr,
                           uint64_t &Out) {
  const unsigned DataSize = Sf ? 64u : 32u;
  const unsigned V = (N << 6) | ((~Imms) & 0x3Fu); // immN:NOT(imms), 7 bits
  if (V == 0)
    return false; // len < 0
  const int Len = static_cast<int>(std::bit_width(V)) - 1;
  if (Len < 1)
    return false;
  const unsigned ESize = 1u << Len;
  if (ESize > DataSize)
    return false; // e.g. 32-bit form with N==1
  const unsigned Levels = ESize - 1u;
  const unsigned S = Imms & Levels;
  const unsigned R = Immr & Levels;
  if (S == Levels)
    return false; // imms all-ones within the field -> UNDEFINED
  const uint64_t ESizeMask = (ESize >= 64) ? ~0ULL : ((1ULL << ESize) - 1);
  uint64_t Welem = ((S + 1u) >= 64u) ? ~0ULL : ((1ULL << (S + 1u)) - 1);
  Welem &= ESizeMask;
  uint64_t Ror = (R == 0) ? Welem
                          : (((Welem >> R) | (Welem << (ESize - R))) & ESizeMask);
  uint64_t Wmask = 0;
  for (unsigned I = 0; I < DataSize; I += ESize)
    Wmask |= Ror << I;
  if (DataSize == 32)
    Wmask &= 0xFFFFFFFFULL;
  Out = Wmask;
  return true;
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
    if (Opc == 0) { Id = AARCH64_INS_STRB; IsStore = true; }
    else if (Opc == 1) Id = AARCH64_INS_LDRB;
    else { Id = AARCH64_INS_LDRSB; RtIs64 = (Opc == 2); }
    return true;
  case 1:
    if (Opc == 0) { Id = AARCH64_INS_STRH; IsStore = true; }
    else if (Opc == 1) Id = AARCH64_INS_LDRH;
    else { Id = AARCH64_INS_LDRSH; RtIs64 = (Opc == 2); }
    return true;
  case 2:
    if (Opc == 0) { Id = AARCH64_INS_STR; IsStore = true; }
    else if (Opc == 1) Id = AARCH64_INS_LDR;
    else if (Opc == 2) { Id = AARCH64_INS_LDRSW; RtIs64 = true; }
    else return false;
    return true;
  default: // size == 3
    if (Opc == 0) { Id = AARCH64_INS_STR; IsStore = true; RtIs64 = true; }
    else if (Opc == 1) { Id = AARCH64_INS_LDR; RtIs64 = true; }
    else return false; // PRFM / UNALLOCATED
    return true;
  }
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

/// Decode \p Word at \p Addr into \p Insn (+ \p Detail) if it belongs to a
/// class this native decoder covers; return true on success (Insn is fully
/// populated), false to decline (the caller must fall back to Capstone).
///
/// Covered classes (each a strict subset of Capstone's accept set, lifted
/// identically to Capstone — see the file-level contract):
///   * ADR / ADRP         (PC-relative address, immediate already resolved)
///   * MOVZ / MOVN / MOVK  (16-bit wide immediate move)
///   * ADD/SUB (immediate, shifted register, extended register) + CMP/CMN/MOV
///   * Logical (immediate + shifted register) + the MOV/MVN/TST/ORR-bitmask
///     aliases; bitfield moves (SBFM/BFM/UBFM) and EXTR/ROR
///   * Multiply-accumulate (MADD/MSUB + MUL/MNEG, SMULH/UMULH) and the widening
///     long forms (SMADDL/UMADDL/SMSUBL/UMSUBL + SMULL/UMULL/SMNEGL/UMNEGL)
///   * SDIV/UDIV and the variable shifts (LSLV/LSRV/ASRV/RORV)
///   * Data-processing 1-source integer (RBIT/REV16/REV32/REV/CLZ/CLS)
///   * Conditional select (CSEL/CSINC/CSINV/CSNEG + cset/cinc/cneg) and
///     conditional compare (CCMP/CCMN)
///   * Integer & FP/SIMD load/store (unsigned-offset, register-offset,
///     unscaled, pre/post-index) and load/store pair
///   * Scalar FP: FMOV (register, general GPR<->FP, 8-bit immediate),
///     FADD/FSUB/FMUL/FDIV/FMAX/FMIN/FMAXNM/FMINNM/FNMUL, FABS/FNEG/FSQRT,
///     FCVT (precision convert), and FCMP/FCMPE
///   * Control flow (B/BL/B.cond/CBZ/CBNZ/TBZ/TBNZ/RET/BR/BLR), NOP, BRK
inline bool tryDecode(uint32_t Word, va_t Addr, cs_insn &Insn,
                      cs_detail &Detail) {
  const unsigned Rd = Word & 0x1F;

  // --- ADR / ADRP: op[28:24] == 0b10000, bit31 selects ADR(0)/ADRP(1). ---
  // Layout: op immlo[30:29] 1 0000 immhi[23:5] Rd[4:0].  No reserved bits, so
  // Capstone accepts the whole (op, immlo, immhi, Rd) space; strict subset is
  // the class mask alone.
  if ((Word & 0x1F000000u) == 0x10000000u) {
    const bool IsAdrp = (Word >> 31) & 1u;
    const uint32_t ImmLo = (Word >> 29) & 0x3u;
    const uint32_t ImmHi = (Word >> 5) & 0x7FFFFu; // 19 bits
    int64_t Imm21 = static_cast<int64_t>((ImmHi << 2) | ImmLo);
    Imm21 = (Imm21 ^ 0x100000) - 0x100000; // sign-extend bit 20
    int64_t Target;
    if (IsAdrp)
      Target = static_cast<int64_t>(Addr & ~static_cast<va_t>(0xFFF)) +
               (Imm21 << 12);
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

  // --- MOVZ / MOVN / MOVK: bits[28:23] == 0b100101, opc[30:29] selects the
  // variant.  sf=bit31 (0=W,1=X).  hw=bits[22:21] (shift = hw*16); for a
  // 32-bit form hw>=2 is UNALLOCATED (Capstone rejects), so require bit22==0
  // when sf==0 to stay a strict subset.  imm16=bits[20:5]. ---
  if (((Word >> 23) & 0x3Fu) == 0x25u) {
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
                         : static_cast<int64_t>(static_cast<int32_t>(
                               static_cast<uint32_t>(Raw)));
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

  // --- Add/subtract (immediate): bits[28:24]==0b10001, bit23==0.
  // sf op S 1 0 0 0 1 0 sh imm12 Rn Rd.  Rd/Rn use the SP interpretation of
  // index 31.  The MOV-to/from-SP, CMP and CMN aliases the lifter recognizes
  // are reproduced exactly; everything else is the plain 3-operand form. ---
  if (((Word >> 24) & 0x1Fu) == 0x11u && ((Word >> 23) & 1u) == 0u) {
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

  // --- Add/subtract (shifted register): bits[28:24]==0b01011, bit21==0.
  // sf op S 0 1 0 1 1 shift(2) 0 Rm imm6 Rn Rd.  Index 31 is the zero register
  // (no SP here).  ROR (shift==3) is reserved, and a 32-bit amount>=32 is
  // UNALLOCATED — both declined to stay a strict Capstone subset.  The
  // extended-register form (bit21==1) is handled by the next block. ---
  if (((Word >> 24) & 0x1Fu) == 0x0Bu && ((Word >> 21) & 1u) == 0u) {
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

  // --- Add/subtract (extended register): bits[28:24]==0b01011, bit21==1.
  // sf op S 0 1 0 1 1 opt(2) 1 Rm option(3) imm3(3) Rn Rd.  opt(23:22) must be
  // 0 and imm3<=4 (else UNALLOCATED).  Rd/Rn use the SP interpretation of index
  // 31; Rm is the zero-register form.  The shifted-register class (bit21==0) is
  // handled above; the register letter for Rm is X only for the UXTX/SXTX
  // options of a 64-bit op, else W (matching Capstone's operand detail). ---
  if (((Word >> 24) & 0x1Fu) == 0x0Bu && ((Word >> 21) & 1u) == 1u) {
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

  // --- Logical (shifted register): bits[28:24]==0b01010.
  // sf opc(2) 0 1 0 1 0 shift(2) N Rm imm6 Rn Rd.  All four shift types legal.
  // opc/N select AND/BIC/ORR/ORN/EOR/EON/ANDS/BICS.  Index 31 is the zero
  // register.  MOV (ORR,Rn==31,LSL#0), MVN (ORN,Rn==31) and TST (ANDS,Rd==31)
  // are the aliases the lifter recognizes. ---
  if (((Word >> 24) & 0x1Fu) == 0x0Au) {
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
    static const unsigned IdTab[4][2] = {
        {AARCH64_INS_AND, AARCH64_INS_BIC},
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

  // --- Data-processing (3 source): bits[30:24]==0b0011011 (op54==00).  Only
  // the non-widening multiply-accumulate forms are taken natively: MADD/MSUB
  // (op31==0) with their MUL/MNEG aliases (Ra==31), and the high-half
  // SMULH/UMULH (op31==010/110, o0==0), and the widening long forms
  // SMADDL/UMADDL/SMSUBL/UMSUBL (op31==001/101) with their
  // SMULL/UMULL/SMNEGL/UMNEGL aliases (Ra==31).  The remaining 3-source forms
  // are left to Capstone. ---
  if (((Word >> 24) & 0x7Fu) == 0x1Bu) {
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

  // --- Data-processing (2 source): bits[30:21]==0b0011010110.  Integer divides
  // (SDIV opcode 000011, UDIV 000010) and the variable shifts LSLV/LSRV/ASRV/
  // RORV (opcodes 001000..001011, which Capstone renders as LSL/LSR/ASR/ROR
  // with a register shift); CRC/PACGA and the rest are left to Capstone. ---
  if (((Word >> 21) & 0x3FFu) == 0x0D6u) {
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

  // --- Data-processing (1 source): bits[30:21]==0b1011010110, opcode2(20:16)==0
  // (else UNALLOCATED).  opcode(15:10) selects RBIT/REV16/REV32/REV/CLZ/CLS;
  // the FEAT_CSSC additions (CTZ/CNT/ABS, opcode>=6) and the PAC forms
  // (opcode2!=0) are left to Capstone.  All are 2-operand `Rd, Rn`. ---
  if (((Word >> 21) & 0x3FFu) == 0x2D6u && ((Word >> 16) & 0x1Fu) == 0u) {
    const bool Is64 = (Word >> 31) & 1u;
    const unsigned Opcode = (Word >> 10) & 0x3Fu;
    const unsigned Rn = (Word >> 5) & 0x1Fu;
    unsigned Id;
    switch (Opcode) {
    case 0: Id = AARCH64_INS_RBIT; break;
    case 1: Id = AARCH64_INS_REV16; break;
    case 2: Id = Is64 ? AARCH64_INS_REV32 : AARCH64_INS_REV; break;
    case 3:
      if (!Is64)
        return false; // opcode 000011 is UNALLOCATED for the 32-bit form.
      Id = AARCH64_INS_REV;
      break;
    case 4: Id = AARCH64_INS_CLZ; break;
    case 5: Id = AARCH64_INS_CLS; break;
    default: return false; // CTZ/CNT/ABS (FEAT_CSSC) etc. -> Capstone.
    }
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, Id);
    A.op_count = 2;
    setReg(A.operands[0], gpr(Word & 0x1Fu, Is64));
    A.operands[0].access = CS_AC_WRITE;
    setReg(A.operands[1], gpr(Rn, Is64));
    A.operands[1].access = CS_AC_READ;
    return true;
  }

  // --- EXTR (extract register): bits[30:23]==0b00100111, N==sf, o0(bit21)==0.
  // Capstone renders the Rn==Rm case as the ROR-immediate alias. ---
  if (((Word >> 23) & 0xFFu) == 0x27u && ((Word >> 22) & 1u) == ((Word >> 31) & 1u) &&
      ((Word >> 21) & 1u) == 0u) {
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

  // --- Conditional compare (register & immediate): bits[28:21]==0b11010010,
  // S(bit29)==1, o3(bit10)==0, o1(bit4)==0 (else UNALLOCATED).  op(bit30)
  // selects CCMN(0)/CCMP(1); o2(bit11) selects register(0)/immediate(1).
  // Layout: sf op 1 11010010 Rm/imm5 cond 0 0 Rn 0 nzcv.  The lifter reads
  // operands[0]=Rn, operands[1]=Rm|#imm5, operands[2]=#nzcv and cc. ---
  if (((Word >> 21) & 0xFFu) == 0xD2u && ((Word >> 29) & 1u) == 1u) {
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

  // --- Conditional select: bits[28:21]==0b11010100, S(bit29)==0, bit11==0.
  // op(bit30):op2(bit10) select CSEL/CSINC/CSINV/CSNEG.  Capstone renders the
  // CSET/CSETM (Rm==Rn==31), CINC/CINV (Rm==Rn!=31) and CNEG (Rm==Rn) aliases
  // when the condition is not AL/NV, reporting the *inverted* condition; the
  // lifter reads is_alias + op_count + cc to reconstruct them, so reproduce
  // them exactly. ---
  if (((Word >> 21) & 0xFFu) == 0xD4u && ((Word >> 29) & 1u) == 0u &&
      ((Word >> 11) & 1u) == 0u) {
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
    if (CondAliasable && Rm == 31 && Rn == 31 &&
        (Variant == 1 || Variant == 2)) {
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

  // --- Bitfield move (SBFM/BFM/UBFM): bits[28:23]==0b100110.  Capstone almost
  // always renders one of the many preferred aliases (lsl/lsr/asr, uxtb/uxth,
  // sxtb/sxth/sxtw, ubfx/ubfiz, sbfx/sbfiz, bfi/bfxil/bfc), and the lifter's
  // decode depends on exactly which alias + operands it sees, so the selection
  // is reproduced bit-for-bit here.  N must equal sf and, for the 32-bit form,
  // immr/imms must be < 32 (else UNALLOCATED — declined). ---
  if (((Word >> 23) & 0x3Fu) == 0x26u) {
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

  // --- Logical (immediate): bits[28:23]==0b100100.  sf opc 100100 N immr imms
  // Rn Rd.  opc selects AND/ORR/EOR/ANDS; the bitmask immediate is decoded to
  // its value (Capstone reports the decoded value).  Rd is the SP-form for
  // AND/ORR/EOR and the ZR-form for ANDS; Rn is always the ZR-form.  The
  // ORR-with-Rn==31 MOV(bitmask) alias is reproduced via the MoveWidePreferred
  // predicate (isAnyMovwMovAlias); the TST (ANDS, Rd==31) alias is reproduced
  // too. ---
  if (((Word >> 23) & 0x3Fu) == 0x24u) {
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
                              : static_cast<int64_t>(
                                    static_cast<int32_t>(static_cast<uint32_t>(Imm)));
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

  // --- NOP (the canonical HINT #0).  Capstone renders it as the HINT alias
  // with no operands; the lifter's HINT case emits a single NOP for any hint it
  // does not special-case (YIELD/WFE/WFI/SEV/SEVL), which this is not. ---
  if (Word == 0xD503201Fu) {
    cs_aarch64 &A = begin(Insn, Detail, Word, Addr, AARCH64_INS_HINT);
    Insn.is_alias = true;
    A.op_count = 0;
    return true;
  }

  // --- Load/store register (unsigned immediate offset): the dominant memory
  // form.  bits[29:27]==0b111, bits[25:24]==0b01, V(bit26)==0 (integer only;
  // FP/SIMD V==1 is declined so Capstone maps the S/D/Q register).  Layout:
  // size(2) 111 V 01 opc(2) imm12 Rn Rt.  The byte displacement is the encoded
  // imm12 scaled by the access size (Capstone surfaces the scaled value); Rn is
  // the SP-form base, Rt the zero-register-form transfer register. ---
  if ((Word & 0x3B000000u) == 0x39000000u && ((Word >> 26) & 1u) == 0u) {
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

  // --- FP/SIMD load/store register (unsigned immediate offset): same shape as
  // the integer form but V(bit26)==1, transferring a B/H/S/D/Q register.  The
  // access width is 1<<size for the scalar forms; opc bit23 selects the 128-bit
  // Q form (size must be 00).  The lifter's LDR/STR handler reads the FP
  // destination register and load size straight from this, so it lifts
  // identically to Capstone. ---
  if ((Word & 0x3B000000u) == 0x39000000u && ((Word >> 26) & 1u) == 1u) {
    const unsigned Size = (Word >> 30) & 3u;
    const unsigned Opc = (Word >> 22) & 3u;
    const bool IsQ = (Opc >> 1) & 1u;   // bit23 extends the width to 128
    const bool IsLoad = Opc & 1u;       // bit22
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

  // --- Load/store register (register offset and unscaled/pre/post-index):
  // bits[29:27]==0b111, bits[25:24]==0b00, V(bit26)==0.  Sub-form by bit21 and
  // bits[11:10]:
  //   * bit21==1, [11:10]==0b10 : register offset  (scaled by option/S)
  //   * bit21==0, [11:10]==0b00 : unscaled signed imm9  (LDUR/STUR family)
  //   * bit21==0, [11:10]==0b01 : post-index  (LDR family, writeback)
  //   * bit21==0, [11:10]==0b11 : pre-index   (LDR family, writeback)
  // The unprivileged ([11:10]==0b10, bit21==0) and atomic/PAC (bit21==1,
  // [11:10]!=0b10) forms are declined. ---
  if (((Word >> 27) & 0x7u) == 0x7u && ((Word >> 24) & 0x3u) == 0x0u &&
      ((Word >> 26) & 1u) == 0u) {
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
      case 2: A.operands[1].ext = AARCH64_EXT_UXTW; break;
      case 6: A.operands[1].ext = AARCH64_EXT_SXTW; break;
      case 7: A.operands[1].ext = AARCH64_EXT_SXTX; break;
      default: A.operands[1].ext = AARCH64_EXT_INVALID; break; // option 3 (LSL)
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
      static const struct { unsigned Scaled, Unscaled; } Map[] = {
          {AARCH64_INS_STRB, AARCH64_INS_STURB},
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
        if (E.Scaled == Scaled) { Id = E.Unscaled; break; }
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

  // --- FP/SIMD load/store register (register-offset, unscaled, pre/post-index):
  // same shape as the integer forms above but V(bit26)==1, transferring a
  // B/H/S/D/Q register.  Access width is 1<<size, or 16 when the Q bit
  // (opc<23>) is set (size must be 00).  Unscaled uses the LDUR/STUR ids; the
  // register-offset and pre/post-index forms use LDR/STR (with writeback), just
  // as Capstone reports them; the lifter reads the FP register and load size
  // straight from this, so it lifts identically. ---
  if (((Word >> 27) & 0x7u) == 0x7u && ((Word >> 24) & 0x3u) == 0x0u &&
      ((Word >> 26) & 1u) == 1u) {
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
      case 2: A.operands[1].ext = AARCH64_EXT_UXTW; break;
      case 6: A.operands[1].ext = AARCH64_EXT_SXTW; break;
      case 7: A.operands[1].ext = AARCH64_EXT_SXTX; break;
      default: A.operands[1].ext = AARCH64_EXT_INVALID; break;
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

  // --- Load/store register pair: bits[29:27]==0b101, V(bit26)==0 (integer).
  // The [25:23] class selects post-index(001)/offset(010)/pre-index(011); the
  // no-allocate STNP/LDNP form(000) is declined.  opc(2) picks 32-bit(00) or
  // 64-bit(10); LDPSW/STGP(01) and the reserved(11) opc are declined (the
  // lifter has no LDPSW path, so leaving it to Capstone preserves behavior).
  // Layout: opc 101 V mode L imm7 Rt2 Rn Rt; disp = SignExtend(imm7)*regsize,
  // exactly as Capstone reports it. ---
  if (((Word >> 27) & 0x7u) == 0x5u && ((Word >> 26) & 1u) == 0u) {
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

  // --- FP/SIMD load/store pair: bits[29:27]==0b101, V(bit26)==1.  Same mode
  // encoding as the integer pair (STNP/LDNP mode 000 declined); opc(2) picks
  // the S(00)/D(01)/Q(10) register width (opc 11 reserved).  disp =
  // SignExtend(imm7) << (opc+2), exactly as Capstone reports it. ---
  if (((Word >> 27) & 0x7u) == 0x5u && ((Word >> 26) & 1u) == 1u) {
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

  // --- FMOV (scalar): bits[30:24]==0b0011110, bit21==1.  Three sub-forms are
  // taken natively; the top-half (Vn.D[1]) forms (rmode!=0) and every other FP
  // scalar op are declined.  ptype(23:22) selects S(00)/D(01)/H(11); ptype 10
  // is UNALLOCATED here.  Register letters map to the V bank (B/H/S/D/Q). ---
  if (((Word >> 24) & 0x7Fu) == 0x1Eu && ((Word >> 21) & 1u) == 1u) {
    if (((Word >> 29) & 1u) != 0u)
      return false; // S bit must be 0.
    const unsigned Ptype = (Word >> 22) & 3u;
    unsigned FpBytes;
    switch (Ptype) {
    case 0: FpBytes = 4; break; // S
    case 1: FpBytes = 8; break; // D
    case 3: FpBytes = 2; break; // H
    default: return false;      // ptype 10 UNALLOCATED
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
          AARCH64_INS_FMUL,  AARCH64_INS_FDIV,    AARCH64_INS_FADD,
          AARCH64_INS_FSUB,  AARCH64_INS_FMAX,    AARCH64_INS_FMIN,
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

  // --- Control flow.  Capstone surfaces the branch target as an absolute VA in
  // operands[0] (or [1]/[2] for the compare/test-and-branch forms) and the
  // condition of a `b.cond` in cc; the lifter reads exactly those.  RET/BR/BLR
  // reuse the fixed-width masks already proven bit-exact in the classifier
  // (the PAC-authenticated RETAA/BRAA/... forms have distinct ids and are
  // excluded by the reserved-bit portion of the mask, so Capstone handles
  // them). ---
  {
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
  }

  return false;
}

} // namespace a64native
} // namespace neverd

#endif // NEVERD_DECODE_AARCH64NATIVEDECODE_H
