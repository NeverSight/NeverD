//===- SignatureDB.cpp - Signature database manager -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/SignatureDB.h"

#include "DataReference.h"
#include "LocalDataReference.h"
#include "X86/X86ImportReference.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/RTTI.h"
#include "neverd/loader/COFF/RichHeader.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/loader/ImageProfile.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/loader/LinkerThunk.h"
#include "neverd/sigs/Limits.h"
#include "neverd/sigs/PatternParser.h"
#include "neverd/sigs/SignatureMatcher.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/FilePath.h"
#include "neverd/support/ISAEncoding.h"
#include "neverd/support/InstructionFields.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

using namespace neverd;
using namespace neverd::sigs;

namespace {

#define NEVERD_SIGS_FILE_STRING(Name, Value)                                   \
  constexpr llvm::StringLiteral Name(Value);
#define NEVERD_SIGS_FILE_VALUE(Name, Value) constexpr unsigned Name = Value;
#include "SignatureFiles.def"

/// One source's pattern lines, parsed, and the bytes their modules keep.
struct ParsedSource {
  std::vector<PatternChunk> Chunks;
  std::unique_ptr<uint8_t[]> Bytes;
};

llvm::Expected<ParsedSource> parseSource(llvm::StringRef Text) {
  ParsedSource Source;
  Source.Chunks = PatternParser::splitChunks(Text);
  Source.Bytes = PatternParser::reserveBytes(Source.Chunks);
  PatternParser::parseChunks(Source.Chunks);
  if (llvm::Error Error = PatternParser::firstError(Source.Chunks))
    return std::move(Error);
  return std::move(Source);
}

/// The modules \p Chunks hold, in order; the names they point into move to
/// \p Names.
std::vector<StoredModule> takeModules(std::vector<PatternChunk> &Chunks,
                                      std::vector<PatternNames> &Names) {
  std::vector<StoredModule> Mods;
  for (PatternChunk &Chunk : Chunks) {
    Mods.insert(Mods.end(), Chunk.Modules.begin(), Chunk.Modules.end());
    Names.push_back(std::move(Chunk.Names));
  }
  return Mods;
}

} // namespace

void SignatureDB::commitSource(SigSource Src, std::vector<StoredModule> Mods) {
  AutoSelection.reset();
  Src.ModuleCount = Mods.size();
  auto Existing = std::find_if(
      LoadedFiles.begin(), LoadedFiles.end(),
      [&](const SigSource &Source) { return Source.Path == Src.Path; });
  if (Existing != LoadedFiles.end()) {
    const size_t Start = Existing->ModuleStart;
    Modules.erase(Modules.begin() + Start,
                  Modules.begin() + Start + Existing->ModuleCount);
    Modules.insert(Modules.begin() + Start, Mods.begin(), Mods.end());
    *Existing = std::move(Src);

    size_t ModuleStart = 0;
    for (SigSource &Source : LoadedFiles) {
      Source.ModuleStart = ModuleStart;
      ModuleStart += Source.ModuleCount;
    }
    Index.reset();
    clearMatches();
    return;
  }

  Src.ModuleStart = Modules.size();
  LoadedFiles.push_back(std::move(Src));
  Modules.insert(Modules.end(), Mods.begin(), Mods.end());
  Index.reset();
  clearMatches();
}

const SignatureMatcher::HashIndex &SignatureDB::index() {
  if (!Index) {
    Index = std::make_unique<SignatureMatcher::HashIndex>();
    Index->build(Modules);
  }
  return *Index;
}

const std::string &SignatureDB::libraryNameOf(size_t ModuleIndex) const {
  for (const SigSource &Src : LoadedFiles)
    if (ModuleIndex >= Src.ModuleStart &&
        ModuleIndex < Src.ModuleStart + Src.ModuleCount)
      return Src.LibraryName;
  static const std::string Empty;
  return Empty;
}

llvm::Error SignatureDB::loadFile(const std::filesystem::path &Path) {
  auto Ext = pathToUTF8(Path.extension());

  if (Ext == ".json")
    return loadFeaturePack(Path);

  if (Ext == PatternExtension || Ext == NativePatternExtension) {
    SigSource Src;
    Src.Path = pathToUTF8(Path);
    Src.LibraryName = libraryName(Path);
    const std::optional<SignatureCache::SourceIdentity> Identity =
        Cache ? SignatureCache::identify(Path) : std::nullopt;
    if (Identity) {
      if (std::optional<SignatureCache::Entry> Hit =
              Cache->load(Path, *Identity)) {
        Src.Mapping = std::move(Hit->Mapping);
        Src.Names.push_back(std::move(Hit->Names));
        commitSource(std::move(Src), std::move(Hit->Modules));
        return llvm::Error::success();
      }
    }
    auto BufferOrErr = llvm::MemoryBuffer::getFile(
        pathToUTF8(Path), /*IsText=*/false, /*RequiresNullTerminator=*/false);
    if (!BufferOrErr)
      return llvm::make_error<llvm::StringError>(
          "cannot open pattern file: " + pathToUTF8(Path),
          llvm::inconvertibleErrorCode());
    auto SourceOrErr = parseSource((*BufferOrErr)->getBuffer());
    if (!SourceOrErr)
      return SourceOrErr.takeError();
    std::vector<StoredModule> Mods =
        takeModules(SourceOrErr->Chunks, Src.Names);
    Src.Bytes = std::move(SourceOrErr->Bytes);
    if (Identity)
      Cache->store(Path, *Identity, Mods);
    commitSource(std::move(Src), std::move(Mods));
    return llvm::Error::success();
  }

  return llvm::make_error<llvm::StringError>(
      "unsupported signature file format: " + pathToUTF8(Path),
      llvm::inconvertibleErrorCode());
}

llvm::Error SignatureDB::loadPatternText(llvm::StringRef Text,
                                         llvm::StringRef LibraryName) {
  auto SourceOrErr = parseSource(Text);
  if (!SourceOrErr)
    return SourceOrErr.takeError();
  SigSource Src;
  Src.Path = LibraryName.str();
  Src.LibraryName = LibraryName.str();
  std::vector<StoredModule> Mods = takeModules(SourceOrErr->Chunks, Src.Names);
  Src.Bytes = std::move(SourceOrErr->Bytes);
  commitSource(std::move(Src), std::move(Mods));
  return llvm::Error::success();
}

llvm::Expected<std::vector<std::filesystem::path>>
SignatureDB::listDirectory(const std::filesystem::path &Dir) {
  std::error_code EC;
  if (!std::filesystem::exists(Dir, EC)) {
    if (EC)
      return llvm::make_error<llvm::StringError>(
          "cannot inspect signature directory: " + pathToUTF8(Dir) + ": " +
              EC.message(),
          llvm::inconvertibleErrorCode());
    return llvm::make_error<llvm::StringError>(
        "signature directory does not exist: " + pathToUTF8(Dir),
        llvm::inconvertibleErrorCode());
  }
  if (!std::filesystem::is_directory(Dir, EC)) {
    if (EC)
      return llvm::make_error<llvm::StringError>(
          "cannot inspect signature directory: " + pathToUTF8(Dir) + ": " +
              EC.message(),
          llvm::inconvertibleErrorCode());
    return llvm::make_error<llvm::StringError>(
        "signature path is not a directory: " + pathToUTF8(Dir),
        llvm::inconvertibleErrorCode());
  }

  // Collect ordinary and native patterns first, then parse in parallel.
  std::vector<std::filesystem::path> PatFiles;
  std::filesystem::directory_iterator It(Dir, EC);
  const std::filesystem::directory_iterator End;
  if (EC)
    return llvm::make_error<llvm::StringError>(
        "cannot enumerate signature directory: " + pathToUTF8(Dir) + ": " +
            EC.message(),
        llvm::inconvertibleErrorCode());
  while (It != End) {
    std::error_code TypeError;
    const bool IsRegular = It->is_regular_file(TypeError);
    if (TypeError)
      return llvm::make_error<llvm::StringError>(
          "cannot inspect signature entry: " + pathToUTF8(It->path()) + ": " +
              TypeError.message(),
          llvm::inconvertibleErrorCode());
    const auto Ext = It->path().extension();
    if (IsRegular && (Ext == PatternExtension.data() ||
                      Ext == NativePatternExtension.data()))
      PatFiles.push_back(It->path());
    It.increment(EC);
    if (EC)
      return llvm::make_error<llvm::StringError>(
          "cannot enumerate signature directory: " + pathToUTF8(Dir) + ": " +
              EC.message(),
          llvm::inconvertibleErrorCode());
  }
  std::sort(PatFiles.begin(), PatFiles.end());
  return PatFiles;
}

std::string SignatureDB::libraryName(const std::filesystem::path &File) {
  const std::string Stem = pathToUTF8(File.stem());
  const size_t Dot = llvm::StringRef(Stem).rfind(PartMarker);
  if (Dot == std::string::npos || Dot == 0)
    return Stem;
  const llvm::StringRef Number =
      llvm::StringRef(Stem).drop_front(Dot + PartMarker.size());
  unsigned Part = 0, FirstDigit = 0;
  // Part 1 is the file under the library's own name; a part number is
  // written without leading zeros.
  if (Number.getAsInteger(NameNumberRadix, Part) || Part < FirstNumberedPart ||
      Number.take_front().getAsInteger(NameNumberRadix, FirstDigit) ||
      FirstDigit == 0)
    return Stem;
  return Stem.substr(0, Dot);
}

std::vector<std::filesystem::path>
SignatureDB::selectForImage(const BinaryImage &Img,
                            std::vector<std::filesystem::path> Files) {
  const ImageProfile Profile = identifyImageProfile(Img);
  if (Profile.ConflictingEvidence)
    return Files;
  const auto Fits = [&](ImageSystem System, ImageDomain Domain,
                        ImageRuntime Runtime) {
    // Linux kernel ABI facts also fit Android; runtime/domain evidence
    // distinguishes its user libraries without excluding common helpers.
    const auto LinuxFamily = [](ImageSystem Value) {
      return Value == ImageSystem::Linux || Value == ImageSystem::Android;
    };
    return (System == ImageSystem::Unknown ||
            Profile.System == ImageSystem::Unknown ||
            System == Profile.System ||
            (LinuxFamily(System) && LinuxFamily(Profile.System))) &&
           (Domain == ImageDomain::Unknown ||
            Profile.Domain == ImageDomain::Unknown ||
            Domain == Profile.Domain) &&
           (Runtime == ImageRuntime::Unknown ||
            Profile.Runtime == ImageRuntime::Unknown ||
            Runtime == Profile.Runtime);
  };
  llvm::erase_if(Files, [&](const std::filesystem::path &File) {
    const std::string Library = libraryName(File);
#define NEVERD_SIGNATURE_SCOPE(Format, Name, System, Domain, Runtime)          \
  if (Img.abiFormat() == BinaryFormat::Format && Library == Name)              \
    return !Fits(ImageSystem::System, ImageDomain::Domain,                     \
                 ImageRuntime::Runtime);
#include "SignatureScopes.def"
    return false;
  });
  if (!Img.COFFRichHeader)
    return Files;
  const std::vector<unsigned> Years = richToolsetYears(*Img.COFFRichHeader);
  if (Years.empty())
    return Files;

  // "vs2026.pat" belongs to Visual Studio 2026; other names to no release.
  auto ReleaseOf = [](const std::filesystem::path &Path) {
    const std::string Stem = libraryName(Path);
    llvm::StringRef Digits = Stem;
    unsigned Year = 0;
    if (!Digits.consume_front(ReleasePrefix) ||
        Digits.size() != ReleaseYearDigits ||
        Digits.getAsInteger(NameNumberRadix, Year))
      return 0u;
    return Year;
  };
  std::vector<std::filesystem::path> Selected;
  bool KeptRelease = false;
  for (const std::filesystem::path &File : Files) {
    const unsigned Release = ReleaseOf(File);
    if (Release == 0) {
      Selected.push_back(File);
      continue;
    }
    if (std::find(Years.begin(), Years.end(), Release) != Years.end()) {
      Selected.push_back(File);
      KeptRelease = true;
    }
  }
  return KeptRelease ? Selected : Files;
}

std::optional<std::filesystem::path>
SignatureDB::treeDirectory(const BinaryImage &Img) {
  std::optional<llvm::StringRef> Format, Family;
  switch (Img.abiFormat()) {
#define NEVERD_SIGS_TREE_FORMAT(Name, Directory)                               \
  case BinaryFormat::Name:                                                     \
    Format = Directory;                                                        \
    break;
#include "SignatureFiles.def"
  default:
    break;
  }
  switch (Img.Arch) {
#define NEVERD_SIGS_TREE_ARCH(Name, Directory)                                 \
  case Arch::Name:                                                             \
    Family = Directory;                                                        \
    break;
#include "SignatureFiles.def"
  default:
    break;
  }
  if (!Format || !Family)
    return std::nullopt;
  return std::filesystem::path(Format->str()) / Family->str() /
         (Img.is64Bit() ? TreeDirectory64Bit : TreeDirectory32Bit).str();
}

