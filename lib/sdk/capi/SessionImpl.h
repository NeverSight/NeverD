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

#include "../../pipeline/PipelineCallAbiDetail.h"
#include "../../pipeline/PipelineFunctionNames.h"
#include "../../pipeline/PipelineSourceStackAccesses.h"
#include "../plugin/PluginManager.h"
#include "FunctionAuditReason.h"
#include "ImageStrings.h"
#include "InstructionFlow.h"
#include "LoadOptions.h"
#include "LocalTypesDebugContext.h"
#include "NativeImmediateIndex.h"
#include "ProjectInput.h"

#include "neverd/DataAccessWidths.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
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
#include "neverd/loader/FunctionNames.h"
#include "neverd/loader/SymbolSpelling.h"
#include "neverd/pass/Limits.h"
#include "neverd/patch/PatchOverlay.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/sbf/emit/SBFLLVMEmitter.h"
#include "neverd/sdk/Limits.h"
#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/sigs/SignatureDB.h"
#include "neverd/types/HeaderLibrary.h"
#include "neverd/types/PDBTypeImport.h"
#include "neverd/types/TypeParse.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace neverd {
namespace sdk {
void preserveAnalystStackObjects(llvm::Module &);

struct LLVMFunctionView;
struct CallReferenceGraph;

struct FuncInfo {
  va_t Entry;
  uint64_t Size;
  std::string Name;
  NameOrigin Origin = NameOrigin::Stated;
  /// Original identity is never overwritten by a display annotation or rename.
  std::string LinkageName;
  NameOrigin LinkageOrigin = NameOrigin::Stated;
  /// Origin of OriginalNames after recognition, before an analyst's rename.
  NameOrigin ResolvedOrigin = NameOrigin::Synthesized;

  bool operator==(const FuncInfo &) const = default;
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
  /// Declared before the image/debug owners, so its private extracted files
  /// outlive all readers. Empty for ordinary file or memory sessions.
  std::shared_ptr<ProjectInput> PackedProject;
  /// Snapshot-backed native sessions have no host path or sidecar namespace.
  std::optional<uint64_t> MemoryInputBytes;
  std::string WebNativeProvenance;
  BinaryImage Img;
  patch::PatchOverlay BytePatches;
  std::optional<std::array<uint8_t, 32>> BytePatchesSavedDigest;
  std::optional<patch::InputFile> PatchInput;
  bool Loaded = false;

  // Debug information for the loaded image.  DbgRequest is the caller's search
  // policy and must be set before neverd_session_load(); the rest describe what
  // that load actually found.
  DebugInfoRequest DbgRequest;
  DebugInfoRequest LoadedDbgRequest;
  LoadProgress LoadProgressCb;
  std::unique_ptr<DebugContext> Dbg;
  DebugInfoKind DbgKind = DebugInfoKind::None;
  std::filesystem::path DbgPath;
  /// Every file the debug information was read from, Dbg's first.
  std::vector<DebugInfoFile> DbgFiles;
  /// Whether neverd_session_load() read the image from FilePath, so a debug
  /// file loaded later can read it again the same way
  /// (reloadDebugInformation).
  bool ReadFromFile = false;
  bool LoadedStaticSymbols = true;
  /// The program's local types, read from its debug information when first
  /// asked for (NeverDCAPILocalTypes.cpp).  A new image or PDB drops them.
  std::unique_ptr<types::TypeLibrary> LocalTypes;
  types::PDBTypeImportStats LocalTypeStats;
  /// The types DWARF describes that the import left out, each with why, and
  /// the names its units define otherwise (DWARFTypeImportStats).
  std::vector<std::string> LocalTypeSkipped, LocalTypeConflicts;
  /// Where they came from: "pdb", "dwarf", or "none" for an image without
  /// types.
  std::string LocalTypesSource;
  /// The declarations the user added, in order, read again over the
  /// program's types whenever those are read anew.
  /// A declaration adds the types its source declares; one with Forget
  /// removes the types it names instead.
  struct TypeDeclaration {
    types::Language Lang = types::Language::C;
    std::string Source;
    std::vector<std::string> Forget;
  };
  std::vector<TypeDeclaration> TypeDeclarations;
  /// What the declarations replayed over the program's types last met:
  /// {declaration index, problem}.
  std::vector<std::pair<size_t, types::SourceProblem>> DeclarationProblems;
  /// The header libraries joined to the program's types, and what each
  /// added; a library that did not read has its problem instead.
  struct JoinedHeaders {
    types::HeaderLibrary Library;
    types::HeaderMerge Merge;
    std::string Error;
  };
  std::vector<JoinedHeaders> LocalTypeHeaders;
  /// The user's choice of header libraries beyond the image's own
  /// (libraries.json): those joined besides, in order, and those of the
  /// image's left out.
  struct LibraryChoice {
    std::vector<types::HeaderLibrary> Added;
    std::vector<types::HeaderLibrary> Removed;
  };
  LibraryChoice TypeLibraryChoice;
  /// The type the joined libraries give the image's entry point, a pointer
  /// to a function, where the image's kind says which
  /// (NEVERD_HEADER_PE_ENTRY_TYPE): empty for none.
  std::string LocalTypeEntryType;
  /// Platform entry contract when no shipped typedef names it.
  std::string LocalTypeEntryDeclaration;
  /// Platform entry text without importing or parsing a type library.
  std::string platformEntryDeclaration() const;
  /// Drops the program's types, read anew when next asked for; the user's
  /// declarations stay, to be read again over them.
  void dropLocalTypes() {
    LocalTypes.reset();
    LocalTypeStats = {};
    LocalTypeSkipped.clear();
    LocalTypeConflicts.clear();
    LocalTypesSource.clear();
    DeclarationProblems.clear();
    LocalTypeHeaders.clear();
    LocalTypeEntryType.clear();
    LocalTypeEntryDeclaration.clear();
    dropLocalTypesDebug();
  }

