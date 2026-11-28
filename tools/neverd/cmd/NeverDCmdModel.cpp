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
#include <filesystem>

namespace neverd::cli {
using namespace llvm;
namespace {
cl::opt<std::string>
    RebuildGGUF("rebuild-gguf",
                cl::desc("Write and verify a new standalone GGUF"),
                cl::value_desc("new file"), cl::sub(ScanCmd));
cl::opt<bool> ModelText("model-text",
                        cl::desc("Print model structure and evidence as text"),
                        cl::sub(ScanCmd));

void printModelText(raw_ostream &OS, const json::Object &Report) {
  OS << "Model scan " << Report.getString("status").value_or("failed") << "\n";
  if (auto E = Report.get("error"))
    OS << *E << "\n";
  if (auto *Packages = Report.getArray("packages"))
    for (const auto &V : *Packages) {
      const auto &P = *V.getAsObject();
      OS << "Package " << *P.get("id") << " ("
         << P.getInteger("tensor_count").value_or(0) << " tensors)\n";
      for (StringRef Key :
           {"declarations", "tensors", "inferred_projection_layouts",
            "findings", "coverage_gaps"})
        if (auto *List = P.getArray(Key))
          for (const auto &Row : *List)
            OS << "  " << Key << " " << Row << "\n";
      if (auto *Graphs = P.getArray("graphs"))
        for (const auto &Value : *Graphs) {
          const auto &Graph = *Value.getAsObject();
          OS << "  Graph " << *Graph.get("id") << " ("
             << Graph.getInteger("node_count").value_or(0) << " nodes)\n";
          for (const auto &N : *Graph.getArray("nodes"))
            OS << "    " << N << "\n";
        }
    }
  if (auto *C = Report.get("comparison"))
    OS << "Comparison " << *C << "\n";
}
} // namespace
bool modelScanRequested() {
  return ModelGGUF || RebuildGGUF.getNumOccurrences() ||
         ModelText.getNumOccurrences() || ModelOptions.getNumOccurrences() ||
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
    if (Opt->getNumOccurrences() && Opt != &ModelGGUF && Opt != &ModelText &&
        Opt != &RebuildGGUF && Opt != &ModelOptions &&
        Opt != &ModelExternalData && Opt != &ModelBaseline &&
        Opt != &ModelGap && Opt != &ModelScales && Opt != &ModelCompare &&
        Opt != &ModelOffset && Opt != &ModelLimit && Opt != &InputFile &&
        Opt != &OutputFile && Opt != &JsonOutput) {
      WithColor::error() << "--gguf cannot be combined with --" << Name << "\n";
      return 1;
    }
  }
  const bool Rebuild = RebuildGGUF.getNumOccurrences();
  if (Rebuild &&
      (RebuildGGUF.empty() || ModelText ||
       ModelExternalData.getNumOccurrences() || ModelGap.getNumOccurrences() ||
       ModelScales.getNumOccurrences() || ModelCompare.getNumOccurrences() ||
       ModelOffset.getNumOccurrences() || ModelLimit.getNumOccurrences() ||
       ModelBaseline.getNumOccurrences())) {
    WithColor::error() << "GGUF reconstruction requires a new file and "
                          "explicit template policy options\n";
    return 1;
  }
  if (Rebuild && !OutputFile.empty() && OutputFile != "-") {
    auto resolve = [](const std::string &Name, std::error_code &EC) {
      auto Path =
          std::filesystem::path(std::u8string(Name.begin(), Name.end()));
      return std::filesystem::weakly_canonical(
          std::filesystem::absolute(Path, EC), EC);
    };
    std::error_code ModelError, ReportError;
    auto ReportPath = resolve(OutputFile, ReportError);
    auto NewModel = resolve(RebuildGGUF, ModelError);
    if (ModelError || ReportError || NewModel == ReportPath ||
        sys::fs::exists(OutputFile)) {
      WithColor::error()
          << "model and report destinations must be distinct new files\n";
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
  auto Stop = [](void *) -> int { return Interrupted.test(); };
  const char *Report =
      Rebuild ? neverd_model_rebuild_gguf_json_v1(
                    InputFile.c_str(), RebuildGGUF.c_str(),
                    ModelOptions.c_str(), Stop, nullptr)
              : neverd_model_scan_json_v1(InputFile.c_str(), Settings.c_str(),
                                          Stop, nullptr);
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
  auto print = [&](raw_ostream &OS) {
    if (ModelText && Parsed->getAsObject())
      printModelText(OS, *Parsed->getAsObject());
    else
      OS << Report << "\n";
  };
  if (OutputFile.empty() || OutputFile == "-")
    print(outs());
  else {
    std::error_code EC;
    raw_fd_ostream OS(OutputFile.getValue(), EC, sys::fs::CD_CreateNew);
    if (EC) {
      WithColor::error() << "cannot create a new model scan report: "
                         << EC.message() << "\n";
      return 1;
    }
    print(OS);
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
