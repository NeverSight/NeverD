//===- X86ReadPrefetchAudit.h - Read cache-hint audit ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate default-space legacy read hints independently of write hints.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86READPREFETCHAUDIT_H
#define NEVERD_LIB_LIFT_X86_X86READPREFETCHAUDIT_H

#include "X86LegacyMemoryAudit.h"

namespace neverd::readprefetchaudit {

// Intel SDM Vol. 2B, rev. 093, PREFETCHh: these four hints introduce no
// undefined architectural output. Preserve the actual hint in LowIR. This
// closed form excludes segment overrides, LOCK/REP, write and code hints.
inline bool form(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Target != Arch::X64 || !Insn->size ||
      Insn->size > 15)
    return false;
  unsigned Hint;
  switch (Insn->id) {
  case X86_INS_PREFETCHNTA:
    Hint = 0;
    break;
  case X86_INS_PREFETCHT0:
    Hint = 1;
    break;
  case X86_INS_PREFETCHT1:
    Hint = 2;
    break;
  case X86_INS_PREFETCHT2:
    Hint = 3;
    break;
  default:
    return false;
  }
  const auto &X = Insn->detail->x86;
  size_t Pos = 0;
  const bool Address32 = Insn->bytes[Pos] == 0x67;
  if (Address32)
    ++Pos;
  uint8_t Rex = 0;
  if (Pos < Insn->size && Insn->bytes[Pos] >= 0x40 && Insn->bytes[Pos] <= 0x43)
    Rex = Insn->bytes[Pos++];
  if (Pos + 3 > Insn->size || Insn->bytes[Pos] != 0x0f ||
      Insn->bytes[Pos + 1] != 0x18)
    return false;
  const uint8_t ModRM = Insn->bytes[Pos + 2];
  const unsigned AddressSize = Address32 ? 4 : 8;
  if ((ModRM >> 6) == 3 || ((ModRM >> 3) & 7) != Hint || X.op_count != 1 ||
      X.modrm != ModRM || X.encoding.modrm_offset != Pos + 2 ||
      X.opcode[0] != 0x0f || X.opcode[1] != 0x18 || X.opcode[2] ||
      X.opcode[3] || X.rex != Rex || X.prefix[0] || X.prefix[1] ||
      X.prefix[2] || X.prefix[3] != (Address32 ? 0x67 : 0) ||
      X.addr_size != AddressSize || X.encoding.imm_size ||
      X.encoding.imm_offset || X.avx_sae || X.avx_rm != X86_AVX_RM_INVALID ||
      X.avx_cc != X86_AVX_CC_INVALID || X.eflags)
    return false;
  const auto &Memory = X.operands[0];
  if (Memory.access != CS_AC_READ ||
      Memory.avx_bcast != X86_AVX_BCAST_INVALID || Memory.avx_zero_opmask)
    return false;
  return legacyaudit::memoryOperand(Insn, Memory, 1, AddressSize, true, Rex,
                                    Pos + 3);
}

} // namespace neverd::readprefetchaudit

#endif
