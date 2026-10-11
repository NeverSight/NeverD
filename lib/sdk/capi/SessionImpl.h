//===- SessionImpl.h - Internal session state for C API -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal session data structure shared across C API implementation files.
/// This header is NOT part of the public API.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_SESSION_IMPL_H
#define NEVERD_SDK_CAPI_SESSION_IMPL_H

#include "../plugin/PluginManager.h"
#include "ImageStrings.h"
#include "LoadOptions.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/dialect/SourceDialect.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/debug/DebugInfoDiscovery.h"
#include "neverd/decode/Decoder.h"
#include "neverd/evm/emit/EVMLLVMEmitter.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/SymbolSpelling.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/sbf/emit/SBFLLVMEmitter.h"
#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/sigs/SignatureDB.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace neverd {
namespace sdk {

struct FuncInfo {
  va_t Entry;
  uint64_t Size;
  std::string Name;
  NameOrigin Origin = NameOrigin::Stated;
  /// Original identity is never overwritten by a display annotation or rename.
  std::string LinkageName;
  NameOrigin LinkageOrigin = NameOrigin::Stated;
};

struct Session {
  enum class SanitizePublicationFaultForTesting : uint8_t {
    None,
    PublishIndeterminate,
    PublishedFinalAuthenticationFailure,
    PublishedFinalizationFailure,
  };

  std::filesystem::path FilePath;
  /// Absolute, symlink-free source locator captured at load time for the
  /// authenticated sanitizer path only.  FilePath deliberately preserves the
  /// caller's legacy spelling for every pre-existing C API consumer.
  std::filesystem::path SanitizeSourcePath;
  /// Snapshot-backed native sessions have no host path or sidecar namespace.
  std::optional<uint64_t> MemoryInputBytes;
  std::string WebNativeProvenance;
  BinaryImage Img;
  bool Loaded = false;

  // Debug information for the loaded image.  DbgRequest is the caller's search
  // policy and must be set before neverd_session_load(); the rest describe what
  // that load actually found.
  DebugInfoRequest DbgRequest;
  LoadProgress LoadProgressCb;
  std::unique_ptr<DebugContext> Dbg;
  DebugInfoKind DbgKind = DebugInfoKind::None;
  std::filesystem::path DbgPath;

  std::unique_ptr<llvm::LLVMContext> LLVMCtx;
  PipelineResult PipeResult;
  bool PipeRan = false;
  uint64_t PipelineFeatureGeneration = 0;
  std::optional<bool> LlvmModuleNoOpt;
  /// One function's native LLVM module: its body, with every other function
  /// declared by its own signature.  A page shows one function, so a sibling
  /// the emitter refuses cannot hide it, and showing it emits one body.
  struct FunctionLlvmModule {
    va_t Entry = 0;
    bool NoOpt = false;
    std::unique_ptr<llvm::Module> Module;
    std::shared_ptr<LLVMSourceMap> Sources;
  };
  /// The modules of the functions shown last, the newest first.
  std::list<FunctionLlvmModule> FunctionLlvmModules;
  static constexpr size_t MaxFunctionLlvmModules = 8;
  /// The C route that emitted a function's source, or the language its
  /// HighC source is spelled in.
  enum class SourceRoute : uint8_t {
    HighC,
    PlainC,
    LLVMC,
    LLVMCNoOpt,
    Cpp,
    Rust,
    Go
  };
  /// One function's emitted C.  A view pages through the whole text, and the
  /// text stays the same until the pipeline or an input of the emitter
  /// outside it changes (forgetEmittedSources), so every page after the first
  /// reads it here instead of emitting the function again.
  struct FunctionSource {
    va_t Entry = 0;
    SourceRoute Route = SourceRoute::HighC;
    std::string Text;
    /// The text's library regions and definitions, once a view asked for
    /// them.  Its recognitions live in PipeResult, which clearPipeline()
    /// drops together with this.
    std::optional<CSourceMap> Map;
    /// A spelled source's declarations shown as C, and its source names.
    std::vector<std::string> Unread;
    std::vector<SourceDialectName> Names;
  };
  /// The sources of the functions shown last, the newest first.
  std::list<FunctionSource> FunctionSources;
  static constexpr size_t MaxFunctionSources = 8;
  /// Whether every native function's call ABI was recovered for this
  /// pipeline's LLVM emission.
  bool NativeCallAbiRecovered = false;
  bool SBFFunctionsSynchronized = false;
  bool NativeFunctionsSynchronized = false;
  /// The image's strings under the options last searched with
  /// (ImageStrings.h); kept until the next load.
  std::optional<ImageStringScan> ImageStrings;
  /// Function entries the detector found on request
  /// (neverd_session_discover_functions); kept until the next load.
  std::optional<std::vector<std::pair<va_t, std::string>>> DiscoveredFunctions;
  std::set<va_t> OnlyFunctionEntries;
  std::map<va_t, InstructionMode> ARMFunctionModes;
  evm::Hardfork EVMFork = evm::Hardfork::Latest;
  bool EVMStrict = true;
  sbf::Version SBFVersion = sbf::Version::Auto;
  bool SBFStrict = true;
  sbf::RuntimeProfile SBFProfile;
  std::optional<sbf::AnchorIdl> SBFIdl;

