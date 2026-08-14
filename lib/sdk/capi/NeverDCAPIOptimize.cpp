//===- NeverDCAPIOptimize.cpp - Transactional LLVM C API -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPIOptimize.h"

#include "SessionImpl.h"

#include "neverd/pipeline/Pipeline.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

using namespace neverd;
using namespace neverd::sdk;

namespace {

static_assert(static_cast<int>(OptimizationStopReason::Stable) ==
              NEVERD_OPTIMIZATION_STABLE);
static_assert(static_cast<int>(OptimizationStopReason::CycleDetected) ==
              NEVERD_OPTIMIZATION_CYCLE_DETECTED);
static_assert(static_cast<int>(OptimizationStopReason::BudgetExhausted) ==
              NEVERD_OPTIMIZATION_BUDGET_EXHAUSTED);
static_assert(static_cast<int>(OptimizationStopReason::VerificationFailed) ==
              NEVERD_OPTIMIZATION_VERIFICATION_FAILED);
static_assert(static_cast<int>(OptimizationStopReason::InputInvalid) ==
              NEVERD_OPTIMIZATION_INPUT_INVALID);

constexpr unsigned kDefaultMaxRounds = 8;
constexpr uint64_t kDefaultSolverMaxConflicts = uint64_t(1) << 18;
constexpr uint64_t kDefaultSolverMaxPropagations = uint64_t(1) << 24;
constexpr uint64_t kDefaultSolverMaxWatchVisits = uint64_t(1) << 26;

#define FIELD_END(Type, Field)                                                 \
  (offsetof(Type, Field) + sizeof(static_cast<Type *>(nullptr)->Field))

bool reaches(size_t Size, size_t End) { return Size >= End; }

bool hasSynthesisPolicy(const neverd_optimize_llvm_ir_options &Options,
                        size_t Size) {
#define PRESENT(Field)                                                         \
  (reaches(Size, FIELD_END(neverd_optimize_llvm_ir_options, Field)) &&         \
   Options.Field != 0)
  return PRESENT(synthesis_max_cost) || PRESENT(synthesis_max_samples) ||
         PRESENT(synthesis_verify_samples) || PRESENT(synthesis_max_work) ||
         PRESENT(synthesis_max_leaves) || PRESENT(synthesis_max_constants) ||
         PRESENT(synthesis_stochastic_slots) ||
         PRESENT(synthesis_stochastic_restarts) ||
         PRESENT(synthesis_stochastic_iterations) ||
         PRESENT(solver_max_conflicts) || PRESENT(solver_max_propagations) ||
         PRESENT(solver_max_watch_visits);
#undef PRESENT
}

struct OptimizationConfig {
  bool Valid = true;
  std::string Error;
  Pipeline::OptimizationOptions Options;
};

OptimizationConfig readOptions(const neverd_optimize_llvm_ir_options *In) {
  OptimizationConfig Config;
  Config.Options.MaxRounds = kDefaultMaxRounds;
  Config.Options.Semantic.Provider = ProofProvider::BuiltInSolver;
  Config.Options.Semantic.Solver.Sat.MaxConflicts = kDefaultSolverMaxConflicts;
  Config.Options.Semantic.Solver.Sat.MaxPropagations =
      kDefaultSolverMaxPropagations;
  Config.Options.Semantic.Solver.Sat.MaxWatchVisits =
      kDefaultSolverMaxWatchVisits;
  if (!In)
    return Config;

  const size_t Size = In->struct_size;
  const neverd_optimization_mode_t Mode =
      reaches(Size, FIELD_END(neverd_optimize_llvm_ir_options, mode))
          ? In->mode
          : NEVERD_OPTIMIZATION_MODE_DEFAULT;
  switch (Mode) {
  case NEVERD_OPTIMIZATION_MODE_DEFAULT:
  case NEVERD_OPTIMIZATION_MODE_DEEP:
    Config.Options.Conservative = false;
    Config.Options.Strength = Pipeline::OptStrength::Deep;
    break;
  case NEVERD_OPTIMIZATION_MODE_CONSERVATIVE:
    Config.Options.Conservative = true;
    Config.Options.Strength = Pipeline::OptStrength::Deep;
    break;
  case NEVERD_OPTIMIZATION_MODE_THIN:
    Config.Options.Conservative = false;
    Config.Options.Strength = Pipeline::OptStrength::Thin;
    break;
  default:
    Config.Valid = false;
    Config.Error = "invalid optimization mode";
    return Config;
  }

  const neverd_llvm_optimization_level_t Level =
      reaches(Size, FIELD_END(neverd_optimize_llvm_ir_options, llvm_level))
          ? In->llvm_level
          : NEVERD_LLVM_OPTIMIZATION_DEFAULT;
  switch (Level) {
  case NEVERD_LLVM_OPTIMIZATION_DEFAULT:
  case NEVERD_LLVM_OPTIMIZATION_O2:
    Config.Options.LLVMLevel = llvm::OptimizationLevel::O2;
    break;
  case NEVERD_LLVM_OPTIMIZATION_O0:
    Config.Options.LLVMLevel = llvm::OptimizationLevel::O0;
    break;
  case NEVERD_LLVM_OPTIMIZATION_O1:
    Config.Options.LLVMLevel = llvm::OptimizationLevel::O1;
    break;
  case NEVERD_LLVM_OPTIMIZATION_O3:
    Config.Options.LLVMLevel = llvm::OptimizationLevel::O3;
    break;
  default:
    Config.Valid = false;
    Config.Error = "invalid LLVM optimization level";
    return Config;
  }

  if (reaches(Size, FIELD_END(neverd_optimize_llvm_ir_options, max_rounds)) &&
      In->max_rounds)
    Config.Options.MaxRounds = In->max_rounds;
  if (reaches(Size,
              FIELD_END(neverd_optimize_llvm_ir_options, enable_synthesis)))
    Config.Options.Semantic.EnableSynthesis = In->enable_synthesis != 0;
  if (Config.Options.Conservative && Config.Options.Semantic.EnableSynthesis) {
    Config.Valid = false;
    Config.Error = "conservative mode cannot enable synthesis";
    return Config;
  }
  if (!Config.Options.Semantic.EnableSynthesis &&
      hasSynthesisPolicy(*In, Size)) {
    Config.Valid = false;
    Config.Error = "synthesis options require enable_synthesis";
    return Config;
  }

  symbolic::SynthOptions &Search = Config.Options.Semantic.Synthesis;
  if (reaches(Size,
              FIELD_END(neverd_optimize_llvm_ir_options, synthesis_max_cost)) &&
      In->synthesis_max_cost)
    Search.MaxCost = In->synthesis_max_cost;
  if (reaches(Size, FIELD_END(neverd_optimize_llvm_ir_options,
                              synthesis_max_samples)) &&
      In->synthesis_max_samples)
    Search.MaxSamples = In->synthesis_max_samples;
  if (reaches(Size, FIELD_END(neverd_optimize_llvm_ir_options,
                              synthesis_verify_samples)) &&
      In->synthesis_verify_samples)
    Search.VerifySamples = In->synthesis_verify_samples;
  if (reaches(Size,
              FIELD_END(neverd_optimize_llvm_ir_options, synthesis_max_work)) &&
      In->synthesis_max_work)
    Search.MaxWork = In->synthesis_max_work;
  if (reaches(Size, FIELD_END(neverd_optimize_llvm_ir_options,
                              synthesis_max_leaves)) &&
      In->synthesis_max_leaves)
    Search.MaxLeaves = In->synthesis_max_leaves;
  if (reaches(Size, FIELD_END(neverd_optimize_llvm_ir_options,
                              synthesis_max_constants)) &&
      In->synthesis_max_constants)
    Search.MaxConstants = In->synthesis_max_constants;
  if (reaches(Size, FIELD_END(neverd_optimize_llvm_ir_options,
                              synthesis_stochastic_slots)) &&
      In->synthesis_stochastic_slots)
    Search.StochasticSlots = In->synthesis_stochastic_slots;
  if (reaches(Size, FIELD_END(neverd_optimize_llvm_ir_options,
                              synthesis_stochastic_restarts)) &&
      In->synthesis_stochastic_restarts)
    Search.StochasticRestarts = In->synthesis_stochastic_restarts;
  if (reaches(Size, FIELD_END(neverd_optimize_llvm_ir_options,
                              synthesis_stochastic_iterations)) &&
      In->synthesis_stochastic_iterations)
    Search.StochasticIterations = In->synthesis_stochastic_iterations;

  solver::SatOptions &Sat = Config.Options.Semantic.Solver.Sat;
  if (reaches(Size, FIELD_END(neverd_optimize_llvm_ir_options,
                              solver_max_conflicts)) &&
      In->solver_max_conflicts)
    Sat.MaxConflicts = In->solver_max_conflicts;
  if (reaches(Size, FIELD_END(neverd_optimize_llvm_ir_options,
                              solver_max_propagations)) &&
      In->solver_max_propagations)
    Sat.MaxPropagations = In->solver_max_propagations;
  if (reaches(Size, FIELD_END(neverd_optimize_llvm_ir_options,
                              solver_max_watch_visits)) &&
      In->solver_max_watch_visits)
    Sat.MaxWatchVisits = In->solver_max_watch_visits;

  const bool Exhaustive =
      reaches(Size, FIELD_END(neverd_optimize_llvm_ir_options, exhaustive)) &&
      In->exhaustive != 0;
  if (Exhaustive) {
    Config.Options.MaxRounds = 0;
    Search.MaxWork = std::numeric_limits<size_t>::max();
    Search.StochasticRestarts = std::numeric_limits<unsigned>::max();
    Search.StochasticIterations = std::numeric_limits<size_t>::max();
    Config.Options.Semantic.Solver = solver::SolverOptions::unlimited();
  }
  return Config;
}

struct OptimizedModule {
  bool Ok = false;
  std::string Error;
  size_t ErrorLine = 0;
  size_t ErrorColumn = 0;
  std::string OutputIR;
  bool HasOutput = false;
  OptimizationResult Result;
};

OptimizedModule optimizeIR(const char *IR, const OptimizationConfig &Config) {
  OptimizedModule Out;
  if (!Config.Valid) {
    Out.Error = Config.Error;
    Out.Result.Stop = OptimizationStopReason::InputInvalid;
    return Out;
  }
  if (!IR) {
    Out.Error = "no LLVM IR given";
    Out.Result.Stop = OptimizationStopReason::InputInvalid;
    return Out;
  }

  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  std::unique_ptr<llvm::Module> Module =
      llvm::parseAssemblyString(IR, Diagnostic, Context);
  if (!Module) {
    Out.Error = Diagnostic.getMessage().str();
    Out.ErrorLine = Diagnostic.getLineNo() > 0
                        ? static_cast<size_t>(Diagnostic.getLineNo())
                        : 0;
    Out.ErrorColumn = Diagnostic.getColumnNo() > 0
                          ? static_cast<size_t>(Diagnostic.getColumnNo())
                          : 0;
    Out.Result.Stop = OptimizationStopReason::InputInvalid;
    return Out;
  }

  Out.Result = Pipeline::optimizeModule(*Module, Config.Options);
  Out.Ok = Out.Result.Stop != OptimizationStopReason::InputInvalid &&
           Out.Result.Stop != OptimizationStopReason::VerificationFailed;
  if (!Out.Ok)
    Out.Error = Out.Result.Stop == OptimizationStopReason::InputInvalid
                    ? "input module failed optimization validation"
                    : "optimized module failed verification";

  llvm::raw_string_ostream Stream(Out.OutputIR);
  Module->print(Stream, nullptr);
  Stream.flush();
  Out.HasOutput = true;
  return Out;
}

void writeResult(const OptimizedModule &From,
                 neverd_optimize_llvm_ir_result *To) {
  const size_t Size = To->struct_size;
#define SET(Field, Value)                                                      \
  do {                                                                         \
    if (reaches(Size, FIELD_END(neverd_optimize_llvm_ir_result, Field)))       \
      To->Field = (Value);                                                     \
  } while (0)

  SET(ok, From.Ok ? 1 : 0);
  SET(error, From.Error.empty() ? nullptr : dupStr(From.Error));
  SET(error_line, From.ErrorLine);
  SET(error_column, From.ErrorColumn);
  SET(output_ir, From.HasOutput ? dupStr(From.OutputIR) : nullptr);
  SET(changed, From.Result.Changed ? 1 : 0);
  SET(stop, static_cast<neverd_optimization_stop_t>(From.Result.Stop));
  SET(functions_visited, From.Result.FunctionsVisited);
  SET(rounds, From.Result.Rounds);
  SET(semantic_rewrites, From.Result.SemanticRewrites);
  SET(search_work, From.Result.SearchWork);
  SET(proof_queries, From.Result.ProofWork.Queries);
  SET(proof_conflicts, From.Result.ProofWork.Conflicts);
  SET(proof_propagations, From.Result.ProofWork.Propagations);
  SET(proof_watch_visits, From.Result.ProofWork.WatchVisits);
#undef SET
}

} // namespace

