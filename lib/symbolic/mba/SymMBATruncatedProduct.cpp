//===- SymMBATruncatedProduct.cpp - Modular offset polynomials -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// Build short Horner forms for repeated word factors modulo the observed
/// width. Distinct words and the factors outside each group remain exact
/// multipliers, so they may overlap or depend on each other arbitrarily.
///
//===----------------------------------------------------------------------===//

#include "SymMBADetail.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace neverd::symbolic::detail {

namespace {

SymRef emitTruncatedPolynomial(SymContext &Ctx, SymRef Word, SymRef Scale,
                               llvm::SmallVectorImpl<llvm::APInt> &Coefficients,
                               size_t Bytes, WorkBudget &Budget,
                               size_t &Storage) {
  const unsigned Width = Ctx.width(Word);
  const unsigned Precision = Coefficients.front().getBitWidth();
  const size_t Words = llvm::APInt::getNumWords(Width);
  auto Literal = [&](const llvm::APInt &C) {
    auto K = C.sextOrTrunc(Width);
    if (Scale)
      K *= Ctx.constValue(Scale);
    return Ctx.mkConst(K);
  };
  auto Horner = [&](llvm::ArrayRef<llvm::APInt> C) {
    auto V = Literal(C.back());
    for (size_t K = C.size() - 1; K != 0; --K)
      V = Ctx.mkAdd(Literal(C[K - 1]), Ctx.mkMul(Word, V));
    return V;
  };
  while (Coefficients.size() > 1 && Coefficients.back().isZero())
    Coefficients.pop_back();
  auto V = Horner(Coefficients);
  const size_t D = Coefficients.size() - 1;
  // The j-th Newton basis element is a product of j consecutive integers.
  // Its j! divisor removes j - popcount(j) coefficient bits at every word.
  const size_t FactorialZeros = D - std::popcount(D);
  if (D >= 2 &&
      Coefficients.back().countr_zero() >=
          Precision - std::min<size_t>(Precision, FactorialZeros) &&
      D + 1 <= Storage / Bytes &&
      D + 1 <= Budget.available() / (D + 1) / (2 * Words + 3)) {
    Budget.consume((D + 1) * (D + 1) * (2 * Words + 3));
    Storage -= (D + 1) * Bytes;
    // Successive synthetic divisions by (x - J) give the Newton coefficients.
    for (size_t J = 0; J <= D; ++J) {
      for (size_t K = D; J && K > J; --K)
        Coefficients[K - 1] += Coefficients[K] * J;
      const unsigned Bits =
          Precision - std::min<size_t>(Precision, J - std::popcount(J));
      auto &C = Coefficients[J];
      if (!Bits)
        C.clearAllBits();
      else if (Bits != Precision)
        C = C.trunc(Bits).sext(Precision);
    }
    while (Coefficients.size() > 1 && Coefficients.back().isZero())
      Coefficients.pop_back();
    // Multiply the surviving Newton terms back into the monomial basis.
    for (size_t J = Coefficients.size() - 1; J > 1; --J)
      for (size_t K = J - 1; K + 1 < Coefficients.size(); ++K)
        Coefficients[K] -= Coefficients[K + 1] * (J - 1);
    const auto Reduced = Horner(Coefficients);
    if (readingScore(Ctx, Reduced) < readingScore(Ctx, V))
      V = Reduced;
  }
  return V;
}

} // namespace

