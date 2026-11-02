//===- SymBitwise.cpp - Truth tables and bitwise synthesis ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements synthesis of a bitwise expression from its truth table.
///
/// Exact constructions, chosen by what the caller can
/// afford and then by which answer reads better:
///
///   - Up to \c BitwiseSynthesisLimits::MaxOptimalAtoms inputs, an exhaustive
///     search.  There are only 2^(2^3) = 256 functions of three inputs, so
///     relaxing every pairwise combination until no cost improves settles the
///     shortest form of all of them at once.  The table is built once and
///     reused.  Each further input squares the count and then squares the
///     work, which is why the default stops at three and the ceiling at four.
///
///   - Otherwise, two exact constructions are costed against each other.  A
///     cover by prime implicants, grown by merging adjacent product terms and
///     then selected greedily, is short for functions close to a union of
///     cubes.  An exclusive-or normal form, the table inverted over the subset
///     lattice in the two-element field, is short for functions close to a
///     parity.  Running only one of them would make the quality of the answer
///     depend on which family the obfuscator happened to use: a seven-input
///     parity is sixty-four seven-literal products one way and a single
///     exclusive-or of seven atoms the other.
///     Independent parity arms can also be removed from that normal form to
///     expose a kernel of at most three inputs for the cached exact recipes.
///
/// Both of the general constructions are polynomial in the number of patterns
/// the function is true on, which is itself exponential in the arity, so each
/// is costed before it is started and the whole of synthesis reports failure
/// rather than beginning a search that would not finish.  That failure is a
/// resource answer: the caller drops the candidate form and keeps the ones it
/// could afford.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymBitwise.h"

#include "Limits.h"
#include "SymMBADetail.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::symbolic {

namespace {

constexpr size_t kCostCeiling = std::numeric_limits<size_t>::max();

/// Saturating arithmetic, so an estimate that overflows reads as "more than
/// any budget" rather than wrapping into an affordable-looking number.  Every
/// affordability test in this file is a comparison against a budget, and a
/// wrapped estimate is exactly how one of those quietly says yes to work that
/// would not finish.
size_t saturatingAdd(size_t A, size_t B) {
  return B > kCostCeiling - A ? kCostCeiling : A + B;
}

size_t saturatingMul(size_t A, size_t B) {
  if (A == 0 || B == 0)
    return 0;
  return B > kCostCeiling / A ? kCostCeiling : A * B;
}

//===----------------------------------------------------------------------===//
// Exhaustive search over the small arities
//===----------------------------------------------------------------------===//

/// How one truth table was reached during the search.
struct Recipe {
  enum Kind : uint8_t { Unreached, Zero, Ones, Atom, Not, And, Or, Xor };
  Kind K = Unreached;
  /// Atom index for \c Atom; otherwise the operand tables, packed.
  uint32_t A = 0;
  uint32_t B = 0;
};

#include "SymBitwiseRecipes.inc"

constexpr unsigned kUnreachable = ~0u;

/// A lower bound on the work needed to settle the optimal table.
///
/// Every function is compared with every other function at least once before
/// the relaxation can be known to have reached a fixed point.  Charging that
/// first sweep up front keeps a request with no work budget from triggering a
/// lazily-built table, especially the four-input table that is intentionally
/// expensive.
size_t optimalTableWork(unsigned NumVars) {
  assert(NumVars <=
         ::neverd::symbolic::detail::limits::kSymBitwiseMaxTabulatedAtoms);
  if (NumVars < std::size(SmallRecipeMaxNodes))
    return (size_t(1) << NumVars) * NumVars + 2 * SmallRecipeMaxNodes[NumVars] +
           NumVars + 2;
  const size_t NumFunctions = size_t(1) << (size_t(1) << NumVars);
  return saturatingMul(NumFunctions, NumFunctions);
}

/// The shortest expression for every boolean function of a given small arity.
///
/// Recipes minimize printed tree nodes over independent unit-cost atoms,
/// breaking node-count ties by operations. Restoring compound atoms can change
/// their costs, so completed candidates are still compared with actual scores.
class SynthTable {
public:
  explicit SynthTable(unsigned NumVars);
  explicit SynthTable(llvm::ArrayRef<Recipe> Prepared)
      : Recipes(Prepared.begin(), Prepared.end()) {}

  const Recipe &recipe(uint32_t Packed) const { return Recipes[Packed]; }

private:
  void relax(uint32_t T, unsigned Cost, unsigned OpCount, Recipe R) {
    if (Cost > Costs[T] || (Cost == Costs[T] && OpCount >= Operations[T]))
      return;
    Costs[T] = Cost;
    Operations[T] = OpCount;
    Recipes[T] = R;
    Improved = true;
  }

