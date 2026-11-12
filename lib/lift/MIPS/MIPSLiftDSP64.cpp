//===- MIPSLiftDSP64.cpp - Early MIPS64 DSP packed GPRs -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/lift/MIPS/MIPSDSP64.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include "llvm/Support/MathExtras.h"

#include <array>

namespace neverd {

bool MIPSLifter::liftDSP64Packed(State &S, uint32_t Word) const {
  if (!mips::dsp64PackedForm(Word))
    return false;
  const unsigned Fn = Word & 63, Sel = (Word >> 6) & 31;
  const unsigned RS = Word >> 21 & 31, RT = Word >> 16 & 31,
                 RD = Word >> 11 & 31;
  const auto A = S.read(RS), B = S.read(RT);
  const auto Lane = [&](NdVar V, unsigned I, uint16_t Bytes) {
    if (!I)
      return S.resize(V, Bytes);
    auto L = S.makeTemp(Bytes);
    S.emit(NdOp::SUBBYTES, L, {V, NdVar::cst(I * Bytes, 4)});
    return L;
  };
  const auto Pack = [&](uint16_t Bytes, auto Element) {
    std::array<NdVar, 8> Lanes;
    const unsigned N = 8 / Bytes;
    for (unsigned I = 0; I < N; ++I)
      Lanes[I] = Element(I);
    for (unsigned Step = 1; Step < N; Step *= 2)
      for (unsigned I = 0; I < N; I += Step * 2)
        Lanes[I] =
            S.binary(NdOp::CONCAT, Lanes[I + Step], Lanes[I], 2 * Step * Bytes);
    S.write(RD, Lanes[0]);
  };
  const auto Control = [&] {
    const auto C = S.makeTemp(4);
    S.emitIntrinsic(Intrinsic::MIPSReadDSPControl, C);
    return C;
  };
  NdVar Overflow = NdVar::cst(0, 1);
  const auto Clamp = [&](NdVar Wide, uint16_t Bytes, bool Signed,
                         bool Saturate) {
    const auto Trunc = S.resize(Wide, Bytes);
    const auto Lost = S.binary(NdOp::INT_NOTEQUAL, Wide,
                               S.resize(Trunc, Wide.Size, Signed), 1);
    Overflow = S.binary(NdOp::BOOL_OR, Overflow, Lost, 1);
    if (!Saturate)
      return Trunc;
    const uint64_t Sign = uint64_t(1) << (Bytes * 8 - 1);
    const auto Limit =
        S.select(S.binary(NdOp::INT_SLESS, Wide, NdVar::cst(0, Wide.Size), 1),
                 NdVar::cst(Signed ? Sign : 0, Bytes),
                 NdVar::cst(Signed ? Sign - 1 : Sign * 2 - 1, Bytes));
    return S.select(Lost, Limit, Trunc);
  };
  const auto Sticky = [&](unsigned Bit) {
    S.emitIntrinsic(Intrinsic::MIPSWriteDSPControl, {},
                    {S.binary(NdOp::INT_OR, Control(),
                              S.binary(NdOp::INT_LEFT, S.resize(Overflow, 4),
                                       NdVar::cst(Bit, 4)))});
  };
  if (Fn == 0x14 && Sel == 20) { // RADDU.L.OB
    auto Sum = NdVar::cst(0, 8);
    for (unsigned I = 0; I < 8; ++I)
      Sum = S.binary(NdOp::INT_ADD, Sum, S.resize(Lane(A, I, 1), 8));
    S.write(RD, Sum);
    return true;
  }
  if (Fn == 0x14 && (Sel == 6 || Sel == 7 || Sel >= 28)) {
    const bool Unsigned = Sel < 8;
    const bool Rounded = Sel == 31;
    Pack(Unsigned || Rounded ? 2 : 4, [&](unsigned I) {
      const unsigned N = I + (Sel == 28 ? 2 : 0);
      const auto X =
          Unsigned ? Lane(A, I + (Sel == 6 ? 4 : 0), 1) : Lane(A, N, 2);
      const auto Y = Lane(B, Unsigned ? I : N, 2);
      auto Product = S.binary(NdOp::INT_MULT, S.resize(X, 8, !Unsigned),
                              S.resize(Y, 8, !Unsigned));
      if (Unsigned)
        return Clamp(Product, 2, false, true);
      Product = S.binary(NdOp::INT_LEFT, Product, NdVar::cst(1, 8));
      auto Q31 = Clamp(Product, 4, true, true);
      if (!Rounded)
        return Q31;
      auto Sum = S.binary(NdOp::INT_ADD, S.resize(Q31, 8, true),
                          NdVar::cst(0x8000, 8));
      return Clamp(S.binary(NdOp::INT_ASHR, Sum, NdVar::cst(16, 8)), 2, true,
                   true);
    });
    Sticky(21);
    S.discardHILO();
    return true;
  }
  if (Fn == 0x14) {
    const uint16_t Bytes = 1u << (Sel >> 3);
    Pack(Bytes, [&](unsigned I) {
      auto X = S.resize(Lane(A, I, Bytes), 8, Bytes > 1),
           Y = S.resize(Lane(B, I, Bytes), 8, Bytes > 1);
      return Clamp(S.binary(Sel & 1 ? NdOp::INT_SUB : NdOp::INT_ADD, X, Y),
                   Bytes, Bytes > 1, Sel & 4);
    });
    Sticky(20);
    return true;
  }
  if (Fn == 0x17) {
    const uint16_t Bytes = 1u << (Sel >> 3);
    const auto Count =
        Sel & 2 ? S.binary(NdOp::INT_AND, A, NdVar::cst(Bytes * 8 - 1, 8))
                : NdVar::cst(RS, 8);
    Pack(Bytes, [&](unsigned I) {
      auto V = S.resize(Lane(B, I, Bytes), 8, Bytes > 1);
      if (!(Sel & 1))
        return Clamp(S.binary(NdOp::INT_LEFT, V, Count), Bytes, Bytes > 1,
                     Sel & 4);
      if (Sel & 4)
        V = S.binary(NdOp::INT_LEFT, V, NdVar::cst(1, 8));
      V = S.binary(Bytes > 1 ? NdOp::INT_ASHR : NdOp::INT_RIGHT, V, Count);
      if (Sel & 4)
        V = S.binary(NdOp::INT_ASHR,
                     S.binary(NdOp::INT_ADD, V, NdVar::cst(1, 8)),
                     NdVar::cst(1, 8));
      return S.resize(V, Bytes);
    });
    if (!(Sel & 1))
      Sticky(22);
    return true;
  }
  if (Fn == 0x16) {
    if ((Sel & 7) == 1) {
      const uint16_t Bytes = 1u << (Sel >> 3);
      Pack(Bytes, [&](unsigned I) {
        const auto V = S.resize(Lane(B, I, Bytes), 8, true),
                   Z = NdVar::cst(0, 8);
        return Clamp(S.select(S.binary(NdOp::INT_SLESS, V, Z, 1),
                              S.binary(NdOp::INT_SUB, Z, V), V),
                     Bytes, true, true);
      });
      Sticky(20);
      return true;
    }
    if ((Sel & 7) == 2 || (Sel & 7) == 3) {
      const uint16_t Bytes = 1u << (Sel >> 3);
      const auto V =
          Sel & 1
              ? S.resize(B, Bytes)
              : NdVar::cst(Bytes == 1
                               ? (Word >> 16 & 255)
                               : uint64_t(llvm::SignExtend64<10>(Word >> 16)),
                           Bytes);
      Pack(Bytes, [&](unsigned) { return V; });
      return true;
    }
    // PRECE selects a half or alternating lanes and widens each one.
    const uint16_t In = Sel >= 28 || Sel < 8 ? 1 : Sel < 16 ? 2 : 4;
    const unsigned Shift = Sel >= 28 ? 0 : Sel < 8 ? 7 : In * 8;
    const unsigned Half = 4 / In, Choice = Sel & 3;
    Pack(In * 2, [&](unsigned I) {
      const unsigned N =
          Choice >= 2 ? 2 * I + (Choice == 2) : I + (Choice == 0 ? Half : 0);
      auto V = S.resize(Lane(B, N, In), 2 * In);
      return Shift ? S.binary(NdOp::INT_LEFT, V, NdVar::cst(Shift, V.Size)) : V;
    });
    return true;
  }
  // Fn 0x15 contains compares, picks and narrowing packs.
  if ((Sel & 7) == 3) {
    const uint16_t Bytes = 1u << (Sel >> 3);
    const auto C = Control();
    Pack(Bytes, [&](unsigned I) {
      const auto Bit =
          S.binary(NdOp::INT_AND, C, NdVar::cst(1u << (24 + I), 4));
      return S.select(S.binary(NdOp::INT_NOTEQUAL, Bit, NdVar::cst(0, 4), 1),
                      Lane(A, I, Bytes), Lane(B, I, Bytes));
    });
    return true;
  }
  if ((Sel & 7) <= 2 || (Sel >= 4 && Sel <= 6)) {
    const uint16_t Bytes = 1u << (Sel >> 3);
    auto Bits = NdVar::cst(0, 4);
    for (unsigned I = 0; I < 8u / Bytes; ++I) {
      const auto Op =
          (Sel & 3) == 0 ? NdOp::INT_EQUAL
          : (Sel & 3) == 1
              ? (Bytes == 1 ? NdOp::INT_LESS : NdOp::INT_SLESS)
              : (Bytes == 1 ? NdOp::INT_LESSEQUAL : NdOp::INT_SLESSEQUAL);
      auto V =
          S.resize(S.binary(Op, Lane(A, I, Bytes), Lane(B, I, Bytes), 1), 4);
      Bits = S.binary(NdOp::INT_OR, Bits,
                      S.binary(NdOp::INT_LEFT, V, NdVar::cst(I, 4)));
    }
    if (Sel >= 4 && Sel <= 6)
      S.write(RD, Bits, false);
    else {
      const uint32_t Mask = ((1u << (8 / Bytes)) - 1) << 24;
      S.emitIntrinsic(
          Intrinsic::MIPSWriteDSPControl, {},
          {S.binary(NdOp::INT_OR,
                    S.binary(NdOp::INT_AND, Control(), NdVar::cst(~Mask, 4)),
                    S.binary(NdOp::INT_LEFT, Bits, NdVar::cst(24, 4)))});
    }
    return true;
  }
  if (Sel == 14) { // PACKRL.PW
    S.write(RD, S.binary(NdOp::CONCAT, Lane(A, 0, 4), Lane(B, 1, 4), 8));
    return true;
  }
  const uint16_t Bytes = Sel < 16 ? 1 : Sel < 24 ? 2 : 4;
  Pack(Bytes, [&](unsigned I) {
    const unsigned Half = 4 / Bytes;
    const auto V = I < Half ? B : A;
    auto L = Lane(V, I % Half, Bytes * 2);
    if (Sel == 15 || Sel == 21) {
      auto X = S.resize(L, 8, true);
      if (Sel == 15) {
        // Q15 values above 0x7f80 saturate even if shifting would
        // truncate to the same largest representable unsigned Q7 byte.
        Overflow =
            S.binary(NdOp::BOOL_OR, Overflow,
                     S.binary(NdOp::INT_SLESS, NdVar::cst(0x7f80, 8), X, 1), 1);
      }
      if (Sel == 21)
        X = S.binary(NdOp::INT_ADD, X, NdVar::cst(0x8000, 8));
      return Clamp(
          S.binary(NdOp::INT_ASHR, X, NdVar::cst(Sel == 15 ? 7 : 16, 8)), Bytes,
          Sel == 21, true);
    }
    return Lane(L, 1, Bytes);
  });
  if (Sel == 15 || Sel == 21)
    Sticky(22);
  return true;
}
} // namespace neverd
