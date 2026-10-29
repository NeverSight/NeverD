//===- X86LockedLogicAudit.h - Locked logical effect audit ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate one legacy locked logical memory transaction.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86LOCKEDLOGICAUDIT_H
#define NEVERD_LIB_LIFT_X86_X86LOCKEDLOGICAUDIT_H

#include "X86LegacyMemoryAudit.h"

namespace neverd::lockedlogicaudit {

inline bool form(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Target != Arch::X64 || !Insn->size ||
      Insn->size > 15 ||
      (Insn->id != X86_INS_AND && Insn->id != X86_INS_OR &&
       Insn->id != X86_INS_XOR))
    return false;
  const auto &X = Insn->detail->x86;
  uint8_t Lock = 0, Operand = 0, Address = 0, Segment = 0, Rex = 0;
  size_t Pos = 0;
  for (; Pos < Insn->size; ++Pos) {
    const auto Byte = Insn->bytes[Pos];
    if (Byte == 0xf0 && !Lock)
      Lock = Byte;
    else if (Byte == 0x66 && !Operand)
      Operand = Byte;
    else if (Byte == 0x67 && !Address)
      Address = Byte;
    else if ((Byte == 0x64 || Byte == 0x65) && !Segment)
      Segment = Byte;
    else
      break;
  }
  if (Pos < Insn->size && Insn->bytes[Pos] >= 0x40 && Insn->bytes[Pos] <= 0x4f)
    Rex = Insn->bytes[Pos++];
  if (!Lock || Pos + 2 > Insn->size || X.op_count != 2)
    return false;
  const auto Opcode = Insn->bytes[Pos], ModRM = Insn->bytes[Pos + 1];
  const unsigned Group = Insn->id == X86_INS_AND  ? 4
                         : Insn->id == X86_INS_OR ? 1
                                                  : 6;
  const unsigned RegisterOpcode = Group * 8;
  const bool Immediate = Opcode == 0x80 || Opcode == 0x81 || Opcode == 0x83;
  const bool Byte = Opcode == 0x80 || Opcode == RegisterOpcode;
  const unsigned Width = Byte ? 1 : (Rex & 8) ? 8 : Operand ? 2 : 4;
  const unsigned ImmediateBytes = !Immediate       ? 0
                                  : Opcode == 0x81 ? (Width == 2 ? 2 : 4)
                                                   : 1;
  const unsigned AddressSize = Address ? 4 : 8;
  if ((!Immediate && Opcode != RegisterOpcode &&
       Opcode != RegisterOpcode + 1) ||
      (Immediate && (((ModRM >> 3) & 7) != Group || (Rex & 4))) ||
      (ModRM >> 6) == 3 || X.modrm != ModRM || X.rex != Rex ||
      X.opcode[0] != Opcode || X.opcode[1] || X.opcode[2] || X.opcode[3] ||
      X.prefix[0] != Lock || X.prefix[1] != Segment || X.prefix[2] != Operand ||
      X.prefix[3] != Address || X.addr_size != AddressSize ||
      X.encoding.modrm_offset != Pos + 1 ||
      X.encoding.imm_size != ImmediateBytes ||
      X.encoding.imm_offset !=
          (ImmediateBytes ? Insn->size - ImmediateBytes : 0) ||
      X.avx_sae || X.avx_rm != X86_AVX_RM_INVALID ||
      X.avx_cc != X86_AVX_CC_INVALID)
    return false;
  for (const auto &Value : llvm::ArrayRef(X.operands, X.op_count))
    if (Value.avx_bcast != X86_AVX_BCAST_INVALID || Value.avx_zero_opmask)
      return false;
  const auto SegmentReg = Segment == 0x64   ? X86_REG_FS
                          : Segment == 0x65 ? X86_REG_GS
                                            : X86_REG_INVALID;
  if (!legacyaudit::memoryOperand(Insn, X.operands[0], Width, AddressSize, true,
                                  Rex, Pos + 2, ImmediateBytes, SegmentReg))
    return false;
  const auto &Source = X.operands[1];
  if (Source.size != Width)
    return false;
  if (!Immediate) {
    const unsigned Number = ((ModRM >> 3) & 7) + ((Rex & 4) ? 8 : 0);
    const unsigned Offset =
        Width == 1 && !Rex && Number >= 4 ? (Number - 4) * 8 + 1 : Number * 8;
    const auto Register = mapCapstoneReg(static_cast<x86_reg>(Source.reg));
    return Source.type == X86_OP_REG && Register.Offset == Offset &&
           Register.Size == Width;
  }
  if (Source.type != X86_OP_IMM || Insn->size < ImmediateBytes)
    return false;
  uint64_t Raw = 0;
  for (unsigned I = 0; I != ImmediateBytes; ++I)
    Raw |= uint64_t(Insn->bytes[Insn->size - ImmediateBytes + I]) << (I * 8);
  if (ImmediateBytes < Width && (Raw >> (ImmediateBytes * 8 - 1)))
    Raw |= UINT64_MAX << (ImmediateBytes * 8);
  const uint64_t Mask =
      Width == 8 ? UINT64_MAX : (uint64_t{1} << (Width * 8)) - 1;
  return (uint64_t(Source.imm) & Mask) == (Raw & Mask);
}

} // namespace neverd::lockedlogicaudit
#endif
