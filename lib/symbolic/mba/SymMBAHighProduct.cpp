//===- SymMBAHighProduct.cpp - Products of high-word disturbances --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reduce products of offset words modulo their observed width. Low factors
/// remain intact when their interactions can reach the output.
///
//===----------------------------------------------------------------------===//

#include "Limits.h"
#include "SymMBADetail.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace neverd::symbolic::detail {
namespace {

// Group equal and opposite modular weights without expanding arithmetic
// words. Reversing every sign in a group is also valid, so use subtraction
// inside the sum instead of introducing a leading negation.
SymRef groupResidueWeights(SymContext &Ctx, SymRef Sum, WorkBudget &Budget,
                           size_t &Storage) {
  if (Ctx.op(Sum) != SymOp::Add)
    return Sum;
  const size_t N = Ctx.numOperands(Sum);
  if (N < 3)
    return Sum;
  const size_t Words = llvm::APInt::getNumWords(Ctx.width(Sum));
  struct Row {
    SymRef Weight, Term;
    bool Negative;
  };
  const size_t Bytes = 2 * sizeof(Row) + 6 * sizeof(SymRef) +
                       sizeof(llvm::APInt) + Words * sizeof(uint64_t);
  const size_t Work = 3 + 2 * Words + std::bit_width(N);
  if (N > Storage / Bytes || N > Budget.available() / Work)
    return Sum;
  Budget.consume(N * Work);
  Storage -= N * Bytes;
  llvm::SmallVector<Row, 8> Rows;
  llvm::SmallVector<SymRef, 8> Parts;
  for (size_t I = 0; I < N; ++I) {
    const auto T = Ctx.operand(Sum, I);
    if (Ctx.op(T) != SymOp::Mul || !Ctx.isConst(Ctx.operand(T, 0))) {
      Parts.push_back(T);
      continue;
    }
    const auto W = Ctx.operand(T, 0);
    const auto K = Ctx.constValue(W);
    const bool Negative = K.isNegative() && !K.isMinSignedValue();
    Rows.push_back({Negative ? Ctx.mkConst(-K) : W, T, Negative});
  }
  llvm::sort(Rows,
             [](const Row &A, const Row &B) { return A.Weight < B.Weight; });
  bool Changed = false;
  for (size_t I = 0; I < Rows.size();) {
    size_t J = I + 1;
    while (J < Rows.size() && Rows[J].Weight == Rows[I].Weight)
      ++J;
    if (J - I == 1) {
      Parts.push_back(Rows[I++].Term);
      continue;
    }
    size_t Edges = 0;
    for (size_t K = I; K < J; ++K) {
      const size_t Count = Ctx.numOperands(Rows[K].Term);
      if (Count > Storage / (3 * sizeof(SymRef)) - Edges)
        return Sum;
      Edges += Count;
    }
    if (!Budget.consume(Edges))
      return Sum;
    Storage -= Edges * 3 * sizeof(SymRef);
    const bool Reverse = llvm::all_of(llvm::ArrayRef(Rows).slice(I, J - I),
                                      [](const Row &R) { return R.Negative; });
    llvm::SmallVector<SymRef, 8> Bodies;
    for (size_t K = I; K < J; ++K) {
      const auto Tail = Ctx.operands(Rows[K].Term).drop_front();
      const auto V = Tail.size() == 1 ? Tail.front() : Ctx.mkMul(Tail);
      Bodies.push_back(Rows[K].Negative != Reverse ? Ctx.mkNeg(V) : V);
    }
    const auto W = Rows[I].Weight;
    Parts.push_back(Ctx.mkMul(Reverse ? Ctx.mkNeg(W) : W, Ctx.mkAdd(Bodies)));
    Changed = true;
    I = J;
  }
  if (!Changed)
    return Sum;
  auto Candidate = Ctx.mkAdd(Parts);
  return readingScore(Ctx, Candidate) < readingScore(Ctx, Sum) ? Candidate
                                                               : Sum;
}

// Factor the common low zeros of a weighted sum and normalize a surviving
// odd unit at its observed width. The omitted quotient bits are annihilated
// by the outer coefficient, including for negative full-width weights.
SymRef normalizeResidueScale(SymContext &Ctx, SymRef Sum, WorkBudget &Budget,
                             size_t &Storage) {
  if (Ctx.op(Sum) != SymOp::Add)
    return Sum;
  const unsigned Width = Ctx.width(Sum);
  const size_t N = Ctx.numOperands(Sum),
               Words = llvm::APInt::getNumWords(Width);
  const size_t Work = 4 * Words + 2 * Words * Words;
  const size_t BytesPerTerm = 8 * sizeof(SymRef) + 4 * Words * sizeof(uint64_t);
  if (N > Budget.available() / Work || N > Storage / BytesPerTerm)
    return Sum;
  Budget.consume(N * (1 + 3 * Words));
  const size_t Bytes = N * BytesPerTerm;
  const size_t MaxEdges = (Storage - Bytes) / (3 * sizeof(SymRef));
  unsigned Zeros = Width;
  SymRef Pivot, Scalar;
  size_t Edges = 0;
  for (auto T : Ctx.operands(Sum)) {
    if (Ctx.isConst(T)) {
      Scalar = T;
      continue;
    }
    if (Ctx.op(T) != SymOp::Mul || !Ctx.isConst(Ctx.operand(T, 0)))
      return Sum;
    const auto C = Ctx.operand(T, 0);
    const auto Weight = Ctx.constValue(C);
    const unsigned Shift = Weight.countr_zero();
    if (Shift < Zeros || (Shift == Zeros && Pivot &&
                          Weight.abs().ult(Ctx.constValue(Pivot).abs()))) {
      Zeros = Shift;
      Pivot = C;
    }
    const size_t Count = Ctx.numOperands(T);
    if (Count > MaxEdges - Edges)
      return Sum;
    Edges += Count;
  }
  // A literal offset can also share the only variable term's weight,
  // even when it observes more than half the result.
  const bool ScaledOffset =
      Scalar && N == 2 && Ctx.constValue(Scalar).countr_zero() >= Zeros;
  if (!Pivot || !Zeros || (!ScaledOffset && Zeros < Width / 2) ||
      Width - Zeros < 3)
    return Sum;
  const size_t InverseWork = Words * Words * std::bit_width(Width - Zeros) * 2;
  if (!Budget.canConsume(InverseWork + N * Work + Edges))
    return Sum;
  Budget.consume(InverseWork + N * Work + Edges);
  Storage -= Bytes + Edges * 3 * sizeof(SymRef);
  const auto Scale = Ctx.constValue(Pivot);
  const auto InverseUnit =
      Scale.extractBits(Width - Zeros, Zeros).multiplicativeInverse();
  llvm::SmallVector<SymRef, 8> Inner;
  auto Quotient = [&](const llvm::APInt &C) {
    return (C.extractBits(Width - Zeros, Zeros) * InverseUnit).sext(Width);
  };
  size_t FlatLiteralBits = 0, FactoredLiteralBits = Scale.getSignificantBits();
  for (size_t I = Scalar ? 1 : 0; I < N; ++I) {
    const auto T = Ctx.operand(Sum, I);
    const auto C = Ctx.constValue(Ctx.operand(T, 0));
    const auto Tail = Ctx.operands(T).drop_front();
    const auto Body = Tail.size() == 1 ? Tail.front() : Ctx.mkMul(Tail);
    const auto Q = Quotient(C);
    FlatLiteralBits += C.getSignificantBits();
    FactoredLiteralBits += Q.getSignificantBits();
    Inner.push_back(Ctx.mkMul(Ctx.mkConst(Q), Body));
  }
  bool InnerScalar = Scalar && Ctx.constValue(Scalar).countr_zero() >= Zeros;
  if (InnerScalar) {
    const auto C = Ctx.constValue(Scalar), Q = Quotient(C);
    FlatLiteralBits += C.getSignificantBits();
    FactoredLiteralBits += Q.getSignificantBits();
    Inner.push_back(Ctx.mkConst(Q));
  }
  auto Result = Ctx.mkMul(Pivot, Ctx.mkAdd(Inner));
  if (Scalar && !InnerScalar)
    Result = Ctx.mkAdd(Scalar, Result);
  const auto Candidate = readingScore(Ctx, Result),
             Original = readingScore(Ctx, Sum);
  // Pay for the new parentheses on a cost tie by saving at least two hex
  // digits.
  return Candidate < Original || (Candidate == Original &&
                                  FlatLiteralBits > FactoredLiteralBits + 2 * 4)
             ? Result
             : Sum;
}

// Only the low bit reaches the enclosing top-bit product. Odd literals act
// as Boolean true in this projection; opaque inputs retain their own low bit.
SymRef parityWord(SymContext &Ctx, SymRef V, WorkBudget &Budget,
                  size_t &Storage, unsigned Depth = 0) {
  if (!Budget.consume())
    return V;
  if (Ctx.isConst(V)) {
    if (!Budget.consume(llvm::APInt::getNumWords(Ctx.width(V))))
      return V;
    return Ctx.constValue(V)[0] ? Ctx.mkOnes(Ctx.width(V))
                                : Ctx.mkZero(Ctx.width(V));
  }
  const auto Op = Ctx.op(V);
  if ((Op != SymOp::Add && Op != SymOp::Mul) ||
      Depth >= limits::kMaxAffineResidueDepth)
    return V;
  const size_t N = Ctx.numOperands(V);
  if (N > Storage / (4 * sizeof(SymRef)) || !Budget.consume(N))
    return V;
  Storage -= N * 4 * sizeof(SymRef);
  llvm::SmallVector<SymRef, 8> Inputs(Ctx.operands(V));
  for (auto &I : Inputs)
    I = parityWord(Ctx, I, Budget, Storage, Depth + 1);
  return Op == SymOp::Add ? Ctx.mkXor(Inputs) : Ctx.mkAnd(Inputs);
}

// The canonical sum has already combined equal disturbances. Its surviving
// high coefficients can therefore observe fewer bits than any input factor.
SymRef factorHighSum(SymContext &Ctx, SymRef Sum, WorkBudget &Budget,
                     size_t &Storage) {
  const unsigned Width = Ctx.width(Sum);
  if (Width < 3 || Ctx.isConst(Sum))
    return Sum;
  llvm::ArrayRef<SymRef> Inputs = Ctx.op(Sum) == SymOp::Add
                                      ? Ctx.operands(Sum)
                                      : llvm::ArrayRef<SymRef>(Sum);
  const auto Literal = Ctx.isConst(Inputs.front()) ? Inputs.front() : SymRef{};
  if (Literal)
    Inputs = Inputs.drop_front();
  const size_t Words = llvm::APInt::getNumWords(Width);
  if (Inputs.empty() || Inputs.size() > Storage / (2 * sizeof(SymRef)) ||
      !Budget.canConsume(Inputs.size() * (1 + Words)))
    return Sum;
  Budget.consume(Inputs.size() * (1 + Words));
  unsigned Zeros = Width;
  size_t Edges = 0;
  for (auto T : Inputs) {
    if (Ctx.op(T) != SymOp::Mul || !Ctx.isConst(Ctx.operand(T, 0)))
      return Sum;
    Zeros = std::min(Zeros, Ctx.constValue(Ctx.operand(T, 0)).countr_zero());
    if (Zeros < Width - 2)
      return Sum;
    const size_t N = Ctx.numOperands(T);
    if (N > (Storage / (2 * sizeof(SymRef)) - Inputs.size()) - Edges)
      return Sum;
    Edges += N;
  }

  if (!Budget.canConsume(Edges))
    return Sum;
  Budget.consume(Edges);
  Storage -= (Inputs.size() + Edges) * 2 * sizeof(SymRef);
  llvm::SmallVector<SymRef, 16> Terms(Inputs);
  // At the lowest bit addition becomes XOR and multiplication becomes AND.
  // Keep larger operands opaque if their extra edges cannot be inspected.
  auto Parity = [&](SymRef T) {
    const auto Tail = Ctx.operands(T).drop_front();
    const auto Body = Tail.size() == 1 ? Tail.front() : Ctx.mkMul(Tail);
    return parityWord(Ctx, Body, Budget, Storage);
  };
  const auto Scale = Ctx.mkConst(llvm::APInt::getOneBitSet(Width, Zeros));
  auto Constant = Literal ? Ctx.constValue(Literal) : llvm::APInt(Width, 0);
  SymRef Body;
  if (Zeros == Width - 1) {
    for (auto &T : Terms)
      T = Parity(T);
    Body = Ctx.mkXor(Terms);
    if (Constant.isMinSignedValue()) {
      Body = Ctx.mkNot(Body);
      Constant.clearAllBits();
    }
  } else {
    // Modulo four the remaining nonzero weights are 1, -1 and 2.
    for (auto &T : Terms) {
      const auto K = Ctx.constValue(Ctx.operand(T, 0));
      if (K[Zeros + 1] && !K[Zeros]) {
        T = Ctx.mkMul(Ctx.mkConst(Width, 2), Parity(T));
      } else {
        const auto Tail = Ctx.operands(T).drop_front();
        const auto V = Tail.size() == 1 ? Tail.front() : Ctx.mkMul(Tail);
        T = K[Zeros + 1] ? Ctx.mkNeg(V) : V;
      }
    }
    Body = Ctx.mkAdd(Terms);
  }
  const auto Result = Ctx.mkAdd(Ctx.mkConst(Constant), Ctx.mkMul(Scale, Body));
  return readingScore(Ctx, Result) <= readingScore(Ctx, Sum) ? Result : Sum;
}

} // namespace

