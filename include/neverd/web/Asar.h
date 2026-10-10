#pragma once

#include "neverd/web/Artifact.h"

namespace neverd::web {
inline constexpr std::string_view AsarProfile = "asar-pickle-json-v1";
inline constexpr uint64_t MaxAsarHeaderBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxAsarMembers = 10000;
inline constexpr uint64_t MaxAsarDepth = 24;
inline constexpr uint64_t MaxAsarPathBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxAsarPayloadBytes = 512ULL * 1024 * 1024;

/// Paths/link targets are private evidence, never ordinary output fields.
struct AsarMember {
  std::string ID, ParentID, Path, Kind, Link;
  std::string Status, IntegrityStatus = "missing", BlobHash;
  std::string StorageArtifactID;
  uint64_t Offset = 0, Size = 0;
  bool Unpacked = false, Executable = false;
  Blob Content;
  bool available() const { return Status == "available"; }
};

struct AsarExtraction {
  std::string ID, ArtifactID, UnpackedDirectoryID;
  uint64_t DataOffset = 0;
  uint64_t UnreferencedPayloadBytes = 0;
  std::vector<AsarMember> Members;
};

bool asarAvailable();
/// Select both inputs by occurrence in the same immutable snapshot. Empty
/// UnpackedDirectoryID performs packed-only inspection; it never guesses a
/// companion path. Structural failure publishes no extraction. Per-member
/// missing/integrity/link limitations remain explicit and cannot supply bytes.
AsarExtraction extractAsar(const Snapshot &Input, std::string_view ArtifactID,
                           std::string_view UnpackedDirectoryID = {});
} // namespace neverd::web
