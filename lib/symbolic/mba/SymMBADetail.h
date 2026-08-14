//===- SymMBADetail.h - Shared MBA solver internals -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the pieces of the measure-and-rewrite loop that more than one of
/// the MBA translation units needs: the abstraction that puts an expression
/// over inputs the solver can drive, the corner measurement, the polynomial
/// expansion over minterms, the exact verifiers, and the resource budget all
/// of them spend from.
///
/// The stages run in a fixed order and several of them rank candidates by
/// cost with a strict comparison, so the first candidate offered wins a tie.
/// Anything that changes the order in which candidates are produced changes
/// which rewrite comes out, even though every candidate is individually
/// proved.
///
/// This header is an implementation detail of the symbolic library and should
/// not be included outside lib/symbolic/mba/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_MBA_SYMMBADETAIL_H
#define NEVERD_SYMBOLIC_MBA_SYMMBADETAIL_H

#include "neverd/symbolic/SymBitwise.h"
#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymMBA.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace neverd::symbolic::detail {

//===----------------------------------------------------------------------===//
// Resolving the public dials
//===----------------------------------------------------------------------===//

/// The most inputs the polynomial reading names minterms over.
///
/// A monomial is a list of minterm indices held in sixteen bits apiece, so a
/// product over seventeen inputs would name a minterm the key cannot hold and
/// would quietly name a different one instead.  That is a property of the key
/// rather than of the algebra: past it the polynomial reading declines and the
/// linear one still applies, which is a narrower answer rather than a wrong
/// one.
constexpr unsigned kMaxPolynomialAtoms = 16;

/// What the public option set means once the sentinels are resolved.
///
/// \c MBAOptions::Unlimited says "as far as the resources allow", and every
/// stage below needs a number instead — one that cannot overflow a shift count
/// and cannot ask for a table larger than the caller said it would hold.
/// Doing that resolution in one place is what keeps "unlimited" from meaning
/// something slightly different at each of the half-dozen sites that consult
/// it.
struct SolverLimits {
  /// Inputs one corner measurement may span.
  unsigned MaxAtoms = 0;
  /// Inputs a truth table may be built over.
  unsigned MaxSynthesisAtoms = 0;
  /// Inputs at which synthesis still returns the shortest form that exists.
  unsigned MaxOptimalSynthesisAtoms = 0;
  /// Operations one sweep of a 2^t table may spend, which also bounds what it
  /// holds: every table it keeps is charged at a bit an entry as it is built.
  ///
  /// Synthesis is the largest such sweep and names it, but the subset
  /// inversion behind the conjunction basis is the same size on the same
  /// table, and answering to the same ceiling is what stops raising the arity
  /// dial from raising the price of a reading nobody asked to be exhaustive.
  size_t SynthesisWork = 0;

  /// The limits to hand \c synthesizeBitwise, refusing anything that would
  /// read worse than \p MaxCost.
  BitwiseSynthesisLimits synthesis(size_t MaxCost) const {
    BitwiseSynthesisLimits Out;
    Out.MaxOptimalAtoms = MaxOptimalSynthesisAtoms;
    Out.MaxWork = SynthesisWork;
    Out.MaxCost = MaxCost;
    return Out;
  }
};

SolverLimits resolveLimits(const MBAOptions &Opts);

//===----------------------------------------------------------------------===//
// Traversal and classification
//===----------------------------------------------------------------------===//

/// Every node reachable from \p Root, in an order where each child precedes
/// its parent.  Interning appends a node only once its operands exist, so
/// ascending index order is such an order.
std::vector<uint32_t> reachableInOrder(const SymContext &Ctx, SymRef Root);

/// Whether measuring at \p R can do more than rebuilding its children.
///
/// Operators outside this set are opaque boundaries in both linear and
/// polynomial readings.  Their children have already been visited by the deep
/// walk, so running a region analysis at the boundary can only rediscover that
/// it is opaque.  Skipping it is what keeps a long chain of divisions, casts or
/// extracts linear in depth without hiding an MBA nested underneath.
bool canMeasureAtRoot(const SymContext &Ctx, SymRef R);