// In the expansion of a product of (C_i + D_i), terms containing two
// different disturbances vanish when their guaranteed zero bits span the
// observed width. Prefix and suffix products supply every surviving
// coefficient without division, including when some C_i are even.
SymRef foldHighProductTerms(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                            size_t &Storage, bool *EmittedReading) {
  auto Factors = Ctx.operands(Root);
  const unsigned Width = Ctx.width(Root);
  const auto Scale = Ctx.isConst(Factors.front()) ? Factors.front() : SymRef{};
  if (Scale)
    Factors = Factors.drop_front();
  const size_t Words = llvm::APInt::getNumWords(Width);
  if (Words > Storage / sizeof(uint64_t) || !Budget.canConsume(3 * Words))
    return Root;
  auto Disturbance = [&](SymRef F) {
    return Ctx.op(F) == SymOp::Add && Ctx.numOperands(F) == 2 &&
                   Ctx.isConst(Ctx.operand(F, 0))
               ? Ctx.operand(F, 1)
               : SymRef{};
  };
  auto ZeroMask = [&](SymRef D) {
    return D && (Ctx.op(D) == SymOp::Mul || Ctx.op(D) == SymOp::And) &&
                   Ctx.isConst(Ctx.operand(D, 0))
               ? Ctx.operand(D, 0)
               : SymRef{};
  };
  // Two unshifted end factors rule out linearizing the entire product.
  // A high subset in between can still be collected.
  auto HasLowZero = [&](SymRef F) {
    const auto Mask = ZeroMask(Disturbance(F));
    if (!Mask)
      return false;
    if (Width <= 64)
      return !(Ctx.node(Mask).Aux & 1);
    Budget.consume(Words);
    return !Ctx.constValue(Mask)[0];
  };
  const bool FullPossible =
      HasLowZero(Factors.back()) || HasLowZero(Factors.front());

  Budget.consume(Words);
  const unsigned Observed =
      Width - (Scale ? Ctx.constValue(Scale).countr_zero() : 0);
  unsigned First = Observed, Second = Observed;
  size_t Edges = 0;
  bool HasMaskedDisturbance = false;
  bool Mixed = !FullPossible;
  if (FullPossible) {
    for (auto F : Factors) {
      if (!Budget.canConsume(Words + 1))
        return Root;
      Budget.consume(Words + 1);
      const auto D = Disturbance(F);
      const auto Mask = ZeroMask(D);
      HasMaskedDisturbance |= Mask && Ctx.op(D) == SymOp::And;
      unsigned Zeros = Mask ? Ctx.constValue(Mask).countr_zero() : 0;
      if (Zeros < First)
        std::swap(First, Zeros);
      Second = std::min(Second, Zeros);
      if (!D || First < Observed - Second) {
        Mixed = true;
        break;
      }
      if (Ctx.numOperands(D) > Storage / (4 * sizeof(SymRef)) - Edges)
        return Root;
      Edges += Ctx.numOperands(D);
    }
  }
  llvm::SmallVector<SymRef, 16> High, Remainder;
  if (Mixed) {
    // Keep the low factors as an exact multiplier. Among disturbances with
    // at least ceil(Observed / 2) trailing zeros, every cross term vanishes.
    const unsigned Half = Observed / 2 + Observed % 2;
    auto IsHigh = [&](SymRef F) {
      const auto Mask = ZeroMask(Disturbance(F));
      return Mask && Ctx.constValue(Mask).countr_zero() >= Half;
    };
    // Check for a candidate before reserving both partitions. Unit-offset
    // families commonly have no scaled or masked high word anywhere.
    const size_t Count = Factors.size();
    if (Count > Budget.available() / (1 + Words))
      return Root;
    const auto FirstHigh = llvm::find_if(Factors, IsHigh);
    Budget.consume(
        (FirstHigh - Factors.begin() + unsigned(FirstHigh != Factors.end())) *
        (1 + Words));
    if (FirstHigh == Factors.end())
      return Root;
    const size_t Remaining = Factors.end() - FirstHigh;
    if (Count + 1 > Storage / (2 * sizeof(SymRef)) ||
        Remaining > Budget.available() / (Words + 2))
      return Root;
    Budget.consume(Remaining * (Words + 2));
    Storage -= (Count + 1) * 2 * sizeof(SymRef);
    High.reserve(Remaining);
    Remainder.reserve(Count + 1);
    Remainder.append(Factors.begin(), FirstHigh);
    Edges = 0;
    for (auto F : llvm::make_range(FirstHigh, Factors.end())) {
      if (!IsHigh(F)) {
        Remainder.push_back(F);
        continue;
      }
      const auto D = Disturbance(F);
      if (Ctx.numOperands(D) > Storage / (4 * sizeof(SymRef)) - Edges)
        return Root;
      Edges += Ctx.numOperands(D);
      High.push_back(F);
    }
    if (High.size() < 2)
      return Root;
    Factors = High;
  }

  const size_t Bytes =
      sizeof(llvm::APInt) + 2 * sizeof(SymRef) + 3 * Words * sizeof(uint64_t);
  const size_t EdgeBytes = Edges * (4 * sizeof(SymRef));
  const size_t FixedWork = 8 + std::bit_width(Factors.size());
  if (Factors.size() + 1 > (Storage - EdgeBytes) / Bytes ||
      Edges > Budget.available())
    return Root;
  const size_t Available = (Budget.available() - Edges) / Factors.size();
  if (Available < FixedWork || Words > (Available - FixedWork) / 5 / Words)
    return Root;
  Budget.consume(Edges + Factors.size() * (5 * Words * Words + FixedWork));
  Storage -= EdgeBytes + (Factors.size() + 1) * Bytes;

  llvm::SmallVector<llvm::APInt, 16> Prefix;
  Prefix.reserve(Factors.size() + 1);
  Prefix.push_back(Scale ? Ctx.constValue(Scale) : llvm::APInt(Width, 1));
  for (auto F : Factors)
    Prefix.push_back(Prefix.back() * Ctx.constValue(Ctx.operand(F, 0)));

  llvm::SmallVector<SymRef, 16> Terms;
  Terms.reserve(Factors.size() + 1);
  const size_t FirstGenerated = Ctx.numNodes();
  llvm::APInt Suffix(Width, 1), Constant = Prefix.back();
  for (size_t I = Factors.size(); I-- != 0;) {
    const auto F =
        Mixed ? High[I] : Ctx.operand(Root, I + unsigned(bool(Scale)));
    auto C = Prefix[I] * Suffix;
    Suffix *= Ctx.constValue(Ctx.operand(F, 0));
    if (C.isZero())
      continue;
    auto D = Ctx.operand(F, 1);
    if (Ctx.op(D) == SymOp::Mul && Ctx.numOperands(D) == 2 &&
        Ctx.isConst(Ctx.operand(D, 0))) {
      C *= Ctx.constValue(Ctx.operand(D, 0));
      D = Ctx.operand(D, 1);
    }
    if (C.isZero())
      continue;
    // C * ~V contributes -C to the constant and -C * V to the sum.
    if (Ctx.op(D) == SymOp::Not) {
      Constant -= C;
      C.negate();
      D = Ctx.operand(D, 0);
    }
    if (Ctx.op(D) == SymOp::And && Ctx.isConst(Ctx.operand(D, 0)) &&
        Ctx.constValue(Ctx.operand(D, 0)).countr_zero() >=
            Width - C.countr_zero())
      continue;
    Terms.push_back(Ctx.mkMul(Ctx.mkConst(C), D));
  }
  Terms.push_back(Ctx.mkConst(Constant));
  const auto Flat = Ctx.mkAdd(Terms);
  auto Result = factorHighSum(Ctx, Flat, Budget, Storage);
  const bool LargeAffine =
      Result == Flat && Ctx.op(Flat) == SymOp::Add &&
      Ctx.numOperands(Flat) >= limits::kMinResidueReadingTerms;
  // Leave a fully masked expansion flat so that its weights can still
  // combine with factors in adjacent summands.
  bool NormalizedScale = false;
  if (Result == Flat && (Mixed || !HasMaskedDisturbance)) {
    Result = groupResidueWeights(Ctx, Result, Budget, Storage);
    const auto Scaled = normalizeResidueScale(Ctx, Result, Budget, Storage);
    NormalizedScale = Scaled != Result;
    Result = Scaled;
  }
  if (Mixed) {
    Remainder.push_back(Result);
    Result = Ctx.mkMul(Remainder);
    if (Ctx.op(Result) == SymOp::Mul && Ctx.isConst(Ctx.operand(Result, 0)) &&
        Ctx.constValue(Ctx.operand(Result, 0)).isMinSignedValue() &&
        Ctx.numOperands(Result) <= Storage / (3 * sizeof(SymRef)) &&
        Budget.canConsume(Ctx.numOperands(Result))) {
      Storage -= Ctx.numOperands(Result) * 3 * sizeof(SymRef);
      Budget.consume(Ctx.numOperands(Result));
      const auto W = Ctx.operand(Result, 0);
      llvm::SmallVector<SymRef, 8> Inputs(Ctx.operands(Result).drop_front());
      for (auto &Input : Inputs)
        Input = parityWord(Ctx, Input, Budget, Storage);
      const auto P = Ctx.mkMul(W, Ctx.mkAnd(Inputs));
      if (readingScore(Ctx, P) < readingScore(Ctx, Result))
        Result = P;
    }
  }
  const auto CandidateCost = readingScore(Ctx, Result);
  const auto InputCost = readingScore(Ctx, Root);
  // Inside a sum, an equally sized linear reading can combine with its
  // neighbours. A standalone product still requires a strict improvement.
  if (CandidateCost > InputCost ||
      (CandidateCost == InputCost && !(HasMaskedDisturbance && EmittedReading)))
    return Root;
  // Re-expanding a factored product or broad affine emission introduces
  // independent polynomial atoms. Keep its proved nodes, while original
  // carriers and smaller sums remain eligible for bottom-up rewrites.
  if (HasMaskedDisturbance || Mixed || LargeAffine || NormalizedScale) {
    recordResidueReading(Ctx, FirstGenerated, Budget, Storage);
    if (EmittedReading)
      *EmittedReading = true;
  }
  return Result;
}