  /// The debug context the decompiler reads: the program's own completed
  /// from its local types (LocalTypesDebugContext), when it has any, and
  /// the declarations the user gave its functions, with or without debug
  /// information.  Built at the first decompilation; the pipeline it fed
  /// goes with it.
  std::unique_ptr<LocalTypesDebugContext> LocalTypesDbg;
  bool LocalTypesChecked = false;
  DebugContext *decompileDebug();
  /// The debug context of an image without debug information, which the
  /// user's declarations complete.
  NullDebugContext NoDebugInfo;
  /// The C declarations the user gave functions, by entry
  /// (FunctionAttributes::Type).
  std::map<va_t, std::string> functionDeclarations() const {
    std::map<va_t, std::string> Declarations;
    for (const auto &[Entry, Attributes] : FunctionAttributeEdits)
      if (Attributes.Type)
        Declarations.emplace(Entry, *Attributes.Type);
    return Declarations;
  }
  /// Whether \p Text declares a function the decompiler can bind, as
  /// FunctionAttributes::Type: it reads against the local types as one
  /// function's declaration, is not variadic, every type it names has a
  /// decompiler type, and its calling convention is one NeverD places the
  /// image's parameters by.  False, with \p Error, otherwise.
  bool checkFunctionDeclaration(const std::string &Text, std::string &Error);
  /// After the user's declarations change: the decompiler reads them anew,
  /// and only what it made of them goes.  Analysis never reads them.
  void declarationsChanged() {
    dropLocalTypesDebug();
    clearPipelineResult();
  }
  /// The program's local types, read when first asked for; null with the
  /// session's error when its PDB's types cannot be read.
  types::TypeLibrary *readLocalTypes();
  /// After a declaration changes \p Changed in \p Library, or adds
  /// prototypes, drops what read them: the analysis, where prototypes read
  /// them, or the text that printed them.  Whether the decompilation
  /// changes.
  bool typesChanged(const types::TypeLibrary &Library,
                    llvm::ArrayRef<std::string> Changed, bool Prototypes);
  void dropLocalTypesDebug() {
    // The text it printed stays, and with it what that read.
    if (LocalTypesDbg)
      for (const auto &Name : LocalTypesDbg->readTypes())
        ShownTypes.insert(Name.getKey());
    LocalTypesDbg.reset();
    LocalTypesChecked = false;
    // The counts of prototyped imports read the declarations it gave.
    AnalysisCache.forgetDeclarations();
  }
  /// The library's types the text kept in FunctionSources read through
  /// contexts since dropped: a change to one changes that text.
  llvm::StringSet<> ShownTypes;

