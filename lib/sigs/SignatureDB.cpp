//===- SignatureDB.cpp - Signature database manager -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/SignatureDB.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/sigs/PatternParser.h"
#include "neverd/sigs/SignatureMatcher.h"

#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <thread>
#include <unordered_set>

using namespace neverd;
using namespace neverd::sigs;

void SignatureDB::commitSource(std::vector<PatternModule> &&Mods,
                               const std::string &LibName,
                               const std::string &FilePath) {
  auto Existing = std::find_if(
      LoadedFiles.begin(), LoadedFiles.end(),
      [&](const SigSource &Source) { return Source.Path == FilePath; });
  if (Existing != LoadedFiles.end()) {
    const size_t Start = Existing->ModuleStart;
    Modules.erase(Modules.begin() + Start,
                  Modules.begin() + Start + Existing->ModuleCount);
    Modules.insert(Modules.begin() + Start,
                   std::make_move_iterator(Mods.begin()),
                   std::make_move_iterator(Mods.end()));
    Existing->LibraryName = LibName;
    Existing->ModuleCount = Mods.size();

    size_t ModuleStart = 0;
    for (SigSource &Source : LoadedFiles) {
      Source.ModuleStart = ModuleStart;
      ModuleStart += Source.ModuleCount;
    }
    Matches.clear();
    return;
  }

  SigSource Src;
  Src.Path = FilePath;
  Src.LibraryName = LibName;
  Src.ModuleStart = Modules.size();
  Src.ModuleCount = Mods.size();
  LoadedFiles.push_back(std::move(Src));

  Modules.insert(Modules.end(), std::make_move_iterator(Mods.begin()),
                 std::make_move_iterator(Mods.end()));
  Matches.clear();
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
  auto Ext = Path.extension().string();

  if (Ext == ".pat") {
    auto ModsOrErr = PatternParser::parseFile(Path);
    if (!ModsOrErr)
      return ModsOrErr.takeError();
    std::string LibName = Path.stem().string();
    commitSource(std::move(*ModsOrErr), LibName, Path.string());
    return llvm::Error::success();
  }

  return llvm::make_error<llvm::StringError>(
      "unsupported signature file format: " + Path.string(),
      llvm::inconvertibleErrorCode());
}

llvm::Error SignatureDB::loadPatternText(llvm::StringRef Text,
                                         llvm::StringRef LibraryName) {
  auto ModulesOrErr = PatternParser::parseText(Text);
  if (!ModulesOrErr)
    return ModulesOrErr.takeError();
  commitSource(std::move(*ModulesOrErr), LibraryName.str(), LibraryName.str());
  return llvm::Error::success();
}

llvm::Error SignatureDB::loadDirectory(const std::filesystem::path &Dir) {
  std::error_code EC;
  if (!std::filesystem::exists(Dir, EC)) {
    if (EC)
      return llvm::make_error<llvm::StringError>(
          "cannot inspect signature directory: " + Dir.string() + ": " +
              EC.message(),
          llvm::inconvertibleErrorCode());
    return llvm::make_error<llvm::StringError>(
        "signature directory does not exist: " + Dir.string(),
        llvm::inconvertibleErrorCode());
  }
  if (!std::filesystem::is_directory(Dir, EC)) {
    if (EC)
      return llvm::make_error<llvm::StringError>(
          "cannot inspect signature directory: " + Dir.string() + ": " +
              EC.message(),
          llvm::inconvertibleErrorCode());
    return llvm::make_error<llvm::StringError>(
        "signature path is not a directory: " + Dir.string(),
        llvm::inconvertibleErrorCode());
  }

  // Collect all .pat files first, then parse in parallel.
  std::vector<std::filesystem::path> PatFiles;
  std::filesystem::directory_iterator It(Dir, EC);
  const std::filesystem::directory_iterator End;
  if (EC)
    return llvm::make_error<llvm::StringError>(
        "cannot enumerate signature directory: " + Dir.string() + ": " +
            EC.message(),
        llvm::inconvertibleErrorCode());
  while (It != End) {
    std::error_code TypeError;
    const bool IsRegular = It->is_regular_file(TypeError);
    if (TypeError)
      return llvm::make_error<llvm::StringError>(
          "cannot inspect signature entry: " + It->path().string() + ": " +
              TypeError.message(),
          llvm::inconvertibleErrorCode());
    if (IsRegular && It->path().extension() == ".pat")
      PatFiles.push_back(It->path());
    It.increment(EC);
    if (EC)
      return llvm::make_error<llvm::StringError>(
          "cannot enumerate signature directory: " + Dir.string() + ": " +
              EC.message(),
          llvm::inconvertibleErrorCode());
  }
  std::sort(PatFiles.begin(), PatFiles.end());

  struct ParsedFile {
    std::filesystem::path Path;
    std::vector<PatternModule> Modules;
    std::string ErrorMessage;
  };
  std::vector<ParsedFile> Parsed(PatFiles.size());
  for (size_t I = 0; I < PatFiles.size(); ++I)
    Parsed[I].Path = PatFiles[I];

  const size_t NumThreads = std::min(
      PatFiles.size(),
      static_cast<size_t>(std::max(1u, std::thread::hardware_concurrency())));
  std::atomic<size_t> NextIdx{0};
  std::vector<std::thread> Workers;
  Workers.reserve(NumThreads);
  for (size_t T = 0; T < NumThreads; ++T) {
    Workers.emplace_back([&]() {
      while (true) {
        const size_t I = NextIdx.fetch_add(1, std::memory_order_relaxed);
        if (I >= Parsed.size())
          break;
        auto ModulesOrErr = PatternParser::parseFile(Parsed[I].Path);
        if (!ModulesOrErr) {
          Parsed[I].ErrorMessage = llvm::toString(ModulesOrErr.takeError());
          continue;
        }
        Parsed[I].Modules = std::move(*ModulesOrErr);
      }
    });
  }
  for (std::thread &Worker : Workers)
    Worker.join();

  for (const ParsedFile &File : Parsed) {
    if (File.ErrorMessage.empty())
      continue;
    return llvm::make_error<llvm::StringError>(
        "cannot parse signature file: " + File.Path.string() + ": " +
            File.ErrorMessage,
        llvm::inconvertibleErrorCode());
  }

  std::vector<PatternModule> NewModules;
  std::vector<SigSource> NewSources;
  for (ParsedFile &File : Parsed) {
    SigSource Source;
    Source.Path = File.Path.string();
    Source.LibraryName = File.Path.stem().string();
    Source.ModuleStart = NewModules.size();
    Source.ModuleCount = File.Modules.size();
    NewSources.push_back(std::move(Source));
    NewModules.insert(NewModules.end(),
                      std::make_move_iterator(File.Modules.begin()),
                      std::make_move_iterator(File.Modules.end()));
  }
  Modules.swap(NewModules);
  LoadedFiles.swap(NewSources);
  Matches.clear();
  return llvm::Error::success();
}