  std::vector<Recipe> Recipes;
  std::vector<unsigned> Costs;
  std::vector<unsigned> Operations;
  bool Improved = false;
};

SynthTable::SynthTable(unsigned NumVars) {
  const auto Mask = static_cast<uint32_t>(TruthTable::ones(NumVars).packed());
  const uint32_t Count = Mask + 1;
  Costs.assign(Count, kUnreachable);
  Operations.assign(Count, kUnreachable);
  Recipes.assign(Count, Recipe{});

  relax(0, 1, 0, Recipe{Recipe::Zero, 0, 0});
  relax(Mask, 1, 0, Recipe{Recipe::Ones, 0, 0});
  for (unsigned J = 0; J < NumVars; ++J)
    relax(static_cast<uint32_t>(atomTruthTable(J, NumVars).packed()), 1, 0,
          Recipe{Recipe::Atom, J, 0});

  // Relax to a fixed point.  Costs only ever fall and are bounded below, so
  // this terminates; at 256 functions the handful of passes it takes is not
  // worth a priority queue.
  for (Improved = true; Improved;) {
    Improved = false;
    for (uint32_t A = 0; A < Count; ++A) {
      if (Costs[A] == kUnreachable)
        continue;
      relax(~A & Mask, Costs[A] + 1, Operations[A] + 1,
            Recipe{Recipe::Not, A, 0});
      for (uint32_t B = A + 1; B < Count; ++B) {
        if (Costs[B] == kUnreachable)
          continue;
        unsigned Cost = Costs[A] + Costs[B] + 1;
        unsigned OpCount = Operations[A] + Operations[B] + 1;
        relax(A & B, Cost, OpCount, Recipe{Recipe::And, A, B});
        relax(A | B, Cost, OpCount, Recipe{Recipe::Or, A, B});
        relax(A ^ B, Cost, OpCount, Recipe{Recipe::Xor, A, B});
      }
    }
  }
}

const SynthTable &synthTable(unsigned NumVars) {
  assert(NumVars <=
         ::neverd::symbolic::detail::limits::kSymBitwiseMaxTabulatedAtoms);
  if (NumVars ==
      ::neverd::symbolic::detail::limits::kSymBitwiseMaxTabulatedAtoms) {
    // Built only when a caller has asked for four-input optimality, because
    // building it is minutes where the smaller arities are microseconds.  A
    // caller that never raises the dial must not pay for the possibility.
    static const SynthTable Widest(
        ::neverd::symbolic::detail::limits::kSymBitwiseMaxTabulatedAtoms);
    return Widest;
  }
  // Built once on first use and shared thereafter: the search is cheap but not
  // free, and the simplifier calls into it in a loop.
  static const SynthTable Tables[] = {
      SynthTable(SmallRecipes0), SynthTable(SmallRecipes1),
      SynthTable(SmallRecipes2), SynthTable(SmallRecipes3)};
  return Tables[NumVars];
}

// Every prepared recipe has at most eight distinct nodes. Keep their memo
// inline; the optional four-input search can still grow it when needed.
using RecipeMemo = llvm::SmallDenseMap<uint32_t, SymRef, 16>;

SymRef buildRecipe(SymContext &Ctx, const SynthTable &Table, uint32_t Packed,
                   llvm::ArrayRef<SymRef> Atoms, uint32_t Width,
                   RecipeMemo &Memo) {
  auto It = Memo.find(Packed);
  if (It != Memo.end())
    return It->second;

  const Recipe &R = Table.recipe(Packed);
  SymRef Result;
  switch (R.K) {
  case Recipe::Zero:
    Result = Ctx.mkZero(Width);
    break;
  case Recipe::Ones:
    Result = Ctx.mkOnes(Width);
    break;
  case Recipe::Atom:
    Result = Atoms[R.A];
    break;
  case Recipe::Not:
    Result = Ctx.mkNot(buildRecipe(Ctx, Table, R.A, Atoms, Width, Memo));
    break;
  case Recipe::And:
  case Recipe::Or:
  case Recipe::Xor: {
    SymRef L = buildRecipe(Ctx, Table, R.A, Atoms, Width, Memo);
    SymRef Rhs = buildRecipe(Ctx, Table, R.B, Atoms, Width, Memo);
    Result = R.K == Recipe::And  ? Ctx.mkAnd(L, Rhs)
             : R.K == Recipe::Or ? Ctx.mkOr(L, Rhs)
                                 : Ctx.mkXor(L, Rhs);
    break;
  }
  case Recipe::Unreached:
    llvm_unreachable("every function of this arity is reachable");
  }
  Memo[Packed] = Result;
  return Result;
}

/// Independent free inputs need one leaf each and binary joins connecting
/// them. Repeated or compound inputs may be related and collapse after
/// substitution, so only a literal's cost is a sound bound in that case.
/// The separate table-work gate still runs before this inexpensive check.
size_t minimumRecipeCost(const SymContext &Ctx, llvm::ArrayRef<SymRef> Atoms) {
  if (Atoms.empty())
    return 1;
  for (size_t I = 0; I < Atoms.size(); ++I) {
    if (!Ctx.isVar(Atoms[I]))
      return 1;
    for (size_t J = 0; J < I; ++J)
      if (Atoms[I] == Atoms[J])
        return 1;
  }
  return saturatingAdd(Atoms.size(), Atoms.size() - 1);
}

//===----------------------------------------------------------------------===//
// What the atoms themselves cost
//===----------------------------------------------------------------------===//

/// The reading cost of each atom, and of its complement.
///
/// Synthesis is offered atoms that are whole expressions as often as it is
/// offered variables — a placeholder standing for a division, say — so costing
/// a candidate form by counting its literals would rank two forms by which one
/// mentions the cheap-looking atom.  Asking the context is exact and cached.
struct AtomCosts {
  llvm::SmallVector<size_t, 8> Plain;
  llvm::SmallVector<size_t, 8> Complemented;

