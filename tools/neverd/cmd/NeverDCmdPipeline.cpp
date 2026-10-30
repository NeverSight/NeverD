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
#include "../ResourceLimits.h"
#include "NeverDSanitizerPublicationCLI.h"

#include "neverd/sdk/Limits.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
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
#include <optional>
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
      {PatchFromSource.getNumOccurrences(), "--from-source"},
      {PatchFromObject.getNumOccurrences(), "--from-object"},
      {PatchCompiler.getNumOccurrences(), "--compiler"},
      {PatchCompilerPath.getNumOccurrences(), "--compiler-path"},
      {PatchLanguage.getNumOccurrences(), "--language"},
      {PatchArtifact.getNumOccurrences(), "--artifact"},
      {PatchCompileMode.getNumOccurrences(), "--compile-mode"},
      {PatchEntry.getNumOccurrences(), "--entry"},
      {PatchCompilerArgs.getNumOccurrences(), "--compiler-arg"},
      {PatchBindings.getNumOccurrences(), "--bind"},
      {PatchDependencies.getNumOccurrences(), "--dependency"},
      {PatchCompilerTimeout.getNumOccurrences(), "--compiler-timeout"},
      {PatchReport.getNumOccurrences(), "--patch-report"},
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

std::optional<std::string> validateSourcePatchArguments() {
  const int Inputs = (PatchFromIR.getNumOccurrences() != 0) +
                     (PatchFromC.getNumOccurrences() != 0) +
                     (PatchFromSource.getNumOccurrences() != 0) +
                     (PatchFromObject.getNumOccurrences() != 0);
  if (Inputs > 1)
    return "select exactly one of --from-c, --from-source, --from-ir, or "
           "--from-object";
  const bool CompilerOptions = PatchCompiler.getNumOccurrences() ||
                               PatchCompilerPath.getNumOccurrences() ||
                               PatchLanguage.getNumOccurrences() ||
                               PatchArtifact.getNumOccurrences() ||
                               PatchCompileMode.getNumOccurrences() ||
                               PatchCompilerArgs.getNumOccurrences() ||
                               PatchCompilerTimeout.getNumOccurrences();
  if (CompilerOptions && PatchFromC.empty() && PatchFromSource.empty())
    return "compiler options require --from-c or --from-source";
  if (Inputs == 0 &&
      (PatchFuncAddr.getNumOccurrences() || PatchEntry.getNumOccurrences() ||
       PatchReport.getNumOccurrences() || PatchBindings.getNumOccurrences() ||
       PatchDependencies.getNumOccurrences()))
    return "replacement options require an external patch input";
  if (Inputs && PatchFromIR.empty() && PatchFromC.empty() &&
      PatchFromSource.empty() && PatchFromObject.empty())
    return "external patch input path is empty";
  if (PatchEntry.getNumOccurrences() &&
      (PatchEntry.empty() || !PatchFuncAddr.getNumOccurrences()))
    return "--entry requires a nonempty symbol and --func";
  uint64_t Address = 0;
  if (PatchFuncAddr.getNumOccurrences() &&
      (PatchFuncAddr.empty() ||
       StringRef(PatchFuncAddr).getAsInteger(0, Address)))
    return "--func requires a valid function address";
  if (PatchCompiler != "clang" && PatchCompiler != "neverc" &&
      PatchCompiler != "gcc")
    return "--compiler must be clang, neverc, or gcc";
  if (PatchLanguage != "c" && PatchLanguage != "c++")
    return "--language must be c or c++";
  if (!PatchFromC.empty() && PatchLanguage != "c")
    return "--from-c requires language c; use --from-source for C++";
  if (PatchArtifact != "ir" && PatchArtifact != "object")
    return "--artifact must be ir or object";
  if (PatchCompileMode != "normal" && PatchCompileMode != "dyncode")
    return "--compile-mode must be normal or dyncode";
  if (PatchCompilerTimeout == 0 ||
      PatchCompilerTimeout >
          ::neverd::limits::cli::
              kNeverDCmdPipelineValidateSourcePatchArgumentsPatchCompilerTimeoutLimit)
    return "--compiler-timeout must be between 1 and 3600 seconds";
  return std::nullopt;
}

bool usesSourcePatchRequest() {
  return !PatchFromC.empty() || !PatchFromSource.empty() ||
         !PatchFromObject.empty() ||
         (!PatchFromIR.empty() &&
          (PatchFuncAddr.getNumOccurrences() ||
           PatchEntry.getNumOccurrences() || PatchReport ||
           PatchBindings.getNumOccurrences() ||
           PatchDependencies.getNumOccurrences()));
}

