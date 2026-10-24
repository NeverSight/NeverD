//===- SignatureDB.h - Signature database manager -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// High-level signature database management: loading text pattern files from
/// directories, applying them to a BinaryImage, and querying match results.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_SIGNATUREDB_H
#define NEVERD_SIGS_SIGNATUREDB_H

#include "neverd/sigs/LibraryFeature.h"
#include "neverd/sigs/LibraryRecognition.h"
#include "neverd/sigs/PatternParser.h"
#include "neverd/sigs/Signature.h"
#include "neverd/sigs/SignatureCache.h"
#include "neverd/sigs/SignatureMatcher.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace neverd {

struct BinaryImage;

namespace sigs {

struct SignatureSelectionStats {
  size_t AvailableFiles = 0;
  size_t SelectedFiles = 0;
};

class SignatureDB {
public:
  /// Present after a successful automatic load; manual changes clear it.
  const std::optional<SignatureSelectionStats> &automaticSelection() const {
    return AutoSelection;
  }
  /// Load pattern files through \p NewCache from now on, or, when it is
  /// none, parse every one.  A file the cache holds as it is now is mapped
  /// rather than parsed, and one it does not is kept there once it is
  /// parsed; what a load returns is the same either way.
  void setCache(std::optional<SignatureCache> NewCache) {
    Cache = std::move(NewCache);
  }

  /// Replace the current database with all pattern files from a directory.
  llvm::Error loadDirectory(const std::filesystem::path &Dir);

  /// The pattern files of a directory, sorted by name.
  static llvm::Expected<std::vector<std::filesystem::path>>
  listDirectory(const std::filesystem::path &Dir);

  /// Replace the current database with exactly these pattern files.  Each
  /// file's library name is its stem, as with \ref loadDirectory.
  llvm::Error loadFiles(const std::vector<std::filesystem::path> &Files);

  /// The library a pattern file holds lines of: the file's stem, less the
  /// `.part<N>` (N from 2) of a file written in parts --
  /// `ubuntu-libc6.part2.pat` holds lines of ubuntu-libc6, `vs2026.part2.pat`
  /// of Visual Studio 2026.  Matches report it as their library.
  static std::string libraryName(const std::filesystem::path &File);

  /// The pattern files among \p Files that fit \p Img.
  ///
  /// Loader-owned platform, runtime and execution-domain evidence excludes
  /// incompatible reviewed bundled packs before their files are opened.
  /// Unknown/custom libraries, unknown dimensions and conflicting evidence
  /// retain coverage. Mixed WDK and common compiler helper packs are not
  /// assigned a kernel/user restriction. Explicit loadFiles bypasses selection.
  ///
  /// A file of library `vs<year>` (see \ref libraryName) holds one Visual
  /// Studio release's runtime libraries.  A PE file links the static runtime
  /// libraries of its linker's release, and other releases state some of the
  /// same bytes under other names, so when the image's Rich header names the
  /// release of its linker, only that release's file is kept.  Every file that
  /// belongs to no release and fits the platform is kept. When the image has
  /// no Rich header, when the header was altered after linking, or when
  /// \p Files hold no file for a release it names, all platform-compatible
  /// files are kept.
  static std::vector<std::filesystem::path>
  selectForImage(const BinaryImage &Img,
                 std::vector<std::filesystem::path> Files);

  /// The directory of a signature tree that holds \p Img's signatures, such
  /// as `pe/x86/64`, or std::nullopt for an image of a format or architecture
  /// no tree holds signatures for.
  static std::optional<std::filesystem::path>
  treeDirectory(const BinaryImage &Img);

  /// Load a single text pattern file.
  llvm::Error loadFile(const std::filesystem::path &Path);

  /// Load pattern lines already in memory, tagged with \p LibraryName.
  /// A signature set does not have to arrive as a file: a plugin can
  /// synthesize one, and a test can state the exact bytes it means to match.
  llvm::Error loadPatternText(llvm::StringRef Text,
                              llvm::StringRef LibraryName);