  AtomCosts(const SymContext &Ctx, llvm::ArrayRef<SymRef> Atoms) {
    for (SymRef A : Atoms) {
      const size_t Cost = Ctx.readabilityCost(A);
      Plain.push_back(Cost);
      // These complements fold during construction without building a
      // candidate: double complement, literal complement, and != spelling.
      if (Ctx.op(A) == SymOp::Not)
        Complemented.push_back(Ctx.readabilityCost(Ctx.operand(A, 0)));
      else if (Ctx.isConst(A) || Ctx.op(A) == SymOp::Eq)
        Complemented.push_back(Cost);
      else
        Complemented.push_back(saturatingAdd(Cost, 1));
    }
  }
};

/// A flattened infix operator still prints one binary join between each
/// adjacent pair. A single part needs no join.
size_t joinCost(llvm::ArrayRef<size_t> Parts) {
  size_t Total = 0;
  for (size_t Part : Parts)
    Total = saturatingAdd(Total, Part);
  return Parts.empty() ? Total : saturatingAdd(Total, Parts.size() - 1);
}

//===----------------------------------------------------------------------===//
// Prime implicant cover
//===----------------------------------------------------------------------===//

/// A product term: \c Care says which inputs the term constrains, \c Value
/// says what it constrains them to.  Uncared positions of \c Value are zero,
/// which is what lets two implicants be compared for equality directly.
struct Implicant {
  uint32_t Value = 0;
  uint32_t Care = 0;
};

/// The minterms an implicant covers.
TruthTable coverage(Implicant P, unsigned NumVars) {
  TruthTable Covered = TruthTable::zero(NumVars);
  const auto Free = static_cast<uint32_t>(Covered.entries() - 1) & ~P.Care;
  // Only the uncared inputs vary. Visit the exact covered subcube instead
  // of scanning every assignment and rejecting the constrained ones.
  uint32_t Subset = Free;
  for (;;) {
    Covered.set(P.Value | Subset);
    if (!Subset)
      break;
    Subset = (Subset - 1) & Free;
  }
  return Covered;
}

/// Merge product terms that differ in exactly one constrained input, dropping
/// that input from the pair, until nothing can grow any further.  Whatever
/// survives a round un-merged is prime.
///
/// Deduplication is by hash rather than by scanning what has already been
/// grown.  The scan made a round cubic in the number of implicants, which is
/// only invisible while the arity keeps that number in the dozens — exactly
/// the assumption this file no longer makes.
LLVM_ATTRIBUTE_NOINLINE
std::vector<Implicant> primeImplicants(const TruthTable &Table,
                                       unsigned NumVars, size_t &Work,
                                       size_t Budget) {
  const auto Full = static_cast<uint32_t>((size_t(1) << NumVars) - 1);
  llvm::SmallVector<Implicant, 16> Current;
  for (size_t K = 0, E = Table.entries(); K < E; ++K)
    if (Table.at(K))
      Current.push_back(Implicant{static_cast<uint32_t>(K), Full});

  std::vector<Implicant> Primes;
  while (!Current.empty()) {
    Work = saturatingAdd(Work, saturatingMul(Current.size(), Current.size()));
    if (Work > Budget)
      return {};

    // Small cover rounds stay inline; larger tables retain ordinary growth.
    // Deduplication keeps both complete 32-bit fields in one integer key.
    llvm::SmallVector<char, 16> Merged(Current.size(), 0);
    llvm::SmallVector<Implicant, 16> Next;
    llvm::SmallDenseSet<uint64_t, 64> Seen;
    auto grow = [&](size_t I, size_t J, uint32_t Diff) {
      Merged[I] = Merged[J] = 1;
      Implicant Grown{Current[I].Value & ~Diff, Current[I].Care & ~Diff};
      if (Seen.insert((uint64_t(Grown.Care) << 32) | Grown.Value).second)
        Next.push_back(Grown);
    };
    // Every round removes exactly one cared bit. When indexing each term and
    // looking up its possible one-bit neighbors needs fewer probes than the
    // complete pair scan, visit only those exact neighbors. The original
    // quadratic work admission stays in force.
    const auto CareBits = std::popcount(Current.front().Care);
    if (Current.size() - 1 > 2 * size_t(CareBits + 1)) {
      auto key = [](uint32_t Value, uint32_t Care) {
        return (uint64_t(Care) << 32) | Value;
      };
      llvm::SmallDenseMap<uint64_t, size_t, 64> Positions;
      Positions.reserve(Current.size());
      for (size_t I = 0; I < Current.size(); ++I)
        Positions.try_emplace(key(Current[I].Value, Current[I].Care), I);
      llvm::SmallVector<size_t, 8> Neighbors;
      for (size_t I = 0; I < Current.size(); ++I) {
        Neighbors.clear();
        // Terms stay ordered by value: the initial minterms are ascending,
        // and each round emits the smaller value of a merge in (I, J) order.
        // Only a zero-to-one flip can reach a later term. The reverse lookup
        // would discover the same pair again and immediately discard it.
        for (uint32_t Rest = Current[I].Care & ~Current[I].Value; Rest;
             Rest &= Rest - 1) {
          const uint32_t Bit = Rest & -Rest;
          const auto It =
              Positions.find(key(Current[I].Value ^ Bit, Current[I].Care));
          if (It != Positions.end() && It->second > I)
            Neighbors.push_back(It->second);
        }
        // Preserve the old (I, J) discovery order: greedy cover ties must not
        // change merely because the lookup table uses a different traversal.
        std::sort(Neighbors.begin(), Neighbors.end());
        for (size_t J : Neighbors)
          grow(I, J, Current[I].Value ^ Current[J].Value);
      }
    } else {
      for (size_t I = 0; I < Current.size(); ++I) {
        for (size_t J = I + 1; J < Current.size(); ++J) {
          if (Current[I].Care != Current[J].Care)
            continue;
          const uint32_t Diff =
              (Current[I].Value ^ Current[J].Value) & Current[I].Care;
          if (Diff == 0 || (Diff & (Diff - 1)) != 0)
            continue;
          grow(I, J, Diff);
        }
      }
    }
    for (size_t I = 0; I < Current.size(); ++I)
      if (!Merged[I])
        Primes.push_back(Current[I]);
    Current = std::move(Next);
  }
  return Primes;
}

/// A cover of the function by product terms, and what writing it out would
/// cost.  Costing the cover before any node is built is what lets a caller
/// refuse a sixty-thousand-node form without paying to intern one first.
struct Cover {
  llvm::SmallVector<Implicant, 8> Products;
  size_t Cost = 0;
};

std::optional<Cover> sumOfProductsCover(const TruthTable &Table,
                                        const AtomCosts &Costs,
                                        size_t WorkBudget, size_t CostBudget) {
  const unsigned NumVars = Table.numVars();
  size_t Work = Table.entries();
  if (Work > WorkBudget)
    return std::nullopt;

  // An empty result here is a budget answer as much as a structural one: a
  // non-constant function always has primes, so nothing to cover with means
  // the merge rounds were stopped, and either way there is no cover to offer.
  std::vector<Implicant> Primes =
      primeImplicants(Table, NumVars, Work, WorkBudget);
  if (Primes.empty())
    return std::nullopt;

  // One coverage sweep per prime, kept rather than recomputed: the greedy loop
  // below asks each prime what it still covers on every round, and re-deriving
  // that is what turns a cover into a cubic search.
  Work = saturatingAdd(Work, saturatingMul(Primes.size(), Table.entries()));
  if (Work > WorkBudget)
    return std::nullopt;
  std::vector<TruthTable> Covers;
  Covers.reserve(Primes.size());
  for (Implicant P : Primes)
    Covers.push_back(coverage(P, NumVars));

  TruthTable Uncovered = Table;
  Cover Out;
  size_t Running = 0;
  llvm::SmallVector<size_t, 8> FactorCosts;
  while (!Uncovered.isZero()) {
    // Greedy: take whichever prime accounts for the most that is still
    // uncovered.  An exact cover would need a set-cover search, and the
    // difference is a term or two.
    size_t Best = Primes.size();
    size_t BestGain = 0;
    for (size_t I = 0; I < Primes.size(); ++I) {
      const size_t Gain = (Covers[I] & Uncovered).count();
      if (Gain > BestGain) {
        BestGain = Gain;
        Best = I;
      }
    }
    // The primes of a function cover every minterm of it, so failing to find
    // one that helps means the table and the cover have drifted apart.
    if (Best == Primes.size())
      return std::nullopt;

    Work = saturatingAdd(Work, Primes.size());
    if (Work > WorkBudget)
      return std::nullopt;

    const Implicant P = Primes[Best];
    Uncovered &= ~Covers[Best];
    Primes.erase(Primes.begin() + Best);
    Covers.erase(Covers.begin() + Best);

    FactorCosts.clear();
    for (unsigned J = 0; J < NumVars; ++J) {
      if (!(P.Care & (1u << J)))
        continue;
      FactorCosts.push_back((P.Value & (1u << J)) ? Costs.Plain[J]
                                                  : Costs.Complemented[J]);
    }
    // A product constraining nothing is a literal all-ones word.
    Running =
        saturatingAdd(Running, FactorCosts.empty() ? 1 : joinCost(FactorCosts));
    Out.Products.push_back(P);
    // Abandoning a cover that has already outgrown what it may replace is the
    // point of costing as it is built rather than after.
    if (Running > CostBudget)
      return std::nullopt;
  }

  Out.Cost = Out.Products.empty()
                 ? Running
                 : saturatingAdd(Running, Out.Products.size() - 1);
  if (Out.Cost > CostBudget)
    return std::nullopt;
  return Out;
}

SymRef buildCover(SymContext &Ctx, const Cover &C, llvm::ArrayRef<SymRef> Atoms,
                  uint32_t Width) {
  const auto NumVars = static_cast<unsigned>(Atoms.size());
  llvm::SmallVector<SymRef, 8> Products;
  for (Implicant P : C.Products) {
    llvm::SmallVector<SymRef, 8> Factors;
    for (unsigned J = 0; J < NumVars; ++J) {
      if (!(P.Care & (1u << J)))
        continue;
      Factors.push_back((P.Value & (1u << J)) ? Atoms[J] : Ctx.mkNot(Atoms[J]));
    }
    Products.push_back(Factors.empty() ? Ctx.mkOnes(Width)
                                       : Ctx.mkAnd(Factors));
  }
  if (Products.empty())
    return Ctx.mkZero(Width);
  return Ctx.mkOr(Products);
}

//===----------------------------------------------------------------------===//
// Exclusive-or normal form
//===----------------------------------------------------------------------===//

/// The conjunctions whose exclusive-or is \p Table, each named by the set of
/// inputs it conjoins.
///
/// Every boolean function is one such exclusive-or and only one, and the
/// coefficients are the table inverted over the subset lattice in the
/// two-element field — the same inversion the linear reading performs over the
/// integers to find its conjunction coefficients.  Subtracting one dimension
/// at a time is what makes it cost t * 2^t rather than the 3^t a direct walk
/// of submasks would.
///
/// The empty conjunction stands for the all-ones word.
llvm::SmallVector<uint32_t, 16> exclusiveOrTerms(const TruthTable &Table) {
  const unsigned NumVars = Table.numVars();
  const size_t Entries = Table.entries();
  TruthTable Coefficients = Table;
  for (unsigned J = 0; J < NumVars; ++J) {
    const size_t Bit = size_t(1) << J;
    for (size_t K = 0; K < Entries; ++K)
      if (K & Bit)
        Coefficients.setValue(K, Coefficients.at(K) ^ Coefficients.at(K ^ Bit));
  }

  llvm::SmallVector<uint32_t, 16> Terms;
  for (size_t K = 0; K < Entries; ++K)
    if (Coefficients.at(K))
      Terms.push_back(static_cast<uint32_t>(K));
  return Terms;
}

size_t exclusiveOrCost(llvm::ArrayRef<uint32_t> Terms, const AtomCosts &Costs,
                       unsigned NumVars, size_t Budget) {
  size_t Running = 0;
  size_t Nonconstant = 0;
  bool Complement = false;
  llvm::SmallVector<size_t, 8> FactorCosts;
  for (uint32_t Term : Terms) {
    if (!Term) {
      // mkXor consumes all-ones as a complement of the remaining expression.
      // It contributes one unary operation, rather than a literal and a join.
      Complement = true;
      continue;
    }
    ++Nonconstant;
    FactorCosts.clear();
    for (unsigned J = 0; J < NumVars; ++J)
      if (Term & (1u << J))
        FactorCosts.push_back(Costs.Plain[J]);
    Running = saturatingAdd(Running, joinCost(FactorCosts));
    if (Running > Budget)
      return kCostCeiling;
  }
  if (!Nonconstant)
    return 1; // Both zero and all-ones are literal leaves.
  return saturatingAdd(Running, Nonconstant - 1 + (Complement ? 1 : 0));
}

SymRef buildExclusiveOr(SymContext &Ctx, llvm::ArrayRef<uint32_t> Terms,
                        llvm::ArrayRef<SymRef> Atoms, uint32_t Width) {
  const auto NumVars = static_cast<unsigned>(Atoms.size());
  llvm::SmallVector<SymRef, 16> Parts;
  for (uint32_t Term : Terms) {
    llvm::SmallVector<SymRef, 8> Factors;
    for (unsigned J = 0; J < NumVars; ++J)
      if (Term & (1u << J))
        Factors.push_back(Atoms[J]);
    Parts.push_back(Factors.empty() ? Ctx.mkOnes(Width) : Ctx.mkAnd(Factors));
  }
  if (Parts.empty())
    return Ctx.mkZero(Width);
  return Ctx.mkXor(Parts);
}

/// Cost a cached small recipe before interning it. This upper bound keeps the
/// binary recipe's joins even when the builders would cancel them.
size_t recipeCost(const SynthTable &Table, uint32_t Packed,
                  llvm::ArrayRef<size_t> Atoms,
                  llvm::DenseMap<uint32_t, size_t> &Memo) {
  auto It = Memo.find(Packed);
  if (It != Memo.end())
    return It->second;
  const Recipe &R = Table.recipe(Packed);
  size_t Cost;
  switch (R.K) {
  case Recipe::Zero:
    Cost = 1;
    break;
  case Recipe::Ones:
    Cost = 1;
    break;
  case Recipe::Atom:
    Cost = Atoms[R.A];
    break;
  case Recipe::Not:
    Cost = saturatingAdd(1, recipeCost(Table, R.A, Atoms, Memo));
    break;
  case Recipe::And:
  case Recipe::Or:
  case Recipe::Xor:
    Cost = saturatingAdd(1, saturatingAdd(recipeCost(Table, R.A, Atoms, Memo),
                                          recipeCost(Table, R.B, Atoms, Memo)));
    break;
  case Recipe::Unreached:
    llvm_unreachable("every function of this arity is reachable");
  }
  Memo[Packed] = Cost;
  return Cost;
}

struct ParityKernel {
  llvm::SmallVector<unsigned, 3> Inputs;
  uint32_t Peeled = 0;
  uint32_t Packed = 0;
};

/// A variable appearing only as a singleton ANF term is an independent XOR
/// arm. Removing all such arms can leave a small function for which the exact
/// recipe is already available. Never enlarge the exhaustive search for this
/// candidate, even when a caller explicitly permits a larger table elsewhere.
std::optional<ParityKernel> parityKernel(llvm::ArrayRef<uint32_t> Terms,
                                         const AtomCosts &Costs,
                                         unsigned NumVars, size_t ANFWork,
                                         size_t BestCost,
                                         const BitwiseSynthesisLimits &Limits) {
  const unsigned Ceiling = std::min(Limits.MaxOptimalAtoms, 3u);
  if (Ceiling < 2 || ANFWork > Limits.MaxWork ||
      Terms.size() > Limits.MaxWork - ANFWork)
    return std::nullopt;
  size_t Work = ANFWork + Terms.size();
  auto charge = [&](size_t Amount) {
    if (Amount > Limits.MaxWork - Work)
      return false;
    Work += Amount;
    return true;
  };
  uint32_t Nonlinear = 0, Singletons = 0;
  for (uint32_t Term : Terms) {
    if (std::has_single_bit(Term)) {
      Singletons |= Term;
    } else {
      Nonlinear |= Term;
      if (std::popcount(Nonlinear) > Ceiling)
        return std::nullopt;
    }
  }
  ParityKernel Out;
  Out.Peeled = Singletons & ~Nonlinear;
  if (!Out.Peeled || !Nonlinear || !charge(NumVars))
    return std::nullopt;

  // Keep this optional construction within the estimated cost of the existing
  // candidates. With compound or related inputs this is a conservative search
  // gate, not a lower bound on every expression after substitution.
  const size_t Limit = std::min(BestCost, Limits.MaxCost);
  if (joinCost(Costs.Plain) > Limit)
    return std::nullopt;
  llvm::SmallVector<size_t, 3> KernelCosts;
  size_t PeeledCost = 0;
  for (unsigned I = 0; I < NumVars; ++I) {
    if (Nonlinear & (1u << I)) {
      Out.Inputs.push_back(I);
      KernelCosts.push_back(Costs.Plain[I]);
    } else if (Out.Peeled & (1u << I)) {
      PeeledCost = saturatingAdd(PeeledCost, Costs.Plain[I]);
    }
  }
  const auto Size = static_cast<unsigned>(Out.Inputs.size());
  const unsigned Entries = 1u << Size;
  // The ANF scan and tiny projection share the original construction's work
  // allowance. In particular, the kernel does not get a fresh optimal budget.
  if (!charge(saturatingMul(Entries, saturatingAdd(Terms.size(), Size))) ||
      !charge(optimalTableWork(Size)))
    return std::nullopt;
  for (unsigned K = 0; K < Entries; ++K) {
    uint32_t Original = 0;
    for (unsigned I = 0; I < Size; ++I)
      if (K & (1u << I))
        Original |= 1u << Out.Inputs[I];
    bool Value = false;
    for (uint32_t Term : Terms)
      if ((Original & Term) == Term)
        Value = !Value;
    if (Value)
      Out.Packed |= 1u << K;
  }
  // There are at most 256 recipes. Bound their cost memo and construction
  // together; recursion follows only these fixed small recipes, never a DAG
  // supplied by the caller.
  if (!charge(2 * (size_t(1) << Entries)))
    return std::nullopt;
  llvm::DenseMap<uint32_t, size_t> Memo;
  const size_t Cost = saturatingAdd(
      std::popcount(Out.Peeled),
      saturatingAdd(PeeledCost, recipeCost(synthTable(Size), Out.Packed,
                                           KernelCosts, Memo)));
  if (Cost > Limits.MaxCost || Cost > BestCost)
    return std::nullopt;
  return Out;
}

SymRef buildParityKernel(SymContext &Ctx, const ParityKernel &Kernel,
                         llvm::ArrayRef<SymRef> Atoms, uint32_t Width) {
  llvm::SmallVector<SymRef, 3> Core;
  for (unsigned I : Kernel.Inputs)
    Core.push_back(Atoms[I]);
  RecipeMemo Memo;
  llvm::SmallVector<SymRef, 8> Parts;
  Parts.push_back(buildRecipe(Ctx, synthTable(Core.size()), Kernel.Packed, Core,
                              Width, Memo));
  for (unsigned I = 0; I < Atoms.size(); ++I)
    if (Kernel.Peeled & (1u << I))
      Parts.push_back(Atoms[I]);
  return Ctx.mkXor(Parts);
}

//===----------------------------------------------------------------------===//
// Dropping the inputs the function ignores
//===----------------------------------------------------------------------===//

/// Restrict \p Table to the inputs listed in \p Kept, renumbering them densely.
/// Sound because the caller has already established that the dropped inputs
/// cannot change the result.
TruthTable projectTruthTable(const TruthTable &Table,
                             llvm::ArrayRef<unsigned> Kept) {
  const auto NumKept = static_cast<unsigned>(Kept.size());
  if (NumKept == Table.numVars())
    return Table;

  TruthTable Out = TruthTable::zero(NumKept);
  for (size_t P = 0, E = Out.entries(); P < E; ++P) {
    size_t Original = 0;
    for (unsigned I = 0; I < NumKept; ++I)
      if (P & (size_t(1) << I))
        Original |= size_t(1) << Kept[I];
    if (Table.at(Original))
      Out.set(P);
  }
  return Out;
}

} // namespace

