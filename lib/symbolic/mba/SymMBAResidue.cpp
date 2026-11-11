//===- SymMBAResidue.cpp - Modular product residue proofs ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Prove that a product vanishes modulo its word width without expanding it.
/// A factor B+c contributes one trailing zero for each matching low-bit prefix
/// of -B and c. Partition all offsets by these prefixes, and take the minimum
/// count along every residue path. This covers every word value of B, including
/// opaque nonlinear bases and correlated bases in different groups. Scaled
/// affine factors use the same bound and may synthesize a smaller nonzero
/// result from their complete observed residue table.
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
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::symbolic::detail {

namespace {

// This bounded, allocation-free hint grants no equality. Most MBA products
// contain bitwise factors and never pay for the optional residue proof.
bool hasOffsetFactors(const SymContext &Ctx, SymRef Root) {
  if (Ctx.op(Root) != SymOp::Mul ||
      Ctx.numOperands(Root) < limits::kMinResidueProductFactors ||
      Ctx.numOperands(Root) > limits::kMaxResidueProductFactors)
    return false;
  unsigned Offsets = 0;
  for (auto Factor : Ctx.operands(Root))
    if (Ctx.op(Factor) == SymOp::Add && Ctx.isConst(Ctx.operand(Factor, 0)) &&
        ++Offsets == limits::kMinResidueShiftedFactors)
      return true;
  return false;
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
    if (Tail.size() != 1)
      return Ctx.mkAnd(Tail);
    const auto V = Tail.front();
    const auto Op = Ctx.op(V);
    if (Op != SymOp::Add && Op != SymOp::Mul)
      return V;
    const size_t Edges = Ctx.numOperands(V);
    if (Edges > Storage / (2 * sizeof(SymRef)) || !Budget.canConsume(Edges))
      return V;
    Storage -= Edges * 2 * sizeof(SymRef);
    Budget.consume(Edges);
    return Op == SymOp::Add ? Ctx.mkXor(Ctx.operands(V))
                            : Ctx.mkAnd(Ctx.operands(V));
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

// In the expansion of a product of (C_i + D_i), terms containing two
// different disturbances vanish when their guaranteed zero bits span the
// observed width. Prefix and suffix products supply every surviving
// coefficient without division, including when some C_i are even.
SymRef foldHighProductTerms(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                            size_t &Storage, bool *EmittedMaskedSum) {
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
  // With no guaranteed low zero in either end factor, the two weakest
  // bounds cannot cover any observed bit. Leave odd affine maps to their
  // reader.
  auto HasLowZero = [&](SymRef F) {
    const auto Mask = ZeroMask(Disturbance(F));
    if (!Mask)
      return false;
    if (Width <= 64)
      return !(Ctx.node(Mask).Aux & 1);
    Budget.consume(Words);
    return !Ctx.constValue(Mask)[0];
  };
  if (!HasLowZero(Factors.back()) && !HasLowZero(Factors.front()))
    return Root;

  Budget.consume(Words);
  const unsigned Observed =
      Width - (Scale ? Ctx.constValue(Scale).countr_zero() : 0);
  unsigned First = Observed, Second = Observed;
  size_t Edges = 0;
  bool HasMaskedDisturbance = false;
  for (auto F : Factors) {
    if (!Budget.canConsume(Words + 1))
      return Root;
    Budget.consume(Words + 1);
    const auto D = Disturbance(F);
    if (!D)
      return Root;
    const auto Mask = ZeroMask(D);
    HasMaskedDisturbance |= Mask && Ctx.op(D) == SymOp::And;
    unsigned Zeros = Mask ? Ctx.constValue(Mask).countr_zero() : 0;
    if (Zeros < First)
      std::swap(First, Zeros);
    Second = std::min(Second, Zeros);
    if (First < Observed - Second)
      return Root;
    // Reweighting a flattened product may copy every operand. Bound that
    // scratch even when many occurrences refer to the same large term.
    if (Ctx.numOperands(D) > Storage / (4 * sizeof(SymRef)) - Edges)
      return Root;
    Edges += Ctx.numOperands(D);
  }

  const size_t Bytes =
      sizeof(llvm::APInt) + 2 * sizeof(SymRef) + 3 * Words * sizeof(uint64_t);
  const size_t EdgeBytes = Edges * (4 * sizeof(SymRef));
  const size_t FixedWork = 6 + std::bit_width(Factors.size());
  if (Factors.size() + 1 > (Storage - EdgeBytes) / Bytes ||
      Edges > Budget.available())
    return Root;
  const size_t Available = (Budget.available() - Edges) / Factors.size();
  if (Available < FixedWork || Words > (Available - FixedWork) / 4 / Words)
    return Root;
  Budget.consume(Edges + Factors.size() * (4 * Words * Words + FixedWork));
  Storage -= EdgeBytes + (Factors.size() + 1) * Bytes;

  llvm::SmallVector<llvm::APInt, 16> Prefix;
  Prefix.reserve(Factors.size() + 1);
  Prefix.push_back(Scale ? Ctx.constValue(Scale) : llvm::APInt(Width, 1));
  for (auto F : Factors)
    Prefix.push_back(Prefix.back() * Ctx.constValue(Ctx.operand(F, 0)));

  llvm::SmallVector<SymRef, 16> Terms;
  Terms.reserve(Factors.size() + 1);
  const size_t FirstGenerated = Ctx.numNodes();
  llvm::APInt Suffix(Width, 1);
  for (size_t I = Factors.size(); I-- != 0;) {
    // Interning a term may grow the operand pool. Reacquire the next factor.
    const auto F = Ctx.operand(Root, I + unsigned(bool(Scale)));
    const auto C = Prefix[I] * Suffix;
    if (!C.isZero())
      Terms.push_back(Ctx.mkMul(Ctx.mkConst(C), Ctx.operand(F, 1)));
    Suffix *= Ctx.constValue(Ctx.operand(F, 0));
  }
  Terms.push_back(Ctx.mkConst(Prefix.back()));
  const auto Result = factorHighSum(Ctx, Ctx.mkAdd(Terms), Budget, Storage);
  const auto CandidateCost = readingScore(Ctx, Result);
  const auto InputCost = readingScore(Ctx, Root);
  // Inside a sum, an equally sized linear reading can combine with its
  // neighbours. A standalone product still requires a strict improvement.
  if (CandidateCost > InputCost ||
      (CandidateCost == InputCost &&
       !(HasMaskedDisturbance && EmittedMaskedSum)))
    return Root;
  // Independent masks become separate atoms in a generic polynomial table.
  // Keep the proved terms as emitted and still visit their original carriers.
  if (HasMaskedDisturbance) {
    recordResidueReading(Ctx, FirstGenerated, Budget, Storage);
    if (EmittedMaskedSum)
      *EmittedMaskedSum = true;
  }
  return Result;
}

// A common exact base needs only one offset vector. Avoid constructing one
// coefficient map per factor when the original unit-offset proof suffices.
SymRef foldCommonOffsetProduct(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                               size_t &Storage, bool &CompleteBase) {
  auto Factors = Ctx.operands(Root);
  const auto Scale = Ctx.isConst(Factors.front()) ? Factors.front() : SymRef{};
  if (Scale)
    Factors = Factors.drop_front();
  auto BaseOf = [&](const SymRef &R) -> llvm::ArrayRef<SymRef> {
    if (Ctx.op(R) != SymOp::Add)
      return {&R, 1};
    auto Terms = Ctx.operands(R);
    return Ctx.isConst(Terms.front()) ? Terms.drop_front() : Terms;
  };
  const auto First = Factors.front();
  const auto Base = BaseOf(First);
  if (Base.size() > limits::kMaxResidueBaseTerms)
    return Root;
  for (auto F : Factors)
    if (!llvm::equal(Base, BaseOf(F)))
      return Root;
  CompleteBase = true;
  const unsigned Width = Ctx.width(Root);
  const size_t Words = (size_t(Width) - 1) / 64 + 1;
  const size_t Bytes = sizeof(llvm::APInt) + 2 * Words * sizeof(uint64_t);
  const size_t Work = Factors.size() * (1 + Base.size() + Words) + Words;
  if (Factors.size() + 2 > Storage / Bytes || !Budget.canConsume(Work))
    return Root;
  Budget.consume(Work);
  Storage -= (Factors.size() + 2) * Bytes;
  unsigned Needed = Width;
  if (Scale)
    Needed -= Ctx.constValue(Scale).countr_zero();
  std::vector<llvm::APInt> Offsets;
  Offsets.reserve(Factors.size());
  for (auto F : Factors) {
    if (Ctx.op(F) == SymOp::Add && Ctx.isConst(Ctx.operand(F, 0)))
      Offsets.push_back(Ctx.constValue(Ctx.operand(F, 0)));
    else
      Offsets.emplace_back(Width, 0);
  }
  auto Zeros = minimumProductResidueZeros(Offsets, Width, Needed, Budget);
  return Zeros && *Zeros == Needed && Budget.consume() ? Ctx.mkZero(Width)
                                                       : Root;
}

SymRef foldOffsetProduct(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                         size_t &Storage, bool *EmittedMaskedSum = nullptr) {
  if (Budget.exhausted() || Ctx.op(Root) != SymOp::Mul ||
      Ctx.numOperands(Root) < limits::kMinResidueProductFactors)
    return Root;
  if (Ctx.numOperands(Root) > limits::kMaxResidueProductFactors)
    return Ctx.isConst(Ctx.operand(Root, 0))
               ? foldResidualProductResidues(Ctx, Root, Budget, Storage)
               : Root;
  if (Ctx.op(Ctx.operands(Root).back()) == SymOp::Xor) {
    if (auto Direct = foldMixedProductProgression(Ctx, Root, Budget, Storage);
        Direct != Root)
      return Direct;
  }
  const bool OffsetHint = hasOffsetFactors(Ctx, Root);
  bool CommonBase = false;
  if (OffsetHint) {
    if (auto Linear =
            foldHighProductTerms(Ctx, Root, Budget, Storage, EmittedMaskedSum);
        Linear != Root)
      return Linear;
    auto Common =
        foldCommonOffsetProduct(Ctx, Root, Budget, Storage, CommonBase);
    if (Common != Root || Budget.exhausted())
      return Common;
  }
  // Inspect the immutable product once. The zero proof and nonzero affine
  // recovery share this admission result, including a negative result.
  const bool ProjectedHint = hasProjectedResidueFactors(Ctx, Root);
  if (ProjectedHint) {
    auto Projected = foldProjectedProductResidues(Ctx, Root, Budget, Storage,
                                                  /*Admitted=*/true);
    if (Projected != Root || Budget.exhausted())
      return Projected;
  }
  if (OffsetHint) {
    bool AffineBase = false;
    if (auto Affine = foldAffineProductResidues(Ctx, Root, Budget, Storage,
                                                ProjectedHint, &AffineBase)) {
      // Complete affine products already have a cheaper authoritative path.
      // Keep the remaining allowance for polynomial recovery if its residue
      // candidates were not smaller; there is no unobserved cofactor to find.
      if (*Affine != Root || Budget.exhausted() || AffineBase || CommonBase)
        return *Affine;
      return ProjectedHint
                 ? foldResidualProductResidues(Ctx, Root, Budget, Storage)
                 : Root;
    }
  }
  if (ProjectedHint && !CommonBase) {
    auto Residual = foldResidualProductResidues(Ctx, Root, Budget, Storage);
    if (Residual != Root || Budget.exhausted())
      return Residual;
  }
  if (!OffsetHint)
    return Root;
  const unsigned Width = Ctx.width(Root);
  const size_t Words = (size_t(Width) - 1) / 64 + 1;
  using Groups = std::map<std::vector<uint32_t>, std::vector<llvm::APInt>>;
  // Include map nodes, temporary and retained base keys, vector growth and
  // wide coefficient copies. Storage is cumulative even after scratch dies.
  const size_t Fixed = 2 * sizeof(Groups::value_type) +
                       4 * sizeof(llvm::APInt) + 8 * sizeof(void *) +
                       12 * sizeof(size_t) +
                       2 * limits::kMaxResidueBaseTerms * sizeof(uint32_t);
  if (Words > Storage / (4 * sizeof(uint64_t)))
    return Root;
  const size_t PerFactor = Fixed + 4 * Words * sizeof(uint64_t);
  const size_t N = Ctx.numOperands(Root);
  const size_t GroupWork =
      N * (1 + std::bit_width(N)) * limits::kMaxResidueBaseTerms;
  if (N > Storage / PerFactor || !Budget.consume(N * (Words + 1) + GroupWork))
    return Root;
  Storage -= N * PerFactor;
  unsigned Needed = Width;
  auto Factors = Ctx.operands(Root);
  if (Ctx.isConst(Factors.front())) {
    Needed -= Ctx.constValue(Factors.front()).countr_zero();
    Factors = Factors.drop_front();
  }
  Groups Families;
  for (auto Factor : Factors) {
    llvm::APInt Offset(Width, 0);
    std::vector<uint32_t> Base;
    if (Ctx.op(Factor) == SymOp::Add &&
        Ctx.numOperands(Factor) <= limits::kMaxResidueBaseTerms) {
      auto Terms = Ctx.operands(Factor);
      if (Ctx.isConst(Terms.front())) {
        Offset = Ctx.constValue(Terms.front());
        Terms = Terms.drop_front();
      }
      if (!Budget.consume(Terms.size()))
        return Root;
      Base.reserve(Terms.size());
      for (auto Term : Terms)
        Base.push_back(Term.index());
    } else {
      Base.push_back(Factor.index());
    }
    // Canonical addition orders the nonconstant operands identically. A base
    // is the entire sum of these operands, not an assumption about their bits.
    Families[std::move(Base)].push_back(std::move(Offset));
  }

  for (auto &[Base, Offsets] : Families) {
    (void)Base;
    if (Offsets.size() < 2)
      continue;
    auto Zeros = minimumProductResidueZeros(Offsets, Width, Needed, Budget);
    if (!Zeros)
      return Root;
    Needed -= *Zeros;
    if (!Needed)
      return Budget.consume() ? Ctx.mkZero(Width) : Root;
  }
  return Root;
}

} // namespace

std::optional<unsigned>
minimumProductResidueZeros(llvm::MutableArrayRef<llvm::APInt> Offsets,
                           unsigned Width, unsigned Ceiling,
                           WorkBudget &Budget) {
  // Both children must be present to descend. The smaller child is solved
  // first; its complete bound often excludes the other child immediately.
  // Thus a dense 2^k-offset family takes O(k*2^k) work, not a Cartesian table
  // of every residue against every factor. Depth is also bounded by N.
  auto Bound = [&](auto &&Self, size_t Begin, size_t End, unsigned Bit,
                   unsigned Ceiling) -> std::optional<unsigned> {
    if (Bit == Width || !Ceiling)
      return 0;
    if (!Budget.consume(End - Begin))
      return std::nullopt;
    auto Middle = std::partition(Offsets.begin() + Begin, Offsets.begin() + End,
                                 [&](const llvm::APInt &C) { return !C[Bit]; });
    const size_t Split = Middle - Offsets.begin();
    const size_t Left = Split - Begin, Right = End - Split;
    if (!Left || !Right)
      return 0;
    const bool SmallLeft = Left <= Right;
    const size_t Small = SmallLeft ? Left : Right;
    const size_t Large = SmallLeft ? Right : Left;
    if (Small >= Ceiling)
      return Ceiling;
    auto A = Self(Self, SmallLeft ? Begin : Split, SmallLeft ? Split : End,
                  Bit + 1, Ceiling - Small);
    if (!A)
      return std::nullopt;
    const unsigned SmallBound = Small + *A;
    if (Large >= SmallBound)
      return SmallBound;
    auto B = Self(Self, SmallLeft ? Split : Begin, SmallLeft ? End : Split,
                  Bit + 1, SmallBound - Large);
    if (!B)
      return std::nullopt;
    return std::min<unsigned>(SmallBound, Large + *B);
  };
  return Bound(Bound, 0, Offsets.size(), 0, Ceiling);
}

SymRef foldProductResidues(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                           size_t &Storage) {
  if (Budget.exhausted())
    return Root;
  if (Ctx.op(Root) != SymOp::Add)
    return foldOffsetProduct(Ctx, Root, Budget, Storage);
  const size_t N = Ctx.numOperands(Root);
  if (N > limits::kMaxResidueProductSummands ||
      !llvm::any_of(Ctx.operands(Root), [&](SymRef Term) {
        if (Ctx.op(Term) != SymOp::Mul ||
            Ctx.numOperands(Term) < limits::kMinResidueProductFactors)
          return false;
        if (Ctx.numOperands(Term) > limits::kMaxResidueProductFactors)
          return Ctx.isConst(Ctx.operand(Term, 0));
        return hasOffsetFactors(Ctx, Term) ||
               hasProjectedResidueFactors(Ctx, Term);
      }))
    return Root;
  if (N > Storage / (2 * sizeof(SymRef)) || !Budget.consume(2 * N))
    return Root;
  Storage -= N * 2 * sizeof(SymRef);
  llvm::SmallVector<SymRef, 16> Terms(Ctx.operands(Root));
  bool Changed = false;
  bool OnlyMaskedChanges = true;
  for (auto &Term : Terms) {
    bool Emitted = false;
    auto Next = foldOffsetProduct(Ctx, Term, Budget, Storage, &Emitted);
    if (Next != Term) {
      Changed = true;
      OnlyMaskedChanges &= Emitted;
    }
    Term = Next;
  }
  if (!Changed)
    return Root;
  const size_t FirstCombined = Ctx.numNodes();
  auto Result = Ctx.mkAdd(Terms);
  // The whole-sum mask and coefficient passes can still combine these terms.
  // During the subsequent bottom-up walk, visit retained input words and
  // propagate their rewrites through the canonical sum just once.
  if (OnlyMaskedChanges)
    recordResidueReading(Ctx, FirstCombined, Budget, Storage);
  return Result;
}

} // namespace neverd::symbolic::detail
