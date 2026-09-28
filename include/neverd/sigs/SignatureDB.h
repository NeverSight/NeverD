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

#include "neverd/sigs/Signature.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace neverd {

struct BinaryImage;

namespace sigs {

class SignatureDB {
public:
  /// Replace the current database with all pattern files from a directory.
  llvm::Error loadDirectory(const std::filesystem::path &Dir);

  /// The pattern files of a directory, sorted by name.
  static llvm::Expected<std::vector<std::filesystem::path>>
  listDirectory(const std::filesystem::path &Dir);

  /// Replace the current database with exactly these pattern files.  Each
  /// file's library name is its stem, as with \ref loadDirectory.
  llvm::Error loadFiles(const std::vector<std::filesystem::path> &Files);

  /// The pattern files among \p Files that fit \p Img.
  ///
  /// A file named `vs<year>.pat` holds one Visual Studio release's runtime
  /// libraries.  A PE file links the static runtime libraries of its
  /// linker's release, and other releases state some of the same bytes under
  /// other names, so when the image's Rich header names the release of its
  /// linker, only that release's file is kept.  Every file that belongs to no
  /// release, such as `winsdk.pat`, is kept.  When the image has no Rich
  /// header, when the header was altered after linking, or when \p Files
  /// hold no file for a release it names, every file is kept.
  static std::vector<std::filesystem::path>
  selectForImage(const BinaryImage &Img,
                 std::vector<std::filesystem::path> Files);

  /// Load a single text pattern file.
  llvm::Error loadFile(const std::filesystem::path &Path);

  /// Load pattern lines already in memory, tagged with \p LibraryName.
  /// A signature set does not have to arrive as a file: a plugin can
  /// synthesize one, and a test can state the exact bytes it means to match.
  llvm::Error loadPatternText(llvm::StringRef Text,
                              llvm::StringRef LibraryName);

  /// Apply loaded signatures against a binary image.
  /// Matches are stored internally and can be queried with matches().
  /// \p FuncEntries are the known function entry addresses to check.
  ///
  /// A module's references (PatternModule::References) are then checked
  /// against the image.  A reference whose branch goes to a routine the other
  /// matches name differently contradicts the match, which is dropped.  One
  /// whose target those matches name the same, or whose target the named
  /// routine's own pattern matches, confirms it.  Anything else -- an import
  /// thunk, a veneer, a routine nothing names -- neither confirms nor
  /// contradicts.  A branch to an incremental-linking thunk is followed to
  /// the thunk's target when the thunk itself settles nothing.
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
  /// An address the matches name differently gets no name, unless exactly one
  /// of those names comes from a match whose references were all confirmed:
  /// the bytes alone could not tell the routines apart, but what they call
  /// could.
  std::unordered_map<uint64_t, std::string> buildNameMap() const;

private:
  struct SigSource {
    std::string Path;
    std::string LibraryName;
    size_t ModuleStart = 0;
    size_t ModuleCount = 0;
  };

  std::vector<PatternModule> Modules;
  std::vector<SigSource> LoadedFiles;
  std::vector<SigMatch> Matches;

  /// Per entry of \ref Matches: the module that made it, or NoModule.
  static constexpr size_t NoModule = std::numeric_limits<size_t>::max();
  std::vector<size_t> MatchModules;

  void clearMatches();

  /// Drop the matches their references contradict and record the ones they
  /// confirm; see \ref apply.
  void checkReferences(const BinaryImage &Img);

  void commitSource(std::vector<PatternModule> &&Mods,
                    const std::string &LibName, const std::string &FilePath);

  /// The library a module was loaded from, by its index in \ref Modules.
  const std::string &libraryNameOf(size_t ModuleIndex) const;
};

} // namespace sigs
} // namespace neverd

#endif // NEVERD_SIGS_SIGNATUREDB_H
