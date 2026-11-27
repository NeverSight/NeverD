//===- NeverDCmdScriptRecovery.cpp - Script source and report export ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Selects an existing prototype and writes only to a new output file.
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Base64.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <climits>
#include <csignal>
#include <memory>
#include <optional>

using namespace llvm;
namespace neverd::cli {
namespace {
volatile std::sig_atomic_t Interrupted = 0;
void interrupt(int) { Interrupted = 1; }
} // namespace
int runScriptRecovery() {
  const bool Python = static_cast<bool>(PythonCmd);
  auto &Function = Python ? PythonFunction : LuaPrototype;
  if (JsonOutput && Function.getNumOccurrences()) {
    errs() << (Python ? "--json exports every function\n"
                      : "--json exports every prototype\n");
    return 1;
  }
  if (Python && PythonComponent > INT_MAX) {
    errs() << "Python component ID exceeds the supported range\n";
    return 1;
  }
  if (Python && PythonNative.getNumOccurrences() &&
      (JsonOutput || Function.getNumOccurrences() ||
       PythonComponent.getNumOccurrences() || OutputFile.empty())) {
    errs() << "--native requires a component ID and -o NEW_FILE\n";
    return 1;
  }
  auto Previous = std::signal(SIGINT, interrupt);
  auto Restore = scope_exit([&] { std::signal(SIGINT, Previous); });
  auto Stop = [](void *) -> int { return Interrupted != 0; };
  using Owned = std::unique_ptr<const char, decltype(&neverd_free_string)>;
  int Component =
      PythonComponent.getNumOccurrences() ? int(PythonComponent) : -1;
  Owned Raw(Python ? neverd_python_recover_json_v1(InputFile.c_str(),
                                                   PythonProfile.c_str(),
                                                   Component, Stop, nullptr)
                   : neverd_lua_recover_json_v1(
                         InputFile.c_str(), LuaProfile.c_str(), Stop, nullptr),
            neverd_free_string);
  if (!Raw) {
    errs() << "Could not obtain script recovery report\n";
    return 1;
  }
  StringRef Report(Raw.get());
  auto Parsed = json::parse(Report);
  if (!Parsed) {
    logAllUnhandledErrors(Parsed.takeError(), errs());
    return 1;
  }
  auto &Root = *Parsed->getAsObject();
  auto Status = *Root.getString("status");
  auto &Evidence = *Root.getObject("evidence");
  if (auto Dialect = Evidence.getString("dialect"))
    errs() << *Dialect << " | ";
  errs() << Status << "\n";
  auto Diagnostics = [](const json::Object &Row) {
    for (auto &D : *Row.getArray("diagnostics")) {
      auto &O = *D.getAsObject();
      if (auto At = O.getInteger("offset"))
        errs() << "byte " << *At << ": ";
      errs() << *O.getString("message") << "\n";
    }
  };
  Diagnostics(Root);
  if (Python && PythonNative.getNumOccurrences()) {
    const auto *Rows = Root.getArray("components");
    if (PythonNative >= Rows->size()) {
      errs() << "Unknown native component ID\n";
      return 1;
    }
    const auto &R = *(*Rows)[PythonNative].getAsObject();
    if (R.getString("backend") != "native") {
      errs() << "Component has no native backend\n";
      return 1;
    }
    std::string Hash = R.getString("sha256")->str();
    Owned Export(neverd_python_export_native_json_v1(
                     InputFile.c_str(), PythonProfile.c_str(), PythonNative,
                     Hash.c_str(), OutputFile.c_str(), Stop, nullptr),
                 neverd_free_string);
    if (!Export) {
      errs() << "Native export failed\n";
      return 1;
    }
    auto Reply = json::parse(Export.get());
    if (!Reply) {
      logAllUnhandledErrors(Reply.takeError(), errs());
      return 1;
    }
    if (Reply->getAsObject()->getString("status") != "exported") {
      errs() << Export.get() << "\n";
      return 1;
    }
    auto Session = neverd_session_create();
    if (!Session) {
      errs() << "Cannot create native session\n";
      return 1;
    }
    auto Close = scope_exit([&] { neverd_session_destroy(Session); });
    if (neverd_session_load_cancellable(Session, OutputFile.c_str(), Stop,
                                        nullptr) != 1) {
      errs() << neverd_last_error(Session) << "\n";
      return 1;
    }
    int Count =
        neverd_session_discover_functions_cancellable(Session, Stop, nullptr);
    if (Count < 0) {
      errs() << neverd_last_error(Session) << "\n";
      return 1;
    }
    json::OStream Output(outs());
    Output.array([&] {
      for (int Index = 0; Index < Count && !Interrupted; ++Index) {
        Owned Name(neverd_func_name(Session, Index), neverd_free_string);
        Output.value(json::Object{
            {"entry", "0x" + utohexstr(neverd_func_entry(Session, Index))},
            {"name", json::fixUTF8(Name ? Name.get() : "")},
            {"size", neverd_func_size(Session, Index)}});
      }
    });
    outs() << "\n";
    if (Interrupted) {
      errs() << "Native function listing cancelled\n";
      return 1;
    }
    return 0;
  }
  std::vector<char> OriginalBytes;
  std::optional<StringRef> Output;
  if (JsonOutput)
    Output = Report;
  else {
    if (Python && (Status == "cancelled" || Status == "budget_exceeded" ||
                   Status == "io_error"))
      return 1;
    auto Id = Function.getNumOccurrences()
                  ? int64_t(Function)
                  : Root.getInteger("root").value_or(-1);
    for (auto &Value : *Root.getArray("functions")) {
      auto &F = *Value.getAsObject();
      if (F.getInteger("id") != Id)
        continue;
      Diagnostics(F);
      if (auto Encoded = F.getString("source_base64")) {
        if (auto E = decodeBase64(*Encoded, OriginalBytes)) {
          logAllUnhandledErrors(std::move(E), errs());
          return 1;
        }
        Output = StringRef(OriginalBytes.data(), OriginalBytes.size());
      } else
        Output = F.getString("source");
      break;
    }
    if (!Output) {
      if (Function.getNumOccurrences())
        errs() << (Python ? "Unknown or unavailable function\n"
                          : "Unknown prototype ID\n");
      return 1;
    }
  }
  if (OutputFile.empty()) {
    sys::ChangeStdoutToBinary();
    outs() << *Output;
    if (JsonOutput)
      outs() << '\n';
  } else {
    std::error_code EC;
    raw_fd_ostream File(OutputFile, EC, sys::fs::CD_CreateNew,
                        sys::fs::FA_Write, sys::fs::OF_None);
    if (EC) {
      errs() << OutputFile << ": " << EC.message() << "\n";
      return 1;
    }
    File << *Output;
    if (JsonOutput)
      File << '\n';
    File.close();
    if (File.has_error()) {
      errs() << File.error().message() << "\n";
      File.clear_error();
      sys::fs::remove(OutputFile);
      return 1;
    }
  }
  if (Python && (!JsonOutput || Status == "disassembly_only" ||
                 Status == "metadata_only"))
    return 0;
  return Status == "reconstructed_source" || Status == "extracted_source" ? 0
                                                                          : 1;
}
} // namespace neverd::cli
