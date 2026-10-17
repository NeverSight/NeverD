//===- NeverDCAPI.cpp - C API: session lifecycle and core queries
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Session lifecycle, function list, lookup helpers, raw-byte access,
/// error handling, memory management, session metadata, and version.
///
/// Additional C API domains live in sibling files:
///   NeverDCAPIDisasm.cpp      — disassembly (JSON + text)
///   NeverDCAPIInfo.cpp        — info panels and address resolution
///   NeverDCAPIGraph.cpp       — xrefs, CFG, and call graph
///   NeverDCAPISearch.cpp      — search and diff
///   NeverDCAPIDecompile.cpp   — decompile, IR dump, lift, inject
///   NeverDCAPITargetConfig.cpp — EVM and SBF analysis configuration
///   NeverDCAPIPatch.cpp       — patch operations
///   NeverDCAPIRoundtrip.cpp   — lift-to-object verification
///   NeverDCAPIPersist.cpp     — annotations and renames
///   NeverDCAPISigs.cpp        — FLIRT signatures
///   NeverDCAPIBench.cpp       — benchmark support
///
//===----------------------------------------------------------------------===//

#include "JSONText.h"
#include "LoadOptions.h"
#include "NativeMobileSession.h"
#include "NativePhaseTrace.h"
#include "NativeSnapshotSession.h"
#include "SessionImpl.h"

#include "neverd/Common.h"
#include "neverd/debug/DebugInfoDiscovery.h"
#include "neverd/debug/PDBFunctionNameHint.h"
#include "neverd/debug/PDBLoader.h"
#include "neverd/debug/SymbolStore.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/ELF/SBFELFLoader.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/loader/ObjectFileUtils.h"
#include "neverd/loader/StaticSymbolTable.h"
#include "neverd/sbf/analysis/SBFAnalysisLimits.h"
#include "neverd/sbf/analysis/SBFFunctionBody.h"
#include "neverd/sdk/NeverDPlugin.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/support/FilePath.h"
#include "neverd/support/ProjectFiles.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <exception>

using namespace neverd;
using namespace neverd::sdk;

namespace neverd::sdk {

bool Session::synchronizeFunctions() {
  if (Img.Arch != Arch::SBF) {
    // Native function discovery lives in the pipeline, not in the loader.
    // Publish its accepted functions after either explicit or lazy analysis,
    // without making metadata/function-list queries run the whole pipeline.
    if (Img.Arch == Arch::EVM || !PipeRan || !PipeResult.Success ||
        NativeFunctionsSynchronized)
      return true;
    llvm::DenseMap<va_t, size_t> FunctionIndices;
    FunctionIndices.reserve(Functions.size() + PipeResult.LowFuncs.size());
    for (size_t Index = 0; Index < Functions.size(); ++Index)
      FunctionIndices.try_emplace(Functions[Index].Entry, Index);
    for (const LowFunc &Function : PipeResult.LowFuncs) {
      if (isDeletedFunction(Function.Entry) ||
          !FunctionIndices.try_emplace(Function.Entry, Functions.size()).second)
        continue;
      // Existing loader/debug names and sizes remain authoritative. A saved
      // rename can precede discovery when reopening a stripped image.
      OriginalNames.try_emplace(Function.Entry, Function.Name);
      const auto Rename = Renames.find(Function.Entry);
      // This entry was absent from the image inventory. Its recovered name
      // is analysis evidence, regardless of its spelling.
      const NameOrigin Origin = NameOrigin::Analysis;
      Functions.push_back(
          {Function.Entry, Function.OriginalSize,
           Rename == Renames.end() ? Function.Name : Rename->second,
           Rename == Renames.end() ? Origin : NameOrigin::User, Function.Name,
           Origin});
    }
    std::stable_sort(Functions.begin(), Functions.end(),
                     [](const FuncInfo &Left, const FuncInfo &Right) {
                       return Left.Entry < Right.Entry;
                     });
    NativeFunctionsSynchronized = true;
    refreshFunctionNames();
    return true;
  }
  if (SBFFunctionsSynchronized)
    return true;
  if (!ensurePipeline() || !PipeResult.SBF)
    return false;

  resetFunctionsFromImage();

  const sbf::SBFProgram &Program = *PipeResult.SBF;
  const sbf::FunctionBodyIndex FunctionBodies(Program);
  const sbf::FunctionBodyIndex::ByteSizeBatch FunctionSizes =
      FunctionBodies.byteSizes(sbf::kFunctionBodyBatchBlockVisitBudget);
  llvm::DenseMap<va_t, size_t> FunctionIndices;
  FunctionIndices.reserve(Functions.size() + Program.High.Functions.size());
  for (size_t FunctionID = 0; FunctionID < Functions.size(); ++FunctionID)
    FunctionIndices.try_emplace(Functions[FunctionID].Entry, FunctionID);

  for (size_t SBFID = 0; SBFID < Program.High.Functions.size(); ++SBFID) {
    const sbf::Function &Function = Program.High.Functions[SBFID];
    const uint64_t Size = FunctionSizes.Bytes[SBFID];
    size_t FunctionID = 0;
    if (auto Existing = FunctionIndices.find(Function.Address);
        Existing != FunctionIndices.end()) {
      FunctionID = Existing->second;
      // SBF semantic reachability, not attacker-controlled ELF st_size, is the
      // public function-size authority. A budget-exhausted batch is explicitly
      // unknown and therefore overwrites any untrusted symbol size with zero.
      Functions[FunctionID].Size = FunctionSizes.Exact.test(SBFID) ? Size : 0;
    } else {
      FunctionID = Functions.size();
      Functions.push_back({Function.Address,
                           FunctionSizes.Exact.test(SBFID) ? Size : 0,
                           Function.Name, NameOrigin::Analysis, Function.Name,
                           NameOrigin::Analysis});
      FunctionIndices.try_emplace(Function.Address, FunctionID);
    }
    OriginalNames.try_emplace(Function.Address, Function.Name);
    if (auto Rename = Renames.find(Function.Address); Rename != Renames.end()) {
      Functions[FunctionID].Name = Rename->second;
      Functions[FunctionID].Origin = NameOrigin::User;
    }
  }
  std::sort(Functions.begin(), Functions.end(),
            [](const FuncInfo &Left, const FuncInfo &Right) {
              return Left.Entry < Right.Entry;
            });
  SBFFunctionsSynchronized = true;
  return true;
}

} // namespace neverd::sdk

