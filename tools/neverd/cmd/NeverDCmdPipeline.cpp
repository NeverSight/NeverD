//===- NeverDCmdPipeline.cpp - Engine-driven commands --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Handlers that drive the decompiler engine through the C API: `plugins`,
/// `lift`, `decompile`, and `patch`.  All heavy pipeline work stays behind the
/// C API so the CLI and GUI share one runtime path and this TU never
/// instantiates LLVM PassManager templates.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"
#include "NeverDSanitizerPublicationCLI.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

// This TU uses LLVM_DEBUG (which expands DEBUG_TYPE); define a category so the
// macro resolves.  Placed after the includes so a header that defines and
// #undefs its own DEBUG_TYPE cannot clobber it.
#define DEBUG_TYPE "neverd-cli"

using namespace llvm;

namespace neverd::cli {

namespace {

int PluginExecutableAnchor;

bool isMissingOptionalValue(StringRef Value) {
  return Value.size() == 1 && Value.front() == '\0';
}

std::optional<std::string> validateSanitizeArguments() {
  const int Occurrences = PatchSanitize.getNumOccurrences();
  if (Occurrences == 0)
    return std::nullopt;
  if (Occurrences > 1 || PatchSanitize.size() > 1)
    return "--sanitize may be specified at most once";
  if (PatchSanitize.empty() || PatchSanitize.front().empty() ||
      isMissingOptionalValue(PatchSanitize.front()))
    return "--sanitize requires a value; supported value: " +
           PatchSanitizeRequiredValue.str();
  if (llvm::StringRef(PatchSanitize.front()) != PatchSanitizeRequiredValue)
    return "unsupported --sanitize value '" + PatchSanitize.front() +
           "'; supported value: " + PatchSanitizeRequiredValue.str();

  struct Conflict {
    int Occurrences;
    const char *Flag;
  };
  const Conflict Conflicts[] = {
      {PatchFromIR.getNumOccurrences(), "--from-ir"},
      {PatchFromC.getNumOccurrences(), "--from-c"},
      {PatchFuncAddr.getNumOccurrences(), "--func"},
      {MaxFunc.getNumOccurrences(), "--max-func"},
      {InjectHello.getNumOccurrences(), "--hello"},
      {RunNop.getNumOccurrences(), "--nop"},
  };
  for (const Conflict &Entry : Conflicts)
    if (Entry.Occurrences != 0)
      return "--sanitize=strict cannot be combined with " +
             std::string(Entry.Flag);
  return std::nullopt;
}

const char *hardforkSpelling(evm::Hardfork Fork) {
  switch (Fork) {
#define EVM_HARDFORK(NAME, SPELLING)                                           \
  case evm::Hardfork::NAME:                                                    \
    return SPELLING;
#include "neverd/evm/bytecode/EVMHardforks.def"
  }
  return "";
}

const char *sbfVersionSpelling(sbf::Version TheVersion) {
  switch (TheVersion) {
#define SBF_VERSION_AUTO(NAME, SPELLING, DISPLAY_NAME)                         \
  case sbf::Version::NAME:                                                     \
    return SPELLING;
#define SBF_VERSION(NAME, ELF_FLAGS, SPELLING, DISPLAY_NAME, FEATURES, STATUS) \
  case sbf::Version::NAME:                                                     \
    return SPELLING;
#include "neverd/sbf/image/SBFVersions.def"
  default:
    return "";
  }
}

const char *sbfClusterSpelling(sbf::Cluster Cluster) {
  switch (Cluster) {
#define SBF_CLUSTER(ID, NAME, ACTIVATES_EVERYTHING, SUMMARY)                   \
  case sbf::Cluster::ID:                                                       \
    return NAME;
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
  default:
    return "";
  }
}

const char *sbfLoaderSpelling(sbf::Loader Loader) {
  switch (Loader) {
#define SBF_LOADER(ID, NAME, KNOWN_ADDRESS, ACCOUNT_ABI, DEPLOYS, EXECUTES,    \
                   SUMMARY)                                                    \
  case sbf::Loader::ID:                                                        \
    return NAME;
#include "neverd/sbf/runtime/SBFLoaders.def"
  default:
    return "";
  }
}

const char *sbfPurposeSpelling(sbf::RuntimePurpose Purpose) {
  switch (Purpose) {
#define SBF_RUNTIME_PURPOSE(ID, NAME, SUMMARY)                                 \
  case sbf::RuntimePurpose::ID:                                                \
    return NAME;
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
  default:
    return "";
  }
}

bool configureEVM(neverd_session_t Sess) {
  neverd_evm_set_strict(Sess, EVMRelaxed ? 0 : 1);
  if (!neverd_evm_set_hardfork(Sess,
                               hardforkSpelling(EVMHardfork.getValue()))) {
    WithColor::error() << "invalid EVM hardfork: " << takeLastError(Sess)
                       << "\n";
    return false;
  }
  return true;
}

bool configureSBF(neverd_session_t Sess) {
  neverd_sbf_set_strict(Sess, SBFRelaxed ? 0 : 1);
  if (!neverd_sbf_set_version(Sess,
                              sbfVersionSpelling(SBFVersion.getValue()))) {
    WithColor::error() << "invalid SBF version: " << takeLastError(Sess)
                       << "\n";
    return false;
  }
  neverd_sbf_set_slot(Sess, SBFSlot.getValue());
  const auto Select = [Sess](const char *What,
                             int (*Set)(neverd_session_t, const char *),
                             const char *Name) {
    if (Set(Sess, Name))
      return true;
    WithColor::error() << "invalid " << What << ": " << takeLastError(Sess)
                       << "\n";
    return false;
  };
  if (!Select("Solana cluster", neverd_sbf_set_cluster,
              sbfClusterSpelling(SBFCluster.getValue())) ||
      !Select("Solana loader", neverd_sbf_set_loader,
              sbfLoaderSpelling(SBFLoader.getValue())) ||
      !Select("runtime purpose", neverd_sbf_set_purpose,
              sbfPurposeSpelling(SBFPurpose.getValue())))
    return false;
  if (!SBFIdl.empty()) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> Document =
        MemoryBuffer::getFile(SBFIdl);
    if (!Document) {
      WithColor::error() << "cannot read " << SBFIdl << ": "
                         << Document.getError().message() << "\n";
      return false;
    }
    if (!neverd_sbf_set_idl(Sess, (*Document)->getBuffer().str().c_str())) {
      WithColor::error() << takeLastError(Sess) << "\n";
      return false;
    }
  }
  return true;
}

} // namespace