  /// Structural and gated-byte rules share this database, but cannot enter
  /// buildNameMap until their independent identity requirements are proved.
  llvm::Error loadFeaturePack(const std::filesystem::path &Path);
  /// Replace only the feature packs, as one transaction. Dir is features/rules.
  llvm::Error loadFeatureDirectory(const std::filesystem::path &Dir);
  /// Load the selected legacy tree and features/rules as one transaction.
  /// An absent feature directory means this tree has no structural rules.
  llvm::Error loadForImage(const BinaryImage &Image,
                           const std::filesystem::path &Root);
  const std::map<std::string, LibraryFeaturePack> &featurePacks() const {
    return FeaturePacks;
  }
  uint64_t featureGeneration() const { return FeatureGeneration; }
  /// Byte features share the existing matching and reference/ambiguity engine.
  /// The caller independently authenticates each candidate's identity first.
  using FeatureIdentityVerifier = std::function<std::string(
      const LibraryFeaturePack &, const LibraryFeatureRule &, va_t)>;
  std::vector<LibraryRecognition>
  recognizeFeatureBytes(const BinaryImage &Image,
                        llvm::ArrayRef<uint64_t> Entries,
                        const FeatureIdentityVerifier &Identity) const;

  /// Apply loaded signatures against a binary image.
  /// Matches are stored internally and can be queried with matches().
  /// \p FuncEntries are the known function entry addresses to check.
  ///
  /// A module's references (PatternModule::References) are then checked
  /// against the image, the references at one offset as one branch that
  /// reaches one of the routines they name.  A branch that goes to a routine
  /// the other matches name as none of them contradicts the match, which is
  /// dropped.  One whose target those matches name as one of them, or whose
  /// target the pattern of one of them matches, confirms it.  In an ELF image a
  /// branch to an import's PLT stub confirms a reference to that import and
  /// contradicts one to anything else.  Anything else -- a COFF import thunk,
  /// a routine nothing names -- neither confirms nor contradicts.  A branch
  /// to a routine that only jumps on -- an incremental-linking thunk, a
  /// branch island, a linker's long-branch or interworking thunk -- is
  /// followed when the routine itself settles nothing.  What it reaches
  /// confirms a reference to it and contradicts nothing: the same bytes are
  /// a routine that tail-calls another, `free` jumping to `_free_base`.
  ///
  /// What the references confirm and contradict names routines the bytes
  /// alone could not, and those are what the references of their callers are
  /// checked against in turn: the check repeats until it names nothing new.
  void apply(const BinaryImage &Img, const std::vector<uint64_t> &FuncEntries);

  /// Name the personality routines \p Img installs but cannot name itself,
  /// and refresh the frames that installed them.
  ///
  /// Locating an exception personality is a name lookup everywhere else in
  /// NeverD, which leaves a stripped, statically linked image with nothing to
  /// look up: the routine is an address, every frame pointing at it reports an
  /// unknown personality.  A schema-independent table may have been decoded
  /// provisionally, but personality-specific forms cannot be trusted until
  /// the routine is identified.  This answers that one question from the
  /// signature set, and only that one.
  /// Ordinary functions are not touched, and neither is a routine the image
  /// names -- see \ref neverd::adoptPersonalityRoutineName for the terms a
  /// name has to meet before exception classification will take it.
  ///
  /// Only a module that agrees with the whole function it describes and
  /// states enough bytes exactly is allowed to answer, and an address two
  /// modules name differently is left alone rather than decided arbitrarily.
  /// The cost of a wrong answer here is not a wrong label: it is an LSDA read
  /// against a schema its bytes do not follow.
  ///
  /// Run this after \ref apply rather than before.  The hits are appended to
  /// \ref matches like any others, and \ref apply begins by clearing that
  /// list, so the other order reports nothing.  Running second also puts the
  /// adopted name in front of \ref buildNameMap, which is what lets the
  /// routine be renamed in a function listing and not only in the frames that
  /// installed it.
  ///
  /// \returns how many routines were named.
  size_t identifyPersonalityRoutines(BinaryImage &Img);