  Decoder Dec;

  std::vector<FuncInfo> Functions;

  PatchResult LastPatch;

  /// Internal JSON-boundary seams used to prove that no C++ exception crosses
  /// extern C. Empty in production.
  std::function<void()> LowIRConcolicBeforeRunForTesting;
  std::function<void()> SafetyBeforeRunForTesting;

  /// Internal fault-injection seams for transaction-integrity tests.  They
  /// are empty in production.  Each callback receives the relevant candidate
  /// temp path and may replace its pathname or contents.
  std::function<void(const std::filesystem::path &)>
      SanitizeAfterBackendForTesting;
  std::function<void(const std::filesystem::path &)>
      SanitizeBeforeReloadForTesting;
  std::function<void(const std::filesystem::path &)>
      SanitizeBeforePublishForTesting;
  std::function<void(const std::filesystem::path &)>
      SanitizeAfterDestinationResolutionForTesting;
  SanitizePublicationFaultForTesting SanitizePublicationFault =
      SanitizePublicationFaultForTesting::None;

  // Instruction-substitution toggle, consulted by every patch
  // entry point.  LastSubstitutionCount records how many operators the most
  // recent patch substituted (0 when the toggle is off).
  bool InstSubstitution = false;
  unsigned InstSubstitutionRounds = 1;
  unsigned LastSubstitutionCount = 0;

  // Constant-encryption toggle, consulted by every patch entry point.
  // LastConstEncCount records how many constant operands the most recent patch
  // encrypted (0 when the toggle is off).
  bool ConstantEncryption = false;
  unsigned LastConstEncCount = 0;

  // Opaque-predicate toggle, consulted by every patch entry point.
  // LastOpaquePredCount records how many predicates the most recent patch
  // inserted (0 when the toggle is off).
  bool OpaquePredicate = false;
  unsigned LastOpaquePredCount = 0;

  // Control-flow flattening toggle, consulted by every patch entry point.
  // LastFlattenCount records how many basic blocks the most recent patch
  // flattened (0 when the toggle is off).
  bool ControlFlowFlattening = false;
  unsigned LastFlattenCount = 0;

  // Bogus-control-flow toggle, consulted by every patch entry point.
  // LastBogusCount records how many basic blocks the most recent patch gave a
  // bogus sub-graph (0 when the toggle is off).
  bool BogusControlFlow = false;
  unsigned LastBogusCount = 0;

  // Indirect-branch toggle, consulted by every patch entry point.
  // LastIndirectBranchCount records how many conditional branches the most
  // recent patch converted to indirect branches (0 when the toggle is off).
  bool IndirectBranch = false;
  unsigned LastIndirectBranchCount = 0;

