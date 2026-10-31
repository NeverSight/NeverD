//===- X86MultiplyUndefined.h - Multiply effect audit ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind legacy 16/32/64-bit register products to four arbitrary flag bits.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86MULTIPLYUNDEFINED_H
#define NEVERD_LIB_LIFT_X86_X86MULTIPLYUNDEFINED_H

#include "X86LiftDetail.h"

#include <array>
#include <cstdint>

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
      (Insn->id != X86_INS_MUL && Insn->id != X86_INS_IMUL) || Insn->size < 2 ||
      Insn->size > 15)
    return false;
  const auto &X = Insn->detail->x86;
  unsigned Pos = 0;
  const uint8_t Prefix = Insn->bytes[Pos] == 0x66 ? Insn->bytes[Pos++] : 0;
  uint8_t Rex = 0;
  if (Pos < Insn->size && Insn->bytes[Pos] >= 0x40 && Insn->bytes[Pos] <= 0x4f)
    Rex = Insn->bytes[Pos++];
  // Register-only canonical forms. REX.X and redundant operand prefixes do
  // not widen admission to other addressing or encoding families.
  if (Pos + 2 > Insn->size || (Rex & 2) || (Prefix && (Rex & 8)))
    return false;
  const unsigned Width = (Rex & 8) ? 8 : Prefix ? 2 : 4;
  const uint8_t Opcode = Insn->bytes[Pos];
  const bool Pair = Opcode == 0x0f;
  const bool Immediate = Opcode == 0x69 || Opcode == 0x6b;
  const unsigned ModRMOffset = Pos + (Pair ? 2 : 1);
  const unsigned ImmediateBytes = Immediate ? (Opcode == 0x6b ? 1
                                               : Width == 2   ? 2
                                                              : 4)
                                            : 0;
  if ((Pair || Immediate) ? Insn->id != X86_INS_IMUL
                          : Opcode != 0xf7 || (Rex & 4))
    return false;
  if (Insn->size != ModRMOffset + 1 + ImmediateBytes ||
      (Pair && Insn->bytes[Pos + 1] != 0xaf))
    return false;
  const uint8_t ModRM = Insn->bytes[ModRMOffset];
  const unsigned RegisterOperands = Pair || Immediate ? 2 : 1;
  if ((ModRM >> 6) != 3 ||
      (RegisterOperands == 1 &&
       ((ModRM >> 3) & 7) != (Insn->id == X86_INS_MUL ? 4 : 5)) ||
      X.op_count != RegisterOperands + Immediate || X.modrm != ModRM ||
      X.rex != Rex || X.prefix[0] || X.prefix[1] || X.prefix[2] != Prefix ||
      X.prefix[3] || X.addr_size != 8 || X.opcode[0] != Opcode ||
      X.opcode[1] != (Pair ? 0xaf : 0) || X.opcode[2] || X.opcode[3] ||
      X.encoding.modrm_offset != ModRMOffset || X.encoding.disp_size ||
      X.encoding.disp_offset || X.disp || X.sib ||
      X.encoding.imm_size != ImmediateBytes ||
      X.encoding.imm_offset != (Immediate ? ModRMOffset + 1 : 0) || X.avx_sae ||
      X.avx_rm != X86_AVX_RM_INVALID || X.avx_cc != X86_AVX_CC_INVALID)
    return false;
  for (unsigned I = 0; I != RegisterOperands; ++I) {
    const auto &Operand = X.operands[I];
    const unsigned Number = RegisterOperands == 2 && I == 0
                                ? ((ModRM >> 3) & 7) + ((Rex & 4) ? 8 : 0)
                                : (ModRM & 7) + ((Rex & 1) ? 8 : 0);
    const auto Register = mapCapstoneReg(static_cast<x86_reg>(Operand.reg));
    if (Operand.type != X86_OP_REG || Operand.size != Width ||
        Register.Offset != Number * 8 || Register.Size != Width ||
        Operand.avx_bcast != X86_AVX_BCAST_INVALID || Operand.avx_zero_opmask)
      return false;
  }
  if (!Immediate)
    return true;
  uint64_t Value = 0;
  for (unsigned I = 0; I != ImmediateBytes; ++I)
    Value |= uint64_t(Insn->bytes[ModRMOffset + 1 + I]) << (I * 8);
  // Capstone keeps a full-width immediate as unsigned bits. Only an encoded
  // immediate narrower than its operand is normalized to a signed value.
  if (ImmediateBytes < Width &&
      (Value & (UINT64_C(1) << (ImmediateBytes * 8 - 1))))
    Value |= UINT64_MAX << (ImmediateBytes * 8);
  const auto &Operand = X.operands[2];
  return Operand.type == X86_OP_IMM && Operand.size == Width &&
         static_cast<uint64_t>(Operand.imm) == Value &&
         Operand.avx_bcast == X86_AVX_BCAST_INVALID && !Operand.avx_zero_opmask;
}

} // namespace neverd::multiplyundefined
#endif
