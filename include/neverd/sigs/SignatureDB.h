//===- SignatureDB.h - Signature database manager -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// High-level signature database management: loading .sig/.pat files from
/// directories, applying them to a BinaryImage, and querying match results.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_SIGNATUREDB_H
#define NEVERD_SIGS_SIGNATUREDB_H

#include "neverd/sigs/Signature.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace neverd {

struct BinaryImage;

namespace sigs {

class SignatureDB {
public:
  /// Load all .sig and .pat files from a directory.
  llvm::Error loadDirectory(const std::filesystem::path &Dir);

  /// Load a single .sig or .pat file.
  llvm::Error loadFile(const std::filesystem::path &Path);

  /// Apply loaded signatures against a binary image.
  /// Matches are stored internally and can be queried with matches().
  /// \p FuncEntries are the known function entry addresses to check.
  void apply(const BinaryImage &Img, const std::vector<uint64_t> &FuncEntries);

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

  void addModules(std::vector<PatternModule> &&Mods, const std::string &LibName,
                  const std::string &FilePath);
};

} // namespace sigs
} // namespace neverd

#endif // NEVERD_SIGS_SIGNATUREDB_H