llvm::Error SignatureDB::loadDirectory(const std::filesystem::path &Dir) {
  auto PatFiles = listDirectory(Dir);
  if (!PatFiles)
    return PatFiles.takeError();
  return loadFiles(*PatFiles);
}

llvm::Error
SignatureDB::loadFiles(const std::vector<std::filesystem::path> &PatFiles) {
  // A file the cache holds as it is now is mapped rather than parsed.  Every
  // entry is opened, and the pieces of all of them -- checking their blocks,
  // making their modules -- are shared out among the workers, as the chunks
  // of the files that are parsed are below.
  std::vector<std::optional<SignatureCache::SourceIdentity>> Identities(
      PatFiles.size());
  std::vector<std::optional<SignatureCache::PendingEntry>> Pending(
      PatFiles.size());
  if (Cache)
    neverd::parallelForEach(PatFiles.size(), [&](auto Claim, size_t Total) {
      for (size_t I = Claim(); I < Total; I = Claim()) {
        Identities[I] = SignatureCache::identify(PatFiles[I]);
        if (Identities[I])
          Pending[I] = Cache->open(PatFiles[I], *Identities[I]);
      }
    });
  std::vector<std::pair<size_t, size_t>> Pieces;
  for (size_t I = 0; I < PatFiles.size(); ++I)
    if (Pending[I])
      for (size_t Piece = 0; Piece < Pending[I]->pieces(); ++Piece)
        Pieces.emplace_back(I, Piece);
  std::vector<std::atomic<bool>> Damaged(PatFiles.size());
  neverd::parallelForEach(Pieces.size(), [&](auto Claim, size_t Total) {
    for (size_t W = Claim(); W < Total; W = Claim())
      if (!Pending[Pieces[W].first]->run(Pieces[W].second))
        Damaged[Pieces[W].first] = true;
  });
  std::vector<std::optional<SignatureCache::Entry>> Cached(PatFiles.size());
  for (size_t I = 0; I < PatFiles.size(); ++I)
    if (Pending[I] && !Damaged[I])
      Cached[I] = std::move(*Pending[I]).take();
  Pending.clear();

  // Every other file is cut into chunks of whole lines, and one pool of
  // workers parses the chunks of all of them: one large file keeps every
  // worker as busy as many small ones do.  A parsed chunk no longer refers to
  // its text, so a file is let go as soon as its last chunk is done.
  std::vector<std::unique_ptr<llvm::MemoryBuffer>> Buffers(PatFiles.size());
  std::vector<std::unique_ptr<uint8_t[]>> Bytes(PatFiles.size());
  std::vector<PatternChunk> Chunks;
  std::vector<size_t> FirstChunk(PatFiles.size() + 1), FileOf;
  for (size_t I = 0; I < PatFiles.size(); ++I) {
    FirstChunk[I] = Chunks.size();
    if (Cached[I])
      continue;
    auto BufferOrErr =
        llvm::MemoryBuffer::getFile(pathToUTF8(PatFiles[I]), /*IsText=*/false,
                                    /*RequiresNullTerminator=*/false);
    if (!BufferOrErr)
      continue;
    Buffers[I] = std::move(*BufferOrErr);
    std::vector<PatternChunk> FileChunks =
        PatternParser::splitChunks(Buffers[I]->getBuffer());
    Bytes[I] = PatternParser::reserveBytes(FileChunks);
    Chunks.insert(Chunks.end(), std::make_move_iterator(FileChunks.begin()),
                  std::make_move_iterator(FileChunks.end()));
    FileOf.resize(Chunks.size(), I);
  }
  FirstChunk[PatFiles.size()] = Chunks.size();
  std::vector<std::atomic<size_t>> Unparsed(PatFiles.size());
  for (size_t I = 0; I < PatFiles.size(); ++I)
    Unparsed[I] = FirstChunk[I + 1] - FirstChunk[I];
  PatternParser::parseChunks(Chunks, [&](size_t Chunk) {
    if (--Unparsed[FileOf[Chunk]] == 0)
      Buffers[FileOf[Chunk]].reset();
  });

  // The first file that cannot be read or parsed, in order, fails the batch
  // and leaves the database as it was.
  size_t Count = 0;
  for (size_t I = 0; I < PatFiles.size(); ++I) {
    if (Cached[I]) {
      Count += Cached[I]->Modules.size();
      continue;
    }
    const llvm::ArrayRef<PatternChunk> FileChunks(
        Chunks.data() + FirstChunk[I], Chunks.data() + FirstChunk[I + 1]);
    const bool Opened = FirstChunk[I] != FirstChunk[I + 1] || Buffers[I];
    llvm::Error Error =
        Opened ? PatternParser::firstError(FileChunks)
               : llvm::make_error<llvm::StringError>(
                     "cannot open pattern file: " + pathToUTF8(PatFiles[I]),
                     llvm::inconvertibleErrorCode());
    if (Error)
      return llvm::make_error<llvm::StringError>(
          "cannot parse signature file: " + pathToUTF8(PatFiles[I]) + ": " +
              llvm::toString(std::move(Error)),
          llvm::inconvertibleErrorCode());
    for (const PatternChunk &Chunk : FileChunks)
      Count += Chunk.Modules.size();
  }

  std::vector<StoredModule> NewModules;
  NewModules.reserve(Count);
  std::vector<SigSource> NewSources;
  NewSources.reserve(PatFiles.size());
  for (size_t I = 0; I < PatFiles.size(); ++I) {
    SigSource Source;
    Source.Path = pathToUTF8(PatFiles[I]);
    Source.LibraryName = libraryName(PatFiles[I]);
    Source.ModuleStart = NewModules.size();
    if (Cached[I]) {
      NewModules.insert(NewModules.end(), Cached[I]->Modules.begin(),
                        Cached[I]->Modules.end());
      Source.Mapping = std::move(Cached[I]->Mapping);
      Source.Names.push_back(std::move(Cached[I]->Names));
    } else {
      Source.Bytes = std::move(Bytes[I]);
      for (size_t C = FirstChunk[I]; C < FirstChunk[I + 1]; ++C) {
        NewModules.insert(NewModules.end(), Chunks[C].Modules.begin(),
                          Chunks[C].Modules.end());
        Source.Names.push_back(std::move(Chunks[C].Names));
      }
    }
    Source.ModuleCount = NewModules.size() - Source.ModuleStart;
    NewSources.push_back(std::move(Source));
  }
  Modules.swap(NewModules);
  LoadedFiles.swap(NewSources);
  AutoSelection.reset();
  Index.reset();
  clearMatches();

  // What was parsed is kept for the next load, once the whole batch has
  // loaded.
  if (Cache)
    neverd::parallelForEach(LoadedFiles.size(), [&](auto Claim, size_t Total) {
      for (size_t I = Claim(); I < Total; I = Claim())
        if (Identities[I] && !LoadedFiles[I].Mapping)
          Cache->store(
              PatFiles[I], *Identities[I],
              llvm::ArrayRef(Modules).slice(LoadedFiles[I].ModuleStart,
                                            LoadedFiles[I].ModuleCount));
    });
  return llvm::Error::success();
}

void SignatureDB::apply(const BinaryImage &Img,
                        const std::vector<uint64_t> &FuncEntries) {
  if (Img.BytePatchOriginal) {
    apply(*Img.BytePatchOriginal, FuncEntries);
    return;
  }
  clearMatches();
  if (Modules.empty() || FuncEntries.empty())
    return;

  const SignatureMatcher::HashIndex &Idx = index();

  // Collect executable segment data for matching.
  std::vector<SignatureMatcher::Hit> Hits;
  std::vector<SignatureMatcher::Hit> DataHits;
  std::vector<SignatureMatcher::Hit> PairHits;
  for (const auto &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.empty())
      continue;

    for (const SignatureMatcher::Hit &Hit :
         SignatureMatcher::findAtAddresses(Seg.Data.data(), Seg.Data.size(),
                                           Seg.VA, FuncEntries, Modules, Idx)) {
      Hits.push_back(Hit);
      const uint64_t Addr = Hit.Address;
      const size_t ModIdx = Hit.Module;
      const StoredModule &Mod = Modules[ModIdx];
      // A short required clause needs an explicit unpruned source body before
      // its address-taken leaf can contribute to a joint byte fingerprint.
      if (Img.isCOFF() && Img.Arch == Arch::X64 && !Img.IsRelocatable &&
          Mod.RequireReferences && Mod.ReferenceCount == 1 &&
          !Mod.references().front().IsData && Mod.PublicNameCount == 1 &&
          Mod.publicNames().front().Offset == 0 && !Mod.CRCLen &&
          Mod.TotalLen < SignatureMatcher::MinStatedBytes + sizeof(uint32_t) &&
          SignatureMatcher::fixedByteCount(Mod) <
              SignatureMatcher::MinStatedBytes &&
          llvm::any_of(
              Idx.callerModules(Modules, Mod.publicNames().front().Name),
              [&](const auto &Source) {
                return Modules[Source.Module].RequireCaller;
              }))
        PairHits.push_back(Hit);
      if (Idx.hasDataContext() && Mod.RequireReferences &&
          llvm::any_of(Mod.references(),
                       [](const StoredName &Ref) { return Ref.IsData; })) {
        DataHits.push_back(Hit);
        continue;
      }
      appendModuleMatches(Addr, ModIdx);
    }
  }
  checkReferences(Img, FuncEntries, std::move(Hits));
  applyCodeAddressPairs(Img, FuncEntries, PairHits);
  std::vector<DeferredCaller> DeferredCallers;
  applyCallerContext(Img, FuncEntries, 0, nullptr, {}, &DeferredCallers);
  applyVTableContext(Img, FuncEntries);
  applyDataContext(Img, FuncEntries, DataHits, DeferredCallers);
  applyRuntimePointerContext(Img, FuncEntries);
  applyCodeLayoutContext(Img, FuncEntries);
  admitStatedNames(Img);
}

void SignatureDB::appendModuleMatches(uint64_t Addr, size_t ModIdx) {
  const StoredModule &Mod = Modules[ModIdx];
  // The names a module gives one offset are one routine's aliases, so
  // each offset makes one match.
  auto AddMatch = [&](uint32_t Offset, llvm::ArrayRef<std::string_view> Names) {
    if (Offset > std::numeric_limits<uint64_t>::max() - Addr)
      return;
    SigMatch M;
    M.Address = Addr + Offset;
    M.Name = std::string(Names.front());
    for (auto It = std::next(Names.begin()); It != Names.end(); ++It)
      M.Aliases.emplace_back(*It);
    M.LibraryName = libraryNameOf(ModIdx);
    M.FuncLen = Mod.TotalLen;
    Matches.push_back(std::move(M));
    MatchModules.push_back(ModIdx);
  };
  const auto Publics = Mod.publicNames();
  // A single name needs neither a grouping tree nor an alias buffer.
  if (Publics.size() == 1) {
    AddMatch(Publics.front().Offset, Publics.front().Name);
    return;
  }
  std::map<uint32_t, std::vector<std::string_view>> ByOffset;
  for (const StoredName &Ref : Publics)
    ByOffset[Ref.Offset].push_back(Ref.Name);
  for (auto &[Offset, Names] : ByOffset) {
    std::sort(Names.begin(), Names.end(), preferredAliasOrder);
    Names.erase(std::unique(Names.begin(), Names.end()), Names.end());
    AddMatch(Offset, Names);
  }
}

void SignatureDB::admitStatedNames(const BinaryImage &Img) {
  const auto IsStatedFunction = [](const Symbol &Sym) {
    return Sym.IsFunc && !Sym.IsSection && !Sym.IsBoundaryGuess &&
           Sym.Origin == NameOrigin::Stated && !Sym.Name.empty();
  };
  if (Matches.empty() ||
      (Img.Exports.empty() && llvm::none_of(Img.Symbols, IsStatedFunction)))
    return;
  // Index producer evidence once. Looking up the entire symbol table for each
  // hit makes larger signature sets unnecessarily expensive. Views live only
  // for this call; retained decisions own their names.
  std::unordered_map<uint64_t, llvm::SmallDenseSet<llvm::StringRef, 4>> Stated;
  for (const SigMatch &M : Matches)
    Stated.try_emplace(M.Address);
  const auto Add = [&](uint64_t Address, std::string_view Name) {
    const auto It = Stated.find(Address);
    if (It != Stated.end() && !Name.empty())
      It->second.insert(llvm::StringRef(Name));
  };
  for (const Symbol &Sym : Img.Symbols)
    if (IsStatedFunction(Sym))
      Add(Sym.Addr, Sym.Name);
  for (const Export &Exp : Img.Exports)
    Add(Exp.Addr, Exp.Name);
  for (const SigMatch &M : Matches) {
    const auto It = Stated.find(M.Address);
    if (It == Stated.end())
      continue;
    // Hash membership avoids comparing every stated ICF alias with every
    // candidate alias. Neither the preferred spelling nor duplicates get
    // special authority.
    const auto Admit = [&](std::string_view Name) {
      if (!It->second.contains(llvm::StringRef(Name)))
        return;
      auto [Decision, Fresh] = NameDecisions.try_emplace(
          M.Address, SignatureNameDecision{std::string(Name),
                                           SignatureNameEvidence::ImageSymbol});
      if (!Fresh && preferredAliasOrder(Name, Decision->second.Name))
        Decision->second.Name = Name;
    };
    Admit(M.Name);
    for (const std::string &Alias : M.Aliases)
      Admit(Alias);
  }
}