TruthTable atomTruthTable(unsigned Index, unsigned NumVars) {
  const auto Entries = TruthTable::entryCount(NumVars);
  if (Index >= NumVars)
    return TruthTable(llvm::APInt::getZero(Entries));
  // The first six input columns repeat within each 64-bit storage word.
  // Higher columns alternate whole runs of words. Construct those exact
  // patterns without visiting and setting every individual truth-table bit.
  static constexpr std::array<uint64_t, 6> Patterns{
      0xaaaaaaaaaaaaaaaaULL, 0xccccccccccccccccULL, 0xf0f0f0f0f0f0f0f0ULL,
      0xff00ff00ff00ff00ULL, 0xffff0000ffff0000ULL, 0xffffffff00000000ULL};
  constexpr unsigned WordBits = std::numeric_limits<uint64_t>::digits;
  if (Index < Patterns.size()) {
    if (Entries <= WordBits)
      return TruthTable(llvm::APInt(Entries, Patterns[Index]));
    return TruthTable(
        llvm::APInt::getSplat(Entries, llvm::APInt(WordBits, Patterns[Index])));
  }
  auto Bits = llvm::APInt::getZero(Entries);
  const unsigned Run = 1u << Index;
  for (unsigned Start = Run; Start < Entries; Start += 2 * Run)
    Bits.setBits(Start, Start + Run);
  return TruthTable(std::move(Bits));
}

