//===- X86LiftBitCount.cpp - Scalar bit-count semantics -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate TZCNT, LZCNT and POPCNT before publishing values or flags.
//===----------------------------------------------------------------------===//

#include "X86LiftBitCount.h"

#include "X86LegacyScalarPrefixes.h"
#include "X86LiftAPXValidation.h"
#include "X86LiftDetail.h"
#include "X86UndefinedFlagAudit.h"

#include "neverd/decode/Decoder.h"
#include "neverd/support/ISAEncoding.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>

namespace neverd::bitcount {
namespace {

constexpr uint64_t CountFlags =
    X86_EFLAGS_MODIFY_CF | X86_EFLAGS_MODIFY_ZF | X86_EFLAGS_UNDEFINED_OF |
    X86_EFLAGS_UNDEFINED_SF | X86_EFLAGS_UNDEFINED_AF | X86_EFLAGS_UNDEFINED_PF;
constexpr uint64_t PopFlags = X86_EFLAGS_RESET_CF | X86_EFLAGS_MODIFY_ZF |
                              X86_EFLAGS_RESET_OF | X86_EFLAGS_RESET_SF |
                              X86_EFLAGS_RESET_AF | X86_EFLAGS_RESET_PF;
constexpr std::array<unsigned, 4> UndefinedFlags = {x86reg::OF, x86reg::SF,
                                                    x86reg::AF, x86reg::PF};

struct Encoding {
  bool NoFlags = false;
};

bool implicit(const cs_insn *Insn, uint64_t Flags) {
  return apxvalidation::implicitDetail(
      Insn, Insn->detail->x86, Flags, {},
      Flags ? std::initializer_list<x86_reg>{X86_REG_EFLAGS}
            : std::initializer_list<x86_reg>{});
}

std::optional<Encoding> encoding(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || !isInstruction(Insn->id) || !Insn->size ||
      Insn->size > x86::kMaxInstructionLen || Insn->illegal ||
      (Target != Arch::X86 && Target != Arch::X64))
    return std::nullopt;
  const auto &X = Insn->detail->x86;
  if (X.op_count != 2 || X.avx_sae || X.avx_rm != X86_AVX_RM_INVALID ||
      X.xop_cc != X86_XOP_CC_INVALID || X.sse_cc != X86_SSE_CC_INVALID ||
      X.avx_cc != X86_AVX_CC_INVALID || X.encoding.imm_size ||
      X.encoding.imm_offset)
    return std::nullopt;
  for (unsigned I = 0; I != X.op_count; ++I)
    if (X.operands[I].avx_bcast != X86_AVX_BCAST_INVALID ||
        X.operands[I].avx_zero_opmask)
      return std::nullopt;
  const unsigned Id = Insn->id;
  const uint64_t Flags = Id == X86_INS_POPCNT ? PopFlags : CountFlags;

  if (apxvalidation::isPresent(Insn, X)) {
    apxvalidation::Header H;
    const uint8_t Opcode = Id == X86_INS_TZCNT   ? 0xf4
                           : Id == X86_INS_LZCNT ? 0xf5
                                                 : 0x88;
    if (Target != Arch::X64 || !apxvalidation::decodeHeader(Insn, X, H) ||
        (H.P0 & 7) != 4 || H.Opcode != Opcode || (H.P1 & 3) > 1 ||
        (H.P1 & 0x78) != 0x78 || (H.P2 & 8) == 0 || (H.P2 & 0xf3) != 0 ||
        (!H.Memory && (H.P1 & 4) == 0) || X.rex || X.rex2)
      return std::nullopt;
    const unsigned Width = H.P1 & 0x80 ? 8 : H.P1 & 1 ? 2 : 4;
    const bool NF = H.P2 & 4;
    if (!implicit(Insn, NF ? 0 : Flags) ||
        !apxvalidation::registerOperand(
            X.operands[0], apxvalidation::modrmReg(H), Width, CS_AC_WRITE) ||
        !apxvalidation::validateRM(Insn, X, H, X.operands[1], Width,
                                   CS_AC_READ))
      return std::nullopt;
    return Encoding{NF};
  }