  // Indirect-call toggle, consulted by every patch entry point.
  // LastIndirectCallCount records how many direct calls the most recent patch
  // converted to indirect calls (0 when the toggle is off).
  bool IndirectCall = false;
  unsigned LastIndirectCallCount = 0;

  // Mixed-boolean-arithmetic toggle, consulted by every patch entry point.
  // LastMBACount records how many operators the most recent patch wrapped with
  // an MBA term (0 when the toggle is off).
  bool MBA = false;
  unsigned LastMBACount = 0;

  // Indirect global-variable toggle, consulted by every patch entry point.
  // LastIndirectGlobalCount records how many global references the most recent
  // patch made indirect (0 when the toggle is off).
  bool IndirectGlobal = false;
  unsigned LastIndirectGlobalCount = 0;

  // Value-laundering toggle, consulted by every patch entry point.
  // LastValueLaunderCount records how many values the most recent patch routed
  // through a volatile stack slot (0 when the toggle is off).
  bool ValueLaunder = false;
  unsigned LastValueLaunderCount = 0;

  // Constant-pooling toggle, consulted by every patch entry point.
  // LastConstPoolCount records how many constant operands the most recent patch
  // moved into a read-only global pool (0 when the toggle is off).
  bool ConstantPooling = false;
  unsigned LastConstPoolCount = 0;

  // Bit-masking toggle, consulted by every patch entry point.
  // LastBitMaskCount records how many values the most recent patch wrapped with
  // the `(x & m) | (x & ~m)` bitwise identity (0 when the toggle is off).
  bool BitMasking = false;
  unsigned LastBitMaskCount = 0;

  // Forced original code-section name (empty = format default).  Lets the user
  // point the patcher at a code section renamed by a packer/protector
  // (e.g. ".vmp0"); consulted by every patch entry point.
  std::string TextSectionOverride;

  struct RoundTripResult {
    std::string IR;
    CodegenResult CG;
    std::vector<int> ParamCounts;
    bool Valid = false;
  } RoundTrip;

  std::map<va_t, std::string> Annotations;

  std::map<va_t, std::string> Renames;
  std::map<va_t, std::string> OriginalNames;

  sigs::SignatureDB SigDB;

  PluginManager PM;

  std::string LastError;

  void setError(const std::string &Msg) { LastError = Msg; }
  void clearError() { LastError.clear(); }
  uint64_t inputFileSize() const {
    if (!Loaded)
      return 0;
    if (MemoryInputBytes)
      return *MemoryInputBytes;
    std::error_code EC;
    const auto Size = std::filesystem::file_size(FilePath, EC);
    return EC ? 0 : Size;
  }
  bool requireFileBacked() {
    if (!MemoryInputBytes)
      return true;
    setError("operation requires a file-backed session");
    return false;
  }

  void applyAnalysisOptions(PipelineOptions &Opts) const {
    if (MemoryInputBytes)
      Opts.EmitDumpOutput = false;
    Opts.LibraryFeatures = &SigDB.featurePacks();
    Opts.EVMFork = EVMFork;
    Opts.EVMStrict = EVMStrict;
    Opts.SBFVersion = SBFVersion;
    Opts.SBFStrict = SBFStrict;
    Opts.SBFProfile = SBFProfile;
    Opts.SBFIdl = SBFIdl ? &*SBFIdl : nullptr;
    Opts.OnlyFunctionEntries = OnlyFunctionEntries;
    Opts.FunctionEdits = FunctionEdits;
  }

