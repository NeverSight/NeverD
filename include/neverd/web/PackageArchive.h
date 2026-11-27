//===- PackageArchive.h - Offline package archive evidence ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded tar, gzip and ZIP member evidence without filesystem extraction.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_WEB_PACKAGEARCHIVE_H
#define NEVERD_WEB_PACKAGEARCHIVE_H

#include "neverd/web/Artifact.h"
#include "neverd/web/Limits.h"

#include "llvm/ADT/STLFunctionalExtras.h"

namespace neverd::web {

inline constexpr std::string_view PackageArchiveProfile =
    "ustar-pax-single-gzip-v1";
inline constexpr std::string_view PackageGNUArchiveProfile =
    "gnu-oldtar-basic-single-gzip-v1";
inline constexpr std::string_view ZipArchiveProfile = "zip32-local-central-v1";

struct PackageArchiveMember {
  std::string ID, ParentID, Path, Kind, Link, BlobHash;
  uint64_t HeaderOffset = 0, Offset = 0, Size = 0;
  bool Executable = false;
  Blob Content;
  // ZIP locators address the original container; Offset addresses the private
  // decoded stream. Unavailable members never enter a readable namespace.
  uint64_t LocalHeaderOffset = 0, StoredOffset = 0, StoredSize = 0;
  uint64_t StoredFrameSize = 0;
  uint16_t Compression = 0;
  std::string UnavailableReason;
  bool available() const { return Kind == "file" && UnavailableReason.empty(); }
};

struct PackageArchive {
  std::string ID, ArtifactID, BlobHash, Format, ExpandedHash;
  Blob Original, Expanded;
  uint64_t ExpandedBytes = 0;
  std::vector<PackageArchiveMember> Members;
  std::string Profile = std::string(PackageArchiveProfile);
};

bool packageArchiveAvailable();
bool packageGzipAvailable();
bool packageZipDeflateAvailable();
/// Format is explicitly tar, tgz or zip. ExpandedBudget also limits pending
/// work; the session passes its remaining aggregate derived-storage allowance.
PackageArchive
extractPackageArchive(const Artifact &Input, std::string_view Format,
                      uint64_t ExpandedBudget = MaxPackageArchiveBytes);
/// A case-sensitive JVM member namespace on the same ZIP framing and CRC
/// reader.
PackageArchive readJarArchive(std::string_view Bytes, uint64_t ExpandedBudget,
                              llvm::function_ref<bool()> Stop);
Snapshot packageArchiveNamespace(const PackageArchive &Archive);

} // namespace neverd::web

#endif // NEVERD_WEB_PACKAGEARCHIVE_H