/// The expression rewritten over inputs the solver can drive, plus the map
/// that puts the hidden subterms back.
struct Abstraction {
  SymRef Body;
  /// Placeholder variable node index to the subterm it stands for.
  std::unordered_map<uint32_t, SymRef> Hidden;
};

/// Rewrite \p Root over inputs the solver can drive.
///
/// With \p AllowProducts a product of two bitwise terms survives as itself
/// rather than becoming one opaque input, which is what lets the degree-two
/// expansion further down see the factors it needs.
std::optional<Abstraction> abstractToMBA(SymContext &Ctx, SymRef Root,
                                         bool AllowProducts);

//===----------------------------------------------------------------------===//
// Measurement
//===----------------------------------------------------------------------===//

/// The 2^t corners a region of \p NumAtoms inputs is measured at, or nothing
/// when a table of that many is not built.
///
/// Every corner count in the solver comes from here rather than from a local
/// `1 << t`.  A shift by a count the type cannot hold is undefined behaviour,
/// and the arity dials are precisely the thing a caller is now encouraged to
/// raise, so the one place that turns an arity into a size is the one place
/// that has to be right.
std::optional<size_t> cornerCount(size_t NumAtoms);

/// Evaluate \p Body at every assignment of its inputs to all-zeros or
/// all-ones, and return the weights of its minterm decomposition.
std::vector<llvm::APInt> measure(const SymContext &Ctx, SymRef Body,
                                 llvm::ArrayRef<uint32_t> Atoms);

//===----------------------------------------------------------------------===//
// Ranking and checking
//===----------------------------------------------------------------------===//

/// What an expression costs whoever reads it.
///
/// Two things make this different from the size of the graph.
///
/// The first is sharing.  A subterm reachable by three paths is one node in
/// the graph and three appearances in anything that writes the expression out,
/// because writing it out is a walk of the tree the graph denotes.  Ranking by
/// graph size therefore calls an expression that says the same thing three
/// times cheaper than one that says two things once each, which is backwards.
/// So this counts the tree.
///
/// The second is the all-ones literal, which costs nothing.  It is never a
/// quantity: it is the sign of a negation — which is how negation is stored —
/// or the mask of a complement.  Charging for it would make `x - y` dearer
/// than `~(~x + y)` and rank the bitwise form above the arithmetic one this
/// exists to recover.
///
/// SymContext caches this per node.  Candidate generation appends nodes to the
/// context and asks for their costs repeatedly; extending one prefix cache
/// keeps all of those queries linear in the number of nodes built, while the
/// number returned still describes the tree a reader sees.
size_t readingCost(const SymContext &Ctx, SymRef R);

/// Compare two expressions at random points.
///
/// The derivation above is exact, so a disagreement means a mistake in it
/// rather than an expression the theory did not cover.  This is the net that
/// catches such a mistake before a wrong answer reaches a user.
bool agreeOnSamples(const SymContext &Ctx, SymRef A, SymRef B,
                    unsigned Samples);

/// The most terms a written-out form may have and still be worth building.
///
/// Every term of a sum contributes at least one node to the tree it is read
/// from, so a form with more terms than \p E has nodes cannot come out shorter
/// than \p E, and building it only for the size guard to reject it is wasted
/// work.  Deriving the bound from the expression at hand is what lets a large
/// input accept a large answer: a hundred-term form is a bad trade against ten
/// nodes and an excellent one against five thousand, and a fixed cap cannot
/// tell those apart.  Growth mode is measuring the solver rather than using it,
/// so nothing is withheld from it.
size_t termBudget(const SymContext &Ctx, SymRef E, const MBAOptions &Opts);

