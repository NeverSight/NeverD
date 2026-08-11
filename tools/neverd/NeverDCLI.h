//===- NeverDCLI.h - Shared declarations for the neverd CLI ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal header shared by the neverd tool's translation units.  The command
/// handlers are split by category across NeverDCmd*.cpp; every one of them
/// reads the same set of command-line options (defined once in
/// NeverDCLIOptions.cpp) and returns a process exit code.  This header declares
/// those options with external linkage, the two small helpers the handlers
/// share, and each run* handler entry point.
///
/// This mirrors the lib/sdk split (NeverDCAPI*.cpp sharing SessionImpl.h):
/// keep one logical component in several focused files behind a private header
/// instead of a single oversized translation unit.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERDCLI_H
#define NEVERD_TOOLS_NEVERDCLI_H

#include "neverd/evm/Opcodes.h"
#include "neverd/sbf/RuntimeProfile.h"
#include "neverd/sbf/Version.h"
#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd::cli {

//===----------------------------------------------------------------------===//
// Shared option value types
//===----------------------------------------------------------------------===//

/// What the `export` subcommand should write out.
enum ExportFormat {
  FmtDecompile,
  FmtIR,
  FmtFuncs,
  FmtImports,
  FmtExports,
  FmtStrings
};

/// How the `patch` subcommand rewrites the binary.
enum PatchStrategy { SectionMode, InplaceMode };

//===----------------------------------------------------------------------===//
// Shared helpers
//===----------------------------------------------------------------------===//

/// Scope guard that destroys a neverd_session_t on exit, so every command
/// handler can return on any path without repeating -- or forgetting --
/// neverd_session_destroy().
class SessionGuard {
public:
  explicit SessionGuard(neverd_session_t S) : Sess(S) {}
  ~SessionGuard() {
    if (Sess)
      neverd_session_destroy(Sess);
  }
  SessionGuard(const SessionGuard &) = delete;
  SessionGuard &operator=(const SessionGuard &) = delete;

private:
  neverd_session_t Sess;
};

inline std::string takeLastError(neverd_session_t Sess) {
  const char *Error = neverd_last_error(Sess);
  std::string Result = Error ? Error : "";
  neverd_free_string(Error);
  return Result;
}

inline llvm::StringRef
outputLanguageDisplayName(neverd_output_language_t Language) {
  switch (Language) {
#define NEVERD_OUTPUT_LANGUAGE(NAME, VALUE, SPELLING, DISPLAY_NAME)            \
  case NEVERD_OUTPUT_##NAME:                                                   \
    return DISPLAY_NAME;
#include "neverd/OutputLanguages.def"
  }
  return "unknown";
}

/// Parse an address argument that may carry an optional "0x" prefix; both
/// forms are interpreted as hexadecimal, the convention shared by every
/// neverd subcommand.
std::optional<uint64_t> parseAddrArg(llvm::StringRef Ref);

//===----------------------------------------------------------------------===//
// Subcommands (defined in NeverDCLIOptions.cpp)
//===----------------------------------------------------------------------===//

extern llvm::cl::SubCommand LiftCmd;
extern llvm::cl::SubCommand DecompileCmd;
extern llvm::cl::SubCommand PatchCmd;
extern llvm::cl::SubCommand InfoCmd;
extern llvm::cl::SubCommand StringsCmd;
extern llvm::cl::SubCommand XrefsCmd;
extern llvm::cl::SubCommand FuncsCmd;
extern llvm::cl::SubCommand DisasmCmd;
extern llvm::cl::SubCommand CfgCmd;
extern llvm::cl::SubCommand HexCmd;
extern llvm::cl::SubCommand ImportsCmd;
extern llvm::cl::SubCommand ExportsCmd;
extern llvm::cl::SubCommand SegmentsCmd;
extern llvm::cl::SubCommand PluginsCmd;
extern llvm::cl::SubCommand ExportCmd;
extern llvm::cl::SubCommand BookmarksCmd;
extern llvm::cl::SubCommand AnnotateCmd;
extern llvm::cl::SubCommand DiffCmd;
extern llvm::cl::SubCommand CallGraphCmd;
extern llvm::cl::SubCommand RenameCmd;
extern llvm::cl::SubCommand SearchCmd;
extern llvm::cl::SubCommand SectionsCmd;
extern llvm::cl::SubCommand SymbolsCmd;
extern llvm::cl::SubCommand RelocsCmd;
extern llvm::cl::SubCommand HeadersCmd;
extern llvm::cl::SubCommand EntryPointsCmd;
extern llvm::cl::SubCommand DashboardCmd;
extern llvm::cl::SubCommand SigsCmd;

