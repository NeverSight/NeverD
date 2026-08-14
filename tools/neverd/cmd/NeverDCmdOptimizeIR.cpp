//===- NeverDCmdOptimizeIR.cpp - The optimize-ir subcommand -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `neverd optimize-ir` — run the public transactional optimizer over textual
/// LLVM IR.  This is intentionally the same C entry point used by plugins, so
/// parse diagnostics, proof telemetry, and commit policy cannot drift between
/// the command line and the SDK.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

using namespace llvm;

namespace neverd::cli {

namespace {

enum ExitStatus : int {
  ExitSuccess = 0,
  ExitInvalidInput = 2,
  ExitIncomplete = 3,
  ExitEngineFailure = 4,
};

bool fitsSize(unsigned long long Value) {
  if constexpr (sizeof(size_t) >= sizeof(unsigned long long))
    return true;
  return Value <= std::numeric_limits<size_t>::max();
}

bool buildOptions(neverd_optimize_llvm_ir_options &Options) {
  const unsigned long long SizeValues[] = {
      OptimizeIRSynthesisMaxCost,
      OptimizeIRSynthesisMaxSamples,
      OptimizeIRSynthesisMaxWork,
      OptimizeIRSynthesisStochasticIterations,
  };
  for (unsigned long long Value : SizeValues) {
    if (fitsSize(Value))
      continue;
    WithColor::error()
        << "a synthesis size option exceeds this build's size_t\n";
    return false;
  }
  const bool HasSynthesisOption =
      OptimizeIRSynthesisMaxCost.getNumOccurrences() != 0 ||
      OptimizeIRSynthesisMaxSamples.getNumOccurrences() != 0 ||
      OptimizeIRSynthesisVerifySamples.getNumOccurrences() != 0 ||
      OptimizeIRSynthesisMaxWork.getNumOccurrences() != 0 ||
      OptimizeIRSynthesisMaxLeaves.getNumOccurrences() != 0 ||
      OptimizeIRSynthesisMaxConstants.getNumOccurrences() != 0 ||
      OptimizeIRSynthesisStochasticSlots.getNumOccurrences() != 0 ||
      OptimizeIRSynthesisStochasticRestarts.getNumOccurrences() != 0 ||
      OptimizeIRSynthesisStochasticIterations.getNumOccurrences() != 0 ||
      OptimizeIRSolverMaxConflicts.getNumOccurrences() != 0 ||
      OptimizeIRSolverMaxPropagations.getNumOccurrences() != 0 ||
      OptimizeIRSolverMaxWatchVisits.getNumOccurrences() != 0;
  if (!OptimizeIRSynthesize && HasSynthesisOption) {
    WithColor::error() << "synthesis and solver options require --synthesize\n";
    return false;
  }
  if (OptimizeIRSynthesize &&
      OptimizeIRMode == NEVERD_OPTIMIZATION_MODE_CONSERVATIVE) {
    WithColor::error()
        << "--synthesize cannot be combined with --mode=conservative\n";
    return false;
  }

  Options = {};
  Options.struct_size = sizeof(Options);
  Options.mode = OptimizeIRMode;
  Options.llvm_level = OptimizeIRLevel;
  Options.max_rounds = OptimizeIRMaxRounds;
  Options.enable_synthesis = OptimizeIRSynthesize ? 1 : 0;
  Options.synthesis_max_cost =
      static_cast<size_t>(OptimizeIRSynthesisMaxCost.getValue());
  Options.synthesis_max_samples =
      static_cast<size_t>(OptimizeIRSynthesisMaxSamples.getValue());
  Options.synthesis_verify_samples = OptimizeIRSynthesisVerifySamples;
  Options.synthesis_max_work =
      static_cast<size_t>(OptimizeIRSynthesisMaxWork.getValue());
  Options.synthesis_max_leaves = OptimizeIRSynthesisMaxLeaves;
  Options.synthesis_max_constants = OptimizeIRSynthesisMaxConstants;
  Options.synthesis_stochastic_slots = OptimizeIRSynthesisStochasticSlots;
  Options.synthesis_stochastic_restarts = OptimizeIRSynthesisStochasticRestarts;
  Options.synthesis_stochastic_iterations =
      static_cast<size_t>(OptimizeIRSynthesisStochasticIterations.getValue());
  Options.solver_max_conflicts = OptimizeIRSolverMaxConflicts;
  Options.solver_max_propagations = OptimizeIRSolverMaxPropagations;
  Options.solver_max_watch_visits = OptimizeIRSolverMaxWatchVisits;
  Options.exhaustive = OptimizeIRExhaustive ? 1 : 0;
  return true;
}

bool writeIR(StringRef Text) {
  if (OptimizeIROutput.empty() || OptimizeIROutput == "-") {
    outs() << Text;
    return true;
  }
  std::error_code EC;
  raw_fd_ostream Stream(OptimizeIROutput, EC, sys::fs::OF_Text);
  if (EC) {
    WithColor::error() << "cannot write " << OptimizeIROutput << ": "
                       << EC.message() << "\n";
    return false;
  }
  Stream << Text;
  Stream.flush();
  if (Stream.has_error()) {
    WithColor::error() << "cannot finish writing " << OptimizeIROutput << "\n";
    return false;
  }
  return true;
}

json::Object resultJSON(const neverd_optimize_llvm_ir_result &Result) {
  json::Object Object{
      {"schemaVersion", 1},
      {"ok", Result.ok != 0},
      {"changed", Result.changed != 0},
      {"stop", neverd_optimization_stop_name(Result.stop)},
      {"functionsVisited", Result.functions_visited},
      {"rounds", Result.rounds},
      {"semanticRewrites", Result.semantic_rewrites},
      {"searchWork", Result.search_work},
      {"proofQueries", Result.proof_queries},
      {"proofConflicts", Result.proof_conflicts},
      {"proofPropagations", Result.proof_propagations},
      {"proofWatchVisits", Result.proof_watch_visits},
  };
  if (Result.error)
    Object["error"] = Result.error;
  if (Result.error_line)
    Object["errorLine"] = Result.error_line;
  if (Result.error_column)
    Object["errorColumn"] = Result.error_column;
  if (Result.output_ir)
    Object["outputIR"] = Result.output_ir;
  return Object;
}

} // namespace

int runOptimizeIR() {
  if (OptimizeIRJson && OptimizeIROutput == "-") {
    WithColor::error() << "--json cannot be combined with -o -\n";
    return ExitInvalidInput;
  }
  ErrorOr<std::unique_ptr<MemoryBuffer>> Buffer =
      OptimizeIRInput == "-" ? MemoryBuffer::getSTDIN()
                             : MemoryBuffer::getFile(OptimizeIRInput);
  if (!Buffer) {
    WithColor::error() << "cannot read " << OptimizeIRInput << ": "
                       << Buffer.getError().message() << "\n";
    return ExitInvalidInput;
  }

  neverd_optimize_llvm_ir_options Options{};
  if (!buildOptions(Options))
    return ExitInvalidInput;

  const std::string IR = (*Buffer)->getBuffer().str();
  neverd_optimize_llvm_ir_result Result{};
  Result.struct_size = sizeof(Result);
  const int Status = neverd_optimize_llvm_ir(IR.c_str(), &Options, &Result);

  if (OptimizeIRJson)
    outs() << json::Value(resultJSON(Result)) << "\n";

  std::string Error = Result.error ? Result.error : "";
  std::string Output = Result.output_ir ? Result.output_ir : "";
  const size_t ErrorLine = Result.error_line;
  const size_t ErrorColumn = Result.error_column;
  const neverd_optimization_stop_t Stop = Result.stop;
  const bool Ok = Status == 0 && Result.ok != 0;
  neverd_optimize_llvm_ir_result_dispose(&Result);

  if (!Ok) {
    if (!OptimizeIRJson) {
      WithColor::error() << OptimizeIRInput;
      if (ErrorLine) {
        errs() << ":" << ErrorLine;
        if (ErrorColumn)
          errs() << ":" << ErrorColumn;
      }
      const StringRef Message = Status != 0 ? "the engine refused the request"
                                : Error.empty() ? "optimization failed"
                                                : StringRef(Error);
      errs() << ": " << Message << "\n";
    }
    if (Status != 0)
      return ExitEngineFailure;
    return Stop == NEVERD_OPTIMIZATION_INPUT_INVALID ? ExitInvalidInput
                                                     : ExitEngineFailure;
  }
  const ExitStatus Completion = Stop == NEVERD_OPTIMIZATION_BUDGET_EXHAUSTED
                                    ? ExitIncomplete
                                    : ExitSuccess;
  if (OptimizeIRJson && OptimizeIROutput.empty())
    return Completion;
  return writeIR(Output) ? Completion : ExitEngineFailure;
}

} // namespace neverd::cli
