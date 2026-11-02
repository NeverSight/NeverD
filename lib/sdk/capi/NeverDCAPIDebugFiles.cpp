//===- NeverDCAPIDebugFiles.cpp - C API: debug files loaded by choice -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The linker MAP beside an input, before it loads, and the MAP, DWARF or PDB
/// a user loads into an open image.  A file loaded after the image reads the
/// image again and publishes every debug file in the order their names rank,
/// so the result is the one a load that named the file would have given.
///
//===----------------------------------------------------------------------===//

#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/debug/DebugInfoDiscovery.h"
#include "neverd/debug/MapFile.h"
#include "neverd/evm/bytecode/EVMBytecode.h"
#include "neverd/loader/ELF/SBFELFLoader.h"
#include "neverd/loader/ObjectFileUtils.h"
#include "neverd/sdk/Limits.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/support/FilePath.h"

#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <filesystem>
#include <string>

using namespace neverd;
using namespace neverd::sdk;

namespace {

std::string stampHex(uint32_t Stamp) {
  std::string Text;
  llvm::raw_string_ostream(Text) << llvm::format_hex_no_prefix(Stamp, 8);
  return Text;
}

llvm::json::Object placedJson(const MapSection &Placed) {
  return llvm::json::Object{{"address", vaHex(Placed.Address)},
                            {"size", vaHex(Placed.Size)}};
}

/// What the MAP at \p Path, read against \p Layout, names and how it holds:
/// {"path","dialect","functions","data","outside","match","reason"?,
/// "stamp"?:{"map","image"},"section"?:{"segment","name","map","image"},
/// "names"}.
llvm::json::Object mapJson(const std::filesystem::path &Path,
                           const MapRead &Read, const MapImageLayout &Layout) {
  const MapCheck &Check = Read.Check;
  llvm::json::Array Names;
  if (Read.Context)
    for (const FunctionSym &Function : Read.Context->allFunctions()) {
      if (Names.size() == kDebugFileSampleNames)
        break;
      Names.push_back(jsonSafeText(Function.Name));
    }
  std::error_code EC;
  llvm::json::Object Answer{
      {"path", jsonSafeText(pathToUTF8(std::filesystem::absolute(Path, EC)))},
      {"dialect", mapDialectName(Read.Facts.Dialect)},
      {"functions", static_cast<int64_t>(Check.Functions)},
      {"data", static_cast<int64_t>(Check.DataNames)},
      {"outside", static_cast<int64_t>(Check.Outside)},
      {"match", mapMatchName(Check.Match)},
      {"names", std::move(Names)}};
  if (Check.Match == MapMatch::Mismatch)
    Answer["reason"] = mapMismatchName(Check.Reason);
  if (Read.Facts.Stamp)
    Answer["stamp"] = llvm::json::Object{
        {"map", stampHex(*Read.Facts.Stamp)},
        {"image", Layout.Stamp ? llvm::json::Value(stampHex(*Layout.Stamp))
                               : llvm::json::Value(nullptr)}};
  if (Check.Reason == MapMismatch::Section)
    Answer["section"] =
        llvm::json::Object{{"segment", jsonSafeText(Check.MapPlaced.Segment)},
                           {"name", jsonSafeText(Check.MapPlaced.Name)},
                           {"map", placedJson(Check.MapPlaced)},
                           {"image", placedJson(Check.ImagePlaced)}};
  return Answer;
}

/// The size and time of \p Path, to tell a file that changed while it was
/// read.
struct FileStamp {
  uintmax_t Size = 0;
  std::filesystem::file_time_type Time;
  bool operator==(const FileStamp &) const = default;
};

std::optional<FileStamp> fileStamp(const std::filesystem::path &Path) {
  std::error_code EC;
  FileStamp Stamp;
  Stamp.Size = std::filesystem::file_size(Path, EC);
  if (EC)
    return std::nullopt;
  Stamp.Time = std::filesystem::last_write_time(Path, EC);
  if (EC)
    return std::nullopt;
  return Stamp;
}

/// Load the debug file \p Path into \p S as \p Request names it, refusing a
/// file that changes while it is read.  \p What names the file for errors.
int loadChosenFile(Session &S, const std::filesystem::path &Path,
                   DebugInfoRequest Request, llvm::StringRef What) {
  const auto Before = fileStamp(Path);
  if (!Before) {
    S.setError(("Cannot read the selected " + What + ": ").str() +
               pathToUTF8(Path));
    return 0;
  }
  Request.Enabled = true;
  return reloadDebugInformation(S, std::move(Request), [&] {
    if (fileStamp(Path) == Before)
      return true;
    S.setError(
        ("The selected " + What + " changed while loading; select it again")
            .str());
    return false;
  });
}

} // namespace

