//===- X86BitTestUndefined.h - Legacy bit-test effect audit ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind legacy register and memory bit tests to their undefined flags.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86BITTESTUNDEFINED_H
#define NEVERD_LIB_LIFT_X86_X86BITTESTUNDEFINED_H

#include "X86LegacyMemoryAudit.h"

#include "neverd/lift/X86/X86Lifter.h"

#include <array>

namespace neverd::bitundefined {

inline bool isBitTest(unsigned Id) {
  return Id == X86_INS_BT || Id == X86_INS_BTS || Id == X86_INS_BTR ||
         Id == X86_INS_BTC;
}

// Intel SDM 093, Vol. 2A, BT/BTC/BTR/BTS, Flags Affected. CF is defined;
// ZF and DF are preserved. These four outputs are fresh bits, not bytes.
inline constexpr std::array<unsigned, 4> Flags = {x86reg::OF, x86reg::SF,
                                                  x86reg::AF, x86reg::PF};

inline void record(X86Lifter::LiftState &S) {
  for (const auto Flag : Flags)
    S.recordUndefinedBits(NdVar::reg(Flag, 1), 0, 1);
}

inline bool matches(llvm::ArrayRef<LowOp> Ops,
                    const LowInstructionUndefinedEffects &Effects) {
  if (Effects.Effects.size() != Flags.size())
    return false;
  for (size_t I = 0; I != Flags.size(); ++I) {
    const auto &E = Effects.Effects[I];
    if (E.Output != NdVar::reg(Flags[I], 1) || E.BitOffset != 0 ||
        E.BitCount != 1 || E.When || E.AfterOp == 0 || E.AfterOp > Ops.size())
      return false;
    for (const auto &Later : Ops.drop_front(E.AfterOp)) {
      if (Later.Output == E.Output)
        return false;
      for (unsigned J = 0; J < Later.NumInputs; ++J)
        if (Later.Inputs[J] == E.Output)
          return false;
    }
  }
  return true;
}

// The raw ModRM, SIB, displacement, immediate and FS/GS prefix must agree
// with the detail. LOCK is admitted only on memory updates; REP, duplicates
// and APX remain outside this audit. The lifter retains one atomic update.
inline bool form(const cs_insn *Insn, Arch Target) {
  const auto &X = Insn->detail->x86;
  if (!isBitTest(Insn->id) || X.op_count != 2 || Insn->size > 15 || X.avx_sae ||
      X.avx_rm != X86_AVX_RM_INVALID || X.avx_cc != X86_AVX_CC_INVALID)
    return false;
  const bool Long = Target == Arch::X64;
  size_t Pos = 0;
  uint8_t LockPrefix = 0, OperandPrefix = 0, SegmentPrefix = 0,
          AddressPrefix = 0, Rex = 0;
  for (; Pos < Insn->size; ++Pos) {
    const uint8_t B = Insn->bytes[Pos];
    if (B == 0xf0 && !LockPrefix && !Rex)
      LockPrefix = B;
    else if (B == 0x66 && !OperandPrefix && !Rex)
      OperandPrefix = B;
    else if (B == 0x67 && !AddressPrefix && !Rex)
      AddressPrefix = B;
    else if ((B == 0x64 || B == 0x65) && !SegmentPrefix && !Rex)
      SegmentPrefix = B;
    else if (Long && B >= 0x40 && B <= 0x4f && !Rex)
      Rex = B;
    else
      break;
  }
  if (Pos + 3 > Insn->size || Insn->bytes[Pos] != 0x0f)
    return false;
  const uint8_t Opcode = Insn->bytes[Pos + 1];
  const uint8_t ModRM = Insn->bytes[Pos + 2];
  const bool Immediate = Opcode == 0xba;
  const bool Memory = (ModRM >> 6) != 3;
  const unsigned Kind = Insn->id == X86_INS_BT    ? 0
                        : Insn->id == X86_INS_BTS ? 1
                        : Insn->id == X86_INS_BTR ? 2
                                                  : 3;
  const unsigned AddressSize =
      Long ? (AddressPrefix ? 4 : 8) : (AddressPrefix ? 2 : 4);
  if ((LockPrefix && (!Memory || Insn->id == X86_INS_BT)) ||
      (Immediate ? ((ModRM >> 3) & 7) != Kind + 4 || (Rex & 4)
                 : Opcode != 0xa3 + Kind * 8) ||
      X.modrm != ModRM || X.encoding.modrm_offset != Pos + 2 ||
      X.opcode[0] != 0x0f || X.opcode[1] != Opcode || X.opcode[2] ||
      X.opcode[3] || X.rex != Rex || X.prefix[0] != LockPrefix ||
      X.prefix[1] != SegmentPrefix || X.prefix[2] != OperandPrefix ||
      X.prefix[3] != AddressPrefix || X.addr_size != AddressSize ||
      X.encoding.imm_size != (Immediate ? 1 : 0) ||
      X.encoding.imm_offset != (Immediate ? Insn->size - 1 : 0))
    return false;
  const unsigned Width = (Rex & 8) ? 8 : OperandPrefix ? 2 : 4;
  const auto Register = [&](const cs_x86_op &Operand, unsigned Number) {
    if (Operand.type != X86_OP_REG || Operand.size != Width)
      return false;
    const auto R = mapCapstoneReg(static_cast<x86_reg>(Operand.reg));
    return R.Offset == Number * 8 && R.Size == Width;
  };
  if (Memory) {
    const x86_reg Segment = SegmentPrefix == 0x64   ? X86_REG_FS
                            : SegmentPrefix == 0x65 ? X86_REG_GS
                                                    : X86_REG_INVALID;
    if (!legacyaudit::memoryOperand(Insn, X.operands[0], Width, AddressSize,
                                    Long, Rex, Pos + 3, Immediate, Segment))
      return false;
  } else if (AddressPrefix || SegmentPrefix || (Rex & 2) ||
             Insn->size != Pos + 3 + Immediate || X.sib || X.disp ||
             X.encoding.disp_size || X.encoding.disp_offset ||
             !Register(X.operands[0], (ModRM & 7) + ((Rex & 1) ? 8 : 0))) {
    return false;
  }
  const auto &Index = X.operands[1];
  return Immediate ? Index.type == X86_OP_IMM && Index.size == 1 &&
                         Index.imm == Insn->bytes[Insn->size - 1]
                   : Register(Index, ((ModRM >> 3) & 7) + ((Rex & 4) ? 8 : 0));
}

} // namespace neverd::bitundefined

#endif
