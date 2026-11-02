//===- X86LiftBMI2.cpp - Flag-preserving BMI2 values ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate BMI2 bytes and decoded operands before publishing values.
//===----------------------------------------------------------------------===//

#include "X86LiftBMI2.h"

#include "X86FPEncoding.h"
#include "X86LiftAPXValidation.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/ArrayRef.h"

#include <initializer_list>
#include <optional>

namespace neverd::bmi2 {
namespace {

std::optional<unsigned> encoding(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || !isInstruction(Insn->id) || !Insn->size ||
      Insn->size > x86::kMaxInstructionLen || Insn->illegal ||
      (Target != Arch::X86 && Target != Arch::X64))
    return std::nullopt;
  const auto &X = Insn->detail->x86;
  const unsigned Id = Insn->id;
  const bool Multiply = Id == X86_INS_MULX;
  const bool Rotate = Id == X86_INS_RORX;
  const bool Scatter = Id == X86_INS_PDEP || Id == X86_INS_PEXT;
  const unsigned RMOperand = Multiply || Scatter ? 2 : 1;
  const unsigned VOperand = Multiply || Scatter ? 1 : 2;
  const unsigned Map = Rotate ? 3 : 2;
  const unsigned PP = Id == X86_INS_SHLX                         ? 1
                      : Id == X86_INS_SARX || Id == X86_INS_PEXT ? 2
                                                                 : 3;
  const uint8_t Opcode = Multiply  ? 0xf6
                         : Scatter ? 0xf5
                         : Rotate  ? 0xf0
                                   : 0xf7;
  if (X.op_count != 3 || X.rex2 || X.avx_sae ||
      X.avx_rm != X86_AVX_RM_INVALID || X.xop_cc != X86_XOP_CC_INVALID ||
      X.sse_cc != X86_SSE_CC_INVALID || X.avx_cc != X86_AVX_CC_INVALID ||
      X.encoding.imm_size != (Rotate ? 1 : 0) ||
      X.encoding.imm_offset != (Rotate ? Insn->size - 1 : 0))
    return std::nullopt;
  for (const auto &Operand : llvm::ArrayRef(X.operands, X.op_count))
    if (Operand.avx_bcast != X86_AVX_BCAST_INVALID || Operand.avx_zero_opmask)
      return std::nullopt;