size_t SignatureDB::identifyPersonalityRoutines(BinaryImage &Img) {
  const std::vector<va_t> Candidates = collectUnnamedPersonalityRoutines(Img);
  if (Candidates.empty() || Modules.empty())
    return 0;

  // One proposal per address, and the addresses two modules disagree about
  // recorded so they can be dropped: a routine that two signatures name
  // differently is a routine neither of them has identified.
  std::map<uint64_t, SigMatch> Proposed;
  std::set<uint64_t> Disputed;
  const SignatureMatcher::HashIndex &Idx = index();

  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.empty())
      continue;

    for (const SignatureMatcher::Hit &Hit :
         SignatureMatcher::findAtAddresses(Seg.Data.data(), Seg.Data.size(),
                                           Seg.VA, Candidates, Modules, Idx)) {
      const StoredModule &Mod = Modules[Hit.Module];
      // Whole-function agreement is the main gate, but on its own it would
      // also be satisfied by a short pattern that is mostly wildcards.
      if (Mod.RequireReferences || Mod.RequireCaller ||
          !SignatureMatcher::isFullyVerified(Mod) ||
          SignatureMatcher::fixedByteCount(Mod) <
              SignatureMatcher::MinStatedBytes)
        continue;

      // Of the personality routines the module names at its start -- aliases
      // of one routine when there are several -- the preferred.  A name at a
      // non-zero offset belongs to some other function the module also
      // describes, not to the routine being identified.
      std::optional<std::string_view> Chosen;
      for (const StoredName &Ref : Mod.publicNames()) {
        if (Ref.Offset != 0)
          continue;
        const ExceptionPersonality P = classifyPersonalityName(Ref.Name);
        if (P == ExceptionPersonality::None ||
            P == ExceptionPersonality::Unknown)
          continue;
        if (!Chosen || preferredAliasOrder(Ref.Name, *Chosen))
          Chosen = Ref.Name;
      }
      if (Chosen) {
        SigMatch M;
        M.Address = Hit.Address;
        M.Name = std::string(*Chosen);
        M.LibraryName = libraryNameOf(Hit.Module);
        M.FuncLen = Mod.TotalLen;
        auto [It, Fresh] = Proposed.emplace(Hit.Address, std::move(M));
        if (!Fresh && It->second.Name != *Chosen)
          Disputed.insert(Hit.Address);
      }
    }
  }

  size_t Named = 0;
  for (const auto &[Addr, Match] : Proposed) {
    if (Disputed.count(Addr))
      continue;
    if (!adoptPersonalityRoutineName(Img, Addr, Match.Name))
      continue;
    Matches.push_back(Match);
    MatchModules.push_back(NoModule);
    NameDecisions.insert_or_assign(
        Addr, SignatureNameDecision{
                  Match.Name, SignatureNameEvidence::ExceptionPersonality});
    ++Named;
  }

  // The adopted names are in the symbol table now, and that is what the
  // image-wide detection reads.  A stripped image that had nothing to go on
  // may well have something now, so ask again -- but only when the earlier
  // answer was that there was no answer, because a runtime already proven
  // from sections and banners is better evidenced than one personality name.
  if (Named != 0 &&
      Img.ExceptionMetadata.Runtime.Runtime == SourceLanguageRuntime::Unknown)
    Img.ExceptionMetadata.Runtime = detectLanguageRuntime(Img);

  return Named;
}

const SigMatch *SignatureDB::findMatch(uint64_t Addr) const {
  for (const auto &M : Matches) {
    if (M.Address == Addr)
      return &M;
  }
  return nullptr;
}

const SignatureNameDecision *SignatureDB::nameDecision(uint64_t Addr) const {
  const auto It = NameDecisions.find(Addr);
  return It == NameDecisions.end() ? nullptr : &It->second;
}

std::optional<SignatureDB::SettledRoutine>
SignatureDB::settle(llvm::ArrayRef<const SigMatch *> Proposals) {
  // Each match's names for its address: its name and its aliases, sorted.
  auto NamesOf = [](const SigMatch &M,
                    llvm::SmallVectorImpl<std::string_view> &Names) {
    Names.clear();
    Names.push_back(M.Name);
    for (const std::string &Alias : M.Aliases)
      Names.push_back(Alias);
    llvm::sort(Names);
    Names.erase(std::unique(Names.begin(), Names.end()), Names.end());
  };
  // Matches agree when a name is in every one of them; the routine then
  // takes the preferred such name, and has every name any of them gives it.
  auto Agree = [&](llvm::ArrayRef<const SigMatch *> Agreeing)
      -> std::optional<SettledRoutine> {
    if (Agreeing.empty())
      return std::nullopt;
    SettledRoutine Routine;
    llvm::SmallVector<std::string_view, 4> Shared, Names, Merged;
    NamesOf(*Agreeing.front(), Shared);
    Routine.Names = Shared;
    for (const SigMatch *M : Agreeing.drop_front()) {
      NamesOf(*M, Names);
      Merged.clear();
      std::set_intersection(Shared.begin(), Shared.end(), Names.begin(),
                            Names.end(), std::back_inserter(Merged));
      Shared.swap(Merged);
      if (Shared.empty())
        return std::nullopt;
      Merged.clear();
      std::set_union(Routine.Names.begin(), Routine.Names.end(), Names.begin(),
                     Names.end(), std::back_inserter(Merged));
      Routine.Names.swap(Merged);
    }
    if (Shared.empty())
      return std::nullopt;
    Routine.Name =
        *std::min_element(Shared.begin(), Shared.end(), preferredAliasOrder);
    return Routine;
  };

  // Incoming references directly request these names at one target. They
  // can distinguish nominal variants whose bodies and outgoing references
  // are identical. Different validated incoming names at that target are
  // candidate aliases, not a reason to choose one source body arbitrarily.
  SettledRoutine Incoming;
  for (const SigMatch *M : Proposals)
    if (M->CallerConfirmed) {
      Incoming.Names.push_back(M->Name);
      for (const std::string &Alias : M->Aliases)
        Incoming.Names.push_back(Alias);
    }
  if (!Incoming.Names.empty()) {
    llvm::sort(Incoming.Names);
    Incoming.Names.erase(
        std::unique(Incoming.Names.begin(), Incoming.Names.end()),
        Incoming.Names.end());
    Incoming.Name = *std::min_element(
        Incoming.Names.begin(), Incoming.Names.end(), preferredAliasOrder);
    return Incoming;
  }
  std::optional<SettledRoutine> Routine = Agree(Proposals);
  if (!Routine) {
    llvm::SmallVector<const SigMatch *, 4> Confirmed;
    for (const SigMatch *M : Proposals)
      if (M->Confirmed)
        Confirmed.push_back(M);
    Routine = Agree(Confirmed);
  }
  return Routine;
}

std::unordered_map<uint64_t, std::string>
SignatureDB::buildCandidateNameMap() const {
  // One contiguous grouping avoids a hash node and a separate vector
  // allocation for every function, which is usually named by one match.
  std::vector<const SigMatch *> Proposed;
  Proposed.reserve(Matches.size());
  for (const SigMatch &M : Matches)
    Proposed.push_back(&M);
  llvm::sort(Proposed, [](const SigMatch *A, const SigMatch *B) {
    return A->Address < B->Address;
  });
  std::unordered_map<uint64_t, std::string> Settled;
  Settled.reserve(Proposed.size());
  for (size_t First = 0; First < Proposed.size();) {
    size_t End = First + 1;
    while (End < Proposed.size() &&
           Proposed[End]->Address == Proposed[First]->Address)
      ++End;
    if (std::optional<SettledRoutine> Routine =
            settle(llvm::ArrayRef(Proposed).slice(First, End - First)))
      Settled.emplace(Proposed[First]->Address, Routine->Name);
    First = End;
  }
  return Settled;
}

std::unordered_map<uint64_t, std::string> SignatureDB::buildNameMap() const {
  std::unordered_map<uint64_t, std::string> Map;
  Map.reserve(NameDecisions.size());
  for (const auto &[Address, Decision] : NameDecisions)
    Map.emplace(Address, Decision.Name);
  return Map;
}

SignatureDB SignatureDB::snapshotForAnalysis() const {
  SignatureDB Snapshot;
  Snapshot.Matches = Matches;
  Snapshot.NameDecisions = NameDecisions;
  Snapshot.FeaturePacks = FeaturePacks;
  Snapshot.FeatureGeneration = FeatureGeneration;
  return Snapshot;
}

void SignatureDB::clear() {
  AutoSelection.reset();
  Modules.clear();
  LoadedFiles.clear();
  FeaturePacks.clear();
  ++FeatureGeneration;
  Index.reset();
  clearMatches();
}

llvm::Error SignatureDB::loadFeaturePack(const std::filesystem::path &Path) {
  auto Pack = readLibraryFeaturePack(Path);
  if (!Pack)
    return Pack.takeError();
  const std::string Id = Pack->Id;
  AutoSelection.reset();
  FeaturePacks.insert_or_assign(Id, std::move(*Pack));
  ++FeatureGeneration;
  return llvm::Error::success();
}

llvm::Error
SignatureDB::loadFeatureDirectory(const std::filesystem::path &Dir) {
  std::error_code EC;
  std::filesystem::directory_iterator I(Dir, EC), End;
  std::vector<std::filesystem::path> Files;
  for (; !EC && I != End; I.increment(EC)) {
    if (I->path().extension() == ".json")
      Files.push_back(I->path());
    if (Files.size() >
        ::neverd::limits::sigs::kSignatureDBLoadFeatureDirectoryFilesSizeLimit)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "library feature pack budget exceeded");
  }
  if (EC)
    return llvm::createStringError(EC, "cannot list library feature directory");
  std::sort(Files.begin(), Files.end());
  std::map<std::string, LibraryFeaturePack> NewPacks;
  for (const auto &File : Files) {
    auto Pack = readLibraryFeaturePack(File);
    if (!Pack)
      return Pack.takeError();
    const std::string Id = Pack->Id;
    if (!NewPacks.emplace(Id, std::move(*Pack)).second)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "duplicate library feature pack ID: " +
                                         Id);
  }
  AutoSelection.reset();
  FeaturePacks = std::move(NewPacks);
  ++FeatureGeneration;
  return llvm::Error::success();
}

void SignatureDB::clearMatches() {
  Matches.clear();
  MatchModules.clear();
  NameDecisions.clear();
}

llvm::Error SignatureDB::loadForImage(const BinaryImage &Image,
                                      const std::filesystem::path &Root) {
  SignatureDB Features;
  std::error_code EC;
  const auto FeatureDirectory = Root / "features" / "rules";
  if (std::filesystem::exists(FeatureDirectory, EC)) {
    if (auto Error = Features.loadFeatureDirectory(FeatureDirectory))
      return Error;
  }
  if (EC)
    return llvm::createStringError(EC,
                                   "cannot inspect library feature directory");
  std::vector<std::filesystem::path> Files;
  SignatureSelectionStats Selection;
  if (auto Relative = treeDirectory(Image)) {
    const auto Directory = Root / *Relative;
    if (std::filesystem::exists(Directory, EC)) {
      auto Listed = listDirectory(Directory);
      if (!Listed)
        return Listed.takeError();
      Selection.AvailableFiles = Listed->size();
      Files = selectForImage(Image, std::move(*Listed));
      Selection.SelectedFiles = Files.size();
    }
    if (EC)
      return llvm::createStringError(
          EC, "cannot inspect legacy signature directory");
  }
  if (auto Error = loadFiles(Files))
    return Error;
  FeaturePacks = std::move(Features.FeaturePacks);
  AutoSelection = Selection;
  ++FeatureGeneration;
  return llvm::Error::success();
}

