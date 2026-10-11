//===- PackageArchive.h - Offline package archive evidence ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded tar and gzip member evidence without filesystem extraction.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_WEB_PACKAGEARCHIVE_H
#define NEVERD_WEB_PACKAGEARCHIVE_H

#include "neverd/web/Artifact.h"

namespace neverd::web {

inline constexpr std::string_view PackageArchiveProfile =
    "ustar-pax-single-gzip-v1";
inline constexpr uint64_t MaxPackageArchiveBytes = 512ULL * 1024 * 1024;
inline constexpr uint64_t MaxPackageArchiveFileBytes = 256ULL * 1024 * 1024;
inline constexpr uint64_t MaxPackageArchiveMembers = 10000;
inline constexpr uint64_t MaxPackageArchiveMetadata = 8ULL * 1024 * 1024;

struct PackageArchiveMember {
  std::string ID, ParentID, Path, Kind, Link, BlobHash;
  uint64_t HeaderOffset = 0, Offset = 0, Size = 0;
  bool Executable = false;
  Blob Content;
  bool available() const { return Kind == "file"; }
};

struct PackageArchive {
  std::string ID, ArtifactID, BlobHash, Format, ExpandedHash;
  Blob Original, Expanded;
  uint64_t ExpandedBytes = 0;
  std::vector<PackageArchiveMember> Members;
};

bool packageArchiveAvailable();
bool packageGzipAvailable();
/// Format is explicitly tar or tgz. ExpandedBudget also limits pending work;
/// the session passes its remaining aggregate derived-storage allowance.
PackageArchive
extractPackageArchive(const Artifact &Input, std::string_view Format,
                      uint64_t ExpandedBudget = MaxPackageArchiveBytes);
Snapshot packageArchiveNamespace(const PackageArchive &Archive);

} // namespace neverd::web

#endif // NEVERD_WEB_PACKAGEARCHIVE_H
