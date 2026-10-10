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

#include "neverd/evm/bytecode/EVMOpcodes.h"
#include "neverd/sbf/image/SBFVersion.h"
#include "neverd/sbf/runtime/SBFRuntimeProfile.h"
#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"

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
  FmtStrings,
  FmtObjCMethods,
  FmtObjCMethodsSummary,
  FmtSwiftMethods
};

/// How the `patch` subcommand rewrites the binary.
enum PatchStrategy { SectionMode, InplaceMode };

/// CLI-only spelling for the translation object container.  The public C ABI
/// deliberately uses fixed-width integers, while llvm::cl literal options need
/// an actual enum type for their parser.
enum class TranslateObjectContainer { ELF, MachO };

/// Where `neverd xrefs` takes references from.
enum class XrefSourceKind {
  /// What each instruction states, as the GUI lists them.
  Direct,
  /// The constants of the analyzed IR.
  IR
};

/// Preserve LLVM's distinction between a missing optional value (`--func`)
/// and an explicitly empty one (`--func=`).  Options using ValueOptional need
/// that distinction so their handlers can produce stable, domain-specific
/// diagnostics; the private NUL sentinel cannot occur in an argv string.
class OptionalStringParser : public llvm::cl::parser<std::string> {
public:
  explicit OptionalStringParser(llvm::cl::Option &Option)
      : llvm::cl::parser<std::string>(Option) {}

  bool parse(llvm::cl::Option &, llvm::StringRef, llvm::StringRef Argument,
             std::string &Value) {
    if (!Argument.data()) {
      Value.assign(1, '\0');
      return false;
    }
    Value = Argument.str();
    return false;
  }
};

using OptionalStringList =
    llvm::cl::list<std::string, bool, OptionalStringParser>;
using ConcolicStringList = OptionalStringList;

#define NEVERD_CLI_LITERAL(OPTION, SPELLING)                                   \
  inline constexpr llvm::StringLiteral OPTION##RequiredValue = SPELLING;
#include "NeverDCLIValues.def"
#undef NEVERD_CLI_LITERAL

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

/// Reads a hexadecimal address, with or without "0x".
inline bool parseHexAddress(const std::string &Text, neverd_va_t &Addr) {
  llvm::StringRef Ref(Text);
  if (Ref.empty() || Ref.front() == '-')
    return false;
  if ((Ref.consume_front("0x") || Ref.consume_front("0X")) && Ref.empty())
    return false;
  return !Ref.getAsInteger(16, Addr);
}

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

/// Apply the signature source selected by --auto, --sig-file, or --sig-dir.
/// Returns an error when no source was selected or matching failed.
llvm::Expected<int> applyRequestedSignatures(neverd_session_t Sess,
                                             const char *Argv0);

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
extern llvm::cl::SubCommand FunctionEditsCmd;
extern llvm::cl::SubCommand ItemsCmd;
extern llvm::cl::SubCommand IdentifyCmd;
extern llvm::cl::SubCommand OperandsCmd;
extern llvm::cl::SubCommand SearchCmd;
extern llvm::cl::SubCommand SectionsCmd;
extern llvm::cl::SubCommand SymbolsCmd;
extern llvm::cl::SubCommand RelocsCmd;
extern llvm::cl::SubCommand HeadersCmd;
extern llvm::cl::SubCommand EntryPointsCmd;
extern llvm::cl::SubCommand SwitchesCmd;
extern llvm::cl::SubCommand DashboardCmd;
extern llvm::cl::SubCommand SigsCmd;
extern llvm::cl::SubCommand SimplifyCmd;
extern llvm::cl::SubCommand SymbolicCmd;
extern llvm::cl::SubCommand OptimizeIRCmd;
extern llvm::cl::SubCommand TranslateObjectCmd;
extern llvm::cl::SubCommand EmulateDriverCmd;
extern llvm::cl::SubCommand CPUCapabilitiesCmd;
extern llvm::cl::SubCommand EmulateProcessCmd;
extern llvm::cl::SubCommand UnpackCmd;
extern llvm::cl::SubCommand AuditCmd;
extern llvm::cl::SubCommand HuntCmd;
extern llvm::cl::SubCommand ConcolicCmd;
extern llvm::cl::SubCommand MobileCmd;
extern llvm::cl::SubCommand WebCmd;
extern llvm::cl::list<std::string> WebArguments;
int runWeb();
int runWebPackages();
int runWebArchive();
int runWebIntegrity();
int runWebInterfaces();

