//===- X86DivisionUndefined.h - DIV/IDIV flag audit ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind canonical register division in both x86 modes to its arbitrary flags.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86DIVISIONUNDEFINED_H
#define NEVERD_LIB_LIFT_X86_X86DIVISIONUNDEFINED_H

#include "X86UndefinedFlagAudit.h"

#include "neverd/lift/X86/X86Lifter.h"

#include <array>

namespace neverd::divisionundefined {

// Intel SDM, DIV/IDIV, Flags Affected. These productions occur only
// after successful division; neither a divide error nor an admission failure
// may publish them. DF and all other flags retain their incoming values.
inline constexpr std::array<unsigned, 6> Flags = {
    x86reg::OF, x86reg::SF, x86reg::ZF, x86reg::AF, x86reg::PF, x86reg::CF};

inline void record(X86Lifter::LiftState &S) {
  for (const auto Flag : Flags)
    S.recordUndefinedBits(NdVar::reg(Flag, 1), 0, 1);
}

inline bool form(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || (Target != Arch::X64 && Target != Arch::X86) ||
      (Insn->id != X86_INS_DIV && Insn->id != X86_INS_IDIV) || Insn->size < 2 ||
      Insn->size > 4)
    return false;
  const auto &X = Insn->detail->x86;
  unsigned Pos = 0;
  const uint8_t Prefix = Insn->bytes[Pos] == 0x66 ? Insn->bytes[Pos++] : 0;
  uint8_t Rex = 0;
  if (Target == Arch::X64 && Pos < Insn->size && Insn->bytes[Pos] >= 0x40 &&
      Insn->bytes[Pos] <= 0x4f)
    Rex = Insn->bytes[Pos++];
  if (Pos + 2 != Insn->size || (Rex & 6) || (Prefix && (Rex & 8)))
    return false;
  const uint8_t Opcode = Insn->bytes[Pos], ModRM = Insn->bytes[Pos + 1];
  const bool Byte = Opcode == 0xf6;
  if ((!Byte && Opcode != 0xf7) || (Byte && (Prefix || (Rex & 8))))
    return false;
  const unsigned Width = Byte ? 1 : (Rex & 8) ? 8 : Prefix ? 2 : 4;
  const unsigned Number = (ModRM & 7) + ((Rex & 1) ? 8 : 0);
  const unsigned Offset =
      Byte && !Rex && Number >= 4 ? (Number - 4) * 8 + 1 : Number * 8;
  if ((ModRM & 0xf8) != (Insn->id == X86_INS_DIV ? 0xf0 : 0xf8) ||
      X.op_count != 1 || X.modrm != ModRM ||
      X.encoding.modrm_offset != Pos + 1 || X.opcode[0] != Opcode ||
      X.opcode[1] || X.opcode[2] || X.opcode[3] || X.rex != Rex ||
      X.prefix[0] || X.prefix[1] || X.prefix[2] != Prefix || X.prefix[3] ||
      X.addr_size != (Target == Arch::X64 ? 8 : 4) || X.sib || X.disp ||
      X.encoding.disp_size || X.encoding.disp_offset || X.encoding.imm_size ||
      X.encoding.imm_offset || X.avx_sae || X.avx_rm != X86_AVX_RM_INVALID ||
      X.avx_cc != X86_AVX_CC_INVALID)
    return false;
  const auto &Source = X.operands[0];
  if (Source.type != X86_OP_REG || Source.size != Width ||
      Source.avx_zero_opmask || Source.avx_bcast != X86_AVX_BCAST_INVALID)
    return false;
  const auto R = mapCapstoneReg(static_cast<x86_reg>(Source.reg));
  return R.Size == Width && R.Offset == Offset;
}

inline bool matches(llvm::ArrayRef<LowOp> Ops,
                    const LowInstructionUndefinedEffects &Effects) {
  return undefinedflags::matches(Flags, Ops, Effects);
}

} // namespace neverd::divisionundefined

#endif // NEVERD_LIB_LIFT_X86_X86DIVISIONUNDEFINED_H