  /// Snapshot the enabled cosmetic transforms without touching observable
  /// per-session result state.  Transactional callers may run the returned
  /// policy on a candidate module and commit its counts only after publishing
  /// the corresponding binary.
  Pipeline::ObfuscationConfig obfuscationConfig() const {
    Pipeline::ObfuscationConfig Cfg;
    Cfg.InstSubstitution = InstSubstitution;
    Cfg.InstSubstitutionRounds = InstSubstitutionRounds;
    Cfg.ConstantEncryption = ConstantEncryption;
    Cfg.OpaquePredicate = OpaquePredicate;
    Cfg.BogusControlFlow = BogusControlFlow;
    Cfg.ControlFlowFlattening = ControlFlowFlattening;
    Cfg.IndirectBranch = IndirectBranch;
    Cfg.IndirectCall = IndirectCall;
    Cfg.MBA = MBA;
    Cfg.IndirectGlobal = IndirectGlobal;
    Cfg.ValueLaunder = ValueLaunder;
    Cfg.ConstantPooling = ConstantPooling;
    Cfg.BitMasking = BitMasking;
    return Cfg;
  }

  Pipeline::ObfuscationCounts runSessionObfuscation(llvm::Module &Mod) const {
    return Pipeline::runObfuscationPasses(Mod, obfuscationConfig());
  }

  void commitObfuscationCounts(const Pipeline::ObfuscationCounts &Counts) {
    LastSubstitutionCount = Counts.Substitution;
    LastConstEncCount = Counts.ConstEnc;
    LastOpaquePredCount = Counts.OpaquePred;
    LastBogusCount = Counts.Bogus;
    LastFlattenCount = Counts.Flatten;
    LastIndirectBranchCount = Counts.IndirectBranch;
    LastIndirectCallCount = Counts.IndirectCall;
    LastMBACount = Counts.MBA;
    LastIndirectGlobalCount = Counts.IndirectGlobal;
    LastValueLaunderCount = Counts.ValueLaunder;
    LastConstPoolCount = Counts.ConstPool;
    LastBitMaskCount = Counts.BitMask;
  }

  /// The user's function edits (neverd_func_create, neverd_func_delete): an
  /// address made the entry of a function (true) or no longer one (false).
  std::map<va_t, bool> FunctionEdits;

  /// A data item the user defines (neverd_item_set): what the bytes from its
  /// address are, over what analysis reads there.
  struct DataItem {
    /// A size keyword (`qword`), "string" or "undefined" (DataNames.def).
    std::string Kind;
    uint64_t Size = 0;
    /// A string's encoding, as strings::encodingName spells it.
    std::string Encoding;
    bool operator==(const DataItem &) const = default;
  };
  std::map<va_t, DataItem> DataItems;
  /// How the user shows an instruction operand's number
  /// (neverd_operand_format_set): a base of OperandFormats.def, with its sign
  /// changed or its bits inverted.
  struct OperandFormat {
    std::string Base;
    bool Negate = false, Invert = false;
    bool operator==(const OperandFormat &) const = default;
  };
  /// The user's operand formats by instruction address and operand index.
  std::map<va_t, std::map<unsigned, OperandFormat>> OperandFormats;
  /// The loader the next load reads the input with, as
  /// neverd_session_set_load_options chose it.  Unset, the load reads the
  /// load options sidecar, else the file's own format.
  std::optional<LoaderChoice> RequestedLoad;
  /// The loader the loaded image was read with.
  LoaderChoice LoadedChoice;

  bool isDeletedFunction(va_t Entry) const {
    const auto Edit = FunctionEdits.find(Entry);
    return Edit != FunctionEdits.end() && !Edit->second;
  }

  void resetFunctionsFromImage() {
    Functions.clear();
    OriginalNames.clear();
    if (!Loaded)
      return;
    const auto Symbols = Img.getFunctionSymbols();
    Functions.reserve(Symbols.size());
    for (const Symbol *Symbol : Symbols) {
      std::string Name = Symbol->Name;
      OriginalNames[Symbol->Addr] = Name;
      if (auto Rename = Renames.find(Symbol->Addr); Rename != Renames.end())
        Name = Rename->second;
      const NameOrigin Origin =
          Renames.contains(Symbol->Addr) ? NameOrigin::User : Symbol->Origin;
      if (!isDeletedFunction(Symbol->Addr))
        Functions.push_back({Symbol->Addr, Symbol->Size, std::move(Name),
                             Origin, Symbol->Name, Symbol->Origin});
    }
    appendDiscoveredFunctions();
    appendCreatedFunctions();
    refreshFunctionNames();
  }