SymRef foldTruncatedProduct(SymContext &Ctx, SymRef Root, unsigned Observed,
                            WorkBudget &Budget, size_t &Storage,
                            bool *EmittedReading) {
  struct Factor {
    SymRef Ref, Word, Weight, Offset;
    unsigned Zeros, Content;
    bool Complement;
  };
  const unsigned Width = Ctx.width(Root);
  const size_t N = Ctx.numOperands(Root);
  const size_t Words = llvm::APInt::getNumWords(Width);
  const size_t PerFactor = sizeof(Factor) + 3 * sizeof(SymRef);
  const size_t ReadWork = 2 + Words;
  if (N > Storage / PerFactor || N > Budget.available() / ReadWork)
    return Root;
  Storage -= N * PerFactor;
  const size_t FirstGenerated = Ctx.numNodes();
  llvm::SmallVector<Factor, 16> Rows;
  llvm::SmallVector<SymRef, 16> Parts;
  unsigned TotalZeros = 0;
  const auto Outer =
      Ctx.isConst(Ctx.operand(Root, 0)) ? Ctx.operand(Root, 0) : SymRef{};
  for (size_t I = bool(Outer); I < N; ++I) {
    if (!Budget.consume(ReadWork))
      return Root;
    const auto F = Ctx.operand(Root, I);
    if (Ctx.op(F) != SymOp::Add || Ctx.numOperands(F) != 2 ||
        !Ctx.isConst(Ctx.operand(F, 0))) {
      Parts.push_back(F);
      continue;
    }
    const auto D = Ctx.operand(F, 1);
    if (Ctx.op(D) != SymOp::Mul || !Ctx.isConst(Ctx.operand(D, 0))) {
      Parts.push_back(F);
      continue;
    }
    const auto Weight = Ctx.operand(D, 0);
    const unsigned Zeros = Ctx.constValue(Weight).countr_zero();
    const auto Offset = Ctx.operand(F, 0);
    const unsigned Content =
        std::min(Zeros, Ctx.constValue(Offset).countr_zero());
    TotalZeros += std::min(Observed - TotalZeros, Content);
    if (TotalZeros == Observed)
      return Ctx.mkZero(Width);
    const auto Tail = Ctx.operands(D).drop_front();
    if (Tail.size() > Storage / (3 * sizeof(SymRef)) ||
        !Budget.consume(Tail.size()))
      return Root;
    Storage -= Tail.size() * 3 * sizeof(SymRef);
    const auto Word = Tail.size() == 1 ? Tail.front() : Ctx.mkMul(Tail);
    const bool Complement = Ctx.op(Word) == SymOp::Not;
    Rows.push_back({F, Complement ? Ctx.operand(Word, 0) : Word, Weight, Offset,
                    Zeros, Content, Complement});
  }
  // A zero product needs only the prefix that proved it. Charge the sort
  // after that proof opportunity and only for the factors that will be sorted.
  const size_t SortWork = 2 + std::bit_width(Rows.size());
  if (Rows.size() > Budget.available() / SortWork)
    return Root;
  Budget.consume(Rows.size() * SortWork);
  llvm::sort(Rows, [](const Factor &A, const Factor &B) {
    return A.Word != B.Word ? A.Word < B.Word : A.Zeros > B.Zeros;
  });

  // The existing linear path already handles predominantly high products.
  // A repeated middle word is relevant when the product of its middle
  // perturbations vanishes, even though some pairwise interactions survive.
  bool TruncatesMiddle = false;
  for (size_t I = 0; I < Rows.size();) {
    unsigned Bits = 0;
    size_t End = I;
    for (; End < Rows.size() && Rows[End].Word == Rows[I].Word; ++End)
      if (Rows[End].Zeros < Observed / 2 + Observed % 2)
        Bits += std::min(Observed - Bits, Rows[End].Zeros);
    TruncatesMiddle |= Bits == Observed;
    I = End;
  }
  if (!TruncatesMiddle)
    return Root;

  bool Changed = false;
  for (size_t I = 0; I < Rows.size();) {
    size_t End = I + 1;
    while (End < Rows.size() && Rows[End].Word == Rows[I].Word)
      ++End;
    unsigned OwnZeros = 0;
    for (size_t J = I; J < End; ++J)
      OwnZeros += Rows[J].Content;
    // Other factors annihilate the high bits of this polynomial. Its own
    // shared zeros survive reduction because the complete product is nonzero.
    const unsigned Precision = Observed - (TotalZeros - OwnZeros);
    // The d smallest valuations bound every coefficient of degree d.
    size_t Degree = 0;
    unsigned Bits = 0;
    for (size_t J = End; J > I; --J) {
      if (Rows[J - 1].Zeros >= Precision - Bits)
        break;
      Bits += Rows[J - 1].Zeros;
      ++Degree;
    }
    size_t Used = End - I;
    const size_t Bytes =
        sizeof(llvm::APInt) + 4 * sizeof(SymRef) + 5 * Words * sizeof(uint64_t);
    const size_t Step = 3 + Words + 2 * Words * Words;
    if (Degree >= Used || Used < 2 || (Degree + 1) > Storage / Bytes ||
        (Degree + 1) > Budget.available() / Step / Used) {
      Used = 0;
    } else {
      Budget.consume(Used * (Degree + 1) * Step);
      Storage -= (Degree + 1) * Bytes;
      llvm::SmallVector<llvm::APInt, 8> Coefficients(Degree + 1,
                                                     llvm::APInt(Precision, 0));
      Coefficients[0] = 1;
      for (size_t J = 0; J < Used; ++J) {
        const auto &F = Rows[I + J];
        auto A = Ctx.constValue(F.Offset).zextOrTrunc(Precision);
        auto B = Ctx.constValue(F.Weight).zextOrTrunc(Precision);
        if (F.Complement) {
          A -= B;
          B.negate();
        }
        for (size_t K = std::min(Degree, J + 1); K != 0; --K)
          Coefficients[K] = A * Coefficients[K] + B * Coefficients[K - 1];
        Coefficients[0] *= A;
      }
      const auto V =
          emitTruncatedPolynomial(Ctx, Rows[I].Word, Changed ? SymRef{} : Outer,
                                  Coefficients, Bytes, Budget, Storage);
      Parts.push_back(V);
      Changed = true;
    }
    for (size_t J = I + Used; J < End; ++J)
      Parts.push_back(Rows[J].Ref);
    I = End;
  }
  if (!Changed)
    return Root;
  const auto Candidate = Ctx.mkMul(Parts);
  if (!(readingScore(Ctx, Candidate) < readingScore(Ctx, Root)))
    return Root;
  recordResidueReading(Ctx, FirstGenerated, Budget, Storage);
  if (EmittedReading)
    *EmittedReading = true;
  return Candidate;
}

} // namespace neverd::symbolic::detail
