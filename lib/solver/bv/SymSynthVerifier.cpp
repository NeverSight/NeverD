//===- SymSynthVerifier.cpp - Prove synthesis candidates -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/solver/SymSynthVerifier.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/solver/SatTypes.h"
#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymSynth.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace neverd::solver {

namespace {

void addSaturating(uint64_t &Total, uint64_t Delta) {
  constexpr uint64_t Max = std::numeric_limits<uint64_t>::max();
  Total = Delta > Max - Total ? Max : Total + Delta;
}

bool isValidRef(const symbolic::SymContext &Ctx, symbolic::SymRef R) {
  return R.isValid() && R.index() < Ctx.numNodes();
}

} // namespace

SymSynthVerifier::SymSynthVerifier(SolverOptions Options)
    : Options(std::move(Options)) {
  // A refutation without its assignment is not useful to a synthesis caller.
  // Keep model ownership as an invariant even if a generic solver option was
  // copied from a proof-only call site.
  this->Options.BuildModel = true;
}

void SymSynthVerifier::addStats(const SatStats &Stats) {
  addSaturating(Report.Stats.Conflicts, Stats.Conflicts);
  addSaturating(Report.Stats.Propagations, Stats.Propagations);
  addSaturating(Report.Stats.WatchVisits, Stats.WatchVisits);
}

void SymSynthVerifier::clearRefutation() {
  Report.RejectedCandidate = symbolic::SymRef();
  Report.Counterexample.reset();
}

symbolic::SynthVerification
SymSynthVerifier::operator()(symbolic::SymContext &Ctx,
                             symbolic::SymRef Original,
                             symbolic::SymRef Candidate) {
  addSaturating(Report.Stats.Queries, 1);

  EquivResult Result = EquivResult::Invalid;
  std::optional<BitVectorModel> Counterexample;

  if (isValidRef(Ctx, Original) && isValidRef(Ctx, Candidate) &&
      Ctx.width(Original) == Ctx.width(Candidate)) {
    if (Original == Candidate) {
      Result = EquivResult::Equal;
    } else {
      BitVectorSolver Solver(Ctx, Options);
      Solver.assertDistinct(Original, Candidate);
      switch (Solver.check()) {
      case SatResult::Unsat:
        Result = EquivResult::Equal;
        break;
      case SatResult::Sat:
        Result = EquivResult::Different;
        Counterexample = Solver.model();
        break;
      case SatResult::Unknown:
        Result = EquivResult::Unknown;
        break;
      case SatResult::Invalid:
        Result = EquivResult::Invalid;
        break;
      }
      addStats(Solver.stats());
    }
  }

  switch (Result) {
  case EquivResult::Equal:
    Report.Proof = ProofStatus::Equivalent;
    clearRefutation();
    return symbolic::SynthVerification::Equivalent;
  case EquivResult::Different:
    Report.Proof = ProofStatus::Different;
    if (!Report.Counterexample.has_value()) {
      Report.RejectedCandidate = Candidate;
      Report.Counterexample = std::move(Counterexample);
    }
    return symbolic::SynthVerification::Different;
  case EquivResult::Unknown:
    Report.Proof = ProofStatus::Unknown;
    clearRefutation();
    return symbolic::SynthVerification::Unknown;
  case EquivResult::Invalid:
    Report.Proof = ProofStatus::Invalid;
    clearRefutation();
    // SynthVerification predates typed query errors.  Unknown is its
    // fail-closed compatibility value; Report retains the precise reason.
    return symbolic::SynthVerification::Unknown;
  }

  Report.Proof = ProofStatus::Invalid;
  clearRefutation();
  return symbolic::SynthVerification::Unknown;
}

} // namespace neverd::solver
