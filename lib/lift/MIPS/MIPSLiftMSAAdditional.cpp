//===- MIPSLiftMSAAdditional.cpp - MSA widening arithmetic and transfers --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include <array>

namespace neverd {

bool MIPSLifter::liftMSAAdditional(State &S, uint32_t Word) const {
  const unsigned Fn = Word & 63, WD = Word >> 6 & 31, WS = Word >> 11 & 31,
                 WT = Word >> 16 & 31, Op = Word >> 23 & 7;
  uint16_t Bytes = 1u << ((Word >> 21) & 3);
  std::array<NdVar, 16> Elements;
  const auto Write = [&](auto Value) {
    for (unsigned I = 0; I < 16u / Bytes; ++I)
      Elements[I] = Value(I);
    S.writeVector(WD, llvm::MutableArrayRef(Elements).take_front(16u / Bytes));
    return true;
  };
  const auto Lane = [&](unsigned R, unsigned I) {
    return S.vectorElement(R, I, Bytes);
  };
  if (Fn >= 32 && Fn <= 39) {
    Bytes = 1u << (Fn & 3);
    const int Offset = int((Word >> 16 & 1023) ^ 512) - 512;
    const auto Base = S.binary(NdOp::INT_ADD, S.read(WS),
                               NdVar::cst(int64_t(Offset) * Bytes, S.Width));
    const auto Address = [&](unsigned I) {
      return I ? S.binary(NdOp::INT_ADD, Base, NdVar::cst(I * Bytes, S.Width))
               : Base;
    };
    // LD/ST have no alignment requirement. Address order is element order on
    // both byte orders. LOAD/STORE supply the byte order within each element.
    if (Fn & 4) {
      for (unsigned I = 0; I < 16u / Bytes; ++I)
        S.emit(NdOp::STORE, {}, {Address(I), Lane(WD, I)});
      return true;
    }
    return Write([&](unsigned I) {
      auto V = S.makeTemp(Bytes);
      S.emit(NdOp::LOAD, V, {Address(I)});
      return V;
    });
  }
  if (Fn == 7 && Op == 6) {
    const int Immediate = int(((Word >> 11) & 1023) ^ 512) - 512;
    return Write([&](unsigned) { return NdVar::cst(Immediate, Bytes); });
  }
  if ((Word & 0xffff003f) == 0x78be0019) {
    S.emit(NdOp::COPY, NdVar::reg(mipsreg::w(WD), 16),
           {NdVar::reg(mipsreg::w(WS), 16)});
    return true;
  }
  if ((Word & 0xfffc003f) == 0x7b00001e) {
    Bytes = 1u << (WT & 3);
    if (Bytes > S.Width)
      return false;
    return Write([&](unsigned) { return S.read(WS, Bytes); });
  }
  if (Fn == 25) {
    const unsigned Major = Word >> 22 & 15, Tag = Word >> 16 & 63;
    if (Major != 2 && Major != 3 && Major != 4)
      return false;
    Bytes = msaElementBytes(Tag);
    // COPY_U has no encoding for an element as wide as its GPR.
    if (!Bytes || Bytes > S.Width || (Major == 3 && Bytes == S.Width))
      return false;
    const unsigned Index = Tag & (16 / Bytes - 1);
    if (Major == 4)
      return Write([&](unsigned I) {
        return I == Index ? S.read(WS, Bytes) : Lane(WD, I);
      });
    if (WD)
      S.write(WD, Lane(WS, Index), Major == 2);
    else
      S.emit(NdOp::NOP, {}, {});
    return true;
  }
  if (Fn == 1 && (Word >> 24 & 3) < 3) {
    const unsigned Kind = Word >> 24 & 3;
    const auto Imm = NdVar::cst(
        uint64_t((Word >> 16) & 255) * UINT64_C(0x0101010101010101), 8);
    Bytes = 8;
    return Write([&](unsigned I) {
      const auto D = Lane(WD, I), X = Lane(WS, I);
      const auto Mask = Kind == 2 ? D : Imm;
      const auto Yes = Kind == 2 ? Imm : Kind == 0 ? X : D;
      const auto No = Kind == 2 ? X : Kind == 0 ? D : X;
      return S.binary(
          NdOp::INT_XOR, No,
          S.binary(NdOp::INT_AND, Mask, S.binary(NdOp::INT_XOR, No, Yes)));
    });
  }
  if ((Fn == 21 && Op >= 4) || (Fn == 19 && Op < 6)) {
    if (Bytes == 1)
      return false;
    const bool Signed = !(Op & 1);
    const auto Wide = [&](unsigned R, unsigned I) {
      return S.resize(S.vectorElement(R, I, Bytes / 2), Bytes, Signed);
    };
    return Write([&](unsigned I) {
      if (Fn == 21)
        return S.binary(Op & 2 ? NdOp::INT_SUB : NdOp::INT_ADD,
                        Wide(WS, 2 * I + 1), Wide(WT, 2 * I));
      const auto Product = [&](unsigned N) {
        return S.binary(NdOp::INT_MULT, Wide(WS, N), Wide(WT, N));
      };
      const auto Dot =
          S.binary(NdOp::INT_ADD, Product(2 * I), Product(2 * I + 1));
      return Op < 2 ? Dot
                    : S.binary(Op < 4 ? NdOp::INT_ADD : NdOp::INT_SUB,
                               Lane(WD, I), Dot);
    });
  }
  if (Fn == 17 && (Op == 2 || Op == 3)) {
    const uint64_t SignBit = uint64_t{1} << (Bytes * 8 - 1);
    return Write([&](unsigned I) {
      const auto X = Lane(WS, I), Y = Lane(WT, I);
      const auto Diff = S.binary(NdOp::INT_SUB, X, Y);
      const auto Borrow = S.binary(NdOp::INT_LESS, X, Y, 1);
      if (Op == 2) {
        const auto Negative =
            S.binary(NdOp::INT_SLESS, Y, NdVar::cst(0, Bytes), 1);
        const auto Carry = S.binary(NdOp::INT_LESS, Diff, X, 1);
        return S.select(Negative, S.select(Carry, NdVar::cst(-1, Bytes), Diff),
                        S.select(Borrow, NdVar::cst(0, Bytes), Diff));
      }
      const auto Negative =
          S.binary(NdOp::INT_SLESS, Diff, NdVar::cst(0, Bytes), 1);
      return S.select(Borrow,
                      S.select(Negative, Diff, NdVar::cst(SignBit, Bytes)),
                      S.select(Negative, NdVar::cst(SignBit - 1, Bytes), Diff));
    });
  }
  if (Fn == 28 && (Op == 2 || Op == 3 || Op == 6 || Op == 7)) {
    const unsigned Kind = Word >> 22 & 7;
    if (Kind < 4 || Kind > 6)
      return false;
    Bytes = (Word & (1u << 21)) ? 4 : 2;
    const uint16_t Wide = 2 * Bytes;
    const unsigned FractionBits = 8 * Bytes - 1;
    const int64_t One = int64_t{1} << FractionBits;
    return Write([&](unsigned I) {
      const auto X = S.resize(Lane(WS, I), Wide, true),
                 Y = S.resize(Lane(WT, I), Wide, true);
      auto P = S.binary(NdOp::INT_MULT, X, Y);
      if (Kind != 4) {
        const auto D =
            S.binary(NdOp::INT_LEFT, S.resize(Lane(WD, I), Wide, true),
                     NdVar::cst(FractionBits, Wide));
        P = S.binary(Kind == 5 ? NdOp::INT_ADD : NdOp::INT_SUB, D, P);
      }
      if (Word & (1u << 25))
        P = S.binary(NdOp::INT_ADD, P, NdVar::cst(One / 2, Wide));
      const auto V =
          S.binary(NdOp::INT_ASHR, P, NdVar::cst(FractionBits, Wide));
      const auto Min = NdVar::cst(-One, Wide), Max = NdVar::cst(One - 1, Wide);
      return S.resize(
          S.select(S.binary(NdOp::INT_SLESS, V, Min, 1), Min,
                   S.select(S.binary(NdOp::INT_SLESS, Max, V, 1), Max, V)),
          Bytes);
    });
  }
  if (Fn == 18 && Op >= 4) {
    const unsigned Index = (Op - 4) * 4 + ((Word >> 21) & 3);
    const auto Id =
        static_cast<Intrinsic>(unsigned(Intrinsic::MIPSMSADIV_S_B) + Index);
    S.emitIntrinsic(
        Id, NdVar::reg(mipsreg::w(WD), 16),
        {NdVar::reg(mipsreg::w(WS), 16), NdVar::reg(mipsreg::w(WT), 16)});
    return true;
  }
  return false;
}
} // namespace neverd