// ===--------------------------------------------------------------------===//
// PipelineRunner implementation (shared by high-level C API functions)
// ===--------------------------------------------------------------------===//

bool PipelineRunner::load(const char *InputPath, std::string &Err,
                          const Session *Policy) {
  if (!InputPath || InputPath[0] == '\0') {
    Err = "input path is empty";
    return false;
  }
  auto Path = std::filesystem::u8path(InputPath);
  if (!std::filesystem::exists(Path)) {
    Err = "file not found: " + pathToUTF8(Path);
    return false;
  }
  BinaryLoadOptions LoadOpts;
  if (Policy) {
    LoadOpts.OnlyFunctionEntries = Policy->OnlyFunctionEntries;
    LoadOpts.ARMFunctionModes = Policy->ARMFunctionModes;
    LoadOpts.ReadStaticSymbols = Policy->ReadsStaticSymbols;
    DbgRequest = Policy->DbgRequest;
  }
  // The file reads with the loader its project keeps, as a session load
  // reads it; the pipeline refuses a binary file.
  if (Policy && Policy->RequestedLoad) {
    LoadOpts.Choice = *Policy->RequestedLoad;
  } else if (auto Kept = readLoadOptionsSidecar(Path); !Kept) {
    Err = llvm::toString(Kept.takeError());
    return false;
  } else {
    LoadOpts.Choice = *Kept;
  }
  if (auto Unsettled = settleBinaryFileProcessor(Path, LoadOpts.Choice)) {
    Err = llvm::toString(std::move(Unsettled));
    return false;
  }
  auto ImgOrErr = loadBinary(Path, LoadOpts);
  if (!ImgOrErr) {
    llvm::handleAllErrors(
        ImgOrErr.takeError(),
        [&](const llvm::ErrorInfoBase &E) { Err = E.message(); });
    return false;
  }
  Img = std::move(*ImgOrErr);
  settleBinaryFilePlatform(Img, LoadOpts.Choice);
  // Analysis of the session's input reads what its user states, as the
  // function edits the options carry.
  if (Policy)
    Img.UserNoReturn = Policy->Img.UserNoReturn;

  auto Found = loadDebugInfo(Path, Img, DbgRequest);
  if (!Found.Error.empty()) {
    Err = Found.Error;
    return false;
  }
  applyDebugInfo(Img, Found);
  if (Found)
    Dbg = std::move(Found.Context);
  return true;
}

bool PipelineRunner::run(PipelineOptions Opts, std::string &Err,
                         DebugContext *Debug) {
  Pipeline ThePipeline;
  Result = ThePipeline.run(Img, LLVMCtx, Opts, Debug ? Debug : Dbg.get());
  if (!Result.Success) {
    Err = Result.Error.empty() ? "pipeline failed" : Result.Error;
    return false;
  }
  return true;
}

// ===--------------------------------------------------------------------===//
// Session lifecycle
// ===--------------------------------------------------------------------===//

neverd_session_t neverd_session_create(void) { return new Session(); }

void neverd_session_destroy(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return;
  S->PM.termAll();
  delete S;
}

namespace {
int finishSessionLoad(neverd_session_t Sess, Session &S, BinaryImage Image,
                      std::filesystem::path Path,
                      std::filesystem::path SanitizeSourcePath,
                      std::optional<uint64_t> MemoryBytes = std::nullopt,
                      std::string NativeProvenance = {}) {
  // Prepare the complete replacement before changing the current image, its
  // analysis, or the user's edits. An authoritative debug-file failure must
  // leave a previously loaded session usable.
  S.LoadProgressCb.report("image", 1, 1, "binary parsed");
  DebugInfoResult Found;
  if (!MemoryBytes)
    Found = loadDebugInfo(Path, Image, S.DbgRequest, S.LoadProgressCb);
  if (!Found.Error.empty()) {
    S.setError(Found.Error);
    return 0;
  }
  applyDebugInfo(Image, Found);
  S.LoadProgressCb.report("ready", 1, 1, "");

  // Decoder::init retains its existing state on failure, so it is the final
  // preparation step before publishing the replacement.
  if (Image.Arch != Arch::EVM && Image.Arch != Arch::SBF &&
      !S.Dec.init(Image)) {
    S.setError("failed to init decoder for arch");
    return 0;
  }

  // Dispose analysis while the image/debug objects it was built from still
  // exist. clearPipeline also releases LLVM modules before their context.
  S.clearPipeline();
  S.Img = std::move(Image);
  S.FilePath = std::move(Path);
  S.SanitizeSourcePath = std::move(SanitizeSourcePath);
  S.MemoryInputBytes = MemoryBytes;
  S.WebNativeProvenance = std::move(NativeProvenance);
  S.Dbg = std::move(Found.Context);
  S.DbgKind = Found.Kind;
  S.DbgPath = std::move(Found.Path);
  S.DbgFiles = std::move(Found.Files);
  // neverd_session_load() says so once the load is its own.
  S.ReadFromFile = false;
  S.dropLocalTypes();
  S.Loaded = true;
  S.LastPatch = {};
  S.LastSourcePatchReport.clear();
  S.Annotations.clear();
  S.Renames.clear();
  S.FunctionEdits.clear();
  S.FunctionAttributeEdits.clear();
  S.DataItems.clear();
  S.OperandFormats.clear();
  S.TypeDeclarations.clear();
  S.SigDB.clear();
  S.DiscoveredFunctions.reset();
  S.ImageStrings.reset();
  S.resetFunctionsFromImage();

  if (!MemoryBytes) {
    neverd_annotations_load(Sess);
    neverd_renames_load(Sess);
    neverd_functions_load(Sess);
    neverd_items_load(Sess);
    neverd_operand_formats_load(Sess);
    neverd_type_declarations_load(Sess);
  }

  return 1;
}
} // namespace

