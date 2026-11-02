//===- X86LiftBMILogic.cpp - BMI logical values and flags ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate VEX and APX logical forms before publishing values or flags.
//===----------------------------------------------------------------------===//

#include "X86LiftBMILogic.h"

#include "X86FPEncoding.h"
#include "X86LiftAPXValidation.h"
#include "X86UndefinedFlagAudit.h"

#include "neverd/decode/Decoder.h"
#include "neverd/support/ISAEncoding.h"

#include <array>
#include <initializer_list>
#include <optional>

namespace neverd::bmilogic {
namespace {

constexpr uint64_t CommonFlags =
    X86_EFLAGS_RESET_OF | X86_EFLAGS_MODIFY_SF | X86_EFLAGS_MODIFY_ZF |
    X86_EFLAGS_UNDEFINED_AF | X86_EFLAGS_UNDEFINED_PF;
constexpr std::array<unsigned, 2> UndefinedFlags = {x86reg::AF, x86reg::PF};

struct Encoding {
  unsigned Width;
  bool NoFlags;
};

std::optional<Encoding> encoding(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || !isInstruction(Insn->id) || !Insn->size ||
      Insn->size > x86::kMaxInstructionLen || Insn->illegal ||
      (Target != Arch::X86 && Target != Arch::X64))
    return std::nullopt;
  const auto &X = Insn->detail->x86;
  const unsigned Id = Insn->id;
  const bool Group = Id != X86_INS_ANDN && Id != X86_INS_BZHI;
  const unsigned GroupOpcode = Id == X86_INS_BLSR     ? 1
                               : Id == X86_INS_BLSMSK ? 2
                                                      : 3;
  const unsigned RMOperand = Id == X86_INS_ANDN ? 2 : 1;
  const unsigned VOperand = Group ? 0 : Id == X86_INS_ANDN ? 1 : 2;
  const uint8_t Opcode = Group ? 0xf3 : Id == X86_INS_ANDN ? 0xf2 : 0xf5;
  const uint64_t Flags =
      CommonFlags |
      (Id == X86_INS_ANDN ? X86_EFLAGS_RESET_CF : X86_EFLAGS_MODIFY_CF);
  if (X.op_count != (Group ? 2 : 3) || X.rex2 || X.avx_sae ||
      X.avx_rm != X86_AVX_RM_INVALID || X.xop_cc != X86_XOP_CC_INVALID ||
      X.sse_cc != X86_SSE_CC_INVALID || X.avx_cc != X86_AVX_CC_INVALID ||
      X.encoding.imm_size || X.encoding.imm_offset)
    return std::nullopt;
  for (unsigned I = 0; I != X.op_count; ++I)
    if (X.operands[I].avx_bcast != X86_AVX_BCAST_INVALID ||
        X.operands[I].avx_zero_opmask)
      return std::nullopt;

  if (apxvalidation::isPresent(Insn, X)) {
    apxvalidation::Header H;
    if (Target != Arch::X64 || !apxvalidation::decodeHeader(Insn, X, H) ||
        (H.P0 & 7) != 2 || H.Opcode != Opcode || (H.P1 & 3) != 0 ||
        (!H.Memory && (H.P1 & 4) == 0) || (H.P2 & 0xf3) != 0 || X.rex ||
        (Group && ((H.ModRM >> 3) & 7) != GroupOpcode))
      return std::nullopt;
    const unsigned Width = H.P1 & 0x80 ? 8 : 4;
    const bool NF = H.P2 & 4;
    if (!apxvalidation::implicitDetail(
            Insn, X, NF ? 0 : Flags, {},
            NF ? std::initializer_list<x86_reg>{}
               : std::initializer_list<x86_reg>{X86_REG_EFLAGS}) ||
        (!Group && !apxvalidation::registerOperand(X.operands[0],
                                                   apxvalidation::modrmReg(H),
                                                   Width, CS_AC_WRITE)) ||
        !apxvalidation::registerOperand(X.operands[VOperand],
                                        apxvalidation::vvvvv(H), Width,
                                        Group ? CS_AC_WRITE : CS_AC_READ) ||
        !apxvalidation::validateRM(Insn, X, H, X.operands[RMOperand], Width,
                                   CS_AC_READ))
      return std::nullopt;
    return Encoding{Width, NF};
  }

