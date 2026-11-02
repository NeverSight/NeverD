//===- SymMBAMeasure.cpp - Reading and rewriting minterm weights ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the corner measurement and the forms the weights it reads are
/// written back out in.
///
/// Setting every input to all-zeros or all-ones puts every bit position into
/// the same pattern k, which leaves the minterm `M_k` at all-ones and every
/// other at zero, so the expression evaluates to `-w_k`.  One evaluation per
/// pattern therefore reads off every weight, and the size the expression was
/// written at has nothing to do with it.
///
/// Four ways of writing the weights back out are tried, because none of them
/// is shortest for every function:
///
///   - *Constant*, when the weights agree.
///   - *Grouped*, `sum over distinct weights v of v * B_v` where `B_v` selects
///     the patterns carrying that weight.  This is what turns
///     `(x | y) - (x & y)` back into `x ^ y`.
///   - *Conjunction basis*, the weights inverted over the subset lattice.
///     This is what turns `(x ^ y) + 2 * (x & y)` back into `x + y`, which the
///     grouped form cannot: it would return the input.
///   - *Nested*, weighted differences over cumulative selectors. Overlapping
///     selectors can spell the same weights more cheaply than disjoint ones.
///
/// They are offered in that order and the caller keeps the cheapest, so the
/// order they are appended in decides ties.
///
/// A later small-region search subtracts one affine atom and synthesizes a
/// two-valued residual. It spends the remaining shared budget only after the
/// established linear and polynomial readings have been proved.
///
//===----------------------------------------------------------------------===//

#include "Limits.h"
#include "SymMBADetail.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Compiler.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <vector>