namespace {

enum class ReferenceVerdict { Unknown, Confirmed, Contradicted };

#define NEVERD_BRANCH_FORM(Name, Mask, Match)                                  \
  [[maybe_unused]] constexpr InstructionForm Name{Mask, Match};
#define NEVERD_BRANCH_FIELD(Name, Low, Width)                                  \
  [[maybe_unused]] constexpr BitField Name{Low, Width};
#define NEVERD_BRANCH_VALUE(Name, Value)                                       \
  [[maybe_unused]] constexpr uint32_t Name = Value;
#include "neverd/support/BranchEncoding.def"

#define NEVERD_PATTERN_VALUE(Name, Value)                                      \
  [[maybe_unused]] constexpr uint32_t Name = Value;
#include "neverd/sigs/PatternSyntax.def"

#define NEVERD_SIGS_SYNTAX_CHAR(Name, Value)                                   \
  [[maybe_unused]] constexpr char Name = Value;
#include "neverd/sigs/LinkerSyntax.def"

#define NEVERD_SIGS_REFERENCE_LIMIT(Name, Value) constexpr size_t Name = Value;
#include "ReferenceCheckLimits.def"

uint64_t wrapToImage(const BinaryImage &Img, uint64_t Address) {
  return Img.is64Bit() ? Address : static_cast<uint32_t>(Address);
}

/// Where a direct branch goes, and on 32-bit ARM whether it enters its
/// target in Thumb state.
struct Branch {
  uint64_t Target = 0;
  bool Thumb = false;
};

/// The direct branch of one of \p Forms at \p Address, in \p Mode, when the
/// image holds one.  A 32-bit image's addresses wrap at 32 bits; an A64
/// program counter is 64 bits wide whatever the image's pointers are.
std::optional<Branch> directBranch(const BinaryImage &Img, uint64_t Address,
                                   InstructionMode Mode,
                                   DirectBranchForms Forms) {
  const size_t Length = getDirectBranchLength(Img.Arch);
  const uint8_t *Insn = Img.readVA(Address, Length);
  if (!Insn)
    return std::nullopt;
  const std::optional<DirectBranch> Decoded =
      decodeDirectBranch(Img.Arch, Mode, Insn, Length, Address, Forms);
  if (!Decoded)
    return std::nullopt;
  const uint64_t Target = Decoded->wrappingTarget();
  return Branch{Img.Arch == Arch::AArch64 ? Target : wrapToImage(Img, Target),
                Decoded->TargetIsThumb};
}

/// The branches a reference names: direct jumps and calls, and on 32-bit ARM
/// the calls that switch state too.  An A32 reference also names a branch
/// under a condition.
DirectBranchForms referenceForms(Arch A, InstructionMode Mode) {
  DirectBranchForms Forms;
  Forms.Jumps = true;
  Forms.Calls = true;
  Forms.ExchangingCalls = A == Arch::ARM;
  Forms.Conditional = A == Arch::ARM && Mode == InstructionMode::ARM;
  return Forms;
}

/// Where the direct branch a reference at \p Offset of the routine at
/// \p Start describes goes, when the image holds that branch; see
/// PatternModule::References for the offsets.
std::optional<Branch> branchTarget(const BinaryImage &Img, uint64_t Start,
                                   uint32_t Offset) {
  const uint64_t Site = Start + Offset;
  switch (Img.Arch) {
  case Arch::X86:
  case Arch::X64:
    if (Img.isCOFF() && Offset >= x86::kJccNearDispOffset &&
        uint64_t(Offset) + sizeof(uint32_t) <=
            std::numeric_limits<size_t>::max() &&
        Start <= InvalidVA - (uint64_t(Offset) + sizeof(uint32_t))) {
      const size_t Size = size_t(Offset) + sizeof(uint32_t);
      if (const uint8_t *Bytes = Img.readVA(Start, Size))
        if (isX86ConditionalReferenceField({Bytes, Size}, Offset,
                                           Img.Arch == Arch::X64))
          return Branch{
              wrapToImage(Img, Start + Size +
                                   int64_t(readLE<int32_t>(Bytes + Offset))),
              false};
    }
    // The reference states the rel32 field, which follows the opcode.
    if (Site < x86::kRel32DispOffset)
      return std::nullopt;
    return directBranch(Img, Site - x86::kRel32DispOffset,
                        InstructionMode::Default,
                        referenceForms(Img.Arch, InstructionMode::Default));
  case Arch::AArch64:
    // B and BL; a veneer or anything else is not the branch the library had.
    return directBranch(Img, Site, InstructionMode::Default,
                        referenceForms(Img.Arch, InstructionMode::Default));
  case Arch::ARM:
    if (Offset & ArmStateReferenceMark)
      return directBranch(Img, Site - ArmStateReferenceMark,
                          InstructionMode::ARM,
                          referenceForms(Img.Arch, InstructionMode::ARM));
    return directBranch(Img, Site, InstructionMode::Thumb,
                        referenceForms(Img.Arch, InstructionMode::Thumb));
  default:
    return std::nullopt;
  }
}

/// The COFF IAT slot addressed by a canonical memory-indirect call/jump.
/// It is data-slot evidence, never an executable target to scan as code.
std::optional<uint64_t> importReferenceSlot(const BinaryImage &Img,
                                            uint64_t Start, uint32_t Offset) {
  if (!Img.isCOFF() || (Img.Arch != Arch::X86 && Img.Arch != Arch::X64))
    return std::nullopt;
  const uint64_t Size = uint64_t(Offset) + sizeof(uint32_t);
  if (Size > std::numeric_limits<size_t>::max() ||
      Start > std::numeric_limits<uint64_t>::max() - Size)
    return std::nullopt;
  const uint8_t *Bytes = Img.readVA(Start, static_cast<size_t>(Size));
  if (!Bytes || !isX86ImportReferenceField({Bytes, static_cast<size_t>(Size)},
                                           Offset, Img.Arch == Arch::X64))
    return std::nullopt;
  const uint32_t Field = readLE<uint32_t>(Bytes + Offset);
  if (Img.Arch == Arch::X86)
    return Field;
  return Start + Size + static_cast<int64_t>(static_cast<int32_t>(Field));
}

/// The address materialized by an x64 RIP-relative LEA. A vftable's metadata
/// is read by the COFF loader; this only locates the relocation field.
std::optional<uint64_t> addressReferenceTarget(const BinaryImage &Img,
                                               uint64_t Start,
                                               uint32_t Offset) {
  if (!Img.isCOFF() || Img.Arch != Arch::X64)
    return std::nullopt;
  const uint64_t Size = uint64_t(Offset) + sizeof(uint32_t);
  if (Size > std::numeric_limits<size_t>::max() ||
      Start > std::numeric_limits<uint64_t>::max() - Size)
    return std::nullopt;
  const uint8_t *Bytes = Img.readVA(Start, static_cast<size_t>(Size));
  if (!Bytes ||
      !isX64AddressReferenceField({Bytes, static_cast<size_t>(Size)}, Offset))
    return std::nullopt;
  return Start + Size + static_cast<int64_t>(readLE<int32_t>(Bytes + Offset));
}

/// Where the thunk the branch \p To enters jumps to, when all the thunk does
/// is jump on: an incremental-linking thunk, a branch island, or a linker's
/// long-branch or interworking thunk.  On 32-bit ARM the thunk runs in the
/// state \p To enters it in.
std::optional<Branch> thunkTarget(const BinaryImage &Img, const Branch &To) {
  const std::optional<LinkerThunk> Thunk = linkerThunkAt(
      Img, To.Target, To.Thumb ? InstructionMode::Thumb : InstructionMode::ARM);
  if (!Thunk)
    return std::nullopt;
  return Branch{Thunk->Target, Thunk->Thumb};
}

} // namespace

void SignatureDB::applyCodeAddressPairs(
    const BinaryImage &Img, llvm::ArrayRef<uint64_t> Entries,
    llvm::ArrayRef<SignatureMatcher::Hit> Hits) {
  if (Hits.empty())
    return;
  const auto &Idx = index();
  std::vector<uint64_t> Sorted(Entries.begin(), Entries.end());
  llvm::sort(Sorted);
  auto SingleStart = [](const StoredModule &Mod) {
    return Mod.PublicNameCount == 1 && Mod.publicNames().front().Offset == 0;
  };
  auto AddressField = [](const StoredModule &Mod, uint32_t Offset) {
    if (Mod.CRCLen || uint64_t(Offset) + sizeof(uint32_t) > Mod.TotalLen)
      return false;
    for (size_t I = Offset; I < uint64_t(Offset) + sizeof(uint32_t); ++I)
      if (Mod.isStated(I))
        return false;
    return true;
  };
  // Both bodies are shorter than MinStatedBytes plus one rel32 field. Check
  // every byte's owner, including nested data sections, without a new scan.
  auto CodeBody = [&](uint64_t Start, uint32_t Size) {
    if (!Img.isFunctionEntryCandidateAt(Start) || Size > InvalidVA - Start ||
        !std::binary_search(Sorted.begin(), Sorted.end(), Start))
      return false;
    const Segment *Seg = Img.getSegmentFor(Start);
    const Section *Sec = Img.getSectionFor(Start);
    for (uint32_t I = 0; I < Size; ++I)
      if (Img.getSegmentFor(Start + I) != Seg ||
          Img.getSectionFor(Start + I) != Sec || !Img.isCodeAddress(Start + I))
        return false;
    return true;
  };
  struct Proof {
    uint64_t Parent, Target;
    size_t Module, Leaf;
  };
  std::vector<Proof> Proofs;
  for (const auto &Hit : Hits) {
    const auto &Mod = Modules[Hit.Module];
    const auto &Ref = Mod.references().front();
    const size_t Fixed = SignatureMatcher::fixedByteCount(Mod);
    if (Mod.CRCLen || !SignatureMatcher::isFullyVerified(Mod) ||
        Mod.TotalLen != Fixed + sizeof(uint32_t) ||
        !AddressField(Mod, Ref.Offset) || !CodeBody(Hit.Address, Mod.TotalLen))
      continue;
    const auto Target = addressReferenceTarget(Img, Hit.Address, Ref.Offset);
    if (!Target)
      continue;

    // A required clause can omit source references. Every matching unpruned
    // source variant must instead state this one exact code-address edge.
    bool Source = false, Complete = true;
    for (const auto &C :
         Idx.callerModules(Modules, Mod.publicNames().front().Name)) {
      const auto &Full = Modules[C.Module];
      if (!Full.RequireCaller)
        continue;
      const uint8_t *Bytes = Img.readVA(Hit.Address, Full.TotalLen);
      if (!Bytes || !SignatureMatcher::matchPattern(Full, Bytes, Full.TotalLen))
        continue;
      Source = true;
      if (!SingleStart(Full) || Full.TotalLen != Mod.TotalLen || Full.CRCLen ||
          !SignatureMatcher::isFullyVerified(Full) ||
          Full.ReferenceCount != 1 || Full.references().front().IsData ||
          Full.references().front().Offset != Ref.Offset ||
          Full.references().front().Name != Ref.Name ||
          !AddressField(Full, Ref.Offset) ||
          SignatureMatcher::fixedByteCount(Full) != Fixed)
        Complete = false;
    }
    if (!Source || !Complete)
      continue;

    std::optional<size_t> Leaf;
    for (const auto &C : Idx.callerModules(Modules, Ref.Name)) {
      const auto &Body = Modules[C.Module];
      if (!Body.RequireCaller)
        continue;
      const uint8_t *Bytes = Img.readVA(*Target, Body.TotalLen);
      if (!Bytes || !SignatureMatcher::matchPattern(Body, Bytes, Body.TotalLen))
        continue;
      const size_t BodyFixed = SignatureMatcher::fixedByteCount(Body);
      if (!SingleStart(Body) || Body.ReferenceCount || Body.CRCLen ||
          !SignatureMatcher::isFullyVerified(Body) ||
          Body.TotalLen != BodyFixed ||
          BodyFixed >= SignatureMatcher::MinStatedBytes ||
          Fixed + BodyFixed < SignatureMatcher::MinStatedBytes ||
          !CodeBody(*Target, Body.TotalLen) ||
          (*Target < Hit.Address + Mod.TotalLen &&
           Hit.Address < *Target + Body.TotalLen) ||
          (Leaf && Modules[*Leaf].TotalLen != Body.TotalLen)) {
        Complete = false;
        break;
      }
      Leaf = C.Module;
    }
    if (Complete && Leaf)
      Proofs.push_back({Hit.Address, *Target, Hit.Module, *Leaf});
  }
  if (Proofs.empty())
    return;

  // Freeze existing claims only at the proof endpoints. New pairs cannot
  // confirm each other, overwrite an independent name, or form a cycle.
  std::map<uint64_t, llvm::SmallVector<const SigMatch *, 4>> At;
  std::set<uint64_t> Parents;
  for (const auto &P : Proofs) {
    At.try_emplace(P.Parent);
    At.try_emplace(P.Target);
    Parents.insert(P.Parent);
  }
  for (const auto &M : Matches)
    if (auto I = At.find(M.Address); I != At.end())
      I->second.push_back(&M);
  std::map<uint64_t, SettledRoutine> Frozen;
  for (const auto &[Address, Claims] : At)
    if (auto Name = settle(Claims))
      Frozen.emplace(Address, std::move(*Name));
  llvm::sort(Proofs, [](const Proof &A, const Proof &B) {
    return std::tie(A.Parent, A.Module, A.Target, A.Leaf) <
           std::tie(B.Parent, B.Module, B.Target, B.Leaf);
  });
  for (size_t I = 0; I < Proofs.size();) {
    const Proof &P = Proofs[I++];
    const auto ParentName = Modules[P.Module].publicNames().front().Name;
    const auto LeafName = Modules[P.Leaf].publicNames().front().Name;
    bool Agreed = !Parents.count(P.Target);
    while (I < Proofs.size() && Proofs[I].Parent == P.Parent) {
      const auto &Other = Proofs[I++];
      Agreed &=
          Other.Target == P.Target &&
          Modules[Other.Module].TotalLen == Modules[P.Module].TotalLen &&
          Modules[Other.Module].publicNames().front().Name == ParentName &&
          Modules[Other.Leaf].publicNames().front().Name == LeafName;
    }
    const auto Parent = Frozen.find(P.Parent), Child = Frozen.find(P.Target);
    if (!Agreed ||
        (Parent != Frozen.end() && !Parent->second.hasName(ParentName)) ||
        (Child != Frozen.end() && !Child->second.hasName(LeafName)))
      continue;
    if (Parent == Frozen.end()) {
      appendModuleMatches(P.Parent, P.Module);
      Matches.back().Confirmed = true;
    }
    if (Child == Frozen.end()) {
      appendModuleMatches(P.Target, P.Leaf);
      Matches.back().CallerConfirmed = true;
      Matches.back().Confirmed = true;
    }
  }
}

