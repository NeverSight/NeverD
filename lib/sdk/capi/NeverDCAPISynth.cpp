//===- NeverDCAPISynth.cpp - Proof-gated synthesis C API -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPISynth.h"

#include "SessionImpl.h"

#include "neverd/pass/ir/simplify/SymSimplifyPass.h"
#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/symbolic/SymParse.h"
#include "neverd/symbolic/SymSynth.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

using namespace neverd;
using namespace neverd::sdk;

namespace {

static_assert(static_cast<int>(solver::ProofStatus::NotRun) ==
              NEVERD_PROOF_NOT_RUN);
static_assert(static_cast<int>(solver::ProofStatus::Equivalent) ==
              NEVERD_PROOF_EQUIVALENT);
static_assert(static_cast<int>(solver::ProofStatus::Different) ==
              NEVERD_PROOF_DIFFERENT);
static_assert(static_cast<int>(solver::ProofStatus::Unknown) ==
              NEVERD_PROOF_UNKNOWN);
static_assert(static_cast<int>(solver::ProofStatus::Invalid) ==
              NEVERD_PROOF_INVALID);

static_assert(static_cast<int>(SymSimplifyOutcome::NotApplicable) ==
              NEVERD_SYNTHESIS_NOT_APPLICABLE);
static_assert(static_cast<int>(SymSimplifyOutcome::AlreadyShortest) ==
              NEVERD_SYNTHESIS_ALREADY_SHORTEST);
static_assert(static_cast<int>(SymSimplifyOutcome::TooManyInputs) ==
              NEVERD_SYNTHESIS_TOO_MANY_INPUTS);
static_assert(static_cast<int>(SymSimplifyOutcome::SearchBudgetExhausted) ==
              NEVERD_SYNTHESIS_SEARCH_BUDGET_EXHAUSTED);
static_assert(static_cast<int>(SymSimplifyOutcome::Counterexample) ==
              NEVERD_SYNTHESIS_COUNTEREXAMPLE);
static_assert(static_cast<int>(SymSimplifyOutcome::ProofIncomplete) ==
              NEVERD_SYNTHESIS_PROOF_INCOMPLETE);
static_assert(static_cast<int>(SymSimplifyOutcome::Rewritten) ==
              NEVERD_SYNTHESIS_REWRITTEN);

static_assert(static_cast<int>(symbolic::SynthOutcome::NotApplicable) ==
              NEVERD_SYNTHESIS_NOT_APPLICABLE);
static_assert(static_cast<int>(symbolic::SynthOutcome::AlreadyShortest) ==
              NEVERD_SYNTHESIS_ALREADY_SHORTEST);
static_assert(static_cast<int>(symbolic::SynthOutcome::TooManyInputs) ==
              NEVERD_SYNTHESIS_TOO_MANY_INPUTS);
static_assert(static_cast<int>(symbolic::SynthOutcome::BudgetExhausted) ==
              NEVERD_SYNTHESIS_SEARCH_BUDGET_EXHAUSTED);
static_assert(static_cast<int>(symbolic::SynthOutcome::Counterexample) ==
              NEVERD_SYNTHESIS_COUNTEREXAMPLE);
static_assert(static_cast<int>(symbolic::SynthOutcome::ProofIncomplete) ==
              NEVERD_SYNTHESIS_PROOF_INCOMPLETE);
static_assert(static_cast<int>(symbolic::SynthOutcome::Synthesized) ==
              NEVERD_SYNTHESIS_REWRITTEN);

constexpr unsigned kDefaultWidth = 32;

// Unlike Solver's C++ defaults, the public C surface is bounded unless the
// caller explicitly asks for exhaustive work.  Powers of two make the three
// independent ceilings predictable and leave ordinary 32-bit proofs ample
// room without turning a zeroed options struct into an unbounded request.
constexpr uint64_t kDefaultSolverMaxConflicts = uint64_t(1) << 18;
constexpr uint64_t kDefaultSolverMaxPropagations = uint64_t(1) << 24;
constexpr uint64_t kDefaultSolverMaxWatchVisits = uint64_t(1) << 26;

#define FIELD_END(Type, Field)                                                 \
  (offsetof(Type, Field) + sizeof(static_cast<Type *>(nullptr)->Field))

bool reaches(size_t Size, size_t End) { return Size >= End; }

uint64_t workCount(size_t Work) {
  if constexpr (sizeof(size_t) > sizeof(uint64_t))
    if (Work > std::numeric_limits<uint64_t>::max())
      return std::numeric_limits<uint64_t>::max();
  return static_cast<uint64_t>(Work);
}

struct SynthesisConfig {
  unsigned Width = kDefaultWidth;
  symbolic::SynthOptions Search;
  solver::SolverOptions Solver;
  symbolic::SymParseOptions Parse;
};

SynthesisConfig readOptions(const neverd_synthesize_options *In) {
  SynthesisConfig Config;
  Config.Solver.Sat.MaxConflicts = kDefaultSolverMaxConflicts;
  Config.Solver.Sat.MaxPropagations = kDefaultSolverMaxPropagations;
  Config.Solver.Sat.MaxWatchVisits = kDefaultSolverMaxWatchVisits;
  if (!In)
    return Config;

  const size_t Size = In->struct_size;
  if (reaches(Size, FIELD_END(neverd_synthesize_options, width)) && In->width)
    Config.Width = In->width;
  if (reaches(Size, FIELD_END(neverd_synthesize_options, max_cost)) &&
      In->max_cost)
    Config.Search.MaxCost = In->max_cost;
  if (reaches(Size, FIELD_END(neverd_synthesize_options, max_samples)) &&
      In->max_samples)
    Config.Search.MaxSamples = In->max_samples;
  if (reaches(Size, FIELD_END(neverd_synthesize_options, verify_samples)) &&
      In->verify_samples)
    Config.Search.VerifySamples = In->verify_samples;
  if (reaches(Size, FIELD_END(neverd_synthesize_options, max_work)) &&
      In->max_work)
    Config.Search.MaxWork = In->max_work;
  if (reaches(Size, FIELD_END(neverd_synthesize_options, max_leaves)) &&
      In->max_leaves)
    Config.Search.MaxLeaves = In->max_leaves;
  if (reaches(Size, FIELD_END(neverd_synthesize_options, max_constants)) &&
      In->max_constants)
    Config.Search.MaxConstants = In->max_constants;
  if (reaches(Size, FIELD_END(neverd_synthesize_options, stochastic_slots)) &&
      In->stochastic_slots)
    Config.Search.StochasticSlots = In->stochastic_slots;
  if (reaches(Size,
              FIELD_END(neverd_synthesize_options, stochastic_restarts)) &&
      In->stochastic_restarts)
    Config.Search.StochasticRestarts = In->stochastic_restarts;
  if (reaches(Size,
              FIELD_END(neverd_synthesize_options, stochastic_iterations)) &&
      In->stochastic_iterations)
    Config.Search.StochasticIterations = In->stochastic_iterations;
  if (reaches(Size,
              FIELD_END(neverd_synthesize_options, solver_max_conflicts)) &&
      In->solver_max_conflicts)
    Config.Solver.Sat.MaxConflicts = In->solver_max_conflicts;
  if (reaches(Size,
              FIELD_END(neverd_synthesize_options, solver_max_propagations)) &&
      In->solver_max_propagations)
    Config.Solver.Sat.MaxPropagations = In->solver_max_propagations;
  if (reaches(Size,
              FIELD_END(neverd_synthesize_options, solver_max_watch_visits)) &&
      In->solver_max_watch_visits)
    Config.Solver.Sat.MaxWatchVisits = In->solver_max_watch_visits;

  const bool Exhaustive =
      reaches(Size, FIELD_END(neverd_synthesize_options, exhaustive)) &&
      In->exhaustive != 0;
  if (Exhaustive) {
    Config.Search.MaxWork = std::numeric_limits<size_t>::max();
    Config.Search.StochasticRestarts = std::numeric_limits<unsigned>::max();
    Config.Search.StochasticIterations = std::numeric_limits<size_t>::max();
    Config.Parse = symbolic::SymParseOptions::unlimited();
    Config.Solver = solver::SolverOptions::unlimited();
  }
  return Config;
}

neverd_synthesis_outcome_t mapOutcome(symbolic::SynthOutcome Outcome) {
  switch (Outcome) {
  case symbolic::SynthOutcome::NotApplicable:
    return NEVERD_SYNTHESIS_NOT_APPLICABLE;
  case symbolic::SynthOutcome::AlreadyShortest:
    return NEVERD_SYNTHESIS_ALREADY_SHORTEST;
  case symbolic::SynthOutcome::TooManyInputs:
    return NEVERD_SYNTHESIS_TOO_MANY_INPUTS;
  case symbolic::SynthOutcome::BudgetExhausted:
    return NEVERD_SYNTHESIS_SEARCH_BUDGET_EXHAUSTED;
  case symbolic::SynthOutcome::Counterexample:
    return NEVERD_SYNTHESIS_COUNTEREXAMPLE;
  case symbolic::SynthOutcome::ProofIncomplete:
    return NEVERD_SYNTHESIS_PROOF_INCOMPLETE;
  case symbolic::SynthOutcome::Synthesized:
    return NEVERD_SYNTHESIS_REWRITTEN;
  }
  return NEVERD_SYNTHESIS_NOT_APPLICABLE;
}

neverd_proof_status_t mapProof(const symbolic::SynthResult &Result,
                               const solver::SymSynthProofReport &Report) {
  if (Report.Proof == solver::ProofStatus::Invalid)
    return NEVERD_PROOF_INVALID;

  switch (Result.Outcome) {
  case symbolic::SynthOutcome::Synthesized:
    return Result.Verification == symbolic::SynthVerification::Equivalent
               ? NEVERD_PROOF_EQUIVALENT
               : NEVERD_PROOF_UNKNOWN;
  case symbolic::SynthOutcome::Counterexample:
    return NEVERD_PROOF_DIFFERENT;
  case symbolic::SynthOutcome::ProofIncomplete:
    return NEVERD_PROOF_UNKNOWN;
  case symbolic::SynthOutcome::NotApplicable:
  case symbolic::SynthOutcome::AlreadyShortest:
  case symbolic::SynthOutcome::TooManyInputs:
  case symbolic::SynthOutcome::BudgetExhausted:
    return NEVERD_PROOF_NOT_RUN;
  }
  return NEVERD_PROOF_NOT_RUN;
}

std::string fixedWidthHex(const llvm::APInt &Value, uint32_t Width) {
  llvm::APInt Normalized = Value.zextOrTrunc(Width);
  llvm::SmallString<64> Digits;
  Normalized.toString(Digits, 16, /*Signed=*/false,
                      /*formatAsCLiteral=*/false, /*UpperCase=*/false);
  const size_t Required = (static_cast<size_t>(Width) + 3) / 4;
  std::string Text = "0x";
  if (Digits.size() < Required)
    Text.append(Required - Digits.size(), '0');
  Text.append(Digits.begin(), Digits.end());
  return Text;
}

std::optional<std::string>
counterexampleJSON(const symbolic::SymContext &Ctx,
                   const solver::SymSynthProofReport &Report) {
  if (Report.Proof != solver::ProofStatus::Different ||
      !Report.RejectedCandidate.isValid() || !Report.Counterexample)
    return std::nullopt;

  SymSimplifyCounterexample Counterexample;
  Counterexample.Candidate = Ctx.toString(Report.RejectedCandidate);
  for (uint32_t Id : Report.Counterexample->vars()) {
    if (Id >= Ctx.numVars())
      continue;
    std::optional<llvm::APInt> Value = Report.Counterexample->value(Id);
    if (!Value)
      continue;
    const symbolic::SymVarInfo &Info = Ctx.varInfo(Id);
    Counterexample.Variables.push_back(
        {Id, Info.Width, Info.Name, fixedWidthHex(*Value, Info.Width)});
  }
  return Counterexample.toJson();
}

struct SynthesisResult {
  bool Ok = false;
  std::string Error;
  size_t ErrorOffset = 0;
  std::string Input;
  std::string Output;
  bool Changed = false;
  size_t CostBefore = 0;
  size_t CostAfter = 0;
  unsigned Inputs = 0;
  size_t CandidateCost = 0;
  neverd_synthesis_outcome_t Outcome = NEVERD_SYNTHESIS_NOT_APPLICABLE;
  neverd_proof_status_t Proof = NEVERD_PROOF_NOT_RUN;
  uint64_t SearchWork = 0;
  solver::ProofStats ProofWork;
  std::optional<std::string> CounterexampleJSON;
};

SynthesisResult synthesizeExpression(const char *Expr,
                                     const SynthesisConfig &Config) {
  SynthesisResult Out;
  if (!Expr) {
    Out.Error = "no expression given";
    return Out;
  }

  symbolic::SymContext Ctx;
  symbolic::SymParseResult Parsed =
      symbolic::parseSymExpr(Ctx, Expr, Config.Width, Config.Parse);
  if (!Parsed.ok()) {
    Out.Error = Parsed.Error;
    Out.ErrorOffset = Parsed.ErrorOffset;
    return Out;
  }

  solver::SymSynthVerifier Verifier(Config.Solver);
  symbolic::SynthResult Search = symbolic::synthesize(
      Ctx, Parsed.Root, Config.Search,
      [&](symbolic::SymContext &VerifyCtx, symbolic::SymRef Original,
          symbolic::SymRef Candidate) {
        return Verifier(VerifyCtx, Original, Candidate);
      });
  const solver::SymSynthProofReport &Report = Verifier.report();

  Out.Ok = true;
  Out.Input = Ctx.toString(Parsed.Root);
  Out.Output = Ctx.toString(Search.Expr);
  Out.Changed = Search.Changed;
  Out.CostBefore = Search.SizeBefore;
  Out.CostAfter = Search.SizeAfter;
  Out.Inputs = Search.NumLeaves;
  Out.CandidateCost = Search.CandidateCost;
  Out.Outcome = mapOutcome(Search.Outcome);
  Out.Proof = mapProof(Search, Report);
  Out.SearchWork = workCount(Search.Work);
  Out.ProofWork = Report.Stats;

  // Treat the solver report as the final authority even if an upstream search
  // invariant regresses: a production rewrite never crosses this ABI without
  // an Equivalent disposition.
  if (Out.Changed && Report.Proof != solver::ProofStatus::Equivalent) {
    Out.Output = Out.Input;
    Out.Changed = false;
    Out.CostAfter = Out.CostBefore;
    Out.CandidateCost = 0;
    Out.Outcome = NEVERD_SYNTHESIS_PROOF_INCOMPLETE;
    Out.Proof = Report.Proof == solver::ProofStatus::Invalid
                    ? NEVERD_PROOF_INVALID
                    : NEVERD_PROOF_UNKNOWN;
  }

  if (Out.Proof == NEVERD_PROOF_DIFFERENT)
    Out.CounterexampleJSON = counterexampleJSON(Ctx, Report);
  return Out;
}

void writeResult(const SynthesisResult &From, neverd_synthesize_result *To) {
  const size_t Size = To->struct_size;
#define SET(Field, Value)                                                      \
  do {                                                                         \
    if (reaches(Size, FIELD_END(neverd_synthesize_result, Field)))             \
      To->Field = (Value);                                                     \
  } while (0)

  SET(ok, From.Ok ? 1 : 0);
  SET(error, From.Error.empty() ? nullptr : dupStr(From.Error));
  SET(error_offset, From.ErrorOffset);
  SET(input, From.Ok ? dupStr(From.Input) : nullptr);
  SET(output, From.Ok ? dupStr(From.Output) : nullptr);
  SET(changed, From.Changed ? 1 : 0);
  SET(cost_before, From.CostBefore);
  SET(cost_after, From.CostAfter);
  SET(inputs, From.Inputs);
  SET(candidate_cost, From.CandidateCost);
  SET(outcome, From.Outcome);
  SET(proof_status, From.Proof);
  SET(search_work, From.SearchWork);
  SET(proof_queries, From.ProofWork.Queries);
  SET(proof_conflicts, From.ProofWork.Conflicts);
  SET(proof_propagations, From.ProofWork.Propagations);
  SET(proof_watch_visits, From.ProofWork.WatchVisits);
  SET(counterexample_json,
      From.CounterexampleJSON ? dupStr(*From.CounterexampleJSON) : nullptr);
#undef SET
}

} // namespace

