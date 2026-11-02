//===- X86LiftADX.cpp - Independent carry-chain semantics ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate ADCX/ADOX before any load, destination or flag publication.
//===----------------------------------------------------------------------===//

#include "X86LiftADX.h"

#include "X86LegacyScalarPrefixes.h"
#include "X86LiftAPXValidation.h"
#include "X86LiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/support/ISAEncoding.h"

#include <algorithm>
#include <array>
#include <optional>

namespace neverd::adx {
namespace {

// The returned index selects the first addend. The following operand is RM.
std::optional<unsigned> encoding(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || !isInstruction(Insn->id) || !Insn->size ||
      Insn->size > x86::kMaxInstructionLen || Insn->illegal ||
      (Target != Arch::X86 && Target != Arch::X64))
    return std::nullopt;
  const auto &X = Insn->detail->x86;
  if ((X.op_count != 2 && X.op_count != 3) || X.rex2 || X.avx_sae ||
      X.avx_rm != X86_AVX_RM_INVALID || X.xop_cc != X86_XOP_CC_INVALID ||
      X.sse_cc != X86_SSE_CC_INVALID || X.avx_cc != X86_AVX_CC_INVALID ||
      X.encoding.imm_size || X.encoding.imm_offset)
    return std::nullopt;
  for (unsigned I = 0; I != X.op_count; ++I)
    if (X.operands[I].avx_bcast != X86_AVX_BCAST_INVALID ||
        X.operands[I].avx_zero_opmask)
      return std::nullopt;
  const bool Overflow = Insn->id == X86_INS_ADOX;
  const uint64_t Flags = Overflow ? X86_EFLAGS_TEST_OF | X86_EFLAGS_MODIFY_OF
                                  : X86_EFLAGS_TEST_CF | X86_EFLAGS_MODIFY_CF;
  const bool Apx = apxvalidation::isPresent(Insn, X);
  // Legacy Capstone omits TEST_OF and the destructive destination's read
  // access for ADOX. Authenticate its detail without inheriting that omission.
  if (!apxvalidation::implicitDetail(
          Insn, X, !Apx && Overflow ? X86_EFLAGS_MODIFY_OF : Flags,
          {X86_REG_EFLAGS}, {X86_REG_EFLAGS}))
    return std::nullopt;
  if (Apx) {
    apxvalidation::Header H;
    if (Target != Arch::X64 || !apxvalidation::decodeHeader(Insn, X, H) ||
        X.rex || (H.P0 & 7) != 4 || H.Opcode != 0x66 ||
        (H.P1 & 3) != (Overflow ? 2 : 1) || (H.P2 & 0xe7) != 0 ||
        (!H.Memory && (H.P1 & 4) == 0))
      return std::nullopt;
    const bool NDD = H.P2 & 0x10;
    const unsigned Width = H.P1 & 0x80 ? 8 : 4;
    const unsigned First = NDD ? 1 : 0;
    if (X.op_count != (NDD ? 3 : 2) || (!NDD && apxvalidation::vvvvv(H) != 0) ||
        (NDD &&
         !apxvalidation::registerOperand(X.operands[0], apxvalidation::vvvvv(H),
                                         Width, CS_AC_WRITE)) ||
        !apxvalidation::registerOperand(
            X.operands[First], apxvalidation::modrmReg(H), Width,
            NDD ? CS_AC_READ : CS_AC_READ | CS_AC_WRITE) ||
        !apxvalidation::validateRM(Insn, X, H, X.operands[First + 1], Width,
                                   CS_AC_READ))
      return std::nullopt;
    return First;
  }