const char *neverd_identify_json(const char *Path) {
  llvm::json::Array Rows;
  llvm::json::Object Root;
  std::error_code EC;
  const auto Input = std::filesystem::u8path(Path ? Path : "");
  if (!Path || !std::filesystem::is_regular_file(Input, EC)) {
    Root["rows"] = std::move(Rows);
    Root["error"] =
        jsonSafeText(std::string("not a regular file: ") + (Path ? Path : ""));
    return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
  }
  for (const LoadCandidate &Row : identifyFile(Input)) {
    llvm::json::Object Entry{{"loader", getLoadRowLoader(Row.Row)},
                             {"text", jsonSafeText(Row.Description)},
                             {"processor", Row.TheArch == Arch::Unknown
                                               ? std::string()
                                               : getArchName(Row.TheArch)},
                             {"bits", Row.Bits},
                             {"endian", Row.BigEndian ? "big" : "little"},
                             {"loadable", Row.Loadable},
                             {"by_name", Row.ByName}};
    if (!Row.Reason.empty())
      Entry["reason"] = jsonSafeText(Row.Reason);
    // What a headerless file's bytes show of the processor they hold code
    // for, and the processor a load reads them as unasked.
    if (Row.ISA) {
      llvm::json::Array Guesses;
      for (const ISAGuess &Guess : Row.ISA->Guesses)
        Guesses.push_back(llvm::json::Object{{"isa", Guess.ISA},
                                             {"name", Guess.Name},
                                             {"processor", Guess.Processor},
                                             {"share", Guess.Share}});
      Entry["guesses"] = std::move(Guesses);
      Entry["code_share"] = Row.ISA->CodeShare;
      Entry["status"] = Row.ISA->outcomeName();
      Entry["code_unit"] = Row.ISA->CodeUnit;
      Entry["code_offset"] = Row.ISA->CodeOffset;
      if (Row.ISA->WideShare)
        Entry["wide_share"] = *Row.ISA->WideShare;
      Entry["detected"] = Row.ISA->detectedProcessor();
      Entry["evidence"] = Row.ISA->describe();
      if (const auto &Print = Row.ISA->Fingerprint)
        Entry["fingerprint"] =
            llvm::json::Object{{"name", Print->Name},
                               {"processor", Print->Processor},
                               {"entry", vaHex(Print->Entry)},
                               {"base", vaHex(Print->Base)}};
    }
    Rows.push_back(std::move(Entry));
  }
  Root["rows"] = std::move(Rows);
  return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
}

namespace {
/// Why a local copy of a PDB does not load, in the C API.
constexpr llvm::StringLiteral MismatchedPDB("mismatch");
constexpr llvm::StringLiteral UnreadablePDB("unreadable");
} // namespace

const char *neverd_pdb_reference_json(const char *Path,
                                      const char *SearchPath) {
  llvm::json::Object Root;
  const auto Answer = [&] {
    return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
  };
  Root["pdb"] = nullptr;
  const auto Input = std::filesystem::u8path(Path ? Path : "");
  auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(Input), /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (!Buffer) {
    Root["error"] = jsonSafeText("cannot read " + pathToUTF8(Input) + ": " +
                                 Buffer.getError().message());
    return Answer();
  }
  // The debug directory alone says which PDB the image names; reading it
  // costs no load of the image.
  auto Object = llvm::object::createBinary((*Buffer)->getMemBufferRef());
  if (!Object) {
    llvm::consumeError(Object.takeError());
    return Answer();
  }
  const auto *COFF =
      llvm::dyn_cast<llvm::object::COFFObjectFile>(Object->get());
  if (!COFF)
    return Answer();
  BinaryImage Image;
  Image.Format = BinaryFormat::COFF;
  coff_loader::parseDebugDirectory(*COFF, Image);
  const auto Request = pdbSymbolRequest(Image);
  if (!Request)
    return Answer();

  const PDBBuildIdentity &Expected = *Image.DynInfo.CodeViewPDBIdentity;
  const SymbolPath Search = symbolPath(
      SearchPath ? std::optional<std::string>(SearchPath) : std::nullopt);
  llvm::json::Value Found = nullptr;
  llvm::json::Array Rejected;
  for (const PDBCandidate &Candidate : pdbCandidates(Input, Image, Search)) {
    std::error_code EC;
    if (!std::filesystem::is_regular_file(Candidate.Path, EC))
      continue;
    // A frontend keeps the path it loads, so it must not depend on the
    // working directory the input was named against.
    llvm::json::Object Copy{
        {"path", pathToUTF8(std::filesystem::absolute(Candidate.Path, EC))},
        {"location", pdbLocationName(Candidate.Location)}};
    auto Identity = readPDBBuildIdentity(Candidate.Path);
    if (Identity && *Identity == Expected) {
      Found = std::move(Copy);
      break;
    }
    Copy["reason"] = Identity ? MismatchedPDB : UnreadablePDB;
    if (!Identity)
      Copy["detail"] = jsonSafeText(llvm::toString(Identity.takeError()));
    Rejected.push_back(std::move(Copy));
  }
  llvm::json::Array Servers;
  for (const SymbolServer &Server : Search.Servers)
    if (!Server.Cache.empty())
      Servers.push_back(llvm::json::Object{
          {"url", jsonSafeText(symbolServerURL(Server, *Request))},
          {"path", pathToUTF8(symbolStorePath(Server.Cache, *Request))}});
  Root["pdb"] =
      llvm::json::Object{{"file", Request->FileName},
                         {"key", Request->Key},
                         {"recorded", jsonSafeText(Image.DynInfo.PDBPath)},
                         {"found", std::move(Found)},
                         {"rejected", std::move(Rejected)},
                         {"servers", std::move(Servers)}};
  return Answer();
}

