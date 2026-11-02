//===- X86LiftBEXTR.cpp - Bit-field extraction semantics ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate each BEXTR encoding before publishing its value and flags.
//===----------------------------------------------------------------------===//

#include "X86LiftBEXTR.h"

#include "X86FPEncoding.h"
#include "X86LiftAPXValidation.h"
#include "X86UndefinedFlagAudit.h"

#include "neverd/decode/Decoder.h"
#include "neverd/support/ISAEncoding.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>

namespace neverd::bextr {
namespace {

constexpr uint64_t Flags = X86_EFLAGS_RESET_CF | X86_EFLAGS_RESET_OF |
                           X86_EFLAGS_MODIFY_ZF | X86_EFLAGS_UNDEFINED_SF |
                           X86_EFLAGS_UNDEFINED_AF | X86_EFLAGS_UNDEFINED_PF;
constexpr std::array<unsigned, 3> UndefinedFlags = {x86reg::SF, x86reg::AF,
                                                    x86reg::PF};

struct Encoding {
  unsigned Width;
  bool NoFlags;
};

std::optional<Encoding> encoding(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Insn->id != X86_INS_BEXTR ||
      Insn->size > x86::kMaxInstructionLen)
    return std::nullopt;
  const auto &X = Insn->detail->x86;
  if (X.op_count != 3 || X.avx_sae || X.avx_rm != X86_AVX_RM_INVALID ||
      X.xop_cc != X86_XOP_CC_INVALID || X.sse_cc != X86_SSE_CC_INVALID ||
      X.avx_cc != X86_AVX_CC_INVALID)
    return std::nullopt;
  for (unsigned I = 0; I != X.op_count; ++I)
    if (X.operands[I].avx_bcast != X86_AVX_BCAST_INVALID ||
        X.operands[I].avx_zero_opmask)
      return std::nullopt;

  if (apxvalidation::isPresent(Insn, X)) {
    apxvalidation::Header H;
    if (Target != Arch::X64 || !apxvalidation::decodeHeader(Insn, X, H) ||
        (H.P0 & 7) != 2 || H.Opcode != 0xf7 || (H.P1 & 3) != 0 ||
        (!H.Memory && (H.P1 & 4) == 0) || (H.P2 & 0xf3) != 0 || X.rex != 0)
      return std::nullopt;
    const unsigned Width = H.P1 & 0x80 ? 8 : 4;
    const bool NF = H.P2 & 4;
    if (!apxvalidation::implicitDetail(
            Insn, X, NF ? 0 : Flags, {},
            NF ? std::initializer_list<x86_reg>{}
               : std::initializer_list<x86_reg>{X86_REG_EFLAGS}) ||
        !apxvalidation::registerOperand(
            X.operands[0], apxvalidation::modrmReg(H), Width, CS_AC_WRITE) ||
        !apxvalidation::validateRM(Insn, X, H, X.operands[1], Width,
                                   CS_AC_READ) ||
        !apxvalidation::registerOperand(X.operands[2], apxvalidation::vvvvv(H),
                                        Width, CS_AC_READ))
      return std::nullopt;
    return Encoding{Width, NF};
  }

  const bool Immediate = X.opcode[0] == 0x8f;
  CanonicalVex3EncodingInfo E;
  if (!(Immediate ? parseCanonicalXopEncodingInfo(Insn, X, Target, E)
                  : parseCanonicalVex3EncodingInfo(Insn, X, Target, E)) ||
      (E.P0 & 0x1f) != (Immediate ? 10 : 2) || (E.P1 & 7) != 0 ||
      E.Opcode != (Immediate ? 0x10 : 0xf7) ||
      (Immediate && (E.P1 & 0x78) != 0x78))
    return std::nullopt;
  // W and register extension B/high-vvvv do not extend operands in i386.
  const unsigned Width = E.Is64Bit && (E.P1 & 0x80) ? 8 : 4;
  if (!matchesFPGPROperand(X.operands[0], E.regIndex(), Width) ||
      X.operands[0].access != CS_AC_WRITE || X.operands[1].size != Width ||
      X.operands[1].access != CS_AC_READ ||
      ((E.ModRM & 0xc0) == 0xc0
           ? !matchesFPGPROperand(X.operands[1], E.rmIndex(), Width)
           : X.operands[1].type != X86_OP_MEM))
    return std::nullopt;
  // The pinned decoder over-reports these resets for legacy BEXTR. Authenticate
  // its detail, but derive architectural effects here: DF and control flags
  // are preserved, and SF/AF/PF are undefined in both AMD and Intel manuals.
  constexpr uint64_t DecoderFlags = Flags | X86_EFLAGS_RESET_TF |
                                    X86_EFLAGS_RESET_IF | X86_EFLAGS_RESET_DF |
                                    X86_EFLAGS_RESET_NT | X86_EFLAGS_RESET_RF;
  if (!apxvalidation::implicitDetail(Insn, X, DecoderFlags, {},
                                     {X86_REG_EFLAGS}))
    return std::nullopt;
  if (Immediate) {
    if (Insn->size < E.Offset + 9 || X.encoding.imm_size != 4 ||
        X.encoding.imm_offset != Insn->size - 4)
      return std::nullopt;
    uint32_t Raw = 0;
    for (unsigned I = 0; I != 4; ++I)
      Raw |= static_cast<uint32_t>(Insn->bytes[Insn->size - 4 + I]) << (I * 8);
    const auto &Control = X.operands[2];
    const int64_t Value =
        Width == 8 ? static_cast<int64_t>(static_cast<int32_t>(Raw)) : Raw;
    if (Control.type != X86_OP_IMM || Control.size != Width || Control.access ||
        Control.imm != Value)
      return std::nullopt;
  } else if (X.encoding.imm_size || X.encoding.imm_offset ||
             !matchesFPGPROperand(X.operands[2], E.vvvvIndex(), Width) ||
             X.operands[2].access != CS_AC_READ) {
    return std::nullopt;
  }
  if (!validateCanonicalScalarConversionTail(
          Insn, X, E.Offset + 5, E.SegmentPrefix, E.Is64Bit, E.AddressSize,
          E.baseExtension(), E.indexExtension(), X.operands[1],
          Immediate ? 4 : 0))
    return std::nullopt;
  return Encoding{Width, false};
}

} // namespace