  /// Get all signature matches found so far.
  const std::vector<SigMatch> &matches() const { return Matches; }

  /// Clear all loaded signatures and matches.
  void clear();

  /// Get the number of loaded pattern modules.
  size_t moduleCount() const { return Modules.size(); }

  /// Get the number of loaded signature files.
  size_t fileCount() const { return LoadedFiles.size(); }

  /// Lookup a match by address. Returns nullptr if not found.
  const SigMatch *findMatch(uint64_t Addr) const;

  /// Build a map from address to function name for quick lookup.
  ///
  /// Each match gives the routine at its address a set of names: its name
  /// and its aliases. Matches whose sets share a name agree, and the address
  /// takes the preferred shared name (preferredAliasOrder). An address the
  /// matches name differently gets no name, unless the matches whose
  /// references were all confirmed agree on one: the bytes alone could not
  /// tell the routines apart, but what they call could.
  std::unordered_map<uint64_t, std::string> buildNameMap() const;

private:
  struct SigSource {
    std::string Path;
    std::string LibraryName;
    size_t ModuleStart = 0;
    size_t ModuleCount = 0;
    /// What the source's modules point into: the bytes and names it parsed
    /// to, or the cache entry it was mapped from.
    std::unique_ptr<uint8_t[]> Bytes;
    std::vector<PatternNames> Names;
    std::unique_ptr<llvm::MemoryBuffer> Mapping;
  };

  std::vector<StoredModule> Modules;
  std::vector<SigSource> LoadedFiles;
  std::vector<SigMatch> Matches;
  std::map<std::string, LibraryFeaturePack> FeaturePacks;
  uint64_t FeatureGeneration = 0;
  std::optional<SignatureCache> Cache;

  /// The index of \ref Modules, built by \ref index when first needed and
  /// dropped whenever the modules change.
  std::unique_ptr<SignatureMatcher::HashIndex> Index;
  const SignatureMatcher::HashIndex &index();

  /// Per entry of \ref Matches: the module that made it, or NoModule.
  static constexpr size_t NoModule = std::numeric_limits<size_t>::max();
  std::vector<size_t> MatchModules;

  void clearMatches();

  /// What the matches settle for one address: the name shown, and every name
  /// the agreeing matches give the routine, sorted.  Both view the matches'
  /// names, and last only as long as the matches do.
  struct SettledRoutine {
    std::string_view Name;
    llvm::SmallVector<std::string_view, 4> Names;
    bool hasName(std::string_view Name) const {
      return std::binary_search(Names.begin(), Names.end(), Name);
    }
    bool operator==(const SettledRoutine &) const = default;
  };
  /// The routines the matches settle; see \ref buildNameMap.
  std::unordered_map<uint64_t, SettledRoutine> settleRoutines() const;
  /// What \p Proposals, the matches at one address, settle there.
  static std::optional<SettledRoutine>
  settle(llvm::ArrayRef<const SigMatch *> Proposals);

  /// Drop the matches their references contradict and record the ones they
  /// confirm; see \ref apply.  \p Hits are the modules that matched at each
  /// of \p Entries, the addresses the matches were looked for at.
  void checkReferences(const BinaryImage &Img, llvm::ArrayRef<uint64_t> Entries,
                       std::vector<SignatureMatcher::Hit> Hits);

  /// Add \p Mods, the modules of \p Src, in place of those of a source with
  /// the same path or after every other source.
  void commitSource(SigSource Src, std::vector<StoredModule> Mods);

  std::optional<SignatureSelectionStats> AutoSelection;

  /// The library a module was loaded from, by its index in \ref Modules.
  const std::string &libraryNameOf(size_t ModuleIndex) const;
};

std::vector<LibraryRecognition> recognizeLibraryFeatureBytes(
    const BinaryImage &Image,
    const std::map<std::string, LibraryFeaturePack> &Packs,
    llvm::ArrayRef<uint64_t> Entries,
    const SignatureDB::FeatureIdentityVerifier &Identity);

} // namespace sigs
} // namespace neverd

#endif // NEVERD_SIGS_SIGNATUREDB_H