const char *neverd_dwarf_reference_json(const char *Path) {
  llvm::json::Object Root;
  const auto Answer = [&] {
    return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
  };
  Root["dwarf"] = nullptr;
  const auto Input = std::filesystem::u8path(Path ? Path : "");
  auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(Input), /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (!Buffer) {
    Root["error"] = jsonSafeText("cannot read " + pathToUTF8(Input) + ": " +
                                 Buffer.getError().message());
    return Answer();
  }
  // The magic says the format; an SBF program is ELF, but discovery reads no
  // debug information for it.
  const BinaryFormat Format =
      magicToFormat(llvm::identify_magic((*Buffer)->getBuffer()));
  const llvm::ArrayRef<uint8_t> Bytes(
      reinterpret_cast<const uint8_t *>((*Buffer)->getBufferStart()),
      (*Buffer)->getBufferSize());
  bool SupportedSBF = false;
  if (Format == BinaryFormat::ELF && isSBFELF(Bytes, SupportedSBF) &&
      !searchesDebugInfo(Arch::SBF))
    return Answer();
  const auto Source = findDWARF(Input, Format);
  if (!Source)
    return Answer();
  // A frontend keeps the path it shows, so it must not depend on the working
  // directory the input was named against.
  std::error_code EC;
  Root["dwarf"] = llvm::json::Object{
      {"path", pathToUTF8(std::filesystem::absolute(Source->Path, EC))},
      {"location", dwarfLocationName(Source->Location)}};
  return Answer();
}

const char *neverd_symbol_table_reference_json(const char *Path) {
  // Enough of the table's names to tell planted ones by, for a question.
  constexpr size_t SampleNames = 8;
  llvm::json::Object Root;
  const auto Answer = [&] {
    return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
  };
  Root["symbols"] = nullptr;
  auto Found = findStaticSymbolTable(std::filesystem::u8path(Path ? Path : ""),
                                     SampleNames);
  if (!Found) {
    Root["error"] = jsonSafeText(llvm::toString(Found.takeError()));
    return Answer();
  }
  if (!*Found)
    return Answer();
  llvm::json::Array Names;
  for (const auto &Name : (*Found)->Names)
    Names.push_back(jsonSafeText(Name));
  Root["symbols"] = llvm::json::Object{{"table", (*Found)->Table},
                                       {"count", (*Found)->Count},
                                       {"names", std::move(Names)}};
  return Answer();
}

int neverd::sdk::loadNativeSnapshotSession(neverd_session_t Sess,
                                           BinaryImage Image,
                                           uint64_t InputBytes,
                                           std::string Provenance) {
  auto *S = toSession(Sess);
  if (!S || S->Loaded || Image.Raw.size() != InputBytes || Provenance.empty())
    return 0;
  return finishSessionLoad(Sess, *S, std::move(Image), {}, {}, InputBytes,
                           std::move(Provenance));
}