bool configureAnalysisSession(neverd_session_t Sess) {
  return configureEVM(Sess) && configureSBF(Sess);
}

int runPlugins(const char *Argv0) {
  neverd_session_t Sess = neverd_session_create();
  SessionGuard SessGuard(Sess);

  std::map<std::string, std::string> ScannedDirectories;
  const auto LoadPluginDirectory = [&](StringRef Directory,
                                       bool Required) -> bool {
    if (Directory.empty())
      return true;

    SmallString<256> CanonicalDirectory;
    if (std::error_code EC =
            sys::fs::real_path(Directory, CanonicalDirectory)) {
      if (Required)
        WithColor::error() << "cannot resolve plugin directory '" << Directory
                           << "': " << EC.message() << "\n";
      return !Required;
    }

    bool IsDirectory = false;
    std::error_code EC = sys::fs::is_directory(CanonicalDirectory, IsDirectory);
    if (EC) {
      if (Required)
        WithColor::error() << "cannot inspect plugin directory '" << Directory
                           << "': " << EC.message() << "\n";
      return !Required;
    }
    if (!IsDirectory) {
      if (Required)
        WithColor::error() << "plugin path '" << Directory
                           << "' is not a directory\n";
      return !Required;
    }

    const std::string Canonical = CanonicalDirectory.str().str();
    if (auto It = ScannedDirectories.find(Canonical);
        It != ScannedDirectories.end()) {
      if (Required && !It->second.empty()) {
        WithColor::error() << "failed to load plugin directory '" << Directory
                           << "': " << It->second << "\n";
        return false;
      }
      return true;
    }

    neverd_plugins_load_dir(Sess, Canonical.c_str());
    std::string LoadError = takeLastError(Sess);
    ScannedDirectories.emplace(Canonical, LoadError);
    if (LoadError.empty())
      return true;

    if (Required) {
      WithColor::error() << "failed to load plugin directory '" << Directory
                         << "': " << LoadError << "\n";
      return false;
    }
    WithColor::warning() << "failed to load plugin directory '" << Directory
                         << "': " << LoadError << "\n";
    return true;
  };

  std::string Executable =
      sys::fs::getMainExecutable(Argv0, &PluginExecutableAnchor);
  SmallString<256> SiblingPlugins(Executable.empty() ? Argv0 : Executable);
  sys::path::remove_filename(SiblingPlugins);
  sys::path::append(SiblingPlugins, "plugins");
  LoadPluginDirectory(SiblingPlugins, false);

  SmallString<256> HomePlugins;
  bool HasHomeDirectory = false;
  if (const char *Home = getenv("HOME"); Home && Home[0] != '\0') {
    HomePlugins = Home;
    HasHomeDirectory = true;
  } else {
    HasHomeDirectory = sys::path::home_directory(HomePlugins);
  }
  if (HasHomeDirectory) {
    sys::path::append(HomePlugins, ".neverd", "plugins");
    LoadPluginDirectory(HomePlugins, false);
  }

  if (const char *Env = getenv("NEVERD_PLUGIN_PATH")) {
    std::string EnvStr(Env);
    size_t Pos = 0;
    while (Pos < EnvStr.size()) {
      size_t Sep = EnvStr.find(llvm::sys::EnvPathSeparator, Pos);
      if (Sep == std::string::npos)
        Sep = EnvStr.size();
      const std::string Directory = EnvStr.substr(Pos, Sep - Pos);
      if (!Directory.empty() && !LoadPluginDirectory(Directory, true))
        return 1;
      Pos = Sep + 1;
    }
  }
  if (!PluginDir.empty() && !LoadPluginDirectory(PluginDir.getValue(), true))
    return 1;

  if (PluginList || PluginRun.empty()) {
    const char *Json = neverd_plugins_list_json(Sess);
    if (JsonOutput) {
      outs() << (Json ? Json : "[]") << "\n";
    } else {
      auto Parsed = json::parse(Json ? Json : "[]");
      if (Parsed) {
        if (auto *Arr = Parsed->getAsArray()) {
          if (Arr->empty()) {
            outs() << "No plugins loaded.\n";
          } else {
            outs() << "Loaded plugins (" << Arr->size() << "):\n";
            for (const auto &V : *Arr) {
              auto *Obj = V.getAsObject();
              if (!Obj)
                continue;
              outs() << "  " << Obj->getString("name").value_or("?") << " v"
                     << Obj->getString("version").value_or("?") << " — "
                     << Obj->getString("description").value_or("") << "\n";
            }
          }
        }
      }
    }
    neverd_free_string(Json);
    return 0;
  }

  if (!PluginRun.empty()) {
    if (!PluginBinary.empty()) {
      if (!neverd_session_load(Sess, PluginBinary.getValue().c_str())) {
        WithColor::error() << "failed to load binary: "
                           << PluginBinary.getValue() << ": "
                           << takeLastError(Sess) << "\n";
        return 1;
      }
    }
    neverd_plugins_init(Sess);
    if (std::string InitError = takeLastError(Sess); !InitError.empty()) {
      WithColor::error() << "failed to initialize plugins: " << InitError
                         << "\n";
      neverd_plugins_term(Sess);
      return 1;
    }

    int Ret = neverd_plugins_run(Sess, PluginRun.getValue().c_str(), 0);
    std::string RunError = takeLastError(Sess);
    if (Ret == -1) {
      WithColor::error() << (RunError.empty() ? "plugin run failed" : RunError)
                         << "\n";
      neverd_plugins_term(Sess);
      return 1;
    }

    if (!JsonOutput)
      outs() << "Plugin '" << PluginRun.getValue() << "' returned " << Ret
             << "\n";
    else {
      json::Object Result;
      Result["plugin"] = PluginRun.getValue();
      Result["result"] = Ret;
      outs() << json::Value(std::move(Result)) << "\n";
    }
    neverd_plugins_term(Sess);
    if (std::string TermError = takeLastError(Sess); !TermError.empty()) {
      WithColor::error() << "failed to terminate plugins: " << TermError
                         << "\n";
      return 1;
    }
    return (Ret == 0) ? 0 : 1;
  }

  return 0;
}

