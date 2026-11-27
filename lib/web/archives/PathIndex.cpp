//===- PathIndex.cpp - Archive namespace admission ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Portable collision checks without converting private names to host paths.
///
//===----------------------------------------------------------------------===//

#include "PathIndex.h"

#include "../PathPolicy.h"

#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"
#include "neverd/web/PackageArchive.h"

#include <algorithm>

namespace neverd::web {
void ArchivePathIndex::charge(uint64_t Bytes) {
  if (Bytes > MaxPackageArchiveMetadata - Metadata)
    throw Error("package_archive_metadata_budget_exceeded");
  Metadata += Bytes;
}

void ArchivePathIndex::admit(const std::string &Path, bool Directory) {
  const auto FullKey = Key(Path);
  if (std::count(Path.begin(), Path.end(), '/') >=
      ::neverd::limits::web::
          kPathIndexArchivePathIndexAdmitStdCountPathBeginPathEndLimit)
    throw Error("package_archive_depth_budget_exceeded");
  charge(Path.size());
  const auto Existing = Paths.find(FullKey);
  if (Existing != Paths.end() && (Existing->second.Explicit ||
                                  Existing->second.Path != Path || !Directory))
    throw Error("package_archive_path_collision");
  for (auto Slash = Path.find('/'); Slash != std::string::npos;
       Slash = Path.find('/', Slash + 1)) {
    const auto Parent = Path.substr(0, Slash);
    const auto ParentKey = Key(Parent);
    charge(Parent.size());
    auto [It, Inserted] =
        Paths.try_emplace(ParentKey, Node{Parent, true, false});
    if (!Inserted && (It->second.Path != Parent || !It->second.Directory))
      throw Error("package_archive_path_collision");
  }
  Paths[FullKey] = Node{Path, Directory, true};
}
} // namespace neverd::web