int neverd_session_load(neverd_session_t Sess, const char *Path) {
  auto *S = toSession(Sess);
  if (!S)
    return 0;
  NativePhaseTrace Trace(NativePhaseTrace::Phase::SessionLoad);
  S->clearError();

  if (!Path || Path[0] == '\0') {
    S->setError("input path is empty");
    Trace.finish(false);
    return 0;
  }
  auto P = std::filesystem::u8path(Path);
  if (!std::filesystem::exists(P)) {
    S->setError(std::string("file not found: ") + Path);
    Trace.finish(false);
    return 0;
  }

  // Capture a separate absolute, symlink-free source locator at the trusted
  // load boundary.  Keep P unchanged: FilePath is an older public contract
  // used by sidecar discovery and must preserve the caller's spelling.
  std::error_code CanonicalError;
  std::filesystem::path SanitizeSourcePath =
      std::filesystem::canonical(P, CanonicalError);
  if (CanonicalError) {
    S->setError("cannot canonicalize input path: " + CanonicalError.message());
    Trace.finish(false);
    return 0;
  }

  S->LoadProgressCb.report("image", 0, 1, "reading binary");
  BinaryLoadOptions LoadOpts;
  LoadOpts.OnlyFunctionEntries = S->OnlyFunctionEntries;
  LoadOpts.ARMFunctionModes = S->ARMFunctionModes;
  LoadOpts.ReadStaticSymbols = S->ReadsStaticSymbols;
  // A project an older NeverD kept as files beside the input moves into its
  // folder, where the rest of the C API and the workbench read it.
  project_files::migrate(P);
  // The file reads with the loader the caller chose, else the one its
  // project keeps.
  if (S->RequestedLoad) {
    LoadOpts.Choice = *S->RequestedLoad;
  } else if (auto Kept = readLoadOptionsSidecar(P); !Kept) {
    S->setError(llvm::toString(Kept.takeError()));
    Trace.finish(false);
    return 0;
  } else {
    LoadOpts.Choice = *Kept;
  }
  if (auto Unsettled = settleBinaryFileProcessor(P, LoadOpts.Choice)) {
    S->setError(llvm::toString(std::move(Unsettled)));
    Trace.finish(false);
    return 0;
  }
  auto ImgOrErr = loadBinary(P, LoadOpts);
  if (!ImgOrErr) {
    std::string Err;
    llvm::raw_string_ostream OS(Err);
    logAllUnhandledErrors(ImgOrErr.takeError(), OS);
    S->setError(Err);
    Trace.finish(false);
    return 0;
  }
  settleBinaryFilePlatform(*ImgOrErr, LoadOpts.Choice);
  const int Result =
      finishSessionLoad(Sess, *S, std::move(*ImgOrErr), std::move(P),
                        std::move(SanitizeSourcePath));
  if (Result) {
    S->LoadedChoice = LoadOpts.Choice;
    S->ReadFromFile = true;
  }
  Trace.finish(Result != 0);
  return Result;
}

int neverd::sdk::loadNativeMobileSession(neverd_session_t Sess,
                                         const char *UTF8Path,
                                         NativeMobileMetadataObserver Observer,
                                         void *Context) {
  auto *S = toSession(Sess);
  if (!S)
    return 0;
  NativePhaseTrace Trace(NativePhaseTrace::Phase::SessionLoad);
  S->clearError();
  if (!UTF8Path || !*UTF8Path || !Observer) {
    S->setError(!UTF8Path || !*UTF8Path
                    ? "input path is empty"
                    : "mobile metadata observer is missing");
    Trace.finish(false);
    return 0;
  }
  auto Path = std::filesystem::u8path(UTF8Path);
  if (!std::filesystem::exists(Path)) {
    S->setError(std::string("file not found: ") + UTF8Path);
    Trace.finish(false);
    return 0;
  }
  std::error_code Error;
  auto Canonical = std::filesystem::canonical(Path, Error);
  if (Error) {
    S->setError("cannot canonicalize input path: " + Error.message());
    Trace.finish(false);
    return 0;
  }
  MachOLoader Loader;
  Loader.setReadsStaticSymbols(S->ReadsStaticSymbols);
  auto Image = Loader.load(Path);
  if (!Image) {
    S->setError("invalid selected Mach-O: " +
                llvm::toString(Image.takeError()));
    Trace.finish(false);
    return 0;
  }
  Observer(*Image, Context);
  normalizeBinaryMetadata(*Image);
  const int Result = finishSessionLoad(Sess, *S, std::move(*Image),
                                       std::move(Path), std::move(Canonical));
  Trace.finish(Result != 0);
  return Result;
}

int neverd_session_is_loaded(neverd_session_t Sess) {
  return toSession(Sess)->Loaded ? 1 : 0;
}

// ===--------------------------------------------------------------------===//
// Debug information
// ===--------------------------------------------------------------------===//

void neverd_session_set_pdb_path(neverd_session_t Sess, const char *Path) {
  (void)neverd_session_set_pdb_path_ex(Sess, Path, 0);
}

int neverd_session_set_pdb_path_ex(neverd_session_t Sess, const char *Path,
                                   int UseWithoutIdentity) {
  auto *S = toSession(Sess);
  if (!S)
    return 0;
  S->clearError();
  if (UseWithoutIdentity != 0 && UseWithoutIdentity != 1) {
    S->setError("UseWithoutIdentity must be 0 or 1");
    return 0;
  }
  S->DbgRequest.PDBPath = Path ? std::filesystem::u8path(Path) : "";
  // Only a named PDB carries the user's choice; discovery stays strict.
  S->DbgRequest.AllowMissingPDBIdentity =
      UseWithoutIdentity == 1 && !S->DbgRequest.PDBPath.empty();
  return 1;
}

const char *neverd_session_inspect_pdb_json(neverd_session_t Sess,
                                            const char *Path) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded || !Path || !*Path) {
    if (S)
      S->setError("Load a PE image and select a PDB file first");
    return nullptr;
  }
  const auto PdbPath = std::filesystem::u8path(Path);
  auto Context = PDBDebugContext::inspect(PdbPath, S->Img, S->LoadProgressCb);
  if (!Context) {
    S->setError(llvm::toString(Context.takeError()));
    return nullptr;
  }
  llvm::json::Array Names;
  const bool Verified = (*Context)->hasAuthenticatedImageIdentity();
  size_t Count = 0;
  for (const FunctionSym &Function : (*Context)->allFunctions())
    if (!Function.Name.empty()) {
      ++Count;
      if (!Verified)
        Names.push_back(
            llvm::json::Object{{"address", vaHex(Function.Addr)},
                               {"name", jsonSafeText(Function.Name)},
                               {"function", true}});
    }
  for (const DataObjectSym &Object : (*Context)->allDataObjects())
    if (!Object.Name.empty()) {
      ++Count;
      if (!Verified)
        Names.push_back(llvm::json::Object{{"address", vaHex(Object.Addr)},
                                           {"name", jsonSafeText(Object.Name)},
                                           {"function", false}});
    }
  if (Count == 0) {
    S->setError("No usable symbol names in the selected PDB");
    return nullptr;
  }
  const auto Text = jsonToString(
      llvm::json::Object{{"verified", Verified},
                         {"path", pathToUTF8(PdbPath)},
                         {"name_count", static_cast<int64_t>(Count)},
                         {"names", std::move(Names)}});
  if (Text.size() > 32 * 1024 * 1024) {
    S->setError("PDB name import exceeds the 32 MiB response budget");
    return nullptr;
  }
  return dupStr(Text);
}