namespace neverd::symbolic::detail {

namespace {

/// Invert a subset-sum over the lattice of input patterns, in place.
///
/// On entry `C[K]` holds the weight of pattern K; on exit it holds the
/// coefficient of the conjunction K, meaning the value that makes the sum of
/// `C[S]` over every subset S of K reproduce the weight that was there.
/// Subtracting one dimension at a time is what makes this cost t * 2^t rather
/// than the 3^t a direct enumeration of submasks would.
void invertOverSubsets(std::vector<llvm::APInt> &C, unsigned NumAtoms) {
  for (unsigned J = 0; J < NumAtoms; ++J) {
    const size_t Bit = size_t(1) << J;
    for (size_t K = 0; K < C.size(); ++K)
      if (K & Bit)
        C[K] -= C[K ^ Bit];
  }
}

/// Order weights by value.  They all share a width, so an unsigned compare is
/// total.
struct APIntLess {
  bool operator()(const llvm::APInt &A, const llvm::APInt &B) const {
    return A.ult(B);
  }
};

/// Whether the weights already describe a sum of independently scaled inputs.
///
/// A zero constant weight and W[K] = W[K without one bit] + W[that bit]
/// characterize this form modulo the word width, including negative and wide
/// coefficients. The cumulative selectors otherwise spend a combinatorial
/// search on threshold functions for an ordinary weighted variable sum.
bool isIndependentWeightedSum(llvm::ArrayRef<llvm::APInt> Weights) {
  if (!Weights[0].isZero())
    return false;
  for (size_t K = 1; K < Weights.size(); ++K) {
    const size_t Rest = K & (K - 1);
    if (Rest != 0 && Weights[K] != Weights[Rest] + Weights[K ^ Rest])
      return false;
  }
  return true;
}

/// `sum over distinct weights v of v * B_v`, with `B_v` selecting the patterns
/// carrying that weight.  Exact because the minterms of one weight are
/// disjoint, so their union is a bitwise function like any other.
std::optional<SymRef> groupedForm(SymContext &Ctx,
                                  llvm::ArrayRef<llvm::APInt> Weights,
                                  llvm::ArrayRef<SymRef> Atoms,
                                  size_t TermBudget, const SolverLimits &Limits,
                                  llvm::SmallVectorImpl<SymRef> &Nested) {
  const auto NumAtoms = static_cast<unsigned>(Atoms.size());
  if (NumAtoms > Limits.MaxSynthesisAtoms)
    return std::nullopt;

  // Each group carries a table of one bit per corner, so how many groups may
  // be held at once is not only the term budget: at twenty inputs a table is
  // a hundred kilobytes, and a measurement whose every corner has a weight of
  // its own would otherwise build a million of them before anybody counted.
  const size_t Entries = Weights.size();
  const size_t Holdable = std::max<size_t>(1, Limits.SynthesisWork / Entries);
  const size_t GroupBudget = std::min(TermBudget, Holdable);

  // Ordered rather than hashed, so the terms come out the same way on every
  // run and a difference in output is a real change instead of a reshuffle.
  std::map<llvm::APInt, TruthTable, APIntLess> Groups;
  for (size_t K = 0; K < Entries; ++K) {
    if (Weights[K].isZero())
      continue;
    auto It = Groups.find(Weights[K]);
    if (It == Groups.end()) {
      // Stopping at the budget rather than after it is the difference between
      // rejecting a form and allocating one first.
      if (Groups.size() >= GroupBudget)
        return std::nullopt;
      It = Groups.emplace(Weights[K], TruthTable::zero(NumAtoms)).first;
    }
    It->second.set(K);
  }
  if (Groups.empty())
    return Ctx.mkZero(Ctx.width(Atoms[0]));

  BitwiseSynthesisLimits Synthesis = Limits.synthesis(TermBudget);
  const bool Independent = isIndependentWeightedSum(Weights);
  if (Independent &&
      llvm::all_of(Atoms, [&](SymRef R) { return Ctx.isVar(R); })) {
    // The complete weights already give an exact sum of scaled variables.
    // Bound selector construction by that available spelling, rather than by
    // the much larger enclosing polynomial. A variable costs one leaf; a
    // scaled variable costs at most a literal, a multiply and that leaf.
    // This needs no speculative interning and keeps the established tie order.
    size_t Cost = 0, Count = 0;
    for (unsigned I = 0; I != NumAtoms; ++I) {
      const auto &Coefficient = Weights[size_t(1) << I];
      if (Coefficient.isZero())
        continue;
      Cost += Coefficient.isOne() ? 1 : 3;
      ++Count;
    }
    Cost = Count ? Cost + Count - 1 : 1;
    Synthesis.MaxCost = std::min(Synthesis.MaxCost, Cost);
  }
  // Only the new cumulative search is skipped for independent weighted sums;
  // the grouped and conjunction candidates remain available, since this test
  // establishes a shape rather than a minimum expression cost.
  if (Groups.size() > 1 && Groups.size() <= Holdable / 2 && !Independent) {
    TruthTable Nonzero = TruthTable::zero(NumAtoms);
    for (const auto &[Value, Table] : Groups)
      Nonzero |= Table;

    // For weights v1,...,vn in any fixed order, let Si select groups i..n.
    // Then sum_i (vi - v(i-1)) * Si, with v0=0, has weight vj on group j.
    // This telescopes modulo the word width, including negative coefficients.
    // Both orders are useful because complements can make one selector cheap.
    auto appendNested = [&](const auto &Ordered) {
      TruthTable Remaining = Nonzero;
      llvm::APInt Previous(Ctx.width(Atoms[0]), 0);
      llvm::SmallVector<SymRef, 8> Terms;
      for (const auto &[Value, Table] : Ordered) {
        std::optional<SymRef> Selector =
            synthesizeBitwise(Ctx, Remaining, Atoms, Synthesis);
        if (!Selector)
          return;
        Terms.push_back(Ctx.mkMul(Ctx.mkConst(Value - Previous), *Selector));
        Previous = Value;
        Remaining ^= Table;
      }
      Nested.push_back(Ctx.mkAdd(Terms));
    };
    appendNested(Groups);
    appendNested(llvm::reverse(Groups));
  }

  llvm::SmallVector<SymRef, 8> Terms;
  for (const auto &[Value, Table] : Groups) {
    // Synthesis declines when no spelling of the selector fits the budget.
    // That is one candidate form lost, not a failure: the conjunction basis
    // below is offered from the same weights and has no table to build.
    std::optional<SymRef> Selector =
        synthesizeBitwise(Ctx, Table, Atoms, Synthesis);
    if (!Selector)
      return std::nullopt;
    Terms.push_back(Ctx.mkMul(Ctx.mkConst(Value), *Selector));
  }
  return Ctx.mkAdd(Terms);
}

/// `sum over subsets S of c_S * AND(inputs in S)`, the conjunction basis.
///
/// The empty conjunction is the all-ones word, so its coefficient contributes
/// the constant term with the sign turned around.
std::optional<SymRef> conjunctionForm(SymContext &Ctx,
                                      std::vector<llvm::APInt> Coefficients,
                                      llvm::ArrayRef<SymRef> Atoms,
                                      size_t TermBudget,
                                      const SolverLimits &Limits) {
  const auto NumAtoms = static_cast<unsigned>(Atoms.size());
  const uint32_t Width = Ctx.width(Atoms[0]);

  // The inversion touches every entry once per input, so it costs t * 2^t wide
  // subtractions before anything can be said about how many terms come out of
  // it.  That was invisible while the arity dial was a fixed sixteen and it is
  // tens of millions of operations at the arities the dial now reaches, which
  // makes it the one step of the linear reading large enough to be worth
  // costing.  Dividing rather than multiplying, because the product is exactly
  // the quantity that would not fit.
  if (NumAtoms != 0 && Coefficients.size() > Limits.SynthesisWork / NumAtoms)
    return std::nullopt;

  invertOverSubsets(Coefficients, NumAtoms);

  size_t NumTerms = 0;
  for (const llvm::APInt &C : Coefficients)
    NumTerms += !C.isZero();
  if (NumTerms > TermBudget)
    return std::nullopt;

  llvm::SmallVector<SymRef, 16> Terms;
  for (size_t K = 0; K < Coefficients.size(); ++K) {
    const llvm::APInt &C = Coefficients[K];
    if (C.isZero())
      continue;
    if (K == 0) {
      Terms.push_back(Ctx.mkConst(-C));
      continue;
    }
    llvm::SmallVector<SymRef, 16> Factors;
    for (unsigned J = 0; J < NumAtoms; ++J)
      if (K & (size_t(1) << J))
        Factors.push_back(Atoms[J]);
    Terms.push_back(Ctx.mkMul(Ctx.mkConst(C), Ctx.mkAnd(Factors)));
  }
  if (Terms.empty())
    return Ctx.mkZero(Width);
  return Ctx.mkAdd(Terms);
}

llvm::APInt randomWord(std::mt19937_64 &Rng, uint32_t Width) {
  llvm::SmallVector<uint64_t, 4> Words((Width + 63) / 64);
  for (uint64_t &Word : Words)
    Word = Rng();
  return llvm::APInt(Width, Words);
}

// Both complete plans and additive residuals use the same original variable
// union and deterministic assignments. A canceled term can still be the only
// use of a variable whose random words precede a surviving input's words.
template <typename NativeCheck, typename WideCheck>
bool agreeOnAssignments(const SymContext &Ctx,
                        llvm::SmallVectorImpl<uint32_t> &Vars, unsigned Samples,
                        bool Native, NativeCheck &&CheckNative,
                        WideCheck &&CheckWide) {
  std::sort(Vars.begin(), Vars.end());
  Vars.erase(std::unique(Vars.begin(), Vars.end()), Vars.end());
  // A contiguous window of sorted unique input IDs needs only its origin to
  // locate assignments. Sparse contexts retain the compact lookup and never
  // allocate scratch for unrelated or unused variables.
  const uint32_t First = Vars.empty() ? 0 : Vars.front();
  const bool Dense = Vars.empty() || Vars.back() - First == Vars.size() - 1;
  llvm::SmallDenseMap<uint32_t, unsigned, 16> Positions;
  if (!Dense)
    for (unsigned I = 0; I < Vars.size(); ++I)
      Positions[Vars[I]] = I;
  auto Position = [&](uint32_t Id) {
    if (Dense)
      return Id - First;
    auto It = Positions.find(Id);
    assert(It != Positions.end() && "plan requested an unlisted variable");
    return It->second;
  };

  if (Native) {
    llvm::SmallVector<uint64_t, 8> Masks;
    for (uint32_t Id : Vars) {
      const uint32_t Width = Ctx.varInfo(Id).Width;
      assert(Width <= 64 && "word plan contains a wide variable");
      Masks.push_back(~uint64_t(0) >> (64 - Width));
    }
    llvm::SmallVector<uint64_t, 8> Assignment(Masks.size());
    auto CheckSamples = [&](auto &&Lookup) {
      std::mt19937_64 Rng(0x9E3779B97F4A7C15ull);
      for (unsigned S = 0; S < Samples; ++S) {
        // The first three points and one generator word per narrow input match
        // the arbitrary-width path. Only referenced inputs advance the stream.
        for (size_t I = 0; I < Assignment.size(); ++I)
          Assignment[I] = S == 0   ? 0
                          : S == 1 ? Masks[I]
                          : S == 2 ? 1
                                   : Rng() & Masks[I];
        if (!CheckNative(Lookup))
          return false;
      }
      return true;
    };
    // Select the lookup once, outside both evaluation loops. Dense samples
    // need neither the position table nor a per-variable layout branch.
    if (Dense)
      return CheckSamples([&](uint32_t Id) { return Assignment[Id - First]; });
    return CheckSamples([&](uint32_t Id) { return Assignment[Position(Id)]; });
  }

  llvm::SmallVector<llvm::APInt, 8> Assignment;
  Assignment.reserve(Vars.size());
  for (uint32_t Id : Vars)
    Assignment.emplace_back(Ctx.varInfo(Id).Width, 0);
  auto Lookup = [&](uint32_t Id) -> const llvm::APInt * {
    return &Assignment[Position(Id)];
  };

  std::mt19937_64 Rng(0x9E3779B97F4A7C15ull);
  for (unsigned S = 0; S < Samples; ++S) {
    for (size_t I = 0; I < Assignment.size(); ++I) {
      uint32_t W = Assignment[I].getBitWidth();
      // The first few rounds pin every input to a corner of the space, where a
      // mistake in the corner arithmetic itself would show; the rest are
      // ordinary values, where a mistake in the algebra would.
      switch (S) {
      case 0:
        Assignment[I] = llvm::APInt(W, 0);
        break;
      case 1:
        Assignment[I] = llvm::APInt::getAllOnes(W);
        break;
      case 2:
        Assignment[I] = llvm::APInt(W, 1);
        break;
      default:
        Assignment[I] = randomWord(Rng, W);
        break;
      }
    }
    if (!CheckWide(Lookup))
      return false;
  }
  return true;
}

bool splitCommonOperands(const SymContext &Ctx, SymRef A, SymRef B,
                         llvm::SmallVectorImpl<SymRef> &Common,
                         llvm::SmallVectorImpl<SymRef> &LeftRest,
                         llvm::SmallVectorImpl<SymRef> &RightRest) {
  llvm::SmallVector<SymRef, 8> Left(Ctx.operands(A)), Right(Ctx.operands(B));
  // Canonical additions order scaled terms by their base, not their full node.
  llvm::sort(Left);
  llvm::sort(Right);
  size_t I = 0, J = 0;
  while (I < Left.size() && J < Right.size()) {
    if (Left[I] == Right[J]) {
      Common.push_back(Left[I++]);
      ++J;
    } else if (Left[I] < Right[J]) {
      LeftRest.push_back(Left[I++]);
    } else {
      RightRest.push_back(Right[J++]);
    }
  }
  LeftRest.append(Left.begin() + I, Left.end());
  RightRest.append(Right.begin() + J, Right.end());
  return !Common.empty();
}

// A product's unchanged factors need evaluating only if the two remaining
// products differ at this assignment. They are never canceled from equality:
// an even common factor can annihilate a nonzero modular difference.
std::optional<bool>
agreeAfterFactoringProducts(const SymContext &Ctx, SymRef A, SymRef B,
                            unsigned Samples,
                            llvm::ArrayRef<SymRef> Canceled = {},
                            llvm::ArrayRef<SymRef> LeftAddends = {},
                            llvm::ArrayRef<SymRef> RightAddends = {}) {
  if (Ctx.op(A) != SymOp::Mul || Ctx.op(B) != SymOp::Mul ||
      Ctx.width(A) != Ctx.width(B))
    return std::nullopt;
  llvm::SmallVector<SymRef, 8> Common, LeftRest, RightRest;
  if (!splitCommonOperands(Ctx, A, B, Common, LeftRest, RightRest))
    return std::nullopt;
  // A small intersection cannot justify splitting the larger changed DAGs
  // into per-factor plans. Keep their internal sharing in the complete-plan
  // path; factoring is useful when unchanged factors dominate the remainder.
  if (Common.size() < LeftRest.size() + RightRest.size())
    return std::nullopt;

  const uint32_t Width = Ctx.width(A);
  llvm::SmallVector<uint32_t, 16> Vars;
  for (SymRef R : Canceled)
    Ctx.collectVars(R, Vars);
  for (SymRef R : Common)
    Ctx.collectVars(R, Vars);
  bool Native = Width <= 64 && llvm::all_of(Vars, [&](uint32_t Id) {
                  return Ctx.varInfo(Id).Width <= 64;
                });
  std::vector<SymEvalPlan> CommonPlans, LeftPlans, RightPlans, LeftSumPlans,
      RightSumPlans;
  auto Compile = [&](llvm::ArrayRef<SymRef> Factors,
                     std::vector<SymEvalPlan> &Plans) {
    Plans.reserve(Factors.size());
    for (SymRef R : Factors) {
      SymEvalPlan &Plan = Plans.emplace_back(Ctx, R);
      Native &= Plan.fitsU64();
      Vars.append(Plan.vars().begin(), Plan.vars().end());
    }
  };
  Compile(LeftRest, LeftPlans);
  Compile(RightRest, RightPlans);
  Compile(LeftAddends, LeftSumPlans);
  Compile(RightAddends, RightSumPlans);
  // Keep every original input in the assignment stream, but lower unchanged
  // factors only if a nonzero sampled difference actually reaches them.
  auto CommonPlan = [&](size_t I) -> SymEvalPlan & {
    if (CommonPlans.size() == I) {
      if (!I)
        CommonPlans.reserve(Common.size());
      CommonPlans.emplace_back(Ctx, Common[I]);
    }
    return CommonPlans[I];
  };
  return agreeOnAssignments(
      Ctx, Vars, Samples, Native,
      [&](auto &&Lookup) {
        uint64_t Rest = 0;
        for (SymEvalPlan &Plan : LeftSumPlans)
          Rest += Plan.evalU64With(Lookup);
        for (SymEvalPlan &Plan : RightSumPlans)
          Rest -= Plan.evalU64With(Lookup);
        uint64_t Left = 1, Right = 1;
        for (SymEvalPlan &Plan : LeftPlans)
          Left *= Plan.evalU64With(Lookup);
        for (SymEvalPlan &Plan : RightPlans)
          Right *= Plan.evalU64With(Lookup);
        const uint64_t Mask = ~uint64_t(0) >> (64 - Width);
        uint64_t Difference = (Left - Right) & Mask;
        for (size_t I = 0; I < Common.size() && Difference; ++I) {
          SymEvalPlan &Plan = CommonPlan(I);
          uint64_t Value;
          if (Plan.fitsU64()) {
            Value = Plan.evalU64With(Lookup);
          } else {
            // All inputs fit a native word, but an unchanged factor can
            // contain a wider intermediate. Preserve that operation's AP
            // semantics while using the same native input assignments.
            llvm::APInt Input(1, 0);
            Value = Plan.evalWith([&](uint32_t Id) -> const llvm::APInt * {
                          Input =
                              llvm::APInt(Ctx.varInfo(Id).Width, Lookup(Id));
                          return &Input;
                        })
                        .getZExtValue();
          }
          Difference = (Difference * Value) & Mask;
        }
        return ((Difference + Rest) & Mask) == 0;
      },
      [&](auto &&Lookup) {
        llvm::APInt Rest(Width, 0);
        for (SymEvalPlan &Plan : LeftSumPlans)
          Rest += Plan.evalWith(Lookup);
        for (SymEvalPlan &Plan : RightSumPlans)
          Rest -= Plan.evalWith(Lookup);
        llvm::APInt Left(Width, 1), Right(Width, 1);
        for (SymEvalPlan &Plan : LeftPlans)
          Left *= Plan.evalWith(Lookup);
        for (SymEvalPlan &Plan : RightPlans)
          Right *= Plan.evalWith(Lookup);
        llvm::APInt Difference = Left - Right;
        for (size_t I = 0; I < Common.size() && !Difference.isZero(); ++I)
          Difference *= CommonPlan(I).evalWith(Lookup);
        return (Difference + Rest).isZero();
      });
}

// Cancel only identical complete addends, with their multiplicities, in the
// same modular ring. The remaining sums have exactly the same difference at
// every assignment; no sampling is used to establish the cancellation itself.
LLVM_ATTRIBUTE_NOINLINE
std::optional<bool> agreeAfterCancelingAddends(const SymContext &Ctx, SymRef A,
                                               SymRef B, unsigned Samples) {
  if (Ctx.op(A) != SymOp::Add || Ctx.op(B) != SymOp::Add ||
      Ctx.width(A) != Ctx.width(B))
    return std::nullopt;
  llvm::SmallVector<SymRef, 8> Common, LeftRest, RightRest;
  splitCommonOperands(Ctx, A, B, Common, LeftRest, RightRest);
  // Isolate one n-ary product beside an additive tail. Splitting a sum of
  // several n-ary products into separate plans would lose shared subterms;
  // that shape keeps the complete DAG evaluation instead.
  auto IsolatedProduct = [&](llvm::ArrayRef<SymRef> Terms) {
    size_t Best = Terms.size();
    for (size_t I = 0; I < Terms.size(); ++I) {
      if (Ctx.op(Terms[I]) == SymOp::Mul && Ctx.numOperands(Terms[I]) > 2) {
        if (Best != Terms.size())
          return Terms.size();
        Best = I;
      }
    }
    return Best;
  };
  const size_t LeftProduct = IsolatedProduct(LeftRest),
               RightProduct = IsolatedProduct(RightRest);
  if (LeftProduct != LeftRest.size() && RightProduct != RightRest.size()) {
    llvm::SmallVector<SymRef, 8> OtherLeft(LeftRest), OtherRight(RightRest);
    OtherLeft.erase(OtherLeft.begin() + LeftProduct);
    OtherRight.erase(OtherRight.begin() + RightProduct);
    if (auto Agrees = agreeAfterFactoringProducts(
            Ctx, LeftRest[LeftProduct], RightRest[RightProduct], Samples,
            Common, OtherLeft, OtherRight))
      return Agrees;
  }
  if (Common.empty())
    return std::nullopt;

  llvm::SmallVector<uint32_t, 16> Vars;
  for (SymRef R : Common)
    Ctx.collectVars(R, Vars);
  bool Native = Ctx.width(A) <= 64 && llvm::all_of(Vars, [&](uint32_t Id) {
                  return Ctx.varInfo(Id).Width <= 64;
                });
  std::vector<SymEvalPlan> LeftPlans, RightPlans;
  auto Compile = [&](llvm::ArrayRef<SymRef> Rest,
                     std::vector<SymEvalPlan> &Plans) {
    Plans.reserve(Rest.size());
    for (SymRef R : Rest) {
      SymEvalPlan &Plan = Plans.emplace_back(Ctx, R);
      Native &= Plan.fitsU64();
      Vars.append(Plan.vars().begin(), Plan.vars().end());
    }
  };
  Compile(LeftRest, LeftPlans);
  Compile(RightRest, RightPlans);
  const uint32_t Width = Ctx.width(A);
  return agreeOnAssignments(
      Ctx, Vars, Samples, Native,
      [&](auto &&Lookup) {
        uint64_t Difference = 0;
        for (SymEvalPlan &Plan : LeftPlans)
          Difference += Plan.evalU64With(Lookup);
        for (SymEvalPlan &Plan : RightPlans)
          Difference -= Plan.evalU64With(Lookup);
        return (Difference & (~uint64_t(0) >> (64 - Width))) == 0;
      },
      [&](auto &&Lookup) {
        llvm::APInt Difference(Width, 0);
        for (SymEvalPlan &Plan : LeftPlans)
          Difference += Plan.evalWith(Lookup);
        for (SymEvalPlan &Plan : RightPlans)
          Difference -= Plan.evalWith(Lookup);
        return Difference.isZero();
      });
}

/// The largest t with 2^t entries or fewer.
unsigned atomsFittingEntries(size_t Entries) {
  unsigned Atoms = 0;
  while (Atoms < kMaxTruthTableAtoms && (size_t(1) << (Atoms + 1)) <= Entries)
    ++Atoms;
  return Atoms;
}

/// Recognize a reduction over each measured input exactly once. At Boolean
/// corners its result depends only on whether the pattern is empty, complete,
/// or has odd parity; evaluating the full expression at every corner is then
/// unnecessary.
std::optional<SymOp> fullCornerReduction(const SymContext &Ctx, SymRef R,
                                         llvm::ArrayRef<uint32_t> SortedAtoms) {
  SymOp Op = Ctx.op(R);
  if (Op != SymOp::And && Op != SymOp::Or && Op != SymOp::Xor)
    return std::nullopt;
  llvm::ArrayRef<SymRef> Terms = Ctx.operands(R);
  if (Terms.size() != SortedAtoms.size())
    return std::nullopt;
  llvm::SmallVector<uint32_t, 8> Seen;
  for (SymRef Term : Terms) {
    if (!Ctx.isVar(Term))
      return std::nullopt;
    Seen.push_back(Ctx.varId(Term));
  }
  std::sort(Seen.begin(), Seen.end());
  return llvm::equal(Seen, SortedAtoms) ? std::optional<SymOp>(Op)
                                        : std::nullopt;
}

bool reductionSelects(SymOp Op, size_t Pattern, size_t Corners) {
  switch (Op) {
  case SymOp::And:
    return Pattern == Corners - 1;
  case SymOp::Or:
    return Pattern != 0;
  case SymOp::Xor:
    return std::popcount(Pattern) & 1;
  default:
    llvm_unreachable("not a corner reduction");
  }
}

} // namespace

std::optional<size_t> cornerCount(size_t NumAtoms) {
  if (NumAtoms > kMaxTruthTableAtoms)
    return std::nullopt;
  return patternCount(static_cast<unsigned>(NumAtoms));
}

SolverLimits resolveLimits(const MBAOptions &Opts) {
  // A corner table holds one weight per corner.  Charging each at the inline
  // size of a weight is an order of magnitude rather than an allocator's
  // ledger — a word wider than sixty-four bits adds a heap allocation on top —
  // and an order of magnitude is what the dial is for: it decides whether a
  // measurement is attempted at all, not how the memory is laid out.
  const unsigned Affordable =
      atomsFittingEntries(Opts.MaxTableBytes / sizeof(llvm::APInt));

  SolverLimits Out;
  Out.MaxAtoms =
      std::min(Opts.MaxAtoms == MBAOptions::Unlimited ? kMaxTruthTableAtoms
                                                      : Opts.MaxAtoms,
               Affordable);
  // Never wider than the measurement that produced the weights: a truth table
  // over more inputs than were measured describes nothing the solver holds.
  Out.MaxSynthesisAtoms = std::min(
      Opts.MaxSynthesisAtoms == MBAOptions::Unlimited ? kMaxTruthTableAtoms
                                                      : Opts.MaxSynthesisAtoms,
      Out.MaxAtoms);
  Out.MaxOptimalSynthesisAtoms = Opts.MaxOptimalSynthesisAtoms;

  // A truth table is a bit an entry, so the same byte ceiling buys eight times
  // what a corner table does.  Synthesis charges itself a unit per bit it
  // touches and per bit it keeps, so this one number bounds both its time and
  // what it holds while spending it.
  constexpr size_t Ceiling = std::numeric_limits<size_t>::max();
  const size_t ByBytes =
      Opts.MaxTableBytes > Ceiling / 8 ? Ceiling : Opts.MaxTableBytes * 8;
  Out.SynthesisWork = std::min(Opts.MaxWork, ByBytes);
  return Out;
}

std::vector<llvm::APInt> measure(const SymContext &Ctx, SymRef Body,
                                 llvm::ArrayRef<uint32_t> Atoms) {
  const uint32_t Width = Ctx.width(Body);
  const auto NumAtoms = static_cast<unsigned>(Atoms.size());
  const std::optional<size_t> Corners = cornerCount(NumAtoms);
  assert(Corners && "corner table does not fit the host address space");

  std::vector<llvm::APInt> Weights(*Corners);

  // A pair of full-input Boolean reductions is common in wide MBAs. Its
  // exact weights are just the number of reductions selecting each corner.
  // Keep the general evaluator for smaller tables and every other shape.
  if (NumAtoms >= 8) {
    llvm::SmallVector<uint32_t, 8> SortedAtoms(Atoms.begin(), Atoms.end());
    std::sort(SortedAtoms.begin(), SortedAtoms.end());
    std::optional<SymOp> First = fullCornerReduction(Ctx, Body, SortedAtoms);
    std::optional<SymOp> Second;
    if (!First && Ctx.op(Body) == SymOp::Add && Ctx.numOperands(Body) == 2) {
      First = fullCornerReduction(Ctx, Ctx.operand(Body, 0), SortedAtoms);
      Second = fullCornerReduction(Ctx, Ctx.operand(Body, 1), SortedAtoms);
      if (!Second)
        First.reset();
    }
    if (First) {
      for (size_t Pattern = 0; Pattern < *Corners; ++Pattern) {
        unsigned Selected = reductionSelects(*First, Pattern, *Corners);
        if (Second)
          Selected += reductionSelects(*Second, Pattern, *Corners);
        Weights[Pattern] = llvm::APInt(Width, Selected);
      }
      return Weights;
    }
  }

  CornerEvaluator Evaluator(Ctx, Body, Atoms);
  for (size_t K = 0; K < *Corners; ++K) {
    // Retain the Gray traversal and ordinary binary table indices used by
    // coefficient and selector consumers.
    const size_t Pattern = K ^ (K >> 1);
    Weights[Pattern] = -Evaluator.eval(Pattern);
  }
  return Weights;
}

size_t readingCost(const SymContext &Ctx, SymRef R) {
  return Ctx.readabilityCost(R);
}

SymReadability readingScore(const SymContext &Ctx, SymRef R) {
  return Ctx.readability(R);
}

bool doesNotGrow(const SymContext &Ctx, SymRef Candidate, SymRef Input) {
  const SymReadability Score = readingScore(Ctx, Candidate);
  // Saturation cannot establish that two actual printed trees have equal size.
  return Score.Nodes != std::numeric_limits<size_t>::max() &&
         Score <= readingScore(Ctx, Input);
}

bool agreeOnSamples(const SymContext &Ctx, SymRef A, SymRef B,
                    unsigned Samples) {
  if (!Samples)
    return true;
  if (auto Agrees = agreeAfterCancelingAddends(Ctx, A, B, Samples))
    return *Agrees;
  if (auto Agrees = agreeAfterFactoringProducts(Ctx, A, B, Samples))
    return *Agrees;
  SymEvalPlan PlanA(Ctx, A);
  SymEvalPlan PlanB(Ctx, B);
  llvm::SmallVector<uint32_t, 16> Vars(PlanA.vars());
  Vars.append(PlanB.vars().begin(), PlanB.vars().end());
  return agreeOnAssignments(
      Ctx, Vars, Samples, PlanA.fitsU64() && PlanB.fitsU64(),
      [&](auto &&Lookup) {
        return PlanA.evalU64With(Lookup) == PlanB.evalU64With(Lookup);
      },
      [&](auto &&Lookup) {
        return PlanA.evalWith(Lookup) == PlanB.evalWith(Lookup);
      });
}

size_t termBudget(const SymContext &Ctx, SymRef E, const MBAOptions &Opts) {
  return Opts.AllowGrowth ? std::numeric_limits<size_t>::max()
                          : readingCost(Ctx, E);
}

void linearCandidates(SymContext &Ctx, std::vector<llvm::APInt> Weights,
                      llvm::ArrayRef<SymRef> Atoms, size_t TermBudget,
                      const SolverLimits &Limits,
                      llvm::SmallVectorImpl<SymRef> &Out) {
  auto append = [&](SymRef Form) {
    if (!llvm::is_contained(Out, Form))
      Out.push_back(Form);
  };
  if (llvm::all_of(Weights,
                   [&](const llvm::APInt &W) { return W == Weights[0]; }))
    append(Ctx.mkConst(-Weights[0]));
  llvm::SmallVector<SymRef, 2> Nested;
  if (std::optional<SymRef> Grouped =
          groupedForm(Ctx, Weights, Atoms, TermBudget, Limits, Nested))
    append(*Grouped);
  // Handing the weights over rather than copying: at the widths and arity this
  // reaches, a copy would be tens of thousands of allocations.
  if (std::optional<SymRef> Conjunctions =
          conjunctionForm(Ctx, std::move(Weights), Atoms, TermBudget, Limits))
    append(*Conjunctions);
  // Preserve the established tie order; the new forms must improve the cost
  // to displace a grouped or conjunction-basis answer.
  for (SymRef Form : Nested)
    append(Form);
}

void affineResidualCandidates(SymContext &Ctx,
                              llvm::ArrayRef<llvm::APInt> Weights,
                              llvm::ArrayRef<SymRef> Atoms, size_t TermBudget,
                              const SolverLimits &Limits, WorkBudget &Budget,
                              llvm::SmallVectorImpl<SymRef> &Out,
                              bool *MayHaveBooleanPair) {
  if (MayHaveBooleanPair)
    *MayHaveBooleanPair = false;
  const unsigned Count = static_cast<unsigned>(Atoms.size());
  // This search reuses only the existing small exact Boolean recipes. A
  // larger caller ceiling must not trigger the four-input optimal table here.
  if (Count < 2 || Count > std::min({limits::kMaxResidualSynthesisAtoms,
                                     Limits.MaxOptimalSynthesisAtoms,
                                     Limits.MaxSynthesisAtoms}))
    return;
  if (TermBudget < 3)
    return;
  assert(Weights.size() == (size_t(1) << Count));
  if (!Budget.consume(Weights.size()))
    return;
  if (isIndependentWeightedSum(Weights))
    return;
  llvm::SmallVector<llvm::APInt, 4> Values;
  for (const llvm::APInt &Value : Weights) {
    if (llvm::is_contained(Values, Value))
      continue;
    if (Values.size() == 4)
      return;
    Values.push_back(Value);
  }
  // At most two values already have the offset/selector readings above. A
  // shifted two-valued residual can cover at most four original values.
  if (Values.size() < 3)
    return;

  if (!Budget.consume(Count * Weights.size()))
    return;
  unsigned Support = 0;
  for (unsigned Axis = 0; Axis < Count; ++Axis) {
    const size_t Bit = size_t(1) << Axis;
    for (size_t K = 0; K < Weights.size(); ++K)
      if (!(K & Bit) && Weights[K] != Weights[K | Bit]) {
        ++Support;
        break;
      }
  }
  // Every essential input needs a leaf and connecting the leaves requires
  // at least Support - 1 printed binary operations. The three distinct
  // values above guarantee more than one essential input.
  if (TermBudget < 2 * Support - 1)
    return;

  // Reuse this charged classification in the later pair reading. Most
  // regions cannot have that form and should not scan their weights twice.
  if (MayHaveBooleanPair)
    *MayHaveBooleanPair = Support == 3 && Values.size() == 4;

  llvm::DenseMap<uint32_t, SymRef> Synthesized;

  for (unsigned Axis = 0; Axis < Count; ++Axis) {
    if (!Budget.consume(Weights.size()))
      return;
    llvm::SmallVector<llvm::APInt, 2> Low, High;
    bool TooMany = false;
    for (size_t K = 0; K < Weights.size(); ++K) {
      auto &Half = (K & (size_t(1) << Axis)) ? High : Low;
      if (llvm::is_contained(Half, Weights[K]))
        continue;
      if (Half.size() == 2) {
        TooMany = true;
        break;
      }
      Half.push_back(Weights[K]);
    }
    if (TooMany)
      continue;

    // Subtracting c*X leaves the low half unchanged and translates the high
    // half by -c. If the low half has two values, any chosen high value must
    // align with one of them. Otherwise one of the two high values must align
    // with the single low value. Thus at most two coefficients suffice; edge
    // differences alone miss valid translations. No inverse is needed, even
    // for coefficients that are not units modulo the word width.
    llvm::SmallVector<llvm::APInt, 2> Coefficients;
    if (Low.size() == 2) {
      Coefficients.push_back(High[0] - Low[0]);
      Coefficients.push_back(High[0] - Low[1]);
    } else {
      for (const llvm::APInt &Value : High)
        Coefficients.push_back(Value - Low[0]);
    }
    for (const llvm::APInt &Coefficient : Coefficients) {
      if (Coefficient.isZero())
        continue;
      if (!Budget.consume(Weights.size()))
        return;
      llvm::SmallVector<llvm::APInt, 2> ResidualValues;
      TruthTable Selector = TruthTable::zero(Count);
      bool TwoValued = true;
      for (size_t K = 0; K < Weights.size(); ++K) {
        llvm::APInt Value = Weights[K];
        if (K & (size_t(1) << Axis))
          Value -= Coefficient;
        if (ResidualValues.empty())
          ResidualValues.push_back(Value);
        if (Value == ResidualValues[0])
          continue;
        if (ResidualValues.size() == 1)
          ResidualValues.push_back(Value);
        if (Value != ResidualValues[1]) {
          TwoValued = false;
          break;
        }
        Selector.set(K);
      }
      if (!TwoValued || ResidualValues.size() != 2)
        continue;

      for (unsigned Orientation = 0; Orientation < 2; ++Orientation) {
        // Pay for each construction, and prepay the small exact table on its
        // first lookup in this region. Reusing an already-built selector does
        // not restart its synthesis allowance.
        if (!Budget.consume(Weights.size()))
          return;
        const TruthTable Table = Orientation == 0 ? Selector : ~Selector;
        const uint32_t Key = static_cast<uint32_t>(Table.packed());
        auto Found = Synthesized.find(Key);
        if (Found == Synthesized.end()) {
          const unsigned Arity = std::popcount(truthTableSupport(Table));
          const size_t SynthesisWork = bitwiseSynthesisWork(Arity);
          // This optional table must fit the remaining shared allowance.
          // Refusing it must not spend work that later regions could use.
          if (SynthesisWork > Limits.SynthesisWork ||
              !Budget.canConsume(SynthesisWork))
            continue;
          if (!Budget.consume(SynthesisWork))
            return;
          BitwiseSynthesisLimits Synthesis = Limits.synthesis(TermBudget);
          Synthesis.MaxOptimalAtoms = Count;
          Synthesis.MaxWork = SynthesisWork;
          std::optional<SymRef> Boolean =
              synthesizeBitwise(Ctx, Table, Atoms, Synthesis);
          Found =
              Synthesized.try_emplace(Key, Boolean.value_or(SymRef())).first;
        }
        if (!Found->second.isValid())
          continue;
        const llvm::APInt &Base = ResidualValues[Orientation];
        const llvm::APInt &Other = ResidualValues[1 - Orientation];
        SymRef Form =
            Ctx.mkAdd({Ctx.mkConst(-Base),
                       Ctx.mkMul(Ctx.mkConst(Other - Base), Found->second),
                       Ctx.mkMul(Ctx.mkConst(Coefficient), Atoms[Axis])});
        // With offset -1, reversing the two coefficients under a complement
        // spells the same word: ~(-a*B-c*X) = -1+a*B+c*X. Reuse the proven
        // selector and keep one form per orientation; no new search is needed.
        if (Base.isOne() && Budget.consume(Weights.size())) {
          SymRef Complement = Ctx.mkNot(
              Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(Base - Other), Found->second),
                        Ctx.mkMul(Ctx.mkConst(-Coefficient), Atoms[Axis])));
          if (readingScore(Ctx, Complement) < readingScore(Ctx, Form))
            Form = Complement;
        }
        if (readingCost(Ctx, Form) <= TermBudget &&
            !llvm::is_contained(Out, Form))
          Out.push_back(Form);
      }
    }
  }
}