void SignatureDB::applyCallerContext(
    const BinaryImage &Img, llvm::ArrayRef<uint64_t> Entries,
    size_t FirstParent, const DataBindings *Data,
    llvm::ArrayRef<uint64_t> Revisit,
    std::vector<DeferredCaller> *DeferredCallers) {
  if (!index().hasCallerModules() || Matches.empty())
    return;
  if (Data && Revisit.empty()) {
    // Most data consumers are short getters or have no code references.
    // Consult existing source/name indices before building a settlement map.
    auto HasQuery = [&](const StoredModule &Mod) {
      return llvm::any_of(Mod.references(), [&](const StoredName &Ref) {
        return !Ref.IsData && !index().callerModules(Modules, Ref.Name).empty();
      });
    };
    bool Queryable = false;
    for (size_t I = FirstParent; I < Matches.size() && !Queryable; ++I) {
      if (!Matches[I].Confirmed || MatchModules[I] == NoModule)
        continue;
      const StoredModule &Mod = Modules[MatchModules[I]];
      if (!Mod.RequireReferences || !SignatureMatcher::isFullyVerified(Mod) ||
          SignatureMatcher::fixedByteCount(Mod) <
              SignatureMatcher::MinStatedBytes)
        continue;
      Queryable = HasQuery(Mod);
      for (const StoredName &Name : Mod.publicNames())
        for (const auto &Source : index().callerModules(Modules, Name.Name))
          if (Modules[Source.Module].RequireCaller &&
              HasQuery(Modules[Source.Module]))
            Queryable = true;
    }
    if (!Queryable)
      return;
  }
  std::unordered_map<uint64_t, std::string> Names;
  std::vector<std::pair<uint64_t, size_t>> FrozenCandidates;
  // Share one contiguous address index between settlement and independent
  // body checks. Rebuilding it after constructing the initial name map would
  // sort every candidate twice whenever a required parent needs a proof.
  FrozenCandidates.reserve(Matches.size());
  for (size_t I = 0; I < Matches.size(); ++I)
    FrozenCandidates.emplace_back(Matches[I].Address, I);
  llvm::sort(FrozenCandidates);
  if (!Data) {
    Names.reserve(FrozenCandidates.size());
    for (size_t First = 0; First < FrozenCandidates.size();) {
      const uint64_t Address = FrozenCandidates[First].first;
      llvm::SmallVector<const SigMatch *, 4> Claims;
      size_t End = First;
      while (End < FrozenCandidates.size() &&
             FrozenCandidates[End].first == Address)
        Claims.push_back(&Matches[FrozenCandidates[End++].second]);
      if (const auto Routine = settle(Claims))
        Names.emplace(Address, Routine->Name);
      First = End;
    }
  }
  // The data frontier settles only the addresses it visits. The initial pass
  // has already settled every frozen address, including unsuccessful queries.
  std::vector<uint64_t> SortedEntries;
  llvm::ArrayRef<uint64_t> Requested = Entries;
  if (!llvm::is_sorted(Entries)) {
    SortedEntries.assign(Entries.begin(), Entries.end());
    llvm::sort(SortedEntries);
    Requested = SortedEntries;
  }
  const size_t FrozenMatches = Matches.size();
  std::optional<std::unordered_map<uint64_t, uint32_t>> FrozenBodies;
  for (size_t Round = 0; Round < MaxIncomingRounds; ++Round) {
    const size_t Before = Matches.size();
    applyCallerContextRound(Img, Requested, Names, FrozenCandidates,
                            FrozenMatches, FirstParent, Data, Revisit,
                            DeferredCallers, FrozenBodies);
    Revisit = {};
    if (Matches.size() == Before)
      break;
    // Earlier rows are immutable and every new row is incoming evidence at a
    // previously unsettled target. Settlement prefers the union of these
    // requested names over its old byte alternatives. Extend that map without
    // sorting and settling every original candidate again in each round.
    for (size_t I = Before; I < Matches.size(); ++I) {
      const SigMatch &Match = Matches[I];
      auto [At, Fresh] = Names.try_emplace(Match.Address, Match.Name);
      if (!Fresh && preferredAliasOrder(Match.Name, At->second))
        At->second = Match.Name;
      for (const std::string &Alias : Match.Aliases)
        if (preferredAliasOrder(Alias, At->second))
          At->second = Alias;
    }
    FirstParent = Before;
  }
}