  const bool Apx = apxvalidation::isPresent(Insn, X);
  unsigned Width;
  if (Apx) {
    apxvalidation::Header H;
    if (Target != Arch::X64 ||
        !apxvalidation::decodeHeader(Insn, X, H, Rotate) || X.rex ||
        (H.P0 & 7) != Map || H.Opcode != Opcode || (H.P1 & 3) != PP ||
        (H.P2 & 0xf7) != 0 || (!H.Memory && (H.P1 & 4) == 0) ||
        (Rotate && apxvalidation::vvvvv(H) != 0))
      return std::nullopt;
    Width = H.P1 & 0x80 ? 8 : 4;
    if (!apxvalidation::registerOperand(
            X.operands[0], apxvalidation::modrmReg(H), Width, CS_AC_WRITE) ||
        (!Rotate && !apxvalidation::registerOperand(
                        X.operands[VOperand], apxvalidation::vvvvv(H), Width,
                        Multiply ? CS_AC_WRITE : CS_AC_READ)) ||
        !apxvalidation::validateRM(Insn, X, H, X.operands[RMOperand], Width,
                                   CS_AC_READ, Rotate))
      return std::nullopt;
  } else {
    CanonicalVex3EncodingInfo E;
    if (!parseCanonicalVex3EncodingInfo(Insn, X, Target, E) ||
        (E.P0 & 0x1f) != Map || (E.P1 & 7) != PP || E.Opcode != Opcode ||
        (Rotate && (E.P1 & 0x78) != 0x78))
      return std::nullopt;
    Width = E.Is64Bit && (E.P1 & 0x80) ? 8 : 4;
    const auto Register = [&](unsigned Operand, unsigned Index,
                              uint8_t Access) {
      return matchesFPGPROperand(X.operands[Operand], Index, Width) &&
             X.operands[Operand].access == Access;
    };
    const auto &RM = X.operands[RMOperand];
    if (!Register(0, E.regIndex(), CS_AC_WRITE) ||
        (!Rotate && !Register(VOperand, E.vvvvIndex(),
                              Multiply ? CS_AC_WRITE : CS_AC_READ)) ||
        RM.size != Width || RM.access != CS_AC_READ ||
        ((E.ModRM & 0xc0) == 0xc0
             ? !Register(RMOperand, E.rmIndex(), CS_AC_READ)
             : RM.type != X86_OP_MEM) ||
        !validateCanonicalScalarConversionTail(
            Insn, X, E.Offset + 5, E.SegmentPrefix, E.Is64Bit, E.AddressSize,
            E.baseExtension(), E.indexExtension(), RM, Rotate))
      return std::nullopt;
  }
  // Legacy Capstone omits immediate access; APX explicitly marks it read.
  if (Rotate && (X.operands[2].type != X86_OP_IMM || X.operands[2].size != 1 ||
                 X.operands[2].access != (Apx ? CS_AC_READ : 0) ||
                 X.operands[2].imm != Insn->bytes[Insn->size - 1]))
    return std::nullopt;
  if (!apxvalidation::implicitDetail(
          Insn, X, 0,
          Multiply ? std::initializer_list<x86_reg>{Width == 8 ? X86_REG_RDX
                                                               : X86_REG_EDX}
                   : std::initializer_list<x86_reg>{},
          {}))
    return std::nullopt;
  return Width;
}

void emitMulx64(X86Lifter::LiftState &S, NdVar Left, NdVar Right, NdVar &Low,
                NdVar &High) {
  constexpr uint64_t Lo32Mask = UINT64_C(0xffffffff);
  NdVar A0 = S.makeTemp(8), A1 = S.makeTemp(8);
  NdVar B0 = S.makeTemp(8), B1 = S.makeTemp(8);
  S.emit(NdOp::INT_AND, A0, {Left, NdVar::scalar(Lo32Mask, 8)});
  S.emit(NdOp::INT_RIGHT, A1, {Left, NdVar::scalar(32, 8)});
  S.emit(NdOp::INT_AND, B0, {Right, NdVar::scalar(Lo32Mask, 8)});
  S.emit(NdOp::INT_RIGHT, B1, {Right, NdVar::scalar(32, 8)});

  NdVar P0 = S.makeTemp(8), Cross10 = S.makeTemp(8);
  NdVar Cross01 = S.makeTemp(8), P3 = S.makeTemp(8);
  S.emit(NdOp::INT_MULT, P0, {A0, B0});
  S.emit(NdOp::INT_MULT, Cross10, {A1, B0});
  S.emit(NdOp::INT_MULT, Cross01, {A0, B1});
  S.emit(NdOp::INT_MULT, P3, {A1, B1});

  NdVar P0High = S.makeTemp(8), T = S.makeTemp(8);
  S.emit(NdOp::INT_RIGHT, P0High, {P0, NdVar::scalar(32, 8)});
  S.emit(NdOp::INT_ADD, T, {Cross10, P0High});
  NdVar TLow = S.makeTemp(8), THigh = S.makeTemp(8);
  S.emit(NdOp::INT_AND, TLow, {T, NdVar::scalar(Lo32Mask, 8)});
  S.emit(NdOp::INT_RIGHT, THigh, {T, NdVar::scalar(32, 8)});
  NdVar Middle = S.makeTemp(8);
  S.emit(NdOp::INT_ADD, Middle, {TLow, Cross01});
  NdVar MiddleHigh = S.makeTemp(8);
  S.emit(NdOp::INT_RIGHT, MiddleHigh, {Middle, NdVar::scalar(32, 8)});
  NdVar HighBase = S.makeTemp(8);
  S.emit(NdOp::INT_ADD, HighBase, {P3, THigh});
  High = S.makeTemp(8);
  S.emit(NdOp::INT_ADD, High, {HighBase, MiddleHigh});

  NdVar MiddleLow = S.makeTemp(8), LowUpper = S.makeTemp(8);
  NdVar P0Low = S.makeTemp(8);
  S.emit(NdOp::INT_AND, MiddleLow, {Middle, NdVar::scalar(Lo32Mask, 8)});
  S.emit(NdOp::INT_LEFT, LowUpper, {MiddleLow, NdVar::scalar(32, 8)});
  S.emit(NdOp::INT_AND, P0Low, {P0, NdVar::scalar(Lo32Mask, 8)});
  Low = S.makeTemp(8);
  S.emit(NdOp::INT_OR, Low, {LowUpper, P0Low});
}

} // namespace