int neverd_session_load_pdb(neverd_session_t Sess, const char *Path) {
  return neverd_session_load_pdb_ex(Sess, Path, 0);
}

int neverd_session_load_pdb_ex(neverd_session_t Sess, const char *Path,
                               int UseWithoutIdentity) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded || !Path || !*Path) {
    if (S)
      S->setError("Load a PE image and select a PDB file first");
    return 0;
  }
  if (UseWithoutIdentity != 0 && UseWithoutIdentity != 1) {
    S->setError("UseWithoutIdentity must be 0 or 1");
    return 0;
  }
  // The PDB joins what the image was loaded with, and outranks its MAP.
  DebugInfoRequest Request = S->DbgRequest;
  Request.Enabled = true;
  Request.PDBPath = std::filesystem::u8path(Path);
  Request.AllowMissingPDBIdentity = UseWithoutIdentity == 1;
  std::error_code EC;
  const auto PdbSize = std::filesystem::file_size(Request.PDBPath, EC);
  if (EC) {
    S->setError("Cannot read the selected PDB: " + EC.message());
    return 0;
  }
  const auto PdbTime = std::filesystem::last_write_time(Request.PDBPath, EC);
  if (EC) {
    S->setError("Cannot read the selected PDB: " + EC.message());
    return 0;
  }
  const std::filesystem::path Selected = Request.PDBPath;
  return reloadDebugInformation(*S, std::move(Request), [&] {
    std::error_code EC;
    const auto CurrentSize = std::filesystem::file_size(Selected, EC);
    const auto CurrentTime = std::filesystem::last_write_time(Selected, EC);
    if (!EC && CurrentSize == PdbSize && CurrentTime == PdbTime)
      return true;
    S->setError("The selected PDB changed while loading; select it again");
    return false;
  });
}

void neverd_session_set_debug_info_enabled(neverd_session_t Sess, int Enabled) {
  if (auto *S = toSession(Sess))
    S->DbgRequest.Enabled = Enabled != 0;
}

void neverd_session_set_source_symbolic(neverd_session_t Sess, int Enabled) {
  if (auto *S = toSession(Sess))
    S->SourceSymbolic = Enabled != 0;
}

void neverd_session_set_pdb_search(neverd_session_t Sess, int Enabled) {
  if (auto *S = toSession(Sess))
    S->DbgRequest.SearchPDB = Enabled != 0;
}

void neverd_session_set_symbol_table(neverd_session_t Sess, int Enabled) {
  if (auto *S = toSession(Sess))
    S->ReadsStaticSymbols = Enabled != 0;
}

void neverd_session_set_dwarf_search(neverd_session_t Sess, int Enabled) {
  if (auto *S = toSession(Sess))
    S->DbgRequest.SearchDWARF = Enabled != 0;
}

void neverd_session_set_symbol_path(neverd_session_t Sess,
                                    const char *SymbolPath) {
  if (auto *S = toSession(Sess))
    S->DbgRequest.SymbolPath =
        SymbolPath ? std::optional<std::string>(SymbolPath) : std::nullopt;
}

void neverd_session_restrict_function(neverd_session_t Sess,
                                      neverd_va_t Entry) {
  auto *S = toSession(Sess);
  if (!S)
    return;
  std::set<va_t> Next;
  if (Entry)
    Next.insert(Entry);
  // A pipeline computed for another restriction must not answer queries for
  // this one.
  if (S->PipeRan && Next != S->OnlyFunctionEntries)
    S->invalidatePipeline();
  S->OnlyFunctionEntries = std::move(Next);
}

int neverd_session_set_arm_function_mode(neverd_session_t Sess,
                                         neverd_va_t Entry, int Mode) {
  auto *S = toSession(Sess);
  if (!S)
    return 0;
  S->clearError();
  switch (Mode) {
  case NEVERD_ARM_FUNCTION_MODE_CLEAR:
    S->ARMFunctionModes.erase(Entry);
    return 1;
  case NEVERD_ARM_FUNCTION_MODE_ARM:
    S->ARMFunctionModes[Entry] = InstructionMode::ARM;
    return 1;
  case NEVERD_ARM_FUNCTION_MODE_THUMB:
    S->ARMFunctionModes[Entry] = InstructionMode::Thumb;
    return 1;
  default:
    S->setError("ARM function mode must be CLEAR, ARM, or Thumb");
    return 0;
  }
}