//===----------------------------------------------------------------------===//
// Options (defined in NeverDCLIOptions.cpp)
//===----------------------------------------------------------------------===//

// Common options, registered with many subcommands.
extern llvm::cl::opt<std::string> InputFile;
extern llvm::cl::opt<std::string> OutputFile;
extern llvm::cl::opt<bool> Verbose;
extern llvm::cl::opt<bool> JsonOutput;
extern llvm::cl::opt<bool> NoDebug;
extern llvm::cl::opt<bool> NoOpt;
extern llvm::cl::opt<size_t> MaxFunc;

// Patch obfuscation-pass toggles.
extern llvm::cl::opt<bool> InjectHello;
extern llvm::cl::opt<bool> RunNop;
extern llvm::cl::opt<bool> InstSubst;
extern llvm::cl::opt<unsigned> InstSubstRounds;
extern llvm::cl::opt<bool> ConstEnc;
extern llvm::cl::opt<bool> OpaquePred;
extern llvm::cl::opt<bool> Flatten;
extern llvm::cl::opt<bool> BogusCF;
extern llvm::cl::opt<bool> IndirectBr;
extern llvm::cl::opt<bool> IndirectCall;
extern llvm::cl::opt<bool> Mba;
extern llvm::cl::opt<bool> IndirectGv;
extern llvm::cl::opt<bool> ValueLaunder;
extern llvm::cl::opt<bool> ConstPool;
extern llvm::cl::opt<bool> BitMask;

// Lift.
extern llvm::cl::opt<bool> DumpLow;
extern llvm::cl::opt<bool> DumpMed;
extern llvm::cl::opt<bool> DumpHigh;

// Decompile.
extern llvm::cl::opt<bool> LlvmRoute;
extern llvm::cl::opt<neverd_output_language_t> OutputLanguage;
extern llvm::cl::opt<evm::Hardfork> EVMHardfork;
extern llvm::cl::opt<bool> EVMRelaxed;
extern llvm::cl::opt<sbf::Version> SBFVersion;
extern llvm::cl::opt<bool> SBFRelaxed;
extern llvm::cl::opt<std::string> SBFIdl;
extern llvm::cl::opt<sbf::Cluster> SBFCluster;
extern llvm::cl::opt<uint64_t> SBFSlot;
extern llvm::cl::opt<sbf::Loader> SBFLoader;
extern llvm::cl::opt<sbf::RuntimePurpose> SBFPurpose;

// Strings.
extern llvm::cl::opt<unsigned> MinStrLen;

// Xrefs.
extern llvm::cl::opt<std::string> XrefAddr;

// Funcs / Disasm / Cfg / Hex.
extern llvm::cl::opt<std::string> DisasmFunc;
extern llvm::cl::opt<bool> DisasmAnnotate;
extern llvm::cl::opt<std::string> HexAddr;
extern llvm::cl::opt<unsigned> HexSize;
extern llvm::cl::opt<bool> CfgDot;
extern llvm::cl::opt<std::string> CfgSvg;

// Plugins.
extern llvm::cl::opt<bool> PluginList;
extern llvm::cl::opt<std::string> PluginRun;
extern llvm::cl::opt<std::string> PluginBinary;
extern llvm::cl::opt<std::string> PluginDir;

// Bookmarks.
extern llvm::cl::opt<bool> BookmarkList;
extern llvm::cl::opt<std::string> BookmarkAdd;
extern llvm::cl::opt<std::string> BookmarkName;
extern llvm::cl::opt<std::string> BookmarkRemove;