  /// Add the functions the user made that the list does not hold yet.
  void appendCreatedFunctions() {
    std::unordered_set<va_t> Known;
    Known.reserve(Functions.size());
    for (const FuncInfo &F : Functions)
      Known.insert(F.Entry);
    bool Added = false;
    for (const auto &[Entry, Created] : FunctionEdits) {
      if (!Created || !Known.insert(Entry).second)
        continue;
      const std::string Name = (kAutoFuncPrefix + llvm::utohexstr(Entry)).str();
      OriginalNames.try_emplace(Entry, Name);
      const auto Rename = Renames.find(Entry);
      const bool Renamed = Rename != Renames.end();
      Functions.push_back({Entry, 0, Renamed ? Rename->second : Name,
                           Renamed ? NameOrigin::User : NameOrigin::Analysis,
                           Name, NameOrigin::Analysis});
      Added = true;
    }
    if (Added)
      std::stable_sort(Functions.begin(), Functions.end(),
                       [](const FuncInfo &Left, const FuncInfo &Right) {
                         return Left.Entry < Right.Entry;
                       });
  }

  /// Add the detector's entries the list does not hold yet, keeping it in
  /// address order.  Returns whether any was added.
  bool appendDiscoveredFunctions() {
    if (!DiscoveredFunctions)
      return false;
    std::unordered_set<va_t> Known;
    Known.reserve(Functions.size());
    for (const FuncInfo &F : Functions)
      Known.insert(F.Entry);
    bool Added = false;
    for (const auto &[Entry, Name] : *DiscoveredFunctions) {
      if (isDeletedFunction(Entry) || !Known.insert(Entry).second)
        continue;
      OriginalNames.try_emplace(Entry, Name);
      const auto Rename = Renames.find(Entry);
      const bool Renamed = Rename != Renames.end();
      Functions.push_back({Entry, 0, Renamed ? Rename->second : Name,
                           Renamed ? NameOrigin::User : NameOrigin::Analysis,
                           Name, NameOrigin::Analysis});
      Added = true;
    }
    if (Added)
      std::stable_sort(Functions.begin(), Functions.end(),
                       [](const FuncInfo &Left, const FuncInfo &Right) {
                         return Left.Entry < Right.Entry;
                       });
    return Added;
  }

  /// Recompute display identity from current evidence. No IR or image names
  /// are modified, and a failed/withdrawn match cannot leave a stale label.
  void refreshFunctionNames();

  /// Drop the kept C of every function.  A change to anything the C emitters
  /// read beyond the pipeline, such as the user's names
  /// (CEmitterOptions::UserNames), must call this, or a view keeps showing
  /// what was emitted before it.
  void forgetEmittedSources() { FunctionSources.clear(); }

  /// The source \p Route emitted for \p Entry under this pipeline, made the
  /// newest; null if it has not emitted one.
  FunctionSource *findFunctionSource(va_t Entry, SourceRoute Route) {
    for (auto It = FunctionSources.begin(); It != FunctionSources.end(); ++It)
      if (It->Entry == Entry && It->Route == Route) {
        FunctionSources.splice(FunctionSources.begin(), FunctionSources, It);
        return &FunctionSources.front();
      }
    return nullptr;
  }

  /// Keep the source \p Route emitted for \p Entry, with its \p Map if the
  /// emission recorded one.
  void rememberFunctionSource(va_t Entry, SourceRoute Route, std::string Text,
                              const CSourceMap *Map) {
    FunctionSource *Source = findFunctionSource(Entry, Route);
    if (!Source) {
      FunctionSources.push_front({Entry, Route, {}, std::nullopt});
      if (FunctionSources.size() > MaxFunctionSources)
        FunctionSources.pop_back();
      Source = &FunctionSources.front();
    }
    Source->Text = std::move(Text);
    if (!Map)
      return;
    Source->Map = *Map;
    // Only an emission reads these; a page reads regions and definitions.
    Source->Map->HighSources = nullptr;
    Source->Map->LLVMSources = nullptr;
  }