  // F3 selects the instruction; 66 independently selects its 16-bit operand.
  const bool Mode64 = Target == Arch::X64;
  legacyvalidation::ScalarPrefixes P;
  if (!legacyvalidation::scalarPrefixes(Insn, Mode64, P) || !P.F3)
    return std::nullopt;
  size_t Cursor = P.OpcodeOffset;
  const uint8_t Segment = P.Segment, Rex = P.Rex;
  const bool Address = P.Address, Operand16 = P.Operand16;
  uint8_t Rex2 = 0;
  std::array<uint8_t, 4> OpcodeDetail{};
  uint8_t Extension = Rex;
  unsigned RegExtension = 0, BaseExtension = 0, IndexExtension = 0;
  if (Insn->bytes[Cursor] == 0xd5) {
    if (!Mode64 || Rex || Insn->size - Cursor < 4)
      return std::nullopt;
    Rex2 = Insn->bytes[Cursor + 1];
    if (!(Rex2 & 0x80))
      return std::nullopt;
    Extension = Rex2;
    RegExtension = (Rex2 & 0x40) ? 16 : 0;
    BaseExtension = (Rex2 & 0x10) ? 16 : 0;
    IndexExtension = (Rex2 & 0x20) ? 16 : 0;
    Cursor += 2;
    OpcodeDetail[0] = Insn->bytes[Cursor];
  } else {
    if (Insn->bytes[Cursor++] != 0x0f || Insn->size - Cursor < 2)
      return std::nullopt;
    OpcodeDetail[0] = 0x0f;
    OpcodeDetail[1] = Insn->bytes[Cursor];
  }
  const uint8_t Opcode = Id == X86_INS_TZCNT   ? 0xbc
                         : Id == X86_INS_LZCNT ? 0xbd
                                               : 0xb8;
  const unsigned Width = Extension & 8 ? 8 : Operand16 ? 2 : 4;
  const unsigned AddressSize = Mode64 ? (Address ? 4 : 8) : (Address ? 2 : 4);
  RegExtension |= (Extension & 4) ? 8 : 0;
  BaseExtension |= (Extension & 1) ? 8 : 0;
  IndexExtension |= (Extension & 2) ? 8 : 0;
  if (Insn->bytes[Cursor] != Opcode || X.rex != Rex || X.rex2 != Rex2 ||
      X.prefix[0] || X.prefix[1] != Segment ||
      X.prefix[2] != (Operand16 ? 0x66 : 0) ||
      X.prefix[3] != (Address ? 0x67 : 0) || X.addr_size != AddressSize ||
      X.encoding.modrm_offset != Cursor + 1 ||
      X.modrm != Insn->bytes[Cursor + 1] ||
      !std::equal(OpcodeDetail.begin(), OpcodeDetail.end(), X.opcode))
    return std::nullopt;
  // Legacy Capstone LZCNT detail omits MODIFY_CF. Authenticate that decoder
  // contract, while the architectural source-zero CF decision stays here.
  if (!implicit(Insn,
                Id == X86_INS_LZCNT ? Flags & ~X86_EFLAGS_MODIFY_CF : Flags) ||
      !apxvalidation::registerOperand(X.operands[0],
                                      ((X.modrm >> 3) & 7) | RegExtension,
                                      Width, CS_AC_WRITE) ||
      X.operands[1].size != Width || X.operands[1].access != CS_AC_READ ||
      ((X.modrm & 0xc0) == 0xc0
           ? !apxvalidation::registerOperand(X.operands[1],
                                             (X.modrm & 7) | BaseExtension,
                                             Width, CS_AC_READ)
           : X.operands[1].type != X86_OP_MEM) ||
      !validateCanonicalScalarConversionTail(Insn, X, Cursor + 2, Segment,
                                             Mode64, AddressSize, BaseExtension,
                                             IndexExtension, X.operands[1]))
    return std::nullopt;
  return Encoding{};
}

} // namespace

bool isInstruction(unsigned Id) {
  return Id == X86_INS_TZCNT || Id == X86_INS_LZCNT || Id == X86_INS_POPCNT;
}

bool form(const cs_insn *Insn, Arch Target) {
  return encoding(Insn, Target).has_value();
}

bool matches(const cs_insn *Insn, llvm::ArrayRef<LowOp> Ops,
             const LowInstructionUndefinedEffects &Effects) {
  if (Insn->id == X86_INS_POPCNT || Insn->detail->x86.eflags == 0)
    return Effects.Effects.empty();
  return undefinedflags::matches(UndefinedFlags, Ops, Effects);
}

bool lift(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn) {
  const auto E = encoding(Insn, L.targetArch());
  if (!E)
    throw UnliftedInstruction(Insn->address, Insn->mnemonic, Insn->op_str);
  const auto &X = Insn->detail->x86;
  const auto Source = L.operandRead(S, X.operands[1]);
  const auto Destination = L.operandWrite(X.operands[0]);
  NdVar SourceWasZero;
  if (!E->NoFlags && Insn->id != X86_INS_POPCNT) {
    SourceWasZero = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, SourceWasZero,
           {Source, NdVar::scalar(0, Source.Size)});
  }
  if (Insn->id == X86_INS_TZCNT) {
    const auto Inverted = S.makeTemp(Source.Size);
    S.emit(NdOp::INT_NOT, Inverted, {Source});
    const auto Previous = S.makeTemp(Source.Size);
    S.emit(NdOp::INT_SUB, Previous, {Source, NdVar::scalar(1, Source.Size)});
    const auto LowZeros = S.makeTemp(Source.Size);
    S.emit(NdOp::INT_AND, LowZeros, {Inverted, Previous});
    S.emit(NdOp::POPCOUNT, Destination, {LowZeros});
  } else {
    S.emit(Insn->id == X86_INS_LZCNT ? NdOp::LZCOUNT : NdOp::POPCOUNT,
           Destination, {Source});
  }
  if (!E->NoFlags) {
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Destination, NdVar::scalar(0, Destination.Size)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1),
           {Insn->id == X86_INS_POPCNT ? NdVar::scalar(0, 1) : SourceWasZero});
    for (unsigned Flag : UndefinedFlags) {
      S.emit(NdOp::COPY, NdVar::reg(Flag, 1), {NdVar::scalar(0, 1)});
      if (Insn->id != X86_INS_POPCNT)
        S.recordUndefinedBits(NdVar::reg(Flag, 1), 0, 1);
    }
  }
  return true;
}

} // namespace neverd::bitcount
