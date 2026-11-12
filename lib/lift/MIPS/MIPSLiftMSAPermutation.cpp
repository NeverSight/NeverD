//===- MIPSLiftMSAPermutation.cpp - MSA lane and column permutations ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Vector lanes are indexed from register bit zero on both byte orders. Capture
/// all sources before committing WD, including three-way operand aliases.
//===----------------------------------------------------------------------===//

#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include <algorithm>
#include <array>

namespace neverd {

bool MIPSLifter::liftMSAPermutation(State &S, uint32_t Word) const {
  const unsigned Fn = Word & 63, WD = (Word >> 6) & 31, WS = (Word >> 11) & 31,
                 WT = (Word >> 16) & 31;
  const unsigned Op = (Word >> 23) & 7, Major = (Word >> 22) & 15;
  const bool Element = Fn == 25, ShuffleSet = Fn == 2, VectorShuffle = Fn == 21;
  unsigned Bytes = 1u << ((Word >> 21) & 3);
  if (ShuffleSet) {
    const unsigned Size = (Word >> 24) & 3;
    if (Size == 3)
      return false;
    Bytes = 1u << Size;
  }
  if (Element) {
    Bytes = msaElementBytes((Word >> 16) & 63);
    if (!Bytes)
      return false;
  }
  const unsigned Count = 16 / Bytes;
  const auto Index = WT & (Count - 1);
  const auto Select = [&](NdVar Condition, NdVar A, NdVar B) {
    auto Result = S.makeTemp(A.Size);
    S.emit(NdOp::SELECT, Result, {Condition, A, B});
    return Result;
  };
  const auto Bit = [&](NdVar X, unsigned Mask) {
    return S.binary(NdOp::INT_NOTEQUAL,
                    S.binary(NdOp::INT_AND, X, NdVar::cst(Mask, X.Size)),
                    NdVar::cst(0, X.Size), 1);
  };
  std::array<NdVar, 16> Values;
  const auto Finish = [&](unsigned N) {
    S.writeVector(WD, llvm::MutableArrayRef(Values).take_front(N));
    return true;
  };
  if (Element && Major == 5) { // INSVE: only ws[0] is observed.
    for (unsigned I = 0; I < Count; ++I)
      Values[I] =
          S.vectorElement(I == Index ? WS : WD, I == Index ? 0 : I, Bytes);
    return Finish(Count);
  }
  if (ShuffleSet || (Fn == 20 && Op >= 2)) {
    for (unsigned I = 0; I < Count; ++I) {
      unsigned Source, Lane;
      if (ShuffleSet) {
        Source = WS;
        Lane = (I & ~3u) | ((Word >> (16 + 2 * (I & 3))) & 3);
      } else if (Op < 4) { // PCKEV/OD
        Source = I < Count / 2 ? WT : WS;
        Lane = (2 * I + (Op & 1)) % Count;
      } else {
        Source = (I & 1) ? WS : WT;
        Lane =
            Op >= 6 ? (I & ~1u) + (Op & 1) : I / 2 + (Op == 4 ? Count / 2 : 0);
      }
      Values[I] = S.vectorElement(Source, Lane, Bytes);
    }
    return Finish(Count);
  }
  const auto ReadHalves = [&](unsigned R) {
    return std::array{S.vectorElement(R, 0, 8), S.vectorElement(R, 1, 8)};
  };
  const auto Indexed = [&](const std::array<NdVar, 2> &Parts, NdVar N) {
    auto Part = Select(Bit(N, Count / 2), Parts[1], Parts[0]);
    if (Bytes < 8) {
      auto Within = S.binary(NdOp::INT_AND, N, NdVar::cst(Count / 2 - 1, 4));
      auto Shift = S.binary(NdOp::INT_MULT, Within, NdVar::cst(Bytes * 8, 4));
      Part = S.binary(NdOp::INT_RIGHT, Part, S.resize(Shift, 8));
    }
    return S.resize(Part, Bytes);
  };
  if (VectorShuffle) {
    const auto A = ReadHalves(WS), B = ReadHalves(WT);
    for (unsigned I = 0; I < Count; ++I) {
      const auto N = S.resize(S.vectorElement(WD, I, Bytes), 4);
      const auto Part = Select(Bit(N, Count), Indexed(A, N), Indexed(B, N));
      Values[I] = Select(Bit(N, 0xc0), NdVar::cst(0, Bytes), Part);
    }
    return Finish(Count);
  }
  if ((Element && Major == 1) || (Fn == 20 && Op == 1)) {
    const auto Value = Element ? S.vectorElement(WS, Index, Bytes)
                               : Indexed(ReadHalves(WS), S.read(WT, 4));
    Values.fill(Value);
    return Finish(Count);
  }
  // SLD/SLDI slide bytes within each of Bytes rows, each Count bytes wide.
  // Operate on at most 64 bits at a time so o32 source never needs a 256-bit
  // concatenation or a shift by the precision of its carrier.
  const unsigned Chunk = std::min(Count, 8u), Chunks = 16 / Chunk;
  const auto N =
      Element ? NdVar::cst(Index, 4)
              : S.binary(NdOp::INT_AND, S.read(WT, 4), NdVar::cst(Count - 1, 4));
  if (Element && !Index) {
    S.emit(NdOp::COPY, NdVar::reg(mipsreg::w(WD), 16),
           {NdVar::reg(mipsreg::w(WS), 16)});
    return true;
  }
  const auto Shift = S.binary(
      NdOp::INT_MULT, S.binary(NdOp::INT_AND, N, NdVar::cst(Chunk - 1, 4)),
      NdVar::cst(8, 4));
  const auto Back =
      S.binary(NdOp::INT_AND, S.binary(NdOp::INT_SUB, NdVar::cst(0, 4), Shift),
               NdVar::cst(Chunk * 8 - 1, 4));
  const auto Nonzero = S.binary(NdOp::INT_NOTEQUAL, Shift, NdVar::cst(0, 4), 1);
  for (unsigned I = 0; I < Chunks; ++I) {
    auto Low = S.vectorElement(WS, I, Chunk),
         High = S.vectorElement(WD, I, Chunk);
    if (Count == 16) {
      const auto Middle =
          I ? S.vectorElement(WD, 0, 8) : S.vectorElement(WS, 1, 8);
      const auto Upper = Bit(N, 8);
      Low = Select(Upper, Middle, Low);
      High = Select(Upper, High, Middle);
    }
    Values[I] = S.binary(
        NdOp::INT_OR, S.binary(NdOp::INT_RIGHT, Low, S.resize(Shift, Chunk)),
        Select(Nonzero, S.binary(NdOp::INT_LEFT, High, S.resize(Back, Chunk)),
               NdVar::cst(0, Chunk)));
  }
  return Finish(Chunks);
}

} // namespace neverd