  void clearPipeline() {
    // Every module lives in the context.
    FunctionLlvmModules.clear();
    FunctionSources.clear();
    NativeCallAbiRecovered = false;
    PipeResult = {};
    LLVMCtx.reset();
    PipeRan = false;
    LlvmModuleNoOpt.reset();
    SBFFunctionsSynchronized = false;
    NativeFunctionsSynchronized = false;
  }

  void invalidatePipeline() {
    clearPipeline();
    resetFunctionsFromImage();
  }

  bool ensurePipeline() {
    if (!Loaded) {
      setError("no binary loaded");
      return false;
    }
    if (PipeRan && PipelineFeatureGeneration != SigDB.featureGeneration())
      clearPipeline();
    if (!PipeRan) {
      LLVMCtx = std::make_unique<llvm::LLVMContext>();
      PipelineOptions Opts;
      applyAnalysisOptions(Opts);
      Pipeline ThePipeline;
      // Debug names already reached Img.Symbols at load time; handing the
      // context to the pipeline also carries source locations, declared sizes,
      // and parameter names into the IR.
      PipeResult = ThePipeline.run(Img, *LLVMCtx, Opts, Dbg.get());
      PipelineFeatureGeneration = SigDB.featureGeneration();
      PipeRan = true;
    }
    if (!PipeResult.Success)
      setError(PipeResult.Error.empty() ? "pipeline failed" : PipeResult.Error);
    return PipeResult.Success;
  }

  /// Merge loader symbols with recovered functions. Native queries preserve
  /// the cheap symbol-only view until analysis has run; SBF queries retain
  /// their existing eager analysis contract. All public function-oriented APIs
  /// consume this single session view.
  bool synchronizeFunctions();
  /// Presentation-only snapshot. Raw names remain the semantic identity;
  /// metadata queries never force native function analysis.
  llvm::json::Object functionIdentity(va_t Entry) const;

  const LowFunc *findLowFunc(va_t Addr) const {
    for (const auto &F : PipeResult.LowFuncs)
      if (F.Entry == Addr)
        return &F;
    return nullptr;
  }

  const HighFunc *findHighFunc(va_t Addr) const {
    for (const auto &F : PipeResult.HighFuncs)
      if (F.Entry == Addr)
        return &F;
    return nullptr;
  }

  /// Native function identity is its original source address, independently of
  /// debug names or emitter-generated symbol suffixes. Declarations and
  /// untagged helpers do not represent source definitions.
  llvm::Function *findNativeLlvmFunction(va_t Addr) {
    if (!PipeResult.LlvmModule) {
      setError("LLVM module is not available");
      return nullptr;
    }
    return findNativeLlvmFunction(*PipeResult.LlvmModule, Addr);
  }

  llvm::Function *findNativeLlvmFunction(llvm::Module &Module, va_t Addr) {
    llvm::Function *Match = nullptr;
    for (auto &Function : Module) {
      if (Function.isDeclaration())
        continue;
      auto OriginalVA = rewrite_source::getOriginalVA(Function);
      if (!OriginalVA) {
        setError("invalid LLVM source identity for function '" +
                 Function.getName().str() +
                 "': " + llvm::toString(OriginalVA.takeError()));
        return nullptr;
      }
      if (!*OriginalVA || **OriginalVA != Addr)
        continue;
      if (Match) {
        setError("ambiguous LLVM source identity at 0x" +
                 llvm::utohexstr(Addr) + ": '" + Match->getName().str() +
                 "' and '" + Function.getName().str() + "'");
        return nullptr;
      }
      Match = &Function;
    }
    if (!Match)
      setError("LLVM function not found at 0x" + llvm::utohexstr(Addr));
    return Match;
  }

