//===- NeverDCmdLua.cpp - Lua source and report export ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Selects an existing prototype and writes only to a new output file.
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <csignal>
#include <optional>

using namespace llvm;
namespace neverd::cli {
namespace {
volatile std::sig_atomic_t Interrupted = 0;
void interrupt(int) { Interrupted = 1; }
} // namespace
int runLua() {
  if (JsonOutput && LuaPrototype.getNumOccurrences()) {
    errs() << "--json exports every prototype\n";
    return 1;
  }
  auto Previous = std::signal(SIGINT, interrupt);
  auto Raw = neverd_lua_recover_json_v1(
      InputFile.c_str(), LuaProfile.c_str(),
      [](void *) -> int { return Interrupted != 0; }, nullptr);
  std::signal(SIGINT, Previous);
  if (!Raw) {
    errs() << "Could not obtain Lua recovery report\n";
    return 1;
  }
  std::string Report = Raw;
  neverd_free_string(Raw);
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
  std::optional<StringRef> Output;
  if (JsonOutput)
    Output = Report;
  else {
    auto Id = LuaPrototype.getNumOccurrences()
                  ? int64_t(LuaPrototype)
                  : Root.getInteger("root").value_or(-1);
    for (auto &Value : *Root.getArray("functions")) {
      auto &F = *Value.getAsObject();
      if (F.getInteger("id") != Id)
        continue;
      Output = *F.getString("source");
      Diagnostics(F);
      break;
    }
    if (!Output) {
      if (LuaPrototype.getNumOccurrences())
        errs() << "Unknown prototype ID\n";
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
  return Status == "reconstructed_source" || Status == "extracted_source" ? 0
                                                                          : 1;
}
} // namespace neverd::cli
