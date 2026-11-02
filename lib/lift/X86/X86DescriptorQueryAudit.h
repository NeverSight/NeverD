//===- X86DescriptorQueryAudit.h - Native descriptor reads ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Closed x64 SGDT, VERW and register LSL encoding/detail agreement.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86DESCRIPTORQUERYAUDIT_H
#define NEVERD_LIB_LIFT_X86_X86DESCRIPTORQUERYAUDIT_H

#include "X86LegacyMemoryAudit.h"

namespace neverd::descriptorqueryaudit {

inline bool form(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Target != Arch::X64 ||
      (Insn->id != X86_INS_SGDT && Insn->id != X86_INS_VERW &&
       Insn->id != X86_INS_LSL))
    return false;
  const auto &X = Insn->detail->x86;
  const bool Limit = Insn->id == X86_INS_LSL;
  size_t Pos = 0;
  uint8_t Address = 0, Segment = 0, Rex = 0;
  if (!Limit)
    for (; Pos < Insn->size; ++Pos) {
      const uint8_t B = Insn->bytes[Pos];
      if (B == 0x67 && !Address)
        Address = B;
      else if ((B == 0x64 || B == 0x65) && !Segment)
        Segment = B;
      else
        break;
    }
  if (Pos < Insn->size && Insn->bytes[Pos] >= 0x40 && Insn->bytes[Pos] <= 0x47)
    Rex = Insn->bytes[Pos++];
  const uint8_t Opcode = Limit ? 0x03 : Insn->id == X86_INS_SGDT ? 0x01 : 0x00;
  if (Pos + 3 > Insn->size || Insn->bytes[Pos] != 0x0f ||
      Insn->bytes[Pos + 1] != Opcode)
    return false;
  const uint8_t ModRM = Insn->bytes[Pos + 2];
  const unsigned AddressSize = Address ? 4 : 8;
  if (X.opcode[0] != 0x0f || X.opcode[1] != Opcode || X.opcode[2] ||
      X.opcode[3] || X.modrm != ModRM || X.encoding.modrm_offset != Pos + 2 ||
      X.rex != Rex || X.addr_size != AddressSize || X.prefix[0] ||
      X.prefix[1] != Segment || X.prefix[2] || X.prefix[3] != Address ||
      X.encoding.imm_offset || X.encoding.imm_size || X.avx_sae ||
      X.avx_rm != X86_AVX_RM_INVALID || X.avx_cc != X86_AVX_CC_INVALID ||
      X.op_count != (Limit ? 2 : 1) ||
      X.eflags != (Insn->id == X86_INS_SGDT ? 0 : X86_EFLAGS_MODIFY_ZF))
    return false;
  for (unsigned I = 0; I != X.op_count; ++I)
    if (X.operands[I].avx_bcast != X86_AVX_BCAST_INVALID ||
        X.operands[I].avx_zero_opmask)
      return false;
  if (Limit) {
    if ((ModRM >> 6) != 3 || (Rex & 2) || Pos + 3 != Insn->size || X.sib ||
        X.disp || X.encoding.disp_offset || X.encoding.disp_size)
      return false;
    const unsigned Registers[]{unsigned((ModRM >> 3) & 7) + ((Rex & 4) ? 8 : 0),
                               unsigned(ModRM & 7) + ((Rex & 1) ? 8 : 0)};
    for (unsigned I = 0; I != 2; ++I) {
      const auto &Operand = X.operands[I];
      if (Operand.type != X86_OP_REG || Operand.size != 4)
        return false;
      const auto Register = mapCapstoneReg(static_cast<x86_reg>(Operand.reg));
      if (Register.Offset != Registers[I] * 8 || Register.Size != 4)
        return false;
    }
    return true;
  }
  const unsigned Extension = Insn->id == X86_INS_SGDT ? 0 : 5;
  if ((ModRM >> 6) == 3 || ((ModRM >> 3) & 7) != Extension || (Rex & 4))
    return false;
  const auto SegmentReg = Segment == 0x64   ? X86_REG_FS
                          : Segment == 0x65 ? X86_REG_GS
                                            : X86_REG_INVALID;
  return legacyaudit::memoryOperand(
      Insn, X.operands[0], Insn->id == X86_INS_SGDT ? 10 : 2, AddressSize, true,
      Rex, Pos + 3, 0, SegmentReg);
}

} // namespace neverd::descriptorqueryaudit

#endif // NEVERD_LIB_LIFT_X86_X86DESCRIPTORQUERYAUDIT_H
