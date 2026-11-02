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
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace neverd::symbolic::detail {

class WorkBudget;

//===----------------------------------------------------------------------===//
// Resolving the public dials
//===----------------------------------------------------------------------===//

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

/// Work bound for the complete small recipe lookup, or the larger runtime
/// table construction when explicitly requested.
size_t bitwiseSynthesisWork(unsigned NumVars);

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
  /// A nonlinear product occurs in the source DAG. Without one, enabling
  /// product roles cannot change this reading or yield a polynomial candidate.
  bool MayHaveProducts = false;
  /// The same hidden inputs before recovering their affine relations. Keep
  /// this reading available when rebinding an arithmetic use would obscure
  /// its independent occurrence inside a bitwise operator.
  SymRef IndependentBody;
  /// Placeholder variable node index to the subterm it stands for.
  std::unordered_map<uint32_t, SymRef> Hidden;
};

/// Rewrite \p Root over inputs the solver can drive.
///
/// With \p AllowProducts a product of two bitwise terms survives as itself
/// rather than becoming one opaque input, which is what lets the degree-two
/// expansion further down see the factors it needs.
/// Optional affine recovery uses \p Budget and \p MaxBytes. Without a supplied
/// work budget it uses the default bounded policy; refusal keeps the original
/// abstraction rather than applying an incomplete relation.
std::optional<Abstraction>
abstractToMBA(SymContext &Ctx, SymRef Root, bool AllowProducts,
              WorkBudget *Budget = nullptr,
              size_t MaxBytes = MBAOptions{}.MaxTableBytes,
              bool CoarsenMaskedXors = false);

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

/// Read a corner without constructing an assignment for every variable in the
/// surrounding context. Values not selected by this region remain zero, just
/// as they do in the dense corner assignment.
class CornerEvaluator {
public:
  CornerEvaluator(const SymContext &Ctx, SymRef Body,
                  llvm::ArrayRef<uint32_t> Atoms)
      : Plan(Ctx, Body), Zero(Ctx.width(Body), 0),
        Ones(llvm::APInt::getAllOnes(Ctx.width(Body))) {
    assert(cornerCount(Atoms.size()) && "unrepresentable corner domain");
    for (unsigned J = 0; J < Atoms.size(); ++J)
      Positions[Atoms[J]] = J;
  }

  llvm::APInt eval(size_t Pattern) {
    auto IsSet = [&](uint32_t Id) {
      auto It = Positions.find(Id);
      return It != Positions.end() && ((Pattern >> It->second) & 1) != 0;
    };
    if (Plan.fitsU64()) {
      const uint64_t AllOnes = Ones.getZExtValue();
      return llvm::APInt(Zero.getBitWidth(), Plan.evalU64With([&](uint32_t Id) {
        return IsSet(Id) ? AllOnes : uint64_t(0);
      }));
    }
    return Plan.evalWith(
        [&](uint32_t Id) { return IsSet(Id) ? &Ones : &Zero; });
  }

private:
  SymEvalPlan Plan;
  llvm::APInt Zero, Ones;
  llvm::SmallDenseMap<uint32_t, unsigned, 8> Positions;
};

/// Evaluate \p Body at every assignment of its inputs to all-zeros or
/// all-ones, and return the weights of its minterm decomposition.
std::vector<llvm::APInt> measure(const SymContext &Ctx, SymRef Body,
                                 llvm::ArrayRef<uint32_t> Atoms);

//===----------------------------------------------------------------------===//
// Ranking and checking
//===----------------------------------------------------------------------===//

/// The node component of the context's cached printed-tree score, used by
/// candidate construction bounds; actual storage has its own byte budget.
/// Shared expressions count once per printed appearance;
/// signs and n-ary joins follow the same spelling rules as the printer.
size_t readingCost(const SymContext &Ctx, SymRef R);

/// Rank completed candidates by printed tree nodes, then printed operations.
SymReadability readingScore(const SymContext &Ctx, SymRef R);

