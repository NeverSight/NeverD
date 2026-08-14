//===- SymMBARegion.cpp - Solving one MBA region --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the region solver: the linear and polynomial readings of one
/// expression, the ranking between them, and the split of a region too wide to
/// measure whole into groups of summands that share no input.
///
/// Both readings run and the shorter proved answer wins.  Ranking uses a
/// strict comparison, so where two candidates cost the same the one produced
/// first is kept — which makes the order the candidates are appended in part
/// of the result.
///
//===----------------------------------------------------------------------===//

#include "SymMBADetail.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cassert>
#include <map>
#include <optional>

namespace neverd::symbolic::detail {

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

std::optional<Region> readRegion(SymContext &Ctx, SymRef E, unsigned MaxAtoms,
                                 bool AllowProducts, SolveReport &Rep) {
  std::optional<Abstraction> Abstract = abstractToMBA(Ctx, E, AllowProducts);
  if (!Abstract)
    return std::nullopt;

  Region Out;
  Out.Abstract = std::move(*Abstract);
  Ctx.collectVars(Out.Abstract.Body, Out.AtomIds);
  const bool TooWide =
      Out.AtomIds.size() > MaxAtoms || !cornerCount(Out.AtomIds.size());
  if (TooWide)
    Rep.TooWide = true;
  if (Out.AtomIds.empty() || TooWide)
    return std::nullopt;
  for (uint32_t Id : Out.AtomIds) {
    const SymVarInfo &Info = Ctx.varInfo(Id);
    Out.Atoms.push_back(Ctx.mkVar(Info.Name, Info.Width));
  }
  return Out;
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
  const SolverLimits Limits = resolveLimits(Opts);
  llvm::SmallVector<Candidate, 6> Candidates;
  bool Measured = false;

  if (std::optional<Region> Linear =
          readRegion(Ctx, E, Limits.MaxAtoms, /*AllowProducts=*/false, Rep)) {
    Measured = true;
    auto NumAtoms = static_cast<unsigned>(Linear->AtomIds.size());
    std::optional<size_t> Corners = cornerCount(Linear->AtomIds.size());
    if (!Corners || !Budget.consume(*Corners)) {
      Rep.BudgetExhausted = true;
    } else {
      llvm::SmallVector<SymRef, 4> Forms;
      linearCandidates(Ctx,
                       measure(Ctx, Linear->Abstract.Body, Linear->AtomIds),
                       Linear->Atoms, termBudget(Ctx, E, Opts), Limits, Forms);
      for (SymRef Form : Forms) {
        SymRef Rewritten = Linear->Abstract.Hidden.empty()
                               ? Form
                               : Ctx.substitute(Form, Linear->Abstract.Hidden);
        if (proveLinearIdentity(Ctx, E, Rewritten, Limits.MaxAtoms, Budget))
          Candidates.push_back({Rewritten, NumAtoms, true});
      }
    }
  }

  if (std::optional<Region> Poly =
          readRegion(Ctx, E, Limits.MaxAtoms, /*AllowProducts=*/true, Rep)) {
    Measured = true;
    if (std::optional<SymRef> Form =
            solvePolynomial(Ctx, Poly->Abstract.Body, Poly->AtomIds,
                            Poly->Atoms, Opts, Budget)) {
      SymRef Rewritten = Poly->Abstract.Hidden.empty()
                             ? *Form
                             : Ctx.substitute(*Form, Poly->Abstract.Hidden);
      if (provePolynomialIdentity(Ctx, E, Rewritten, Limits.MaxAtoms, Budget))
        Candidates.push_back(
            {Rewritten, static_cast<unsigned>(Poly->AtomIds.size()), true});
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
  size_t BestCost = 0;
  for (const Candidate &C : Candidates) {
    if (!C.Proven)
      continue;
    size_t Cost = readingCost(Ctx, C.Expr);
    if (!Best || Cost < BestCost) {
      Best = &C;
      BestCost = Cost;
    }
  }
  if (!Best || Best->Expr == E)
    return E;

  assert(Best->Proven && "an unproved MBA candidate reached selection");
  bool Verified = agreeOnSamples(Ctx, E, Best->Expr, Opts.VerifySamples);
  assert(Verified &&
         "an MBA rewrite disagreed with the expression it replaces");
  if (!Verified)
    return E;

  if (!Opts.AllowGrowth && BestCost > readingCost(Ctx, E))
    return E;

  Rep.NumAtoms = Best->NumAtoms;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Best->Expr;
}

//===----------------------------------------------------------------------===//
// Regions too wide to measure whole
//===----------------------------------------------------------------------===//

/// Solve a region with more inputs than one measurement can afford by cutting
/// it into groups of summands that share no input.
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
SymRef solveWide(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                 WorkBudget &Budget, SolveReport &Rep) {
  // Against the resolved ceiling rather than the requested one.  A caller that
  // asks for an unlimited arity is asking to measure as wide as the resources
  // allow, not to switch the split off — and comparing a node count against
  // the sentinel would do exactly that, silently, at the one input size the
  // split exists for.
  const SolverLimits Limits = resolveLimits(Opts);

  // An expression cannot abstract to more inputs than it has nodes, so this
  // rejects everything narrow without paying for an abstraction first.
  if (Ctx.dagSize(E) <= Limits.MaxAtoms)
    return E;

  std::optional<Abstraction> Abstract =
      abstractToMBA(Ctx, E, /*AllowProducts=*/false);
  if (!Abstract)
    return E;

  llvm::SmallVector<uint32_t, 32> AllAtoms;
  Ctx.collectVars(Abstract->Body, AllAtoms);
  // Measurable in one piece, which the ordinary solver has already tried;
  // splitting it could only reach the same answer by a longer road.
  if (AllAtoms.size() <= Limits.MaxAtoms)
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
  unsigned Widest = 0;
  bool AnySolved = false;
  for (const auto &Group : Groups) {
    llvm::ArrayRef<SymRef> GroupTerms = Group.second;
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
  if (!AnySolved)
    return E;

  Parts.append(Constants.begin(), Constants.end());
  SymRef Rebuilt = Parts.size() == 1 ? Parts[0] : Ctx.mkAdd(Parts);
  if (!Abstract->Hidden.empty())
    Rebuilt = Ctx.substitute(Rebuilt, Abstract->Hidden);
  if (Rebuilt == E)
    return E;

  bool Verified = agreeOnSamples(Ctx, E, Rebuilt, Opts.VerifySamples);
  assert(Verified && "a split MBA rewrite disagreed with what it replaces");
  if (!Verified)
    return E;

  if (!Opts.AllowGrowth && readingCost(Ctx, Rebuilt) > readingCost(Ctx, E))
    return E;

  Rep.NumAtoms = Widest;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Rebuilt;
}

} // namespace

SymRef solveOneRegion(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                      WorkBudget &Budget, SolveReport &Rep) {
  SymRef Solved = solveRegion(Ctx, E, Opts, Budget, Rep);
  return Solved == E ? solveWide(Ctx, E, Opts, Budget, Rep) : Solved;
}

} // namespace neverd::symbolic::detail
