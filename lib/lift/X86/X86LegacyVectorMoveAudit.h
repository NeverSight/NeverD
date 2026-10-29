//===- X86LegacyVectorMoveAudit.h - Legacy vector move audit ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Undefined-output admission for legacy vector moves and register XOR.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86LEGACYVECTORMOVEAUDIT_H
#define NEVERD_LIB_LIFT_X86_X86LEGACYVECTORMOVEAUDIT_H

#include "X86LegacyMemoryAudit.h"

namespace neverd::vectormoveaudit {

// MOVD/MOVQ between one GPR and one XMM. MMX, memory, VEX/EVEX and
// redundant prefixes need separate audits; legacy XMM upper lanes survive.
inline bool scalarRegisterMoveForm(const cs_insn *Insn, Arch Target) {
  if (Target != Arch::X64 ||
      (Insn->id != X86_INS_MOVD && Insn->id != X86_INS_MOVQ) ||
      Insn->size < 4 || Insn->size > 5 || Insn->bytes[0] != 0x66)
    return false;
  const auto &X = Insn->detail->x86;
  size_t Pos = 1;
  uint8_t Rex = 0;
  if (Insn->bytes[Pos] >= 0x40 && Insn->bytes[Pos] <= 0x4f)
    Rex = Insn->bytes[Pos++];
  const unsigned Width = Rex & 8 ? 8 : 4;
  if ((Rex & 2) || Pos + 3 != Insn->size || Insn->bytes[Pos] != 0x0f ||
      (Insn->id == X86_INS_MOVQ) != (Width == 8))
    return false;
  const auto Opcode = Insn->bytes[Pos + 1], ModRM = Insn->bytes[Pos + 2];
  if ((Opcode != 0x6e && Opcode != 0x7e) || (ModRM >> 6) != 3 ||
      X.op_count != 2 || X.opcode[0] != 0x0f || X.opcode[1] != Opcode ||
      X.opcode[2] || X.opcode[3] || X.modrm != ModRM || X.rex != Rex ||
      X.addr_size != 8 || X.prefix[0] || X.prefix[1] || X.prefix[2] != 0x66 ||
      X.prefix[3] || X.encoding.modrm_offset != Pos + 2 ||
      X.encoding.disp_offset || X.encoding.disp_size || X.disp || X.sib ||
      X.encoding.imm_offset || X.encoding.imm_size || X.avx_sae ||
      X.avx_rm != X86_AVX_RM_INVALID || X.avx_cc != X86_AVX_CC_INVALID ||
      X.eflags)
    return false;
  for (unsigned I = 0; I != 2; ++I) {
    const auto &Operand = X.operands[I];
    const bool Vector = I == (Opcode == 0x6e ? 0u : 1u);
    const unsigned Number = Vector ? ((ModRM >> 3) & 7) + ((Rex & 4) ? 8 : 0)
                                   : (ModRM & 7) + ((Rex & 1) ? 8 : 0);
    const auto Register = mapCapstoneReg(static_cast<x86_reg>(Operand.reg));
    const unsigned Bytes = Vector ? 16 : Width;
    if (Operand.type != X86_OP_REG || Operand.size != Bytes ||
        Register.Size != Bytes ||
        Register.Offset != (Vector ? x86reg::vectorReg(Number) : Number * 8) ||
        Operand.avx_bcast != X86_AVX_BCAST_INVALID || Operand.avx_zero_opmask)
      return false;
  }
  return true;
}

inline bool byteShiftForm(const cs_insn *Insn, Arch Target) {
  if (Target != Arch::X64 ||
      (Insn->id != X86_INS_PSLLDQ && Insn->id != X86_INS_PSRLDQ) ||
      Insn->size < 5 || Insn->size > 6 || Insn->bytes[0] != 0x66)
    return false;
  const auto &X = Insn->detail->x86;
  size_t Pos = 1;
  uint8_t Rex = 0;
  if (Insn->bytes[Pos] == 0x40 || Insn->bytes[Pos] == 0x41)
    Rex = Insn->bytes[Pos++];
  if (Pos + 4 != Insn->size || Insn->bytes[Pos] != 0x0f ||
      Insn->bytes[Pos + 1] != 0x73)
    return false;
  const auto ModRM = Insn->bytes[Pos + 2];
  if ((ModRM >> 6) != 3 ||
      ((ModRM >> 3) & 7) != (Insn->id == X86_INS_PSLLDQ ? 7 : 3) ||
      X.op_count != 2 || X.opcode[0] != 0x0f || X.opcode[1] != 0x73 ||
      X.opcode[2] || X.opcode[3] || X.modrm != ModRM || X.rex != Rex ||
      X.addr_size != 8 || X.prefix[0] || X.prefix[1] || X.prefix[2] != 0x66 ||
      X.prefix[3] || X.encoding.modrm_offset != Pos + 2 ||
      X.encoding.disp_offset || X.encoding.disp_size || X.disp || X.sib ||
      X.encoding.imm_offset != Pos + 3 || X.encoding.imm_size != 1 ||
      X.avx_sae || X.avx_rm != X86_AVX_RM_INVALID ||
      X.avx_cc != X86_AVX_CC_INVALID || X.eflags)
    return false;
  const auto &Destination = X.operands[0];
  const auto Register = mapCapstoneReg(static_cast<x86_reg>(Destination.reg));
  return Destination.type == X86_OP_REG && Destination.size == 16 &&
         Register.Size == 16 &&
         Register.Offset ==
             x86reg::vectorReg((ModRM & 7) + ((Rex & 1) ? 8 : 0)) &&
         Destination.avx_bcast == X86_AVX_BCAST_INVALID &&
         !Destination.avx_zero_opmask && X.operands[1].type == X86_OP_IMM &&
         X.operands[1].size == 1 && X.operands[1].imm == Insn->bytes[Pos + 3] &&
         X.operands[1].avx_bcast == X86_AVX_BCAST_INVALID &&
         !X.operands[1].avx_zero_opmask;
}

// Intel SDM Vol. 2B: packed moves copy all 128 bits. MOVSS/MOVSD copy the low
// scalar, merging register destinations or clearing the remaining XMM bits
// for memory loads. Legacy writes preserve higher vector bits and flags.
// This is only undefined-output coverage, not a preserved-opaque-bank audit.
// Aligned forms retain the lifter-owned guard in the bound operation span.
inline bool form(const cs_insn *Insn, Arch Target) {
  if (scalarRegisterMoveForm(Insn, Target) || byteShiftForm(Insn, Target))
    return true;
  const auto &X = Insn->detail->x86;
  if (X.op_count != 2 || Insn->size > 15)
    return false;
  uint8_t RequiredPrefix = 0, LoadOpcode = 0, StoreOpcode = 0;
  unsigned MemoryBytes = 16;
  switch (Insn->id) {
  case X86_INS_XORPS:
  case X86_INS_XORPD:
  case X86_INS_PXOR:
    // Legacy register XOR changes all XMM bits and preserves flags and the
    // upper vector lanes. Memory forms need their own alignment audit.
    if (X.operands[1].type != X86_OP_REG)
      return false;
    RequiredPrefix = Insn->id == X86_INS_XORPS ? 0 : 0x66;
    LoadOpcode = StoreOpcode = Insn->id == X86_INS_PXOR ? 0xef : 0x57;
    break;
  case X86_INS_MOVDQU:
    RequiredPrefix = 0xf3;
    LoadOpcode = 0x6f;
    StoreOpcode = 0x7f;
    break;
  case X86_INS_MOVDQA:
    RequiredPrefix = 0x66;
    LoadOpcode = 0x6f;
    StoreOpcode = 0x7f;
    break;
  case X86_INS_MOVUPS:
    LoadOpcode = 0x10;
    StoreOpcode = 0x11;
    break;
  case X86_INS_MOVUPD:
    RequiredPrefix = 0x66;
    LoadOpcode = 0x10;
    StoreOpcode = 0x11;
    break;
  case X86_INS_MOVAPS:
    LoadOpcode = 0x28;
    StoreOpcode = 0x29;
    break;
  case X86_INS_MOVAPD:
    RequiredPrefix = 0x66;
    LoadOpcode = 0x28;
    StoreOpcode = 0x29;
    break;
  case X86_INS_MOVSS:
    RequiredPrefix = 0xf3;
    MemoryBytes = 4;
    LoadOpcode = 0x10;
    StoreOpcode = 0x11;
    break;
  case X86_INS_MOVSD:
    RequiredPrefix = 0xf2;
    MemoryBytes = 8;
    LoadOpcode = 0x10;
    StoreOpcode = 0x11;
    break;
  default:
    return false;
  }
  const bool Long = Target == Arch::X64;
  size_t Pos = 0;
  uint8_t MandatoryPrefix = 0, AddressPrefix = 0, Rex = 0;
  for (; Pos < Insn->size; ++Pos) {
    const uint8_t B = Insn->bytes[Pos];
    if (RequiredPrefix && B == RequiredPrefix) {
      if (MandatoryPrefix || Rex)
        return false;
      MandatoryPrefix = B;
    } else if (B == 0x67) {
      if (AddressPrefix || Rex)
        return false;
      AddressPrefix = B;
    } else if (Long && B >= 0x40 && B <= 0x4f) {
      if (Rex)
        return false;
      Rex = B;
    } else {
      break;
    }
  }
  if (MandatoryPrefix != RequiredPrefix || Pos + 3 > Insn->size ||
      Insn->bytes[Pos] != 0x0f)
    return false;
  const uint8_t Opcode = Insn->bytes[Pos + 1];
  const uint8_t ModRM = Insn->bytes[Pos + 2];
  const unsigned AddressSize =
      Long ? (AddressPrefix ? 4 : 8) : (AddressPrefix ? 2 : 4);
  // Capstone removes mandatory F2/F3 from its normalized repeat-prefix field;
  // the raw prefix above remains authoritative for the instruction identity.
  if ((Opcode != LoadOpcode && Opcode != StoreOpcode) || X.modrm != ModRM ||
      X.encoding.modrm_offset != Pos + 2 || X.opcode[0] != 0x0f ||
      X.opcode[1] != Opcode || X.opcode[2] || X.opcode[3] || X.rex != Rex ||
      X.prefix[0] || X.prefix[1] ||
      X.prefix[2] != (RequiredPrefix == 0x66 ? 0x66 : 0) ||
      X.prefix[3] != AddressPrefix || X.addr_size != AddressSize ||
      X.encoding.imm_size || X.encoding.imm_offset || X.avx_sae ||
      X.avx_rm != X86_AVX_RM_INVALID || X.avx_cc != X86_AVX_CC_INVALID)
    return false;
  for (unsigned I = 0; I != 2; ++I)
    if (X.operands[I].size !=
            (X.operands[I].type == X86_OP_MEM ? MemoryBytes : 16) ||
        X.operands[I].avx_bcast != X86_AVX_BCAST_INVALID ||
        X.operands[I].avx_zero_opmask)
      return false;
  const auto Register = [&](const cs_x86_op &Operand, unsigned Number) {
    if (Operand.type != X86_OP_REG)
      return false;
    const auto R = mapCapstoneReg(static_cast<x86_reg>(Operand.reg));
    return R.Offset == x86reg::vectorReg(Number) && R.Size == 16;
  };
  const auto &Reg = X.operands[Opcode == LoadOpcode ? 0 : 1];
  const auto &RMOperand = X.operands[Opcode == LoadOpcode ? 1 : 0];
  if (!Register(Reg, ((ModRM >> 3) & 7) + ((Rex & 4) ? 8 : 0)))
    return false;
  if ((ModRM >> 6) == 3)
    return !AddressPrefix && !(Rex & 2) && Pos + 3 == Insn->size &&
           !X.encoding.disp_size && !X.encoding.disp_offset && !X.disp &&
           !X.sib && Register(RMOperand, (ModRM & 7) + ((Rex & 1) ? 8 : 0));
  return legacyaudit::memoryOperand(Insn, RMOperand, MemoryBytes, AddressSize,
                                    Long, Rex, Pos + 3);
}

} // namespace neverd::vectormoveaudit

#endif