/// Every way of writing a set of minterm weights that the solver knows.
///
/// The constant, grouped and conjunction-basis forms are appended in that
/// order and the caller keeps the cheapest, so on a tie the earlier form wins.
void linearCandidates(SymContext &Ctx, std::vector<llvm::APInt> Weights,
                      llvm::ArrayRef<SymRef> Atoms, size_t TermBudget,
                      const SolverLimits &Limits,
                      llvm::SmallVectorImpl<SymRef> &Out);

SymRef cheapestOf(const SymContext &Ctx, llvm::ArrayRef<SymRef> Candidates);

//===----------------------------------------------------------------------===//
// Resource budget
//===----------------------------------------------------------------------===//

/// A bounded amount of semantic-simplification work.
///
/// The same counter covers graph traversal, corner measurements, proof
/// recomputation, product expansion, and factor recovery.  The latter two are
/// exponential in the expression, which is an algorithmic fact rather than a
/// reason to reject a particular degree.  Exhaustive mode passes the largest
/// size_t and therefore removes the budget without changing the representation
/// or the set of degrees it can express.
class WorkBudget {
public:
  explicit WorkBudget(size_t Limit)
      : Remaining(Limit),
        Unlimited(Limit == std::numeric_limits<size_t>::max()) {}

  bool consume(size_t Units = 1) {
    if (Unlimited) {
      Used = Units > std::numeric_limits<size_t>::max() - Used
                 ? std::numeric_limits<size_t>::max()
                 : Used + Units;
      return true;
    }
    if (Units > Remaining) {
      Used += Remaining;
      Remaining = 0;
      Exhausted = true;
      return false;
    }
    Remaining -= Units;
    Used += Units;
    return true;
  }

  bool exhausted() const { return Exhausted; }
  size_t used() const { return Used; }

private:
  size_t Remaining;
  size_t Used = 0;
  bool Unlimited;
  bool Exhausted = false;
};

//===----------------------------------------------------------------------===//
// Polynomials over the minterm basis
//===----------------------------------------------------------------------===//

/// One term of a polynomial: a coefficient and the factors it multiplies.  No
/// factors is a constant, one is a linear term, more is where measurement stops
/// working and the expansion takes over.
struct PolyTerm {
  llvm::APInt Coeff;
  llvm::SmallVector<SymRef, 4> Factors;
};

/// Split \p Body into terms the expansion covers, or nothing when some term is
/// of a higher degree than it does.
std::optional<llvm::SmallVector<PolyTerm, 8>>
splitIntoTerms(const SymContext &Ctx, SymRef Body);

/// A monomial of arbitrary degree: the minterms whose product it is, sorted.
///
/// The old packed integer key made degree a property of storage width.  A
/// vector key costs a small allocation only for higher-degree terms and lets
/// the resource budget, rather than an eight-byte container, decide how far a
/// search goes.
using Monomial = std::vector<uint16_t>;

Monomial makeMonomial(llvm::ArrayRef<uint16_t> Sorted);

unsigned monomialDegree(const Monomial &Key);

/// The minterms a monomial names, as a set over the 2^\p NumVars corners.
TruthTable monomialSupport(const Monomial &Key, unsigned NumVars);

/// An expression's coefficients over the minterm basis: the degree-one part
/// indexed by minterm, and every higher monomial keyed by its packing.
struct PolyForm {
  std::vector<llvm::APInt> Linear;
  std::map<Monomial, llvm::APInt> Higher;
  /// True when some term was a product, even if the products then cancelled.
  /// That is the difference between "linear all along", which the measurement
  /// already handled, and "quadratic and above but it came to nothing", which
  /// is a real answer the measurement could not have reached.
  bool SawProduct = false;
  /// Longest monomial still carrying a coefficient.
  unsigned Degree = 0;
};

/// The minterms \p Table selects, as indices.
llvm::SmallVector<uint16_t, 8> selectedMinterms(const TruthTable &Table);

std::optional<PolyForm> expandOverMinterms(const SymContext &Ctx,
                                           llvm::ArrayRef<PolyTerm> Terms,
                                           llvm::ArrayRef<uint32_t> AtomIds,
                                           uint32_t Width, WorkBudget &Budget);