bool isInstruction(unsigned Id) {
  return Id == X86_INS_MULX || Id == X86_INS_PDEP || Id == X86_INS_PEXT ||
         Id == X86_INS_RORX || Id == X86_INS_SARX || Id == X86_INS_SHLX ||
         Id == X86_INS_SHRX;
}

bool form(const cs_insn *Insn, Arch Target) {
  return encoding(Insn, Target).has_value();
}

bool lift(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn) {
  const auto Width = encoding(Insn, L.targetArch());
  if (!Width)
    throw UnliftedInstruction(Insn->address, Insn->mnemonic, Insn->op_str);
  const auto &X = Insn->detail->x86;
  const unsigned Id = Insn->id;
  const auto Dst = L.operandWrite(X.operands[0]);
  if (Id == X86_INS_MULX) {
    const auto Src = L.operandRead(S, X.operands[2]);
    const auto Rdx = NdVar::reg(x86reg::RDX, *Width);
    NdVar Low, High;
    if (*Width == 8) {
      emitMulx64(S, Rdx, Src, Low, High);
    } else {
      auto A = S.makeTemp(8), B = S.makeTemp(8), Product = S.makeTemp(8);
      S.emit(NdOp::INT_ZEXT, A, {Rdx});
      S.emit(NdOp::INT_ZEXT, B, {Src});
      S.emit(NdOp::INT_MULT, Product, {A, B});
      Low = S.makeTemp(4);
      High = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Low, {Product, NdVar::scalar(0, 4)});
      S.emit(NdOp::SUBBYTES, High, {Product, NdVar::scalar(4, 4)});
    }
    // Both halves are complete before either destination changes. When the
    // destinations alias each other, the high half wins architecturally.
    S.emit(NdOp::COPY, L.operandWrite(X.operands[1]), {Low});
    S.emit(NdOp::COPY, Dst, {High});
  } else {
    const auto A = L.operandRead(S, X.operands[1]);
    const auto B = Id == X86_INS_RORX ? NdVar::scalar(X.operands[2].imm, *Width)
                                      : L.operandRead(S, X.operands[2]);
    if (Id == X86_INS_PDEP || Id == X86_INS_PEXT) {
      S.emitIntrinsic(Id == X86_INS_PDEP ? Intrinsic::Pdep : Intrinsic::Pext,
                      Dst, {A, B});
    } else {
      const unsigned Bits = *Width * 8;
      auto Count = S.makeTemp(*Width);
      S.emit(NdOp::INT_AND, Count, {B, NdVar::scalar(Bits - 1, *Width)});
      if (Id == X86_INS_RORX) {
        auto Right = S.makeTemp(*Width), Complement = S.makeTemp(*Width);
        auto Left = S.makeTemp(*Width);
        S.emit(NdOp::INT_RIGHT, Right, {A, Count});
        S.emit(NdOp::INT_SUB, Complement, {NdVar::scalar(Bits, *Width), Count});
        S.emit(NdOp::INT_AND, Complement,
               {Complement, NdVar::scalar(Bits - 1, *Width)});
        S.emit(NdOp::INT_LEFT, Left, {A, Complement});
        S.emit(NdOp::INT_OR, Dst, {Right, Left});
      } else {
        S.emit(Id == X86_INS_SARX   ? NdOp::INT_ASHR
               : Id == X86_INS_SHLX ? NdOp::INT_LEFT
                                    : NdOp::INT_RIGHT,
               Dst, {A, Count});
      }
    }
  }
  return true;
}

} // namespace neverd::bmi2