  const bool Mode64 = Target == Arch::X64;
  legacyvalidation::ScalarPrefixes P;
  if (!legacyvalidation::scalarPrefixes(Insn, Mode64, P) ||
      (Overflow ? !P.F3 : !P.Operand16 || P.F3))
    return std::nullopt;
  const size_t Offset = P.OpcodeOffset;
  const unsigned Width = P.Rex & 8 ? 8 : 4;
  const unsigned AddressSize =
      Mode64 ? (P.Address ? 4 : 8) : (P.Address ? 2 : 4);
  const unsigned RegExtension = P.Rex & 4 ? 8 : 0;
  const unsigned BaseExtension = P.Rex & 1 ? 8 : 0;
  const unsigned IndexExtension = P.Rex & 2 ? 8 : 0;
  constexpr std::array<uint8_t, 4> OpcodeDetail = {0x0f, 0xf6, 0, 0};
  // ADX is legacy map 2. REX2 can encode only maps 0/1, never 0F 38.
  if (Insn->size - Offset < 4 || Insn->bytes[Offset] != 0x0f ||
      Insn->bytes[Offset + 1] != 0x38 || Insn->bytes[Offset + 2] != 0xf6 ||
      X.op_count != 2 || X.rex != P.Rex || X.prefix[0] ||
      X.prefix[1] != P.Segment || X.prefix[2] != (P.Operand16 ? 0x66 : 0) ||
      X.prefix[3] != (P.Address ? 0x67 : 0) || X.addr_size != AddressSize ||
      X.encoding.modrm_offset != Offset + 3 ||
      X.modrm != Insn->bytes[Offset + 3] ||
      !std::equal(OpcodeDetail.begin(), OpcodeDetail.end(), X.opcode) ||
      !apxvalidation::registerOperand(
          X.operands[0], ((X.modrm >> 3) & 7) | RegExtension, Width,
          Overflow ? CS_AC_WRITE : CS_AC_READ | CS_AC_WRITE) ||
      X.operands[1].size != Width || X.operands[1].access != CS_AC_READ ||
      ((X.modrm & 0xc0) == 0xc0
           ? !apxvalidation::registerOperand(X.operands[1],
                                             (X.modrm & 7) | BaseExtension,
                                             Width, CS_AC_READ)
           : X.operands[1].type != X86_OP_MEM) ||
      !validateCanonicalScalarConversionTail(Insn, X, Offset + 4, P.Segment,
                                             Mode64, AddressSize, BaseExtension,
                                             IndexExtension, X.operands[1]))
    return std::nullopt;
  return 0;
}

} // namespace

bool isInstruction(unsigned Id) {
  return Id == X86_INS_ADCX || Id == X86_INS_ADOX;
}

bool form(const cs_insn *Insn, Arch Target) {
  return encoding(Insn, Target).has_value();
}

bool lift(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn) {
  const auto FirstIndex = encoding(Insn, L.targetArch());
  if (!FirstIndex)
    throw UnliftedInstruction(Insn->address, Insn->mnemonic, Insn->op_str);
  const auto &X = Insn->detail->x86;
  const auto First = L.operandRead(S, X.operands[*FirstIndex]);
  const auto Second = L.operandRead(S, X.operands[*FirstIndex + 1]);
  const auto Dst = L.operandWrite(X.operands[0]);
  const auto Flag = Insn->id == X86_INS_ADCX ? x86reg::CF : x86reg::OF;
  const auto CarryExt = S.makeTemp(First.Size);
  S.emit(NdOp::INT_ZEXT, CarryExt, {NdVar::reg(Flag, 1)});
  const auto C1 = S.makeTemp(1);
  S.emit(NdOp::INT_CARRY, C1, {Second, CarryExt});
  const auto Adjusted = S.makeTemp(First.Size);
  S.emit(NdOp::INT_ADD, Adjusted, {Second, CarryExt});
  const auto C2 = S.makeTemp(1);
  S.emit(NdOp::INT_CARRY, C2, {First, Adjusted});
  const auto Result = S.makeTemp(First.Size);
  S.emit(NdOp::INT_ADD, Result, {First, Adjusted});
  const auto CarryOut = S.makeTemp(1);
  S.emit(NdOp::BOOL_OR, CarryOut, {C1, C2});
  S.emit(NdOp::COPY, Dst, {Result});
  S.emit(NdOp::COPY, NdVar::reg(Flag, 1), {CarryOut});
  return true;
}

} // namespace neverd::adx