extern llvm::cl::opt<bool> Devirtualize;
extern llvm::cl::list<std::string> VMControlRegisters;
extern llvm::cl::list<std::string> VMControlFrameSlots;
extern llvm::cl::opt<unsigned> VMMaxNodes;
extern llvm::cl::opt<unsigned> VMMaxContexts;
extern llvm::cl::opt<std::string> VMMaxRefinements;
extern llvm::cl::opt<std::string> VMMaxFields;
extern llvm::cl::opt<std::string> VMMaxQueries;
extern llvm::cl::opt<std::string> VMMaxEvaluations;
extern llvm::cl::opt<std::string> VMMaxDiscoveryVisits;
extern llvm::cl::opt<std::string> VMChainTransfers;
extern llvm::cl::opt<std::string> VMChainVisits;
extern llvm::cl::opt<bool> VMChainStopAtRepeat;
extern llvm::cl::opt<std::string> VMEntryFrame;
extern llvm::cl::opt<std::string> VMEntryAlignment;
extern llvm::cl::opt<bool> VMExternalStoresDisjointFrame;
extern llvm::cl::opt<std::string> VMMaxSymbolicNodes;
extern llvm::cl::opt<bool> VMNoControlDiscovery;
extern llvm::cl::opt<uint64_t> VMMaxOperations;
extern llvm::cl::opt<std::string> VMRecoveryReport;
extern llvm::cl::opt<bool> VMMachineState;

int runMobile(const char *Argv0);
extern llvm::cl::opt<std::string> MobilePlatform;
extern llvm::cl::opt<std::string> MobileArch;
extern llvm::cl::opt<std::string> MobileArtifact;
extern llvm::cl::opt<bool> MobileMetadataOnly;
extern llvm::cl::opt<bool> MobileListClasses;
extern llvm::cl::opt<std::string> MobileClassPrefix;
extern llvm::cl::opt<std::string> MobileFindRefs;
extern llvm::cl::opt<std::string> MobileReferenceQuery;
extern llvm::cl::opt<bool> MobileReferenceExact;
extern llvm::cl::opt<std::string> MobileReferenceOwner;
extern llvm::cl::opt<bool> MobileInternalIOSWorker;
extern llvm::cl::opt<unsigned> MobileTimeout;
extern llvm::cl::opt<unsigned> MobileMaxFiles;
extern llvm::cl::opt<uint64_t> MobileMaxBytes;

//===----------------------------------------------------------------------===//
// Options (defined in NeverDCLIOptions.cpp)
//===----------------------------------------------------------------------===//

// Common options, registered with many subcommands.
extern llvm::cl::opt<std::string> InputFile;
extern llvm::cl::opt<std::string> OutputFile;
extern llvm::cl::opt<bool> Verbose;
extern llvm::cl::opt<bool> JsonOutput;
extern llvm::cl::opt<bool> NoDebug;
extern llvm::cl::list<std::string> ARMFunctionModeHints;
extern llvm::cl::opt<std::string> PdbFile;
extern llvm::cl::opt<std::string> MapFile;
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
extern llvm::cl::list<std::string> StringEncodings;
extern llvm::cl::opt<std::string> StringCodePage;
extern llvm::cl::opt<bool> StringRefs;
extern llvm::cl::opt<std::string> StringFilter;

// Xrefs.
extern llvm::cl::opt<std::string> XrefAddr;
extern llvm::cl::opt<XrefSourceKind> XrefSource;

// Funcs / Disasm / Cfg / Hex.
extern llvm::cl::opt<std::string> DisasmFunc;
extern llvm::cl::opt<bool> DisasmAnnotate;
extern llvm::cl::opt<std::string> HexAddr;
extern llvm::cl::opt<unsigned> HexSize;
extern llvm::cl::opt<std::string> HexTextEncoding;
extern llvm::cl::opt<bool> CfgDot;
extern llvm::cl::opt<std::string> CfgSvg;
extern llvm::cl::opt<unsigned> SymbolicMaxPaths;
extern llvm::cl::opt<unsigned> SymbolicMaxSteps;
extern llvm::cl::opt<unsigned> SymbolicMaxBlockVisits;
extern llvm::cl::opt<bool> SymbolicExpressions;

// Safety (audit / hunt).
extern llvm::cl::opt<std::string> SafetySinks;
extern llvm::cl::opt<std::string> SafetySources;
extern llvm::cl::opt<unsigned> SafetyMaxPaths;
extern llvm::cl::opt<unsigned> SafetyMaxSteps;
extern llvm::cl::opt<unsigned> SafetyMaxLoop;
extern llvm::cl::opt<unsigned> SafetyMaxCallDepth;
extern llvm::cl::opt<unsigned> SafetyMaxSummaryIterations;
extern llvm::cl::opt<unsigned long long> SafetySolverConflicts;

