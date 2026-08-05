//===- SignatureDB.cpp - Signature database manager -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/SignatureDB.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sigs/PatternParser.h"
#include "neverd/sigs/SignatureMatcher.h"

#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <thread>

using namespace neverd;
using namespace neverd::sigs;

void SignatureDB::addModules(std::vector<PatternModule> &&Mods,
                             const std::string &LibName,
                             const std::string &FilePath) {
  SigSource Src;
  Src.Path = FilePath;
  Src.LibraryName = LibName;
  Src.ModuleStart = Modules.size();
  Src.ModuleCount = Mods.size();
  LoadedFiles.push_back(std::move(Src));

  Modules.insert(Modules.end(), std::make_move_iterator(Mods.begin()),
                 std::make_move_iterator(Mods.end()));
}

llvm::Error SignatureDB::loadFile(const std::filesystem::path &Path) {
  auto Ext = Path.extension().string();

  if (Ext == ".pat") {
    auto ModsOrErr = PatternParser::parseFile(Path);
    if (!ModsOrErr)
      return ModsOrErr.takeError();
    std::string LibName = Path.stem().string();
    addModules(std::move(*ModsOrErr), LibName, Path.string());
    return llvm::Error::success();
  }

  return llvm::make_error<llvm::StringError>(
      "unsupported signature file format: " + Path.string(),
      llvm::inconvertibleErrorCode());
}

llvm::Error SignatureDB::loadDirectory(const std::filesystem::path &Dir) {
  if (!std::filesystem::exists(Dir))
    return llvm::make_error<llvm::StringError>(
        "signature directory does not exist: " + Dir.string(),
        llvm::inconvertibleErrorCode());

  // Collect all .pat files first, then parse in parallel.
  std::vector<std::filesystem::path> PatFiles;
  std::error_code EC;
  for (auto &Entry : std::filesystem::directory_iterator(Dir, EC)) {
    if (EC)
      break;
    if (!Entry.is_regular_file())
      continue;
    if (Entry.path().extension() == ".pat")
      PatFiles.push_back(Entry.path());
  }

  if (PatFiles.size() <= 1) {
    for (auto &P : PatFiles) {
      auto Err = loadFile(P);
      if (Err)
        llvm::consumeError(std::move(Err));
    }
    return llvm::Error::success();
  }

  // Parallel parse: each thread parses one .pat file independently.
  struct ParsedFile {
    std::filesystem::path Path;
    std::vector<PatternModule> Modules;
    bool Ok = false;
  };
  std::vector<ParsedFile> Parsed(PatFiles.size());
  for (size_t I = 0; I < PatFiles.size(); ++I)
    Parsed[I].Path = PatFiles[I];

  unsigned NumThreads =
      std::min(static_cast<unsigned>(PatFiles.size()),
               std::max(1u, std::thread::hardware_concurrency()));
  std::atomic<size_t> NextIdx{0};
  std::vector<std::thread> Workers;
  Workers.reserve(NumThreads);
  for (unsigned T = 0; T < NumThreads; ++T) {
    Workers.emplace_back([&]() {
      while (true) {
        size_t I = NextIdx.fetch_add(1, std::memory_order_relaxed);
        if (I >= Parsed.size())
          break;
        auto ModsOrErr = PatternParser::parseFile(Parsed[I].Path);
        if (ModsOrErr) {
          Parsed[I].Modules = std::move(*ModsOrErr);
          Parsed[I].Ok = true;
        } else {
          llvm::consumeError(ModsOrErr.takeError());
        }
      }
    });
  }
  for (auto &W : Workers)
    W.join();

  for (auto &PF : Parsed) {
    if (PF.Ok) {
      std::string LibName = PF.Path.stem().string();
      addModules(std::move(PF.Modules), LibName, PF.Path.string());
    }
  }

  return llvm::Error::success();
}

void SignatureDB::apply(const BinaryImage &Img,
                        const std::vector<uint64_t> &FuncEntries) {
  Matches.clear();

  // Build a source index for finding library names.
  auto findLibName = [this](size_t ModIdx) -> const std::string & {
    for (const auto &Src : LoadedFiles) {
      if (ModIdx >= Src.ModuleStart &&
          ModIdx < Src.ModuleStart + Src.ModuleCount)
        return Src.LibraryName;
    }
    static const std::string Empty;
    return Empty;
  };

  // Collect executable segment data for matching.
  for (const auto &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.empty())
      continue;

    SignatureMatcher::scanAtAddresses(
        Seg.Data.data(), Seg.Data.size(), Seg.VA, FuncEntries, Modules,
        [&](uint64_t Addr, const PatternModule &Mod) {
          // Find the index of this module.
          size_t ModIdx = 0;
          for (size_t I = 0; I < Modules.size(); ++I) {
            if (&Modules[I] == &Mod) {
              ModIdx = I;
              break;
            }
          }

          for (const auto &Ref : Mod.PublicNames) {
            SigMatch M;
            M.Address = Addr + Ref.Offset;
            M.Name = Ref.Name;
            M.LibraryName = findLibName(ModIdx);
            M.FuncLen = Mod.TotalLen;
            Matches.push_back(std::move(M));
          }
        });
  }
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
  for (const auto &M : Matches)
    Map[M.Address] = M.Name;
  return Map;
}

void SignatureDB::clear() {
  Modules.clear();
  LoadedFiles.clear();
  Matches.clear();
}
