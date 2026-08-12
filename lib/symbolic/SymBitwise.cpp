//===- SymBitwise.cpp - Truth tables and bitwise synthesis ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements synthesis of a bitwise expression from its truth table.
///
/// Two strategies, chosen by how many inputs the function actually depends on:
///
///   - Three or fewer: an exhaustive search.  There are only 2^(2^3) = 256
///     functions of three inputs, so relaxing every pairwise combination until
///     no cost improves settles the shortest form of all of them at once.  The
///     table is built once and reused.
///
///   - Four to six: a cover by prime implicants, grown by merging adjacent
///     product terms and then selected greedily.  The search above is
///     quadratic in the number of functions, which is doubly exponential in
///     the arity, so it stops being affordable exactly here.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymBitwise.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/bit.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cassert>
#include <vector>

namespace neverd::symbolic {

namespace {

//===----------------------------------------------------------------------===//
// Exhaustive search over the small arities
//===----------------------------------------------------------------------===//

/// How one truth table was reached during the search.
struct Recipe {
  enum Kind : uint8_t { Unreached, Zero, Ones, Atom, Not, And, Or, Xor };
  Kind K = Unreached;
  /// Atom index for \c Atom; otherwise the operand tables.
  uint32_t A = 0;
  uint32_t B = 0;
};

constexpr unsigned kUnreachable = ~0u;

/// The shortest expression for every boolean function of a given small arity.
///
/// Cost counts nodes in a tree, which over-charges a shared subterm.  That
/// only affects the ranking between two forms of the same function, and at
/// these sizes the two measures agree on the winner.
class SynthTable {
public:
  explicit SynthTable(unsigned NumVars);

  const Recipe &recipe(TruthTable T) const { return Recipes[T]; }

private:
  void relax(uint32_t T, unsigned Cost, Recipe R) {
    if (Cost >= Costs[T])
      return;
    Costs[T] = Cost;
    Recipes[T] = R;
    Improved = true;
  }

  std::vector<Recipe> Recipes;
  std::vector<unsigned> Costs;
  bool Improved = false;
};

SynthTable::SynthTable(unsigned NumVars) {
  const auto Mask = static_cast<uint32_t>(truthTableMask(NumVars));
  const uint32_t Count = Mask + 1;
  Costs.assign(Count, kUnreachable);
  Recipes.assign(Count, Recipe{});

  relax(0, 1, Recipe{Recipe::Zero, 0, 0});
  relax(Mask, 1, Recipe{Recipe::Ones, 0, 0});
  for (unsigned J = 0; J < NumVars; ++J)
    relax(static_cast<uint32_t>(atomTruthTable(J, NumVars)), 1,
          Recipe{Recipe::Atom, J, 0});

  // Relax to a fixed point.  Costs only ever fall and are bounded below, so
  // this terminates; at 256 functions the handful of passes it takes is not
  // worth a priority queue.
  for (Improved = true; Improved;) {
    Improved = false;
    for (uint32_t A = 0; A < Count; ++A) {
      if (Costs[A] == kUnreachable)
        continue;
      relax(~A & Mask, Costs[A] + 1, Recipe{Recipe::Not, A, 0});
      for (uint32_t B = A + 1; B < Count; ++B) {
        if (Costs[B] == kUnreachable)
          continue;
        unsigned Cost = Costs[A] + Costs[B] + 1;
        relax(A & B, Cost, Recipe{Recipe::And, A, B});
        relax(A | B, Cost, Recipe{Recipe::Or, A, B});
        relax(A ^ B, Cost, Recipe{Recipe::Xor, A, B});
      }
    }
  }
}

const SynthTable &synthTable(unsigned NumVars) {
  assert(NumVars <= kMaxOptimalTruthTableVars);
  // Built once on first use and shared thereafter: the search is cheap but not
  // free, and the simplifier calls into it in a loop.
  static const SynthTable Tables[] = {SynthTable(0), SynthTable(1),
                                      SynthTable(2), SynthTable(3)};
  return Tables[NumVars];
}

SymRef buildRecipe(SymContext &Ctx, const SynthTable &Table, TruthTable T,
                   llvm::ArrayRef<SymRef> Atoms, uint32_t Width,
                   llvm::DenseMap<uint32_t, SymRef> &Memo) {
  auto Key = static_cast<uint32_t>(T);
  auto It = Memo.find(Key);
  if (It != Memo.end())
    return It->second;

  const Recipe &R = Table.recipe(T);
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
  Memo[Key] = Result;
  return Result;
}

//===----------------------------------------------------------------------===//
// Prime implicant cover for the larger arities
//===----------------------------------------------------------------------===//

/// A product term: \c Care says which inputs the term constrains, \c Value
/// says what it constrains them to.  Uncared positions of \c Value are zero,
/// which is what lets two implicants be compared for equality directly.
struct Implicant {
  uint32_t Value = 0;
  uint32_t Care = 0;