extern "C" {

const char *neverd_proof_status_name(neverd_proof_status_t Status) {
  switch (Status) {
  case NEVERD_PROOF_NOT_RUN:
    return "not-run";
  case NEVERD_PROOF_EQUIVALENT:
    return "equivalent";
  case NEVERD_PROOF_DIFFERENT:
    return "different";
  case NEVERD_PROOF_UNKNOWN:
    return "unknown";
  case NEVERD_PROOF_INVALID:
    return "invalid";
  }
  return "invalid";
}

const char *neverd_synthesis_outcome_name(neverd_synthesis_outcome_t Outcome) {
  switch (Outcome) {
  case NEVERD_SYNTHESIS_NOT_APPLICABLE:
    return "not-applicable";
  case NEVERD_SYNTHESIS_ALREADY_SHORTEST:
    return "already-shortest";
  case NEVERD_SYNTHESIS_TOO_MANY_INPUTS:
    return "too-many-inputs";
  case NEVERD_SYNTHESIS_SEARCH_BUDGET_EXHAUSTED:
    return "search-budget-exhausted";
  case NEVERD_SYNTHESIS_COUNTEREXAMPLE:
    return "counterexample";
  case NEVERD_SYNTHESIS_PROOF_INCOMPLETE:
    return "proof-incomplete";
  case NEVERD_SYNTHESIS_REWRITTEN:
    return "rewritten";
  }
  return "invalid";
}

int neverd_synthesize_expr(const char *Expr,
                           const neverd_synthesize_options *Options,
                           neverd_synthesize_result *Result) {
  if (!Result ||
      !reaches(Result->struct_size, FIELD_END(neverd_synthesize_result, ok)))
    return 1;
  writeResult(synthesizeExpression(Expr, readOptions(Options)), Result);
  return 0;
}

void neverd_synthesize_result_dispose(neverd_synthesize_result *Result) {
  if (!Result)
    return;
  const size_t Size = Result->struct_size;
#define RELEASE(Field)                                                         \
  do {                                                                         \
    if (reaches(Size, FIELD_END(neverd_synthesize_result, Field))) {           \
      neverd_free_string(Result->Field);                                       \
      Result->Field = nullptr;                                                 \
    }                                                                          \
  } while (0)

  RELEASE(error);
  RELEASE(input);
  RELEASE(output);
  RELEASE(counterexample_json);
#undef RELEASE
}

const char *
neverd_synthesize_expr_json_v1(const char *Expr,
                               const neverd_synthesize_options *Options) {
  neverd_synthesize_result Result{};
  Result.struct_size = sizeof(Result);
  const int Status = neverd_synthesize_expr(Expr, Options, &Result);

  llvm::json::Object Object{
      {"schemaVersion", 1},
      {"ok", Status == 0 && Result.ok != 0},
      {"outcome", neverd_synthesis_outcome_name(Result.outcome)},
      {"proofStatus", neverd_proof_status_name(Result.proof_status)},
  };
  if (Status != 0) {
    Object["error"] = "invalid result buffer";
  } else if (!Result.ok) {
    Object["error"] = Result.error ? Result.error : "invalid expression";
    Object["errorOffset"] = Result.error_offset;
  } else {
    Object["input"] = Result.input ? Result.input : "";
    Object["output"] = Result.output ? Result.output : "";
    Object["changed"] = Result.changed != 0;
    Object["costBefore"] = Result.cost_before;
    Object["costAfter"] = Result.cost_after;
    Object["inputs"] = Result.inputs;
    Object["candidateCost"] = Result.candidate_cost;
    Object["searchWork"] = Result.search_work;
    Object["proofQueries"] = Result.proof_queries;
    Object["proofConflicts"] = Result.proof_conflicts;
    Object["proofPropagations"] = Result.proof_propagations;
    Object["proofWatchVisits"] = Result.proof_watch_visits;
    if (Result.counterexample_json) {
      llvm::Expected<llvm::json::Value> Counterexample =
          llvm::json::parse(Result.counterexample_json);
      if (Counterexample)
        Object["counterexample"] = std::move(*Counterexample);
      else
        llvm::consumeError(Counterexample.takeError());
    }
  }

  std::string JSON = jsonToString(llvm::json::Value(std::move(Object)));
  neverd_synthesize_result_dispose(&Result);
  return dupStr(JSON);
}

} // extern "C"