uint32_t truthTableSupport(const TruthTable &Table) {
  const unsigned NumVars = Table.numVars();
  const size_t Entries = Table.entries();
  uint32_t Support = 0;
  for (unsigned J = 0; J < NumVars; ++J) {
    const size_t Bit = size_t(1) << J;
    for (size_t K = 0; K < Entries; ++K) {
      if (K & Bit)
        continue;
      if (Table.at(K) != Table.at(K | Bit)) {
        Support |= static_cast<uint32_t>(Bit);
        break;
      }
    }
  }
  return Support;
}

size_t detail::bitwiseSynthesisWork(unsigned NumVars) {
  return optimalTableWork(NumVars);
}

std::optional<SymRef> synthesizeBitwise(SymContext &Ctx,
                                        const TruthTable &Table,
                                        llvm::ArrayRef<SymRef> Atoms,
                                        const BitwiseSynthesisLimits &Limits) {
  assert(!Atoms.empty() && "synthesis needs at least one atom for its width");
  assert(Atoms.size() == Table.numVars() &&
         "the table's arity has to be the number of atoms it is written over");

  const uint32_t Width = Ctx.width(Atoms[0]);
  if (Limits.MaxCost < 1)
    return std::nullopt;
  if (Table.isZero())
    return Ctx.mkZero(Width);
  if (Table.isOnes())
    return Ctx.mkOnes(Width);

  // Neither constant, so at least one input matters.
  const uint32_t Support = truthTableSupport(Table);
  llvm::SmallVector<unsigned, 8> Kept;
  llvm::SmallVector<SymRef, 8> KeptAtoms;
  for (unsigned J = 0, E = Table.numVars(); J < E; ++J) {
    if (!(Support & (1u << J)))
      continue;
    Kept.push_back(J);
    KeptAtoms.push_back(Atoms[J]);
  }

  const TruthTable Projected = projectTruthTable(Table, Kept);
  const auto NumVars = static_cast<unsigned>(Kept.size());
  const unsigned OptimalCeiling = std::min(
      Limits.MaxOptimalAtoms,
      ::neverd::symbolic::detail::limits::kSymBitwiseMaxTabulatedAtoms);
  // A small function can fit the ordinary cover or ANF construction even
  // when the complete optimal recipe table would exceed this call's budget.
  // In particular, do not discard a recovered product's simple XOR relation
  // merely because its surrounding consumers have a small work allowance.
  if (NumVars <= OptimalCeiling &&
      optimalTableWork(NumVars) <= Limits.MaxWork) {
    if (minimumRecipeCost(Ctx, KeptAtoms) > Limits.MaxCost)
      return std::nullopt;

    const SynthTable &Table = synthTable(NumVars);
    const auto Packed = static_cast<uint32_t>(Projected.packed());
    RecipeMemo Memo;
    SymRef Result = buildRecipe(Ctx, Table, Packed, KeptAtoms, Width, Memo);
    if (Ctx.readabilityCost(Result) > Limits.MaxCost)
      return std::nullopt;
    return Result;
  }

  const AtomCosts Costs(Ctx, KeptAtoms);

  // Both constructions are exact, so this ranks two spellings of one function
  // rather than choosing what the answer means.  A tie goes to the cover,
  // which is the form a reader is more likely to recognise.
  std::optional<Cover> Sum =
      sumOfProductsCover(Projected, Costs, Limits.MaxWork, Limits.MaxCost);

  size_t ExclusiveCost = kCostCeiling;
  llvm::SmallVector<uint32_t, 16> Terms;
  if (saturatingMul(Projected.entries(), NumVars) <= Limits.MaxWork) {
    Terms = exclusiveOrTerms(Projected);
    ExclusiveCost = exclusiveOrCost(Terms, Costs, NumVars, Limits.MaxCost);
  }

  const size_t SumCost = Sum ? Sum->Cost : kCostCeiling;
  std::optional<ParityKernel> Kernel;
  if (!Terms.empty())
    Kernel = parityKernel(Terms, Costs, NumVars,
                          saturatingMul(Projected.entries(), NumVars),
                          std::min(SumCost, ExclusiveCost), Limits);
  SymRef Best;
  auto consider = [&](SymRef Candidate) {
    const SymReadability Score = Ctx.readability(Candidate);
    if (Score.Nodes > Limits.MaxCost)
      return;
    if (!Best.isValid() || Score < Ctx.readability(Best))
      Best = Candidate;
  };
  // Keep the existing estimate-based construction bound. Only a node-count
  // tie needs both forms to resolve their actual operation counts.
  if (SumCost <= ExclusiveCost && SumCost != kCostCeiling)
    consider(buildCover(Ctx, *Sum, KeptAtoms, Width));
  if (ExclusiveCost <= SumCost && ExclusiveCost != kCostCeiling &&
      ExclusiveCost <= Limits.MaxCost)
    consider(buildExclusiveOr(Ctx, Terms, KeptAtoms, Width));
  if (Kernel)
    consider(buildParityKernel(Ctx, *Kernel, KeptAtoms, Width));
  return Best.isValid() ? std::optional<SymRef>(Best) : std::nullopt;
}

} // namespace neverd::symbolic
