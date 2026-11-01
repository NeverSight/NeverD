//===- X86LockedCompareExchangeAudit.h - Scalar CMPXCHG audit -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate one legacy locked scalar compare-exchange transaction.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86LOCKEDCOMPAREEXCHANGEAUDIT_H
#define NEVERD_LIB_LIFT_X86_X86LOCKEDCOMPAREEXCHANGEAUDIT_H

#include "X86LegacyMemoryAudit.h"

#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>

namespace neverd::lockedcompareexchangeaudit {

// Intel SDM Vol. 2, CMPXCHG: subtraction defines all six arithmetic flags.
// This receipt covers scalar memory forms only, not CMPXCHG8B/CMPXCHG16B.
inline bool form(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Target != Arch::X64 || !Insn->size ||
      Insn->size > x86::kMaxInstructionLen || Insn->id != X86_INS_CMPXCHG)
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
  if (!Lock || Pos + 3 > Insn->size || X.op_count != 2 ||
      Insn->bytes[Pos] != 0x0f)
    return false;
  const auto Opcode = Insn->bytes[Pos + 1], ModRM = Insn->bytes[Pos + 2];
  const unsigned Width = Opcode == 0xb0 ? 1 : (Rex & 8) ? 8 : Operand ? 2 : 4;
  const unsigned AddressSize = Address ? 4 : 8;
  if ((Opcode != 0xb0 && Opcode != 0xb1) || (ModRM >> 6) == 3 ||
      X.modrm != ModRM || X.rex != Rex || X.opcode[0] != 0x0f ||
      X.opcode[1] != Opcode || X.opcode[2] || X.opcode[3] ||
      X.prefix[0] != Lock || X.prefix[1] != Segment || X.prefix[2] != Operand ||
      X.prefix[3] != Address || X.addr_size != AddressSize ||
      X.encoding.modrm_offset != Pos + 2 || X.encoding.imm_size ||
      X.encoding.imm_offset || X.avx_sae || X.avx_rm != X86_AVX_RM_INVALID ||
      X.avx_cc != X86_AVX_CC_INVALID ||
      X.eflags !=
          (X86_EFLAGS_MODIFY_AF | X86_EFLAGS_MODIFY_CF | X86_EFLAGS_MODIFY_SF |
           X86_EFLAGS_MODIFY_ZF | X86_EFLAGS_MODIFY_PF | X86_EFLAGS_MODIFY_OF))
    return false;
  for (const auto &Value : llvm::ArrayRef(X.operands, X.op_count))
    if (Value.avx_bcast != X86_AVX_BCAST_INVALID || Value.avx_zero_opmask)
      return false;
  const auto SegmentReg = Segment == 0x64   ? X86_REG_FS
                          : Segment == 0x65 ? X86_REG_GS
                                            : X86_REG_INVALID;
  // Capstone marks this destination read-only. Authenticate that decoder
  // representation; the locked raw opcode owns the unconditional RMW cycle.
  if (X.operands[0].access != CS_AC_READ ||
      !legacyaudit::memoryOperand(Insn, X.operands[0], Width, AddressSize, true,
                                  Rex, Pos + 3, 0, SegmentReg))
    return false;
  const auto &Source = X.operands[1];
  const unsigned Number = ((ModRM >> 3) & 7) + ((Rex & 4) ? 8 : 0);
  const unsigned Offset =
      Width == 1 && !Rex && Number >= 4 ? (Number - 4) * 8 + 1 : Number * 8;
  const auto Register = mapCapstoneReg(static_cast<x86_reg>(Source.reg));
  return Source.type == X86_OP_REG && Source.access == CS_AC_READ &&
         Source.size == Width && Register.Offset == Offset &&
         Register.Size == Width;
}

} // namespace neverd::lockedcompareexchangeaudit

#endif // NEVERD_LIB_LIFT_X86_X86LOCKEDCOMPAREEXCHANGEAUDIT_H
