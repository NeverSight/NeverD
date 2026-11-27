//===- PathIndex.h - Archive namespace admission ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared explicit and implicit directory collision ownership for archives.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "../PathPolicy.h"

#include <cstdint>
#include <map>
#include <string>

namespace neverd::web {
class ArchivePathIndex {
  struct Node {
    std::string Path;
    bool Directory = false, Explicit = false;
  };
  std::map<std::string, Node> Paths;
  uint64_t &Metadata;
  std::string (*Key)(std::string_view);
  void charge(uint64_t Bytes);

public:
  explicit ArchivePathIndex(
      uint64_t &Metadata, std::string (*Key)(std::string_view) = archivePathKey)
      : Metadata(Metadata), Key(Key) {}
  void admit(const std::string &Path, bool Directory);
};
} // namespace neverd::web