  std::unique_ptr<llvm::LLVMContext> LLVMCtx;
  PipelineResult PipeResult;
  PipelineAnalysisCache AnalysisCache;
  bool PipeRan = false;
  uint64_t PipelineFeatureGeneration = 0;
  /// Whether handler discovery can add nothing more to Img for any function
  /// (coff_loader::exceptionHandlersSettled): preparing a function then
  /// skips it.  Only a load changes that, and a load clears it.
  bool ExceptionHandlersSettled = false;
  std::optional<bool> LlvmModuleNoOpt;
  /// Scoped by an extended source-page request; never a project/analysis
  /// option.
  SourceViewOptions SourceOptions;
  struct HighIRVariant {
    std::vector<HighFunc> HighFuncs;
    HighSourceMap HighSources;
  };
  std::unique_ptr<HighIRVariant> EnhancedHighIR;
  struct CallIRVariant {
    va_t Entry = 0;
    std::string Snapshot;
    bool Symbolic = false;
    std::vector<SourceCallEdit> Edits;
    SourceStackFrame Frame;
    std::vector<SourceCallEditResult> Results;
    std::unique_ptr<PipelineResult> Result;
    std::optional<std::vector<MedFunc>> Native;
    MedLLVMEmitter::SourceTargetSignatures TargetSignatures;
    std::map<std::string, SourceCallTypeHint> Hints;
  };
  std::unique_ptr<CallIRVariant> CallVariant;
  std::vector<SourceCallEditResult> CurrentCallResults;
  bool UsingAnalystCallContracts = false;
  /// One function's native LLVM module: its body, with every other function
  /// declared by its own signature.  A page shows one function, so a sibling
  /// the emitter refuses cannot hide it, and showing it emits one body.
  struct FunctionLlvmModule {
    va_t Entry = 0;
    bool NoOpt = false;
    bool OriginalImageStorage = false;
    SourceViewOptions Options;
    std::unique_ptr<llvm::Module> Module;
    std::shared_ptr<LLVMSourceMap> Sources;
    /// The LLVM IR view of an inspection module's function, made when first
    /// shown (functionLlvmView).  Its comments read the user's names.
    mutable std::shared_ptr<const LLVMFunctionView> View;
  };
  /// The modules of the functions shown last, the newest first.
  std::list<FunctionLlvmModule> FunctionLlvmModules;
  static constexpr size_t MaxFunctionLlvmModules = kMaxFunctionLlvmModules;
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
    SourceViewOptions Options;
  };
  /// The sources of the functions shown last, the newest first.
  std::list<FunctionSource> FunctionSources;
  static constexpr size_t MaxFunctionSources = kMaxFunctionSources;
  /// The widths of the reads and writes the code of every function states
  /// at each address (dataAccessWidths), and the exact function extents they
  /// were read from. Navigation can keep these image facts, but no output IR.
  std::optional<DataAccessWidths> AccessWidths;
  std::vector<std::pair<va_t, uint64_t>> AccessWidthsFunctions;
  /// What each function read for AccessWidths, which goes with them.
  DataAccessScan AccessScan;
  /// Native functions prepared for LLVM call-site lowering. This must not
  /// mutate the HighIR route's MedIR: later source variants rebuild from that
  /// same prepared input.
  std::optional<std::vector<MedFunc>> NativeMedFuncs;
  std::vector<MedFunc> NativeCalleeDeclarations;
  bool SBFFunctionsSynchronized = false;
  bool NativeFunctionsSynchronized = false;
  /// The image's strings under the options last searched with
  /// (ImageStrings.h); kept until the next load.
  std::optional<ImageStringScan> ImageStrings;
  /// Function entries the detector found on request
  /// (neverd_session_discover_functions); kept until the next load.
  std::optional<std::vector<std::pair<va_t, std::string>>> DiscoveredFunctions;
  struct FunctionDiscoveryResult {
    std::vector<std::pair<va_t, std::string>> Functions;
    std::set<va_t> VerifiedEntries, CodeReferences;
    bool Cancelled = false;
  };
  struct FunctionDiscoveryJob {
    uint64_t Revision;
    std::future<FunctionDiscoveryResult> Result;
    std::shared_ptr<std::atomic<bool>> Cancel =
        std::make_shared<std::atomic<bool>>(false);
  };
  /// The job owns its image; it never borrows Session state. Stale jobs stay
  /// here until ready, so an edit neither waits nor accumulates retired jobs.
  std::optional<FunctionDiscoveryJob> FunctionDiscovery;
  ~Session() {
    if (FunctionDiscovery)
      FunctionDiscovery->Cancel->store(true, std::memory_order_relaxed);
  }
  /// Drop a finished job of an earlier function list, its result and failure
  /// alike, whatever the current image is. A running one stays: an async
  /// future joins its task on destruction.
  void retireStaleDiscovery() {
    if (!FunctionDiscovery ||
        FunctionDiscovery->Revision == FunctionListRevision ||
        FunctionDiscovery->Result.wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready)
      return;
    auto Job = std::move(*FunctionDiscovery);
    FunctionDiscovery.reset();
    try {
      (void)Job.Result.get();
    } catch (...) {
    }
  }
  std::set<va_t> OnlyFunctionEntries;
  /// Whole-program source simplifies symbolically
  /// (neverd_session_set_source_symbolic).
  bool SourceSymbolic = false;
  std::map<va_t, InstructionMode> ARMFunctionModes;
  evm::Hardfork EVMFork = evm::Hardfork::Latest;
  bool EVMStrict = true;
  sbf::Version SBFVersion = sbf::Version::Auto;
  bool SBFStrict = true;
  sbf::RuntimeProfile SBFProfile;
  std::optional<sbf::AnchorIdl> SBFIdl;

  Decoder Dec;

  std::vector<FuncInfo> Functions;
  /// Caller-declared analysis sections. Bytes live in Img, never FilePath.
  llvm::json::Array AnalysisSections;
  /// What resetFunctionsFromImage last built Functions from, while nothing
  /// but a name refresh has changed the list since (functionListCurrent).
  /// The image's own tables change only on loads, which build the list
  /// again, or after clearPipeline(), which drops this.
  struct FunctionListInputs {
    const Symbol *Symbols;
    size_t SymbolCount;
    const Export *Exports;
    size_t ExportCount;
    bool Discovered;
    std::map<va_t, std::string> Renames;
    std::map<va_t, bool> FunctionEdits;
  };
  std::optional<FunctionListInputs> ListInputs;
  /// The extents complete lifts gave listed functions that had none, by
  /// entry (synchronizeFunctions).  A rebuild of the list from the image,
  /// which states none for them, gives them again; the analysis that found
  /// them going (clearPipeline) takes them with it.
  std::map<va_t, uint64_t> RecoveredSizes;
  /// Advances with every change to Functions or to their names, so that what
  /// was read from the list knows whether the list is still the one it read.
  /// Code that changes the list calls functionListChanged().
  uint64_t FunctionListRevision = 0;
  void functionListChanged() {
    ++FunctionListRevision;
    if (FunctionDiscovery)
      FunctionDiscovery->Cancel->store(true, std::memory_order_relaxed);
  }
  /// Address order belongs to the complete function-list revision, not to
  /// one page. Reference scans and warmup can request thousands of pages.
  struct FunctionPageIndex {
    uint64_t Revision;
    const FuncInfo *List;
    size_t Count;
    std::vector<const FuncInfo *> Ordered;
  };
  mutable std::optional<FunctionPageIndex> FunctionPages;
  /// What refreshFunctionNames last derived the names from.  From the same
  /// inputs it derives the names the list holds, so it keeps them: an image
  /// lists tens of thousands of functions.
  struct FunctionNameInputs {
    uint64_t Revision;
    const FuncInfo *List;
    size_t Count;
    std::map<va_t, std::string> Renames;
    std::unordered_map<uint64_t, std::string> Signatures;
  };
  std::optional<FunctionNameInputs> NamesDerivedFrom;
  /// The program's main (findProgramMain), found once for the image: the
  /// list names it main where nothing else names it, and the decompiler
  /// types it as the C runtime calls it.
  std::optional<va_t> programMain();
  std::optional<std::optional<va_t>> ProgramMainFound;
  /// Where functionIndexNamed() finds a name, read from the list as it was
  /// at Revision.
  struct FunctionNameIndex {
    uint64_t Revision;
    const FuncInfo *List;
    size_t Count;
    /// The first function listed under each name.
    std::unordered_map<std::string, size_t> FirstNamed;
    /// The first function listed at each entry.
    std::unordered_map<va_t, size_t> FirstAt;
    /// The addresses the image exports under each name, in export order.
    std::unordered_map<std::string, std::vector<va_t>> Exported;
  };
  std::optional<FunctionNameIndex> NameIndex;
  /// The function graph the direct references of the listed functions make
  /// (NeverDCAPICallPaths.cpp), read from the list at a revision.
  std::shared_ptr<const CallReferenceGraph> CallGraph;
  /// The first function listed under \p Name, else the first listed at an
  /// address the image exports under it, taking the exports in order: a
  /// linker that folds identical functions exports several names for one.
  /// On Mach-O only, a C spelling also finds its ABI-decorated symbol after
  /// both exact-name lookups fail.
  std::optional<size_t> functionIndexNamed(llvm::StringRef Name);
  /// Emits the session's C (cEmitter()), keeping the image's function names
  /// from one emission to the next.  Those name the symbol table Symbols
  /// held when it read them; clearPipeline() drops them.
  HighCEmitter CEmitter;
  const Symbol *CEmitterSymbols = nullptr;
  size_t CEmitterSymbolCount = 0;

