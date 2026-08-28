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
#include "neverd/support/StackSizeMain.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <filesystem>

using namespace llvm;
using namespace neverd;
using namespace neverd::cli;

static int realMain(int Argc, char *Argv[]);

int main(int Argc, char *Argv[]) {
  return neverd::runWithLargeStack(realMain, Argc, Argv);
}

// LLVM's default reaction to a failed allocation is "LLVM ERROR: out of memory"
// plus a symbol-less stack trace, which reads like an LLVM bug and tells the
// user nothing about what to do.  Name the levers that lower lift memory.  LLVM
// requires a bad-allocation handler not to allocate and not to return, so keep
// this to raw diagnostic writes and terminate immediately afterwards.
static void reportOutOfMemory(void *, const char *Reason, bool) {
  errs()
      << "error: out of memory: " << (Reason ? Reason : "allocation failed")
      << "\n"
         "  Large binaries retain more IR, and concurrent LLVM emission\n"
         "  adds one working set per worker thread.  To lower the peak:\n"
         "    * NEVERD_THREADS=1 (or 2) to shrink the concurrent working set\n"
         "    * --max-func=N to lift only the first N functions\n"
         "    * --no-opt to skip the LLVM optimization pipeline\n";
  if constexpr (sizeof(void *) == 4)
    errs() << "  This is a 32-bit build; its address space caps usable memory\n"
              "  at 2-4 GB regardless of how much RAM the machine has.  A\n"
              "  64-bit build is the real fix for multi-megabyte inputs.\n";
  errs().flush();
  std::_Exit(1);
}

static int realMain(int Argc, char *Argv[]) {
  InitLLVM X(Argc, Argv);
  install_bad_alloc_error_handler(reportOutOfMemory);

  cl::ParseCommandLineOptions(
      Argc, Argv,
      (Twine(ProjectName) + " Decompiler Engine v" + VersionString).str());

  if (Verbose)
    llvm::DebugFlag = true;

  // These are handled before the shared session load, because none of them
  // works from one binary at a path: plugins needs no input at all, diff takes
  // its own -a/-b operands, simplify's input is an expression, and optimize-ir
  // reads textual LLVM IR.
  if (PluginsCmd)
    return runPlugins(Argv[0]);
  if (DiffCmd)
    return runDiff();
  if (SimplifyCmd)
    return runSimplify();
  if (OptimizeIRCmd)
    return runOptimizeIR();
  if (TranslateObjectCmd)
    return runTranslateObject();

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

  // The debug-symbol search is part of loading, so its policy has to be in
  // place before the load rather than adjusted afterwards.
  neverd_session_set_debug_info_enabled(Sess, NoDebug ? 0 : 1);
  if (!PdbFile.getValue().empty())
    neverd_session_set_pdb_path(Sess, PdbFile.getValue().c_str());
  if (!MapFile.getValue().empty())
    neverd_session_set_map_path(Sess, MapFile.getValue().c_str());

  if (!neverd_session_load(Sess, InputFile.getValue().c_str())) {
    WithColor::error() << "failed to load: " << takeLastError(Sess) << "\n";
    return 1;
  }
  if ((LiftCmd || DecompileCmd || DisasmCmd || CfgCmd || SymbolicCmd ||
       AuditCmd || HuntCmd) &&
      !configureAnalysisSession(Sess))
    return 1;

  // Hunt and audit name callees through the same identity view as the rest of
  // the engine, so optional signature matching has to land before they run.
  if ((AuditCmd || HuntCmd) &&
      (SigAuto || !SigFile.getValue().empty() || !SigDir.getValue().empty())) {
    Expected<int> MatchCount = applyRequestedSignatures(Sess, Argv[0]);
    if (!MatchCount) {
      WithColor::error() << toString(MatchCount.takeError()) << "\n";
      return 1;
    }
  }

  if (!JsonOutput && !SymbolicCmd && !AuditCmd && !HuntCmd) {
    const char *ArchStr = neverd_session_arch_name(Sess);
    outs() << "=== " << ProjectName << " v" << VersionString << " (" << Command
           << ") ===\n";
    outs() << "File:  " << Path.filename().string() << "\n";
    outs() << "Arch:  " << ArchStr << "\n";
    neverd_free_string(ArchStr);

    // Which names the run is working from is worth stating up front: it
    // explains why a function reads as `parse_header` in one run and
    // `sub_140001A20` in the next.
    const char *DbgKind = neverd_session_debug_info_kind(Sess);
    const char *DbgPath = neverd_session_debug_info_path(Sess);
    if (DbgPath[0] != '\0')
      outs() << "Debug: " << DbgKind << " ("
             << std::filesystem::path(DbgPath).filename().string() << ")\n";
    neverd_free_string(DbgKind);
    neverd_free_string(DbgPath);
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
  if (SymbolicCmd)
    return runSymbolicExplore(Sess);
  if (AuditCmd)
    return runAudit(Sess);
  if (HuntCmd)
    return runHunt(Sess);
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
