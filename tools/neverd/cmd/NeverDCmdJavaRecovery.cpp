//===- NeverDCmdJavaRecovery.cpp - JVM views and export -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Export the selected backend view, preserving incomplete method evidence.
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <csignal>
#include <memory>

using namespace llvm;
namespace neverd::cli {
namespace {
volatile std::sig_atomic_t Interrupted = 0;
void interrupt(int) { Interrupted = 1; }
} // namespace
int runJavaRecovery() {
  if (JavaView != "java" && JavaView != "bytecode" && JavaView != "ir") {
    errs() << "Unknown JVM view\n";
    return 1;
  }
  if ((JavaInventory || JsonOutput) && JavaView.getNumOccurrences()) {
    errs() << "Choose the report or one method representation\n";
    return 1;
  }
  auto Previous = std::signal(SIGINT, interrupt);
  auto Restore = scope_exit([&] { std::signal(SIGINT, Previous); });
  json::Array Deps;
  for (const auto &D : JavaDependencies)
    Deps.emplace_back(D);
  json::Object Options{
      {"release", unsigned(JavaRelease)}, {"preview", bool(JavaPreview)},
      {"class", JavaClass.getValue()},    {"method", JavaMethod.getValue()},
      {"component", int(JavaComponent)},  {"inventory", bool(JavaInventory)},
      {"dependencies", std::move(Deps)}};
  std::string Request;
  raw_string_ostream(Request) << json::Value(std::move(Options));
  std::unique_ptr<const char, decltype(&neverd_free_string)> Raw(
      neverd_jvm_recover_json_v1(
          InputFile.c_str(), Request.c_str(),
          [](void *) -> int { return Interrupted != 0; }, nullptr),
      neverd_free_string);
  if (!Raw) {
    errs() << "Cannot obtain JVM recovery report\n";
    return 1;
  }
  auto Parsed = json::parse(Raw.get());
  if (!Parsed) {
    logAllUnhandledErrors(Parsed.takeError(), errs());
    return 1;
  }
  const auto &Root = *Parsed->getAsObject();
  auto Status = *Root.getString("status");
  auto diagnostics = [](const json::Object &O) {
    for (auto &D : *O.getArray("diagnostics")) {
      auto &E = *D.getAsObject();
      errs() << *E.getString("code") << " | " << *E.getString("message")
             << '\n';
    }
  };
  errs() << Status << '\n';
  diagnostics(Root);
  bool Complete = Status == "reconstructed_source" ||
                  Status == "extracted_source" || Status == "metadata_only";
  std::string Text;
  if (JsonOutput || JavaInventory) {
    Text = Raw.get();
    Text += '\n';
  } else {
    raw_string_ostream OS(Text);
    bool Any = false, Missing = false;
    for (auto &V : *Root.getArray("components")) {
      auto &C = *V.getAsObject();
      if (C.getBoolean("requested").value_or(false) &&
          !C.getArray("diagnostics")->empty()) {
        diagnostics(C);
        Missing = true;
      }
    }
    for (auto &V : *Root.getArray("functions")) {
      auto &F = *V.getAsObject();
      if (JavaView == "java") {
        if (auto S = F.getString("source")) {
          if (Any)
            OS << '\n';
          OS << *S;
          Any = true;
        } else if (F.getString("status") != "metadata_only" ||
                   !JavaMethod.empty()) {
          diagnostics(F);
          Missing = true;
        }
      } else {
        auto *Data = F.get(JavaView == "ir" ? "ir" : "instructions");
        if (Data && (JavaView == "ir" || !Data->getAsArray()->empty())) {
          OS << json::Value(json::Object{
                    {"class", F.getString("class").value_or("").str()},
                    {"name", F.getString("name").value_or("").str()},
                    {"descriptor",
                     F.getString("descriptor").value_or("").str()},
                    {"status", F.getString("status").value_or("").str()},
                    {"view", *Data}})
             << '\n';
          Any = true;
        } else if (F.getString("status") != "metadata_only" ||
                   !JavaMethod.empty()) {
          diagnostics(F);
          Missing = true;
        }
      }
    }
    if (!Any) {
      errs() << "Selected JVM representation is unavailable\n";
      return 1;
    }
    Complete = !Missing && (Complete || Status == "partial_reconstruction");
  }
  if (OutputFile.empty()) {
    sys::ChangeStdoutToBinary();
    outs() << Text;
  } else {
    std::error_code EC;
    raw_fd_ostream F(OutputFile, EC, sys::fs::CD_CreateNew, sys::fs::FA_Write,
                     sys::fs::OF_None);
    if (EC) {
      errs() << EC.message() << '\n';
      return 1;
    }
    F << Text;
    F.close();
    if (F.has_error()) {
      errs() << F.error().message() << '\n';
      F.clear_error();
      sys::fs::remove(OutputFile);
      return 1;
    }
  }
  return Complete ? 0 : 1;
}
} // namespace neverd::cli