/// Whether a completed candidate is no larger than its input. Saturated node
/// counts cannot establish this relation, even when both scores are saturated.
bool doesNotGrow(const SymContext &Ctx, SymRef Candidate, SymRef Input);

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
/// The constant, grouped, conjunction-basis and nested forms are appended in
/// that order and the caller keeps the cheapest, so on a tie the earlier wins.
void linearCandidates(SymContext &Ctx, std::vector<llvm::APInt> Weights,
                      llvm::ArrayRef<SymRef> Atoms, size_t TermBudget,
                      const SolverLimits &Limits,
                      llvm::SmallVectorImpl<SymRef> &Out);

/// Additional small linear readings obtained by subtracting one affine atom
/// and synthesizing its two-valued residual. Every scan and small-table lookup
/// spends the shared budget; callers prove the returned forms independently.
void affineResidualCandidates(SymContext &Ctx,
                              llvm::ArrayRef<llvm::APInt> Weights,
                              llvm::ArrayRef<SymRef> Atoms, size_t TermBudget,
                              const SolverLimits &Limits, WorkBudget &Budget,
                              llvm::SmallVectorImpl<SymRef> &Out,
                              bool *MayHaveBooleanPair = nullptr);

/// Four distinct weights can be the sums of two independently selected
/// coefficients and an offset. Only small, exactly synthesized selectors are
/// offered; the caller proves every form before restoring hidden inputs.
void booleanResidualCandidates(SymContext &Ctx,
                               llvm::ArrayRef<llvm::APInt> Weights,
                               llvm::ArrayRef<SymRef> Atoms, size_t TermBudget,
                               const SolverLimits &Limits, WorkBudget &Budget,
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
struct ArithmeticMemo;
struct RegionMemo;
struct AbstractionMemo;
struct ParityMemo;
struct ResidueMemo;

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
  bool unlimited() const { return Unlimited; }
  size_t available() const {
    return Unlimited ? std::numeric_limits<size_t>::max() : Remaining;
  }
  bool canConsume(size_t Units) const {
    return Unlimited || Units <= Remaining;
  }

  // Enabled only by the deep entry point, whose context outlives this budget.
  // The implementation retains completed arithmetic results, never refusals.
  const SymContext *MemoOwner = nullptr;
  size_t MemoCapacity = 0;
  std::shared_ptr<ArithmeticMemo> Arithmetic;
  std::shared_ptr<RegionMemo> Regions;
  std::shared_ptr<AbstractionMemo> Abstractions;
  std::shared_ptr<ParityMemo> Parities;
  // Optional search attempts, including declines. A declined attempt grants
  // no equality; it only avoids repeating the same search under fewer
  // resources.
  std::shared_ptr<ResidueMemo> Residues;
  size_t MultipleResidueAttempts = 0;
  size_t IncompleteAbstractions = 0;

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
/// The old packed integer key made degree a property of storage width. Small
/// inline storage avoids an allocation for common low-degree products while
/// still letting the resource budget decide how far a search goes.
using MintermIndex = uint32_t;
static_assert(kMaxTruthTableAtoms <= std::numeric_limits<MintermIndex>::digits,
              "a minterm index must address every truth-table entry");
using Monomial = llvm::SmallVector<MintermIndex, 4>;

struct MonomialHash {
  size_t operator()(const Monomial &Key) const {
    return static_cast<size_t>(
        llvm::hash_combine_range(Key.begin(), Key.end()));
  }
};

using HigherCoefficients =
    std::unordered_map<Monomial, llvm::APInt, MonomialHash>;

Monomial makeMonomial(llvm::ArrayRef<MintermIndex> Sorted);

unsigned monomialDegree(const Monomial &Key);

/// The minterms a monomial names, as a set over the 2^\p NumVars corners.
TruthTable monomialSupport(const Monomial &Key, unsigned NumVars);

/// An expression's coefficients over the minterm basis: the degree-one part
/// indexed by minterm, and every higher monomial keyed by its packing. The
/// product search orders factor candidates explicitly; coefficient iteration
/// order has no bearing on which rewrite wins.
struct PolyForm {
  std::vector<llvm::APInt> Linear;
  HigherCoefficients Higher;
  /// True when some term was a product, even if the products then cancelled.
  /// That is the difference between "linear all along", which the measurement
  /// already handled, and "quadratic and above but it came to nothing", which
  /// is a real answer the measurement could not have reached.
  bool SawProduct = false;
  /// Longest monomial still carrying a coefficient.
  unsigned Degree = 0;
};

/// The minterms \p Table selects, as indices.
llvm::SmallVector<MintermIndex, 8> selectedMinterms(const TruthTable &Table);

struct QuadraticProduct {
  llvm::APInt Coefficient;
  llvm::SmallVector<TruthTable, 2> Factors;
};

/// Remove the exact common factor multiset of a homogeneous term group.
/// An empty result leaves the group unchanged, including on optional refusal.
llvm::SmallVector<SymRef, 8>
extractPolynomialCofactor(llvm::SmallVectorImpl<PolyTerm> &Terms,
                          WorkBudget &Budget, size_t MaxBytes);

/// Recover the unique unordered pair of Boolean factors from a quadratic
/// coefficient matrix. Missing coefficients are zero modulo the word width.
std::optional<QuadraticProduct>
recoverQuadraticProduct(const HigherCoefficients &Higher, unsigned NumAtoms,
                        uint32_t Width, WorkBudget &Budget, size_t MaxBytes);

/// Recover a quadratic product by reading coefficient rows directly from its
/// terms. Determined distinguishes a disproved product from resource refusal.
/// For a single term, success preserves its coefficient and factor-table order.
std::optional<QuadraticProduct>
recoverQuadraticTerms(const SymContext &Ctx, llvm::ArrayRef<PolyTerm> Terms,
                      llvm::ArrayRef<uint32_t> AtomIds, uint32_t Width,
                      WorkBudget &Budget, size_t MaxBytes, bool &Determined);

/// Infer factor supports from all subset degrees. Nullopt declines the bounded
/// reconstruction; an empty vector disproves a single product. Nonempty support
/// is a candidate only: all coefficients still need independent validation.
std::optional<llvm::SmallVector<TruthTable, 4>>
recoverProductSupport(const HigherCoefficients &Higher, unsigned Degree,
                      unsigned NumAtoms, WorkBudget &Budget, size_t MaxBytes);

struct CubicProduct {
  llvm::APInt Coefficient;
  llvm::SmallVector<TruthTable, 3> Factors;
};

/// Recover a cubic product with a common minterm and a scale of additive
/// order at least four. When Determined is set, this anchor uniquely fixes
/// the factors (up to permutation): failure needs no exhaustive retry.
/// Other products remain eligible for bounded search.
std::optional<CubicProduct>
recoverCubicProduct(const HigherCoefficients &Higher, unsigned NumAtoms,
                    uint32_t Width, WorkBudget &Budget, size_t MaxBytes,
                    bool *Determined = nullptr);

/// Exact general expansion by default. SingleProduct additionally permits
/// rejecting incompatible extremal coefficients during candidate synthesis;
/// difference verifiers must leave it disabled.
std::optional<PolyForm> expandOverMinterms(const SymContext &Ctx,
                                           llvm::ArrayRef<PolyTerm> Terms,
                                           llvm::ArrayRef<uint32_t> AtomIds,
                                           uint32_t Width, WorkBudget &Budget,
                                           bool Optional = false,
                                           bool SingleProduct = false);

/// Walk every way of taking one minterm from each factor, handing each
/// resulting monomial to \p Visit.  Stops early when \p Visit returns false.
///
/// This is the whole of multiplying the factors out: a product of sums is the
/// sum over one choice per factor, and the minterms chosen name the monomial
/// however they were ordered.
template <typename FnT>
void forEachMonomial(
    llvm::ArrayRef<llvm::SmallVector<MintermIndex, 8>> Selected,
    WorkBudget &Budget, FnT Visit) {
  if (Selected.empty())
    return;
  llvm::SmallVector<size_t, 4> Pick(Selected.size(), 0);
  // The visitor borrows the sorted key only for this combination. Reuse its
  // storage across the Cartesian product, including degrees beyond inline
  // capacity, and keep comparison of scalar indices inside the sort.
  Monomial Key(Selected.size(), 0);
  for (;;) {
    if (!Budget.consume())
      return;

    for (size_t J = 0; J < Selected.size(); ++J)
      Key[J] = Selected[J][Pick[J]];
    std::sort(Key.begin(), Key.end());
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
                         unsigned MaxAtoms, WorkBudget &Budget,
                         size_t MaxBytes = MBAOptions{}.MaxTableBytes);

/// Prove \p Before and \p After equal as sparse polynomials over bitwise
/// minterms.
///
/// Product matching is intentionally absent here.  Both sides are subtracted,
/// expanded independently of whatever factor search produced \p After, and the
/// rewrite is accepted only when no linear or higher-degree coefficient
/// remains.
bool provePolynomialIdentity(SymContext &Ctx, SymRef Before, SymRef After,
                             unsigned MaxAtoms, WorkBudget &Budget,
                             size_t MaxBytes = MBAOptions{}.MaxTableBytes);

/// Certify modular arithmetic distribution with nonarithmetic words opaque.
/// Normalize both sides independently; incomplete normalization is refusal.
bool proveArithmeticIdentity(SymContext &Ctx, SymRef Before, SymRef After,
                             WorkBudget &Budget,
                             size_t MaxBytes = MBAOptions{}.MaxTableBytes,
                             bool ExpandComplements = true);

/// Certify an affine equality over bitwise views of every bit of one opaque
/// word, including literal AND/OR/XOR and NOT chains.
/// Opaque residual words must cancel exactly. Reject incomplete proofs;
/// sampling is not authority.
bool proveMaskedSumIdentity(const SymContext &Ctx, SymRef Before, SymRef After,
                            SymRef Carrier, WorkBudget &Budget,
                            size_t MaxBytes = MBAOptions{}.MaxTableBytes);

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

/// Combine weighted bitwise views by their per-bit coefficients and constants.
/// Independently prove each shorter group before composing the result.
SymRef solveWeightedMasks(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                          WorkBudget &Budget, SolveReport &Rep);

/// Normalize and factor Add/Mul regions over fixed-width words. Complements
/// may expand as -1-x or remain opaque alongside other operations. Resource
/// refusal leaves the region intact; both modes independently check the result.
SymRef solveArithmetic(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                       WorkBudget &Budget, SolveReport &Rep,
                       bool ExpandComplements = true);

/// A final coefficient reading, kept outside the repeated region walk.
SymRef solveCoefficientFactors(SymContext &Ctx, SymRef E,
                               const MBAOptions &Opts, WorkBudget &Budget,
                               SolveReport &Rep);

/// Fold small exact complement sums in the already selected answer.
SymRef completeComplementarySums(SymContext &Ctx, SymRef Root,
                                 const MBAOptions &Opts, WorkBudget &Budget);

/// Recognize direct and De Morgan complements in canonical bitwise nodes.
bool isDirectComplement(const SymContext &Ctx, SymRef A, SymRef B);
bool isBitwiseComplement(const SymContext &Ctx, SymRef A, SymRef B);

/// Complete a sum when its two terms partition a shared bit mask.
SymRef foldPartitionedMaskSum(SymContext &Ctx, SymRef A, SymRef B,
                              const llvm::APInt &Offset);

/// Factor complete immediate operands before other readings erase their
/// shared spelling; measure at most two quotients using the same budget.
SymRef solveStructuralFactors(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                              WorkBudget &Budget, SolveReport &Rep);

/// Recover higher products by proving small quotients under shared cofactors.
SymRef solveProductCofactors(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                             WorkBudget &Budget, SolveReport &Rep);

/// Recover a bounded product frontier after earlier arithmetic factoring.
SymRef solveNestedProductCofactors(SymContext &Ctx, SymRef E,
                                   const MBAOptions &Opts, WorkBudget &Budget,
                                   SolveReport &Rep);

/// Necessary multiplicity bounds only; never authorizes a replacement.
bool productCoefficientsFitScale(const HigherCoefficients &Higher,
                                 unsigned Degree, const llvm::APInt &Scale,
                                 WorkBudget &Budget);

/// Prove modular zero products from complete low-bit residue partitions of
/// factors sharing an additive base. Opaque bases need no independent inputs.
SymRef foldProductResidues(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                           size_t &Storage);

/// A complete lower bound over every low-bit residue, capped at Ceiling.
std::optional<unsigned>
minimumProductResidueZeros(llvm::MutableArrayRef<llvm::APInt> Offsets,
                           unsigned Width, unsigned Ceiling,
                           WorkBudget &Budget);

/// No value means the cheap affine hint declined; a value is a complete
/// result or the original product when proof resources were insufficient.
/// CompleteBase reports whether all factors share a proved full affine word,
/// even when none of its candidate readings improves the product.
std::optional<SymRef>
foldAffineProductResidues(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                          size_t &Storage,
                          std::optional<bool> ProjectedHint = std::nullopt,
                          bool *CompleteBase = nullptr);

/// Cheap admission only; no equality follows from this structural hint.
bool hasProjectedResidueFactors(const SymContext &Ctx, SymRef Root);

/// Prove full-word product zero from complete affine maps of low prefixes.
/// Admitted reuses a successful hint; it never authorizes a replacement.
SymRef foldProjectedProductResidues(SymContext &Ctx, SymRef Root,
                                    WorkBudget &Budget, size_t &Storage,
                                    bool Admitted = false);

/// Recover nonzero products from complete cofactor precision proofs after
/// the cheaper full-affine and projected-zero paths have declined.
SymRef foldResidualProductResidues(SymContext &Ctx, SymRef Root,
                                   WorkBudget &Budget, size_t &Storage);

/// Multiplicative period of odd words. Width is in [1, 8], as established by
/// the bounded residue readers before constructing or reducing a low word.
inline size_t residueUnitPeriod(unsigned Width) {
  return size_t(1) << (Width > 2 ? Width - 2 : Width - 1);
}

/// Complete bounded bit decisions over a product's observed low word.
/// Complete, when supplied, records a finished search over free variables;
/// it does not certify global minimality or an equivalence on its own.
SymRef foldResidueDecisions(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                            size_t &Storage, bool OddProductOnly = false,
                            size_t MaxWork = std::numeric_limits<size_t>::max(),
                            bool *StorageLimited = nullptr,
                            bool *Complete = nullptr);

/// Avoid rediscovering an emitted exact low-word reading. Original opaque
/// inputs still participate in the deep walk. The canonical builder propagates
/// their rewrites; changed original products remain eligible for analysis.
void recordResidueReading(SymContext &Ctx, size_t First, WorkBudget &Budget,
                          size_t &Storage);
bool hasCompletedResidueReading(const SymContext &Ctx, SymRef Root,
                                WorkBudget &Budget, bool GeneratedOnly = false);

/// Only the low product bit survives a highest-bit scale.
SymRef foldProductValuations(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                             size_t &Storage);

SymRef foldProductParity(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                         size_t &Storage);

/// Recognize a three-node reading whose free inputs rule out every cheaper
/// same-width expression. Shared by deep and post-arithmetic region checks.
bool isMinimalBinaryLeafExpression(const SymContext &Ctx, SymRef E,
                                   const MBAOptions &Opts);

/// Solve independent summand groups before measuring \p E as one region.
/// This is the mask-free half of the region solver; \c solveMasked reduces to
/// it once it has split a masked expression into mask-free columns, so keeping
/// it separate is what stops the mask split from recursing into itself.
SymRef solveOneRegion(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                      WorkBudget &Budget, SolveReport &Rep);

/// Recover an expanded quadratic/cubic root before local rewrites obscure its
/// independently partitioned factors. Uses the same polynomial verifier.
SymRef solveExpandedProducts(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                             WorkBudget &Budget, SolveReport &Rep);

} // namespace neverd::symbolic::detail

#endif // NEVERD_SYMBOLIC_MBA_SYMMBADETAIL_H