  CanonicalVex3EncodingInfo E;
  if (!parseCanonicalVex3EncodingInfo(Insn, X, Target, E) ||
      (E.P0 & 0x1f) != 2 || (E.P1 & 7) != 0 || E.Opcode != Opcode ||
      (Group && ((E.ModRM >> 3) & 7) != GroupOpcode))
    return std::nullopt;
  const unsigned Width = E.Is64Bit && (E.P1 & 0x80) ? 8 : 4;
  const auto Register = [&](unsigned Operand, unsigned Index, uint8_t Access) {
    return matchesFPGPROperand(X.operands[Operand], Index, Width) &&
           X.operands[Operand].access == Access;
  };
  const auto &RM = X.operands[RMOperand];
  // The pinned decoder reports RESET_ZF for both VEX BLSMSK and BLSR.
  // Authenticate that detail; only BLSMSK architecturally clears ZF.
  const uint64_t DecoderFlags =
      Id == X86_INS_BLSMSK || Id == X86_INS_BLSR
          ? (Flags & ~X86_EFLAGS_MODIFY_ZF) | X86_EFLAGS_RESET_ZF
          : Flags;
  if (!apxvalidation::implicitDetail(Insn, X, DecoderFlags, {},
                                     {X86_REG_EFLAGS}) ||
      (!Group && !Register(0, E.regIndex(), CS_AC_WRITE)) ||
      !Register(VOperand, E.vvvvIndex(), Group ? CS_AC_WRITE : CS_AC_READ) ||
      RM.size != Width || RM.access != CS_AC_READ ||
      ((E.ModRM & 0xc0) == 0xc0 ? !Register(RMOperand, E.rmIndex(), CS_AC_READ)
                                : RM.type != X86_OP_MEM) ||
      !validateCanonicalScalarConversionTail(
          Insn, X, E.Offset + 5, E.SegmentPrefix, E.Is64Bit, E.AddressSize,
          E.baseExtension(), E.indexExtension(), RM))
    return std::nullopt;
  return Encoding{Width, false};
}

} // namespace

bool isInstruction(unsigned Id) {
  return Id == X86_INS_ANDN || Id == X86_INS_BLSI || Id == X86_INS_BLSMSK ||
         Id == X86_INS_BLSR || Id == X86_INS_BZHI;
}

bool form(const cs_insn *Insn, Arch Target) {
  return encoding(Insn, Target).has_value();
}

bool matches(const cs_insn *Insn, llvm::ArrayRef<LowOp> Ops,
             const LowInstructionUndefinedEffects &Effects) {
  return undefinedflags::matches(Insn->detail->x86.eflags == 0
                                     ? llvm::ArrayRef<unsigned>{}
                                     : UndefinedFlags,
                                 Ops, Effects);
}

bool lift(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn) {
  const auto E = encoding(Insn, L.targetArch());
  if (!E)
    throw UnliftedInstruction(Insn->address, Insn->mnemonic, Insn->op_str);
  const auto &X = Insn->detail->x86;
  const auto A = L.operandRead(S, X.operands[1]);
  const bool Binary = X.op_count == 3;
  const auto B = Binary ? L.operandRead(S, X.operands[2]) : NdVar{};
  const auto Dst = L.operandWrite(X.operands[0]);
  const unsigned Width = E->Width;
  const unsigned Id = Insn->id;
  NdVar Carry;
  if (Id == X86_INS_ANDN) {
    auto Inverted = S.makeTemp(Width);
    S.emit(NdOp::INT_NOT, Inverted, {A});
    S.emit(NdOp::INT_AND, Dst, {Inverted, B});
    Carry = NdVar::scalar(0, 1);
  } else if (Id == X86_INS_BZHI) {
    auto Index = S.makeTemp(Width);
    S.emit(NdOp::INT_AND, Index, {B, NdVar::scalar(0xff, Width)});
    auto InRange = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, InRange, {Index, NdVar::scalar(Width * 8, Width)});
    auto SafeIndex = S.makeTemp(Width);
    S.emit(NdOp::SELECT, SafeIndex, {InRange, Index, NdVar::scalar(0, Width)});
    auto Partial = S.makeTemp(Width);
    S.emit(NdOp::INT_LEFT, Partial, {NdVar::scalar(1, Width), SafeIndex});
    S.emit(NdOp::INT_SUB, Partial, {Partial, NdVar::scalar(1, Width)});
    auto Mask = S.makeTemp(Width);
    S.emit(NdOp::SELECT, Mask,
           {InRange, Partial, NdVar::scalar(UINT64_MAX, Width)});
    S.emit(NdOp::INT_AND, Dst, {A, Mask});
    if (!E->NoFlags) {
      Carry = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, Carry, {InRange});
    }
  } else {
    // Source-zero CF must observe the input before an aliased destination.
    if (!E->NoFlags) {
      Carry = S.makeTemp(1);
      S.emit(Id == X86_INS_BLSI ? NdOp::INT_NOTEQUAL : NdOp::INT_EQUAL, Carry,
             {A, NdVar::scalar(0, Width)});
    }
    auto Adjusted = S.makeTemp(Width);
    if (Id == X86_INS_BLSI)
      S.emit(NdOp::INT_NEG2, Adjusted, {A});
    else
      S.emit(NdOp::INT_SUB, Adjusted, {A, NdVar::scalar(1, Width)});
    S.emit(Id == X86_INS_BLSMSK ? NdOp::INT_XOR : NdOp::INT_AND, Dst,
           {Adjusted, A});
  }
  if (!E->NoFlags) {
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {Carry});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Dst, NdVar::scalar(0, Width)});
    S.emit(NdOp::INT_SLESS, NdVar::reg(x86reg::SF, 1),
           {Dst, NdVar::scalar(0, Width)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::scalar(0, 1)});
    for (unsigned Flag : UndefinedFlags) {
      S.emit(NdOp::COPY, NdVar::reg(Flag, 1), {NdVar::scalar(0, 1)});
      S.recordUndefinedBits(NdVar::reg(Flag, 1), 0, 1);
    }
  }
  return true;
}

} // namespace neverd::bmilogic