/// Walk every way of taking one minterm from each factor, handing each
/// resulting monomial to \p Visit.  Stops early when \p Visit returns false.
///
/// This is the whole of multiplying the factors out: a product of sums is the
/// sum over one choice per factor, and the minterms chosen name the monomial
/// however they were ordered.
template <typename FnT>
void forEachMonomial(llvm::ArrayRef<llvm::SmallVector<uint16_t, 8>> Selected,
                     WorkBudget &Budget, FnT Visit) {
  if (Selected.empty())
    return;
  llvm::SmallVector<size_t, 4> Pick(Selected.size(), 0);
  for (;;) {
    if (!Budget.consume())
      return;

    Monomial Key;
    Key.reserve(Selected.size());
    for (size_t J = 0; J < Selected.size(); ++J)
      Key.push_back(Selected[J][Pick[J]]);
    llvm::sort(Key);
    if (!Visit(Key))
      return;

    size_t J = Selected.size();
    while (J > 0) {
      if (++Pick[J - 1] < Selected[J - 1].size())
        break;
      Pick[J - 1] = 0;
      --J;
    }
    if (J == 0)
      return;
  }
}

//===----------------------------------------------------------------------===//
// Exact verifiers
//===----------------------------------------------------------------------===//

/// Prove \p Before and \p After equal as linear MBA expressions.
///
/// This is deliberately separate from candidate synthesis.  The synthesizer
/// measures \p Before and invents a compact spelling from the weights; the
/// verifier instead abstracts their difference and accepts only when every
/// coefficient of that difference is zero.  It never asks which spelling was
/// chosen and therefore cannot bless a candidate merely because the code that
/// produced it says so.
bool proveLinearIdentity(SymContext &Ctx, SymRef Before, SymRef After,
                         unsigned MaxAtoms, WorkBudget &Budget);

/// Prove \p Before and \p After equal as sparse polynomials over bitwise
/// minterms.
///
/// Product matching is intentionally absent here.  Both sides are subtracted,
/// expanded independently of whatever factor search produced \p After, and the
/// rewrite is accepted only when no linear or higher-degree coefficient
/// remains.
bool provePolynomialIdentity(SymContext &Ctx, SymRef Before, SymRef After,
                             unsigned MaxAtoms, WorkBudget &Budget);

//===----------------------------------------------------------------------===//
// Solving
//===----------------------------------------------------------------------===//

/// Rewrite \p Body as one product plus a linear remainder, when it is of that
/// shape.
std::optional<SymRef> solvePolynomial(SymContext &Ctx, SymRef Body,
                                      llvm::ArrayRef<uint32_t> AtomIds,
                                      llvm::ArrayRef<SymRef> Atoms,
                                      const MBAOptions &Opts,
                                      WorkBudget &Budget);

/// What an attempt on a region found, beyond the expression it returns.
struct SolveReport {
  unsigned NumAtoms = 0;
  MBAOutcome Outcome = MBAOutcome::NotApplicable;
  MBAEvidence Evidence = MBAEvidence::None;
  /// Set when a reading was refused for naming more inputs than the budget
  /// allows.  That is the one refusal a larger budget would undo, so it is
  /// worth telling apart from having nothing to measure at all.
  bool TooWide = false;
  /// Set when any traversal, measurement, proof, or product search stopped at
  /// MaxWork.  Unlike an unsupported shape, a larger budget can change this
  /// answer.
  bool BudgetExhausted = false;
};

/// Measure \p E as one region, then in independent parts when it is too wide.
/// This is the mask-free half of the region solver; \c solveMasked reduces to
/// it once it has split a masked expression into mask-free columns, so keeping
/// it separate is what stops the mask split from recursing into itself.
SymRef solveOneRegion(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                      WorkBudget &Budget, SolveReport &Rep);

} // namespace neverd::symbolic::detail

#endif // NEVERD_SYMBOLIC_MBA_SYMMBADETAIL_H
