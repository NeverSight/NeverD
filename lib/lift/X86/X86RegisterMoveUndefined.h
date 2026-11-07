//===- X86RegisterMoveUndefined.h - System move flags audit ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind control/debug register moves to their architectural arbitrary flags.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86REGISTERMOVEUNDEFINED_H
#define NEVERD_LIB_LIFT_X86_X86REGISTERMOVEUNDEFINED_H

#include "X86UndefinedFlagAudit.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86/X86Lifter.h"

#include <array>

namespace neverd::registermoveundefined {

// Intel SDM Vol. 2B, MOV to/from control and debug registers, Flags Affected.
// A successful transfer leaves these six bits undefined; DF is preserved.
inline constexpr std::array<unsigned, 6> Flags = {
    x86reg::OF, x86reg::SF, x86reg::ZF, x86reg::AF, x86reg::PF, x86reg::CF};

inline bool supportsControlIndex(unsigned Index) {
  return Index == 2 || Index == 3 || Index == 4 || Index == 8;
}

inline void record(X86Lifter::LiftState &S) {
  for (const auto Flag : Flags)
    S.recordUndefinedBits(NdVar::reg(Flag, 1), 0, 1);
}

inline bool form(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Target != Arch::X64 ||
      Insn->id != X86_INS_MOV || (Insn->size != 3 && Insn->size != 4))
    return false;
  const auto &X = Insn->detail->x86;
  const unsigned Prefix = Insn->size == 4 ? 1 : 0;
  const uint8_t Rex = Prefix ? Insn->bytes[0] : 0;
  const uint8_t Opcode = Insn->bytes[Prefix + 1];
  const uint8_t ModRM = Insn->bytes[Prefix + 2];
  const bool Debug = Opcode == 0x21 || Opcode == 0x23;
  const bool Read = Opcode == 0x20 || Opcode == 0x21;
  const unsigned Index = ((ModRM >> 3) & 7) + ((Rex & 4) ? 8 : 0);
  const bool ValidRex = Debug ? Rex >= 0x40 && Rex <= 0x49 && !(Rex & 6)
                              : Rex >= 0x40 && Rex <= 0x45 && !(Rex & 2);
  // DR4/DR5 aliases depend on CR4.DE and retain their native boundary.
  const bool ValidIndex = Debug ? Index < 4 || Index == 6 || Index == 7
                                : supportsControlIndex(Index);
  if ((Prefix && !ValidRex) || Insn->bytes[Prefix] != 0x0f ||
      (Opcode != 0x20 && Opcode != 0x22 && !Debug) || (ModRM >> 6) != 3 ||
      !ValidIndex || X.op_count != 2 || X.modrm != ModRM ||
      X.encoding.modrm_offset != Prefix + 2 || X.opcode[0] != 0x0f ||
      X.opcode[1] != Opcode || X.opcode[2] || X.opcode[3] || X.rex != Rex ||
      X.prefix[0] || X.prefix[1] || X.prefix[2] || X.prefix[3] ||
      X.addr_size != 8 || X.sib || X.disp || X.encoding.disp_size ||
      X.encoding.disp_offset || X.encoding.imm_size || X.encoding.imm_offset ||
      X.avx_sae || X.avx_rm != X86_AVX_RM_INVALID ||
      X.avx_cc != X86_AVX_CC_INVALID)
    return false;
  const auto &General = X.operands[Read ? 0 : 1];
  const auto &System = X.operands[Read ? 1 : 0];
  if (General.type != X86_OP_REG || System.type != X86_OP_REG ||
      General.size != 8 || System.size != 8 ||
      System.reg != (Debug ? X86_REG_DR0 : X86_REG_CR0) + Index ||
      General.avx_zero_opmask || System.avx_zero_opmask ||
      General.avx_bcast != X86_AVX_BCAST_INVALID ||
      System.avx_bcast != X86_AVX_BCAST_INVALID)
    return false;
  const auto R = mapCapstoneReg(static_cast<x86_reg>(General.reg));
  return R.Size == 8 && R.Offset == ((ModRM & 7) + ((Rex & 1) ? 8 : 0)) * 8;
}

inline bool matches(llvm::ArrayRef<LowOp> Ops,
                    const LowInstructionUndefinedEffects &Effects) {
  return undefinedflags::matches(Flags, Ops, Effects);
}

} // namespace neverd::registermoveundefined

#endif // NEVERD_LIB_LIFT_X86_X86REGISTERMOVEUNDEFINED_H