  friend bool operator==(Implicant A, Implicant B) {
    return A.Value == B.Value && A.Care == B.Care;
  }
};

/// The minterms an implicant covers, as a bitmask over input patterns.
uint64_t coverage(Implicant P, unsigned NumVars) {
  uint64_t Covered = 0;
  for (uint32_t K = 0, E = 1u << NumVars; K < E; ++K)
    if ((K & P.Care) == P.Value)
      Covered |= uint64_t(1) << K;
  return Covered;
}

/// Merge product terms that differ in exactly one constrained input, dropping
/// that input from the pair, until nothing can grow any further.  Whatever
/// survives a round un-merged is prime.
std::vector<Implicant> primeImplicants(TruthTable Table, unsigned NumVars) {
  const uint32_t Full = (1u << NumVars) - 1;
  std::vector<Implicant> Current;
  for (uint32_t K = 0, E = 1u << NumVars; K < E; ++K)
    if (truthTableAt(Table, K))
      Current.push_back(Implicant{K, Full});

  std::vector<Implicant> Primes;
  while (!Current.empty()) {
    std::vector<char> Merged(Current.size(), 0);
    std::vector<Implicant> Next;
    for (size_t I = 0; I < Current.size(); ++I) {
      for (size_t J = I + 1; J < Current.size(); ++J) {
        if (Current[I].Care != Current[J].Care)
          continue;
        uint32_t Diff = (Current[I].Value ^ Current[J].Value) & Current[I].Care;
        // Adjacent means differing in exactly one constrained input.
        if (Diff == 0 || (Diff & (Diff - 1)) != 0)
          continue;
        Merged[I] = Merged[J] = 1;
        Implicant Grown{Current[I].Value & ~Diff, Current[I].Care & ~Diff};
        if (!llvm::is_contained(Next, Grown))
          Next.push_back(Grown);
      }
    }
    for (size_t I = 0; I < Current.size(); ++I)
      if (!Merged[I])
        Primes.push_back(Current[I]);
    Current = std::move(Next);
  }
  return Primes;
}

SymRef buildSumOfProducts(SymContext &Ctx, TruthTable Table,
                          llvm::ArrayRef<SymRef> Atoms) {
  const auto NumVars = static_cast<unsigned>(Atoms.size());
  const uint32_t Width = Ctx.width(Atoms[0]);

  uint64_t Uncovered = 0;
  for (uint32_t K = 0, E = 1u << NumVars; K < E; ++K)
    if (truthTableAt(Table, K))
      Uncovered |= uint64_t(1) << K;

  std::vector<Implicant> Primes = primeImplicants(Table, NumVars);
  llvm::SmallVector<SymRef, 8> Products;
  while (Uncovered) {
    // Greedy: take whichever prime accounts for the most that is still
    // uncovered.  An exact cover would need a set-cover search, and at this
    // size the difference is a term or two.
    size_t Best = 0;
    unsigned BestGain = 0;
    for (size_t I = 0; I < Primes.size(); ++I) {
      auto Gain = static_cast<unsigned>(
          llvm::popcount(coverage(Primes[I], NumVars) & Uncovered));
      if (Gain > BestGain) {
        BestGain = Gain;
        Best = I;
      }
    }
    assert(BestGain > 0 && "the primes must between them cover every minterm");
    Implicant P = Primes[Best];
    Uncovered &= ~coverage(P, NumVars);
    Primes.erase(Primes.begin() + Best);

    llvm::SmallVector<SymRef, 6> Factors;
    for (unsigned J = 0; J < NumVars; ++J) {
      if (!(P.Care & (1u << J)))
        continue;
      Factors.push_back((P.Value & (1u << J)) ? Atoms[J]
                                              : Ctx.mkNot(Atoms[J]));
    }
    Products.push_back(Factors.empty() ? Ctx.mkOnes(Width)
                                       : Ctx.mkAnd(Factors));
  }

  if (Products.empty())
    return Ctx.mkZero(Width);
  return Ctx.mkOr(Products);
}

/// Restrict \p Table to the inputs listed in \p Kept, renumbering them densely.
/// Sound because the caller has already established that the dropped inputs
/// cannot change the result.
TruthTable projectTruthTable(TruthTable Table, llvm::ArrayRef<unsigned> Kept) {
  TruthTable Out = 0;
  for (uint32_t P = 0, E = 1u << Kept.size(); P < E; ++P) {
    uint32_t Original = 0;
    for (unsigned I = 0; I < Kept.size(); ++I)
      if (P & (1u << I))
        Original |= 1u << Kept[I];
    if (truthTableAt(Table, Original))
      Out |= TruthTable(1) << P;
  }
  return Out;
}

} // namespace

TruthTable atomTruthTable(unsigned Index, unsigned NumVars) {
  TruthTable Out = 0;
  for (uint32_t K = 0, E = 1u << NumVars; K < E; ++K)
    if ((K >> Index) & 1)
      Out |= TruthTable(1) << K;
  return Out;
}

unsigned truthTableSupport(TruthTable Table, unsigned NumVars) {
  unsigned Support = 0;
  for (unsigned J = 0; J < NumVars; ++J) {
    uint32_t Bit = 1u << J;
    for (uint32_t K = 0, E = 1u << NumVars; K < E; ++K) {
      if (K & Bit)
        continue;
      if (truthTableAt(Table, K) != truthTableAt(Table, K | Bit)) {
        Support |= Bit;
        break;
      }
    }
  }
  return Support;
}

SymRef synthesizeBitwise(SymContext &Ctx, TruthTable Table,
                         llvm::ArrayRef<SymRef> Atoms) {
  assert(!Atoms.empty() && "synthesis needs at least one atom for its width");
  assert(Atoms.size() <= kMaxTruthTableVars);

  const auto NumVars = static_cast<unsigned>(Atoms.size());
  const uint32_t Width = Ctx.width(Atoms[0]);
  Table &= truthTableMask(NumVars);

  if (Table == 0)
    return Ctx.mkZero(Width);
  if (Table == truthTableMask(NumVars))
    return Ctx.mkOnes(Width);

  // Neither constant, so at least one input matters.
  unsigned Support = truthTableSupport(Table, NumVars);
  llvm::SmallVector<unsigned, 6> Kept;
  llvm::SmallVector<SymRef, 6> KeptAtoms;
  for (unsigned J = 0; J < NumVars; ++J) {
    if (!(Support & (1u << J)))
      continue;
    Kept.push_back(J);
    KeptAtoms.push_back(Atoms[J]);
  }

  TruthTable Projected = projectTruthTable(Table, Kept);
  if (Kept.size() <= kMaxOptimalTruthTableVars) {
    llvm::DenseMap<uint32_t, SymRef> Memo;
    return buildRecipe(Ctx, synthTable(Kept.size()), Projected, KeptAtoms,
                       Width, Memo);
  }
  return buildSumOfProducts(Ctx, Projected, KeptAtoms);
}

} // namespace neverd::symbolic