void booleanResidualCandidates(SymContext &Ctx,
                               llvm::ArrayRef<llvm::APInt> Weights,
                               llvm::ArrayRef<SymRef> Atoms, size_t TermBudget,
                               const SolverLimits &Limits, WorkBudget &Budget,
                               llvm::SmallVectorImpl<SymRef> &Out) {
  const unsigned Count = static_cast<unsigned>(Atoms.size());
  // With two effective inputs, four different weights require both selectors
  // to be balanced. The balanced tables are the inputs, their complements,
  // XOR and its complement. Two XOR tables cannot give four value pairs, so
  // one selector is an affine atom already covered by the earlier reading.
  if (Count < 3 ||
      Count > std::min({limits::kMaxResidualSynthesisAtoms,
                        Limits.MaxOptimalSynthesisAtoms,
                        Limits.MaxSynthesisAtoms}) ||
      TermBudget < 3)
    return;
  assert(Weights.size() == (size_t(1) << Count));
  if (!Budget.consume(Weights.size()))
    return;
  llvm::SmallVector<llvm::APInt, 4> Values;
  for (const llvm::APInt &Value : Weights) {
    if (llvm::is_contained(Values, Value))
      continue;
    if (Values.size() == 4)
      return;
    Values.push_back(Value);
  }
  // Repeated values leave choices in how a selector should split a group.
  // This bounded reading handles only the unambiguous four-value case.
  if (Values.size() != 4)
    return;
  // Four values have only three possible pairs of opposite corners. Reject
  // nonrectangular tables before computing support or constructing selectors.
  if (!Budget.consume(3))
    return;
  if (Values[0] + Values[1] != Values[2] + Values[3] &&
      Values[0] + Values[2] != Values[1] + Values[3] &&
      Values[0] + Values[3] != Values[1] + Values[2])
    return;
  if (!Budget.consume(Count * Weights.size()))
    return;
  unsigned Support = 0;
  for (unsigned Axis = 0; Axis < Count; ++Axis) {
    const size_t Bit = size_t(1) << Axis;
    for (size_t K = 0; K < Weights.size(); ++K)
      if (!(K & Bit) && Weights[K] != Weights[K | Bit]) {
        ++Support;
        break;
      }
  }
  if (Support < 3 || TermBudget < 2 * Support - 1)
    return;

  llvm::DenseMap<uint32_t, SymRef> Synthesized;
  auto selector = [&](const TruthTable &Table) -> std::optional<SymRef> {
    const uint32_t Key = static_cast<uint32_t>(Table.packed());
    auto Found = Synthesized.find(Key);
    if (Found == Synthesized.end()) {
      const unsigned Arity = std::popcount(truthTableSupport(Table));
      const size_t Work = bitwiseSynthesisWork(Arity);
      if (Work > Limits.SynthesisWork || !Budget.canConsume(Work))
        return std::nullopt;
      if (!Budget.consume(Work))
        return std::nullopt;
      BitwiseSynthesisLimits Synthesis = Limits.synthesis(TermBudget);
      Synthesis.MaxOptimalAtoms = Count;
      Synthesis.MaxWork = Work;
      auto Built = synthesizeBitwise(Ctx, Table, Atoms, Synthesis);
      Found = Synthesized.try_emplace(Key, Built.value_or(SymRef())).first;
    }
    if (!Found->second.isValid())
      return std::nullopt;
    return Found->second;
  };

  // Label the four values as 00, 10, 01, 11. They form an additive rectangle
  // precisely when opposite sums agree modulo the word width. No division is
  // involved, so even coefficients and wrapping sums follow the same rule.
  // There are 24 labelings and only six possible selectors: each joins two
  // of the four value groups. Cache those tables for this reading.
  std::array<unsigned, 4> Order{0, 1, 2, 3};
  do {
    if (!Budget.consume())
      return;
    const llvm::APInt &Base = Values[Order[0]];
    if (Base + Values[Order[3]] != Values[Order[1]] + Values[Order[2]])
      continue;
    if (!Budget.consume(Weights.size()))
      return;
    TruthTable Left = TruthTable::zero(Count);
    TruthTable Right = TruthTable::zero(Count);
    for (size_t K = 0; K < Weights.size(); ++K) {
      if (Weights[K] == Values[Order[1]] || Weights[K] == Values[Order[3]])
        Left.set(K);
      if (Weights[K] == Values[Order[2]] || Weights[K] == Values[Order[3]])
        Right.set(K);
    }
    if (!Budget.consume(2 * Count * Weights.size()))
      return;
    if (std::popcount(truthTableSupport(Left)) < 2 ||
        std::popcount(truthTableSupport(Right)) < 2)
      continue;
    auto L = selector(Left);
    if (!L)
      continue;
    auto R = selector(Right);
    if (!R)
      continue;
    if (!Budget.consume(Weights.size()))
      return;
    SymRef Form =
        Ctx.mkAdd({Ctx.mkConst(-Base),
                   Ctx.mkMul(Ctx.mkConst(Values[Order[1]] - Base), *L),
                   Ctx.mkMul(Ctx.mkConst(Values[Order[2]] - Base), *R)});
    if (readingCost(Ctx, Form) <= TermBudget && !llvm::is_contained(Out, Form))
      Out.push_back(Form);
  } while (std::next_permutation(Order.begin(), Order.end()));
}

SymRef cheapestOf(const SymContext &Ctx, llvm::ArrayRef<SymRef> Candidates) {
  SymRef Best;
  SymReadability BestCost;
  for (SymRef Candidate : Candidates) {
    SymReadability Cost = readingScore(Ctx, Candidate);
    if (!Best.isValid() || Cost < BestCost) {
      Best = Candidate;
      BestCost = Cost;
    }
  }
  return Best;
}

} // namespace neverd::symbolic::detail
