//===- X86DivisionUndefined.h - Unsigned DIV flag audit -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind legacy unsigned 64-bit register division to its arbitrary flags.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86DIVISIONUNDEFINED_H
#define NEVERD_LIB_LIFT_X86_X86DIVISIONUNDEFINED_H

#include "X86UndefinedFlagAudit.h"

#include "neverd/lift/X86/X86Lifter.h"

#include <array>

namespace neverd::divisionundefined {

// Intel SDM 253666-093US, DIV, Flags Affected. These productions occur only
// after successful division; neither a divide error nor an admission failure
// may publish them. DF and all other flags retain their incoming values.
inline constexpr std::array<unsigned, 6> Flags = {
    x86reg::OF, x86reg::SF, x86reg::ZF, x86reg::AF, x86reg::PF, x86reg::CF};

inline void record(X86Lifter::LiftState &S) {
  for (const auto Flag : Flags)
    S.recordUndefinedBits(NdVar::reg(Flag, 1), 0, 1);
}

inline bool form(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Target != Arch::X64 ||
      Insn->id != X86_INS_DIV || Insn->size != 3)
    return false;
  const auto &X = Insn->detail->x86;
  const uint8_t Rex = Insn->bytes[0], ModRM = Insn->bytes[2];
  if ((Rex != 0x48 && Rex != 0x49) || Insn->bytes[1] != 0xf7 ||
      (ModRM & 0xf8) != 0xf0 || X.op_count != 1 || X.modrm != ModRM ||
      X.encoding.modrm_offset != 2 || X.opcode[0] != 0xf7 || X.opcode[1] ||
      X.opcode[2] || X.opcode[3] || X.rex != Rex || X.prefix[0] ||
      X.prefix[1] || X.prefix[2] || X.prefix[3] || X.addr_size != 8 || X.sib ||
      X.disp || X.encoding.disp_size || X.encoding.disp_offset ||
      X.encoding.imm_size || X.encoding.imm_offset || X.avx_sae ||
      X.avx_rm != X86_AVX_RM_INVALID || X.avx_cc != X86_AVX_CC_INVALID)
    return false;
  const auto &Source = X.operands[0];
  if (Source.type != X86_OP_REG || Source.size != 8 || Source.avx_zero_opmask ||
      Source.avx_bcast != X86_AVX_BCAST_INVALID)
    return false;
  const auto R = mapCapstoneReg(static_cast<x86_reg>(Source.reg));
  return R.Size == 8 && R.Offset == ((ModRM & 7) + ((Rex & 1) ? 8 : 0)) * 8;
}

inline bool matches(llvm::ArrayRef<LowOp> Ops,
                    const LowInstructionUndefinedEffects &Effects) {
  return undefinedflags::matches(Flags, Ops, Effects);
}

} // namespace neverd::divisionundefined

#endif // NEVERD_LIB_LIFT_X86_X86DIVISIONUNDEFINED_H