neverd_va_t neverd_session_resolve_function_name_before_load(
    neverd_session_t Sess, const char *BinaryPath, const char *Name) {
  auto *S = toSession(Sess);
  if (!S || !BinaryPath || !*BinaryPath || !Name || !*Name ||
      !S->DbgRequest.Enabled || S->DbgRequest.PDBPath.empty())
    return 0;
  try {
    return resolvePDBPublicFunctionNameHint(std::filesystem::u8path(BinaryPath),
                                            S->DbgRequest.PDBPath, Name)
        .value_or(0);
  } catch (const std::exception &) {
    // An optional pre-load lookup never replaces normal load diagnostics.
    return 0;
  }
}

void neverd_session_set_load_progress(neverd_session_t Sess,
                                      neverd_load_progress_fn Fn,
                                      void *UserData) {
  if (auto *S = toSession(Sess)) {
    S->LoadProgressCb.Callback = Fn;
    S->LoadProgressCb.User = UserData;
  }
}

const char *neverd_session_debug_info_kind(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return dupStr(debugInfoKindName(S ? S->DbgKind : DebugInfoKind::None));
}

const char *neverd_session_debug_info_path(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return dupStr(S ? pathToUTF8(S->DbgPath) : std::string());
}

int neverd_session_discover_functions(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return -1;
  }
  const bool Native = S->Img.Arch == Arch::X86 || S->Img.Arch == Arch::X64 ||
                      S->Img.Arch == Arch::ARM || S->Img.Arch == Arch::AArch64;
  if (Native && !S->DiscoveredFunctions) {
    Decoder Dec;
    if (!Dec.init(S->Img)) {
      S->setError("failed to initialize a decoder for the image");
      return -1;
    }
    FuncDetector Detector;
    S->DiscoveredFunctions = Detector.detect(S->Img, Dec);
    if (S->appendDiscoveredFunctions())
      S->refreshFunctionNames();
  }
  return static_cast<int>(S->Functions.size());
}

int neverd_session_analyze(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return (S->ensurePipeline() && S->synchronizeFunctions()) ? 1 : 0;
}

const char *neverd_session_file_path(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S->Loaded ? dupStr(pathToUTF8(S->FilePath)) : dupStr(std::string());
}

const char *neverd_session_arch_name(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return dupStr(S->Loaded ? getArchName(S->Img.Arch) : "unknown");
}

const char *neverd_session_format_name(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return dupStr(S->Loaded ? S->Img.getFormatName() : "unknown");
}

int neverd_session_is_64bit(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return (S->Loaded && S->Img.is64Bit()) ? 1 : 0;
}

int neverd_session_bitness(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded)
    return 0;
  return static_cast<int>(getBitnessValue(S->Img.Bits));
}

// ===--------------------------------------------------------------------===//
// Function list
// ===--------------------------------------------------------------------===//

int neverd_func_count(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded || !S->synchronizeFunctions())
    return 0;
  return static_cast<int>(S->Functions.size());
}

neverd_va_t neverd_func_entry(neverd_session_t Sess, int Idx) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded || !S->synchronizeFunctions() || Idx < 0 ||
      Idx >= static_cast<int>(S->Functions.size()))
    return 0;
  return S->Functions[Idx].Entry;
}

int neverd_func_size(neverd_session_t Sess, int Idx) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded || !S->synchronizeFunctions() || Idx < 0 ||
      Idx >= static_cast<int>(S->Functions.size()))
    return 0;
  return static_cast<int>(S->Functions[Idx].Size);
}

const char *neverd_func_name(neverd_session_t Sess, int Idx) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded || !S->synchronizeFunctions() || Idx < 0 ||
      Idx >= static_cast<int>(S->Functions.size()))
    return dupStr(std::string());
  return dupStr(S->Functions[Idx].Name);
}

// ===--------------------------------------------------------------------===//
// Function lookup helpers
// ===--------------------------------------------------------------------===//

int neverd_func_find_by_name(neverd_session_t Sess, const char *Name) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded || !Name || !S->synchronizeFunctions())
    return -1;
  for (size_t I = 0; I < S->Functions.size(); ++I)
    if (S->Functions[I].Name == Name)
      return static_cast<int>(I);
  // Every name the image exports a function under finds it: a linker that
  // folds identical functions exports several names for one entry.
  for (const Export &Exp : S->Img.Exports)
    if (Exp.Name == Name)
      for (size_t I = 0; I < S->Functions.size(); ++I)
        if (S->Functions[I].Entry == Exp.Addr)
          return static_cast<int>(I);
  return -1;
}

int neverd_func_find_by_addr(neverd_session_t Sess, neverd_va_t Addr) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded || !S->synchronizeFunctions())
    return -1;
  for (size_t I = 0; I < S->Functions.size(); ++I)
    if (S->Functions[I].Entry == Addr)
      return static_cast<int>(I);
  return -1;
}

// ===--------------------------------------------------------------------===//
// Raw bytes
// ===--------------------------------------------------------------------===//

int neverd_read_bytes(neverd_session_t Sess, neverd_va_t Addr,
                      unsigned char *Buf, int Size) {
  auto *S = toSession(Sess);
  if (!S->Loaded || !Buf || Size <= 0)
    return 0;

  int BytesRead = 0;
  va_t Cur = Addr;
  int Remaining = Size;

  while (Remaining > 0) {
    const Segment *Seg = S->Img.getSegmentFor(Cur);
    if (!Seg)
      break;

    uint64_t OffInSeg64 = Cur - Seg->VA;
    if (OffInSeg64 >= Seg->Data.size())
      break;

    size_t OffInSeg = static_cast<size_t>(OffInSeg64);
    uint64_t VirtualAvail = Seg->Size - OffInSeg64;
    uint64_t MaterializedAvail = Seg->Data.size() - OffInSeg;
    size_t Chunk = static_cast<size_t>(
        std::min<uint64_t>(static_cast<uint64_t>(Remaining),
                           std::min(VirtualAvail, MaterializedAvail)));
    if (Chunk == 0)
      break;

    std::memcpy(Buf + BytesRead, Seg->Data.data() + OffInSeg, Chunk);
    BytesRead += static_cast<int>(Chunk);
    Cur += Chunk;
    Remaining -= static_cast<int>(Chunk);
  }
  return BytesRead;
}