  /// Emit, verify and optimize native LLVM for the pipeline's functions,
  /// with bodies as \p BodyMask selects (MedLLVMEmitter::emit).  Sets the
  /// error and returns null when the emitter refuses or verification fails.
  std::unique_ptr<llvm::Module>
  emitNativeLlvm(bool NoOpt, const std::vector<char> *BodyMask,
                 std::shared_ptr<LLVMSourceMap> &Sources) {
    // HighIR decompile does not run recoverCallAbi. --func --llvm still emits
    // from that MedIR, so populate CallInfos here; otherwise MedLLVM emits
    // 0-arg calls for unwritten live-in rcx.  A body-masked module declares
    // the other functions by the signatures this recovers too; recovery
    // recomputes each function's call information, so once per pipeline is
    // enough for such modules.
    if (!BodyMask || !NativeCallAbiRecovered) {
      std::map<va_t, std::string> FuncNames;
      for (const auto &MF : PipeResult.MedFuncs)
        if (!MF.Name.empty())
          FuncNames[MF.Entry] = MF.Name;
      for (auto &MF : PipeResult.MedFuncs)
        recoverCallAbi(MF, Img.Arch, FuncNames, &Img);
      NativeCallAbiRecovered = true;
    }
    std::vector<std::pair<va_t, std::string>> ImportMap;
    for (const auto &[Addr, Name] : Img.getImportAddressNames())
      ImportMap.emplace_back(Addr, Name);
    MedLLVMEmitter Emitter;
    // Native source views need surviving instruction observations even when
    // this function contains no recognized library operation.
    Sources = std::make_shared<LLVMSourceMap>();
    Emitter.setSourceMap(Sources.get());
    auto Candidate = Emitter.emit(
        PipeResult.MedFuncs, *LLVMCtx, "neverd_output", Img.Arch, ImportMap,
        &Img, Img.abiFormat(), /*MergeableGlobals=*/false, BodyMask);
    if (Sources)
      Sources->preserveExpressionOrigins(PipeResult.LibraryRecognitions);
    if (!Candidate) {
      setError("native LLVM emission failed");
      return nullptr;
    }
    // The native emitter can return a module after reporting verifier errors.
    // Reject optional LLVM output without invalidating completed analysis.
    std::string VerifyError;
    llvm::raw_string_ostream VerifyStream(VerifyError);
    if (llvm::verifyModule(*Candidate, &VerifyStream)) {
      setError("native LLVM verification failed: " + VerifyError);
      return nullptr;
    }
    if (NoOpt) {
      Pipeline::promoteScaffoldingAllocas(*Candidate);
      if (Sources)
        Sources->refreshExpressionOrigins();
    } else {
      const OptimizationResult Optimization =
          Pipeline::optimizeOrPromoteModule(*Candidate, Sources.get());
      if (Optimization.Stop == OptimizationStopReason::VerificationFailed) {
        setError(std::string("native LLVM optimization failed: ") +
                 optimizationStopReasonName(Optimization.Stop));
        return nullptr;
      }
    }
    std::string FinalVerifyError;
    llvm::raw_string_ostream FinalVerifyStream(FinalVerifyError);
    if (llvm::verifyModule(*Candidate, &FinalVerifyStream)) {
      setError("native LLVM post-optimization verification failed: " +
               FinalVerifyError);
      return nullptr;
    }
    return Candidate;
  }

