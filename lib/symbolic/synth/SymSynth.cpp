//===- SymSynth.cpp - Oracle-guided expression synthesis ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the entry points described in SymSynth.h: reduce, search, check,
/// put back what was hidden, and decide whether the answer was worth having.
///
/// The order of those last two matters.  The search works over a body whose
/// inputs are independent, and an accepted candidate is a statement about that
/// body — the strongest form of the statement, because independent inputs
/// range over everything the subterms behind them could have been.  Only after
/// the statement is settled are the subterms substituted back, which is an
/// exact rewriting rather than another thing to check.  Whether the result is
/// an improvement is then a question about the expression a reader would see,
/// so it is asked of the concrete form and not of the body: a candidate that
/// mentions one hidden subterm twice is short as a body and long as an answer.
///
/// The searches run in order of what they promise.  Enumeration goes first
/// because when it finds something, that something is the shortest expression
/// the grammar holds.  The sampler runs only on its failure, and its answer
/// carries no such claim — which is why the result says which one produced it.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymSynth.h"

#include "SymSynthDetail.h"

#include "llvm/Support/ErrorHandling.h"

#include <cstddef>
#include <optional>

namespace neverd::symbolic {

using namespace synth;

const char *synthOutcomeName(SynthOutcome Outcome) {
  switch (Outcome) {
  case SynthOutcome::NotApplicable:
    return "not-applicable";
  case SynthOutcome::AlreadyShortest:
    return "already-shortest";
  case SynthOutcome::TooManyInputs:
    return "too-many-inputs";
  case SynthOutcome::BudgetExhausted:
    return "budget-exhausted";
  case SynthOutcome::Counterexample:
    return "counterexample";
  case SynthOutcome::ProofIncomplete:
    return "proof-incomplete";
  case SynthOutcome::Synthesized:
    return "synthesized";
  }
  llvm_unreachable("unhandled synthesis outcome");
}

const char *synthVerificationName(SynthVerification Verification) {
  switch (Verification) {
  case SynthVerification::Equivalent:
    return "equivalent";
  case SynthVerification::Different:
    return "different";
  case SynthVerification::Unknown:
    return "unknown";
  }
  llvm_unreachable("unhandled synthesis verification");
}

const char *synthEvidenceName(SynthEvidence Evidence) {
  switch (Evidence) {
  case SynthEvidence::None:
    return "none";
  case SynthEvidence::Samples:
    return "samples";
  case SynthEvidence::Verifier:
    return "verifier";
  }
  llvm_unreachable("unhandled synthesis evidence");
}

namespace {

SynthResult synthesizeImpl(SymContext &Ctx, SymRef E, const SynthOptions &Opts,
                           std::optional<SynthVerifyFn> Verify) {
  SynthResult Result;
  Result.Expr = E;
  if (!E.isValid())
    return Result;

  Result.SizeBefore = Ctx.readabilityCost(E);
  Result.SizeAfter = Result.SizeBefore;

  SynthProblem Problem;
  switch (buildProblem(Ctx, E, Opts, Problem)) {
  case ProblemStatus::Trivial:
    return Result;
  case ProblemStatus::TooManyLeaves:
    Result.NumLeaves = static_cast<unsigned>(Problem.Leaves.size());
    Result.Outcome = SynthOutcome::TooManyInputs;
    return Result;
  case ProblemStatus::Ready:
    break;
  }
  Result.NumLeaves = static_cast<unsigned>(Problem.Leaves.size());

  const OpSemantics Sem(Ctx, Problem.Width);
  const Checker Check = makeChecker(Problem, Opts);
  SearchEffort Effort(Opts.MaxWork);

  SearchOutcome Found = enumerateShortest(Ctx, Problem, Opts, Sem, Effort,
                                          Check, Verify, Result.ProofQueries);
  bool FromSampler = false;
  if (!Found.Candidate.isValid() && !Found.StopForProof &&
      Opts.UseStochasticFallback) {
    SearchOutcome Sampled = searchStochastically(
        Ctx, Problem, Opts, Sem, Effort, Check, Verify, Result.ProofQueries);
    Sampled.SawRefuted |= Found.SawRefuted;
    Sampled.SawProofIncomplete |= Found.SawProofIncomplete;
    Sampled.StopForProof |= Found.StopForProof;
    Found = Sampled;
    FromSampler = Found.Candidate.isValid();
  }

  Result.Work = Effort.used();
  if (!Found.Candidate.isValid()) {
    if (Found.StopForProof) {
      Result.Outcome = SynthOutcome::ProofIncomplete;
      Result.Verification = SynthVerification::Unknown;
      return Result;
    }
    // Running out of budget with candidates left unbuilt is the one refusal
    // that says nothing about the expression, so it outranks a refutation,
    // which in turn outranks having searched and found nothing.
    if (Effort.empty())
      Result.Outcome = SynthOutcome::BudgetExhausted;
    else if (Found.SawProofIncomplete) {
      Result.Outcome = SynthOutcome::ProofIncomplete;
      Result.Verification = SynthVerification::Unknown;
    } else if (Found.SawRefuted) {
      Result.Outcome = SynthOutcome::Counterexample;
      Result.Verification = SynthVerification::Different;
    } else
      Result.Outcome = SynthOutcome::AlreadyShortest;
    return Result;
  }

  // Exact by substitution: the candidate was accepted over inputs that stand
  // for these subterms and range over everything they could be, so putting the
  // subterms back cannot make it disagree.
  const SymRef Concrete = Ctx.substitute(Found.Candidate, Problem.Hidden);
  const size_t Cost = Ctx.readabilityCost(Concrete);
  Result.Verification = Found.Evidence == SynthEvidence::Verifier
                            ? SynthVerification::Equivalent
                            : SynthVerification::Unknown;
  if (Concrete == E || (!Opts.AllowGrowth && Cost >= Result.SizeBefore)) {
    Result.Outcome = SynthOutcome::AlreadyShortest;
    return Result;
  }

  Result.Expr = Concrete;
  Result.Changed = true;
  Result.SizeAfter = Cost;
  Result.CandidateCost = Found.Cost;
  Result.Outcome = SynthOutcome::Synthesized;
  Result.Evidence = Found.Evidence;
  Result.FromSampler = FromSampler;
  return Result;
}

} // namespace

SynthResult synthesize(SymContext &Ctx, SymRef E, const SynthOptions &Opts,
                       SynthVerifyFn Verify) {
  if (!Verify) {
    SynthResult Result;
    Result.Expr = E;
    if (!E.isValid())
      return Result;
    Result.SizeBefore = Ctx.readabilityCost(E);
    Result.SizeAfter = Result.SizeBefore;
    Result.Outcome = SynthOutcome::ProofIncomplete;
    return Result;
  }
  return synthesizeImpl(Ctx, E, Opts, std::optional<SynthVerifyFn>(Verify));
}

SynthResult discoverSynthesisCandidate(SymContext &Ctx, SymRef E,
                                       const SynthOptions &Opts) {
  return synthesizeImpl(Ctx, E, Opts, std::nullopt);
}

SymRef synthesizeEquivalent(SymContext &, SymRef Input, const SynthOptions &) {
  return Input;
}

SymRef synthesizeEquivalent(SymContext &Ctx, SymRef Input,
                            const SynthOptions &Opts, SynthVerifyFn Verify) {
  return synthesize(Ctx, Input, Opts, Verify).Expr;
}

} // namespace neverd::symbolic