// ===--------------------------------------------------------------------===//
// Error handling + memory management
// ===--------------------------------------------------------------------===//

const char *neverd_last_error(neverd_session_t Sess) {
  return dupStr(toSession(Sess)->LastError);
}

void neverd_free_string(const char *Str) { free(const_cast<char *>(Str)); }

// ===--------------------------------------------------------------------===//
// Session metadata
// ===--------------------------------------------------------------------===//

unsigned long long neverd_session_file_size(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S->inputFileSize();
}

neverd_va_t neverd_session_base_addr(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S->Loaded ? S->Img.Base : 0;
}

neverd_va_t neverd_session_entry_addr(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S->Loaded ? S->Img.Entry : 0;
}

const char *neverd_session_load_diagnostics_json(neverd_session_t Sess) {
  llvm::json::Array Diagnostics;
  const auto *S = toSession(Sess);
  if (S && S->Loaded)
    for (const auto &D : S->Img.LoadDiagnostics)
      Diagnostics.push_back(
          llvm::json::Object{{"code", D.Code}, {"message", D.Message}});
  return dupStr(jsonToString(llvm::json::Value(std::move(Diagnostics))));
}

int neverd_session_segment_count(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S->Loaded ? static_cast<int>(S->Img.Segments.size()) : 0;
}

int neverd_session_section_count(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S->Loaded ? static_cast<int>(S->Img.Sections.size()) : 0;
}

int neverd_session_import_count(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S->Loaded ? static_cast<int>(S->Img.Imports.size()) : 0;
}

int neverd_session_export_count(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S->Loaded ? static_cast<int>(S->Img.Exports.size()) : 0;
}

int neverd_session_symbol_count(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S->Loaded ? static_cast<int>(S->Img.Symbols.size()) : 0;
}

// ===--------------------------------------------------------------------===//
// Plugin management
// ===--------------------------------------------------------------------===//

int neverd_plugins_load_dir(neverd_session_t Sess, const char *Dir) {
  auto *S = toSession(Sess);
  if (!S || !Dir) {
    if (S)
      S->setError("plugin directory path is null");
    return 0;
  }
  S->clearError();
  const int Loaded = S->PM.loadPluginsFromDir(Dir);
  if (!S->PM.lastError().empty())
    S->setError(S->PM.lastError());
  return Loaded;
}

int neverd_plugins_load_file(neverd_session_t Sess, const char *Path) {
  auto *S = toSession(Sess);
  if (!S || !Path) {
    if (S)
      S->setError("plugin file path is null");
    return 0;
  }
  S->clearError();
  if (!S->PM.loadPluginFile(Path)) {
    S->setError(S->PM.lastError());
    return 0;
  }
  return 1;
}

const char *neverd_plugins_list_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  llvm::json::Array Arr;
  for (const auto &P : S->PM.plugins()) {
    const neverd_plugin_t &Descriptor = P.descriptor();
    llvm::json::Object Obj;
    Obj["name"] = Descriptor.Name ? Descriptor.Name : "";
    Obj["version"] = Descriptor.Version ? Descriptor.Version : "";
    Obj["author"] = Descriptor.Author ? Descriptor.Author : "";
    Obj["description"] = Descriptor.Description ? Descriptor.Description : "";
    Obj["type"] = static_cast<int64_t>(Descriptor.Type);
    Obj["kind"] = P.Runtime->kind();
    Obj["path"] = P.Path;
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

void neverd_plugins_init(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return;
  S->clearError();
  S->PM.initAll(Sess);
  if (!S->PM.lastError().empty())
    S->setError(S->PM.lastError());
}

void neverd_plugins_term(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return;
  S->clearError();
  S->PM.termAll();
  if (!S->PM.lastError().empty())
    S->setError(S->PM.lastError());
}

int neverd_plugins_run(neverd_session_t Sess, const char *Name, int Arg) {
  auto *S = toSession(Sess);
  if (!S || !Name)
    return -1;
  S->clearError();
  const int Result = S->PM.runPlugin(Name, Sess, Arg);
  if (Result == -1 && !S->PM.lastError().empty())
    S->setError(S->PM.lastError());
  return Result;
}

int neverd_plugins_count(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S ? static_cast<int>(S->PM.plugins().size()) : 0;
}

void neverd_plugins_dispatch_event(neverd_session_t Sess, const void *Event) {
  if (!Event)
    return;
  auto *S = toSession(Sess);
  if (!S)
    return;
  S->clearError();
  S->PM.dispatchEvent(*static_cast<const neverd_event_t *>(Event));
  if (!S->PM.lastError().empty())
    S->setError(S->PM.lastError());
}

// ===--------------------------------------------------------------------===//
// Version info
// ===--------------------------------------------------------------------===//

const char *neverd_version(void) {
  return dupStr(std::string(ProjectName) + " v" + VersionString);
}

const char *neverd_project_name(void) { return dupStr(ProjectName); }

const char *neverd_version_number(void) { return dupStr(VersionString); }