  PatchResult LastPatch;
  std::string LastSourcePatchReport;

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
  unsigned InstSubstitutionRounds =
      ::neverd::limits::pass::kDefaultInstructionSubstitutionRounds;
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
  /// Selection-specific sidecar namespace, without changing the source locator.
  std::filesystem::path projectInputPath() const;
  std::string projectNamespaceSuffix() const;

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

  /// What the user states of a function beyond whether it is one
  /// (neverd_func_edit), as IDA's Edit function does.  An unset member is
  /// analysis's to decide.
  struct FunctionAttributes {
    /// Whether the function never returns, which ends every call to it.
    std::optional<bool> NoReturn;
    /// The user's notes, which analysis does not read: a library function,
    /// a static one, and the 0xRRGGBB color it shows in.
    std::optional<bool> Library;
    std::optional<bool> Static;
    std::optional<uint32_t> Color;
    /// The C declaration the user gave the function, which the decompiler
    /// prints and binds its parameters by (FunctionAttributes.def).
    std::optional<std::string> Type;
    bool empty() const {
      return !NoReturn && !Library && !Static && !Color && !Type;
    }
    bool operator==(const FunctionAttributes &) const = default;
  };
  std::map<va_t, FunctionAttributes> FunctionAttributeEdits;

  /// Give the image the user's statements of which functions return, which
  /// analysis reads; a change needs the pipeline cleared.
  void applyUserNoReturn() {
    Img.UserNoReturn.clear();
    for (const auto &[Entry, Attributes] : FunctionAttributeEdits)
      if (Attributes.NoReturn)
        Img.UserNoReturn[Entry] = *Attributes.NoReturn;
  }

  /// The pipeline's MedIR for the function at \p Addr, or null.
  const MedFunc *findMedFunc(va_t Addr) const {
    for (const MedFunc &F : PipeResult.MedFuncs)
      if (F.Entry == Addr)
        return &F;
    return nullptr;
  }

  /// A data item the user defines (neverd_item_set): what the bytes from its
  /// address are, over what analysis reads there.
  struct DataItem {
    /// A size keyword (`qword`), "string" or "undefined" (DataNames.def).
    std::string Kind;
    uint64_t Size = 0;
    /// A string's encoding, as strings::encodingName spells it.
    std::string Encoding;
    uint64_t ElementSize = 0, Count = 0, Alignment = 0;
    std::string Type;
    bool operator==(const DataItem &) const = default;
  };
  std::map<va_t, DataItem> DataItems;
  NativeImmediateIndex ImmediateIndex;
  /// How the user shows an instruction operand's number
  /// (neverd_operand_format_set): a base of OperandFormats.def, with its sign
  /// changed or its bits inverted, or for a structure offset the local type
  /// and how many bytes into it the register points.
  struct OperandFormat {
    std::string Base;
    bool Negate = false, Invert = false;
    std::string Type;
    int64_t Delta = 0;
    bool operator==(const OperandFormat &) const = default;
  };
  /// The user's operand formats by instruction address and operand index.
  std::map<va_t, std::map<unsigned, OperandFormat>> OperandFormats;
  /// The loader the next load reads the input with, as
  /// neverd_session_set_load_options chose it.  Unset, the load reads the
  /// load options sidecar, else the file's own format.
  std::optional<LoaderChoice> RequestedLoad;
  /// Whether the next load reads the image's static symbol table, as
  /// neverd_session_set_symbol_table chose (BinaryImage::ReadsStaticSymbols).
  bool ReadsStaticSymbols = true;
  /// The loader the loaded image was read with.
  LoaderChoice LoadedChoice;

  bool isDeletedFunction(va_t Entry) const {
    const auto Edit = FunctionEdits.find(Entry);
    return Edit != FunctionEdits.end() && !Edit->second;
  }