// LowIR concolic branch flipping.  Strings are parsed by the handler so
// overflow, boolean-like values, and repeated scalar flags become stable JSON
// errors instead of command-line library diagnostics.
extern llvm::cl::list<std::string> ConcolicInputFiles;
extern ConcolicStringList ConcolicOutputFiles;
extern ConcolicStringList ConcolicFunctions;
extern ConcolicStringList ConcolicSeeds;
extern ConcolicStringList ConcolicMaxSteps;
extern ConcolicStringList ConcolicMaxBlockVisits;
extern ConcolicStringList ConcolicMaxLoopIterations;
extern ConcolicStringList ConcolicMaxFlipAttempts;
extern ConcolicStringList ConcolicMaxCandidates;
extern ConcolicStringList ConcolicSolverConflicts;
extern ConcolicStringList ConcolicSolverPropagations;
extern ConcolicStringList ConcolicSolverWatchVisits;
extern ConcolicStringList ConcolicSolverGates;

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
extern llvm::cl::opt<std::string> ExportSourceSignatures;

// Rename.
extern llvm::cl::opt<std::string> RenameFrom;
extern llvm::cl::opt<std::string> RenameTo;
extern llvm::cl::opt<bool> RenameList;
extern llvm::cl::opt<std::string> RenameAddr;
extern llvm::cl::opt<bool> RenameClear;

// Function edits.
extern llvm::cl::opt<std::string> FunctionCreate;
extern llvm::cl::opt<std::string> FunctionDelete;
extern llvm::cl::opt<bool> FunctionEditsList;
extern llvm::cl::opt<std::string> ItemData;
extern llvm::cl::opt<std::string> ItemString;
extern llvm::cl::opt<std::string> ItemUndefine;
extern llvm::cl::opt<std::string> ItemClear;
extern llvm::cl::opt<unsigned> ItemSize;
extern llvm::cl::opt<std::string> ItemEncoding;
extern llvm::cl::opt<std::string> OperandAddr;
extern llvm::cl::opt<std::string> LoadLoader;
extern llvm::cl::opt<std::string> LoadProcessor;
extern llvm::cl::opt<std::string> LoadPlatform;
extern llvm::cl::opt<std::string> LoadBase;
extern llvm::cl::opt<std::string> LoadOffset;
extern llvm::cl::opt<std::string> LoadSize;
extern llvm::cl::opt<std::string> LoadEntry;
extern llvm::cl::opt<unsigned> OperandIndex;
extern llvm::cl::opt<std::string> OperandBase;
extern llvm::cl::opt<bool> OperandNegate;
extern llvm::cl::opt<bool> OperandInvert;
extern llvm::cl::opt<bool> OperandClear;

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
extern OptionalStringList PatchSanitize;

// Patch strategy.
extern llvm::cl::opt<PatchStrategy> PatchStrat;
extern llvm::cl::opt<std::string> TextSection;

// Sigs.
extern llvm::cl::opt<std::string> SigDir;
extern llvm::cl::opt<std::string> SigFile;
extern llvm::cl::opt<bool> SigAuto;
extern llvm::cl::opt<std::string> SigBase;

// Simplify.
extern llvm::cl::opt<std::string> SimplifyExpr;
extern llvm::cl::opt<std::string> SimplifyFile;
extern llvm::cl::opt<unsigned> SimplifyWidth;
extern llvm::cl::opt<bool> SimplifyShallow;
extern llvm::cl::opt<unsigned> SimplifyMaxAtoms;
extern llvm::cl::opt<unsigned long long> SimplifyMaxWork;
extern llvm::cl::opt<bool> SimplifyExhaustive;
extern llvm::cl::opt<unsigned> SimplifyVerifySamples;
extern llvm::cl::opt<bool> SimplifyAllowGrowth;
extern llvm::cl::opt<bool> SimplifyStats;
extern llvm::cl::opt<bool> SimplifyJson;
extern llvm::cl::opt<bool> SimplifySynthesize;
extern llvm::cl::opt<unsigned long long> SimplifyMaxCost;
extern llvm::cl::opt<unsigned long long> SimplifyMaxSamples;
extern llvm::cl::opt<unsigned> SimplifyMaxLeaves;
extern llvm::cl::opt<unsigned> SimplifyMaxConstants;
extern llvm::cl::opt<unsigned> SimplifyStochasticSlots;
extern llvm::cl::opt<unsigned> SimplifyStochasticRestarts;
extern llvm::cl::opt<unsigned long long> SimplifyStochasticIterations;
extern llvm::cl::opt<unsigned long long> SimplifySolverMaxConflicts;
extern llvm::cl::opt<unsigned long long> SimplifySolverMaxPropagations;
extern llvm::cl::opt<unsigned long long> SimplifySolverMaxWatchVisits;
extern llvm::cl::opt<std::string> SimplifySolver;
extern llvm::cl::opt<unsigned> SimplifySolverTimeoutMs;