int runLift(neverd_session_t Sess) {
  const char *InPath = InputFile.getValue().c_str();

  if (DumpLow || DumpMed || DumpHigh) {
    int Level = DumpLow ? 0 : (DumpMed ? 1 : 2);
    const char *Dump = neverd_lift_dump(Sess, InPath, Level, MaxFunc);
    if (!Dump) {
      WithColor::error() << "dump failed: " << takeLastError(Sess) << "\n";
      return 1;
    }
    outs() << Dump;
    neverd_free_string(Dump);
    return 0;
  }

  const char *IR = neverd_lift_module(Sess, InPath, NoOpt, MaxFunc);
  if (!IR) {
    WithColor::error() << "lift failed: " << takeLastError(Sess) << "\n";
    return 1;
  }
  if (!OutputFile.empty()) {
    std::error_code EC;
    raw_fd_ostream OS(OutputFile, EC);
    if (EC) {
      WithColor::error() << "cannot open: " << EC.message() << "\n";
      neverd_free_string(IR);
      return 1;
    }
    OS << IR;
    outs() << "IR written to " << OutputFile.getValue() << "\n";
  } else {
    outs() << IR;
  }
  neverd_free_string(IR);
  return 0;
}

int runDecompile(neverd_session_t Sess) {
  const char *InPath = InputFile.getValue().c_str();

  const neverd_output_language_t Language = OutputLanguage.getValue();
  const bool DedicatedLanguage = Language != NEVERD_OUTPUT_C;
  if (DedicatedLanguage && LlvmRoute) {
    WithColor::error()
        << "--llvm cannot be combined with a dedicated source language\n";
    return 1;
  }
  const char *Source =
      DedicatedLanguage
          ? neverd_decompile_all_ex(Sess, InPath, Language, NoOpt, MaxFunc)
          : neverd_decompile_all(Sess, InPath, LlvmRoute ? 1 : 0, NoOpt,
                                 MaxFunc);
  if (!Source) {
    WithColor::error() << "decompile failed: " << takeLastError(Sess) << "\n";
    return 1;
  }
  if (!OutputFile.empty()) {
    std::error_code EC;
    raw_fd_ostream OS(OutputFile, EC);
    if (EC) {
      WithColor::error() << "cannot open: " << EC.message() << "\n";
      neverd_free_string(Source);
      return 1;
    }
    OS << Source;
    outs() << outputLanguageDisplayName(Language) << " source written to "
           << OutputFile.getValue() << "\n";
  } else {
    outs() << Source;
  }
  neverd_free_string(Source);
  return 0;
}

