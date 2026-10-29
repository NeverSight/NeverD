//===- X86MultiplyUndefined.h - Multiply effect audit ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind legacy 64-bit register products to their four arbitrary flag bits.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86MULTIPLYUNDEFINED_H
#define NEVERD_LIB_LIFT_X86_X86MULTIPLYUNDEFINED_H

#include "X86LiftDetail.h"

#include <array>

namespace neverd::multiplyundefined {

// Intel SDM MUL/IMUL: CF and OF describe overflow; SF, ZF, AF and PF are
// undefined. DF and the other system flags remain unchanged.
inline constexpr std::array<unsigned, 4> Flags = {x86reg::SF, x86reg::ZF,
                                                  x86reg::AF, x86reg::PF};

inline void record(X86Lifter::LiftState &State) {
  for (unsigned Flag : Flags)
    State.recordUndefinedBits(NdVar::reg(Flag, 1), 0, 1);
}

inline bool matches(llvm::ArrayRef<LowOp> Ops,
                    const LowInstructionUndefinedEffects &Effects) {
  if (Effects.Effects.size() != Flags.size())
    return false;
  for (size_t I = 0; I != Flags.size(); ++I) {
    const auto &Effect = Effects.Effects[I];
    if (Effect.Output != NdVar::reg(Flags[I], 1) || Effect.BitOffset ||
        Effect.BitCount != 1 || Effect.When || Effect.AfterOp == 0 ||
        Effect.AfterOp > Ops.size())
      return false;
    for (const auto &Later : Ops.drop_front(Effect.AfterOp)) {
      if (Later.Output == Effect.Output)
        return false;
      for (unsigned J = 0; J != Later.NumInputs; ++J)
        if (Later.Inputs[J] == Effect.Output)
          return false;
    }
  }
  return true;
}

inline bool form(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Target != Arch::X64 ||
      (Insn->id != X86_INS_MUL && Insn->id != X86_INS_IMUL) || Insn->size < 3 ||
      Insn->size > 4)
    return false;
  const auto &X = Insn->detail->x86;
  const uint8_t Rex = Insn->bytes[0];
  const bool Pair = Insn->size == 4;
  if (Rex < 0x48 || Rex > 0x4f || (Rex & 2) ||
      (Pair ? Insn->id != X86_INS_IMUL || Insn->bytes[1] != 0x0f ||
                  Insn->bytes[2] != 0xaf
            : Insn->bytes[1] != 0xf7 || (Rex & 4)))
    return false;
  const uint8_t ModRM = Insn->bytes[Insn->size - 1];
  if ((ModRM >> 6) != 3 ||
      (!Pair && ((ModRM >> 3) & 7) != (Insn->id == X86_INS_MUL ? 4 : 5)) ||
      X.op_count != (Pair ? 2 : 1) || X.modrm != ModRM || X.rex != Rex ||
      X.prefix[0] || X.prefix[1] || X.prefix[2] || X.prefix[3] ||
      X.addr_size != 8 || X.opcode[0] != (Pair ? 0x0f : 0xf7) ||
      X.opcode[1] != (Pair ? 0xaf : 0) || X.opcode[2] || X.opcode[3] ||
      X.encoding.modrm_offset != Insn->size - 1 || X.encoding.disp_size ||
      X.encoding.disp_offset || X.disp || X.sib || X.encoding.imm_size ||
      X.encoding.imm_offset || X.avx_sae || X.avx_rm != X86_AVX_RM_INVALID ||
      X.avx_cc != X86_AVX_CC_INVALID)
    return false;
  for (unsigned I = 0; I != X.op_count; ++I) {
    const auto &Operand = X.operands[I];
    const unsigned Number = Pair && I == 0
                                ? ((ModRM >> 3) & 7) + ((Rex & 4) ? 8 : 0)
                                : (ModRM & 7) + ((Rex & 1) ? 8 : 0);
    const auto Register = mapCapstoneReg(static_cast<x86_reg>(Operand.reg));
    if (Operand.type != X86_OP_REG || Operand.size != 8 ||
        Register.Offset != Number * 8 || Register.Size != 8 ||
        Operand.avx_bcast != X86_AVX_BCAST_INVALID || Operand.avx_zero_opmask)
      return false;
  }
  return true;
}

} // namespace neverd::multiplyundefined
#endif
