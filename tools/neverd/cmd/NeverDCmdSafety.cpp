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

// A malformed safety verdict is a failure.
int safetyExitCode(StringRef Report) {
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
  return Code;
}

int emit(const char *Report, int Code) {
  scope_exit FreeReport([&] { neverd_free_string(Report); });
  if (!OutputFile.empty()) {
    std::error_code EC;
    raw_fd_ostream OS(OutputFile.getValue(), EC);
    if (EC) {
      WithColor::error() << "cannot open scan output file: "
                         << OutputFile.getValue() << ": " << EC.message()
                         << "\n";
      return 1;
    }
    OS << Report << "\n";
    OS.flush();
    if (OS.has_error()) {
      WithColor::error() << "cannot write scan output file: "
                         << OutputFile.getValue() << "\n";
      OS.clear_error();
      return 1;
    }
  } else {
    outs() << Report << "\n";
  }
  return Code;
}

} // namespace

int runScan(neverd_session_t Sess) {
  if (ScanDataPtr &&
      (ScanAnalyzers.getNumOccurrences() || SafetySinks.getNumOccurrences() ||
       SafetySources.getNumOccurrences() ||
       SafetyMaxPaths.getNumOccurrences() ||
       SafetyMaxSteps.getNumOccurrences() ||
       SafetyMaxLoop.getNumOccurrences() ||
       SafetyMaxCallDepth.getNumOccurrences() ||
       SafetyMaxSummaryIterations.getNumOccurrences() ||
       SafetySolverConflicts.getNumOccurrences())) {
    WithColor::error()
        << "--data-ptr cannot be combined with memory-safety options\n";
    return 1;
  }
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
  const char *Report = ScanDataPtr ? neverd_session_data_ptr_scan_json(
                                         Sess, Options.cancel, Options.context)
                                   : neverd_session_scan_json(Sess, &Options);
  if (!Report) {
    const std::string Error = takeLastError(Sess);
    WithColor::error() << (Error.empty() ? "scan produced no report" : Error)
                       << "\n";
    return 1;
  }
  return emit(Report, ScanDataPtr ? 0 : safetyExitCode(Report));
}

} // namespace neverd::cli