void SignatureDB::applyCallerContextRound(
    const BinaryImage &Img, llvm::ArrayRef<uint64_t> Entries,
    std::unordered_map<uint64_t, std::string> &Names,
    std::vector<std::pair<uint64_t, size_t>> &FrozenCandidates,
    size_t FrozenMatches, size_t FirstParent, const DataBindings *Data,
    llvm::ArrayRef<uint64_t> Revisit,
    std::vector<DeferredCaller> *DeferredCallers,
    std::optional<std::unordered_map<uint64_t, uint32_t>> &FrozenBodies) {
  const auto &Idx = index();
  auto FindName = [&](uint64_t Address) {
    auto Found = Names.find(Address);
    if (Found != Names.end() || !Data)
      return Found;
    const auto Begin = llvm::lower_bound(
        FrozenCandidates, Address,
        [](const auto &Row, uint64_t VA) { return Row.first < VA; });
    llvm::SmallVector<const SigMatch *, 4> Claims;
    for (auto I = Begin; I != FrozenCandidates.end() && I->first == Address;
         ++I)
      Claims.push_back(&Matches[I->second]);
    if (const auto Settled = settle(Claims))
      return Names.emplace(Address, Settled->Name).first;
    return Names.end();
  };

  // Extend only the latest supported frontier. All rounds validate their
  // outgoing constraints against the same original result, so descendants
  // cannot confirm or change the proof of an ancestor. A target already
  // named by a preceding round is never a new request, including cycle edges.
  const size_t OriginalMatches = Matches.size();
  std::map<uint64_t, std::vector<size_t>> At;
  auto AddParent = [&](size_t I) {
    const auto Name = FindName(Matches[I].Address);
    if (Name != Names.end() &&
        (Matches[I].Name == Name->second ||
         llvm::is_contained(Matches[I].Aliases, Name->second)))
      At[Matches[I].Address].push_back(I);
  };
  // Revisit only ordinary parents that previously had ambiguous data-bearing
  // requests. The frozen address index avoids another scan of old parents.
  for (uint64_t Address : Revisit) {
    auto I = llvm::lower_bound(
        FrozenCandidates, Address,
        [](const auto &Row, uint64_t VA) { return Row.first < VA; });
    for (; I != FrozenCandidates.end() && I->first == Address; ++I)
      if (I->second < FirstParent)
        AddParent(I->second);
  }
  for (size_t I = FirstParent; I < OriginalMatches; ++I)
    AddParent(I);
  // Short bodies and initial required-reference parents need a separately
  // established ordinary callee. Required roots also need a complete source
  // declaration: their selected clause cannot supply omitted outgoing edges.
  // Incoming or data-supported long parents already have their own roots.
  std::unordered_map<uint64_t, bool> IndependentParents;
  for (auto It = At.begin(); It != At.end();) {
    bool Anchored = true;
    bool NeedsBinding = false, NeedsSource = false;
    for (size_t &I : It->second) {
      if (MatchModules[I] == NoModule) {
        Anchored = false;
        break;
      }
      const bool Incoming = I >= FrozenMatches && Matches[I].CallerConfirmed;
      const bool Confirmed = I < FrozenMatches && Matches[I].Confirmed;
      I = MatchModules[I];
      const StoredModule &Mod = Modules[I];
      const bool Weak = SignatureMatcher::fixedByteCount(Mod) <
                        SignatureMatcher::MinStatedBytes;
      if (((Mod.RequireReferences || Mod.RequireCaller) && !Incoming &&
           !(Confirmed && Mod.RequireReferences)) ||
          !SignatureMatcher::isFullyVerified(Mod) ||
          (Weak && !Incoming && !(Confirmed && Mod.RequireReferences)) ||
          llvm::any_of(Mod.publicNames(), [&](const StoredName &Public) {
            return Public.Name == Names.at(It->first) && Public.Offset != 0;
          })) {
        Anchored = false;
        break;
      }
      const bool Independent =
          Weak || (Mod.RequireReferences && !Incoming && !Data);
      NeedsBinding |= Independent;
      NeedsSource |= Independent && !Incoming;
    }
    if (Anchored) {
      if (NeedsBinding)
        IndependentParents.emplace(It->first, NeedsSource);
      ++It;
    } else {
      It = At.erase(It);
    }
  }

  // Old packs can omit newly supported address references. A source record
  // may refine an existing full-body anchor only after its whole body and
  // outgoing constraints agree. These temporary rows never become output or
  // new anchors, and all validation reads the same frozen candidates.
  std::map<uint64_t, std::vector<size_t>> Refined;
  for (const auto &[Address, Proposals] : At) {
    const auto &Name = Names.at(Address);
    for (const auto &Candidate : Idx.callerModules(Modules, Name)) {
      const StoredModule &Mod = Modules[Candidate.Module];
      if (!Mod.RequireCaller ||
          (!IndependentParents.contains(Address) &&
           SignatureMatcher::fixedByteCount(Mod) <
               SignatureMatcher::MinStatedBytes) ||
          llvm::any_of(Proposals, [&](size_t I) {
            return Modules[I].TotalLen != Mod.TotalLen;
          }))
        continue;
      const uint8_t *Bytes = Img.readVA(Address, Mod.TotalLen);
      if (!Bytes || !SignatureMatcher::matchPattern(Mod, Bytes, Mod.TotalLen))
        continue;
      Refined[Address].push_back(Candidate.Module);
    }
  }
  using CandidateKey = std::pair<uint64_t, size_t>;
  struct Support {
    CandidateKey Candidate;
    std::string_view Name;
    uint64_t Parent;
    uint32_t Offset;
  };
  std::vector<Support> Supports;
  llvm::DenseSet<std::pair<uint64_t, uint32_t>> DataQueries;
  llvm::DenseMap<std::pair<uint64_t, size_t>, size_t> Tried;
  using Reference = std::pair<uint32_t, std::string_view>;
  auto ReferencesOf = [](const StoredModule &Mod) {
    llvm::SmallVector<Reference, 8> Result;
    for (const StoredName &Ref : Mod.references())
      if (!Ref.IsData)
        Result.emplace_back(Ref.Offset, Ref.Name);
    llvm::sort(Result);
    Result.erase(std::unique(Result.begin(), Result.end()), Result.end());
    return Result;
  };
  auto Constrain = [](llvm::SmallVector<Reference, 8> &References,
                      llvm::ArrayRef<Reference> Other, bool AllowMissing) {
    llvm::SmallVector<Reference, 8> Common;
    for (size_t R = 0; R < References.size();) {
      size_t End = R + 1;
      while (End < References.size() &&
             References[End].first == References[R].first)
        ++End;
      const auto BeginOther =
          llvm::lower_bound(Other, Reference{References[R].first, {}});
      auto EndOther = BeginOther;
      while (EndOther != Other.end() && EndOther->first == References[R].first)
        ++EndOther;
      if ((BeginOther == EndOther && AllowMissing) ||
          std::equal(References.begin() + R, References.begin() + End,
                     BeginOther, EndOther))
        Common.append(References.begin() + R, References.begin() + End);
      R = End;
    }
    References = std::move(Common);
  };
  for (const auto &[Address, Proposals] : At) {
    llvm::SmallVector<Reference, 8> References;
    bool First = true;
    const auto Extra = Refined.find(Address);
    for (size_t I : Extra == Refined.end() ? Proposals : Extra->second) {
      const StoredModule &Mod = Modules[I];
      if (Mod.ReferenceCount == 0) {
        References.clear();
        break;
      }
      // Every matching variant must state the same complete set of possible
      // callees at a site. Later body checks may leave one supported choice;
      // missing source records or several viable names cannot select one.
      auto Current = ReferencesOf(Mod);
      if (First) {
        References = std::move(Current);
        First = false;
      } else {
        Constrain(References, Current, /*AllowMissing=*/false);
      }
      if (References.empty())
        break;
    }
    if (Extra != Refined.end())
      for (size_t I : Proposals)
        Constrain(References, ReferencesOf(Modules[I]), /*AllowMissing=*/true);
    if (const auto Parent = IndependentParents.find(Address);
        Parent != IndependentParents.end()) {
      if (References.empty() || (Parent->second && Extra == Refined.end()))
        continue;
      if (llvm::none_of(References, [&](const Reference &Ref) {
            return !Idx.callerModules(Modules, Ref.second).empty();
          }))
        continue;
      // Reuse the settlement index; inspect body strength only at an actual
      // requested target rather than for every frozen candidate.
      if (!FrozenBodies)
        FrozenBodies.emplace();
      uint32_t ParentLength = 0;
      for (size_t I : Proposals)
        ParentLength = std::max(ParentLength, Modules[I].TotalLen);
      bool Bound = false;
      for (size_t R = 0; R < References.size();) {
        size_t End = R + 1;
        while (End < References.size() &&
               References[End].first == References[R].first)
          ++End;
        const auto [Offset, Expected] = References[R];
        const bool Single = End == R + 1;
        R = End;
        if (!Single)
          continue;
        const uint64_t Start = Img.Arch == Arch::ARM
                                   ? Address & ~uint64_t(ThumbStateBit)
                                   : Address;
        const auto Target = branchTarget(Img, Start, Offset);
        if (!Target || !Img.isCodeAddress(Target->Target))
          continue;
        const auto Name = FindName(Target->Target);
        if (Name == Names.end() || Name->second != Expected)
          continue;
        auto [Body, Fresh] = FrozenBodies->try_emplace(Target->Target, 0);
        if (Fresh) {
          const auto Begin = llvm::lower_bound(
              FrozenCandidates, Target->Target,
              [](const auto &Row, uint64_t VA) { return Row.first < VA; });
          llvm::SmallVector<const SigMatch *, 4> Claims;
          auto End = Begin;
          while (End != FrozenCandidates.end() && End->first == Target->Target)
            Claims.push_back(&Matches[(End++)->second]);
          const auto FrozenName = settle(Claims);
          // New incoming names cannot disambiguate their old nominal rows
          // and then reuse those rows as supposedly independent evidence.
          if (!FrozenName || FrozenName->Name != Expected)
            continue;
          for (auto Row = Begin; Row != End; ++Row) {
            const size_t I = Row->second;
            const SigMatch &Match = Matches[I];
            if (Match.Name != Expected &&
                !llvm::is_contained(Match.Aliases, Expected))
              continue;
            if (MatchModules[I] == NoModule) {
              Body->second = 0;
              break;
            }
            const StoredModule &Mod = Modules[MatchModules[I]];
            if (Mod.RequireCaller || Mod.RequireReferences ||
                Match.CallerConfirmed ||
                !SignatureMatcher::isFullyVerified(Mod) ||
                SignatureMatcher::fixedByteCount(Mod) <
                    SignatureMatcher::MinStatedBytes ||
                llvm::any_of(Mod.publicNames(), [&](const StoredName &N) {
                  return N.Name == Expected && N.Offset != 0;
                })) {
              Body->second = 0;
              break;
            }
            Body->second = std::max(Body->second, Mod.TotalLen);
          }
        }
        if (!Body->second)
          continue;
        if (Target->Target >= Address ? Target->Target - Address < ParentLength
                                      : Address - Target->Target < Body->second)
          continue;
        Bound = true;
        break;
      }
      if (!Bound)
        continue;
    }
    for (size_t R = 0; R < References.size();) {
      const uint32_t Offset = References[R].first;
      size_t End = R + 1;
      while (End < References.size() && References[End].first == Offset)
        ++End;
      const auto Alternatives = llvm::ArrayRef(References).slice(R, End - R);
      R = End;
      // Query the name index before decoding any image reference. Most calls
      // in a selected image have no incoming-context record at all.
      using CallerEntry = SignatureMatcher::HashIndex::CallerEntry;
      llvm::SmallVector<
          std::pair<std::string_view, llvm::ArrayRef<CallerEntry>>, 2>
          Queries;
      for (const auto &[UnusedOffset, Expected] : Alternatives) {
        const auto Candidates = Idx.callerModules(Modules, Expected);
        if (Candidates.empty()) {
          Queries.clear();
          break;
        }
        Queries.emplace_back(Expected, Candidates);
      }
      if (Queries.empty())
        continue;
      const uint64_t Start =
          Img.Arch == Arch::ARM ? Address & ~uint64_t(ThumbStateBit) : Address;
      std::optional<Branch> To = branchTarget(Img, Start, Offset);
      if (!To)
        if (const auto Target = addressReferenceTarget(Img, Start, Offset))
          To = Branch{*Target, false};
      if (!To || !Img.isCodeAddress(To->Target) ||
          !std::binary_search(Entries.begin(), Entries.end(), To->Target))
        continue;
      // Existing settled candidates already have a result. Do not duplicate
      // their work or modify the evidence used as a frozen anchor.
      if (FindName(To->Target) != Names.end())
        continue;
      if (DeferredCallers && !Data && Alternatives.size() > 1 &&
          llvm::all_of(Proposals,
                       [&](size_t I) {
                         return !Modules[I].RequireCaller &&
                                !Modules[I].RequireReferences;
                       }) &&
          llvm::any_of(Queries, [&](const auto &Query) {
            return llvm::any_of(Query.second, [&](const auto &Candidate) {
              return llvm::any_of(
                  Modules[Candidate.Module].references(),
                  [](const StoredName &Ref) { return Ref.IsData; });
            });
          }))
        DataQueries.insert({Address, Offset});
      for (const auto &[Expected, Candidates] : Queries) {
        for (const auto &Candidate : Candidates) {
          const StoredModule &Mod = Modules[Candidate.Module];
          if (Mod.RequireReferences &&
              llvm::any_of(Mod.references(),
                           [](const StoredName &Ref) { return Ref.IsData; }))
            continue;
          auto [Previous, Fresh] = Tried.try_emplace(
              std::pair{To->Target, Candidate.Module}, NoModule);
          if (!Fresh) {
            if (Previous->second != NoModule)
              Supports.push_back({Previous->first, Expected, Address, Offset});
            continue;
          }
          const uint8_t *Bytes = Img.readVA(To->Target, Mod.TotalLen);
          if (!Bytes ||
              !SignatureMatcher::matchPattern(Mod, Bytes, Mod.TotalLen))
            continue;
          SigMatch Match;
          Match.Address = To->Target;
          Match.LibraryName = libraryNameOf(Candidate.Module);
          Match.FuncLen = Mod.TotalLen;
          Match.Name = Expected;
          Match.CallerConfirmed = true;
          Previous->second = Matches.size();
          Matches.push_back(std::move(Match));
          MatchModules.push_back(Candidate.Module);
          Supports.push_back({Previous->first, Expected, Address, Offset});
        }
      }
    }
  }
  if (Matches.size() == OriginalMatches)
    return;
  // Only refine anchors that actually support a matched child. Validate both
  // in one frozen pass, then remove every name whose supporting refinements
  // were contradicted. Neither can certify the other through that pass.
  llvm::DenseSet<uint64_t> UsedRefinements;
  for (const Support &Claim : Supports) {
    const auto Extra = Refined.find(Claim.Parent);
    if (Extra == Refined.end() || !UsedRefinements.insert(Claim.Parent).second)
      continue;
    for (size_t Module : Extra->second) {
      SigMatch Match;
      Match.Address = Claim.Parent;
      Match.Name = Names.at(Claim.Parent);
      Matches.push_back(std::move(Match));
      MatchModules.push_back(Module);
    }
  }
  // Validate incoming bodies' own references with the same resolver. Existing
  // candidates are frozen; new candidates cannot certify one another or feed
  // back into the caller that requested them.
  checkReferences(Img, Entries, {}, FrozenMatches, OriginalMatches, Data);
  llvm::DenseSet<uint64_t> ValidRefinements;
  llvm::DenseSet<CandidateKey> Viable;
  for (size_t I = OriginalMatches; I < Matches.size(); ++I)
    if (UsedRefinements.contains(Matches[I].Address))
      ValidRefinements.insert(Matches[I].Address);
    else
      Viable.insert({Matches[I].Address, MatchModules[I]});
  llvm::DenseMap<std::pair<uint64_t, uint32_t>, std::string_view> SiteNames;
  for (const Support &Claim : Supports)
    if (Viable.contains(Claim.Candidate) &&
        (!UsedRefinements.contains(Claim.Parent) ||
         ValidRefinements.contains(Claim.Parent))) {
      auto [Site, First] = SiteNames.try_emplace(
          std::pair{Claim.Parent, Claim.Offset}, Claim.Name);
      if (!First && Site->second != Claim.Name)
        Site->second = {};
    }
  llvm::sort(Supports, [](const Support &A, const Support &B) {
    return A.Candidate < B.Candidate;
  });
  if (DeferredCallers && !DataQueries.empty())
    for (const Support &Claim : Supports) {
      const auto Site = std::pair{Claim.Parent, Claim.Offset};
      const auto Names = SiteNames.find(Site);
      if (Names == SiteNames.end() || !Names->second.empty() ||
          !DataQueries.contains(Site) || !Viable.contains(Claim.Candidate))
        continue;
      const auto &[Address, Module] = Claim.Candidate;
      for (const StoredName &Ref : Modules[Module].references()) {
        if (!Ref.IsData)
          continue;
        auto Target = dataReferenceTarget(Img, Address, Ref.Offset,
                                          Modules[Module].TotalLen);
        if (Target && Ref.IsLocalData)
          Target =
              localDataReferenceBase(Img, *Target, *localDataAnchor(Ref.Name));
        if (Target)
          DeferredCallers->push_back({Claim.Parent, *Target});
      }
    }
  size_t Kept = OriginalMatches;
  for (size_t I = OriginalMatches; I < Matches.size(); ++I) {
    SigMatch &Match = Matches[I];
    if (UsedRefinements.contains(Match.Address))
      continue;
    const CandidateKey Key{Match.Address, MatchModules[I]};
    auto Begin = llvm::lower_bound(Supports, Key,
                                   [](const Support &A, const CandidateKey &B) {
                                     return A.Candidate < B;
                                   });
    llvm::SmallVector<std::string_view, 4> SupportedNames;
    for (; Begin != Supports.end() && Begin->Candidate == Key; ++Begin)
      if (SiteNames.lookup({Begin->Parent, Begin->Offset}) == Begin->Name)
        SupportedNames.push_back(Begin->Name);
    if (SupportedNames.empty())
      continue;
    llvm::sort(SupportedNames, preferredAliasOrder);
    SupportedNames.erase(
        std::unique(SupportedNames.begin(), SupportedNames.end()),
        SupportedNames.end());
    Match.Name = SupportedNames.front();
    Match.Aliases.clear();
    for (std::string_view Name : llvm::drop_begin(SupportedNames))
      Match.Aliases.emplace_back(Name);
    if (Kept != I)
      Matches[Kept] = std::move(Match);
    MatchModules[Kept++] = MatchModules[I];
  }
  Matches.resize(Kept);
  MatchModules.resize(Kept);
}

