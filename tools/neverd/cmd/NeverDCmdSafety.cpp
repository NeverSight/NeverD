//===- NeverDCmdSafety.cpp - native memory-safety scan -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Runs the native memory-safety scan on the loaded session and emits its
/// versioned JSON report. SAFE returns 0, UNSAFE returns 2, and UNKNOWN or
/// an error returns 1.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/sdk/NeverDCAPISafety.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <optional>

using namespace llvm;

namespace neverd::cli {

namespace {

neverd_safety_options makeOptions() {
  neverd_safety_options Options{};
  Options.struct_size = sizeof(Options);
  Options.max_paths = SafetyMaxPaths;
  Options.max_steps = SafetyMaxSteps;
  Options.max_loop = SafetyMaxLoop;
  Options.max_call_depth = SafetyMaxCallDepth;
  Options.max_summary_iterations = SafetyMaxSummaryIterations;
  Options.solver_conflicts = SafetySolverConflicts;
  Options.sinks_path = SafetySinks.empty() ? nullptr : SafetySinks.c_str();
  Options.sources_path =
      SafetySources.empty() ? nullptr : SafetySources.c_str();
  return Options;
}

// Print the report to -o or stdout, and derive the exit code from its verdict
// field.  A malformed report is treated as a failure.
int emit(const char *Report) {
  if (!Report) {
    WithColor::error() << "safety analysis produced no report\n";
    return 1;
  }

  int Code = 1;
  if (Expected<json::Value> Parsed = json::parse(Report)) {
    if (const json::Object *Root = Parsed->getAsObject()) {
      const bool Ok = Root->getBoolean("ok").value_or(false);
      const std::optional<StringRef> Verdict = Root->getString("verdict");
      if (Ok && Verdict) {
        if (*Verdict == "SAFE")
          Code = 0;
        else if (*Verdict == "UNSAFE")
          Code = 2;
        else if (*Verdict == "UNKNOWN")
          Code = 1;
      }
    }
  } else {
    consumeError(Parsed.takeError());
  }

  if (!OutputFile.empty()) {
    std::error_code EC;
    raw_fd_ostream OS(OutputFile.getValue(), EC);
    if (EC) {
      WithColor::error() << "cannot open safety output file: "
                         << OutputFile.getValue() << ": " << EC.message()
                         << "\n";
      neverd_free_string(Report);
      return 1;
    }
    OS << Report << "\n";
    OS.flush();
    if (OS.has_error()) {
      WithColor::error() << "cannot write safety output file: "
                         << OutputFile.getValue() << "\n";
      neverd_free_string(Report);
      return 1;
    }
  } else {
    outs() << Report << "\n";
  }
  neverd_free_string(Report);
  return Code;
}

} // namespace

int runScan(neverd_session_t Sess) {
  static std::atomic_flag Interrupted;
  Interrupted.clear(std::memory_order_relaxed);
  sys::SetInterruptFunction(
      [] { Interrupted.test_and_set(std::memory_order_relaxed); });
  scope_exit RestoreInterrupt([] { sys::SetInterruptFunction(nullptr); });
  const neverd_safety_options Analysis = makeOptions();
  neverd_scan_options Options{};
  Options.struct_size = sizeof(Options);
  Options.analysis = &Analysis;
  Options.analyzers = ScanAnalyzers;
  Options.cancel = [](void *) -> int {
    return Interrupted.test(std::memory_order_relaxed);
  };
  return emit(neverd_session_scan_json(Sess, &Options));
}

} // namespace neverd::cli
