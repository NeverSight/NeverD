//===- SymMBAProduct.cpp - Recovering a product from its expansion --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the search for a product of bitwise factors whose expansion over
/// the minterm basis is exactly the higher-degree part SymMBAPoly.cpp computed,
/// and the rewrite of a region as one such product plus a linear remainder.
///
/// See the theory note at the head of SymMBAPoly.cpp for why the expansion pins
/// a product down where the corner measurement cannot.
///
//===----------------------------------------------------------------------===//

#include "SymMBADetail.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

namespace neverd::symbolic::detail {

namespace {

/// Multiply \p Factors out and count how often each monomial is reached, or
/// report that they cannot be the product being looked for.
///
/// Failing at the first monomial the target does not name is what keeps the
/// search far cheaper than its bound: almost every candidate is wrong, and
/// almost every wrong one is wrong on its first monomial.
bool expandProduct(llvm::ArrayRef<TruthTable> Factors,
                   const std::map<Monomial, llvm::APInt> &Higher,
                   std::map<Monomial, unsigned> &Counts, WorkBudget &Budget) {
  Counts.clear();
  llvm::SmallVector<llvm::SmallVector<uint16_t, 8>, 4> Selected;
  for (const TruthTable &Table : Factors) {
    Selected.push_back(selectedMinterms(Table));
    if (Selected.back().empty())
      return false;
  }

  bool Explains = true;
  forEachMonomial(Selected, Budget, [&](const Monomial &Key) {
    if (!Higher.count(Key)) {
      Explains = false;
      return false;
    }
    ++Counts[Key];
    return true;
  });
  // Reaching only monomials the target names is not enough; it has to reach all
  // of them, or the target holds something this product does not account for.
  return !Budget.exhausted() && Explains && Counts.size() == Higher.size();
}

/// Visit every product of \p Degree bitwise factors whose expansion is exactly
/// \p Higher, until \p Budget is spent.
///
/// The search is over factors rather than over all functions of the inputs,
/// because the target says a great deal about what the factors must be.  A
/// factor can only select a minterm the target names, and between them the
/// factors must name all of them; and a minterm repeated to the full degree is
/// a monomial exactly when *every* factor selects it, so those are known before
/// the search starts.  What is left to guess is a subset of the remainder,
/// which is what brings a degree-three product over three inputs -- the shape
/// polynomial obfuscation reaches for -- down from an unaffordable enumeration
/// to a few tens of thousands of candidates.
///
/// More than one product can match, and which reads best is not the order they
/// are found in, so the caller ranks every match reached within the budget.
/// Combination generation is iterative: product degree changes memory use, not
/// call-stack depth.
template <typename FnT>
void forEachProductMatch(const std::map<Monomial, llvm::APInt> &Higher,
                         unsigned Degree, unsigned NumAtoms, uint32_t Width,
                         size_t MaxCandidates, WorkBudget &Budget, FnT Visit) {
  const std::optional<size_t> Corners = cornerCount(NumAtoms);
  if (!Corners || Degree < 2)
    return;

  TruthTable Active = TruthTable::zero(NumAtoms);
  for (const auto &[Key, Coeff] : Higher)
    Active |= monomialSupport(Key, NumAtoms);

  TruthTable Shared = TruthTable::zero(NumAtoms);
  for (size_t M = 0; M < *Corners; ++M) {
    Monomial Repeat(Degree, static_cast<uint16_t>(M));
    if (Higher.count(Repeat))
      Shared.set(M);
  }

  llvm::SmallVector<TruthTable, 64> Candidates;
  const TruthTable Free = Active & ~Shared;
  for (TruthTable Sub = Free;; Sub = Sub.nextSubsetBelow(Free)) {
    TruthTable Table = Shared | Sub;
    if (!Table.isZero()) {
      if (!Budget.consume())
        return;
      // A candidate is a table of one bit per corner and the walk below is
      // over tuples of them, so a list nobody bounds is how a wide region
      // turns a search into an allocation failure rather than a long wait.
      if (Candidates.size() >= MaxCandidates)
        return;
      Candidates.push_back(std::move(Table));
    }
    if (Sub.isZero())
      break;
  }
  if (Candidates.empty())
    return;

  llvm::SmallVector<size_t, 4> Choice(Degree, 0);
  llvm::SmallVector<TruthTable, 4> Factors(Degree);
  std::map<Monomial, unsigned> Counts;
  // Nondecreasing tuples, because a product does not care what order its
  // factors are written in.
  for (;;) {
    if (!Budget.consume())
      return;
    TruthTable Union = TruthTable::zero(NumAtoms);
    for (size_t I = 0; I < Degree; ++I) {
      Factors[I] = Candidates[Choice[I]];
      Union |= Factors[I];
    }

    if (Union == Active && expandProduct(Factors, Higher, Counts, Budget)) {
      // Pin the coefficient on a monomial the product reaches exactly once.
      // Where every monomial is reached more than once no single coefficient
      // can be read off, and the shape is refused rather than guessed at.
      const llvm::APInt *Coeff = nullptr;
      for (const auto &[Key, Count] : Counts)
        if (Count == 1) {
          Coeff = &Higher.find(Key)->second;
          break;
        }
      bool Matches = Coeff && !Coeff->isZero();
      if (Matches) {
        for (const auto &[Key, Count] : Counts) {
          llvm::APInt Predicted =
              *Coeff * llvm::APInt(Width, Count, /*isSigned=*/false,
                                   /*implicitTrunc=*/true);
          if (Predicted != Higher.find(Key)->second) {
            Matches = false;
            break;
          }
        }
      }
      if (Matches && !Visit(*Coeff, Factors))
        return;
    }

    if (Budget.exhausted())
      return;

    size_t Pos = Degree;
    while (Pos > 0 && Choice[Pos - 1] + 1 == Candidates.size())
      --Pos;
    if (Pos == 0)
      return;
    const size_t Next = Choice[Pos - 1] + 1;
    for (size_t I = Pos - 1; I < Degree; ++I) {
      Choice[I] = Next;
    }
  }
}

} // namespace

std::optional<SymRef> solvePolynomial(SymContext &Ctx, SymRef Body,
                                      llvm::ArrayRef<uint32_t> AtomIds,
                                      llvm::ArrayRef<SymRef> Atoms,
                                      const MBAOptions &Opts,
                                      WorkBudget &Budget) {
  // Two ceilings meet here and they mean different things.  The caller's is a
  // budget: tabulating a factor over this many inputs is what it agreed to
  // pay for.  The other belongs to the monomial key, which names corners in
  // sixteen bits and would name the wrong one past that rather than fail.
  const SolverLimits Limits = resolveLimits(Opts);
  const auto NumAtoms = static_cast<unsigned>(AtomIds.size());
  if (NumAtoms > Limits.MaxSynthesisAtoms || NumAtoms > kMaxPolynomialAtoms)
    return std::nullopt;
  const std::optional<size_t> Corners = cornerCount(NumAtoms);
  if (!Corners)
    return std::nullopt;

  std::optional<llvm::SmallVector<PolyTerm, 8>> Terms =
      splitIntoTerms(Ctx, Body);
  if (!Terms)
    return std::nullopt;

  const uint32_t Width = Ctx.width(Body);
  std::optional<PolyForm> Form =
      expandOverMinterms(Ctx, *Terms, AtomIds, Width, Budget);
  // Without a product there is nothing here the linear measurement did not
  // already see.
  if (!Form || !Form->SawProduct)
    return std::nullopt;

  const size_t TermCeiling = termBudget(Ctx, Body, Opts);
  llvm::SmallVector<SymRef, 4> Linear;
  linearCandidates(Ctx, std::move(Form->Linear), Atoms, TermCeiling, Limits,
                   Linear);
  SymRef Remainder = cheapestOf(Ctx, Linear);
  if (!Remainder.isValid())
    return std::nullopt;

  // The products may have cancelled each other out, in which case what looked
  // like a polynomial is linear after all and the remainder is the whole
  // answer.  That is worth reaching: it is the shape an obfuscator leaves
  // behind when it multiplies a term out and adds the pieces back.
  if (Form->Higher.empty())
    return Remainder;

  // A product contributes monomials of exactly its own degree.  That used to
  // be the reason a target mixing degrees was refused; read the other way
  // round it is the reason it need not be.  The degrees cannot interact, so a
  // degree-three monomial can only have come from a degree-three product and a
  // degree-two one from a degree-two product, and matching each degree against
  // its own part of the target is a decomposition rather than a guess.  That
  // is what reaches the shape left behind when an obfuscator expands a square
  // and a cube into the same sum.
  std::map<unsigned, std::map<Monomial, llvm::APInt>> ByDegree;
  for (const auto &[Key, Coeff] : Form->Higher)
    ByDegree[monomialDegree(Key)].emplace(Key, Coeff);

  const BitwiseSynthesisLimits Synthesis = Limits.synthesis(TermCeiling);
  const size_t MaxCandidates =
      std::max<size_t>(1, Limits.SynthesisWork / *Corners);

  llvm::SmallVector<SymRef, 4> Parts;
  for (const auto &[Degree, Part] : ByDegree) {
    // More than one product can match, and which reads best is not the order
    // they are found in, so every match reached within the budget is ranked.
    // Ranking the product alone rather than the whole sum is the same order:
    // the remainder is the same term under every candidate.
    SymRef Best;
    size_t BestCost = 0;
    forEachProductMatch(
        Part, Degree, NumAtoms, Width, MaxCandidates, Budget,
        [&](const llvm::APInt &Coeff, llvm::ArrayRef<TruthTable> Match) {
          llvm::SmallVector<SymRef, 8> Factors;
          Factors.reserve(Match.size() + 1);
          Factors.push_back(Ctx.mkConst(Coeff));
          for (const TruthTable &Table : Match) {
            std::optional<SymRef> Written =
                synthesizeBitwise(Ctx, Table, Atoms, Synthesis);
            if (!Written)
              return true;
            Factors.push_back(*Written);
          }
          SymRef Candidate = Ctx.mkMul(Factors);
          const size_t Cost = readingCost(Ctx, Candidate);
          if (!Best.isValid() || Cost < BestCost) {
            Best = Candidate;
            BestCost = Cost;
          }
          return true;
        });
    // One unexplained degree means the whole reading is unexplained: dropping
    // it would return an expression missing a term rather than a shorter one.
    if (!Best.isValid())
      return std::nullopt;
    Parts.push_back(Best);
  }

  Parts.push_back(Remainder);
  return Ctx.mkAdd(Parts);
}

} // namespace neverd::symbolic::detail