// Diff.
extern llvm::cl::opt<std::string> DiffFileA;
extern llvm::cl::opt<std::string> DiffFileB;
extern llvm::cl::opt<std::string> DiffFunc;
extern llvm::cl::opt<bool> DiffJson;

// Annotate.
extern llvm::cl::opt<bool> AnnotateList;
extern llvm::cl::opt<std::string> AnnotateAdd;
extern llvm::cl::opt<std::string> AnnotateText;
extern llvm::cl::opt<std::string> AnnotateRemove;

// Export.
extern llvm::cl::opt<ExportFormat> ExportFmt;
extern llvm::cl::opt<std::string> ExportOutput;
extern llvm::cl::opt<std::string> ExportFunc;

// Rename.
extern llvm::cl::opt<std::string> RenameFrom;
extern llvm::cl::opt<std::string> RenameTo;
extern llvm::cl::opt<bool> RenameList;

// Search.
extern llvm::cl::opt<std::string> SearchText;
extern llvm::cl::opt<std::string> SearchHex;
extern llvm::cl::opt<bool> SearchCaseSensitive;
extern llvm::cl::opt<unsigned> SearchMaxResults;

// CallGraph.
extern llvm::cl::opt<bool> CgDot;
extern llvm::cl::opt<std::string> CgSvg;

// Patch from external file.
extern llvm::cl::opt<std::string> PatchFromIR;
extern llvm::cl::opt<std::string> PatchFromC;
extern llvm::cl::opt<std::string> PatchFuncAddr;

// Patch strategy.
extern llvm::cl::opt<PatchStrategy> PatchStrat;
extern llvm::cl::opt<std::string> TextSection;

// Sigs.
extern llvm::cl::opt<std::string> SigDir;
extern llvm::cl::opt<std::string> SigFile;
extern llvm::cl::opt<bool> SigAuto;

//===----------------------------------------------------------------------===//
// Command handlers
//
// Handlers reading a loaded session take it as the first argument.  Each reads
// its own options from the globals above and returns a process exit code.
//===----------------------------------------------------------------------===//

// NeverDCmdInfo.cpp — metadata summaries.
int runInfo(neverd_session_t Sess);
int runHeaders(neverd_session_t Sess);
int runDashboard(neverd_session_t Sess);

// NeverDCmdTables.cpp — flat listings backed by a *_json C-API call.
int runImports(neverd_session_t Sess);
int runExports(neverd_session_t Sess);
int runSegments(neverd_session_t Sess);
int runSections(neverd_session_t Sess);
int runSymbols(neverd_session_t Sess);
int runRelocs(neverd_session_t Sess);
int runEntryPoints(neverd_session_t Sess);
int runStrings(neverd_session_t Sess);

// NeverDCmdDisasm.cpp — code views.
int runFuncs(neverd_session_t Sess);
int runDisasm(neverd_session_t Sess);
int runHex(neverd_session_t Sess);
int runCfg(neverd_session_t Sess);
int runXrefs(neverd_session_t Sess);
int runCallGraph(neverd_session_t Sess);

// NeverDCmdMarkup.cpp — user annotations persisted beside the binary.
// runBookmarks operates purely on the JSON sidecar, so it needs no session.
int runBookmarks();
int runAnnotate(neverd_session_t Sess);
int runRename(neverd_session_t Sess);

// NeverDCmdSearch.cpp — byte/string search and signature matching.
int runSearch(neverd_session_t Sess);
int runSigs(neverd_session_t Sess, const char *Argv0);

// NeverDCmdExport.cpp — file export and two-binary diff.
int runExport(neverd_session_t Sess);
int runDiff();

// NeverDCmdPipeline.cpp — engine-driven operations.
bool configureAnalysisSession(neverd_session_t Sess);
int runPlugins(const char *Argv0);
int runLift(neverd_session_t Sess);
int runDecompile(neverd_session_t Sess);
int runPatch(neverd_session_t Sess);

} // namespace neverd::cli

#endif // NEVERD_TOOLS_NEVERDCLI_H
