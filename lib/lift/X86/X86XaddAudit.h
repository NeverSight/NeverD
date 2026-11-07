//===- X86XaddAudit.h - Legacy XADD effect audit ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86XADDAUDIT_H
#define NEVERD_LIB_LIFT_X86_X86XADDAUDIT_H

#include "X86LegacyMemoryAudit.h"

namespace neverd::xaddaudit {

// Intel SDM, Vol. 2D, XADD: the addition defines all six arithmetic
// flags, and DF is preserved. Audit legacy register and unsegmented memory
// forms, including one LOCK on memory. HLE, APX and duplicate prefixes refuse.
inline bool form(const cs_insn *Insn, Arch Target) {
  const auto &X = Insn->detail->x86;
  if (Insn->id != X86_INS_XADD || X.op_count != 2 || Insn->size > 15)
    return false;
  const bool Long = Target == Arch::X64;
  size_t Pos = 0;
  uint8_t LockPrefix = 0, OperandPrefix = 0, AddressPrefix = 0, Rex = 0;
  for (; Pos < Insn->size; ++Pos) {
    const uint8_t B = Insn->bytes[Pos];
    if (B == 0xf0 && !LockPrefix && !Rex) {
      LockPrefix = B;
    } else if (B == 0x66) {
      if (OperandPrefix || Rex)
        return false;
      OperandPrefix = B;
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
  if (Pos + 3 > Insn->size || Insn->bytes[Pos] != 0x0f)
    return false;
  const uint8_t Opcode = Insn->bytes[Pos + 1];
  const uint8_t ModRM = Insn->bytes[Pos + 2];
  const unsigned AddressSize =
      Long ? (AddressPrefix ? 4 : 8) : (AddressPrefix ? 2 : 4);
  if ((Opcode != 0xc0 && Opcode != 0xc1) || X.modrm != ModRM ||
      X.encoding.modrm_offset != Pos + 2 || X.opcode[0] != 0x0f ||
      X.opcode[1] != Opcode || X.opcode[2] || X.opcode[3] || X.rex != Rex ||
      X.prefix[0] != LockPrefix || X.prefix[1] ||
      X.prefix[2] != OperandPrefix || X.prefix[3] != AddressPrefix ||
      X.addr_size != AddressSize || X.encoding.imm_size ||
      X.encoding.imm_offset)
    return false;
  const unsigned Width = Opcode == 0xc0  ? 1
                         : (Rex & 8)     ? 8
                         : OperandPrefix ? 2
                                         : 4;
  const auto Register = [&](const cs_x86_op &Operand, unsigned Number) {
    if (Operand.type != X86_OP_REG || Operand.size != Width)
      return false;
    const uint64_t Offset =
        Width == 1 && !Rex && Number >= 4 ? (Number - 4) * 8 + 1 : Number * 8;
    const auto R = mapCapstoneReg(static_cast<x86_reg>(Operand.reg));
    return R.Offset == Offset && R.Size == Width;
  };
  if (!Register(X.operands[1], ((ModRM >> 3) & 7) + ((Rex & 4) ? 8 : 0)))
    return false;
  const unsigned Mod = ModRM >> 6, RM = ModRM & 7;
  if (Mod == 3)
    return !LockPrefix && !AddressPrefix && !(Rex & 2) &&
           Pos + 3 == Insn->size && !X.encoding.disp_size &&
           !X.encoding.disp_offset &&
           Register(X.operands[0], RM + ((Rex & 1) ? 8 : 0));

  return legacyaudit::memoryOperand(Insn, X.operands[0], Width, AddressSize,
                                    Long, Rex, Pos + 3);
}

} // namespace neverd::xaddaudit

#endif
