//===- X86LockedUnary.h - Locked increment/decrement forms ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate legacy LOCK INC/DEC memory forms before atomic lowering.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86LOCKEDUNARY_H
#define NEVERD_LIB_LIFT_X86_X86LOCKEDUNARY_H

#include "X86LegacyMemoryAudit.h"

namespace neverd::lockedunary {

inline bool form(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || (Target != Arch::X64 && Target != Arch::X86) ||
      (Insn->id != X86_INS_INC && Insn->id != X86_INS_DEC) || !Insn->size ||
      Insn->size > 15)
    return false;
  const auto &X = Insn->detail->x86;
  bool Locked = false;
  uint8_t Operand = 0, Address = 0, Segment = 0, Rex = 0;
  size_t Pos = 0;
  for (; Pos < Insn->size; ++Pos) {
    const auto B = Insn->bytes[Pos];
    if (B == 0xf0 && !Locked)
      Locked = true;
    else if (B == 0x66 && !Operand)
      Operand = B;
    else if (B == 0x67 && !Address)
      Address = B;
    else if ((B == 0x64 || B == 0x65) && !Segment)
      Segment = B;
    else
      break;
  }
  if (Target == Arch::X64 && Pos < Insn->size && Insn->bytes[Pos] >= 0x40 &&
      Insn->bytes[Pos] <= 0x4f)
    Rex = Insn->bytes[Pos++];
  if (!Locked || Pos + 2 > Insn->size ||
      (Insn->bytes[Pos] != 0xfe && Insn->bytes[Pos] != 0xff))
    return false;
  const uint8_t Opcode = Insn->bytes[Pos], ModRM = Insn->bytes[Pos + 1];
  const unsigned Group = Insn->id == X86_INS_INC ? 0 : 1;
  const unsigned Width = Opcode == 0xfe ? 1 : Rex & 8 ? 8 : Operand ? 2 : 4;
  const unsigned AddressSize =
      Target == Arch::X64 ? (Address ? 4 : 8) : (Address ? 2 : 4);
  if (ModRM >> 6 == 3 || ((ModRM >> 3) & 7) != Group || (Rex & 4) ||
      X.op_count != 1 || X.opcode[0] != Opcode || X.opcode[1] || X.opcode[2] ||
      X.opcode[3] || X.modrm != ModRM || X.rex != Rex || X.prefix[0] != 0xf0 ||
      X.prefix[1] != Segment || X.prefix[2] != Operand ||
      X.prefix[3] != Address || X.addr_size != AddressSize ||
      X.encoding.modrm_offset != Pos + 1 || X.encoding.imm_size ||
      X.encoding.imm_offset || X.avx_sae || X.avx_rm != X86_AVX_RM_INVALID ||
      X.avx_cc != X86_AVX_CC_INVALID)
    return false;
  const auto &Memory = X.operands[0];
  if (Memory.access != (CS_AC_READ | CS_AC_WRITE) ||
      Memory.avx_bcast != X86_AVX_BCAST_INVALID || Memory.avx_zero_opmask)
    return false;
  const auto SegmentReg = Segment == 0x64   ? X86_REG_FS
                          : Segment == 0x65 ? X86_REG_GS
                                            : X86_REG_INVALID;
  return legacyaudit::memoryOperand(Insn, Memory, Width, AddressSize,
                                    Target == Arch::X64, Rex, Pos + 2, 0,
                                    SegmentReg);
}

} // namespace neverd::lockedunary

#endif // NEVERD_LIB_LIFT_X86_X86LOCKEDUNARY_H
