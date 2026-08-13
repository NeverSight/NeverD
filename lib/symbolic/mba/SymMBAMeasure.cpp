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
/// Three ways of writing the weights back out are tried, because none of them
/// is shortest for every function:
///
///   - *Constant*, when the weights agree.
///   - *Grouped*, `sum over distinct weights v of v * B_v` where `B_v` selects
///     the patterns carrying that weight.  This is what turns
///     `(x | y) - (x & y)` back into `x ^ y`.
///   - *Conjunction basis*, the weights inverted over the subset lattice.
///     This is what turns `(x ^ y) + 2 * (x & y)` back into `x + y`, which the
///     grouped form cannot: it would return the input.
///
/// They are offered in that order and the caller keeps the cheapest, so the
/// order they are appended in decides ties.
///
//===----------------------------------------------------------------------===//

#include "SymMBADetail.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

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

/// `sum over distinct weights v of v * B_v`, with `B_v` selecting the patterns
/// carrying that weight.  Exact because the minterms of one weight are
/// disjoint, so their union is a bitwise function like any other.
std::optional<SymRef> groupedForm(SymContext &Ctx,
                                  llvm::ArrayRef<llvm::APInt> Weights,
                                  llvm::ArrayRef<SymRef> Atoms,
                                  size_t TermBudget) {
  const auto NumAtoms = static_cast<unsigned>(Atoms.size());
  if (NumAtoms > kMaxTruthTableVars)
    return std::nullopt;

  // Ordered rather than hashed, so the terms come out the same way on every
  // run and a difference in output is a real change instead of a reshuffle.
  std::map<llvm::APInt, TruthTable, APIntLess> Groups;
  for (size_t K = 0; K < Weights.size(); ++K) {
    if (Weights[K].isZero())
      continue;
    Groups[Weights[K]] |= TruthTable(1) << K;
  }
  if (Groups.empty())
    return Ctx.mkZero(Ctx.width(Atoms[0]));
  if (Groups.size() > TermBudget)
    return std::nullopt;

  llvm::SmallVector<SymRef, 8> Terms;
  for (const auto &[Value, Table] : Groups)
    Terms.push_back(
        Ctx.mkMul(Ctx.mkConst(Value), synthesizeBitwise(Ctx, Table, Atoms)));
  return Ctx.mkAdd(Terms);
}

/// `sum over subsets S of c_S * AND(inputs in S)`, the conjunction basis.
///
/// The empty conjunction is the all-ones word, so its coefficient contributes
/// the constant term with the sign turned around.
std::optional<SymRef> conjunctionForm(SymContext &Ctx,
                                      std::vector<llvm::APInt> Coefficients,
                                      llvm::ArrayRef<SymRef> Atoms,
                                      size_t TermBudget) {
  const auto NumAtoms = static_cast<unsigned>(Atoms.size());
  const uint32_t Width = Ctx.width(Atoms[0]);
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

} // namespace

std::optional<size_t> cornerCount(size_t NumAtoms) {
  if (NumAtoms >= std::numeric_limits<size_t>::digits)
    return std::nullopt;
  return size_t(1) << NumAtoms;
}

std::vector<llvm::APInt> measure(const SymContext &Ctx, SymRef Body,
                                 llvm::ArrayRef<uint32_t> Atoms) {
  const uint32_t Width = Ctx.width(Body);
  const auto NumAtoms = static_cast<unsigned>(Atoms.size());
  const std::optional<size_t> Corners = cornerCount(NumAtoms);
  assert(Corners && "corner table does not fit the host address space");

  SymEvalPlan Plan(Ctx, Body);
  std::vector<llvm::APInt> Weights(*Corners);

  if (Plan.fitsU64()) {
    const uint64_t Ones =
        Width == 64 ? ~uint64_t(0) : (uint64_t(1) << Width) - 1;
    std::vector<uint64_t> Assignment(Ctx.numVars(), 0);
    for (size_t K = 0; K < *Corners; ++K) {
      for (unsigned J = 0; J < NumAtoms; ++J)
        Assignment[Atoms[J]] = (K >> J) & 1 ? Ones : 0;
      // The corner value is the negated weight, so negating is what turns a
      // reading into a weight.
      Weights[K] = -llvm::APInt(Width, Plan.evalU64(Assignment),
                                /*isSigned=*/false, /*implicitTrunc=*/true);
    }
    return Weights;
  }

  const llvm::APInt Zero(Width, 0);
  const llvm::APInt Ones = llvm::APInt::getAllOnes(Width);
  std::vector<llvm::APInt> Assignment(Ctx.numVars(), Zero);
  for (size_t K = 0; K < *Corners; ++K) {
    for (unsigned J = 0; J < NumAtoms; ++J)
      Assignment[Atoms[J]] = (K >> J) & 1 ? Ones : Zero;
    Weights[K] = -Plan.eval(Assignment);
  }
  return Weights;
}

size_t readingCost(const SymContext &Ctx, SymRef R) {
  return Ctx.readabilityCost(R);
}

bool agreeOnSamples(const SymContext &Ctx, SymRef A, SymRef B,
                    unsigned Samples) {
  SymEvalPlan PlanA(Ctx, A);
  SymEvalPlan PlanB(Ctx, B);

  std::vector<llvm::APInt> Assignment;
  Assignment.reserve(Ctx.numVars());
  for (size_t I = 0; I < Ctx.numVars(); ++I)
    Assignment.emplace_back(Ctx.varInfo(uint32_t(I)).Width, 0);

  std::mt19937_64 Rng(0x9E3779B97F4A7C15ull);
  for (unsigned S = 0; S < Samples; ++S) {
    for (size_t I = 0; I < Assignment.size(); ++I) {
      uint32_t W = Ctx.varInfo(uint32_t(I)).Width;
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
    if (PlanA.eval(Assignment) != PlanB.eval(Assignment))
      return false;
  }
  return true;
}

size_t termBudget(const SymContext &Ctx, SymRef E, const MBAOptions &Opts) {
  return Opts.AllowGrowth ? std::numeric_limits<size_t>::max()
                          : readingCost(Ctx, E);
}

void linearCandidates(SymContext &Ctx, std::vector<llvm::APInt> Weights,
                      llvm::ArrayRef<SymRef> Atoms, size_t TermBudget,
                      llvm::SmallVectorImpl<SymRef> &Out) {
  if (llvm::all_of(Weights,
                   [&](const llvm::APInt &W) { return W == Weights[0]; }))
    Out.push_back(Ctx.mkConst(-Weights[0]));
  if (std::optional<SymRef> Grouped =
          groupedForm(Ctx, Weights, Atoms, TermBudget))
    Out.push_back(*Grouped);
  // Handing the weights over rather than copying: at the widths and arity this
  // reaches, a copy would be tens of thousands of allocations.
  if (std::optional<SymRef> Conjunctions =
          conjunctionForm(Ctx, std::move(Weights), Atoms, TermBudget))
    Out.push_back(*Conjunctions);
}

SymRef cheapestOf(const SymContext &Ctx, llvm::ArrayRef<SymRef> Candidates) {
  SymRef Best;
  size_t BestCost = 0;
  for (SymRef Candidate : Candidates) {
    size_t Cost = readingCost(Ctx, Candidate);
    if (!Best.isValid() || Cost < BestCost) {
      Best = Candidate;
      BestCost = Cost;
    }
  }
  return Best;
}

} // namespace neverd::symbolic::detail