void SignatureDB::apply(const BinaryImage &Img,
                        const std::vector<uint64_t> &FuncEntries) {
  Matches.clear();
  if (Modules.empty() || FuncEntries.empty())
    return;

  SignatureMatcher::HashIndex Index;
  Index.build(Modules);

  // Collect executable segment data for matching.
  for (const auto &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.empty())
      continue;

    SignatureMatcher::scanAtAddresses(
        Seg.Data.data(), Seg.Data.size(), Seg.VA, FuncEntries, Modules, Index,
        [&](uint64_t Addr, const PatternModule &Mod) {
          const size_t ModIdx = static_cast<size_t>(&Mod - Modules.data());
          for (const auto &Ref : Mod.PublicNames) {
            if (Ref.Offset > std::numeric_limits<uint64_t>::max() - Addr)
              continue;
            SigMatch M;
            M.Address = Addr + Ref.Offset;
            M.Name = Ref.Name;
            M.LibraryName = libraryNameOf(ModIdx);
            M.FuncLen = Mod.TotalLen;
            Matches.push_back(std::move(M));
          }
        });
  }
}

namespace {

/// The fewest bytes a module may state exactly and still be allowed to name a
/// personality routine.
///
/// Whole-function agreement is the main gate, but on its own it would also be
/// satisfied by a short pattern that is mostly wildcards -- a thing that
/// agrees with a great deal of code.  Sixteen exact bytes is several
/// instructions of one specific routine, which no unrelated function reaches
/// by coincidence.
constexpr size_t kMinFixedBytesForPersonality = 16;

} // namespace

size_t SignatureDB::identifyPersonalityRoutines(BinaryImage &Img) {
  const std::vector<va_t> Candidates = collectUnnamedPersonalityRoutines(Img);
  if (Candidates.empty() || Modules.empty())
    return 0;

  // One proposal per address, and the addresses two modules disagree about
  // recorded so they can be dropped: a routine that two signatures name
  // differently is a routine neither of them has identified.
  std::map<uint64_t, SigMatch> Proposed;
  std::set<uint64_t> Disputed;
  SignatureMatcher::HashIndex Index;
  Index.build(Modules);

  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.empty())
      continue;

    SignatureMatcher::scanAtAddresses(
        Seg.Data.data(), Seg.Data.size(), Seg.VA, Candidates, Modules, Index,
        [&](uint64_t Addr, const PatternModule &Mod) {
          if (!SignatureMatcher::isFullyVerified(Mod) ||
              SignatureMatcher::fixedByteCount(Mod) <
                  kMinFixedBytesForPersonality)
            return;

          for (const FuncRef &Ref : Mod.PublicNames) {
            // A name at a non-zero offset belongs to some other function the
            // module also describes, not to the routine being identified.
            if (Ref.Offset != 0)
              continue;
            const ExceptionPersonality P = classifyPersonalityName(Ref.Name);
            if (P == ExceptionPersonality::None ||
                P == ExceptionPersonality::Unknown)
              continue;

            SigMatch M;
            M.Address = Addr;
            M.Name = Ref.Name;
            M.LibraryName =
                libraryNameOf(static_cast<size_t>(&Mod - Modules.data()));
            M.FuncLen = Mod.TotalLen;
            auto [It, Fresh] = Proposed.emplace(Addr, std::move(M));
            if (!Fresh && It->second.Name != Ref.Name)
              Disputed.insert(Addr);
          }
        });
  }

  size_t Named = 0;
  for (const auto &[Addr, Match] : Proposed) {
    if (Disputed.count(Addr))
      continue;
    if (!adoptPersonalityRoutineName(Img, Addr, Match.Name))
      continue;
    Matches.push_back(Match);
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

std::unordered_map<uint64_t, std::string> SignatureDB::buildNameMap() const {
  std::unordered_map<uint64_t, std::string> Map;
  std::unordered_set<uint64_t> Disputed;
  for (const auto &M : Matches) {
    if (Disputed.count(M.Address))
      continue;
    auto [It, Inserted] = Map.emplace(M.Address, M.Name);
    if (!Inserted && It->second != M.Name) {
      Map.erase(It);
      Disputed.insert(M.Address);
    }
  }
  return Map;
}

void SignatureDB::clear() {
  Modules.clear();
  LoadedFiles.clear();
  Matches.clear();
}