int neverd::sdk::reloadDebugInformation(Session &S, DebugInfoRequest Request,
                                        llvm::function_ref<bool()> Unchanged) {
  if (!S.Loaded) {
    S.setError("no binary loaded");
    return 0;
  }
  if (!S.ReadFromFile) {
    S.setError("This image was not read from a file, so no debug file can "
               "be loaded into it");
    return 0;
  }
  // The image as the load read it, before any debug file named it.
  BinaryLoadOptions Options;
  Options.OnlyFunctionEntries = S.OnlyFunctionEntries;
  Options.ARMFunctionModes = S.ARMFunctionModes;
  Options.ReadStaticSymbols = S.LoadedStaticSymbols;
  Options.Choice = S.LoadedChoice;
  auto Read = loadBinary(S.FilePath, Options);
  if (!Read) {
    S.setError("Cannot read " + pathToUTF8(S.FilePath) +
               " again: " + llvm::toString(Read.takeError()));
    return 0;
  }
  BinaryImage Image = std::move(*Read);
  settleBinaryFilePlatform(Image, Options.Choice);
  if (Image.Raw != S.Img.Raw) {
    S.setError("The input changed since it was loaded; reload it first");
    return 0;
  }
  auto Found = loadDebugInfo(S.FilePath, Image, Request, S.LoadProgressCb);
  if (!Found.Error.empty()) {
    S.setError(Found.Error);
    return 0;
  }
  applyDebugInfo(Image, Found);
  // The analysis sections are the session's, not the file's: the image read
  // again takes them as an edit adds them, or the session keeps its image.
  auto Kept = withAnalysisSections(S, std::move(Image));
  if (!Kept) {
    S.setError(llvm::toString(Kept.takeError()));
    return 0;
  }
  Image = std::move(*Kept);
  if (Unchanged && !Unchanged())
    return 0;
  if (Image.Arch != Arch::EVM && Image.Arch != Arch::SBF &&
      !S.Dec.init(Image)) {
    S.setError("Failed to initialize the decoder for the image");
    return 0;
  }
  // Retire analysis while its old image and debug owners still exist.
  S.clearPipeline();
  S.ImmediateIndex = {};
  S.Img = std::move(Image);
  S.ExceptionHandlersSettled = false;
  S.applyUserNoReturn();
  S.Dbg = std::move(Found.Context);
  S.DbgKind = Found.Kind;
  S.DbgPath = std::move(Found.Path);
  S.DbgFiles = std::move(Found.Files);
  S.DbgRequest = std::move(Request);
  S.LoadedDbgRequest = S.DbgRequest;
  S.dropLocalTypes();
  S.DiscoveredFunctions.reset();
  S.ProgramMainFound.reset();
  S.ImageStrings.reset();
  // Signature matches name addresses, which the image keeps; the routines
  // frames install as their personality are adopted into it again.
  if (S.SigDB.fileCount() != 0 || !S.SigDB.matches().empty())
    S.SigDB.identifyPersonalityRoutines(S.Img);
  S.resetFunctionsFromImage();
  return 1;
}

const char *neverd_map_reference_json(const char *Path) {
  llvm::json::Object Root;
  const auto Answer = [&] {
    return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
  };
  Root["map"] = nullptr;
  const auto Input = std::filesystem::u8path(Path ? Path : "");
  auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(Input), /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (!Buffer) {
    Root["error"] = jsonSafeText("cannot read " + pathToUTF8(Input) + ": " +
                                 Buffer.getError().message());
    return Answer();
  }
  // EVM and SBF programs describe themselves by their own metadata, and a
  // load reads no MAP for them.
  const llvm::MemoryBufferRef Bytes = (*Buffer)->getMemBufferRef();
  const BinaryFormat Format =
      magicToFormat(llvm::identify_magic(Bytes.getBuffer()));
  bool SupportedSBF = false;
  if (Format == BinaryFormat::ELF &&
      isSBFELF(llvm::arrayRefFromStringRef(Bytes.getBuffer()), SupportedSBF) &&
      !searchesDebugInfo(Arch::SBF))
    return Answer();
  if (Format == BinaryFormat::Unknown &&
      evm::matchEVMInput(Input) == evm::EVMInputMatch::Bytecode &&
      !searchesDebugInfo(Arch::EVM))
    return Answer();
  const auto Map = findMap(Input);
  if (!Map)
    return Answer();
  // A binary file has no headers to hold the MAP against before it loads.
  MapImageLayout Layout;
  if (auto Read = readMapImageLayout(Bytes))
    Layout = std::move(*Read);
  else
    llvm::consumeError(Read.takeError());
  const MapRead Read = readMapFile(*Map, Layout);
  // A MAP that names nothing is one a load passes over.
  if (!Read.Context)
    return Answer();
  Root["map"] = mapJson(*Map, Read, Layout);
  return Answer();
}

