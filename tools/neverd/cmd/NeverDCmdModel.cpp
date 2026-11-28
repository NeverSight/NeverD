//===- NeverDCmdModel.cpp - Explicit read-only model scan dispatch --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit read-only model scan dispatch.
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/sdk/NeverDCAPISession.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>

namespace neverd::cli {
using namespace llvm;
bool modelScanRequested() {
  return ModelGGUF || ModelOptions.getNumOccurrences() ||
         ModelExternalData.getNumOccurrences() ||
         ModelBaseline.getNumOccurrences() || ModelGap.getNumOccurrences() ||
         ModelScales.getNumOccurrences() || ModelCompare.getNumOccurrences() ||
         ModelOffset.getNumOccurrences() || ModelLimit.getNumOccurrences();
}
int runModelScan() {
  if (!ModelGGUF) {
    WithColor::error() << "model scan options require --gguf\n";
    return 1;
  }
  if (ModelBaseline.getNumOccurrences() && ModelBaseline.empty()) {
    WithColor::error() << "--baseline requires a local model path\n";
    return 1;
  }
  for (const auto &[Name, Opt] : cl::getRegisteredOptions(ScanCmd)) {
    if (Opt->getNumOccurrences() && Opt != &ModelGGUF && Opt != &ModelOptions &&
        Opt != &ModelExternalData && Opt != &ModelBaseline &&
        Opt != &ModelGap && Opt != &ModelScales && Opt != &ModelCompare &&
        Opt != &ModelOffset && Opt != &ModelLimit && Opt != &InputFile &&
        Opt != &OutputFile && Opt != &JsonOutput) {
      WithColor::error() << "--gguf cannot be combined with --" << Name << "\n";
      return 1;
    }
  }
  json::Object Options;
  if (ModelOptions.getNumOccurrences())
    Options["options_json"] = ModelOptions.getValue();
  if (ModelExternalData)
    Options["external_data"] = true;
  if (!ModelBaseline.empty())
    Options["baseline"] = ModelBaseline.getValue();
  if (ModelGap.getNumOccurrences())
    Options["gap_bytes"] = ModelGap.getValue();
  if (ModelScales.getNumOccurrences())
    Options["scale_bytes"] = ModelScales.getValue();
  if (ModelCompare.getNumOccurrences())
    Options["compare_bytes"] = ModelCompare.getValue();
  if (ModelOffset.getNumOccurrences())
    Options["offset"] = ModelOffset.getValue();
  if (ModelLimit.getNumOccurrences())
    Options["limit"] = ModelLimit.getValue();
  std::string Settings;
  raw_string_ostream(Settings) << json::Value(std::move(Options));
  static std::atomic_flag Interrupted;
  Interrupted.clear();
  sys::SetInterruptFunction([] { Interrupted.test_and_set(); });
  scope_exit Restore([] { sys::SetInterruptFunction(nullptr); });
  const char *Report = neverd_model_scan_json_v1(
      InputFile.c_str(), Settings.c_str(),
      [](void *) -> int { return Interrupted.test(); }, nullptr);
  if (!Report) {
    WithColor::error() << "model scan could not produce a report\n";
    return 1;
  }
  scope_exit Free([&] { neverd_free_string(Report); });
  auto Parsed = json::parse(Report);
  if (!Parsed) {
    consumeError(Parsed.takeError());
    return 1;
  }
  if (OutputFile.empty() || OutputFile == "-")
    outs() << Report << "\n";
  else {
    std::error_code EC;
    raw_fd_ostream OS(OutputFile.getValue(), EC, sys::fs::CD_CreateNew);
    if (EC) {
      WithColor::error() << "cannot create a new model scan report: "
                         << EC.message() << "\n";
      return 1;
    }
    OS << Report << "\n";
    OS.close();
    if (OS.has_error()) {
      WithColor::error() << "cannot write model report: "
                         << OS.error().message() << "\n";
      OS.clear_error();
      sys::fs::remove(OutputFile);
      return 1;
    }
  }
  auto *Obj = Parsed->getAsObject();
  return Obj ? Obj->getInteger("exit_code").value_or(1) : 1;
}
} // namespace neverd::cli