// Adding H * (A xor B) is equivalent to adding H*A + H*B when H
// is the top bit. Absorb the parity of a shared unscaled sum into its
// coefficient only when the remaining Boolean expression becomes cheaper.
SymRef foldResidueParitySum(SymContext &Ctx, SymRef Root,
                            const MBAOptions &Opts, WorkBudget &Budget,
                            size_t Storage, SolveReport &Rep) {
  if (Ctx.op(Root) != SymOp::Add || Budget.exhausted())
    return Root;
  const size_t N = Ctx.numOperands(Root);
  if (N > limits::kMaxResidueBaseTerms || N < 2)
    return Root;
  SymRef Scale, Word, Literal;
  llvm::SmallVector<SymRef, limits::kMaxResidueBaseTerms + 1> Leaves;
  for (auto T : Ctx.operands(Root)) {
    if (Ctx.isConst(T))
      Literal = T;
    else if (Ctx.isVar(T))
      Leaves.push_back(T);
    else if (!Scale && Ctx.op(T) == SymOp::Mul && Ctx.numOperands(T) == 2 &&
             Ctx.isConst(Ctx.operand(T, 0)) &&
             Ctx.op(Ctx.operand(T, 1)) == SymOp::Xor) {
      Scale = Ctx.operand(T, 0);
      Word = Ctx.operand(T, 1);
    } else
      return Root;
  }
  if (!Scale || Leaves.empty() ||
      !Budget.consume(llvm::APInt::getNumWords(Ctx.width(Root))) ||
      !Ctx.constValue(Scale).isMinSignedValue())
    return Root;
  const size_t Edges = Ctx.numOperands(Word);
  const size_t Temporary = (N + Edges) * 4 * sizeof(SymRef);
  if (Edges > limits::kMaxResidueBaseTerms || Temporary > Storage ||
      !Budget.canConsume(N + Edges * Leaves.size()))
    return Root;
  Budget.consume(N + Edges * Leaves.size());
  if (llvm::none_of(Leaves, [&](SymRef V) {
        return llvm::is_contained(Ctx.operands(Word), V);
      }))
    return Root;
  const auto Sum = Ctx.mkAdd(Leaves);
  Leaves.push_back(Word);
  const auto Difference = Ctx.mkXor(Leaves);
  MBAOptions Inner = Opts;
  Inner.MaxTableBytes = Storage - Temporary;
  const auto Kernel = solveOneRegion(Ctx, Difference, Inner, Budget, Rep);
  if (Rep.Evidence == MBAEvidence::Samples)
    return Root;
  const auto Adjusted = Ctx.mkConst(Ctx.constValue(Scale) + 1);
  auto Candidate =
      Ctx.mkAdd(Ctx.mkMul(Adjusted, Sum), Ctx.mkMul(Scale, Kernel));
  if (Literal)
    Candidate = Ctx.mkAdd(Literal, Candidate);
  return readingScore(Ctx, Candidate) < readingScore(Ctx, Root) ? Candidate
                                                                : Root;
}

} // namespace neverd::symbolic::detail