const char *neverd_session_inspect_map_json(neverd_session_t Sess,
                                            const char *MapPath) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded || !MapPath || !*MapPath) {
    S->setError("Load an image and select a MAP file first");
    return nullptr;
  }
  const auto Path = std::filesystem::u8path(MapPath);
  const MapImageLayout Layout = mapImageLayout(S->Img, S->FilePath);
  const MapRead Read = readMapFile(Path, Layout);
  if (!Read.Error.empty()) {
    S->setError(Read.Error);
    return nullptr;
  }
  if (!Read.Context) {
    S->setError("No function names in the selected MAP file");
    return nullptr;
  }
  return dupStr(jsonToString(mapJson(Path, Read, Layout)));
}

int neverd_session_load_map(neverd_session_t Sess, const char *Path,
                            int Unchecked) {
  auto *S = toSession(Sess);
  if (!S)
    return 0;
  S->clearError();
  if (!S->Loaded || !Path || !*Path) {
    S->setError("Load an image and select a MAP file first");
    return 0;
  }
  if (Unchecked != 0 && Unchecked != 1) {
    S->setError("Unchecked must be 0 or 1");
    return 0;
  }
  const auto Chosen = std::filesystem::u8path(Path);
  DebugInfoRequest Request = S->DbgRequest;
  Request.MapPath = Chosen;
  Request.UncheckedMap = Unchecked == 1;
  return loadChosenFile(*S, Chosen, std::move(Request), "MAP file");
}

int neverd_session_load_dwarf(neverd_session_t Sess, const char *Path) {
  auto *S = toSession(Sess);
  if (!S)
    return 0;
  S->clearError();
  if (!S->Loaded || !Path || !*Path) {
    S->setError("Load an image and select a DWARF file first");
    return 0;
  }
  const auto Chosen = std::filesystem::u8path(Path);
  DebugInfoRequest Request = S->DbgRequest;
  Request.DWARFPath = Chosen;
  return loadChosenFile(*S, Chosen, std::move(Request), "DWARF file");
}

llvm::json::Array neverd::sdk::debugInfoFilesJson(const Session &S) {
  llvm::json::Array Files;
  if (!S.Loaded)
    return Files;
  for (const DebugInfoFile &File : S.DbgFiles) {
    llvm::json::Object Entry{{"kind", debugInfoKindName(File.Kind)},
                             {"path", jsonSafeText(pathToUTF8(File.Path))},
                             {"names", static_cast<int64_t>(File.Applied)}};
    if (File.Kind == DebugInfoKind::Map) {
      Entry["dialect"] = mapDialectName(File.Dialect);
      Entry["match"] = mapMatchName(File.Check.Match);
      Entry["functions"] = static_cast<int64_t>(File.Check.Functions);
    }
    Files.push_back(std::move(Entry));
  }
  return Files;
}

const char *neverd_session_debug_info_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return dupStr(jsonToString(llvm::json::Object{
      {"enabled", S ? S->DbgRequest.Enabled : true},
      {"kind", debugInfoKindName(S ? S->DbgKind : DebugInfoKind::None)},
      {"files", S ? debugInfoFilesJson(*S) : llvm::json::Array()}}));
}

void neverd_session_set_map_path(neverd_session_t Sess, const char *Path) {
  (void)neverd_session_set_map_path_ex(Sess, Path, 0);
}

int neverd_session_set_map_path_ex(neverd_session_t Sess, const char *Path,
                                   int Unchecked) {
  auto *S = toSession(Sess);
  if (!S)
    return 0;
  S->clearError();
  if (Unchecked != 0 && Unchecked != 1) {
    S->setError("Unchecked must be 0 or 1");
    return 0;
  }
  S->DbgRequest.MapPath = Path ? std::filesystem::u8path(Path) : "";
  // Only a named MAP carries the user's choice; one beside the binary that
  // contradicts the image is never read.
  S->DbgRequest.UncheckedMap = Unchecked == 1 && !S->DbgRequest.MapPath.empty();
  return 1;
}

void neverd_session_set_map_search(neverd_session_t Sess, int Enabled) {
  if (auto *S = toSession(Sess))
    S->DbgRequest.SearchMap = Enabled != 0;
}

void neverd_session_set_dwarf_path(neverd_session_t Sess, const char *Path) {
  if (auto *S = toSession(Sess))
    S->DbgRequest.DWARFPath = Path ? std::filesystem::u8path(Path) : "";
}