bool form(const cs_insn *Insn, Arch Target) {
  return encoding(Insn, Target).has_value();
}

bool matches(const cs_insn *Insn, llvm::ArrayRef<LowOp> Ops,
             const LowInstructionUndefinedEffects &Effects) {
  // Form admission distinguishes ordinary flags from APX NF.
  if (Insn->detail->x86.eflags == 0)
    return Effects.Effects.empty();
  return undefinedflags::matches(UndefinedFlags, Ops, Effects);
}

bool lift(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn) {
  const auto E = encoding(Insn, L.targetArch());
  if (!E)
    throw UnliftedInstruction(Insn->address, Insn->mnemonic, Insn->op_str);
  const auto &X = Insn->detail->x86;
  const auto Src = L.operandRead(S, X.operands[1]);
  const auto Ctrl = L.operandRead(S, X.operands[2]);
  const auto Dst = L.operandWrite(X.operands[0]);
  const auto Sz = E->Width;
  const uint64_t Bits = Sz * 8;
  auto Start = S.makeTemp(Sz);
  S.emit(NdOp::INT_AND, Start, {Ctrl, NdVar::scalar(0xff, Sz)});
  auto StartInRange = S.makeTemp(1);
  S.emit(NdOp::INT_LESS, StartInRange, {Start, NdVar::scalar(Bits, Sz)});
  auto SafeStart = S.makeTemp(Sz);
  S.emit(NdOp::SELECT, SafeStart, {StartInRange, Start, NdVar::scalar(0, Sz)});
  auto Shifted = S.makeTemp(Sz);
  S.emit(NdOp::INT_RIGHT, Shifted, {Src, SafeStart});
  auto Len = S.makeTemp(Sz);
  S.emit(NdOp::INT_RIGHT, Len, {Ctrl, NdVar::scalar(8, Sz)});
  S.emit(NdOp::INT_AND, Len, {Len, NdVar::scalar(0xff, Sz)});
  auto Remaining = S.makeTemp(Sz);
  S.emit(NdOp::INT_SUB, Remaining, {NdVar::scalar(Bits, Sz), SafeStart});
  auto LenFits = S.makeTemp(1);
  S.emit(NdOp::INT_LESS, LenFits, {Len, Remaining});
  auto EffectiveLen = S.makeTemp(Sz);
  S.emit(NdOp::SELECT, EffectiveLen, {LenFits, Len, Remaining});
  auto LenBelowWidth = S.makeTemp(1);
  S.emit(NdOp::INT_LESS, LenBelowWidth,
         {EffectiveLen, NdVar::scalar(Bits, Sz)});
  auto SafeLen = S.makeTemp(Sz);
  S.emit(NdOp::SELECT, SafeLen,
         {LenBelowWidth, EffectiveLen, NdVar::scalar(0, Sz)});
  auto PartialMask = S.makeTemp(Sz);
  S.emit(NdOp::INT_LEFT, PartialMask, {NdVar::scalar(1, Sz), SafeLen});
  S.emit(NdOp::INT_SUB, PartialMask, {PartialMask, NdVar::scalar(1, Sz)});
  auto Mask = S.makeTemp(Sz);
  S.emit(NdOp::SELECT, Mask,
         {LenBelowWidth, PartialMask, NdVar::scalar(UINT64_MAX, Sz)});
  auto Extracted = S.makeTemp(Sz);
  S.emit(NdOp::INT_AND, Extracted, {Shifted, Mask});
  S.emit(NdOp::SELECT, Dst, {StartInRange, Extracted, NdVar::scalar(0, Sz)});
  if (!E->NoFlags) {
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Dst, NdVar::scalar(0, Sz)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NdVar::scalar(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::scalar(0, 1)});
    for (unsigned Flag : UndefinedFlags) {
      S.emit(NdOp::COPY, NdVar::reg(Flag, 1), {NdVar::scalar(0, 1)});
      S.recordUndefinedBits(NdVar::reg(Flag, 1), 0, 1);
    }
  }
  return true;
}

} // namespace neverd::bextr
