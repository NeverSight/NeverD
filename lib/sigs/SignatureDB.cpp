//===- SignatureDB.cpp - Signature database manager -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/SignatureDB.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/RichHeader.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/sigs/PatternParser.h"
#include "neverd/sigs/SignatureMatcher.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
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
    clearMatches();
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
  clearMatches();
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

llvm::Expected<std::vector<std::filesystem::path>>
SignatureDB::listDirectory(const std::filesystem::path &Dir) {
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
  return PatFiles;
}

std::vector<std::filesystem::path>
SignatureDB::selectForImage(const BinaryImage &Img,
                            std::vector<std::filesystem::path> Files) {
  if (!Img.COFFRichHeader)
    return Files;
  const std::vector<unsigned> Years = richToolsetYears(*Img.COFFRichHeader);
  if (Years.empty())
    return Files;

  // "vs2026.pat" belongs to Visual Studio 2026; other names to no release.
  auto ReleaseOf = [](const std::filesystem::path &Path) {
    const std::string Stem = Path.stem().string();
    unsigned Year = 0;
    if (Stem.size() != 6 || llvm::StringRef(Stem).take_front(2) != "vs" ||
        llvm::StringRef(Stem).drop_front(2).getAsInteger(10, Year))
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

llvm::Error SignatureDB::loadDirectory(const std::filesystem::path &Dir) {
  auto PatFiles = listDirectory(Dir);
  if (!PatFiles)
    return PatFiles.takeError();
  return loadFiles(*PatFiles);
}

llvm::Error
SignatureDB::loadFiles(const std::vector<std::filesystem::path> &PatFiles) {
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
  clearMatches();
  return llvm::Error::success();
}

void SignatureDB::apply(const BinaryImage &Img,
                        const std::vector<uint64_t> &FuncEntries) {
  clearMatches();
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
            MatchModules.push_back(ModIdx);
          }
        });
  }
  checkReferences(Img);
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
  SignatureMatcher::HashIndex Index;
  Index.build(Modules);

  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.empty())
      continue;

    SignatureMatcher::scanAtAddresses(
        Seg.Data.data(), Seg.Data.size(), Seg.VA, Candidates, Modules, Index,
        [&](uint64_t Addr, const PatternModule &Mod) {
          // Whole-function agreement is the main gate, but on its own it
          // would also be satisfied by a short pattern that is mostly
          // wildcards.
          if (!SignatureMatcher::isFullyVerified(Mod) ||
              SignatureMatcher::fixedByteCount(Mod) <
                  SignatureMatcher::MinStatedBytes)
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
    MatchModules.push_back(NoModule);
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
  // Every name proposed for an address, and whether a match with confirmed
  // references proposed it.
  std::unordered_map<uint64_t, std::map<std::string, bool>> Proposed;
  for (size_t I = 0; I < Matches.size(); ++I) {
    bool &Confirmed = Proposed[Matches[I].Address][Matches[I].Name];
    Confirmed = Confirmed || Matches[I].Confirmed;
  }
  std::unordered_map<uint64_t, std::string> Map;
  for (const auto &[Address, Names] : Proposed) {
    if (Names.size() == 1) {
      Map.emplace(Address, Names.begin()->first);
      continue;
    }
    const std::string *Settled = nullptr;
    size_t ConfirmedNames = 0;
    for (const auto &[Name, Confirmed] : Names)
      if (Confirmed) {
        Settled = &Name;
        ++ConfirmedNames;
      }
    if (ConfirmedNames == 1)
      Map.emplace(Address, *Settled);
  }
  return Map;
}

void SignatureDB::clear() {
  Modules.clear();
  LoadedFiles.clear();
  clearMatches();
}

void SignatureDB::clearMatches() {
  Matches.clear();
  MatchModules.clear();
}

namespace {

enum class ReferenceVerdict { Unknown, Confirmed, Contradicted };

int64_t signExtend(uint64_t Value, unsigned Bits) {
  const uint64_t Sign = uint64_t(1) << (Bits - 1);
  return static_cast<int64_t>((Value ^ Sign) - Sign);
}

uint64_t wrapToImage(const BinaryImage &Img, uint64_t Address) {
  return Img.is64Bit() ? Address : Address & 0xFFFFFFFFu;
}

/// The target of a Thumb-2 B.W (T4), BL (T1) or BLX (T2) at \p Address, or
/// of only a B.W when \p JumpOnly.
std::optional<uint64_t> thumbBranchTarget(const BinaryImage &Img,
                                          uint64_t Address, bool JumpOnly) {
  const uint8_t *Insn = Img.readVA(Address, 4);
  if (!Insn)
    return std::nullopt;
  const uint16_t First = readLE<uint16_t>(Insn);
  const uint16_t Second = readLE<uint16_t>(Insn + 2);
  if ((First & 0xF800) != 0xF000)
    return std::nullopt;
  const unsigned Kind = Second & 0xD000;
  const bool Jump = Kind == 0x9000, Link = Kind == 0xD000,
             Exchange = Kind == 0xC000;
  if (!(Jump || (!JumpOnly && (Link || Exchange))))
    return std::nullopt;
  const uint64_t S = (First >> 10) & 1;
  const uint64_t I1 = ~(((Second >> 13) & 1) ^ S) & 1;
  const uint64_t I2 = ~(((Second >> 11) & 1) ^ S) & 1;
  const uint64_t Offset = (S << 24) | (I1 << 23) | (I2 << 22) |
                          (uint64_t(First & 0x3FF) << 12) |
                          (uint64_t(Second & 0x7FF) << 1);
  uint64_t Target = Address + 4 + static_cast<uint64_t>(signExtend(Offset, 25));
  if (Exchange)
    Target &= ~uint64_t(3);
  return wrapToImage(Img, Target);
}

/// Where the direct branch a reference describes goes, when the image holds
/// that branch at \p Site; see PatternModule::References for the offsets.
std::optional<uint64_t> branchTarget(const BinaryImage &Img, uint64_t Site) {
  switch (Img.Arch) {
  case Arch::X86:
  case Arch::X64: {
    if (Site == 0)
      return std::nullopt;
    const uint8_t *Insn = Img.readVA(Site - 1, 5);
    if (!Insn || (Insn[0] != 0xE8 && Insn[0] != 0xE9))
      return std::nullopt;
    const int64_t Disp = readLE<int32_t>(Insn + 1);
    return wrapToImage(Img, Site + 4 + static_cast<uint64_t>(Disp));
  }
  case Arch::AArch64: {
    const uint8_t *Insn = Img.readVA(Site, 4);
    if (!Insn)
      return std::nullopt;
    const uint32_t Word = readLE<uint32_t>(Insn);
    // B and BL; a veneer or anything else is not the branch the library had.
    if ((Word & 0x7C000000u) != 0x14000000u)
      return std::nullopt;
    return Site + static_cast<uint64_t>(signExtend(Word & 0x03FFFFFFu, 26) * 4);
  }
  case Arch::ARM:
    return thumbBranchTarget(Img, Site, /*JumpOnly=*/false);
  default:
    return std::nullopt;
  }
}

/// The routine a thunk at \p Address jumps to, when all it is is one
/// unconditional direct jump: an incremental-linking thunk, or a branch
/// island.
std::optional<uint64_t> thunkTarget(const BinaryImage &Img, uint64_t Address) {
  switch (Img.Arch) {
  case Arch::X86:
  case Arch::X64: {
    const uint8_t *Insn = Img.readVA(Address, 5);
    if (!Insn || Insn[0] != 0xE9)
      return std::nullopt;
    const int64_t Disp = readLE<int32_t>(Insn + 1);
    return wrapToImage(Img, Address + 5 + static_cast<uint64_t>(Disp));
  }
  case Arch::AArch64: {
    const uint8_t *Insn = Img.readVA(Address, 4);
    if (!Insn)
      return std::nullopt;
    const uint32_t Word = readLE<uint32_t>(Insn);
    if ((Word & 0xFC000000u) != 0x14000000u)
      return std::nullopt;
    return Address +
           static_cast<uint64_t>(signExtend(Word & 0x03FFFFFFu, 26) * 4);
  }
  case Arch::ARM:
    return thumbBranchTarget(Img, Address, /*JumpOnly=*/true);
  default:
    return std::nullopt;
  }
}

} // namespace

void SignatureDB::checkReferences(const BinaryImage &Img) {
  bool AnyReferences = false;
  for (size_t Module : MatchModules)
    AnyReferences |= Module != NoModule && !Modules[Module].References.empty();
  if (!AnyReferences)
    return;

  // What the bytes alone settle, which is what a reference is checked
  // against: an address two matches name differently names nothing.
  const std::unordered_map<uint64_t, std::string> Settled = buildNameMap();

  // The modules that describe each routine from its start, to confirm a
  // reference by the named routine's own pattern.
  std::unordered_map<std::string, std::vector<size_t>> ByName;
  for (size_t I = 0; I < Modules.size(); ++I)
    for (const FuncRef &Name : Modules[I].PublicNames)
      if (Name.Offset == 0)
        ByName[Name.Name].push_back(I);

  auto PatternAt = [&](const std::string &Name, uint64_t Address) {
    const auto Candidates = ByName.find(Name);
    const Segment *Seg = Img.getSegmentFor(Address);
    if (Candidates == ByName.end() || !Seg || !Seg->isExecutable() ||
        Address < Seg->VA || Address - Seg->VA >= Seg->Data.size())
      return false;
    const size_t Offset = static_cast<size_t>(Address - Seg->VA);
    for (size_t Module : Candidates->second)
      if (SignatureMatcher::matchPattern(Modules[Module],
                                         Seg->Data.data() + Offset,
                                         Seg->Data.size() - Offset))
        return true;
    return false;
  };

  // A Thumb routine may be entered with the interworking bit set.
  auto SettledAt = [&](uint64_t Target) {
    auto It = Settled.find(Target);
    if (It == Settled.end() && Img.Arch == Arch::ARM)
      It = Settled.find(Target | 1);
    return It;
  };

  auto Judge = [&](const std::string &Name, uint64_t Target) {
    // An import thunk is named after the import, not after the decorated
    // symbol the library called; it settles nothing either way.
    if (Img.decodeImportThunkAt(Target))
      return ReferenceVerdict::Unknown;
    if (const auto It = SettledAt(Target); It != Settled.end())
      return It->second == Name ? ReferenceVerdict::Confirmed
                                : ReferenceVerdict::Contradicted;
    // A routine the image replaced (operator new, say) does not match the
    // library's pattern and is still the routine called, so a pattern that
    // does not match contradicts nothing.
    return PatternAt(Name, Target) ? ReferenceVerdict::Confirmed
                                   : ReferenceVerdict::Unknown;
  };

  std::vector<SigMatch> Kept;
  std::vector<size_t> KeptModules;
  Kept.reserve(Matches.size());
  for (size_t I = 0; I < Matches.size(); ++I) {
    const size_t Module = MatchModules[I];
    const std::vector<FuncRef> *References =
        Module == NoModule ? nullptr : &Modules[Module].References;
    bool Contradicted = false;
    size_t Confirmed = 0;
    if (References) {
      // Every public name of a module shares its references; the module's
      // start is the match address less the name's offset.
      uint64_t Start = Matches[I].Address;
      for (const FuncRef &Name : Modules[Module].PublicNames)
        if (Name.Name == Matches[I].Name) {
          Start = Matches[I].Address - Name.Offset;
          break;
        }
      if (Img.Arch == Arch::ARM)
        Start &= ~uint64_t(1);
      for (const FuncRef &Ref : *References) {
        const std::optional<uint64_t> Target =
            branchTarget(Img, Start + Ref.Offset);
        if (!Target)
          continue;
        ReferenceVerdict Verdict = Judge(Ref.Name, *Target);
        if (Verdict == ReferenceVerdict::Unknown)
          if (const std::optional<uint64_t> Next = thunkTarget(Img, *Target))
            Verdict = Judge(Ref.Name, *Next);
        if (Verdict == ReferenceVerdict::Contradicted) {
          Contradicted = true;
          break;
        }
        Confirmed += Verdict == ReferenceVerdict::Confirmed;
      }
    }
    if (Contradicted)
      continue;
    Kept.push_back(std::move(Matches[I]));
    Kept.back().Confirmed =
        References && !References->empty() && Confirmed == References->size();
    KeptModules.push_back(Module);
  }
  Matches = std::move(Kept);
  MatchModules = std::move(KeptModules);
}