int runSourcePatch(neverd_session_t Sess, StringRef Output, int Strategy) {
  const std::string &Input =
      !PatchFromC.empty()        ? PatchFromC.getValue()
      : !PatchFromSource.empty() ? PatchFromSource.getValue()
      : !PatchFromObject.empty() ? PatchFromObject.getValue()
                                 : PatchFromIR.getValue();
  auto Buffer = MemoryBuffer::getFile(Input);
  if (!Buffer) {
    WithColor::error() << "cannot read replacement input: " << Input << "\n";
    return 1;
  }
  neverd_source_patch_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.input_kind = !PatchFromObject.empty() ? NEVERD_SOURCE_PATCH_OBJECT
                       : !PatchFromIR.empty()   ? NEVERD_SOURCE_PATCH_LLVM_IR
                                                : NEVERD_SOURCE_PATCH_SOURCE;
  Options.compiler = PatchCompiler == "neverc" ? NEVERD_SOURCE_PATCH_NEVERC
                     : PatchCompiler == "gcc"  ? NEVERD_SOURCE_PATCH_GCC
                                               : NEVERD_SOURCE_PATCH_CLANG;
  Options.language =
      PatchLanguage == "c++" ? NEVERD_SOURCE_PATCH_CXX : NEVERD_SOURCE_PATCH_C;
  Options.artifact =
      PatchArtifact == "object" || !PatchFromObject.empty() ||
              ((PatchCompiler == "gcc" || PatchCompileMode == "dyncode") &&
               !PatchArtifact.getNumOccurrences())
          ? NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT
          : NEVERD_SOURCE_PATCH_ARTIFACT_IR;
  Options.mode = PatchCompileMode == "dyncode" ? NEVERD_SOURCE_PATCH_DYNCODE
                                               : NEVERD_SOURCE_PATCH_NORMAL;
  Options.strategy = Strategy;
  Options.compiler_path = PatchCompilerPath.c_str();
  Options.source_name = Input.c_str();
  Options.timeout_seconds = PatchCompilerTimeout;
  std::vector<const char *> Arguments;
  for (const std::string &Argument : PatchCompilerArgs)
    Arguments.push_back(Argument.c_str());
  Options.compiler_args = Arguments.data();
  Options.compiler_arg_count = Arguments.size();
  neverd_source_patch_mapping_v1 Mapping{};
  if (PatchFuncAddr.getNumOccurrences()) {
    Mapping.symbol = PatchEntry.c_str();
    StringRef(PatchFuncAddr).getAsInteger(0, Mapping.original_va);
    Options.mappings = &Mapping;
    Options.mapping_count = 1;
  }
  std::vector<std::string> BindingNames;
  std::vector<neverd_source_patch_binding_v1> Bindings;
  BindingNames.reserve(PatchBindings.size());
  for (const std::string &Text : PatchBindings) {
    const auto [Kind, Rest] = StringRef(Text).split(':');
    const auto [Symbol, Location] = Rest.split('=');
    const auto [Address, Size] = Location.split(':');
    neverd_source_patch_binding_v1 Binding{};
    if (Kind == "function")
      Binding.kind = NEVERD_SOURCE_BIND_FUNCTION;
    else if (Kind == "original")
      Binding.kind = NEVERD_SOURCE_BIND_ORIGINAL_FUNCTION;
    else if (Kind == "const")
      Binding.kind = NEVERD_SOURCE_BIND_READ_ONLY_DATA;
    else if (Kind == "data")
      Binding.kind = NEVERD_SOURCE_BIND_WRITABLE_DATA;
    else {
      WithColor::error()
          << "--bind kind must be function, original, const, or data\n";
      return 1;
    }
    if (Symbol.empty() || Address.empty() ||
        Address.getAsInteger(0, Binding.original_va) ||
        (Location.contains(':') &&
         (Size.empty() || Size.getAsInteger(0, Binding.size)))) {
      WithColor::error() << "--bind requires KIND:SYMBOL=VA[:SIZE]\n";
      return 1;
    }
    BindingNames.push_back(Symbol.str());
    Binding.symbol = BindingNames.back().c_str();
    Bindings.push_back(Binding);
  }
  Options.bindings = Bindings.data();
  Options.binding_count = Bindings.size();
  std::vector<std::string> ImportNames, Libraries;
  std::vector<neverd_source_patch_dependency_v1> Dependencies;
  ImportNames.reserve(PatchDependencies.size());
  Libraries.reserve(PatchDependencies.size());
  for (const auto &Text : PatchDependencies) {
    const auto [Kind, Rest] = StringRef(Text).split(':');
    const auto [Symbol, Library] = Rest.split('=');
    if ((Kind != "function" && Kind != "data") || Symbol.empty() ||
        Library.empty()) {
      WithColor::error()
          << "--dependency requires function|data:SYMBOL=LIBRARY\n";
      return 1;
    }
    ImportNames.push_back(Symbol.str());
    Libraries.push_back(Library.str());
    Dependencies.push_back(
        {Libraries.back().c_str(), ImportNames.back().c_str(),
         static_cast<uint32_t>(Kind == "function"
                                   ? NEVERD_SOURCE_DEPENDENCY_FUNCTION
                                   : NEVERD_SOURCE_DEPENDENCY_DATA)});
  }
  Options.dependencies = Dependencies.data();
  Options.dependency_count = Dependencies.size();
  return neverd_patch_source(Sess, (*Buffer)->getBufferStart(),
                             (*Buffer)->getBufferSize(), &Options,
                             Output.str().c_str())
             ? 0
             : 1;
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

/// One C file of \p Root and every function it calls, directly or not,
/// in one translation unit: the functions neverd_call_tree_json reaches,
/// with imports and library code that byte signatures name left declared.
const char *decompileWithCallees(neverd_session_t Sess, uint64_t Root,
                                 std::string &Out) {
  const std::string Options =
      "{\"root\":\"0x" + utohexstr(Root) +
      "\",\"max_depth\":" + std::to_string(sdk::kMaxCallPathDepth) +
      ",\"max_listed\":" + std::to_string(sdk::kMaxCallGraphListed) + "}";
  const char *Tree = neverd_call_tree_json(Sess, Options.c_str());
  if (!Tree)
    return "the callees cannot be listed";
  Expected<json::Value> Parsed = json::parse(Tree);
  neverd_free_string(Tree);
  const json::Object *Reply = Parsed ? Parsed->getAsObject() : nullptr;
  const json::Array *Order = Reply ? Reply->getArray("bottom_up") : nullptr;
  if (!Order) {
    if (!Parsed)
      consumeError(Parsed.takeError());
    return "the engine gave no callee order";
  }
  if (!Reply->getBoolean("complete").value_or(false))
    return "the callees exceed the listing limit";
  if (Reply->getBoolean("depth_limited").value_or(false))
    return "the callees exceed the call-depth limit";
  std::vector<neverd_va_t> Entries;
  for (const json::Value &Value : *Order) {
    uint64_t Entry = 0;
    if (StringRef(Value.getAsString().value_or(""))
            .drop_front(2)
            .getAsInteger(16, Entry))
      return "the engine gave a malformed callee";
    Entries.push_back(Entry);
  }
  const char *Source = neverd_decompile_functions(
      Sess, InputFile.getValue().c_str(), NEVERD_OUTPUT_C, Entries.data(),
      static_cast<int>(Entries.size()));
  if (!Source || !*Source) {
    neverd_free_string(Source);
    return "the functions do not decompile";
  }
  Out = Source;
  neverd_free_string(Source);
  return nullptr;
}

/// The note a whole unit's head gives \p Entry when the pipeline refused it
/// (`/* neverd.not-decompiled: NAME at 0xENTRY: REASON */`), without its
/// comment markers.
std::optional<std::string> refusalNote(StringRef Unit, uint64_t Entry) {
  const std::string At = " at 0x" + utohexstr(Entry) + ": ";
  for (StringRef Rest = Unit;
       Rest.consume_front("/* neverd.not-decompiled: ");) {
    auto [Note, After] = Rest.split('\n');
    if (Note.consume_back(" */") && Note.contains(At))
      return Note.str();
    Rest = After;
  }
  return std::nullopt;
}

/// The address the image exports \p Name at: a routine the function list
/// does not hold before discovery, as a leaf without unwind data.
std::optional<uint64_t> exportedAddress(neverd_session_t Sess, StringRef Name) {
  const char *Raw = neverd_exports_json(Sess);
  Expected<json::Value> Parsed = json::parse(Raw ? Raw : "[]");
  neverd_free_string(Raw);
  if (!Parsed) {
    consumeError(Parsed.takeError());
    return std::nullopt;
  }
  if (const json::Array *Exports = Parsed->getAsArray())
    for (const json::Value &Export : *Exports)
      if (const json::Object *Row = Export.getAsObject();
          Row && Row->getString("name") == Name) {
        uint64_t Address = 0;
        StringRef Text = Row->getString("addr").value_or("");
        if ((Text.consume_front("0x") || Text.consume_front("0X")) &&
            !Text.getAsInteger(16, Address))
          return Address;
      }
  return std::nullopt;
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
  if (X64Continuation.getNumOccurrences() ||
      ContinuationReport.getNumOccurrences())
    return runX64Continuation(Sess);
  const bool LLVMSourceTransforms = SourceLLVMControlFlow.getNumOccurrences() ||
                                    SourceLLVMLevel.getNumOccurrences() ||
                                    SourceLLVMPasses.getNumOccurrences();
  const bool SourceTransforms =
      SourceSymbolic.getNumOccurrences() || LLVMSourceTransforms;
  const bool HasStackFrame = StackFrameFile.getNumOccurrences() != 0;
  if ((StackFrameInfo || HasStackFrame) &&
      (ExportFunc.empty() || NoOpt || Devirtualize ||
       OutputLanguage != NEVERD_OUTPUT_C)) {
    WithColor::error()
        << "stack frame options require --func and C output, without "
           "--no-opt or --devirtualize\n";
    return 1;
  }
  if (StackFrameInfo && (SourceTransforms || LlvmRoute)) {
    WithColor::error()
        << "--stack-frame-info inspects machine stack offsets; omit source "
           "optimization options and --llvm\n";
    return 1;
  }
  if (SourceTransforms && (NoOpt || Devirtualize)) {
    WithColor::error() << "source optimization options cannot combine with "
                          "--no-opt or --devirtualize\n";
    return 1;
  }
  // Symbolic simplification also applies to the whole program; the LLVM
  // source options shape one function's view.
  if (LLVMSourceTransforms && ExportFunc.empty()) {
    WithColor::error() << "LLVM source optimization options require --func\n";
    return 1;
  }
  if (!LlvmRoute && (SourceLLVMControlFlow.getNumOccurrences() ||
                     SourceLLVMLevel.getNumOccurrences() ||
                     SourceLLVMPasses.getNumOccurrences())) {
    WithColor::error() << "LLVM source options require --llvm\n";
    return 1;
  }
  json::Object ViewOptions;
  if (SourceSymbolic.getNumOccurrences())
    ViewOptions["symbolic"] = bool(SourceSymbolic);
  if (SourceLLVMControlFlow.getNumOccurrences())
    ViewOptions["llvm_control_flow"] = bool(SourceLLVMControlFlow);
  if (SourceLLVMLevel.getNumOccurrences())
    ViewOptions["llvm_optimization"] = SourceLLVMLevel.getValue();
  if (SourceLLVMPasses.getNumOccurrences()) {
    json::Array Passes;
    for (const auto &Pass : SourceLLVMPasses)
      Passes.push_back(Pass);
    ViewOptions["llvm_passes"] = std::move(Passes);
  }
  std::string StackPolicyText = "{}";
  bool HasStackVariables = false;
  if (HasStackFrame) {
    uint64_t Size = 0;
    const auto EC = sys::fs::file_size(StackFrameFile.getValue(), Size);
    if (EC ||
        Size > ::neverd::limits::cli::kNeverDCmdPipelineRunDecompileSizeLimit) {
      WithColor::error() << "cannot read --stack-frame: "
                         << (EC ? EC.message() : "policy exceeds 128 KiB")
                         << '\n';
      return 1;
    }
    auto Buffer = MemoryBuffer::getFile(StackFrameFile.getValue());
    if (!Buffer ||
        (*Buffer)->getBufferSize() >
            ::neverd::limits::cli::
                kNeverDCmdPipelineRunDecompileBufferGetBufferSizeLimit) {
      WithColor::error() << "cannot read bounded --stack-frame policy\n";
      return 1;
    }
    auto Parsed = json::parse((*Buffer)->getBuffer());
    if (!Parsed) {
      WithColor::error() << "invalid --stack-frame JSON: "
                         << toString(Parsed.takeError()) << '\n';
      return 1;
    }
    auto *Policy = Parsed->getAsObject();
    if (Policy && Policy->get("policy"))
      Policy = Policy->getObject("policy");
    if (!Policy) {
      WithColor::error() << "--stack-frame must contain a policy object\n";
      return 1;
    }
    if (const auto *Variables = Policy->getArray("variables"))
      HasStackVariables = !Variables->empty();
    StackPolicyText.clear();
    raw_string_ostream(StackPolicyText) << json::Value(json::Object(*Policy));
    ViewOptions["stack_frame"] = json::Object(*Policy);
  }
  std::string ViewOptionsText;
  raw_string_ostream(ViewOptionsText) << json::Value(std::move(ViewOptions));
  const char *InPath = InputFile.getValue().c_str();

  const neverd_output_language_t Language = OutputLanguage.getValue();
  const bool DedicatedLanguage = Language != NEVERD_OUTPUT_C;
  // C++, Rust, Go and the program's own language spell a native function's
  // HighC source, one function as well as the whole program.
  const char *SourceStage = Language == NEVERD_OUTPUT_CPP      ? "cpp"
                            : Language == NEVERD_OUTPUT_RUST   ? "rust"
                            : Language == NEVERD_OUTPUT_GO     ? "go"
                            : Language == NEVERD_OUTPUT_SOURCE ? "source"
                                                               : nullptr;
  if (JsonOutput &&
      (ExportFunc.empty() || (DedicatedLanguage && !SourceStage) ||
       Devirtualize || NoOpt)) {
    WithColor::error()
        << "--json requires --func with the default HighC, "
           "--llvm, C++, Rust, Go or program-language source view\n";
    return 1;
  }
  if (DedicatedLanguage && LlvmRoute) {
    WithColor::error()
        << "--llvm cannot be combined with a dedicated source language\n";
    return 1;
  }
  if (!ExportFunc.empty() && DedicatedLanguage && !SourceStage) {
    WithColor::error() << "--func is supported only for C, C++, Rust, Go and "
                          "program-language output\n";
    return 1;
  }
  if (!ExportFunc.empty() && MaxFunc > 0) {
    WithColor::error() << "--func cannot be combined with --max-func\n";
    return 1;
  }

  if (Devirtualize && (ExportFunc.empty() || DedicatedLanguage)) {
    WithColor::error() << "--devirtualize requires --func and C output\n";
    return 1;
  }
  if (!Devirtualize &&
      (VMMachineState || VMMachineSourceABI.getNumOccurrences() ||
       !VMControlRegisters.empty() || !VMControlFrameSlots.empty() ||
       !VMRecoveryReport.empty() || VMMaxNodes.getNumOccurrences() ||
       VMMaxContexts.getNumOccurrences() ||
       VMMaxOperations.getNumOccurrences() ||
       VMMaxRefinements.getNumOccurrences() ||
       VMMaxFields.getNumOccurrences() || VMMaxQueries.getNumOccurrences() ||
       VMMaxEvaluations.getNumOccurrences() ||
       VMMaxDiscoveryVisits.getNumOccurrences() ||
       VMChainTransfers.getNumOccurrences() ||
       VMChainVisits.getNumOccurrences() ||
       VMChainStopAtRepeat.getNumOccurrences() ||
       VMEntryFrame.getNumOccurrences() ||
       VMEntryAlignment.getNumOccurrences() ||
       VMExternalStoresDisjointFrame.getNumOccurrences() ||
       VMProveStoreReturnSeparation.getNumOccurrences() ||
       VMProveStoreFrameSeparation.getNumOccurrences() ||
       VMMaxSymbolicNodes.getNumOccurrences() ||
       VMNoControlDiscovery.getNumOccurrences())) {
    WithColor::error() << "VM recovery options require --devirtualize\n";
    return 1;
  }
  if (Devirtualize && (!VMMaxNodes || !VMMaxContexts || !VMMaxOperations)) {
    WithColor::error() << "VM recovery budgets must be positive\n";
    return 1;
  }
  uint32_t MachineSourceABI = NEVERD_DEVIRTUALIZE_MACHINE_SCALAR_V1;
  if (VMMachineSourceABI.getNumOccurrences()) {
    if (!VMMachineState) {
      WithColor::error() << "--vm-source-abi requires --vm-machine-state\n";
      return 1;
    }
    if (VMMachineSourceABI == "queries")
      MachineSourceABI = NEVERD_DEVIRTUALIZE_MACHINE_QUERIES_V1;
    else if (VMMachineSourceABI == "xmm")
      MachineSourceABI = NEVERD_DEVIRTUALIZE_MACHINE_QUERIES_XMM_V2;
    else if (VMMachineSourceABI != "scalar") {
      WithColor::error() << "--vm-source-abi expects scalar, queries or xmm\n";
      return 1;
    }
  }
  uint32_t MaxRefinements = 0, MaxFields = 0, MaxQueries = 0,
           ChainTransfers = 0, MaxSymbolicNodes = 0, MaxEvaluations = 0,
           MaxDiscoveryVisits = 0, ChainVisits = 0;
  const auto ParseCount = [](StringRef Text, StringRef Option, uint32_t &Value,
                             bool AllowZero = false) {
    if (Text.empty() ||
        Text.find_first_not_of("0123456789") != StringRef::npos ||
        Text.getAsInteger(10, Value) || (!AllowZero && !Value)) {
      WithColor::error() << Option
                         << (AllowZero ? " expects a nonnegative"
                                       : " expects a positive")
                         << " 32-bit decimal integer\n";
      return false;
    }
    return true;
  };
  if (Devirtualize &&
      (!ParseCount(VMMaxRefinements.getValue(), "--vm-max-refinements",
                   MaxRefinements) ||
       !ParseCount(VMMaxFields.getValue(), "--vm-max-fields", MaxFields) ||
       !ParseCount(VMMaxQueries.getValue(), "--vm-max-queries", MaxQueries) ||
       !ParseCount(VMMaxEvaluations.getValue(), "--vm-max-evaluations",
                   MaxEvaluations) ||
       !ParseCount(VMMaxDiscoveryVisits.getValue(), "--vm-max-discovery-visits",
                   MaxDiscoveryVisits) ||
       !ParseCount(VMMaxSymbolicNodes.getValue(), "--vm-max-symbolic-nodes",
                   MaxSymbolicNodes) ||
       !ParseCount(VMChainTransfers.getValue(), "--vm-chain-transfers",
                   ChainTransfers, true) ||
       !ParseCount(VMChainVisits.getValue(), "--vm-chain-visits", ChainVisits,
                   true))) {
    return 1;
  }
  uint32_t EntryAlignment = 0, EntryResidue = 0;
  if (VMEntryAlignment.getNumOccurrences()) {
    if (!VMMachineState) {
      WithColor::error()
          << "--vm-entry-alignment requires --vm-machine-state\n";
      return 1;
    }
    const auto Parts = StringRef(VMEntryAlignment.getValue()).split(':');
    if (!ParseCount(Parts.first, "--vm-entry-alignment", EntryAlignment) ||
        !ParseCount(Parts.second, "--vm-entry-alignment residue", EntryResidue,
                    true))
      return 1;
    // The shared entry-domain validator owns power-of-two/residue validity.
  }
  int64_t EntryFrameBegin = 0, EntryFrameEnd = 0;
  if (VMExternalStoresDisjointFrame &&
      (!VMMachineState || !VMEntryFrame.getNumOccurrences())) {
    WithColor::error() << "--vm-external-stores-disjoint-frame requires "
                          "--vm-machine-state and --vm-entry-frame\n";
    return 1;
  }
  if (VMEntryFrame.getNumOccurrences()) {
    if (!VMMachineState) {
      WithColor::error() << "--vm-entry-frame requires --vm-machine-state\n";
      return 1;
    }
    const auto Parts = StringRef(VMEntryFrame.getValue()).split(':');
    const auto SignedDecimal = [](StringRef Text, int64_t &Value) {
      auto Digits = Text;
      Digits.consume_front("-");
      return !Digits.empty() &&
             Digits.find_first_not_of("0123456789") == StringRef::npos &&
             !Text.getAsInteger(10, Value);
    };
    if (!SignedDecimal(Parts.first, EntryFrameBegin) ||
        !SignedDecimal(Parts.second, EntryFrameEnd)) {
      WithColor::error()
          << "--vm-entry-frame expects signed 64-bit decimal begin:end\n";
      return 1;
    }
    // The engine owns range validity and the nonwrapping entry predicate.
  }
  const char *Source = nullptr;
  if (!ExportFunc.empty()) {
    int FuncIdx = -1;
    uint64_t DirectAddr = 0;
    bool HaveDirectAddr = false;
    StringRef FuncRef(ExportFunc.getValue());
    if (FuncRef.consume_front("0x") || FuncRef.consume_front("0X")) {
      if (!FuncRef.empty() && !FuncRef.getAsInteger(16, DirectAddr)) {
        HaveDirectAddr = true;
        FuncIdx = neverd_func_find_by_addr(Sess, DirectAddr);
      }
    } else {
      FuncIdx = neverd_func_find_by_name(Sess, ExportFunc.c_str());
      if (FuncIdx < 0)
        if (const std::optional<uint64_t> Exported =
                exportedAddress(Sess, FuncRef)) {
          HaveDirectAddr = true;
          DirectAddr = *Exported;
        }
      if (FuncIdx < 0 && !HaveDirectAddr && !FuncRef.empty() &&
          !FuncRef.getAsInteger(16, DirectAddr)) {
        HaveDirectAddr = true;
        FuncIdx = neverd_func_find_by_addr(Sess, DirectAddr);
      }
    }
    if (FuncIdx < 0 && !HaveDirectAddr) {
      WithColor::error() << "function not found: " << ExportFunc.getValue()
                         << "\n";
      return 1;
    }
    const uint64_t Entry =
        FuncIdx >= 0 ? neverd_func_entry(Sess, FuncIdx) : DirectAddr;
    if (DecompileCallees) {
      if (LlvmRoute || Devirtualize || NoOpt || DedicatedLanguage ||
          SourceTransforms || StackFrameInfo || HasStackFrame || JsonOutput) {
        WithColor::error() << "--callees writes the default C of --func and "
                              "its callees; omit other view options\n";
        return 1;
      }
      std::string Text;
      if (const char *Error = decompileWithCallees(Sess, Entry, Text)) {
        WithColor::error() << "decompile failed: " << Error << ": "
                           << takeLastError(Sess) << "\n";
        return 1;
      }
      // The unit names a refused callee and leaves it out. The function asked
      // for fails the request, as --func alone does.
      if (const std::optional<std::string> Refused = refusalNote(Text, Entry)) {
        WithColor::error() << "decompile failed: " << *Refused << "\n";
        return 1;
      }
      if (OutputFile.empty()) {
        outs() << Text;
        return 0;
      }
      std::error_code EC;
      raw_fd_ostream OS(OutputFile, EC);
      if (EC) {
        WithColor::error() << "cannot open: " << EC.message() << "\n";
        return 1;
      }
      OS << Text;
      outs() << "C source of " << ExportFunc.getValue()
             << " and its callees written to " << OutputFile.getValue() << "\n";
      return 0;
    }
    if (StackFrameInfo || HasStackFrame) {
      const char *Frame =
          neverd_stack_frame_json(Sess, Entry, StackPolicyText.c_str());
      if (!Frame) {
        WithColor::error() << "stack frame refused: " << takeLastError(Sess)
                           << '\n';
        return 1;
      }
      if (StackFrameInfo) {
        std::error_code EC;
        std::optional<raw_fd_ostream> File;
        if (!OutputFile.empty())
          File.emplace(OutputFile, EC);
        if (EC) {
          neverd_free_string(Frame);
          WithColor::error() << "cannot open: " << EC.message() << '\n';
          return 1;
        }
        (File ? *File : outs()) << Frame << '\n';
        neverd_free_string(Frame);
        return 0;
      }
      neverd_free_string(Frame);
      if (HasStackVariables) {
        // Both frontends require the policy to preserve storage in both C
        // routes. The SDK checks the emitted bindings, including name clashes.
        const char *Other = neverd_ir_view_ex_json(
            Sess, Entry, LlvmRoute ? "c" : "llvmc", 0, 1,
            ("{\"stack_frame\":" + StackPolicyText + "}").c_str());
        if (!Other) {
          WithColor::error()
              << "stack frame refused: " << takeLastError(Sess) << '\n';
          return 1;
        }
        neverd_free_string(Other);
      }
    }
    if (JsonOutput || SourceTransforms || HasStackFrame ||
        (SourceStage && !Devirtualize)) {
      const char *Stage = SourceStage ? SourceStage : LlvmRoute ? "llvmc" : "c";
      json::Array Pages;
      std::string Text;
      size_t Offset = 0, Bytes = 0;
      for (;;) {
        const char *Raw =
            SourceTransforms || HasStackFrame
                ? neverd_ir_view_ex_json(Sess, Entry, Stage, Offset, 2048,
                                         ViewOptionsText.c_str())
                : neverd_ir_view_json(Sess, Entry, Stage, Offset, 2048);
        if (!Raw) {
          WithColor::error()
              << "source view failed: " << takeLastError(Sess) << "\n";
          return 1;
        }
        const size_t Size = StringRef(Raw).size();
        if (Size >
            ::neverd::limits::cli::kMaxPipelineAggregateSourceBytes - Bytes) {
          neverd_free_string(Raw);
          WithColor::error()
              << "JSON source view exceeds the 64 MiB output budget\n";
          return 1;
        }
        Bytes += Size;
        auto Value = json::parse(Raw);
        neverd_free_string(Raw);
        if (!Value || !Value->getAsObject()) {
          if (!Value)
            consumeError(Value.takeError());
          WithColor::error() << "invalid source view response\n";
          return 1;
        }
        auto *Page = Value->getAsObject();
        if (Page->getString("mapping_status") == "unsupported_architecture") {
          WithColor::error() << "--func spells only native code in Rust or "
                                "Go; use the whole-program output\n";
          return 1;
        }
        // Rust and Go are offered for programs written in them.
        if (Page->getString("mapping_status") == "unsupported_representation") {
          WithColor::error() << Page->getString("reason").value_or(
                                    "this view is not offered for this program")
                             << '\n';
          return 1;
        }
        if (auto PageText = Page->getString("text"))
          Text += *PageText;
        const bool Complete = Page->getBoolean("complete").value_or(false);
        auto Next = Page->getInteger("next_offset");
        if (!Complete &&
            (!Next || *Next < 0 || static_cast<size_t>(*Next) <= Offset)) {
          WithColor::error() << "source view did not advance\n";
          return 1;
        }
        Pages.push_back(std::move(*Value));
        if (Complete)
          break;
        Offset = static_cast<size_t>(*Next);
      }
      json::Value Output(
          json::Object{{"schema_version", 1}, {"pages", std::move(Pages)}});
      std::error_code EC;
      std::optional<raw_fd_ostream> File;
      if (!OutputFile.empty()) {
        File.emplace(OutputFile, EC);
        if (EC) {
          WithColor::error() << "cannot open: " << EC.message() << '\n';
          return 1;
        }
      }
      raw_ostream &OS = File ? *File : outs();
      if (JsonOutput)
        OS << Output << '\n';
      else
        OS << Text;
      return 0;
    }
    if (Devirtualize) {
      std::vector<const char *> Controls;
      for (const auto &Name : VMControlRegisters)
        Controls.push_back(Name.c_str());
      std::vector<neverd_devirtualize_frame_slot_v1> FrameSlots;
      for (const auto &Text : VMControlFrameSlots) {
        const auto Parts = StringRef(Text).split(':');
        int64_t Offset = 0;
        unsigned Bytes = 0;
        if (Parts.first.empty() || Parts.second.empty() ||
            Parts.first.getAsInteger(0, Offset) ||
            Parts.second.getAsInteger(0, Bytes) || !Bytes || Bytes > 8) {
          WithColor::error()
              << "--vm-control-stack expects signed-offset:bytes (1..8)\n";
          return 1;
        }
        FrameSlots.push_back({Offset, static_cast<uint16_t>(Bytes), 0});
      }
      neverd_devirtualize_options_v10 SourceRecovery{};
      SourceRecovery.machine_source_abi = MachineSourceABI;
      auto &ProofRecovery = SourceRecovery.base;
      if (VMProveStoreReturnSeparation)
        ProofRecovery.flags |=
            NEVERD_DEVIRTUALIZE_V9_PROVE_STORE_RETURN_SLOT_SEPARATION;
      if (VMProveStoreFrameSeparation)
        ProofRecovery.flags |=
            NEVERD_DEVIRTUALIZE_V9_PROVE_STORE_FRAME_SEPARATION;
      auto &ChainRecovery = ProofRecovery.base;
      ChainRecovery.max_chained_visits_per_destination = ChainVisits;
      auto &ExtendedRecovery = ChainRecovery.base;
      ExtendedRecovery.max_node_evaluations = MaxEvaluations;
      ExtendedRecovery.max_discovery_visits = MaxDiscoveryVisits;
      if (VMChainStopAtRepeat)
        ExtendedRecovery.flags |= NEVERD_DEVIRTUALIZE_V7_STOP_CHAIN_AT_REPEAT;
      if (VMExternalStoresDisjointFrame)
        ExtendedRecovery.base.flags |=
            NEVERD_DEVIRTUALIZE_V6_EXTERNAL_STORES_DISJOINT_ENTRY_FRAME;
      ExtendedRecovery.base.base.max_symbolic_nodes = MaxSymbolicNodes;
      ExtendedRecovery.base.base.entry_frame_alignment = EntryAlignment;
      ExtendedRecovery.base.base.entry_frame_residue = EntryResidue;
      auto &Recovery = ExtendedRecovery.base.base.base;
      auto &Base = Recovery.base.base.base;
      Base.struct_size = sizeof(SourceRecovery);
      Recovery.base.base.max_control_refinements = MaxRefinements;
      Recovery.base.max_control_fields = MaxFields;
      Recovery.base.max_solver_queries = MaxQueries;
      Recovery.max_chained_transfers = ChainTransfers;
      if (VMNoControlDiscovery)
        Recovery.flags |= NEVERD_DEVIRTUALIZE_V4_DISABLE_CONTROL_DISCOVERY;
      if (VMEntryFrame.getNumOccurrences()) {
        Recovery.flags |= NEVERD_DEVIRTUALIZE_V4_HAS_ENTRY_FRAME_BOUNDS;
        Recovery.entry_frame_begin = EntryFrameBegin;
        Recovery.entry_frame_end = EntryFrameEnd;
      }
      Base.control_registers = Controls.data();
      Base.control_register_count = Controls.size();
      Base.control_frame_slots = FrameSlots.data();
      Base.control_frame_slot_count = FrameSlots.size();
      Base.max_nodes = VMMaxNodes;
      Base.max_contexts_per_address = VMMaxContexts;
      Base.max_operations = VMMaxOperations;
      Base.use_llvm = LlvmRoute;
      Base.no_opt = NoOpt;
      const char *Report = nullptr;
      Source = VMMachineState ? neverd_devirtualize_machine_source_v10(
                                    Sess, Entry, &SourceRecovery, &Report)
                              : neverd_devirtualize_source_v10(
                                    Sess, Entry, &SourceRecovery, &Report);
      if (!VMRecoveryReport.empty() && Report) {
        std::error_code EC;
        raw_fd_ostream OS(VMRecoveryReport, EC);
        if (EC) {
          WithColor::error()
              << "cannot write recovery report: " << EC.message() << "\n";
          neverd_free_string(Report);
          neverd_free_string(Source);
          return 1;
        }
        OS << Report << '\n';
      }
      neverd_free_string(Report);
    } else {
      Source = LlvmRoute ? neverd_decompile_llvm_ex(Sess, Entry, NoOpt)
                         : neverd_decompile(Sess, Entry);
    }
  } else {
    neverd_session_set_source_symbolic(Sess, SourceSymbolic ? 1 : 0);
    Source =
        DedicatedLanguage
            ? neverd_decompile_all_ex(Sess, InPath, Language, NoOpt, MaxFunc)
            : neverd_decompile_all(Sess, InPath, LlvmRoute ? 1 : 0, NoOpt,
                                   MaxFunc);
  }
  if (!Source || Source[0] == '\0') {
    WithColor::error() << "decompile failed: " << takeLastError(Sess) << "\n";
    neverd_free_string(Source);
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
  if (std::optional<std::string> Error = validateSourcePatchArguments()) {
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
  } else if (usesSourcePatchRequest()) {
    Ret = runSourcePatch(Sess, OutPath, Strategy);
    if (PatchReport) {
      const char *Report = neverd_patch_source_report(Sess);
      if (Report && *Report)
        outs() << Report << "\n";
      neverd_free_string(Report);
      if (Ret == 0)
        return 0;
    }
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
