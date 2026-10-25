//===- X86LiftTBM.cpp - Trailing bit manipulation semantics --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate XOP operands before publishing TBM results and flags.
//===----------------------------------------------------------------------===//

#include "X86LiftTBM.h"

#include "X86FPEncoding.h"

#include "neverd/decode/Decoder.h"
#include "neverd/support/ISAEncoding.h"

#include <array>
#include <optional>

namespace neverd::tbm {
namespace {

struct Operation {
  uint8_t Opcode;
  uint8_t Group;
  NdOp Logic;
  bool Decrement = false;
  bool InvertSource = false;
  bool InvertAdjacent = false;
};

std::optional<Operation> operation(unsigned Id) {
  switch (Id) {
  case X86_INS_BLCFILL:
    return Operation{0x01, 1, NdOp::INT_AND};
  case X86_INS_BLCI:
    return Operation{0x02, 6, NdOp::INT_OR, false, false, true};
  case X86_INS_BLCIC:
    return Operation{0x01, 5, NdOp::INT_AND, false, true};
  case X86_INS_BLCMSK:
    return Operation{0x02, 1, NdOp::INT_XOR};
  case X86_INS_BLCS:
    return Operation{0x01, 3, NdOp::INT_OR};
  case X86_INS_BLSFILL:
    return Operation{0x01, 2, NdOp::INT_OR, true};
  case X86_INS_BLSIC:
    return Operation{0x01, 6, NdOp::INT_OR, true, true};
  case X86_INS_T1MSKC:
    return Operation{0x01, 7, NdOp::INT_OR, false, true};
  case X86_INS_TZMSK:
    return Operation{0x01, 4, NdOp::INT_AND, true, true};
  default:
    return std::nullopt;
  }
}

constexpr std::array<unsigned, 2> UndefinedFlags = {x86reg::AF, x86reg::PF};

} // namespace

bool isInstruction(unsigned Id) { return operation(Id).has_value(); }

bool form(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Insn->size > x86::kMaxInstructionLen)
    return false;
  const auto O = operation(Insn->id);
  if (!O)
    return false;
  const auto &X = Insn->detail->x86;
  CanonicalVex3EncodingInfo E;
  if (!parseCanonicalXopEncodingInfo(Insn, X, Target, E) ||
      (E.P0 & 0x1f) != 9 || (E.P1 & 7) != 0 || E.Opcode != O->Opcode ||
      ((E.ModRM >> 3) & 7) != O->Group || X.op_count != 2 ||
      X.encoding.imm_size || X.encoding.imm_offset || X.avx_sae ||
      X.avx_rm != X86_AVX_RM_INVALID || X.xop_cc != X86_XOP_CC_INVALID ||
      X.sse_cc != X86_SSE_CC_INVALID || X.avx_cc != X86_AVX_CC_INVALID)
    return false;
  // AMD APM 24594: W is ignored outside long mode, as is the high vvvv bit.
  const unsigned Width = E.Is64Bit && (E.P1 & 0x80) ? 8 : 4;
  const unsigned Destination = ((~E.P1 & 0x78) >> 3) & (E.Is64Bit ? 15 : 7);
  const unsigned BaseExtension = E.Is64Bit && !(E.P0 & 0x20) ? 8 : 0;
  const unsigned IndexExtension = E.Is64Bit && !(E.P0 & 0x40) ? 8 : 0;
  for (unsigned I = 0; I != X.op_count; ++I)
    if (X.operands[I].avx_bcast != X86_AVX_BCAST_INVALID ||
        X.operands[I].avx_zero_opmask)
      return false;
  const auto &Source = X.operands[1];
  if (!matchesFPGPROperand(X.operands[0], Destination, Width) ||
      X.operands[0].access != CS_AC_WRITE || Source.size != Width ||
      Source.access != CS_AC_READ ||
      ((E.ModRM & 0xc0) == 0xc0
           ? !matchesFPGPROperand(Source, (E.ModRM & 7) | BaseExtension, Width)
           : Source.type != X86_OP_MEM))
    return false;
  // The pinned decoder leaves eflags empty for TBM, while reporting the
  // implicit EFLAGS write. The architectural flag decision belongs here.
  if (X.eflags || Insn->detail->regs_read_count ||
      Insn->detail->regs_write_count != 1 ||
      Insn->detail->regs_write[0] != X86_REG_EFLAGS)
    return false;
  return validateCanonicalScalarConversionTail(
      Insn, X, E.Offset + 5, E.SegmentPrefix, E.Is64Bit, E.AddressSize,
      BaseExtension, IndexExtension, Source);
}

bool matches(llvm::ArrayRef<LowOp> Ops,
             const LowInstructionUndefinedEffects &Effects) {
  if (Effects.Effects.size() != UndefinedFlags.size())
    return false;
  for (size_t I = 0; I != UndefinedFlags.size(); ++I) {
    const auto &E = Effects.Effects[I];
    if (E.Output != NdVar::reg(UndefinedFlags[I], 1) || E.BitOffset ||
        E.BitCount != 1 || E.When || !E.AfterOp || E.AfterOp > Ops.size())
      return false;
    for (const auto &Later : Ops.drop_front(E.AfterOp)) {
      if (Later.Output == E.Output)
        return false;
      for (unsigned J = 0; J != Later.NumInputs; ++J)
        if (Later.Inputs[J] == E.Output)
          return false;
    }
  }
  return true;
}

bool lift(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn) {
  if (!form(Insn, L.targetArch()))
    throw UnliftedInstruction(Insn->address, Insn->mnemonic, Insn->op_str);
  const auto O = *operation(Insn->id);
  const auto &X = Insn->detail->x86;
  const auto Source = L.operandRead(S, X.operands[1]);
  const auto Destination = L.operandWrite(X.operands[0]);
  const auto Width = Destination.Size;
  const auto One = NdVar::scalar(1, Width);
  const auto Zero = NdVar::scalar(0, Width);
  // Read memory first, then compute CF before an aliased destination write.
  // No flag or GPR effect is published before the potentially faulting load.
  S.emit(O.Decrement ? NdOp::INT_LESS : NdOp::INT_CARRY,
         NdVar::reg(x86reg::CF, 1), {Source, One});
  auto Adjacent = S.makeTemp(Width);
  S.emit(O.Decrement ? NdOp::INT_SUB : NdOp::INT_ADD, Adjacent, {Source, One});
  auto Left = Source;
  if (O.InvertSource) {
    Left = S.makeTemp(Width);
    S.emit(NdOp::INT_NOT, Left, {Source});
  }
  if (O.InvertAdjacent) {
    const auto Inverted = S.makeTemp(Width);
    S.emit(NdOp::INT_NOT, Inverted, {Adjacent});
    Adjacent = Inverted;
  }
  S.emit(O.Logic, Destination, {Left, Adjacent});
  S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1), {Destination, Zero});
  S.emit(NdOp::INT_SLESS, NdVar::reg(x86reg::SF, 1), {Destination, Zero});
  S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::scalar(0, 1)});
  // Select zero for the concrete lift, but preserve the undefined-bit audit
  // so independence checks cannot treat either bit as architecturally zero.
  for (unsigned Flag : UndefinedFlags) {
    S.emit(NdOp::COPY, NdVar::reg(Flag, 1), {NdVar::scalar(0, 1)});
    S.recordUndefinedBits(NdVar::reg(Flag, 1), 0, 1);
  }
  return true;
}

} // namespace neverd::tbm