// Optimize textual LLVM IR.
extern llvm::cl::opt<std::string> OptimizeIRInput;
extern llvm::cl::opt<std::string> OptimizeIROutput;
extern llvm::cl::opt<neverd_optimization_mode_t> OptimizeIRMode;
extern llvm::cl::opt<neverd_llvm_optimization_level_t> OptimizeIRLevel;
extern llvm::cl::opt<unsigned> OptimizeIRMaxRounds;
extern llvm::cl::opt<bool> OptimizeIRSynthesize;
extern llvm::cl::opt<unsigned long long> OptimizeIRSynthesisMaxCost;
extern llvm::cl::opt<unsigned long long> OptimizeIRSynthesisMaxSamples;
extern llvm::cl::opt<unsigned> OptimizeIRSynthesisVerifySamples;
extern llvm::cl::opt<unsigned long long> OptimizeIRSynthesisMaxWork;
extern llvm::cl::opt<unsigned> OptimizeIRSynthesisMaxLeaves;
extern llvm::cl::opt<unsigned> OptimizeIRSynthesisMaxConstants;
extern llvm::cl::opt<unsigned> OptimizeIRSynthesisStochasticSlots;
extern llvm::cl::opt<unsigned> OptimizeIRSynthesisStochasticRestarts;
extern llvm::cl::opt<unsigned long long>
    OptimizeIRSynthesisStochasticIterations;
extern llvm::cl::opt<unsigned long long> OptimizeIRSolverMaxConflicts;
extern llvm::cl::opt<unsigned long long> OptimizeIRSolverMaxPropagations;
extern llvm::cl::opt<unsigned long long> OptimizeIRSolverMaxWatchVisits;
extern llvm::cl::opt<bool> OptimizeIRExhaustive;
extern llvm::cl::opt<bool> OptimizeIRJson;

// Translate only canonical, legacy-prefix-free x86-64 v1 REX.W full-width GPR
// MOV, ADD/SUB, and register/immediate AND/OR/XOR forms; full-width
// register-only CMP 39/3B and register/immediate CMP 81/7, 83/7, and 3D; and
// full-width register-only TEST 85 plus register/immediate TEST F7/0 and A9.
// Logical and TEST forms compute architecture-defined flags while preserving
// AF in the NeverD state model. Canonical C3 RET, C2 iw RET-imm16, or
// direct-relative EB cb/E9 cd JMP terminates the block. Lowering
// schema 9 also accepts canonical, legacy-prefix-free traditional Jcc: JO/JNO
// 70/71 or 0F 80/81, JB/JAE 72/73 or 0F 82/83, JE/JNE 74/75 or 0F 84/85,
// JBE/JA 76/77 or 0F 86/87, JS/JNS 78/79 or 0F 88/89, JP/JNP 7A/7B or 0F
// 8A/8B, JL/JGE 7C/7D or 0F 8C/8D, and JLE/JG 7E/7F or 0F 8E/8F, with cb short
// or cd near displacements respectively. JRCXZ/JECXZ/JCXZ and
// LOOP/LOOPE/LOOPNE remain unpublished and fail closed. Reserved F7 /1, memory
// operands, partial registers, legacy prefixes, semantically redundant REX
// extension bits, and any instruction outside that exact subset also fail
// closed.
extern llvm::cl::opt<std::string> TranslateObjectInput;
extern llvm::cl::opt<std::string> TranslateObjectOutput;
extern llvm::cl::opt<TranslateObjectContainer> TranslateObjectFormat;
extern llvm::cl::opt<std::string> TranslateObjectEntry;
extern llvm::cl::opt<unsigned long long> TranslateObjectGeneration;
extern llvm::cl::opt<std::string> EmulateDriverInput;
extern llvm::cl::opt<unsigned long long> DriverInstructionLimit;
extern llvm::cl::opt<std::string> DriverScenarioFile;
extern llvm::cl::opt<std::string> DriverBackend;
extern llvm::cl::opt<std::string> DriverExecutionContract;
extern llvm::cl::opt<std::string> CPUConfiguration;
extern llvm::cl::opt<bool> CPUProbeHost;
extern llvm::cl::opt<std::string> ProcessInput;
extern llvm::cl::opt<std::string> ProcessProfile;
extern llvm::cl::opt<std::string> ProcessOptions;
extern llvm::cl::opt<std::string> UnpackInput;
extern llvm::cl::opt<std::string> UnpackOutput;
extern llvm::cl::opt<std::string> UnpackOptions;

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
int runSwitches(neverd_session_t Sess);
int runStrings(neverd_session_t Sess);