  void resetFunctionsFromImage() {
    functionListChanged();
    ListInputs.reset();
    Functions.clear();
    OriginalNames.clear();
    AccessWidths.reset();
    AccessWidthsFunctions.clear();
    AccessScan.clear();
    if (!Loaded)
      return;
    const auto Symbols = Img.getFunctionSymbols();
    Functions.reserve(Symbols.size());
    // Loader boundary guesses are provisional. A completed detector run can
    // refute one, but cannot withdraw a stated symbol or a user's identity.
    // Keep this gate in rebuilding the list so lazy analysis cannot resurrect
    // a rejected guess after discovery has reconciled the catalog.
    std::unordered_set<va_t> Confirmed;
    if (DiscoveredFunctions) {
      for (const auto &[Entry, Name] : *DiscoveredFunctions)
        Confirmed.insert(Entry);
      for (const Symbol *Symbol : Symbols)
        if (!Symbol->IsBoundaryGuess)
          Confirmed.insert(Symbol->Addr);
    }
    // The export each address names first, as findExportAt finds it, for
    // every symbol at once: an image can hold thousands of both.
    std::unordered_map<va_t, const Export *> ExportAt;
    ExportAt.reserve(Img.Exports.size());
    for (const Export &Exp : Img.Exports)
      ExportAt.try_emplace(Exp.Addr, &Exp);
    for (const Symbol *Symbol : Symbols) {
      if (DiscoveredFunctions && Symbol->IsBoundaryGuess &&
          !Confirmed.count(Symbol->Addr) && !Renames.count(Symbol->Addr) &&
          !FunctionEdits.count(Symbol->Addr))
        continue;
      // A name that states nothing gives way to the export there, as the
      // analysis names the function (getFunctionNameAt): a PE names its
      // functions by exports, and one entry several times where its linker
      // folded identical functions (ObDereferenceObject, ObfDereferenceObject).
      // A name functions at lower addresses take is numbered, as the analysis
      // numbers it (`helper_0`).
      std::string Linkage = Img.functionListName(Symbol->Addr, Symbol->Name);
      NameOrigin LinkageOrigin = Symbol->Origin;
      if (LinkageOrigin < NameOrigin::Stated)
        if (const auto Found = ExportAt.find(Symbol->Addr);
            Found != ExportAt.end() && !Found->second->Name.empty()) {
          Linkage = Found->second->Name;
          LinkageOrigin = NameOrigin::Stated;
        }
      std::string Name = Linkage;
      OriginalNames[Symbol->Addr] = Name;
      if (auto Rename = Renames.find(Symbol->Addr); Rename != Renames.end())
        Name = Rename->second;
      const NameOrigin Origin =
          Renames.contains(Symbol->Addr) ? NameOrigin::User : LinkageOrigin;
      if (!isDeletedFunction(Symbol->Addr))
        Functions.push_back({Symbol->Addr, Symbol->Size, std::move(Name),
                             Origin, std::move(Linkage), LinkageOrigin});
    }
    appendDiscoveredFunctions();
    appendCreatedFunctions();
    for (FuncInfo &F : Functions)
      if (F.Size == 0)
        if (const auto Recovered = RecoveredSizes.find(F.Entry);
            Recovered != RecoveredSizes.end())
          F.Size = Recovered->second;
    refreshFunctionNames();
    ListInputs = FunctionListInputs{Img.Symbols.data(),
                                    Img.Symbols.size(),
                                    Img.Exports.data(),
                                    Img.Exports.size(),
                                    DiscoveredFunctions.has_value(),
                                    Renames,
                                    FunctionEdits};
  }

