//===- neverd-sbf-corpus.cpp - Isolated SBF qualification worker ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Runs one bounded corpus request through the production loader, analyzer
/// and emitters. Flushed JSON events preserve earlier evidence after a fault.
/// The Python parent owns process isolation, deadlines and atomic reports.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/Loader.h"
#include "neverd/sbf/analysis/SBFAnalysisReport.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/analysis/SBFCallXContexts.h"
#include "neverd/sbf/analysis/SBFStructuredCFG.h"
#include "neverd/sbf/emit/SBFCEmitter.h"
#include "neverd/sbf/emit/SBFLLVMEmitter.h"
#include "neverd/sbf/emit/SBFRustEmitter.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <utility>

using namespace neverd;
using namespace neverd::sbf;

namespace {
llvm::cl::opt<std::string> Input(llvm::cl::Positional, llvm::cl::Required,
                                 llvm::cl::desc("input SBF ELF"));
llvm::cl::opt<std::string>
    Kind("kind", llvm::cl::Required,
         llvm::cl::desc("analysis, llvm, c, rust or callx-contexts"));
llvm::cl::opt<std::string> Owner("loader", llvm::cl::Required);
llvm::cl::opt<std::string> Chain("cluster", llvm::cl::Required);
llvm::cl::opt<std::string> Stop("stop-after", llvm::cl::init("solana"));
llvm::cl::opt<std::string> Output("output");
llvm::cl::opt<uint64_t> MaxInput("max-input-bytes", llvm::cl::Required);
llvm::cl::opt<uint64_t> MaxOutput("max-output-bytes", llvm::cl::Required);
llvm::cl::opt<uint64_t> Slot("slot");

const auto Started = std::chrono::steady_clock::now();

void event(llvm::StringRef Stage, llvm::StringRef State,
           llvm::json::Object Details = {}) {
  llvm::json::Object Row;
  Row["stage"] = Stage;
  Row["state"] = State;
  Row["elapsed_us"] = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - Started)
                          .count();
  Row["details"] = std::move(Details);
  llvm::outs() << llvm::json::Value(std::move(Row)) << '\n';
  llvm::outs().flush();
}

void event(llvm::StringRef Stage, llvm::StringRef State,
           std::initializer_list<llvm::json::Object::KV> Details) {
  event(Stage, State, llvm::json::Object(Details));
}

int fail(llvm::StringRef Stage, llvm::Error Error) {
  event(Stage, "failed", {{"error", llvm::toString(std::move(Error))}});
  return 1;
}

int run() {
  if (Kind != "analysis" && Kind != "llvm" && Kind != "c" && Kind != "rust" &&
      Kind != "callx-contexts") {
    llvm::errs() << "unknown output kind\n";
    return 1;
  }
  const auto LoaderID = parseLoader(Owner);
  const auto ClusterID = parseCluster(Chain);
  const auto LastStage = parseAnalysisStage(Stop);
  if (!LoaderID || !ClusterID || !LastStage ||
      (Kind != "analysis" && *LastStage != AnalysisStage::Solana)) {
    llvm::errs() << "invalid profile/stage combination\n";
    return 1;
  }

  event("load", "started");
  std::error_code EC;
  const uint64_t Size = std::filesystem::file_size(Input.getValue(), EC);
  if (EC || Size > MaxInput) {
    event("load", EC ? "failed" : "budget_exhausted",
          {{"error", EC ? EC.message() : "input byte budget exceeded"}});
    return 1;
  }
  auto Reader =
      neverd::Loader::create(std::filesystem::u8path(Input.getValue()));
  if (!Reader) {
    event("load", "failed", {{"error", "no supported loader"}});
    return 1;
  }
  auto Image = Reader->load(std::filesystem::u8path(Input.getValue()));
  if (!Image)
    return fail("load", Image.takeError());
  if (Image->Arch != Arch::SBF || !Image->SBF) {
    event("load", "failed", {{"error", "not an SBF ELF"}});
    return 1;
  }
  event("load", "passed",
        {{"file_bytes", static_cast<int64_t>(Size)},
         {"text_bytes", static_cast<int64_t>(Image->SBF->TextVM.Size)},
         {"version", versionName(Image->SBF->Version)},
         {"machine", Image->SBF->Machine},
         {"relocations", static_cast<int64_t>(Image->Relocations.size())}});

  AnalyzeOptions Options;
  Options.StopAfter = *LastStage;
  Options.Profile.OwningLoader = *LoaderID;
  Options.Profile.OnCluster = *ClusterID;
  if (Slot.getNumOccurrences())
    Options.Profile.Slot = Slot;
  Options.Observer = [](AnalysisStage Stage, AnalysisStageState State,
                        const SBFProgram &Program) {
    llvm::json::Object Details;
    if (State == AnalysisStageState::Passed) {
      Details["coverage"] = analysisCoverage(Program);
      Details["runtime_features"] =
          static_cast<int64_t>(Program.ActiveRuntimeFeatures);
      Details["version"] = versionName(Program.Low.TheVersion);
      if (Stage == AnalysisStage::Verification) {
        Details["policy"] = "requisite";
        Details["local_preflight"] = "not_requested";
      }
      if (Stage == AnalysisStage::HighIR)
        Details["whole_program_structured"] =
            buildStructuredControlFlow(Program).has_value();
    }
    event(analysisStageName(Stage), analysisStageStateName(State),
          std::move(Details));
  };
  auto Program = analyze(*Image, Options);
  if (!Program) {
    event("analysis_error", "failed",
          {{"error", llvm::toString(Program.takeError())}});
    return 1;
  }
  if (Kind == "analysis")
    return 0;

  event(Kind, "started");
  auto Text = [&]() -> llvm::Expected<std::string> {
    if (Kind == "callx-contexts") {
      auto Report = recoverCallXContexts(*Program);
      if (!Report)
        return Report.takeError();
      return llvm::formatv("{0:2}\n", llvm::json::Value(callXContextReportJSON(
                                          *Program, *Report)))
          .str();
    }
    if (Kind == "c")
      return emitC(*Program);
    if (Kind == "rust")
      return emitRust(*Program);
    llvm::LLVMContext Context;
    auto Module = emitLLVM(*Program, Context);
    if (!Module)
      return Module.takeError();
    std::string Diagnostic;
    llvm::raw_string_ostream Stream(Diagnostic);
    if (llvm::verifyModule(**Module, &Stream))
      return llvm::make_error<llvm::StringError>(
          Diagnostic, llvm::inconvertibleErrorCode());
    return emitLLVMText(**Module);
  }();
  if (!Text)
    return fail(Kind, Text.takeError());
  if (Text->size() > MaxOutput) {
    event(Kind, "budget_exhausted",
          {{"error", "output byte budget exceeded"},
           {"output_bytes", static_cast<int64_t>(Text->size())}});
    return 1;
  }
  if (!Output.empty()) {
    llvm::raw_fd_ostream Stream(Output, EC);
    if (EC)
      return fail(Kind, llvm::errorCodeToError(EC));
    Stream << *Text;
    Stream.close();
    if (Stream.has_error())
      return fail(Kind, llvm::errorCodeToError(Stream.error()));
  }
  event(Kind, "passed",
        {{"output_bytes", static_cast<int64_t>(Text->size())},
         {"module_verified", Kind == "llvm"}});
  return 0;
}
} // namespace

int main(int Argc, char **Argv) {
  llvm::InitLLVM Init(Argc, Argv);
  llvm::cl::ParseCommandLineOptions(Argc, Argv,
                                    "SBF corpus qualification worker\n");
  return run();
}
