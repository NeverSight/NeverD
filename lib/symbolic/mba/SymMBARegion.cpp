//===- SymMBARegion.cpp - Solving one MBA region --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the region solver: the linear and polynomial readings of one
/// expression, the ranking between them, and the split into groups of summands
/// that share no input before paying for a whole-region measurement.
///
/// Both readings run and the shorter proved answer wins.  Ranking uses a
/// strict comparison, so where two candidates cost the same the one produced
/// first is kept — which makes the order the candidates are appended in part
/// of the result.
///
//===----------------------------------------------------------------------===//

#include "Limits.h"
#include "SymMBADetail.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <map>
#include <optional>
#include <set>

namespace neverd::symbolic::detail {

// Bit patterns use stable input slots for this one request. Appending a slot
// leaves every older pattern unchanged: each already covers all 64 assignments.
struct ParityMemo {
  llvm::DenseMap<uint32_t, uint64_t> Values;
  llvm::DenseMap<uint32_t, unsigned> Inputs;
  llvm::DenseMap<uint32_t, SymRef> Products;
};

struct RegionMemo {
  struct Entry {
    SymRef Result;
    SolveReport Report;
    MBAOptions Options;
  };
  llvm::DenseMap<uint32_t, Entry> Entries;
};

// These forms already meet a three-node, one-operation reading bound. Two
// independent variables both need a leaf; a nontrivial constant transform is
// neither a literal, a variable, its complement nor its negation (the only
// cheaper same-width forms). Reject the coefficients and masks that can be
// one of those forms. Only actual variables establish this bound: opaque
// arithmetic inputs can be related and still need the complete region solver.
bool isMinimalBinaryLeafExpression(const SymContext &Ctx, SymRef E,
                                   const MBAOptions &Opts) {
  const SymOp Op = Ctx.op(E);
  if ((Op != SymOp::Add && Op != SymOp::Mul && Op != SymOp::And &&
       Op != SymOp::Or && Op != SymOp::Xor) ||
      Ctx.numOperands(E) != 2 || (Op == SymOp::Add && Ctx.width(E) <= 1))
    return false;
  SymRef A = Ctx.operand(E, 0), B = Ctx.operand(E, 1);
  const unsigned MaxAtoms = resolveLimits(Opts).MaxAtoms;
  if (Ctx.isVar(A) && Ctx.isVar(B))
    return A != B && MaxAtoms >= 2;
  if (Ctx.isVar(A))
    std::swap(A, B);
  if (!Ctx.isConst(A) || MaxAtoms < 1 || Ctx.isConstZero(A))
    return false;
  // A subtraction prints one operation too. C-X can only have a cheaper
  // unary spelling at C=0 (negation) or C=-1 (complement). The builder has
  // already removed a zero addend; retain complement recovery for the latter.
  if (Op == SymOp::Add && Ctx.op(B) == SymOp::Mul && Ctx.numOperands(B) == 2 &&
      Ctx.isConstOnes(Ctx.operand(B, 0)) && Ctx.isVar(Ctx.operand(B, 1)))
    return !Ctx.isConstOnes(A);
  if (!Ctx.isVar(B))
    return false;
  if (Op == SymOp::Mul)
    return !Ctx.constValue(A).isOne() && !Ctx.isConstOnes(A);
  return Op == SymOp::Add || !Ctx.isConstOnes(A);
}

namespace {

/// A rewrite, and how many inputs the reading that produced it was over.  The
/// count travels with the candidate because the two readings disagree about
/// it: the linear one counts a product of unknowns as one opaque input, the
/// polynomial one looks inside and counts its factors.  Reporting the larger of
/// the two would describe a reading that lost.
struct Candidate {
  SymRef Expr;
  unsigned NumAtoms = 0;
  /// True only after an exact verifier, separate from candidate synthesis, has
  /// established that \c Expr equals the region it may replace.
  bool Proven = false;
};

/// One reading of an expression: what it looks like over inputs the solver can
/// drive, and what those inputs are.
struct Region {
  Abstraction Abstract;
  llvm::SmallVector<uint32_t, 16> AtomIds;
  llvm::SmallVector<SymRef, 16> Atoms;
};

bool isMinimalUnaryVariable(const SymContext &Ctx, SymRef E) {
  // These forms cost two: one free-variable leaf and one operation. A cheaper
  // word expression is a literal or a free variable. Neither transform is
  // constant or identical to its input, and another independent variable
  // cannot replace it. Width-one negation has already folded to the variable.
  if (Ctx.op(E) == SymOp::Not)
    return Ctx.op(Ctx.operand(E, 0)) == SymOp::Var;
  return Ctx.width(E) > 1 && Ctx.op(E) == SymOp::Mul &&
         Ctx.numOperands(E) == 2 && Ctx.isConstOnes(Ctx.operand(E, 0)) &&
         Ctx.op(Ctx.operand(E, 1)) == SymOp::Var;
}

bool isMinimalVariableProduct(const SymContext &Ctx, SymRef E,
                              const SolverLimits &Limits) {
  if (Ctx.op(E) != SymOp::Mul || Ctx.numOperands(E) > Limits.MaxAtoms)
    return false;
  // Setting every other factor to one shows that each distinct free input
  // affects the product. It therefore needs all these leaves and the binary
  // operations connecting them, exactly this multiplication's printed cost.
  // Constants, repeated inputs and opaque factors do not establish the bound.
  SymRef Previous;
  for (SymRef Factor : Ctx.operands(E)) {
    // Canonical multiplication sorts its factors, so equal variables are
    // adjacent and checking uniqueness needs no allocation.
    if (Ctx.op(Factor) != SymOp::Var || Factor == Previous)
      return false;
    Previous = Factor;
  }
  return true;
}

bool isMinimalVariableSum(const SymContext &Ctx, SymRef E,
                          const SolverLimits &Limits) {
  if (Ctx.op(E) != SymOp::Add || Ctx.width(E) == 1)
    return false;
  llvm::ArrayRef<SymRef> Terms = Ctx.operands(E);
  if (Terms.size() > Limits.MaxAtoms)
    return false;
  // Canonical sums contain each variable at most once. Every input affects
  // the result, so an expression needs all these leaves and at least one
  // binary operation per additional leaf. This sum meets that lower bound.
  return std::all_of(Terms.begin(), Terms.end(),
                     [&](SymRef Term) { return Ctx.op(Term) == SymOp::Var; });
}

// A canonical affine sum already is its sparse conjunction-basis form:
// only the constant and singleton coefficients are nonzero. Read those
// coefficients structurally instead of allocating all 2^N Boolean corners.
// The exact body remains the candidate; restoring opaque inputs and the
// arithmetic pass retain coefficient factoring opportunities.
bool isCanonicalAffineSum(const SymContext &Ctx, SymRef R, size_t Atoms) {
  if (Atoms < limits::kMinCanonicalAffineAtoms || Ctx.op(R) != SymOp::Add)
    return false;
  return llvm::all_of(Ctx.operands(R), [&](SymRef Term) {
    return Ctx.isVar(Term) || Ctx.isConst(Term) ||
           (Ctx.op(Term) == SymOp::Mul && Ctx.numOperands(Term) == 2 &&
            Ctx.isConst(Ctx.operand(Term, 0)) &&
            Ctx.isVar(Ctx.operand(Term, 1)));
  });
}

std::optional<Region> readRegion(SymContext &Ctx, SymRef E,
                                 const MBAOptions &Opts, bool AllowProducts,
                                 WorkBudget &Budget, SolveReport &Rep,
                                 bool CoarsenMaskedXors = false) {
  std::optional<Abstraction> Abstract = abstractToMBA(
      Ctx, E, AllowProducts, &Budget, Opts.MaxTableBytes, CoarsenMaskedXors);
  if (!Abstract)
    return std::nullopt;

  Region Out;
  Out.Abstract = std::move(*Abstract);
  Ctx.collectVars(Out.Abstract.Body, Out.AtomIds);
  const unsigned MaxAtoms = resolveLimits(Opts).MaxAtoms;
  const bool TooWide =
      Out.AtomIds.size() > MaxAtoms || !cornerCount(Out.AtomIds.size());
  if (TooWide)
    Rep.TooWide = true;
  if (TooWide || (Out.AtomIds.empty() && !Ctx.isConst(Out.Abstract.Body)))
    return std::nullopt;
  for (uint32_t Id : Out.AtomIds)
    Out.Atoms.push_back(Ctx.varRef(Id));
  return Out;
}

// Reject a dense linear measurement only when every candidate consumer is
// already impossible. Different observed corner weights disprove a constant;
// too many distinct nonzero weights exceed groupedForm's exact group limit.
// The conjunction and tiny residual consumers have independent static guards.
// These observations are only rejection certificates, never equality proofs.
bool linearFormsCannotFit(const SymContext &Ctx, SymRef Body,
                          llvm::ArrayRef<uint32_t> AtomIds, size_t Corners,
                          size_t TermBudget, const SolverLimits &Limits,
                          WorkBudget &Budget, size_t &Storage) {
  const size_t Count = AtomIds.size();
  if (!Count || Budget.exhausted() || Corners <= Limits.SynthesisWork / Count ||
      Count <=
          std::min({limits::kMaxResidualSynthesisAtoms,
                    Limits.MaxOptimalSynthesisAtoms, Limits.MaxSynthesisAtoms}))
    return false;
  const bool Grouped = Count <= Limits.MaxSynthesisAtoms;
  const size_t GroupBudget =
      std::min(TermBudget, std::max<size_t>(1, Limits.SynthesisWork / Corners));
  // Inversion's rejected work bound implies GroupBudget < Count. Keep that
  // relation explicit before sizing a certificate from caller-controlled
  // limits.
  if (Grouped && GroupBudget >= Count)
    return false;
  const size_t Witnesses = Grouped ? GroupBudget + 2 : 2;
  const size_t Probes = std::min(Corners, std::bit_ceil(Witnesses));
  const size_t PrintedCost = readingCost(Ctx, Body);
  if (PrintedCost > SIZE_MAX / 2)
    return false;
  // Printed subtraction hides its minus-one coefficient and unary sign;
  // inequality can merge Not/Eq. Twice the printed tree bounds the actual
  // operator/leaf tree, and therefore both plan steps and operand edges.
  const size_t Cost = 2 * PrintedCost;
  const size_t Words = (size_t(Ctx.width(Body)) - 1) / 64 + 1;
  constexpr size_t NodeBytes = 256;
  const size_t WordBytes = sizeof(llvm::APInt) + Words * sizeof(uint64_t);
  if (Words > (SIZE_MAX - NodeBytes) / (4 * sizeof(uint64_t)) ||
      2 * Witnesses + 4 > SIZE_MAX / WordBytes ||
      Cost > SIZE_MAX - Witnesses - 3)
    return false;
  const size_t ContainerBytes =
      sizeof(CornerEvaluator) + sizeof(std::vector<llvm::APInt>);
  const size_t WitnessBytes = (2 * Witnesses + 4) * WordBytes;
  if (WitnessBytes > SIZE_MAX - ContainerBytes)
    return false;
  const size_t FixedBytes = ContainerBytes + WitnessBytes;
  // Cover the evaluator's lowering maps, pending edges, flat steps and wide
  // scratch before constructing it. The expanded tree bound covers reachable
  // nodes and edges; a shared subgraph only makes it more conservative.
  const size_t PerNodeBytes = NodeBytes + 4 * Words * sizeof(uint64_t);
  if (FixedBytes > Storage || Cost > (Storage - FixedBytes) / PerNodeBytes)
    return false;
  // Every probe executes the complete lowered plan. Include word-width work
  // for its steps, negation and all certificate comparisons, even if an early
  // witness finishes the exclusion before the planned probe count.
  const size_t ProbeTerms = Cost + Witnesses + 3;
  if (ProbeTerms > (SIZE_MAX - 1) / Words ||
      Probes > SIZE_MAX / (1 + ProbeTerms * Words))
    return false;
  const size_t ProbeWork = Probes * (1 + ProbeTerms * Words);
  if (Cost > (SIZE_MAX - ProbeWork) / 3 ||
      !Budget.canConsume(3 * Cost + ProbeWork))
    return false;
  Budget.consume(3 * Cost + ProbeWork);
  Storage -= FixedBytes + Cost * PerNodeBytes;
  CornerEvaluator Evaluator(Ctx, Body, AtomIds);
  std::vector<llvm::APInt> Nonzero;
  Nonzero.reserve(Witnesses);
  std::optional<llvm::APInt> First;
  bool Different = false;
  // Flipping an even number of free coordinates keeps the Gray-code shear
  // invertible; an odd count would collapse probes onto one parity plane.
  const size_t FoldMask = Corners - ((Count & 1) ? 2 : 4);
  for (size_t K = 0; K < Probes; ++K) {
    // Keep the first variable asserted while alternating the other axes
    // between distant corners. These are only rejection witnesses; failure
    // preserves the full reading.
    const size_t Pattern =
        ((((K ^ (K >> 1)) << 1) | 1) ^ ((K & 1) ? FoldMask : 0)) &
        (Corners - 1);
    auto Weight = -Evaluator.eval(Pattern);
    if (!First)
      First = Weight;
    else
      Different |= Weight != *First;
    if (!Weight.isZero() && !llvm::is_contained(Nonzero, Weight))
      Nonzero.push_back(std::move(Weight));
    if (Different && (!Grouped || Nonzero.size() > GroupBudget))
      return true;
  }
  return false;
}

/// Best form of \p E as a single region.
///
/// Two readings of the same expression are tried.  The linear one treats a
/// product of unknowns as opaque and measures everything around it; the
/// polynomial one keeps the product and expands it.  Neither subsumes the
/// other — the linear reading handles inputs the expansion cannot see inside
/// of, and the expansion handles products the measurement cannot read — so
/// both run and the shorter answer wins.
SymRef solveRegion(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                   WorkBudget &Budget, SolveReport &Rep) {
  // Generated cubic and higher queries are outside the deep walk. Project
  // their unobservable product bits before entering the polynomial search.
  // Two-factor quotients retain their basis for enclosing product recovery.
  auto Projectable = [&](SymRef R) {
    return Ctx.op(R) == SymOp::Mul && Ctx.numOperands(R) >= 4 &&
           Ctx.isConst(Ctx.operand(R, 0));
  };
  size_t ParityStorage = Opts.MaxTableBytes;
  SymRef Projected =
      Projectable(E) ? foldProductParity(Ctx, E, Budget, ParityStorage) : E;
  if (Ctx.op(E) == SymOp::Add &&
      Ctx.numOperands(E) <= ::neverd::symbolic::detail::limits::
                                kSymMBARegionSolveRegionCtxNumOperandsELimit &&
      llvm::any_of(Ctx.operands(E), Projectable)) {
    const size_t N = Ctx.numOperands(E);
    if (N <= ParityStorage / (2 * sizeof(SymRef)) && Budget.consume(N)) {
      ParityStorage -= N * 2 * sizeof(SymRef);
      llvm::SmallVector<SymRef, 8> Terms(Ctx.operands(E));
      bool Changed = false;
      for (SymRef &Term : Terms) {
        if (!Projectable(Term))
          continue;
        SymRef Next = foldProductParity(Ctx, Term, Budget, ParityStorage);
        Changed |= Next != Term;
        Term = Next;
      }
      if (Changed)
        Projected = Ctx.mkAdd(Terms);
    }
  }
  if (Projected != E) {
    MBAOptions Inner = Opts;
    Inner.MaxTableBytes = ParityStorage;
    SymRef Reduced = solveRegion(Ctx, Projected, Inner, Budget, Rep);
    Rep.Outcome = MBAOutcome::Rewritten;
    Rep.Evidence = MBAEvidence::Derivation;
    return Reduced;
  }
  const SolverLimits Limits = resolveLimits(Opts);
  if (isMinimalVariableSum(Ctx, E, Limits)) {
    Rep.Outcome = MBAOutcome::AlreadyShortest;
    return E;
  }
  llvm::SmallVector<Candidate, 6> Candidates;
  bool Measured = false;
  bool TryCoarseMasks = true;
  bool TryProducts = true;
  std::optional<Region> LinearRegion;
  std::vector<llvm::APInt> ResidualWeights;

  if (std::optional<Region> Linear =
          readRegion(Ctx, E, Opts, /*AllowProducts=*/false, Budget, Rep)) {
    Measured = true;
    TryProducts = Linear->Abstract.MayHaveProducts;
    auto NumAtoms = static_cast<unsigned>(Linear->AtomIds.size());
    TryCoarseMasks = NumAtoms > std::min(limits::kMaxResidualSynthesisAtoms,
                                         Limits.MaxOptimalSynthesisAtoms);
    std::optional<size_t> Corners = cornerCount(Linear->AtomIds.size());
    const bool Direct =
        isCanonicalAffineSum(Ctx, Linear->Abstract.Body, NumAtoms);
    const bool Excluded =
        !Direct && Corners &&
        linearFormsCannotFit(Ctx, Linear->Abstract.Body, Linear->AtomIds,
                             *Corners, termBudget(Ctx, E, Opts), Limits, Budget,
                             ParityStorage);
    if (!Excluded &&
        (!Corners ||
         !Budget.consume(Direct ? Ctx.numOperands(Linear->Abstract.Body)
                                : *Corners))) {
      Rep.BudgetExhausted = true;
    } else if (!Excluded) {
      llvm::SmallVector<SymRef, 4> Forms;
      // Exact abstraction can eliminate every input. The literal is already
      // the candidate; bitwise synthesis requires at least one atom. Keep the
      // one-corner budget charge and the same proof and selection gates below.
      if (Direct || Linear->AtomIds.empty())
        Forms.push_back(Linear->Abstract.Body);
      else {
        std::vector<llvm::APInt> Weights =
            measure(Ctx, Linear->Abstract.Body, Linear->AtomIds);
        // A tiny corner count can still hold very wide heap-backed APInts.
        // Reserve room for the retained weights, half sets, coefficients,
        // residuals and arithmetic temporaries before making the extra copy.
        // The conservative word count also covers their container overhead.
        const size_t Width = Ctx.width(Linear->Abstract.Body);
        const size_t WordBytes =
            sizeof(llvm::APInt) +
            (Width / 64 + (Width % 64 != 0)) * sizeof(uint64_t);
        const bool ResidualStorageFits = WordBytes <= Opts.MaxTableBytes / 64;
        if (NumAtoms >= 2 &&
            NumAtoms <= std::min({limits::kMaxResidualSynthesisAtoms,
                                  Limits.MaxOptimalSynthesisAtoms,
                                  Limits.MaxSynthesisAtoms}) &&
            ResidualStorageFits)
          ResidualWeights = Weights;
        linearCandidates(Ctx, std::move(Weights), Linear->Atoms,
                         termBudget(Ctx, E, Opts), Limits, Forms);
      }
      for (SymRef Form : Forms) {
        // Prove the identity over independent inputs before restoring their
        // sources. Restoration may combine coefficients and erase the shared
        // input spelling, but instantiating a proved identity remains exact.
        if (!proveLinearIdentity(Ctx, Linear->Abstract.Body, Form,
                                 Limits.MaxAtoms, Budget, Opts.MaxTableBytes))
          continue;
        SymRef Rewritten = Linear->Abstract.Hidden.empty()
                               ? Form
                               : Ctx.substitute(Form, Linear->Abstract.Hidden);
        Candidates.push_back({Rewritten, NumAtoms, true});
      }
      if (!ResidualWeights.empty() ||
          (Linear->Abstract.IndependentBody != Linear->Abstract.Body &&
           readingScore(Ctx, Linear->Abstract.IndependentBody) <
               readingScore(Ctx, Linear->Abstract.Body)))
        LinearRegion = std::move(Linear);
    } else if (Linear->Abstract.IndependentBody != Linear->Abstract.Body &&
               readingScore(Ctx, Linear->Abstract.IndependentBody) <
                   readingScore(Ctx, Linear->Abstract.Body)) {
      LinearRegion = std::move(Linear);
    }
  }

  // With no nonlinear product in the source, the second abstraction would
  // repeat the linear reading and solvePolynomial would immediately decline.
  // Reuse that structural fact instead of rebuilding and rescanning the DAG.
  if (std::optional<Region> Poly =
          TryProducts
              ? readRegion(Ctx, E, Opts, /*AllowProducts=*/true, Budget, Rep)
              : std::nullopt) {
    Measured = true;
    if (std::optional<SymRef> Form =
            solvePolynomial(Ctx, Poly->Abstract.Body, Poly->AtomIds,
                            Poly->Atoms, Opts, Budget)) {
      if (provePolynomialIdentity(Ctx, Poly->Abstract.Body, *Form,
                                  Limits.MaxAtoms, Budget,
                                  Opts.MaxTableBytes)) {
        SymRef Rewritten = Poly->Abstract.Hidden.empty()
                               ? *Form
                               : Ctx.substitute(*Form, Poly->Abstract.Hidden);
        Candidates.push_back(
            {Rewritten, static_cast<unsigned>(Poly->AtomIds.size()), true});
      }
    }
  }

  // Additional readings spend only what remains after the established linear
  // and polynomial candidates have been proved. Refusing the optional search
  // therefore cannot erase a candidate already available at this budget.
  if (TryCoarseMasks && !Budget.exhausted()) {
    if (auto Coarse = readRegion(Ctx, E, Opts, false, Budget, Rep, true)) {
      auto Corners = cornerCount(Coarse->AtomIds.size());
      const size_t Width = Ctx.width(Coarse->Abstract.Body);
      const size_t WordBytes =
          sizeof(llvm::APInt) +
          (Width / 64 + (Width % 64 != 0)) * sizeof(uint64_t);
      const bool Direct = isCanonicalAffineSum(Ctx, Coarse->Abstract.Body,
                                               Coarse->AtomIds.size());
      if (Corners &&
          (Direct || *Corners <= Opts.MaxTableBytes / 8 / WordBytes) &&
          (Direct ||
           !linearFormsCannotFit(Ctx, Coarse->Abstract.Body, Coarse->AtomIds,
                                 *Corners, termBudget(Ctx, E, Opts), Limits,
                                 Budget, ParityStorage)) &&
          Budget.consume(Direct ? Ctx.numOperands(Coarse->Abstract.Body)
                                : *Corners)) {
        llvm::SmallVector<SymRef, 4> Forms;
        if (Direct || Coarse->AtomIds.empty())
          Forms.push_back(Coarse->Abstract.Body);
        else
          linearCandidates(
              Ctx, measure(Ctx, Coarse->Abstract.Body, Coarse->AtomIds),
              Coarse->Atoms, termBudget(Ctx, E, Opts), Limits, Forms);
        for (SymRef Form : Forms) {
          if (!proveLinearIdentity(Ctx, Coarse->Abstract.Body, Form,
                                   Limits.MaxAtoms, Budget, Opts.MaxTableBytes))
            continue;
          if (!Budget.consume(Ctx.dagSize(Form)))
            break;
          Candidates.push_back({Ctx.substitute(Form, Coarse->Abstract.Hidden),
                                static_cast<unsigned>(Coarse->Atoms.size()),
                                true});
        }
      }
    }
  }
  if (LinearRegion && !Budget.exhausted() &&
      LinearRegion->Abstract.IndependentBody != LinearRegion->Abstract.Body &&
      readingScore(Ctx, LinearRegion->Abstract.IndependentBody) <
          readingScore(Ctx, LinearRegion->Abstract.Body)) {
    // An affine relation can shorten arithmetic while hiding a bitwise
    // identity that used the original independent inputs. Retain that one
    // reading when it is smaller, with the same opaque-input substitution.
    const Abstraction &Abstract = LinearRegion->Abstract;
    llvm::SmallVector<uint32_t, 16> AtomIds;
    if (Budget.consume(Ctx.dagSize(Abstract.IndependentBody))) {
      Ctx.collectVars(Abstract.IndependentBody, AtomIds);
      std::optional<size_t> Corners = cornerCount(AtomIds.size());
      const size_t Width = Ctx.width(Abstract.IndependentBody);
      const size_t WordBytes =
          sizeof(llvm::APInt) +
          (Width / 64 + (Width % 64 != 0)) * sizeof(uint64_t);
      // Cover measurement and basis-transform scratch before allocating the
      // additional reading. The opaque-input map remains owned by LinearRegion.
      const bool Direct =
          isCanonicalAffineSum(Ctx, Abstract.IndependentBody, AtomIds.size());
      const bool StorageFits =
          Corners && (Direct || *Corners <= Opts.MaxTableBytes / 8 / WordBytes);
      if (AtomIds.size() <= Limits.MaxAtoms && StorageFits &&
          (Direct ||
           !linearFormsCannotFit(Ctx, Abstract.IndependentBody, AtomIds,
                                 *Corners, termBudget(Ctx, E, Opts), Limits,
                                 Budget, ParityStorage)) &&
          Budget.consume(Direct ? Ctx.numOperands(Abstract.IndependentBody)
                                : *Corners)) {
        llvm::SmallVector<SymRef, 16> Atoms;
        for (uint32_t Id : AtomIds)
          Atoms.push_back(Ctx.varRef(Id));
        llvm::SmallVector<SymRef, 4> Forms;
        if (Direct || AtomIds.empty())
          Forms.push_back(Abstract.IndependentBody);
        else
          linearCandidates(Ctx, measure(Ctx, Abstract.IndependentBody, AtomIds),
                           Atoms, termBudget(Ctx, E, Opts), Limits, Forms);
        for (SymRef Form : Forms) {
          if (!proveLinearIdentity(Ctx, Abstract.IndependentBody, Form,
                                   Limits.MaxAtoms, Budget, Opts.MaxTableBytes))
            continue;
          if (!Abstract.Hidden.empty() && !Budget.consume(Ctx.dagSize(Form)))
            break;
          SymRef Rewritten = Abstract.Hidden.empty()
                                 ? Form
                                 : Ctx.substitute(Form, Abstract.Hidden);
          Candidates.push_back(
              {Rewritten, static_cast<unsigned>(Atoms.size()), true});
        }
      }
    }
  }
  if (LinearRegion && !ResidualWeights.empty() && !Budget.exhausted()) {
    llvm::SmallVector<SymRef, 8> Forms;
    auto optionalTermBudget = [&]() {
      size_t MaxCost = termBudget(Ctx, E, Opts);
      if (!LinearRegion->Abstract.Hidden.empty())
        return MaxCost;
      auto tighten = [&](SymRef Reference) {
        const SymReadability Score = readingScore(Ctx, Reference);
        size_t Ceiling = Score.Nodes;
        // These restored forms contain only leaves and unary/binary
        // operations. With N printed nodes, at least floor(N / 2) must be
        // operations. Once a known form meets that bound, a node-count tie
        // cannot improve it. Hidden inputs could violate this grammar.
        if (Ceiling && Score.Operations == Ceiling / 2)
          --Ceiling;
        MaxCost = std::min(MaxCost, Ceiling);
      };
      if (!Opts.AllowGrowth)
        tighten(E);
      for (const Candidate &C : Candidates)
        tighten(C.Expr);
      return MaxCost;
    };
    size_t MaxCost = optionalTermBudget();
    bool MayHaveBooleanPair = false;
    affineResidualCandidates(Ctx, ResidualWeights, LinearRegion->Atoms, MaxCost,
                             Limits, Budget, Forms, &MayHaveBooleanPair);
    for (SymRef Form : Forms) {
      if (Budget.exhausted())
        break;
      if (!proveLinearIdentity(Ctx, LinearRegion->Abstract.Body, Form,
                               Limits.MaxAtoms, Budget, Opts.MaxTableBytes))
        continue;
      if (!LinearRegion->Abstract.Hidden.empty() &&
          !Budget.consume(Ctx.dagSize(Form)))
        break;
      SymRef Rewritten =
          LinearRegion->Abstract.Hidden.empty()
              ? Form
              : Ctx.substitute(Form, LinearRegion->Abstract.Hidden);
      Candidates.push_back(
          {Rewritten, static_cast<unsigned>(LinearRegion->Atoms.size()), true});
    }
    // Keep the older affine residual proofs before spending anything on this
    // additional reading. Both searches reuse the small retained weight table
    // and its wide-word scratch guard above.
    if (MayHaveBooleanPair && !Budget.exhausted()) {
      Forms.clear();
      MaxCost = optionalTermBudget();
      booleanResidualCandidates(Ctx, ResidualWeights, LinearRegion->Atoms,
                                MaxCost, Limits, Budget, Forms);
      for (SymRef Form : Forms) {
        if (Budget.exhausted())
          break;
        if (!proveLinearIdentity(Ctx, LinearRegion->Abstract.Body, Form,
                                 Limits.MaxAtoms, Budget, Opts.MaxTableBytes))
          continue;
        if (!LinearRegion->Abstract.Hidden.empty() &&
            !Budget.consume(Ctx.dagSize(Form)))
          break;
        SymRef Rewritten =
            LinearRegion->Abstract.Hidden.empty()
                ? Form
                : Ctx.substitute(Form, LinearRegion->Abstract.Hidden);
        Candidates.push_back({Rewritten,
                              static_cast<unsigned>(LinearRegion->Atoms.size()),
                              true});
      }
    }
  }
  Rep.BudgetExhausted |= Budget.exhausted();

  // Reaching a measurement at all is the difference between "there is nothing
  // here of the kind I read" and "I read it and it is already as short as it
  // gets".  Being refused for width is a third answer again, and the only one a
  // larger budget would change.
  if (Rep.Outcome == MBAOutcome::NotApplicable) {
    if (Rep.BudgetExhausted)
      Rep.Outcome = MBAOutcome::BudgetExhausted;
    else if (Measured)
      Rep.Outcome = MBAOutcome::AlreadyShortest;
    else if (Rep.TooWide)
      Rep.Outcome = MBAOutcome::TooManyInputs;
  }

  const Candidate *Best = nullptr;
  SymReadability BestCost;
  SymReadability RawBest =
      readingScore(Ctx, Candidates.empty() ? E : Candidates.front().Expr);
  for (const Candidate &C : Candidates)
    RawBest = std::min(RawBest, readingScore(Ctx, C.Expr));
  llvm::SmallVector<std::pair<SymRef, SymRef>, 8> FactoredForms;
  for (Candidate &C : Candidates) {
    if (!C.Proven)
      continue;
    // Compare completed forms: a slightly larger basis expansion can expose
    // repeated coefficients whose factored spelling beats the shorter basis.
    // Complete only forms that would otherwise be discarded. The winning
    // spelling still gets the public final coefficient stage; factoring it
    // here would repeatedly normalize deep regions and hide useful children.
    // Normalize each unique losing form once, retaining it on refusal.
    auto Found = llvm::find_if(FactoredForms, [&](const auto &Entry) {
      return Entry.first == C.Expr;
    });
    if (Found != FactoredForms.end())
      C.Expr = Found->second;
    else if (!Budget.exhausted() && RawBest < readingScore(Ctx, C.Expr)) {
      const SymRef Before = C.Expr;
      SolveReport Factored;
      C.Expr = solveCoefficientFactors(Ctx, Before, Opts, Budget, Factored);
      Rep.BudgetExhausted |= Factored.BudgetExhausted;
      FactoredForms.emplace_back(Before, C.Expr);
    }
    SymReadability Cost = readingScore(Ctx, C.Expr);
    if (!Best || Cost < BestCost) {
      Best = &C;
      BestCost = Cost;
    }
  }
  if (!Best || Best->Expr == E)
    return E;

  // Keep a visible complement on an exact score tie: expanding it can hide
  // complementary factors from the enclosing polynomial. A strict operation
  // improvement still wins when the node counts agree.
  if (Ctx.op(E) == SymOp::Not && BestCost == readingScore(Ctx, E))
    return E;

  assert(Best->Proven && "an unproved MBA candidate reached selection");
  if (!Opts.AllowGrowth && !doesNotGrow(Ctx, Best->Expr, E))
    return E;

  bool Verified = agreeOnSamples(Ctx, E, Best->Expr, Opts.VerifySamples);
  assert(Verified &&
         "an MBA rewrite disagreed with the expression it replaces");
  if (!Verified)
    return E;

  Rep.NumAtoms = Best->NumAtoms;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Best->Expr;
}

//===----------------------------------------------------------------------===//
// Independent summand groups
//===----------------------------------------------------------------------===//

/// Solve independent summand groups before attempting a whole measurement.
///
/// The 2^t cost of a measurement is real arithmetic, not a tunable: it is how
/// many corners the weights are read off.  But it bounds a single measurement,
/// not an expression.  Two summands of a linear MBA that share no input are
/// independent, so a sum of them is several separate linear MBAs written next
/// to each other, and each can be measured over its own few inputs.  The price
/// is then set by the largest group instead of by the total, which puts an
/// expression over any number of inputs back in reach so long as they were
/// tangled a few at a time -- and tangling them all at once is something the
/// obfuscator cannot afford either, for the same 2^t reason.
///
/// Exactness comes from the independence: no input crosses a group boundary, so
/// summing the solved groups reproduces the original term for term.  The sample
/// check at the end guards against a slip in that reasoning rather than being
/// the reason to believe it.
SymRef solveIndependentTerms(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                             WorkBudget &Budget, SolveReport &Rep) {
  if (Ctx.op(E) != SymOp::Add ||
      isMinimalVariableSum(Ctx, E, resolveLimits(Opts)))
    return E;

  std::optional<Abstraction> Abstract = abstractToMBA(
      Ctx, E, /*AllowProducts=*/false, &Budget, Opts.MaxTableBytes);
  if (!Abstract)
    return E;

  llvm::SmallVector<uint32_t, 32> AllAtoms;
  Ctx.collectVars(Abstract->Body, AllAtoms);
  if (AllAtoms.size() < 2)
    return E;

  // The cut is between summands, so there has to be a sum to cut.
  if (Ctx.op(Abstract->Body) != SymOp::Add)
    return E;
  llvm::ArrayRef<SymRef> Terms = Ctx.operands(Abstract->Body);

  // Two inputs share a group when one term mentions both; closing the relation
  // up means a chain of terms puts their inputs in one group too.
  llvm::DenseMap<uint32_t, uint32_t> Parent;
  for (uint32_t Id : AllAtoms)
    Parent[Id] = Id;
  auto find = [&Parent](uint32_t X) {
    while (Parent[X] != X) {
      Parent[X] = Parent[Parent[X]];
      X = Parent[X];
    }
    return X;
  };

  llvm::SmallVector<llvm::SmallVector<uint32_t, 4>, 16> PerTerm(Terms.size());
  for (size_t I = 0; I < Terms.size(); ++I) {
    Ctx.collectVars(Terms[I], PerTerm[I]);
    for (size_t J = 1; J < PerTerm[I].size(); ++J) {
      uint32_t A = find(PerTerm[I][0]);
      uint32_t B = find(PerTerm[I][J]);
      if (A != B)
        Parent[A] = B;
    }
  }

  // Keyed by group representative so the rebuilt sum comes out the same way on
  // every run; a term naming no input is a constant and joins no group.
  std::map<uint32_t, llvm::SmallVector<SymRef, 8>> Groups;
  llvm::SmallVector<SymRef, 4> Constants;
  for (size_t I = 0; I < Terms.size(); ++I) {
    if (PerTerm[I].empty()) {
      Constants.push_back(Terms[I]);
      continue;
    }
    Groups[find(PerTerm[I][0])].push_back(Terms[I]);
  }
  // A single group means the inputs really are all tangled together, so the
  // width belongs to the expression rather than to how it was written.
  if (Groups.size() < 2)
    return E;

  llvm::SmallVector<SymRef, 16> Parts;
  llvm::SmallVector<size_t, 4> OffsetGroups;
  const bool HasOffset = Constants.size() == 1 &&
                         Ctx.isConst(Constants.front()) &&
                         !Ctx.isConstZero(Constants.front());
  unsigned Widest = 0;
  bool AnySolved = false;
  for (const auto &Group : Groups) {
    llvm::ArrayRef<SymRef> GroupTerms = Group.second;
    // A shared offset can expose a complement in one nontrivial region.
    // Keep a fixed number of alternatives, not every partition of that offset.
    if (HasOffset && GroupTerms.size() > 1 && OffsetGroups.size() < 4)
      OffsetGroups.push_back(Parts.size());
    SymRef Part =
        GroupTerms.size() == 1 ? GroupTerms[0] : Ctx.mkAdd(GroupTerms);
    SolveReport PartRep;
    SymRef Solved = solveRegion(Ctx, Part, Opts, Budget, PartRep);
    Rep.BudgetExhausted |= PartRep.BudgetExhausted;
    if (Solved != Part) {
      AnySolved = true;
      Widest = std::max(Widest, PartRep.NumAtoms);
    }
    Parts.push_back(Solved);
  }
  if (!AnySolved && OffsetGroups.empty())
    return E;

  Parts.append(Constants.begin(), Constants.end());
  auto restoreParts = [&]() {
    SymRef R = Ctx.mkAdd(Parts);
    return Abstract->Hidden.empty() ? R : Ctx.substitute(R, Abstract->Hidden);
  };
  SymRef Rebuilt = AnySolved ? restoreParts() : E;
  if (!OffsetGroups.empty() && !Budget.exhausted()) {
    const SymReadability InputCost = readingScore(Ctx, E);
    SymReadability BestCost = readingScore(Ctx, Rebuilt);
    const size_t RestoreWork = Ctx.dagSize(E);
    // Bound the temporary sum edges separately from the solver's tables.
    const bool Fits = Parts.size() <= Opts.MaxTableBytes / (2 * sizeof(SymRef));
    if (!Fits)
      Rep.BudgetExhausted = true;
    for (size_t I : OffsetGroups) {
      if (!Fits || Budget.exhausted())
        break;
      if (!Budget.consume(Parts.size()) || !Budget.consume(RestoreWork)) {
        Rep.BudgetExhausted = true;
        break;
      }
      SymRef Previous = Parts[I];
      SymRef WithOffset = Ctx.mkAdd(Previous, Constants.front());
      SolveReport OffsetRep;
      SymRef Solved = solveRegion(Ctx, WithOffset, Opts, Budget, OffsetRep);
      Rep.BudgetExhausted |= OffsetRep.BudgetExhausted;
      if (Solved == WithOffset)
        continue;

      // The offset belongs to this group exactly once. Every other group
      // keeps its independently proved value, including on later attempts.
      Parts[I] = Solved;
      Parts.pop_back();
      SymRef Candidate = restoreParts();
      Parts.push_back(Constants.front());
      Parts[I] = Previous;
      SymReadability Cost = readingScore(Ctx, Candidate);
      if (Cost < InputCost && Cost < BestCost) {
        Rebuilt = Candidate;
        BestCost = Cost;
        Widest = std::max(Widest, OffsetRep.NumAtoms);
      }
    }
  }
  if (Rebuilt == E)
    return E;

  if (!Opts.AllowGrowth && !doesNotGrow(Ctx, Rebuilt, E))
    return E;

  bool Verified = agreeOnSamples(Ctx, E, Rebuilt, Opts.VerifySamples);
  assert(Verified && "a split MBA rewrite disagreed with what it replaces");
  if (!Verified)
    return E;

  Rep.NumAtoms = Widest;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Rebuilt;
}

// Every even factor contributes at least one trailing zero to a product.
// Enumerate the complete parity domain (up to six independent opaque inputs)
// in parallel, then find whether every assignment supplies enough zeros.
SymRef foldZeroProduct(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                       size_t &Storage, bool Share = true) {
  if (Ctx.op(Root) != SymOp::Mul || Ctx.numOperands(Root) < 8 ||
      Ctx.numOperands(Root) >
          ::neverd::symbolic::detail::limits::
              kSymMBARegionFoldZeroProductCtxNumOperandsRootLimit ||
      Budget.exhausted())
    return Root;
  if (Share && Budget.MemoOwner == &Ctx && Budget.Parities) {
    auto Found = Budget.Parities->Products.find(Root.index());
    if (Found != Budget.Parities->Products.end() && Budget.canConsume(1)) {
      Budget.consume();
      return Found->second;
    }
  }
  auto Factors = Ctx.operands(Root);
  unsigned Needed = Ctx.width(Root);
  if (Ctx.isConst(Factors.front())) {
    const size_t Words = (size_t(Needed) + 63) / 64;
    if (Needed <= 64) {
      Needed -= std::countr_zero(Ctx.node(Factors.front()).Aux);
    } else {
      if (Words > Storage / 8 || !Budget.consume(Words))
        return Root;
      Storage -= Words * 8;
      Needed -= Ctx.constValue(Factors.front()).countr_zero();
    }
    Factors = Factors.drop_front();
  }
  if (Needed > 8 || Needed > Factors.size())
    return Root;
  auto Charge = [&](size_t Work, size_t Bytes = 0) {
    if (Bytes > Storage || !Budget.consume(Work))
      return false;
    Storage -= Bytes;
    return true;
  };
  auto Transparent = [&](SymRef R) {
    auto Op = Ctx.op(R);
    return Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor ||
           Op == SymOp::Not || Op == SymOp::Add || Op == SymOp::Mul;
  };
  ParityMemo *Cache = nullptr;
  if (Share && Budget.MemoOwner == &Ctx &&
      Budget.MemoCapacity >= limits::kMinParityMemoCapacity) {
    if (!Budget.Parities) {
      if (!Charge(1, sizeof(ParityMemo)))
        return Root;
      Budget.Parities = std::make_shared<ParityMemo>();
    }
    Cache = Budget.Parities.get();
  }
  std::array<SymRef, 256> Pending;
  size_t Top = Factors.size();
  if (!Charge(Top, Top * sizeof(SymRef)))
    return Root;
  std::copy(Factors.begin(), Factors.end(), Pending.begin());
  std::set<uint32_t> Nodes;
  while (Top) {
    auto R = Pending[--Top];
    if (!Charge(1))
      return Root;
    if (Nodes.contains(R.index()))
      continue;
    if (Nodes.size() == ::neverd::symbolic::detail::limits::
                            kSymMBARegionFoldZeroProductNodesSizeLimit ||
        !Charge(1, 128))
      return Root;
    Nodes.insert(R.index());
    if ((Cache && Cache->Values.contains(R.index())) || !Transparent(R))
      continue;
    auto Children = Ctx.operands(R);
    if (Children.size() > Pending.size() - Top || !Charge(Children.size()))
      return Root;
    for (auto Child : Children)
      Pending[Top++] = Child;
  }
  constexpr std::array<uint64_t, 6> Variables{
      0xaaaaaaaaaaaaaaaaULL, 0xccccccccccccccccULL, 0xf0f0f0f0f0f0f0f0ULL,
      0xff00ff00ff00ff00ULL, 0xffff0000ffff0000ULL, 0xffffffff00000000ULL};
  std::map<uint32_t, uint64_t> Values;
  unsigned Atoms = 0;
  for (auto Id : Nodes) {
    if (!Charge(1, 128))
      return Root;
    auto R = SymRef(Id);
    uint64_t Value;
    auto Op = Ctx.op(R);
    auto Cached = Cache ? Cache->Values.find(Id)
                        : llvm::DenseMap<uint32_t, uint64_t>::iterator{};
    if (Cache && Cached != Cache->Values.end()) {
      Value = Cached->second;
    } else if (Ctx.isConst(R)) {
      const size_t Words = (size_t(Ctx.width(R)) + 63) / 64;
      if (!Charge(Words, Words * sizeof(uint64_t)))
        return Root;
      Value = Ctx.constValue(R)[0] ? ~uint64_t(0) : 0;
    } else if (!Transparent(R)) {
      if (Cache) {
        auto Found = Cache->Inputs.find(Id);
        if (Found == Cache->Inputs.end()) {
          // The current product may still fit a smaller local domain when
          // unrelated earlier products filled the shared domain. Retry it
          // without memoization; do not turn cache capacity into a refusal.
          if (Cache->Inputs.size() == Variables.size() ||
              Cache->Inputs.size() + Cache->Products.size() +
                      Cache->Values.size() >=
                  Budget.MemoCapacity)
            return foldZeroProduct(Ctx, Root, Budget, Storage, false);
          if (!Charge(1, 64))
            return Root;
          Found = Cache->Inputs.try_emplace(Id, Cache->Inputs.size()).first;
        }
        Value = Variables[Found->second];
      } else {
        if (Atoms == Variables.size())
          return Root;
        Value = Variables[Atoms++];
      }
    } else if (Op == SymOp::Not) {
      Value = ~Values.at(Ctx.operand(R, 0).index());
    } else {
      Value = (Op == SymOp::And || Op == SymOp::Mul) ? ~uint64_t(0) : 0;
      for (auto Child : Ctx.operands(R)) {
        if (!Charge(1))
          return Root;
        auto V = Values.at(Child.index());
        if (Op == SymOp::And || Op == SymOp::Mul)
          Value &= V;
        else if (Op == SymOp::Or)
          Value |= V;
        else
          Value ^= V; // Addition and XOR have identical parity.
      }
    }
    Values.emplace(Id, Value);
    if (Cache && Cached == Cache->Values.end() &&
        Cache->Values.size() + Cache->Products.size() + Cache->Inputs.size() <
            Budget.MemoCapacity &&
        Budget.canConsume(1)) {
      Budget.consume();
      Cache->Values.try_emplace(Id, Value);
    }
  }
  // Bit-sliced counters count even factors for all 64 assignments together.
  // Repeated factors count repeatedly; their parity is never made independent.
  std::array<uint64_t, 7> Counts{};
  for (size_t F = 0; F != Factors.size();) {
    const SymRef Factor = Factors[F++];
    unsigned Multiplicity = 1;
    while (F != Factors.size() && Factors[F] == Factor) {
      ++Multiplicity;
      ++F;
    }
    if (!Charge(Multiplicity))
      return Root;
    const uint64_t Even = ~Values.at(Factor.index());
    while (Multiplicity) {
      const unsigned Bit = std::countr_zero(Multiplicity);
      Multiplicity &= Multiplicity - 1;
      uint64_t Carry = Even;
      for (size_t I = Bit; Carry && I != Counts.size(); ++I) {
        if (!Charge(1))
          return Root;
        const uint64_t Next = Carry & Counts[I];
        Counts[I] ^= Carry;
        Carry = Next;
      }
    }
  }
  if (!Charge(Counts.size()))
    return Root;
  uint64_t Greater = 0, Equal = ~uint64_t(0);
  for (unsigned I = Counts.size(); I-- != 0;) {
    if (Needed & (1u << I))
      Equal &= Counts[I];
    else {
      Greater |= Equal & Counts[I];
      Equal &= ~Counts[I];
    }
  }
  const SymRef Result =
      (Greater | Equal) == ~uint64_t(0) ? Ctx.mkZero(Ctx.width(Root)) : Root;
  if (Cache &&
      Cache->Products.size() + Cache->Values.size() + Cache->Inputs.size() <
          Budget.MemoCapacity &&
      Budget.canConsume(1)) {
    Budget.consume();
    Cache->Products.try_emplace(Root.index(), Result);
  }
  return Result;
}

} // namespace

SymRef solveExpandedProducts(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                             WorkBudget &Budget, SolveReport &Rep) {
  // An allocation-free frontier hint keeps ordinary linear regions on their
  // existing path. It grants no equivalence: the full abstraction, degree
  // check and independent polynomial proof below must all succeed.
  auto Unscale = [&](SymRef R) {
    return Ctx.op(R) == SymOp::Mul && Ctx.numOperands(R) == 2 &&
                   Ctx.isConst(Ctx.operand(R, 0))
               ? Ctx.operand(R, 1)
               : R;
  };
  SymRef Sum = Unscale(E);
  if (Ctx.op(Sum) != SymOp::Add ||
      Ctx.numOperands(Sum) >
          ::neverd::symbolic::detail::limits::kMaxExpandedProductSumOperands ||
      Budget.exhausted())
    return E;
  unsigned Products = 0, Remaining = ::neverd::symbolic::detail::limits::
                             kSymMBARegionSolveExpandedProductsRemaining;
  auto Inspect = [&](auto &&Self, SymRef R, unsigned Depth) -> void {
    if (!Remaining || Products >= 2 ||
        Depth > ::neverd::symbolic::detail::limits::
                    kSymMBARegionSolveExpandedProductsInspectDepthLimit)
      return;
    --Remaining;
    R = Unscale(R);
    if (Ctx.op(R) == SymOp::Add && Ctx.numOperands(R) <=
                                       ::neverd::symbolic::detail::limits::
                                           kMaxExpandedProductSumOperands) {
      for (SymRef Term : Ctx.operands(R))
        Self(Self, Term, Depth + 1);
      return;
    }
    if (Ctx.op(R) != SymOp::Mul)
      return;
    auto Factors = Ctx.operands(R);
    if (Ctx.isConst(Factors.front()))
      Factors = Factors.drop_front();
    Products += Factors.size() == 2 || Factors.size() == 3;
  };
  Inspect(Inspect, Sum, 0);
  if (Products < 2)
    return E;
  if (!Budget.consume(Ctx.dagSize(E))) {
    Rep.BudgetExhausted = true;
    return E;
  }
  auto Region = readRegion(Ctx, E, Opts, true, Budget, Rep);
  Rep.BudgetExhausted |= Budget.exhausted();
  if (!Region)
    return E;
  auto Terms = splitIntoTerms(Ctx, Region->Abstract.Body);
  if (!Terms || llvm::any_of(*Terms, [](const PolyTerm &T) {
        return T.Factors.size() > 3;
      }))
    return E;
  auto Form = solvePolynomial(Ctx, Region->Abstract.Body, Region->AtomIds,
                              Region->Atoms, Opts, Budget);
  if (!Form ||
      !provePolynomialIdentity(Ctx, Region->Abstract.Body, *Form,
                               resolveLimits(Opts).MaxAtoms, Budget,
                               Opts.MaxTableBytes) ||
      !Budget.consume(Ctx.dagSize(*Form))) {
    Rep.BudgetExhausted |= Budget.exhausted();
    return E;
  }
  SymRef Result = Region->Abstract.Hidden.empty()
                      ? *Form
                      : Ctx.substitute(*Form, Region->Abstract.Hidden);
  if (readingScore(Ctx, Result) >= readingScore(Ctx, E))
    return E;
  Rep.NumAtoms = Region->Atoms.size();
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Result;
}

SymRef foldProductValuations(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                             size_t &Storage) {
  Root = foldProductResidues(Ctx, Root, Budget, Storage);
  if (Ctx.op(Root) != SymOp::Add)
    return foldZeroProduct(Ctx, Root, Budget, Storage);
  const size_t N = Ctx.numOperands(Root);
  if (N > ::neverd::symbolic::detail::limits::kMaxProductParityInputs ||
      N > Storage / (2 * sizeof(SymRef)))
    return Root;
  bool HasLongProduct = false;
  for (auto Term : Ctx.operands(Root)) {
    if (Ctx.op(Term) != SymOp::Mul || Ctx.numOperands(Term) < 8)
      continue;
    HasLongProduct = true;
    auto First = Ctx.operand(Term, 0);
    if (Ctx.width(Term) <= 64 &&
        Ctx.width(Term) - (Ctx.isConst(First)
                               ? std::countr_zero(Ctx.node(First).Aux)
                               : 0) >
            8)
      return Root;
  }
  if (!HasLongProduct)
    return Root;
  if (!Budget.consume(N))
    return Root;
  Storage -= N * 2 * sizeof(SymRef);
  llvm::SmallVector<SymRef, 16> Terms(Ctx.operands(Root));
  bool Changed = false;
  for (auto &Term : Terms) {
    auto Value = foldZeroProduct(Ctx, Term, Budget, Storage);
    Changed |= Value != Term;
    if (Value == Term && Ctx.op(Term) == SymOp::Mul &&
        Ctx.numOperands(Term) >= 8)
      return Root;
    Term = Value;
  }
  return Changed ? Ctx.mkAdd(Terms) : Root;
}

SymRef foldProductParity(SymContext &Ctx, SymRef Root, WorkBudget &Budget,
                         size_t &Storage) {
  if (Budget.exhausted() || !Budget.canConsume(1) || Storage < 128)
    return Root;
  if (Ctx.op(Root) == SymOp::Mul)
    if (auto Zero = foldProductResidues(Ctx, Root, Budget, Storage);
        Zero != Root)
      return Zero;
  if (auto Zero = foldZeroProduct(Ctx, Root, Budget, Storage); Zero != Root)
    return Zero;
  if (Ctx.op(Root) == SymOp::Mul && Ctx.numOperands(Root) >= 3 &&
      Ctx.numOperands(Root) <=
          ::neverd::symbolic::detail::limits::
              kSymMBARegionFoldProductParityCtxNumOperandsRootLimit &&
      Ctx.isConst(Ctx.operand(Root, 0))) {
    const SymRef Scale = Ctx.operand(Root, 0);
    const unsigned Width = Ctx.width(Root);
    const size_t Words = (size_t(Width) - 1) / 64 + 1;
    bool ObservesLowBit = false;
    if (Width <= 64) {
      ObservesLowBit = Ctx.node(Scale).Aux == (uint64_t(1) << (Width - 1));
    } else if (Words <= Storage / sizeof(uint64_t) && Budget.consume(Words)) {
      Storage -= Words * sizeof(uint64_t);
      ObservesLowBit = Ctx.constValue(Scale).isMinSignedValue();
    }
    if (ObservesLowBit) {
      auto Factors = Ctx.operands(Root).drop_front();
      size_t Edges = 0;
      for (SymRef Factor : Factors) {
        const size_t Count =
            Ctx.op(Factor) == SymOp::And ? Ctx.numOperands(Factor) : 1;
        if (Count >
            ::neverd::symbolic::detail::limits::kMaxProductParityEdges - Edges)
          return Root;
        Edges += Count;
      }
      const size_t Bytes = 128 + 4 * sizeof(SymRef) * (Edges + Factors.size());
      if (Bytes > Storage || !Budget.consume(1 + Edges + Factors.size()))
        return Root;
      Storage -= Bytes;
      llvm::SmallVector<SymRef, 8> Inputs(Factors);
      // Multiplication modulo two is Boolean AND. Scaling by 2^(Width-1)
      // discards every other product bit, for arbitrary word-valued factors.
      // A smaller power of two observes carries and does not admit this fold.
      return Ctx.mkMul(Scale, Ctx.mkAnd(Inputs));
    }
  }
  return Root;
}

static SymRef solveOneRegionImpl(SymContext &Ctx, SymRef E,
                                 const MBAOptions &Opts, WorkBudget &Budget,
                                 SolveReport &Rep) {
  SolveReport ArithmeticRep;
  SymRef Arithmetic = solveArithmetic(Ctx, E, Opts, Budget, ArithmeticRep);
  if (Budget.exhausted() || Ctx.isConst(Arithmetic) ||
      (Arithmetic != E && Ctx.op(Arithmetic) == SymOp::Var) ||
      isMinimalUnaryVariable(Ctx, Arithmetic) ||
      isMinimalBinaryLeafExpression(Ctx, Arithmetic, Opts)) {
    Rep = ArithmeticRep;
    if (Arithmetic == E && !Budget.exhausted() &&
        isMinimalBinaryLeafExpression(Ctx, Arithmetic, Opts))
      Rep.Outcome = MBAOutcome::AlreadyShortest;
    return Arithmetic;
  }

  if (isMinimalVariableProduct(Ctx, Arithmetic, resolveLimits(Opts))) {
    Rep = ArithmeticRep;
    if (Arithmetic == E)
      Rep.Outcome = MBAOutcome::AlreadyShortest;
    return Arithmetic;
  }

  // Factoring can hide a bitwise product identity inside a new sum. Measure
  // the original spelling too, then rank both exact readings.
  SymRef Split = solveIndependentTerms(Ctx, E, Opts, Budget, Rep);

  // Local rewrites can expose a cheaper shared form after placeholders are
  // restored. Keep that whole-region opportunity, while sums of distinct
  // variables stop at their proven minimum instead of enumerating 2^n corners.
  SolveReport Refined;
  SymRef Solved = solveRegion(Ctx, Split, Opts, Budget, Refined);
  if (Split == E)
    Rep = Refined;
  Rep.BudgetExhausted |= Refined.BudgetExhausted;
  Rep.TooWide |= Refined.TooWide;
  Rep.NumAtoms = std::max(Rep.NumAtoms, Refined.NumAtoms);
  if (Arithmetic != E) {
    if (readingScore(Ctx, Arithmetic) <= readingScore(Ctx, Solved)) {
      Solved = Arithmetic;
      Rep.NumAtoms = ArithmeticRep.NumAtoms;
    }
    Rep.Outcome = MBAOutcome::Rewritten;
    Rep.Evidence = MBAEvidence::Derivation;
  }
  Rep.BudgetExhausted |= ArithmeticRep.BudgetExhausted;
  if (Solved == E && Rep.BudgetExhausted)
    Rep.Outcome = MBAOutcome::BudgetExhausted;
  if (Solved != E && Solved != Arithmetic && !Budget.exhausted()) {
    // The arithmetic candidate already has a checked canonical polynomial.
    // Normalize only a newly restored region, not that identical candidate.
    // Restoring hidden affine inputs can expose products of constants and
    // sums. Cancel their modular coefficients before publishing the spelling.
    SolveReport Restored;
    SymRef Normalized = solveArithmetic(Ctx, Solved, Opts, Budget, Restored);
    Rep.BudgetExhausted |= Restored.BudgetExhausted;
    if (Normalized != Solved) {
      Solved = Normalized;
      Rep.NumAtoms = std::max(Rep.NumAtoms, Restored.NumAtoms);
    }
  }
  if (Solved != E && !Budget.exhausted() &&
      (Ctx.op(Solved) == SymOp::Add || Ctx.op(Solved) == SymOp::Mul) &&
      readingScore(Ctx, Solved) < readingScore(Ctx, E) &&
      !isMinimalVariableSum(Ctx, Solved, resolveLimits(Opts))) {
    // A sum of scaled opaque inputs has no bitwise relation left to read.
    // Canonical multiplication puts its one combined coefficient first.
    auto unscaled = [&](SymRef R) {
      return Ctx.op(R) == SymOp::Mul && Ctx.numOperands(R) == 2 &&
                     Ctx.isConst(Ctx.operand(R, 0))
                 ? Ctx.operand(R, 1)
                 : R;
    };
    auto isBitwise = [&](SymRef R) {
      SymOp Op = Ctx.op(unscaled(R));
      return Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor ||
             Op == SymOp::Not;
    };
    SymRef Body = unscaled(Solved);
    bool HasRelation = isBitwise(Body);
    if (!HasRelation && Ctx.op(Body) == SymOp::Add) {
      for (SymRef Term : Ctx.operands(Body)) {
        if (!Budget.consume())
          break;
        if (isBitwise(Term)) {
          HasRelation = true;
          break;
        }
      }
    }
    Rep.BudgetExhausted |= Budget.exhausted();
    if (!HasRelation || Budget.exhausted())
      return Solved;
    // Restoring inputs can expose a new linear relation after the original
    // reading. Give that result one more exact reading with the same budget;
    // it is not in the deep walk's original postorder. Requiring a strict
    // decrease keeps equal-cost spellings stable, even in growth mode.
    SolveReport Restored;
    SymRef Refined = solveRegion(Ctx, Solved, Opts, Budget, Restored);
    Rep.BudgetExhausted |= Restored.BudgetExhausted;
    if (readingScore(Ctx, Refined) < readingScore(Ctx, Solved)) {
      Solved = Refined;
      Rep.NumAtoms = std::max(Rep.NumAtoms, Restored.NumAtoms);
      Rep.Outcome = MBAOutcome::Rewritten;
      Rep.Evidence = MBAEvidence::Derivation;
    }
  }
  return Solved;
}

SymRef solveOneRegion(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                      WorkBudget &Budget, SolveReport &Rep) {
  const bool Cacheable = Budget.MemoOwner == &Ctx && Budget.MemoCapacity &&
                         Rep.Outcome == MBAOutcome::NotApplicable &&
                         Rep.Evidence == MBAEvidence::None &&
                         !Rep.BudgetExhausted && !Rep.TooWide &&
                         Rep.NumAtoms == 0;
  auto Compatible = [&](const MBAOptions &Previous) {
    return Previous.MaxTableBytes >= Opts.MaxTableBytes &&
           Previous.MaxAtoms == Opts.MaxAtoms &&
           Previous.MaxSynthesisAtoms == Opts.MaxSynthesisAtoms &&
           Previous.MaxOptimalSynthesisAtoms == Opts.MaxOptimalSynthesisAtoms &&
           Previous.VerifySamples == Opts.VerifySamples &&
           Previous.AllowGrowth == Opts.AllowGrowth &&
           Previous.MaxWork == Opts.MaxWork;
  };
  if (Cacheable && Budget.Regions) {
    auto Found = Budget.Regions->Entries.find(E.index());
    if (Found != Budget.Regions->Entries.end() &&
        Compatible(Found->second.Options) && Budget.canConsume(1)) {
      Budget.consume();
      Rep = Found->second.Report;
      return Found->second.Result;
    }
  }
  const size_t Start = Budget.used();
  SymRef Result = solveOneRegionImpl(Ctx, E, Opts, Budget, Rep);
  if (Cacheable && !Budget.exhausted() && !Rep.BudgetExhausted &&
      Budget.used() - Start >= limits::kMinCompletedRegionCacheWork &&
      (!Budget.Regions ||
       Budget.Regions->Entries.size() < Budget.MemoCapacity) &&
      Budget.canConsume(1)) {
    Budget.consume();
    if (!Budget.Regions)
      Budget.Regions = std::make_shared<RegionMemo>();
    Budget.Regions->Entries.insert_or_assign(
        E.index(), RegionMemo::Entry{Result, Rep, Opts});
  }
  return Result;
}

} // namespace neverd::symbolic::detail
