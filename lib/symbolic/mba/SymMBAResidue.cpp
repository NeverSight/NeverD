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
                         size_t &Storage) {
  if (Budget.exhausted() || Ctx.op(Root) != SymOp::Mul ||
      Ctx.numOperands(Root) < limits::kMinResidueProductFactors)
    return Root;
  if (Ctx.numOperands(Root) > limits::kMaxResidueProductFactors)
    return Ctx.isConst(Ctx.operand(Root, 0))
               ? foldResidualProductResidues(Ctx, Root, Budget, Storage)
               : Root;
  const bool OffsetHint = hasOffsetFactors(Ctx, Root);
  bool CommonBase = false;
  if (OffsetHint) {
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
  for (auto &Term : Terms) {
    auto Next = foldOffsetProduct(Ctx, Term, Budget, Storage);
    Changed |= Next != Term;
    Term = Next;
  }
  return Changed ? Ctx.mkAdd(Terms) : Root;
}

} // namespace neverd::symbolic::detail
