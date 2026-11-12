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
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace neverd::symbolic::detail {

namespace {

struct Factor {
  SymRef Ref, Word, Weight, Offset;
  unsigned Zeros, Content;
  bool Complement;
};

bool readTruncatedCoordinate(SymContext &Ctx, Factor &Row, unsigned Observed,
                             WorkBudget &Budget, size_t &Storage) {
  const unsigned Width = Ctx.width(Row.Word);
  const size_t Words = llvm::APInt::getNumWords(Width);
  auto &Word = Row.Word;
  auto &Weight = Row.Weight;
  auto &Offset = Row.Offset;
  // Read nested affine coordinates without distributing the product. Only
  // the observed bits can survive multiplication by the outer coefficient.
  auto HasScalar = [&](SymRef V) {
    return Ctx.numOperands(V) == 2 && Ctx.isConst(Ctx.operand(V, 0));
  };
  auto Scaled = [&](SymRef V) {
    return (Ctx.op(V) == SymOp::Mul || Ctx.op(V) == SymOp::And) &&
           Ctx.numOperands(V) >= 2 && Ctx.isConst(Ctx.operand(V, 0));
  };
  auto CanRead = [&] {
    const auto Op = Ctx.op(Word);
    if (HasScalar(Word) && (Op == SymOp::Add || Op == SymOp::Mul))
      return true;
    if (Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor)
      return Ctx.isConst(Ctx.operand(Word, 0));
    if (Op != SymOp::Add)
      return false;
    auto Terms = Ctx.operands(Word);
    if (Terms.size() == 2 && !Scaled(Terms[0]) && !Scaled(Terms[1]))
      return false;
    if (Terms.size() > Budget.available() / (1 + Words))
      return false;
    Budget.consume(Terms.size() * (1 + Words));
    return llvm::any_of(Terms, [&](SymRef T) {
      return Scaled(T) && !Ctx.constValue(Ctx.operand(T, 0))[0];
    });
  };
  if (CanRead()) {
    const size_t ReadBytes =
        2 * (sizeof(llvm::APInt) + Words * sizeof(uint64_t));
    if (ReadBytes > Storage || !Budget.consume(2 + 2 * Words))
      return false;
    Storage -= ReadBytes;
    const size_t ReadWords = llvm::APInt::getNumWords(Observed);
    auto A = Ctx.constValue(Offset).trunc(Observed);
    auto B = Ctx.constValue(Weight).trunc(Observed);
    do {
      const auto Op = Ctx.op(Word);
      if (HasScalar(Word) && (Op == SymOp::Add || Op == SymOp::Mul)) {
        if (!Budget.consume(3 + ReadWords + ReadWords * ReadWords))
          return false;
        const auto K = Ctx.constValue(Ctx.operand(Word, 0)).trunc(Observed);
        if (Op == SymOp::Add)
          A += B * K;
        else
          B *= K;
        Word = Ctx.operand(Word, 1);
        continue;
      }
      // B annihilates every bit above this prefix. A literal XOR of
      // all ones in the prefix is -1 - word there.
      const unsigned Bits = Observed - B.countr_zero();
      if (!Budget.consume(2 + 2 * Words))
        return false;
      if (Op != SymOp::Add) {
        const auto K = Ctx.constValue(Ctx.operand(Word, 0));
        const bool Flip = Op == SymOp::Xor && K.countr_one() >= Bits;
        const bool Identity =
            Op == SymOp::And ? K.countr_one() >= Bits : K.countr_zero() >= Bits;
        if (!Identity && !Flip)
          break;
        const auto Tail = Ctx.operands(Word).drop_front();
        if (Tail.size() > Storage / (2 * sizeof(SymRef)) ||
            !Budget.consume(Tail.size()))
          return false;
        Storage -= Tail.size() * 2 * sizeof(SymRef);
        if (Flip) {
          A -= B;
          B.negate();
        }
        Word = Op == SymOp::And  ? Ctx.mkAnd(Tail)
               : Op == SymOp::Or ? Ctx.mkOr(Tail)
                                 : Ctx.mkXor(Tail);
        continue;
      }
      const auto Terms = Ctx.operands(Word);
      if (Terms.size() > Storage / (2 * sizeof(SymRef)) ||
          !Budget.consume(Terms.size() * (2 + Words)))
        return false;
      Storage -= Terms.size() * 2 * sizeof(SymRef);
      llvm::SmallVector<SymRef, 8> Live;
      for (auto Term : Terms) {
        if (Ctx.isConst(Term)) {
          if (!Budget.consume(ReadWords * ReadWords))
            return false;
          A += B * Ctx.constValue(Term).trunc(Observed);
        } else if (!Scaled(Term) ||
                   Ctx.constValue(Ctx.operand(Term, 0)).countr_zero() < Bits) {
          Live.push_back(Term);
        }
      }
      if (Live.size() == Terms.size())
        break;
      Word = Live.empty() ? Ctx.mkZero(Width) : Ctx.mkAdd(Live);
    } while (CanRead());
    Offset = Ctx.mkConst(A.sextOrTrunc(Width));
    Weight = Ctx.mkConst(B.sextOrTrunc(Width));
    Row.Zeros = Ctx.constValue(Weight).countr_zero();
    Row.Content = std::min(Row.Zeros, Ctx.constValue(Offset).countr_zero());
  }
  Row.Complement = Ctx.op(Word) == SymOp::Not;
  if (Row.Complement)
    Word = Ctx.operand(Word, 0);
  return true;
}

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

SymRef buildTruncatedGroup(SymContext &Ctx, llvm::ArrayRef<Factor> Group,
                           unsigned Precision, SymRef Scale, WorkBudget &Budget,
                           size_t &Storage) {
  // The d smallest valuations bound every coefficient of degree d.
  size_t Degree = 0;
  unsigned Bits = 0;
  for (size_t J = Group.size(); J != 0; --J) {
    if (Group[J - 1].Zeros >= Precision - Bits)
      break;
    Bits += Group[J - 1].Zeros;
    ++Degree;
  }
  const size_t Used = Group.size();
  const size_t Words = llvm::APInt::getNumWords(Ctx.width(Group.front().Word));
  const size_t Bytes =
      sizeof(llvm::APInt) + 4 * sizeof(SymRef) + 5 * Words * sizeof(uint64_t);
  if (Degree >= Used || Used < 2 || (Degree + 1) > Storage / Bytes)
    return {};
  const size_t W = llvm::APInt::getNumWords(Precision);
  const size_t Step = 3 + W + 2 * W * W;
  size_t Work = llvm::SaturatingMultiply(
      Used, llvm::SaturatingMultiply(Degree + 1, Step));
  unsigned Shift = 0;
  const unsigned MinimumZeros = Group.back().Zeros;
  // Every coefficient of x^k contains 2^(k*s) when every weight contains
  // 2^s. Keep its quotient at P-k*s bits if that saves word arithmetic.
  if (W > 1 && Degree && MinimumZeros &&
      llvm::APInt::getNumWords(Precision - Degree * MinimumZeros) < W) {
    size_t PerFactor = 2 + 3 * W + W * W;
    for (size_t K = 1; K <= Degree; ++K) {
      const size_t KW = llvm::APInt::getNumWords(Precision - K * MinimumZeros);
      PerFactor = llvm::SaturatingAdd(PerFactor, 3 + 4 * KW + 2 * KW * KW);
    }
    const size_t Projected = llvm::SaturatingMultiplyAdd(
        Used, PerFactor, llvm::SaturatingMultiply(Degree, 2 + 2 * W));
    if (Projected < Work) {
      Work = Projected;
      Shift = MinimumZeros;
    }
  }
  if (!Budget.canConsume(Work))
    return {};
  Budget.consume(Work);
  Storage -= (Degree + 1) * Bytes;
  llvm::SmallVector<llvm::APInt, 8> Coefficients;
  Coefficients.reserve(Degree + 1);
  for (size_t K = 0; K <= Degree; ++K)
    Coefficients.emplace_back(Precision - K * Shift, 0);
  Coefficients[0] = 1;
  for (size_t J = 0; J < Used; ++J) {
    const auto &F = Group[J];
    auto A = Ctx.constValue(F.Offset).zextOrTrunc(Precision);
    auto B = Ctx.constValue(F.Weight).zextOrTrunc(Precision);
    if (F.Complement) {
      A -= B;
      B.negate();
    }
    if (Shift) {
      B.lshrInPlace(Shift);
      for (size_t K = std::min(Degree, J + 1); K != 0; --K) {
        const auto P = Coefficients[K].getBitWidth();
        Coefficients[K] = A.trunc(P) * Coefficients[K] +
                          B.trunc(P) * Coefficients[K - 1].trunc(P);
      }
    } else {
      for (size_t K = std::min(Degree, J + 1); K != 0; --K)
        Coefficients[K] = A * Coefficients[K] + B * Coefficients[K - 1];
    }
    Coefficients[0] *= A;
  }
  if (Shift)
    for (size_t K = 1; K <= Degree; ++K)
      Coefficients[K] = Coefficients[K].sext(Precision).shl(K * Shift);
  return emitTruncatedPolynomial(Ctx, Group.front().Word, Scale, Coefficients,
                                 Bytes, Budget, Storage);
}

} // namespace

SymRef foldTruncatedProduct(SymContext &Ctx, SymRef Root, unsigned Observed,
                            WorkBudget &Budget, size_t &Storage,
                            bool *EmittedReading) {
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
    auto Weight = Ctx.operand(D, 0);
    auto Offset = Ctx.operand(F, 0);
    const unsigned Zeros = Ctx.constValue(Weight).countr_zero();
    const unsigned InitialContent =
        std::min(Zeros, Ctx.constValue(Offset).countr_zero());
    TotalZeros += std::min(Observed - TotalZeros, InitialContent);
    if (TotalZeros == Observed)
      return Ctx.mkZero(Width);
    const auto Tail = Ctx.operands(D).drop_front();
    if (Tail.size() > Storage / (3 * sizeof(SymRef)) ||
        !Budget.consume(Tail.size()))
      return Root;
    Storage -= Tail.size() * 3 * sizeof(SymRef);
    auto Word = Tail.size() == 1 ? Tail.front() : Ctx.mkMul(Tail);
    Rows.push_back({F, Word, Weight, Offset, Zeros, InitialContent, false});
  }
  // Admit the complete product's content before reading coordinates.
  // Once that prefix proves zero, none of their alternate spellings matter.
  for (auto &Row : Rows) {
    const unsigned InitialContent = Row.Content;
    // Other factors annihilate high bits here too: instruction combining
    // can place the outer scale inside one offset instead of on the product.
    const unsigned LiveBits = Observed - (TotalZeros - InitialContent);
    if (!readTruncatedCoordinate(Ctx, Row, LiveBits, Budget, Storage))
      return Root;
    TotalZeros += std::min(Observed - TotalZeros, Row.Content - InitialContent);
    if (TotalZeros == Observed)
      return Ctx.mkZero(Width);
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
    const auto Group = llvm::ArrayRef(Rows).slice(I, End - I);
    if (const auto V =
            buildTruncatedGroup(Ctx, Group, Precision,
                                Changed ? SymRef{} : Outer, Budget, Storage)) {
      Parts.push_back(V);
      Changed = true;
    } else {
      for (const auto &F : Group)
        Parts.push_back(F.Ref);
    }
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
