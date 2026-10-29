//===- DebugInfoDiscovery.cpp - Locate and load debug info ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Chooses which debug format applies to an image, finds the file that carries
/// it, and publishes the resulting names into the image's symbol table.
///
//===----------------------------------------------------------------------===//

#include "neverd/debug/DebugInfoDiscovery.h"

#include "neverd/Common.h"
#include "neverd/debug/DWARFLoader.h"
#include "neverd/debug/DebugFileIdentity.h"
#include "neverd/debug/MapFile.h"
#include "neverd/debug/PDBLoader.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/loader/StringLiteralSymbols.h"
#include "neverd/support/FilePath.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CRC.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <span>
#include <system_error>

#define DEBUG_TYPE "neverd-debug-discovery"

namespace neverd {

namespace {

/// Record as pointers to data the relocated pointers that the image took for
/// pointers to code but whose targets \p Dbg places in data: at a data
/// symbol, the image's own among them (\p DataByAddr), or after one that no
/// function follows.
void recordDebugDataPointerTargets(BinaryImage &Img, const DebugContext &Dbg,
                                   const std::map<va_t, size_t> &DataByAddr) {
  recordDataPointerTargets(Img, [&](va_t Addr) {
    if (DataByAddr.count(Addr) != 0)
      return true;
    return (Dbg.resolveDataObject(Addr) || Dbg.precedingDataSymbol(Addr)) &&
           !Dbg.resolveFunction(Addr);
  });
}

/// Drop the functions the loader only guessed after padding where \p Dbg
/// names a data object, the image's own data symbols (\p DataByAddr) among
/// them: a string literal that an executable section keeps begins no
/// function, however its first bytes read as a prologue.
void dropDebugDataBoundaryGuesses(BinaryImage &Img, const DebugContext &Dbg,
                                  const std::map<va_t, size_t> &DataByAddr) {
  const auto IsGuess = [](const Symbol &Sym) {
    return Sym.IsFunc && Sym.IsBoundaryGuess && isSynthesizedFuncName(Sym.Name);
  };
  // What else says a function begins there, which the data does not refute.
  std::set<va_t> Confirmed;
  for (const Symbol &Sym : Img.Symbols)
    if (Sym.IsFunc && !IsGuess(Sym) && !Img.isFunctionContinuationAt(Sym.Addr))
      Confirmed.insert(Sym.Addr);
  for (const auto &[Start, End] : Img.KnownCodeRanges)
    if (Img.hasKnownFunctionEntryAt(Start))
      Confirmed.insert(Start);
  std::set<va_t> Refuted;
  for (const Symbol &Sym : Img.Symbols)
    if (IsGuess(Sym) && !Confirmed.count(Sym.Addr) &&
        (DataByAddr.count(Sym.Addr) != 0 ||
         (Dbg.resolveDataObject(Sym.Addr) && !Dbg.resolveFunction(Sym.Addr))))
      Refuted.insert(Sym.Addr);
  if (Refuted.empty())
    return;
  llvm::erase_if(Img.Symbols, [&](const Symbol &Sym) {
    return IsGuess(Sym) && Refuted.count(Sym.Addr) != 0;
  });
}

bool isReadableFile(const std::filesystem::path &P) {
  if (P.empty())
    return false;
  std::error_code EC;
  return std::filesystem::is_regular_file(P, EC);
}

void addCandidate(std::vector<std::filesystem::path> &Out,
                  std::filesystem::path Candidate) {
  if (Candidate.empty())
    return;
  if (std::find(Out.begin(), Out.end(), Candidate) == Out.end())
    Out.push_back(std::move(Candidate));
}

/// Companion files that hold the DWARF stripped out of an ELF image, in the
/// layouts objcopy --only-keep-debug and the distro debug packages produce.
std::vector<std::filesystem::path>
splitDwarfCandidates(const std::filesystem::path &BinaryPath) {
  std::vector<std::filesystem::path> Out;
  const std::filesystem::path Dir = BinaryPath.parent_path();
  const auto Name = BinaryPath.filename();

  auto Companion = BinaryPath;
  Companion += ".debug";
  addCandidate(Out, std::move(Companion));
  Companion = BinaryPath;
  Companion.replace_extension(".debug");
  addCandidate(Out, std::move(Companion));
  addCandidate(Out, Dir / ".debug" / Name);
  return Out;
}

/// The environment variable whose directories replace the default ones.
#define NEVERD_DEBUG_FILE_DIRECTORY_VARIABLE(Name)                             \
  constexpr char DebugFileDirectoryVariable[] = Name;
#include "neverd/debug/DWARFLocation.def"

/// The directories companion files are searched under by build ID and debug
/// link: the ones the environment lists, else the defaults
/// (DWARFLocation.def).
std::vector<std::filesystem::path> debugFileDirectories() {
  if (const char *Listed = std::getenv(DebugFileDirectoryVariable)) {
    llvm::SmallVector<llvm::StringRef, 4> Parts;
    llvm::StringRef(Listed).split(Parts, llvm::sys::EnvPathSeparator, -1,
                                  /*KeepEmpty=*/false);
    std::vector<std::filesystem::path> Listing;
    for (llvm::StringRef Part : Parts)
      Listing.push_back(std::filesystem::u8path(Part.str()));
    return Listing;
  }
  return {
#define NEVERD_DEBUG_FILE_DIRECTORY(Path) std::filesystem::u8path(Path),
#include "neverd/debug/DWARFLocation.def"
  };
}

/// A file that may hold an image's DWARF, and how it was found.
struct DWARFCandidate {
  std::filesystem::path Path;
  DWARFLocation Location = DWARFLocation::Beside;
};

/// What an ELF image records of its companion: its build ID and debug link.
struct CompanionIdentity {
  std::vector<uint8_t> BuildID;
  std::optional<DebugLink> Link;
};

CompanionIdentity companionIdentity(const std::filesystem::path &BinaryPath) {
  CompanionIdentity Identity;
  auto Opened = openObjectFile(BinaryPath);
  if (!Opened) {
    llvm::consumeError(Opened.takeError());
    return Identity;
  }
  if (!Opened->Object)
    return Identity;
  Identity.BuildID = recordedBuildIdentity(*Opened->Object);
  Identity.Link = elfDebugLink(*Opened->Object);
  return Identity;
}

/// The companion files of the ELF image at \p BinaryPath in the order GDB
/// tries them: by build ID under each debug directory, the file the debug
/// link names beside the image, in .debug/ beside it and under each debug
/// directory, then the files beside it named after it.
std::vector<DWARFCandidate>
companionCandidates(const std::filesystem::path &BinaryPath,
                    const CompanionIdentity &Identity) {
  std::vector<DWARFCandidate> Out;
  const auto add = [&](std::filesystem::path Path, DWARFLocation Location) {
    for (const DWARFCandidate &Known : Out)
      if (Known.Path == Path)
        return;
    Out.push_back({std::move(Path), Location});
  };
  const std::vector<std::filesystem::path> Directories = debugFileDirectories();
  if (Identity.BuildID.size() >= 2) {
    const std::string Hex = buildIdentityHex(Identity.BuildID);
    for (const std::filesystem::path &Directory : Directories)
      add(Directory / ".build-id" / Hex.substr(0, 2) /
              (Hex.substr(2) + ".debug"),
          DWARFLocation::BuildID);
  }
  if (Identity.Link) {
    const std::filesystem::path Name =
        std::filesystem::u8path(Identity.Link->FileName);
    std::error_code EC;
    const std::filesystem::path Dir =
        std::filesystem::absolute(BinaryPath, EC).parent_path();
    add(Dir / Name, DWARFLocation::DebugLink);
    add(Dir / ".debug" / Name, DWARFLocation::DebugLink);
    for (const std::filesystem::path &Directory : Directories)
      add(Directory / Dir.relative_path() / Name, DWARFLocation::DebugLink);
  }
  for (std::filesystem::path &Path : splitDwarfCandidates(BinaryPath))
    add(std::move(Path), DWARFLocation::Beside);
  return Out;
}

/// Whether \p Candidate can be the companion \p Identity describes: a build
/// ID both record agrees, and a file found by the debug link of an image
/// without a build ID has the CRC the link records.
bool companionMatches(const DWARFCandidate &Candidate,
                      const CompanionIdentity &Identity) {
  if (!isReadableFile(Candidate.Path))
    return false;
  auto Opened = openObjectFile(Candidate.Path);
  if (!Opened) {
    llvm::consumeError(Opened.takeError());
    return false;
  }
  if (!Opened->Object)
    return false;
  const std::vector<uint8_t> BuildID = recordedBuildIdentity(*Opened->Object);
  if (!Identity.BuildID.empty() && !BuildID.empty())
    return BuildID == Identity.BuildID;
  if (Candidate.Location == DWARFLocation::BuildID)
    return false;
  if (Candidate.Location == DWARFLocation::DebugLink && Identity.Link)
    return llvm::crc32(llvm::arrayRefFromStringRef(
               Opened->Buffer->getBuffer())) == Identity.Link->CRC;
  return true;
}

/// The companion files of the ELF image at \p BinaryPath that may hold its
/// DWARF, in the order they are tried.
std::vector<DWARFCandidate>
matchingCompanions(const std::filesystem::path &BinaryPath) {
  const CompanionIdentity Identity = companionIdentity(BinaryPath);
  std::vector<DWARFCandidate> Out;
  for (DWARFCandidate &Candidate : companionCandidates(BinaryPath, Identity)) {
    if (companionMatches(Candidate, Identity)) {
      Out.push_back(std::move(Candidate));
      continue;
    }
    LLVM_DEBUG(llvm::dbgs() << "debug-discovery: passed over companion "
                            << pathToUTF8(Candidate.Path) << "\n");
  }
  return Out;
}

std::vector<std::filesystem::path>
mapCandidates(const std::filesystem::path &BinaryPath) {
  std::vector<std::filesystem::path> Out;
  const std::filesystem::path Dir = BinaryPath.parent_path();

  auto Companion = BinaryPath;
  Companion.replace_extension(".map");
  addCandidate(Out, std::move(Companion));
  Companion = BinaryPath;
  Companion += ".map";
  addCandidate(Out, std::move(Companion));
  return Out;
}

DebugInfoResult loadPDB(const std::filesystem::path &P, const BinaryImage &Img,
                        const LoadProgress &Progress,
                        bool AllowMissingIdentity = false) {
  DebugInfoResult R;
  auto CtxOr = AllowMissingIdentity
                   ? PDBDebugContext::loadUserSelected(P, Img, Progress)
                   : PDBDebugContext::load(P, Img, Progress);
  if (!CtxOr) {
    R.Error = llvm::toString(CtxOr.takeError());
    return R;
  }
  auto Ctx = std::move(*CtxOr);
  if (Ctx && Ctx->hasInfo()) {
    R.Context = std::move(Ctx);
    R.Kind = DebugInfoKind::PDB;
    R.Path = P;
  } else {
    R.Error = "no function symbols in " + pathToUTF8(P);
  }
  return R;
}

DebugInfoResult
loadDWARF(const std::filesystem::path &P, BinaryFormat Format,
          DWARFLoadTrust Trust,
          std::span<const uint8_t> ExpectedImageBytes = {},
          std::optional<llvm::Triple::ArchType> RequiredArch = std::nullopt) {
  DebugInfoResult R;
  auto Ctx = DWARFDebugContext::load(P, Format, Trust, ExpectedImageBytes,
                                     RequiredArch);
  if (Ctx && Ctx->hasInfo()) {
    R.Context = std::move(Ctx);
    R.Kind = DebugInfoKind::DWARF;
    R.Path = P;
  }
  return R;
}

/// Read the MAP at \p P against the image at \p BinaryPath.
MapRead readMap(const std::filesystem::path &P, const BinaryImage &Img,
                const std::filesystem::path &BinaryPath) {
  return readMapFile(P, mapImageLayout(Img, BinaryPath));
}

/// Take \p Map, read from \p P, into \p Result: as its context when it has
/// none, else as the supplement whose names fill what the context leaves.
void adoptMap(MapRead Map, const std::filesystem::path &P,
              DebugInfoResult &Result) {
  DebugInfoFile File;
  File.Kind = DebugInfoKind::Map;
  File.Path = P;
  File.Dialect = Map.Facts.Dialect;
  File.Check = Map.Check;
  Result.Files.push_back(std::move(File));
  if (Result) {
    Result.Supplement = std::move(Map.Context);
    return;
  }
  Result.Context = std::move(Map.Context);
  Result.Kind = DebugInfoKind::Map;
  Result.Path = P;
}

/// Load the DWARF file \p Named the caller chose for \p Img: a companion
/// file, an unstripped copy of the image, or a .dSYM bundle.
DebugInfoResult loadNamedDWARF(const std::filesystem::path &Named,
                               const std::filesystem::path &BinaryPath,
                               const BinaryImage &Img) {
  DebugInfoResult R;
  const std::string Shown = pathToUTF8(Named);
  if (Img.Format != BinaryFormat::ELF && Img.Format != BinaryFormat::MachO) {
    R.Error = "DWARF files describe ELF and Mach-O images; " +
              pathToUTF8(BinaryPath.filename()) + " is neither";
    return R;
  }
  std::filesystem::path File = Named;
  std::error_code EC;
  if (std::filesystem::is_directory(Named, EC)) {
    File = dSYMBundleFile(Named);
    if (File.empty()) {
      R.Error = "no DWARF file in the bundle " + Shown;
      return R;
    }
  }
  if (!isReadableFile(File)) {
    R.Error = "DWARF file not found: " + Shown;
    return R;
  }
  auto Companion = openObjectFile(File, Img.Arch);
  if (!Companion) {
    R.Error = "cannot read " + pathToUTF8(File) + ": " +
              llvm::toString(Companion.takeError());
    return R;
  }
  if (!Companion->Object ||
      !isTripleArchOf(Companion->Object->getArch(), Img.Arch)) {
    R.Error =
        pathToUTF8(File) + " describes no " + getArchName(Img.Arch) + " image";
    return R;
  }
  // The image's own record of its build, from the bytes it was loaded from.
  std::vector<uint8_t> Expected;
  const llvm::MemoryBufferRef Bytes(
      llvm::StringRef(reinterpret_cast<const char *>(Img.Raw.data()),
                      Img.Raw.size()),
      "image");
  if (auto Image = llvm::object::ObjectFile::createObjectFile(Bytes))
    Expected = recordedBuildIdentity(**Image);
  else
    llvm::consumeError(Image.takeError());
  const std::vector<uint8_t> Recorded =
      recordedBuildIdentity(*Companion->Object);
  if (!Expected.empty() && !Recorded.empty() && Expected != Recorded) {
    const char *What = Img.Format == BinaryFormat::MachO ? "UUID" : "build ID";
    R.Error = pathToUTF8(File) +
              " is the debug information of another build: " + "its " + What +
              " is " + buildIdentityHex(Recorded) + ", the image's " +
              buildIdentityHex(Expected);
    return R;
  }
  R = loadDWARF(File, Img.Format, DWARFLoadTrust::UnauthenticatedCompanion, {},
                Companion->Object->getArch());
  if (!R)
    R.Error = "no DWARF debug information in " + pathToUTF8(File);
  return R;
}

/// The PDB or DWARF discovery finds for \p Img, as \p Req allows.
DebugInfoResult discoverNativeDebugInfo(const std::filesystem::path &BinaryPath,
                                        const BinaryImage &Img,
                                        const DebugInfoRequest &Req,
                                        const LoadProgress &Progress) {
  DebugInfoResult Result;
  switch (Img.Format) {
  case BinaryFormat::COFF:
    if (!Req.SearchPDB)
      break;
    for (const PDBCandidate &C :
         pdbCandidates(BinaryPath, Img, symbolPath(Req.SymbolPath))) {
      if (!isReadableFile(C.Path))
        continue;
      DebugInfoResult Attempt = loadPDB(C.Path, Img, Progress);
      if (Attempt)
        return Attempt;
      LLVM_DEBUG(llvm::dbgs()
                 << "debug-discovery: rejected PDB candidate "
                 << pathToUTF8(C.Path) << (Attempt.Error.empty() ? "" : ": ")
                 << Attempt.Error << "\n");
    }
    break;

  case BinaryFormat::ELF:
  case BinaryFormat::MachO:
    if (!Req.SearchDWARF)
      break;
    // Covers in-image .debug_info and, for Mach-O, an adjacent .dSYM bundle,
    // in the order findDWARF reports.
    Result =
        loadDWARF(BinaryPath, Img.Format, DWARFLoadTrust::InImage, Img.Raw);
    if (Result)
      return Result;
    for (const DWARFCandidate &C : matchingCompanions(BinaryPath)) {
      // Split ELF/Mach-O candidates remain useful for names and lines, but
      // cannot publish object extents: a build ID or CRC that matches says
      // which build they are of, not that their extents are this image's.
      // dSYM discovery from the primary Mach-O is handled by DWARFDebugContext
      // and separately verifies LC_UUID.
      Result = loadDWARF(C.Path, Img.Format,
                         DWARFLoadTrust::UnauthenticatedCompanion);
      if (Result)
        return Result;
    }
    break;

  default:
    break;
  }
  return {};
}

} // anonymous namespace

const char *debugInfoKindName(DebugInfoKind Kind) {
  switch (Kind) {
  case DebugInfoKind::None:
    return "none";
  case DebugInfoKind::DWARF:
    return "dwarf";
  case DebugInfoKind::PDB:
    return "pdb";
  case DebugInfoKind::Map:
    return "map";
  }
  return "none";
}

bool searchesDebugInfo(Arch A) { return A != Arch::EVM && A != Arch::SBF; }

const char *dwarfLocationName(DWARFLocation Location) {
  switch (Location) {
#define NEVERD_DWARF_LOCATION(Name, Spelling)                                  \
  case DWARFLocation::Name:                                                    \
    return Spelling;
#include "neverd/debug/DWARFLocation.def"
  }
  return "input";
}

std::optional<DWARFSource> findDWARF(const std::filesystem::path &BinaryPath,
                                     BinaryFormat Format) {
  if (Format != BinaryFormat::ELF && Format != BinaryFormat::MachO)
    return std::nullopt;
  if (hasDWARFUnits(BinaryPath))
    return DWARFSource{BinaryPath, DWARFLocation::Input};
  // DWARFDebugContext::load turns to the bundle when the image has none.
  if (Format == BinaryFormat::MachO)
    if (auto Bundle = findDSYMFile(BinaryPath);
        !Bundle.empty() && hasDWARFUnits(Bundle))
      return DWARFSource{std::move(Bundle), DWARFLocation::DSYM};
  for (DWARFCandidate &C : matchingCompanions(BinaryPath))
    if (hasDWARFUnits(C.Path))
      return DWARFSource{std::move(C.Path), C.Location};
  return std::nullopt;
}

std::optional<std::filesystem::path>
findMap(const std::filesystem::path &BinaryPath) {
  for (std::filesystem::path &C : mapCandidates(BinaryPath))
    if (isReadableFile(C))
      return std::move(C);
  return std::nullopt;
}

std::string describeMapMismatch(const MapCheck &Check) {
  const auto placed = [](const MapSection &Section) {
    const std::string Name = Section.Segment.empty()
                                 ? Section.Name
                                 : Section.Segment + "," + Section.Name;
    return Name + " at 0x" + llvm::utohexstr(Section.Address) + " (0x" +
           llvm::utohexstr(Section.Size) + " bytes)";
  };
  switch (Check.Reason) {
  case MapMismatch::Stamp:
    return "it records the build stamp " +
           llvm::utohexstr(Check.MapStamp, /*LowerCase=*/true) +
           ", the image " +
           llvm::utohexstr(Check.ImageStamp, /*LowerCase=*/true);
  case MapMismatch::Section:
    return "it places " + placed(Check.MapPlaced) + ", the image has " +
           placed(Check.ImagePlaced);
  case MapMismatch::Outside:
    return std::to_string(Check.Outside) + " of its " +
           std::to_string(Check.Functions) +
           " functions lie outside the image's code";
  case MapMismatch::None:
    break;
  }
  return "it does not match the image";
}

static DebugInfoResult
loadDebugInfoImpl(const std::filesystem::path &BinaryPath,
                  const BinaryImage &Img, const DebugInfoRequest &Req,
                  const LoadProgress &Progress) {
  DebugInfoResult Result;
  if (!Req.Enabled)
    return Result;
  // The cache container and its companions describe many images. Only an
  // explicitly selected companion can belong to this projected Mach-O image.
  const bool Searches = searchesDebugInfo(Img.Arch) && !Img.MachOSharedCache;

  // The PDB or DWARF: the file the caller named, else the one discovery
  // finds.
  if (!Req.PDBPath.empty()) {
    if (!isReadableFile(Req.PDBPath)) {
      Result.Error = "PDB file not found: " + pathToUTF8(Req.PDBPath);
      return Result;
    }
    Result = loadPDB(Req.PDBPath, Img, Progress, Req.AllowMissingPDBIdentity);
    if (!Result) {
      if (Result.Error.empty())
        Result.Error = "no function symbols in " + pathToUTF8(Req.PDBPath);
      return Result;
    }
  } else if (!Req.DWARFPath.empty()) {
    Result = loadNamedDWARF(Req.DWARFPath, BinaryPath, Img);
    if (!Result.Error.empty())
      return Result;
  } else if (Searches) {
    Result = discoverNativeDebugInfo(BinaryPath, Img, Req, Progress);
  }
  if (Result)
    Result.Files.push_back({Result.Kind, Result.Path});

  // A MAP the caller named is read beside them.
  if (!Req.MapPath.empty()) {
    const std::string Shown = pathToUTF8(Req.MapPath);
    if (!isReadableFile(Req.MapPath)) {
      Result = {};
      Result.Error = "MAP file not found: " + Shown;
      return Result;
    }
    MapRead Map = readMap(Req.MapPath, Img, BinaryPath);
    if (!Map.Error.empty() || !Map.Context) {
      Result = {};
      Result.Error =
          Map.Error.empty() ? "no function symbols in " + Shown : Map.Error;
      return Result;
    }
    if (Map.Check.Match == MapMatch::Mismatch && !Req.UncheckedMap) {
      Result = {};
      Result.Error =
          Shown + " describes another build: " + describeMapMismatch(Map.Check);
      return Result;
    }
    adoptMap(std::move(Map), Req.MapPath, Result);
    return Result;
  }

  // A MAP carries names and addresses but no types, source lines, or build
  // identity, so the one beside the binary is the last resort: it exists for
  // a stripped binary whose build kept nothing else.  One that contradicts
  // the image belongs to another build.
  if (Result || !Searches || !Req.SearchMap)
    return Result;
  for (const std::filesystem::path &C : mapCandidates(BinaryPath)) {
    Progress.check();
    if (!isReadableFile(C))
      continue;
    MapRead Map = readMap(C, Img, BinaryPath);
    if (!Map.Context)
      continue;
    if (Map.Check.Match == MapMatch::Mismatch) {
      LLVM_DEBUG(llvm::dbgs()
                 << "debug-discovery: passed over MAP " << pathToUTF8(C) << ": "
                 << describeMapMismatch(Map.Check) << "\n");
      continue;
    }
    adoptMap(std::move(Map), C, Result);
    return Result;
  }

  return Result;
}

DebugInfoResult loadDebugInfo(const std::filesystem::path &BinaryPath,
                              const BinaryImage &Img,
                              const DebugInfoRequest &Req,
                              const LoadProgress &Progress) {
  Progress.report("debug", 0, 1);
  auto Result = loadDebugInfoImpl(BinaryPath, Img, Req, Progress);
  Progress.report("debug", 1, 1);
  return Result;
}

unsigned applyDebugSymbols(BinaryImage &Img, const DebugContext &Dbg) {
  const std::vector<FunctionSym> DbgFuncs = Dbg.allFunctions();
  if (Dbg.hasAuthenticatedObjectExtents()) {
    for (const DataObjectSym &Object : Dbg.allDataObjects()) {
      if (Object.Addr == InvalidVA || Object.Size == 0 ||
          Object.Size > InvalidVA - Object.Addr)
        continue;
      Img.ExactDataObjects.push_back(ExactDataObjectExtent{
          Object.Addr, Object.Size, ExactDataObjectEvidence::AuthenticatedDebug,
          Object.IsBuffer ? ExactDataObjectPrecision::TypedBuffer
                          : ExactDataObjectPrecision::TypedNonBuffer});
    }
  }

  // Names-only data publics (Phase A PDB / MAP) must still name image
  // objects.  Size 0 is intentional: extents stay unauthenticated.
  // Index once: a large MAP can carry tens of thousands of data publics.
  std::map<va_t, size_t> DataByAddr;
  for (size_t I = 0; I < Img.Symbols.size(); ++I) {
    if (!Img.Symbols[I].IsFunc)
      DataByAddr.try_emplace(Img.Symbols[I].Addr, I);
  }
  // The objects that may lie among code.  A literal, which its symbol names
  // by its text, reads as that text rather than as an object.
  std::vector<va_t> DataObjects;
  for (const DataObjectSym &Object : Dbg.allDataObjects()) {
    if (Object.Name.empty() || Object.Addr == InvalidVA)
      continue;
    if (!isStringLiteralName(Object.Name))
      DataObjects.push_back(Object.Addr);
    auto It = DataByAddr.find(Object.Addr);
    if (It != DataByAddr.end()) {
      Symbol &Existing = Img.Symbols[It->second];
      if (Existing.Name.empty() || Existing.Origin < NameOrigin::Stated) {
        Existing.Name = Object.Name;
        Existing.Origin = NameOrigin::Stated;
      }
      continue;
    }
    Symbol New;
    New.Name = Object.Name;
    New.Addr = Object.Addr;
    New.Size = Object.Size;
    New.IsFunc = false;
    DataByAddr.emplace(Object.Addr, Img.Symbols.size());
    Img.Symbols.push_back(std::move(New));
  }

  // A guessed function that a data object refutes claims no pointer to it
  // as one to code, so the guesses go first.
  if (DbgFuncs.empty()) {
    dropDebugDataBoundaryGuesses(Img, Dbg, DataByAddr);
    recordDebugDataPointerTargets(Img, Dbg, DataByAddr);
    recordDataInCode(Img, DataObjects);
    return 0;
  }

  // Index the function symbols once rather than scanning the table per debug
  // entry; both sides run to tens of thousands of entries on a real image.
  std::map<va_t, size_t> ByAddr;
  for (size_t I = 0; I < Img.Symbols.size(); ++I) {
    const Symbol &S = Img.Symbols[I];
    if (!S.IsFunc)
      continue;
    auto [It, Inserted] = ByAddr.try_emplace(S.Addr, I);
    // One address can carry both a stated name and a placeholder, when a
    // discovery pass minted `sub_` for code the symbol table also describes.
    // The stated one is what decides whether debug info may speak here.
    if (!Inserted && Img.Symbols[It->second].Origin < S.Origin)
      It->second = I;
  }

  unsigned Applied = 0;
  for (const FunctionSym &FS : DbgFuncs) {
    if (FS.Name.empty() || FS.Addr == 0)
      continue;

    auto It = ByAddr.find(FS.Addr);
    if (It == ByAddr.end()) {
      Symbol New;
      New.Name = FS.Name;
      New.Addr = FS.Addr;
      New.Size = FS.Size;
      New.IsFunc = true;
      Img.Symbols.push_back(std::move(New));
      ByAddr.emplace(FS.Addr, Img.Symbols.size() - 1);
      ++Applied;
      continue;
    }

    Symbol &Existing = Img.Symbols[It->second];
    if (Existing.Origin >= NameOrigin::Stated && !Existing.Name.empty())
      continue;
    Existing.Name = FS.Name;
    Existing.Origin = NameOrigin::Stated;
    if (Existing.Size == 0)
      Existing.Size = FS.Size;
    ++Applied;
  }

  dropDebugDataBoundaryGuesses(Img, Dbg, DataByAddr);
  recordDebugDataPointerTargets(Img, Dbg, DataByAddr);
  recordDataInCode(Img, DataObjects);
  // Static functions of several modules share names (ntoskrnl's PDB names
  // thirteen twice).
  Img.numberSharedFunctionNames();
  if (Applied && Img.Format == BinaryFormat::COFF)
    coff_loader::refreshExceptionHandlerNames(Img);
  LLVM_DEBUG(llvm::dbgs() << "debug-discovery: published " << Applied << " of "
                          << DbgFuncs.size() << " debug function symbols\n");
  return Applied;
}

unsigned applyDebugInfo(BinaryImage &Img, DebugInfoResult &Found) {
  unsigned Applied = 0;
  size_t File = 0;
  for (const DebugContext *Context :
       {Found.Context.get(), Found.Supplement.get()}) {
    if (!Context || !Context->hasInfo())
      continue;
    const unsigned Names = applyDebugSymbols(Img, *Context);
    if (File < Found.Files.size())
      Found.Files[File++].Applied = Names;
    Applied += Names;
  }
  return Applied;
}

} // namespace neverd
