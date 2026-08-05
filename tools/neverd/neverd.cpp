//===- neverd.cpp - NeverD decompiler driver ------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Main driver for the neverd tool.  This translation unit is deliberately
/// thin: it parses the command line, loads the session, prints the banner, and
/// dispatches to a per-category run* handler.  The subcommands themselves live
/// in NeverDCmd*.cpp and the option table in NeverDCLIOptions.cpp, all behind
/// the shared NeverDCLI.h — see that header for the split rationale.
///
//===----------------------------------------------------------------------===//

#include "NeverDCLI.h"

#include "neverd/Common.h"
#include "neverd/Support/StackSizeMain.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>

using namespace llvm;
using namespace neverd;
using namespace neverd::cli;

static int realMain(int Argc, char *Argv[]);

int main(int Argc, char *Argv[]) {
  return neverd::runWithLargeStack(realMain, Argc, Argv);
}

static int realMain(int Argc, char *Argv[]) {
  InitLLVM X(Argc, Argv);

  cl::ParseCommandLineOptions(
      Argc, Argv,
      (Twine(ProjectName) + " Decompiler Engine v" + VersionString).str());

  if (Verbose)
    llvm::DebugFlag = true;

  // Plugins and diff are handled before the shared session load: plugins does
  // not require an input binary, and diff takes its own -a/-b operands.
  if (PluginsCmd)
    return runPlugins(Argv[0]);
  if (DiffCmd)
    return runDiff();

  // The active subcommand's registered name feeds the banner below.  Plugins
  // and diff returned already, so exactly one of the remaining named
  // subcommands is active here; the top-level pseudo-subcommands have empty
  // names.
  StringRef Command;
  for (auto *Sub : cl::getRegisteredSubcommands()) {
    if (*Sub && !Sub->getName().empty()) {
      Command = Sub->getName();
      break;
    }
  }
  if (Command.empty())
    return 1;

  auto Path = std::filesystem::path(InputFile.getValue());
  if (!std::filesystem::exists(Path)) {
    WithColor::error() << "file not found: " << InputFile.getValue() << "\n";
    return 1;
  }

  neverd_session_t Sess = neverd_session_create();
  SessionGuard SessGuard(Sess);
  if (!neverd_session_load(Sess, InputFile.getValue().c_str())) {
    WithColor::error() << "failed to load: " << takeLastError(Sess) << "\n";
    return 1;
  }

  if (!JsonOutput) {
    const char *ArchStr = neverd_session_arch_name(Sess);
    outs() << "=== " << ProjectName << " v" << VersionString << " (" << Command
           << ") ===\n";
    outs() << "File:  " << Path.filename().string() << "\n";
    outs() << "Arch:  " << ArchStr << "\n";
    neverd_free_string(ArchStr);
  }

  // Dispatch to the category handler for the active subcommand.  Each reads its
  // own options and returns the process exit code.
  if (InfoCmd)
    return runInfo(Sess);
  if (HeadersCmd)
    return runHeaders(Sess);
  if (DashboardCmd)
    return runDashboard(Sess);
  if (ImportsCmd)
    return runImports(Sess);
  if (ExportsCmd)
    return runExports(Sess);
  if (SegmentsCmd)
    return runSegments(Sess);
  if (SectionsCmd)
    return runSections(Sess);
  if (SymbolsCmd)
    return runSymbols(Sess);
  if (RelocsCmd)
    return runRelocs(Sess);
  if (EntryPointsCmd)
    return runEntryPoints(Sess);
  if (StringsCmd)
    return runStrings(Sess);
  if (FuncsCmd)
    return runFuncs(Sess);
  if (DisasmCmd)
    return runDisasm(Sess);
  if (HexCmd)
    return runHex(Sess);
  if (CfgCmd)
    return runCfg(Sess);
  if (XrefsCmd)
    return runXrefs(Sess);
  if (CallGraphCmd)
    return runCallGraph(Sess);
  if (BookmarksCmd)
    return runBookmarks();
  if (AnnotateCmd)
    return runAnnotate(Sess);
  if (RenameCmd)
    return runRename(Sess);
  if (SearchCmd)
    return runSearch(Sess);
  if (SigsCmd)
    return runSigs(Sess, Argv[0]);
  if (ExportCmd)
    return runExport(Sess);
  if (LiftCmd)
    return runLift(Sess);
  if (DecompileCmd)
    return runDecompile(Sess);
  if (PatchCmd)
    return runPatch(Sess);

  return 0;
}