  /// The module that shows the function at \p Entry: its body alone, every
  /// other function declared.  Kept for the functions shown last.
  const FunctionLlvmModule *ensureFunctionLlvmModule(va_t Entry, bool NoOpt) {
    if (PipeRan && PipelineFeatureGeneration != SigDB.featureGeneration())
      clearPipeline();
    for (auto It = FunctionLlvmModules.begin(); It != FunctionLlvmModules.end();
         ++It)
      if (It->Entry == Entry && It->NoOpt == NoOpt) {
        FunctionLlvmModules.splice(FunctionLlvmModules.begin(),
                                   FunctionLlvmModules, It);
        return &FunctionLlvmModules.front();
      }
    if (!ensurePipeline())
      return nullptr;
    std::vector<char> Mask(PipeResult.MedFuncs.size(), 0);
    bool Found = false;
    for (size_t I = 0; I < Mask.size(); ++I)
      if (PipeResult.MedFuncs[I].Entry == Entry) {
        Mask[I] = 1;
        Found = true;
      }
    if (!Found) {
      setError("LLVM function not found at 0x" + llvm::utohexstr(Entry));
      return nullptr;
    }
    FunctionLlvmModule Built;
    Built.Entry = Entry;
    Built.NoOpt = NoOpt;
    Built.Module = emitNativeLlvm(NoOpt, &Mask, Built.Sources);
    if (!Built.Module)
      return nullptr;
    FunctionLlvmModules.push_front(std::move(Built));
    if (FunctionLlvmModules.size() > MaxFunctionLlvmModules)
      FunctionLlvmModules.pop_back();
    return &FunctionLlvmModules.front();
  }

  bool ensureLlvmModule(bool NoOpt = false) {
    if (PipeRan && PipelineFeatureGeneration != SigDB.featureGeneration())
      clearPipeline();
    if (PipeResult.LlvmModule &&
        (!LlvmModuleNoOpt || *LlvmModuleNoOpt == NoOpt))
      return true;
    if (!ensurePipeline())
      return false;
    if (PipeResult.EVM) {
      auto Module = evm::emitLLVM(*PipeResult.EVM, *LLVMCtx);
      if (!Module) {
        setError(llvm::toString(Module.takeError()));
        return false;
      }
      PipeResult.LlvmModule = std::move(*Module);
      return true;
    }
    if (PipeResult.SBF) {
      auto Module = sbf::emitLLVM(*PipeResult.SBF, *LLVMCtx);
      if (!Module) {
        setError(llvm::toString(Module.takeError()));
        return false;
      }
      PipeResult.LlvmModule = std::move(*Module);
      return true;
    }
    if (PipeResult.MedFuncs.empty()) {
      setError("no native functions available for LLVM emission");
      return false;
    }
    std::shared_ptr<LLVMSourceMap> Sources;
    auto Candidate = emitNativeLlvm(NoOpt, nullptr, Sources);
    if (!Candidate)
      return false;
    PipeResult.LlvmModule = std::move(Candidate);
    PipeResult.LLVMSources = std::move(Sources);
    LlvmModuleNoOpt = NoOpt;
    return true;
  }
};

inline Session *toSession(neverd_session_t Sess) {
  return static_cast<Session *>(Sess);
}

inline char *dupStr(const std::string &S) { return strdup(S.c_str()); }

/// How a name reads in identities and listings: as its source language spells
/// it when it is a mangled name (SymbolSpelling.h), else as it is.
inline std::string demangledName(llvm::StringRef Name) {
  return displaySymbolName(Name);
}

inline std::string vaHex(va_t Addr) { return "0x" + llvm::utohexstr(Addr); }

inline std::string jsonToString(const llvm::json::Value &V) {
  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  OS << V;
  return Buf;
}

/// PipelineRunner — self-contained binary-load-and-run for high-level
/// C API functions that take an input path instead of a session.
struct PipelineRunner {
  BinaryImage Img;
  DebugInfoRequest DbgRequest;
  std::unique_ptr<DebugContext> Dbg;
  llvm::LLVMContext LLVMCtx;
  PipelineResult Result;

  bool load(const char *InputPath, std::string &Err, const Session *Policy);
  bool run(PipelineOptions Opts, std::string &Err);
};

} // namespace sdk
} // namespace neverd

#endif // NEVERD_SDK_CAPI_SESSION_IMPL_H