  /// Whether resetFunctionsFromImage would build the function list as it
  /// is: only name refreshes have changed it since that built it, and the
  /// inputs it read are the ones it read then.
  bool functionListCurrent() const {
    return Loaded && ListInputs && ListInputs->Symbols == Img.Symbols.data() &&
           ListInputs->SymbolCount == Img.Symbols.size() &&
           ListInputs->Exports == Img.Exports.data() &&
           ListInputs->ExportCount == Img.Exports.size() &&
           ListInputs->Discovered == DiscoveredFunctions.has_value() &&
           ListInputs->Renames == Renames &&
           ListInputs->FunctionEdits == FunctionEdits;
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
                           Renamed ? NameOrigin::User : NameOrigin::Synthesized,
                           Name, NameOrigin::Synthesized});
      Added = true;
    }
    if (Added) {
      functionListChanged();
      std::stable_sort(Functions.begin(), Functions.end(),
                       [](const FuncInfo &Left, const FuncInfo &Right) {
                         return Left.Entry < Right.Entry;
                       });
    }
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
    std::map<va_t, llvm::StringRef> NewNames;
    for (const auto &[Entry, Name] : *DiscoveredFunctions)
      if (!isDeletedFunction(Entry) && !Known.contains(Entry))
        NewNames.try_emplace(Entry, Name);
    const auto Origins = recoveredFunctionNameOrigins(Img, NewNames);
    bool Added = false;
    for (const auto &[Entry, Name] : *DiscoveredFunctions) {
      if (isDeletedFunction(Entry) || !Known.insert(Entry).second)
        continue;
      OriginalNames.try_emplace(Entry, Name);
      const auto Rename = Renames.find(Entry);
      const bool Renamed = Rename != Renames.end();
      const auto Origin = Origins.at(Entry);
      Functions.push_back({Entry, 0, Renamed ? Rename->second : Name,
                           Renamed ? NameOrigin::User : Origin, Name, Origin});
      Added = true;
    }
    if (Added) {
      functionListChanged();
      std::stable_sort(Functions.begin(), Functions.end(),
                       [](const FuncInfo &Left, const FuncInfo &Right) {
                         return Left.Entry < Right.Entry;
                       });
    }
    return Added;
  }

  /// Recompute display identity from current evidence. No IR or image names
  /// are modified, and a failed/withdrawn match cannot leave a stale label.
  void refreshFunctionNames();
  /// The same from \p Names, the names signatures give entries
  /// (SignatureDB::buildNameMap): a list prepared apart from the session that
  /// publishes it names its entries by that session's signatures.
  void refreshFunctionNames(std::unordered_map<uint64_t, std::string> Names);

  /// Drop the kept C of every function and the kept LLVM IR views.  A change
  /// to anything the C emitters or the views' comments read beyond the
  /// pipeline, such as the user's names (CEmitterOptions::UserNames), must
  /// call this, or a view keeps showing what was emitted before it.
  void forgetEmittedSources() {
    FunctionSources.clear();
    for (FunctionLlvmModule &Kept : FunctionLlvmModules)
      Kept.View.reset();
  }

  /// The widths of the reads and writes the code of every function states at
  /// each address (collectDataAccessWidths), which name unnamed image data in
  /// the C as the listing names it (CEmitterOptions::AccessWidths).  Read
  /// again when the functions change, which drops the C kept with the names
  /// of the old widths.  Null for an image without native code references
  /// (EVM, SBF) or when no decoder could be set up.
  const DataAccessWidths *dataAccessWidths();

  /// The source \p Route emitted for \p Entry under this pipeline, made the
  /// newest; null if it has not emitted one.
  FunctionSource *findFunctionSource(va_t Entry, SourceRoute Route) {
    for (auto It = FunctionSources.begin(); It != FunctionSources.end(); ++It)
      if (It->Entry == Entry && It->Route == Route &&
          It->Options == SourceOptions) {
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
      Source->Options = SourceOptions;
    }
    Source->Text = std::move(Text);
    if (!Map)
      return;
    Source->Map = *Map;
    // Only an emission reads these; a page reads regions and definitions.
    Source->Map->HighSources = nullptr;
    Source->Map->LLVMSources = nullptr;
  }

  void clearPipelineResult() {
    // Every module lives in the context.
    FunctionLlvmModules.clear();
    FunctionSources.clear();
    EnhancedHighIR.reset();
    CallVariant.reset();
    CurrentCallResults.clear();
    UsingAnalystCallContracts = false;
    NativeMedFuncs.reset();
    NativeCalleeDeclarations.clear();
    PipeResult = {};
    LLVMCtx.reset();
    PipeRan = false;
    LlvmModuleNoOpt.reset();
    SBFFunctionsSynchronized = false;
    NativeFunctionsSynchronized = false;
  }

  void clearPipeline() {
    clearPipelineResult();
    // The local types' context refers to the debug context a load replaces
    // after this.
    dropLocalTypesDebug();
    ShownTypes.clear();
    AnalysisCache.reset();
    AccessWidths.reset();
    AccessWidthsFunctions.clear();
    AccessScan.clear();
    ListInputs.reset();
    RecoveredSizes.clear();
    CEmitterSymbols = nullptr;
    CEmitterSymbolCount = 0;
  }

  /// The emitter of this session's C.  It reads the image's function names
  /// again when the symbol table has moved or grown since it read them, or
  /// after clearPipeline(); an image holds tens of thousands.
  HighCEmitter &cEmitter() {
    if (CEmitterSymbols != Img.Symbols.data() ||
        CEmitterSymbolCount != Img.Symbols.size() || !CEmitterSymbols) {
      CEmitter.prepareImageFunctionNames(Img);
      CEmitterSymbols = Img.Symbols.data();
      CEmitterSymbolCount = Img.Symbols.size();
    }
    return CEmitter;
  }

  /// Only navigation may keep immutable image facts. Edits, reloads and
  /// policy changes continue through clearPipeline()/invalidatePipeline().
  void selectFunction(va_t Entry) {
    clearPipelineResult();
    refreshFunctionList();
    OnlyFunctionEntries = {Entry};
  }

  /// The function list as a selection reads it.  A list built from inputs
  /// that have not changed since is the list a rebuild makes, and its names
  /// are derived again as a rebuild derives them; any other list is built
  /// again, an image's tens of thousands of functions.  Selecting another
  /// function does not edit the image, so a rebuild keeps the decoded access
  /// widths: dataAccessWidths still compares every extent after
  /// synchronization, and reads newly discovered or differently sized
  /// functions again.
  void refreshFunctionList() {
    if (functionListCurrent()) {
      refreshFunctionNames();
      return;
    }
    auto KeptWidths = std::move(AccessWidths);
    auto KeptExtents = std::move(AccessWidthsFunctions);
    auto KeptScan = std::move(AccessScan);
    resetFunctionsFromImage();
    AccessWidths = std::move(KeptWidths);
    AccessWidthsFunctions = std::move(KeptExtents);
    AccessScan = std::move(KeptScan);
  }

  void invalidatePipeline() {
    clearPipeline();
    resetFunctionsFromImage();
  }

  /// What a selected function's pipeline must have prepared for a view:
  /// its control-flow graphs alone, the analysis every IR and source view
  /// shares, or that with the HighIR source emission reads.
  enum class PipelineDepth { LowIR, Analysis, HighIR };

  bool ensurePipeline(bool NeedHighIR = true) {
    return ensurePipeline(NeedHighIR ? PipelineDepth::HighIR
                                     : PipelineDepth::Analysis);
  }

  bool ensurePipeline(PipelineDepth Depth) {
    if (!Loaded) {
      setError("no binary loaded");
      return false;
    }
    if (PipelineFeatureGeneration != SigDB.featureGeneration())
      clearPipeline();
    // A deeper view of the same run resumes it with the run's own options.
    const auto runOptions = [&] {
      PipelineOptions Opts;
      applyAnalysisOptions(Opts);
      Opts.DeferMedIR = Depth == PipelineDepth::LowIR;
      Opts.DeferHighIR = Depth != PipelineDepth::HighIR;
      Opts.AnalysisCache = &AnalysisCache;
      return Opts;
    };
    if (!PipeRan) {
      LLVMCtx = std::make_unique<llvm::LLVMContext>();
      PipelineOptions Opts = runOptions();
      Pipeline ThePipeline;
      // Debug names already reached Img.Symbols at load time; handing the
      // context to the pipeline also carries source locations, declared sizes,
      // and parameter names into the IR.
      PipeResult = ThePipeline.run(Img, *LLVMCtx, Opts, decompileDebug());
      PipelineFeatureGeneration = SigDB.featureGeneration();
      PipeRan = true;
    }
    if (Depth != PipelineDepth::LowIR && PipeResult.Success &&
        PipeResult.MedIRDeferred)
      Pipeline().resumeMedIR(Img, runOptions(), PipeResult, decompileDebug());
    if (Depth == PipelineDepth::HighIR && PipeResult.Success &&
        PipeResult.HighIRDeferred) {
      PipelineOptions Opts;
      applyAnalysisOptions(Opts);
      Pipeline().resumeHighIR(Img, Opts, PipeResult, decompileDebug());
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
      setError(missingPipelineFunctionReason(PipeResult, Addr,
                                             "LLVM function not found at 0x" +
                                                 llvm::utohexstr(Addr)));
    return Match;
  }

  /// Emit, verify and optimize native LLVM for the pipeline's functions,
  /// with bodies as \p BodyMask selects (MedLLVMEmitter::emit).  Sets the
  /// error and returns null when the emitter refuses or verification fails.
  std::unique_ptr<llvm::Module>
  emitNativeLlvm(bool NoOpt, const std::vector<char> *BodyMask,
                 std::shared_ptr<LLVMSourceMap> &Sources,
                 bool OriginalImageStorage = false) try {
    // HighIR decompile does not run recoverCallAbi. --func --llvm still emits
    // from that MedIR, so populate CallInfos here; otherwise MedLLVM emits
    // 0-arg calls for unwritten live-in rcx.  A body-masked module declares
    // the other functions by the signatures this recovers too; recovery
    // recomputes each function's call information, so once per pipeline is
    // enough for such modules.
    if (!BodyMask || !NativeMedFuncs) {
      std::map<va_t, std::string> FuncNames;
      for (const auto &MF : PipeResult.MedFuncs)
        if (!MF.Name.empty())
          FuncNames[MF.Entry] = MF.Name;
      for (const auto &[Address, Name] : Img.getImportAddressNames())
        FuncNames[Address] = Name;
      for (auto &[Address, Name] : pipeline_detail::debugFunctionNames(
               PipeResult, FuncNames, decompileDebug()))
        FuncNames[Address] = std::move(Name);
      // Recover the calls as the module pipeline does, with its evidence:
      // every callee's arity, the signatures of callees this pipeline did
      // not keep, and what each function-pointer variable starts at.
      PipelineResult Recovery;
      Recovery.MedFuncs = PipeResult.MedFuncs;
      copyCallSignatureFacts(PipeResult, Recovery);
      Recovery.OmittedCallees = PipeResult.OmittedCallees;
      // The pipeline's callee memo keeps the forwarder probes of those
      // callees, which serve where this recovery reads the same.
      Recovery.CalleeSignatureMemo = PipeResult.CalleeSignatureMemo;
      recoverModuleCallAbi(Img, Recovery, FuncNames,
                           /*SourceDeclarations=*/true,
                           /*SourceParameterReads=*/true);
      // Recovery rebuilds CallInfos. Reapply the same source observation
      // proof as module emission before caching these single-function views.
      prepareRecoveredSourceStackAccesses(Img, Recovery, PipeResult.LowFuncs);
      NativeMedFuncs = std::move(Recovery.MedFuncs);
      NativeCalleeDeclarations = std::move(Recovery.OmittedCalleeDeclarations);
    }
    std::vector<std::pair<va_t, std::string>> ImportMap;
    for (const auto &[Addr, Name] : Img.getImportAddressNames())
      ImportMap.emplace_back(Addr, Name);
    MedLLVMEmitter Emitter;
    Emitter.setPreserveUnknownSourceValues(true);
    Emitter.setSourceCalleeDeclarations(NativeCalleeDeclarations);
    Emitter.setOriginalImageStorageForSource(OriginalImageStorage);
    Emitter.setAnalystSourceProjection(UsingAnalystCallContracts);
    if (UsingAnalystCallContracts && CallVariant)
      Emitter.setSourceTargetSignatures(&CallVariant->TargetSignatures);
    Emitter.setDebugContext(decompileDebug());
    Emitter.setPrototypeArgumentCallees(&PipeResult.CallPrototypeArguments);
    // Native source views need surviving instruction observations even when
    // this function contains no recognized library operation.
    Sources = std::make_shared<LLVMSourceMap>();
    Emitter.setSourceMap(Sources.get());
    auto Candidate = Emitter.emit(*NativeMedFuncs, *LLVMCtx, "neverd_output",
                                  Img.Arch, ImportMap, &Img, Img.abiFormat(),
                                  /*MergeableGlobals=*/false, BodyMask);
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
    if (UsingAnalystCallContracts &&
        !SourceOptions.StackFrame.Variables.empty())
      preserveAnalystStackObjects(*Candidate);
    if (NoOpt) {
      Pipeline::promoteScaffoldingAllocas(*Candidate);
      if (Sources)
        Sources->refreshExpressionOrigins();
    } else {
      const OptimizationResult Optimization = Pipeline::optimizeOrPromoteModule(
          *Candidate, Sources.get(), SourceOptions);
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
  } catch (const std::exception &Error) {
    Sources.reset();
    setError(std::string("native LLVM emission failed: ") + Error.what());
    return nullptr;
  } catch (...) {
    Sources.reset();
    setError("unexpected native LLVM emission failure");
    return nullptr;
  }

  /// The module that shows the function at \p Entry: its body alone, every
  /// other function declared.  Kept for the functions shown last.
  const FunctionLlvmModule *
  ensureFunctionLlvmModule(va_t Entry, bool NoOpt,
                           bool OriginalImageStorage = false) {
    if (PipeRan && PipelineFeatureGeneration != SigDB.featureGeneration())
      clearPipeline();
    SourceViewOptions NativeOptions = SourceOptions;
    NativeOptions.VariableTypes.clear();
    NativeOptions.VariableEdits.clear();
    for (auto It = FunctionLlvmModules.begin(); It != FunctionLlvmModules.end();
         ++It)
      if (It->Entry == Entry && It->NoOpt == NoOpt &&
          It->OriginalImageStorage == OriginalImageStorage &&
          It->Options == NativeOptions) {
        FunctionLlvmModules.splice(FunctionLlvmModules.begin(),
                                   FunctionLlvmModules, It);
        return &FunctionLlvmModules.front();
      }
    if (!ensurePipeline(/*NeedHighIR=*/false))
      return nullptr;
    std::vector<char> Mask(PipeResult.MedFuncs.size(), 0);
    bool Found = false;
    for (size_t I = 0; I < Mask.size(); ++I)
      if (PipeResult.MedFuncs[I].Entry == Entry) {
        Mask[I] = 1;
        Found = true;
      }
    if (!Found) {
      setError(missingPipelineFunctionReason(PipeResult, Entry,
                                             "LLVM function not found at 0x" +
                                                 llvm::utohexstr(Entry)));
      return nullptr;
    }
    FunctionLlvmModule Built;
    Built.Entry = Entry;
    Built.NoOpt = NoOpt;
    Built.OriginalImageStorage = OriginalImageStorage;
    Built.Options = std::move(NativeOptions);
    Built.Module =
        emitNativeLlvm(NoOpt, &Mask, Built.Sources, OriginalImageStorage);
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
  /// Read-only views of the same loaded file retain its analysis additions.
  /// File rewriting and round trips continue to use load().
  bool loadAnalysis(const char *InputPath, std::string &Err,
                    const Session *Policy);
  /// Runs the pipeline over the image with \p Debug, else the image's own
  /// debug information.
  bool run(PipelineOptions Opts, std::string &Err,
           DebugContext *Debug = nullptr);
};

/// Writes \p Values to the sidecar \p Path through a temporary file, so a
/// reader sees the old file or the new one; false with the session's error
/// naming \p Kind.
bool saveSidecar(Session *S, const std::filesystem::path &Path,
                 llvm::json::Value Values, llvm::StringRef Kind);
/// Stream an already canonical value through the same atomic writer. Emit
/// exactly one value with sorted keys and canonical addresses.
bool saveSidecar(Session *S, const std::filesystem::path &Path,
                 llvm::function_ref<void(llvm::json::OStream &)> Emit,
                 llvm::StringRef Kind);

/// The SHA-256 of the input as loaded, in lowercase hexadecimal: the one the
/// loader took while it read the input, else one hash of the file, kept.
/// Empty when the file cannot be read.
std::string inputSha256(Session &S);

/// The loaded input as a project's project.json names it.
llvm::json::Object projectInput(Session &S);
llvm::json::Object sharedCacheIdentity(const BinaryImage &Image);
bool matchesProjectInput(const llvm::json::Object &Left,
                         const llvm::json::Object &Right);

/// The input the project's project.json names, if it names one.
std::optional<llvm::json::Object> recordedInput(const Session &S);

/// Records in the project folder's project.json which build of the input the
/// project's files describe, when it names none or another; false with the
/// session's error.
bool recordProjectInput(Session *S);

/// Read the session's input again as it was loaded and publish \p Request's
/// debug information into it, in the order the names rank, so a file loaded
/// after the image ranks as it would had the load named it: a PDB's or
/// DWARF's names replace a MAP's, and a MAP's fill only what they leave.
/// The session then takes the image, keeping the user's names, comments and
/// edits, and \p Request becomes its next load's.  The session's analysis
/// sections join the image read again as an edit adds them; one it cannot
/// take keeps the session as it was.  \p Unchanged, when given, is asked once
/// the files are read and before the session takes them; it sets the error
/// when it answers false.  Returns 1, or 0 with the session's error and the
/// session as it was.
int reloadDebugInformation(Session &S, DebugInfoRequest Request,
                           llvm::function_ref<bool()> Unchanged = {});

/// \p Image, the session's input read again, with the session's analysis
/// sections added as neverd_analysis_section_add adds them, from the bytes
/// the session's image holds; else the error naming the section \p Image
/// cannot take.  Without sections \p Image passes through unchanged.
llvm::Expected<BinaryImage> withAnalysisSections(const Session &S,
                                                 BinaryImage Image);

/// The files \p S read its debug information from, as
/// neverd_session_debug_info_json() lists them.
llvm::json::Array debugInfoFilesJson(const Session &S);

} // namespace sdk
} // namespace neverd

#endif // NEVERD_SDK_CAPI_SESSION_IMPL_H