void SignatureDB::checkReferences(const BinaryImage &Img,
                                  llvm::ArrayRef<uint64_t> Entries,
                                  std::vector<SignatureMatcher::Hit> Hits,
                                  std::optional<size_t> FrozenMatches,
                                  size_t FirstChecked,
                                  const DataBindings *Data) {
  const size_t Frozen = FrozenMatches.value_or(0);
  const size_t First = std::max(Frozen, FirstChecked);
  bool AnyReferences = false;
  for (size_t I = First; I < MatchModules.size(); ++I)
    AnyReferences |= MatchModules[I] != NoModule &&
                     Modules[MatchModules[I]].ReferenceCount != 0;
  if (!AnyReferences)
    return;
  auto ReferencesOf = [&](size_t I) {
    return MatchModules[I] == NoModule ? llvm::ArrayRef<StoredName>()
                                       : Modules[MatchModules[I]].references();
  };
  auto RequiresReferences = [&](size_t I) {
    return MatchModules[I] != NoModule &&
           Modules[MatchModules[I]].RequireReferences &&
           !Matches[I].CallerConfirmed;
  };
  auto ModuleStartOf = [&](size_t I) {
    const uint64_t Address = Matches[I].Address;
    for (const StoredName &Name : Modules[MatchModules[I]].publicNames())
      if (Name.Name == Matches[I].Name)
        return Address - Name.Offset;
    return Address;
  };

  // Where each reference of each match branches: the branch's target and,
  // when that is a routine that only jumps on, the routine it reaches.  The
  // references at one offset are one branch, which reaches one of the
  // routines they name: a COFF link resolves a symbol no object defines to
  // its alternate name.  A branch the image does not hold at its site is
  // none of them, and leaves its match unconfirmed.  Match I's are
  // Sites[FirstSite[I]] up to Sites[FirstSite[I + 1]], of Branches[I].
  struct Site {
    llvm::SmallVector<std::string_view, 1> Names;
    uint64_t Target = 0;
    std::optional<uint64_t> Onward;
  };
  // Index only when a matched reference actually uses an import slot.
  // Repeated checks then avoid a linear search of the import table. An
  // ambiguous or unmapped slot is insufficient evidence for either name.
  llvm::DenseMap<uint64_t, const Import *> ImportsBySlot;
  bool ImportsIndexed = false;
  auto ImportAtSlot = [&](uint64_t Slot) -> const Import * {
    if (!ImportsIndexed) {
      ImportsBySlot.reserve(Img.Imports.size());
      for (const Import &Imp : Img.Imports) {
        if (!Imp.IATAddr || !Img.readVA(Imp.IATAddr, Img.getPointerSize()))
          continue;
        auto [It, Fresh] = ImportsBySlot.try_emplace(Imp.IATAddr, &Imp);
        if (!Fresh)
          It->second = nullptr;
      }
      ImportsIndexed = true;
    }
    return ImportsBySlot.lookup(Slot);
  };
  std::vector<Site> Sites;
  std::vector<size_t> FirstSite(Matches.size() + 1, 0);
  std::vector<uint32_t> Branches(Matches.size(), 0);
  // Import and RTTI evidence is immutable within one application, unlike
  // provisional function candidates. Confirm it once without adding data slots
  // to the branch graph or growing its hot Site records. Old packs pay no
  // allocation.
  std::vector<uint32_t> ConfirmedMetadata;
  llvm::DenseSet<size_t> ContradictedMetadata;
  llvm::DenseMap<uint64_t, std::optional<llvm::StringRef>> VTableTypes;
  llvm::DenseMap<uint64_t, std::optional<coff_loader::ThrowInfoType>>
      ThrowTypes;
  auto ThrowTypeOf = [](llvm::StringRef Name) {
    return Name.starts_with("_TI") ? coff_loader::throwInfoTypeName(Name)
                                   : std::nullopt;
  };
  std::vector<uint8_t> Unconfirmable;
  for (size_t I = First; I < Matches.size(); ++I) {
    FirstSite[I] = Sites.size();
    llvm::SmallVector<StoredName, 8> References(ReferencesOf(I).begin(),
                                                ReferencesOf(I).end());
    if (References.empty())
      continue;
    llvm::stable_sort(References, [](const StoredName &A, const StoredName &B) {
      return A.Offset < B.Offset;
    });
    // Every public name of a module shares its references; the module's
    // start is the match address less the name's offset.
    uint64_t Start = ModuleStartOf(I);
    if (Img.Arch == Arch::ARM)
      Start &= ~uint64_t(ThumbStateBit);
    for (size_t R = 0; R < References.size();) {
      size_t End = R + 1;
      while (End < References.size() &&
             References[End].Offset == References[R].Offset)
        ++End;
      ++Branches[I];
      if (References[R].IsData) {
        if (Data)
          if (const auto Target =
                  dataReferenceTarget(Img, Start, References[R].Offset,
                                      Modules[MatchModules[I]].TotalLen)) {
            bool Known = false;
            const bool Confirmed = llvm::any_of(
                llvm::ArrayRef(References).slice(R, End - R),
                [&](const StoredName &Ref) {
                  // Validated exact keys need only their existing string
                  // comparison. Decode coordinates only for signed deltas.
                  const bool Relative =
                      Ref.IsLocalData &&
                      Ref.Name[local_data_syntax::
                                   LocalDataAnchorOffsetDigits] !=
                          local_data_syntax::LocalDataAnchorSeparator;
                  const auto Anchor =
                      Relative ? localDataAnchor(Ref.Name) : std::nullopt;
                  const auto Base =
                      Anchor ? localDataReferenceBase(Img, *Target, *Anchor)
                             : Target;
                  if (!Base)
                    return false;
                  const auto Bound = Data->find(*Base);
                  if (Bound == Data->end() || (Bound->second.Parent == Start &&
                                               !Bound->second.MultipleParents))
                    return false;
                  Known = true;
                  if (Ref.IsLocalData != Bound->second.IsLocalData)
                    return false;
                  if (!Anchor)
                    return Ref.Name == Bound->second.Name;
                  const auto BoundAnchor = localDataAnchor(Bound->second.Name);
                  // Query only the frozen exact operand. Relative consumers
                  // never establish bindings, including for another delta.
                  return !BoundAnchor->Delta &&
                         BoundAnchor->Offset == Anchor->Offset &&
                         BoundAnchor->Name == Anchor->Name;
                });
            if (Known) {
              if (Confirmed) {
                if (ConfirmedMetadata.empty())
                  ConfirmedMetadata.resize(Matches.size(), 0);
                ++ConfirmedMetadata[I];
              } else {
                ContradictedMetadata.insert(I);
              }
            }
          }
        R = End;
        continue;
      }
      const auto Alternatives = llvm::ArrayRef(References).slice(R, End - R);
      const bool IsThrowInfo =
          llvm::any_of(Alternatives, [&](const StoredName &Name) {
            return ThrowTypeOf(Name.Name).has_value();
          });
      std::optional<Branch> To =
          IsThrowInfo ? std::nullopt
                      : branchTarget(Img, Start, References[R].Offset);
      // Taking a named function's address carries the same exact target
      // constraint as calling it. Metadata symbols retain their data-specific
      // checks and cannot turn a vftable or import slot into executable code.
      if (!To && !IsThrowInfo &&
          llvm::none_of(llvm::ArrayRef(References).slice(R, End - R),
                        [](const StoredName &Name) {
                          return llvm::StringRef(Name.Name).starts_with(
                                     COFFImportPointerPrefix) ||
                                 coff_loader::primaryVTableTypeName(Name.Name)
                                     .has_value();
                        }))
        if (const auto Target =
                addressReferenceTarget(Img, Start, References[R].Offset))
          if (Img.isCodeAddress(*Target))
            To = Branch{*Target, false};
      if (To) {
        Site Where;
        for (size_t Alternative = R; Alternative < End; ++Alternative)
          Where.Names.push_back(References[Alternative].Name);
        Where.Target = To->Target;
        if (const std::optional<Branch> Next = thunkTarget(Img, *To))
          Where.Onward = Next->Target;
        Sites.push_back(std::move(Where));
      } else if (IsThrowInfo) {
        if (const auto Target =
                addressReferenceTarget(Img, Start, References[R].Offset)) {
          auto [At, Fresh] = ThrowTypes.try_emplace(*Target);
          if (Fresh)
            At->second = coff_loader::readThrowInfoType(Img, *Target);
          if (At->second) {
            bool Confirmed = false;
            bool AllTypes = true;
            for (const StoredName &Reference : Alternatives) {
              const auto Type = ThrowTypeOf(Reference.Name);
              AllTypes &= Type.has_value();
              Confirmed |= Type && *Type == *At->second;
            }
            if (Confirmed) {
              if (ConfirmedMetadata.empty())
                ConfirmedMetadata.resize(Matches.size(), 0);
              ++ConfirmedMetadata[I];
            } else if (AllTypes) {
              ContradictedMetadata.insert(I);
            }
          }
        }
      } else if (llvm::any_of(llvm::ArrayRef(References).slice(R, End - R),
                              [](const StoredName &Name) {
                                return llvm::StringRef(Name.Name).starts_with(
                                    COFFImportPointerPrefix);
                              })) {
        if (const auto Slot =
                importReferenceSlot(Img, Start, References[R].Offset)) {
          const Import *Imp = ImportAtSlot(*Slot);
          // Export and linker spellings can differ. Exact agreement confirms
          // this pointer; missing or different spellings remain unknown.
          if (Imp && !Imp->Name.empty() &&
              llvm::any_of(llvm::ArrayRef(References).slice(R, End - R),
                           [&](const StoredName &Reference) {
                             llvm::StringRef Name(Reference.Name);
                             return Name.consume_front(
                                        COFFImportPointerPrefix) &&
                                    Name == Imp->Name;
                           })) {
            if (ConfirmedMetadata.empty())
              ConfirmedMetadata.resize(Matches.size(), 0);
            ++ConfirmedMetadata[I];
          }
        }
      } else if (llvm::any_of(llvm::ArrayRef(References).slice(R, End - R),
                              [](const StoredName &Name) {
                                return coff_loader::primaryVTableTypeName(
                                           Name.Name)
                                    .has_value();
                              })) {
        if (const auto Target =
                addressReferenceTarget(Img, Start, References[R].Offset)) {
          auto [At, Fresh] = VTableTypes.try_emplace(*Target);
          if (Fresh)
            At->second = coff_loader::readPrimaryVTableTypeName(Img, *Target);
          if (At->second) {
            bool Confirmed = false;
            bool AllTypes = true;
            for (size_t Alternative = R; Alternative < End; ++Alternative) {
              const auto Type = coff_loader::primaryVTableTypeName(
                  References[Alternative].Name);
              AllTypes &= Type.has_value();
              Confirmed |= Type && *Type == *At->second;
            }
            if (Confirmed) {
              if (ConfirmedMetadata.empty())
                ConfirmedMetadata.resize(Matches.size(), 0);
              ++ConfirmedMetadata[I];
            } else if (AllTypes) {
              ContradictedMetadata.insert(I);
            }
          }
        }
      }
      R = End;
    }
    if (RequiresReferences(I) &&
        (ContradictedMetadata.contains(I) ||
         Branches[I] !=
             Sites.size() - FirstSite[I] +
                 (ConfirmedMetadata.empty() ? 0 : ConfirmedMetadata[I]))) {
      // Every reference must either have immutable metadata confirmation or
      // an executable target whose candidates can change. No graph iteration
      // can repair a missing/unsupported field or absent import/type evidence.
      if (Unconfirmable.empty())
        Unconfirmable.resize(Matches.size(), 0);
      Unconfirmable[I] = 1;
    }
  }
  FirstSite[Matches.size()] = Sites.size();

  // The matches whose references branch to each address, and the matches at
  // each address, as sorted (address, match) pairs.
  using Entry = std::pair<uint64_t, size_t>;
  // A raw byte hit cannot confirm another candidate while its own references
  // contradict it. The set changes only between parallel check rounds.
  llvm::DenseSet<Entry> DisprovedPatterns;
  std::vector<Entry> Calling, At;
  // Only the checked suffix owns outgoing sites. Frozen prefix settlement is
  // collected below, at the targets that these sites actually read.
  for (size_t I = First; I < Matches.size(); ++I) {
    if (!Unconfirmable.empty() && Unconfirmable[I])
      continue;
    if (!FrozenMatches)
      At.emplace_back(Matches[I].Address, I);
    for (size_t S = FirstSite[I]; S < FirstSite[I + 1]; ++S) {
      Calling.emplace_back(Sites[S].Target, I);
      if (Sites[S].Onward)
        Calling.emplace_back(*Sites[S].Onward, I);
    }
  }
  llvm::sort(Calling);
  Calling.erase(std::unique(Calling.begin(), Calling.end()), Calling.end());
  auto Range = [](const std::vector<Entry> &Pairs, uint64_t Address) {
    const auto Begin =
        std::lower_bound(Pairs.begin(), Pairs.end(), Entry{Address, 0});
    auto End = Begin;
    while (End != Pairs.end() && End->first == Address)
      ++End;
    return llvm::make_range(Begin, End);
  };
  if (FrozenMatches.has_value() && !Calling.empty()) {
    // Frozen validation only reads settlement at actually referenced targets.
    // Newly appended matches cannot be evidence, and unrelated original
    // matches need no grouping or sorting in every incoming/data pass.
    for (size_t I = 0; I < Frozen; ++I) {
      const uint64_t Address = Matches[I].Address;
      if (!Range(Calling, Address).empty() ||
          (Img.Arch == Arch::ARM &&
           !Range(Calling, Address & ~uint64_t(ThumbStateBit)).empty()))
        At.emplace_back(Address, I);
    }
  }
  llvm::sort(At);

  // The modules whose patterns match where a reference branches, to confirm
  // a reference by the named routine's own pattern: at an entry, the ones
  // that matched there; at any other target, the ones the index finds.
  std::vector<uint64_t> Untried;
  if (!Calling.empty()) {
    // Metadata-only checks have no code targets. Other frozen checks can
    // reuse the already sorted entry view without copying it every round.
    llvm::ArrayRef<uint64_t> Tried = Entries;
    std::vector<uint64_t> SortedEntries;
    if (!llvm::is_sorted(Tried)) {
      SortedEntries.assign(Tried.begin(), Tried.end());
      llvm::sort(SortedEntries);
      Tried = SortedEntries;
    }
    for (size_t C = 0; C < Calling.size(); ++C)
      if ((C == 0 || Calling[C].first != Calling[C - 1].first) &&
          !std::binary_search(Tried.begin(), Tried.end(), Calling[C].first))
        Untried.push_back(Calling[C].first);
  }
  if (!Untried.empty())
    for (const Segment &Seg : Img.Segments)
      if (Seg.isExecutable() && !Seg.Data.empty())
        for (const SignatureMatcher::Hit &Hit :
             SignatureMatcher::findAtAddresses(Seg.Data.data(), Seg.Data.size(),
                                               Seg.VA, Untried, Modules,
                                               index()))
          Hits.push_back(Hit);
  std::vector<Entry> PatternsAt;
  PatternsAt.reserve(Hits.size());
  for (const SignatureMatcher::Hit &Hit : Hits)
    // Required hits support callers only after settlement. Do not sort or
    // search them again as raw evidence that PatternAt cannot use.
    if (!Modules[Hit.Module].RequireReferences &&
        !Modules[Hit.Module].RequireCaller && !Modules[Hit.Module].IsVTable &&
        !Modules[Hit.Module].IsLoadConfig)
      PatternsAt.emplace_back(Hit.Address, Hit.Module);
  llvm::sort(PatternsAt);
  auto PatternAt = [&](std::string_view Name, uint64_t Address) {
    for (const Entry &Hit : Range(PatternsAt, Address)) {
      // Context-dependent patterns cannot bootstrap each other from bytes.
      // Only their confirmed settlement can support a caller; an unvisited
      // target's contextual byte hit remains insufficient evidence.
      for (const StoredName &Public : Modules[Hit.second].publicNames())
        if (Public.Offset == 0 && Public.Name == Name &&
            !DisprovedPatterns.contains(Hit))
          return true;
    }
    return false;
  };

  // What the matches settle where a reference branches, which is what a
  // reference is checked against: an address two matches name differently
  // names nothing.  At first that is what the bytes alone settle.  A Thumb
  // routine may be entered with the interworking bit set.
  std::unordered_map<uint64_t, SettledRoutine> Settled;
  // Import veneers are immutable during this application. Resolve each
  // referenced address once before parallel rounds rather than decoding it
  // again for every candidate name and every round. Only actual imports need
  // retained entries; absence has the same meaning as an uncached null result.
  llvm::DenseMap<uint64_t, const Import *> ImportStubs;
  std::vector<bool> Dropped(Matches.size(), false);
  for (size_t I = First; I < Matches.size(); ++I)
    Dropped[I] = RequiresReferences(I);
  auto SettleAt = [&](uint64_t Address) {
    llvm::SmallVector<const SigMatch *, 4> Proposals;
    for (const Entry &Match : Range(At, Address))
      if (!Dropped[Match.second] && (!FrozenMatches || Match.second < Frozen))
        Proposals.push_back(&Matches[Match.second]);
    return settle(Proposals);
  };
  for (size_t C = 0; C < Calling.size(); ++C) {
    if (C != 0 && Calling[C].first == Calling[C - 1].first)
      continue;
    const uint64_t Target = Calling[C].first;
    if (const Import *Imp = Img.findImportStubAt(Target))
      ImportStubs.try_emplace(Target, Imp);
    for (uint64_t Address :
         {Target, Img.Arch == Arch::ARM ? Target | ThumbStateBit : Target})
      if (!Settled.count(Address))
        if (std::optional<SettledRoutine> Routine = SettleAt(Address))
          Settled.emplace(Address, std::move(*Routine));
  }
  auto SettledAt = [&](uint64_t Target) {
    auto It = Settled.find(Target);
    if (It == Settled.end() && Img.Arch == Arch::ARM)
      It = Settled.find(Target | ThumbStateBit);
    return It;
  };

  auto Judge = [&](std::string_view Name, uint64_t Target) {
    if (const Import *Imp = ImportStubs.lookup(Target)) {
      // An ELF import is the very symbol the library called, named without
      // the version a `name@VERSION` reference states, and a Mach-O stub is
      // named after the undefined symbol it binds, so either stub settles the
      // call.  A COFF import thunk is named after the import, not after the
      // decorated symbol the library called; it settles nothing.
      if (Img.isMachO())
        return Imp->Name == Name ? ReferenceVerdict::Confirmed
                                 : ReferenceVerdict::Contradicted;
      if (!Img.isELF())
        return ReferenceVerdict::Unknown;
      return Imp->Name == Name.substr(0, Name.find(ELFVersionSeparator))
                 ? ReferenceVerdict::Confirmed
                 : ReferenceVerdict::Contradicted;
    }
    // A library calls a routine by whichever of its names it uses, so any
    // name the routine settled with confirms the call.
    if (const auto It = SettledAt(Target); It != Settled.end())
      return It->second.hasName(Name) ? ReferenceVerdict::Confirmed
                                      : ReferenceVerdict::Contradicted;
    // A routine the image replaced (operator new, say) does not match the
    // library's pattern and is still the routine called, so a pattern that
    // does not match contradicts nothing.
    return PatternAt(Name, Target) ? ReferenceVerdict::Confirmed
                                   : ReferenceVerdict::Unknown;
  };

  // Whether match \p I is contradicted, and whether every one of its
  // branches is confirmed.
  enum class Outcome : uint8_t { Unconfirmed, Confirmed, Contradicted };
  auto Check = [&](size_t I) {
    if (ContradictedMetadata.contains(I))
      return Outcome::Contradicted;
    size_t Confirmed = ConfirmedMetadata.empty() ? 0 : ConfirmedMetadata[I];
    for (size_t S = FirstSite[I]; S < FirstSite[I + 1]; ++S) {
      const Site &Where = Sites[S];
      // Any routine the branch may reach confirms it; it contradicts the
      // match only when the routine it reaches is none of them.
      ReferenceVerdict Verdict = ReferenceVerdict::Contradicted;
      for (std::string_view Name : Where.Names) {
        const ReferenceVerdict One = Judge(Name, Where.Target);
        if (One == ReferenceVerdict::Confirmed) {
          Verdict = One;
          break;
        }
        if (One == ReferenceVerdict::Unknown)
          Verdict = One;
      }
      // A routine that only jumps on is a thunk, or a routine that
      // tail-calls another -- `free` that jumps to `_free_base`, `operator
      // delete` to `free` -- and the bytes cannot tell which.  So the
      // routine it reaches confirms the reference when it is the one named,
      // and otherwise contradicts nothing.
      if (Verdict == ReferenceVerdict::Unknown && Where.Onward &&
          llvm::any_of(Where.Names, [&](std::string_view Name) {
            return Judge(Name, *Where.Onward) == ReferenceVerdict::Confirmed;
          }))
        Verdict = ReferenceVerdict::Confirmed;
      if (Verdict == ReferenceVerdict::Contradicted)
        return Outcome::Contradicted;
      Confirmed += Verdict == ReferenceVerdict::Confirmed;
    }
    return Confirmed != 0 && Confirmed == Branches[I] ? Outcome::Confirmed
                                                      : Outcome::Unconfirmed;
  };

  // A routine the references name is one its callers' references can be
  // checked against: a leaf that calls an import tells two same-byte
  // instantiations apart, and then so does every routine that calls one of
  // them.  So the check repeats, each round against what the last one
  // settled, until a round changes no evidence. A round checks only callers
  // of changed targets. Both settlement and raw-pattern availability matter:
  // a rejected pattern may have confirmed a caller even at an unnamed target.
  // Contradictions derived from provisional names can also be withdrawn, so
  // rejected callers must be reconsidered when that evidence changes.
  // The first round checks every match with references; a round with many
  // to check checks them in parallel, against what the last one settled.
  std::vector<size_t> Work;
  for (size_t I = First; I < Matches.size(); ++I)
    if (!ReferencesOf(I).empty() &&
        (Unconfirmable.empty() || !Unconfirmable[I]))
      Work.push_back(I);
  std::vector<size_t> Next;
  std::vector<uint8_t> Queued(Matches.size(), 0);
  auto SortUnique = [](auto &Values) {
    llvm::sort(Values);
    Values.erase(std::unique(Values.begin(), Values.end()), Values.end());
  };
  for (size_t Round = 0; Round < MaxReferenceRounds && !Work.empty(); ++Round) {
    std::vector<Outcome> Outcomes(Work.size());
    if (Work.size() >= ParallelReferenceMatches) {
      const size_t Blocks =
          llvm::divideCeil(Work.size(), ReferenceMatchesPerBlock);
      neverd::parallelForEach(Blocks, [&](auto Claim, size_t Total) {
        for (size_t B = Claim(); B < Total; B = Claim()) {
          const size_t End =
              std::min(Work.size(), (B + 1) * ReferenceMatchesPerBlock);
          for (size_t W = B * ReferenceMatchesPerBlock; W < End; ++W)
            Outcomes[W] = Check(Work[W]);
        }
      });
    } else {
      for (size_t W = 0; W < Work.size(); ++W)
        Outcomes[W] = Check(Work[W]);
    }

    std::vector<uint64_t> Touched;
    std::vector<uint64_t> ChangedPatterns;
    for (size_t W = 0; W < Work.size(); ++W) {
      const size_t I = Work[W];
      const bool Confirmed = Outcomes[W] == Outcome::Confirmed;
      const bool Contradicted = Outcomes[W] == Outcome::Contradicted ||
                                (RequiresReferences(I) && !Confirmed);
      if (Contradicted == Dropped[I] && Confirmed == Matches[I].Confirmed)
        continue;
      if (Contradicted != Dropped[I]) {
        Dropped[I] = Contradicted;
        const uint64_t Start = ModuleStartOf(I);
        const Entry Pattern{Start, MatchModules[I]};
        const bool Changed = Contradicted
                                 ? DisprovedPatterns.insert(Pattern).second
                                 : DisprovedPatterns.erase(Pattern);
        if (Changed) {
          ChangedPatterns.push_back(Start);
          Touched.push_back(Start);
        }
      }
      Matches[I].Confirmed = Confirmed;
      Touched.push_back(Matches[I].Address);
    }

    if (FrozenMatches.has_value()) {
      Work.clear();
      break;
    }
    SortUnique(Touched);
    SortUnique(ChangedPatterns);
    Next.clear();
    for (uint64_t Address : Touched) {
      // SettledAt reads a Thumb routine's settlement for the address without
      // its interworking bit too; an address no reference reads is not kept.
      const uint64_t Even =
          Img.Arch == Arch::ARM ? Address & ~uint64_t(ThumbStateBit) : Address;
      if (Range(Calling, Address).empty() && Range(Calling, Even).empty())
        continue;
      std::optional<SettledRoutine> Routine = SettleAt(Address);
      const auto Old = Settled.find(Address);
      const bool Same = Routine
                            ? Old != Settled.end() && Old->second == *Routine
                            : Old == Settled.end();
      // Judge consults raw patterns only when the target has no settled name.
      // With an unchanged name, withdrawing an unused pattern changes nothing
      // for its callers and must not enqueue them again.
      if (Same &&
          (Routine || !std::binary_search(ChangedPatterns.begin(),
                                          ChangedPatterns.end(), Address)))
        continue;
      if (!Same) {
        if (Routine)
          Settled.insert_or_assign(Address, std::move(*Routine));
        else
          Settled.erase(Old);
      }
      for (uint64_t Callee : {Address, Even})
        for (const Entry &Caller : Range(Calling, Callee))
          if (!Queued[Caller.second]) {
            Queued[Caller.second] = 1;
            Next.push_back(Caller.second);
          }
    }
    llvm::sort(Next);
    for (size_t I : Next)
      Queued[I] = 0;
    Work.swap(Next);
  }

  // Cyclic evidence can oscillate, and a call chain can exceed the round
  // budget. No partially checked name at those addresses, or at a dependent
  // caller, may escape. Suppress every proposal at an affected address so a
  // remaining twin does not acquire a name just because its rival was removed.
  if (!Work.empty()) {
    std::unordered_set<uint64_t> Unsettled;
    std::vector<uint64_t> Pending;
    auto MarkUnsettled = [&](size_t I) {
      for (uint64_t Address : {Matches[I].Address, ModuleStartOf(I)})
        if (Unsettled.insert(Address).second)
          Pending.push_back(Address);
    };
    for (size_t I : Work)
      MarkUnsettled(I);
    for (size_t P = 0; P < Pending.size(); ++P) {
      const uint64_t Address = Pending[P];
      const uint64_t Even =
          Img.Arch == Arch::ARM ? Address & ~uint64_t(ThumbStateBit) : Address;
      for (uint64_t Callee : {Address, Even})
        for (const Entry &Caller : Range(Calling, Callee))
          MarkUnsettled(Caller.second);
    }
    for (size_t I = 0; I < Matches.size(); ++I)
      if (Unsettled.contains(Matches[I].Address))
        Dropped[I] = true;
  }

  if (FrozenMatches.has_value()) {
    // Only the newly checked suffix can change. Keep the frozen prefix and
    // earlier validated incoming rows in place instead of moving the whole
    // image's matches into fresh allocations.
    size_t Kept = First;
    for (size_t I = First; I < Matches.size(); ++I)
      if (!Dropped[I]) {
        if (Kept != I) {
          Matches[Kept] = std::move(Matches[I]);
          MatchModules[Kept] = MatchModules[I];
        }
        ++Kept;
      }
    Matches.resize(Kept);
    MatchModules.resize(Kept);
    return;
  }
  std::vector<SigMatch> Kept;
  std::vector<size_t> KeptModules;
  Kept.reserve(Matches.size());
  KeptModules.reserve(Matches.size());
  for (size_t I = 0; I < Matches.size(); ++I)
    if (!Dropped[I]) {
      Kept.push_back(std::move(Matches[I]));
      KeptModules.push_back(MatchModules[I]);
    }
  Matches = std::move(Kept);
  MatchModules = std::move(KeptModules);
}