// NeverDCmdDisasm.cpp — code views.
int runFuncs(neverd_session_t Sess);
int runDisasm(neverd_session_t Sess);
int runHex(neverd_session_t Sess);
int runCfg(neverd_session_t Sess);
int runXrefs(neverd_session_t Sess);
int runCallGraph(neverd_session_t Sess);
int runSymbolicExplore(neverd_session_t Sess);

// NeverDCmdSafety.cpp — memory-safety audit and hunt over the lifted IR.
int runAudit(neverd_session_t Sess);
int runHunt(neverd_session_t Sess);

// NeverDCmdConcolic.cpp — dedicated versioned JSON command.  It owns its
// session so load, argument, target, and pipeline failures share one schema.
int runConcolic();

// NeverDCmdMarkup.cpp — user annotations persisted beside the binary.
// runBookmarks operates purely on the JSON sidecar, so it needs no session.
int runBookmarks();
int runAnnotate(neverd_session_t Sess);
int runRename(neverd_session_t Sess);
int runFunctionEdits(neverd_session_t Sess);
int runItems(neverd_session_t Sess);
int runOperands(neverd_session_t Sess);

// NeverDCmdSearch.cpp — byte/string search and signature matching.
int runSearch(neverd_session_t Sess);
int runSigs(neverd_session_t Sess, const char *Argv0);

// NeverDCmdExport.cpp — file export and two-binary diff.
int runExport(neverd_session_t Sess);
int runDiff();
int runIdentify();

// NeverDCmdSimplify.cpp — semantic optimisation of a written expression.
// Takes no session: its input is text, not a binary.
int runSimplify();

// NeverDCmdOptimizeIR.cpp — transactional semantic + LLVM optimization.
// Takes textual IR rather than a binary session.
int runOptimizeIR();

// NeverDCmdTranslate.cpp — only canonical, legacy-prefix-free x86-64 v1 REX.W
// full-width GPR MOV, ADD/SUB, and register/immediate AND/OR/XOR forms;
// full-width register-only CMP 39/3B and register/immediate CMP 81/7, 83/7,
// and 3D; and full-width register-only TEST 85 plus register/immediate TEST
// F7/0 and A9. Logical and TEST forms compute architecture-defined flags while
// preserving AF in the NeverD state model. Canonical C3 RET, C2 iw RET-imm16,
// or direct-relative EB cb/E9 cd JMP terminates the block. Lowering schema 9
// also accepts canonical, legacy-prefix-free
// traditional Jcc: JO/JNO 70/71 or 0F 80/81, JB/JAE 72/73 or 0F 82/83,
// JE/JNE 74/75 or 0F 84/85, JBE/JA 76/77 or 0F 86/87, JS/JNS 78/79 or 0F
// 88/89, JP/JNP 7A/7B or 0F 8A/8B, JL/JGE 7C/7D or 0F 8C/8D, and JLE/JG 7E/7F
// or 0F 8E/8F, with cb short or cd near displacements respectively.
// JRCXZ/JECXZ/JCXZ and LOOP/LOOPE/LOOPNE remain unpublished and fail closed.
// Reserved F7 /1, memory operands, partial registers, legacy prefixes,
// semantically redundant REX extension bits, and any instruction outside that
// exact subset also fail closed.
// Takes raw bytes rather than a binary session and does not link, load,
// publish, dispatch, execute, or debug.
int runTranslateObject();
int runEmulateDriver();
int runCPUCapabilities();
int runEmulateProcess();
int runUnpack();

// NeverDCmdPipeline.cpp — engine-driven operations.
bool configureAnalysisSession(neverd_session_t Sess);
int runPlugins(const char *Argv0);
int runLift(neverd_session_t Sess);
int runDecompile(neverd_session_t Sess);
int runPatch(neverd_session_t Sess);

} // namespace neverd::cli

#endif // NEVERD_TOOLS_NEVERDCLI_H