int runPatch(neverd_session_t Sess) {
  const char *InPath = InputFile.getValue().c_str();

  if (std::optional<std::string> Error = validateSanitizeArguments()) {
    WithColor::error() << *Error << "\n";
    return 1;
  }
  const bool StrictSanitize = PatchSanitize.getNumOccurrences() != 0;

  std::string OutPath = OutputFile.getValue();
  if (OutPath.empty())
    OutPath = InputFile.getValue() + ".patched";

  int Strategy = PatchStrat == InplaceMode ? 1 : 0;
  neverd_set_inst_substitution(Sess, InstSubst ? 1 : 0, InstSubstRounds);
  neverd_set_constant_encryption(Sess, ConstEnc ? 1 : 0);
  neverd_set_opaque_predicate(Sess, OpaquePred ? 1 : 0);
  neverd_set_control_flow_flattening(Sess, Flatten ? 1 : 0);
  neverd_set_bogus_control_flow(Sess, BogusCF ? 1 : 0);
  neverd_set_indirect_branch(Sess, IndirectBr ? 1 : 0);
  neverd_set_indirect_call(Sess, IndirectCall ? 1 : 0);
  neverd_set_mba(Sess, Mba ? 1 : 0);
  neverd_set_indirect_global(Sess, IndirectGv ? 1 : 0);
  neverd_set_value_launder(Sess, ValueLaunder ? 1 : 0);
  neverd_set_constant_pooling(Sess, ConstPool ? 1 : 0);
  neverd_set_bit_masking(Sess, BitMask ? 1 : 0);
  neverd_set_text_section(Sess, TextSection.c_str());
  std::optional<neverd_sanitize_result_v1> SanitizeResult;
  std::optional<sanitizer_publication::SuccessDisposition>
      SanitizePublicationDisposition;
  int Ret = 0;
  if (StrictSanitize) {
    const uint32_t PublicationABIVersion =
        neverd_sanitize_publication_abi_version();
    if (PublicationABIVersion !=
        sanitizer_publication::kSupportedPublicationABIVersion) {
      WithColor::error()
          << "strict sanitize failed: unsupported publication ABI version "
          << PublicationABIVersion << "; expected "
          << sanitizer_publication::kSupportedPublicationABIVersion
          << "; no output was attempted\n";
      return 1;
    }

    neverd_sanitize_options_v1 Options{};
    Options.struct_size = sizeof(Options);
    Options.strategy = PatchStrat == InplaceMode
                           ? NEVERD_SANITIZE_STRATEGY_INPLACE
                           : NEVERD_SANITIZE_STRATEGY_SECTION;

    SanitizeResult.emplace();
    SanitizeResult->struct_size = sizeof(*SanitizeResult);
    const int Sanitized = neverd_session_sanitize(Sess, OutPath.c_str(),
                                                  &Options, &*SanitizeResult);
    if (Sanitized != 1 || SanitizeResult->ok != 1 ||
        SanitizeResult->status != NEVERD_SANITIZE_STATUS_OK) {
      std::string Detail = takeLastError(Sess);
      if (Detail.empty())
        Detail = SanitizeResult->status != NEVERD_SANITIZE_STATUS_OK
                     ? neverd_sanitize_status_name(SanitizeResult->status)
                     : "inconsistent sanitizer success result";
      Detail = sanitizer_publication::failureMessage(
          *SanitizeResult, std::move(Detail), OutPath);
      if (sanitizer_publication::
              terminalTupleRequiresUnknownDestinationAdvisory(Sanitized,
                                                              *SanitizeResult))
        Detail = sanitizer_publication::unknownDestinationStateMessage(
            std::move(Detail), OutPath);
      WithColor::error() << "strict sanitize failed: " << Detail << "\n";
      return 1;
    }
    if (std::optional<std::string> Error =
            sanitizer_publication::validateSuccessResult(*SanitizeResult,
                                                         OutPath)) {
      WithColor::error() << "strict sanitize failed: " << *Error << "\n";
      return 1;
    }
    SanitizePublicationDisposition =
        sanitizer_publication::successDisposition(*SanitizeResult);
  } else if (!PatchFromIR.empty()) {
    // Patch from external LLVM IR: skip the lift stage and feed the IR
    // straight into the rewrite pipeline (section or inplace per --mode).
    auto IRBuf = MemoryBuffer::getFile(PatchFromIR.getValue());
    if (!IRBuf) {
      WithColor::error() << "cannot read IR file: " << PatchFromIR.getValue()
                         << "\n";
      return 1;
    }
    Ret = neverd_patch_from_ir(Sess, (*IRBuf)->getBufferStart(), Strategy,
                               OutPath.c_str())
              ? 0
              : 1;
  } else if (!PatchFromC.empty()) {
    auto CBuf = MemoryBuffer::getFile(PatchFromC.getValue());
    if (!CBuf) {
      WithColor::error() << "cannot read C file: " << PatchFromC.getValue()
                         << "\n";
      return 1;
    }
    uint64_t FuncAddr = 0;
    if (!PatchFuncAddr.empty())
      StringRef(PatchFuncAddr.getValue()).getAsInteger(0, FuncAddr);
    Ret = neverd_patch_from_c(Sess, (*CBuf)->getBufferStart(), FuncAddr,
                              OutPath.c_str())
              ? 0
              : 1;
  } else {
    Ret = neverd_patch_full(Sess, InPath, OutPath.c_str(), Strategy, NoOpt,
                            InjectHello ? 1 : 0, RunNop ? 1 : 0, MaxFunc);
  }
  if (Ret != 0) {
    WithColor::error() << "patch failed: " << takeLastError(Sess) << "\n";
    return 1;
  }
  const char *PatchOut = neverd_patch_output_path(Sess);
  if (SanitizeResult) {
    if (std::optional<std::string> Error =
            sanitizer_publication::validateSuccessOutputPath(PatchOut,
                                                             OutPath)) {
      neverd_free_string(PatchOut);
      WithColor::error() << "strict sanitize failed: " << *Error << "\n";
      return 1;
    }
  }
  std::string FinalPath = PatchOut ? PatchOut : OutPath;
  const uint64_t CodeSize =
      SanitizeResult ? SanitizeResult->code_size : neverd_patch_code_size(Sess);
  const uint64_t TrampolineCount =
      SanitizeResult
          ? SanitizeResult->trampoline_count
          : static_cast<uint64_t>(neverd_patch_trampoline_count(Sess));
  if (!SanitizePublicationDisposition ||
      *SanitizePublicationDisposition ==
          sanitizer_publication::SuccessDisposition::CreatedExclusive)
    outs() << "Patched binary written to " << FinalPath << " (" << CodeSize
           << " bytes code, " << TrampolineCount << " trampolines)\n";
  else
    outs() << "Existing binary authenticated at " << FinalPath << " ("
           << CodeSize << " bytes code, " << TrampolineCount
           << " trampolines)\n";
  if (SanitizeResult) {
    outs() << "strict sanitizer publication: "
           << sanitizer_publication::successMessage(
                  *SanitizePublicationDisposition)
           << "\n";
    outs() << "strict sanitizer: guarded sites: "
           << SanitizeResult->guarded_sites
           << ", unsupported sites: " << SanitizeResult->unsupported_sites
           << ", patched functions: " << SanitizeResult->patched_functions
           << "\n";
  }
  if (InstSubst)
    outs() << "instruction substitution: "
           << neverd_patch_substitution_count(Sess) << " operator(s)\n";
  if (ConstEnc)
    outs() << "constant encryption: "
           << neverd_patch_constant_encryption_count(Sess) << " constant(s)\n";
  if (OpaquePred)
    outs() << "opaque predicate: " << neverd_patch_opaque_predicate_count(Sess)
           << " predicate(s)\n";
  if (Flatten)
    outs() << "control-flow flattening: "
           << neverd_patch_control_flow_flattening_count(Sess) << " block(s)\n";
  if (BogusCF)
    outs() << "bogus control flow: "
           << neverd_patch_bogus_control_flow_count(Sess) << " block(s)\n";
  if (IndirectBr)
    outs() << "indirect branch: " << neverd_patch_indirect_branch_count(Sess)
           << " branch(es)\n";
  if (IndirectCall)
    outs() << "indirect call: " << neverd_patch_indirect_call_count(Sess)
           << " call(s)\n";
  if (Mba)
    outs() << "mba: " << neverd_patch_mba_count(Sess) << " operator(s)\n";
  if (IndirectGv)
    outs() << "indirect global: " << neverd_patch_indirect_global_count(Sess)
           << " reference(s)\n";
  if (ValueLaunder)
    outs() << "value launder: " << neverd_patch_value_launder_count(Sess)
           << " value(s)\n";
  if (ConstPool)
    outs() << "constant pooling: " << neverd_patch_constant_pooling_count(Sess)
           << " constant(s)\n";
  if (BitMask)
    outs() << "bit masking: " << neverd_patch_bit_masking_count(Sess)
           << " value(s)\n";
  if (PatchOut)
    neverd_free_string(PatchOut);

#ifdef __APPLE__
  if (!StrictSanitize)
    if (auto Codesign = sys::findProgramByName("codesign")) {
      std::vector<StringRef> Args = {*Codesign, "-f", "-s", "-", FinalPath};
      if (sys::ExecuteAndWait(*Codesign, Args) == 0)
        LLVM_DEBUG(llvm::dbgs()
                   << "patch: ad-hoc codesigned " << FinalPath << "\n");
    }
#endif
  return 0;
}

} // namespace neverd::cli