extern "C" {

const char *neverd_optimization_stop_name(neverd_optimization_stop_t Stop) {
  switch (Stop) {
  case NEVERD_OPTIMIZATION_STABLE:
    return "stable";
  case NEVERD_OPTIMIZATION_CYCLE_DETECTED:
    return "cycle-detected";
  case NEVERD_OPTIMIZATION_BUDGET_EXHAUSTED:
    return "budget-exhausted";
  case NEVERD_OPTIMIZATION_VERIFICATION_FAILED:
    return "verification-failed";
  case NEVERD_OPTIMIZATION_INPUT_INVALID:
    return "input-invalid";
  }
  return "invalid";
}

int neverd_optimize_llvm_ir(const char *IR,
                            const neverd_optimize_llvm_ir_options *Options,
                            neverd_optimize_llvm_ir_result *Result) {
  if (!Result || !reaches(Result->struct_size,
                          FIELD_END(neverd_optimize_llvm_ir_result, ok)))
    return 1;
  writeResult(optimizeIR(IR, readOptions(Options)), Result);
  return 0;
}

void neverd_optimize_llvm_ir_result_dispose(
    neverd_optimize_llvm_ir_result *Result) {
  if (!Result)
    return;
  const size_t Size = Result->struct_size;
#define RELEASE(Field)                                                         \
  do {                                                                         \
    if (reaches(Size, FIELD_END(neverd_optimize_llvm_ir_result, Field))) {     \
      neverd_free_string(Result->Field);                                       \
      Result->Field = nullptr;                                                 \
    }                                                                          \
  } while (0)

  RELEASE(error);
  RELEASE(output_ir);
#undef RELEASE
}

const char *neverd_optimize_llvm_ir_json_v1(
    const char *IR, const neverd_optimize_llvm_ir_options *Options) {
  neverd_optimize_llvm_ir_result Result{};
  Result.struct_size = sizeof(Result);
  const int Status = neverd_optimize_llvm_ir(IR, Options, &Result);

  llvm::json::Object Object{
      {"schemaVersion", 1},
      {"ok", Status == 0 && Result.ok != 0},
      {"stop", neverd_optimization_stop_name(Result.stop)},
      {"changed", Result.changed != 0},
      {"functionsVisited", Result.functions_visited},
      {"rounds", Result.rounds},
      {"semanticRewrites", Result.semantic_rewrites},
      {"searchWork", Result.search_work},
      {"proofQueries", Result.proof_queries},
      {"proofConflicts", Result.proof_conflicts},
      {"proofPropagations", Result.proof_propagations},
      {"proofWatchVisits", Result.proof_watch_visits},
  };
  if (Status != 0) {
    Object["error"] = "invalid result buffer";
  } else {
    if (Result.error)
      Object["error"] = Result.error;
    if (Result.error_line)
      Object["errorLine"] = Result.error_line;
    if (Result.error_column)
      Object["errorColumn"] = Result.error_column;
    if (Result.output_ir)
      Object["outputIR"] = Result.output_ir;
  }

  std::string JSON = jsonToString(llvm::json::Value(std::move(Object)));
  neverd_optimize_llvm_ir_result_dispose(&Result);
  return dupStr(JSON);
}

} // extern "C"
