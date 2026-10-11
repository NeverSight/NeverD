//===- SEA.h - Offline Node SEA evidence ----------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Offline Node SEA evidence.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Artifact.h"

namespace neverd::web {
inline constexpr std::string_view SEABlobProfile =
    "node-sea-22.15.0-blob-le64-v1";
inline constexpr uint32_t MaxSEAAssets = 4096;
inline constexpr uint64_t MaxSEAPrivateBytes = 1024 * 1024;
inline constexpr uint64_t MaxSEANameBytes = 32768;
inline constexpr uint64_t MaxSEAInputBytes = 256ULL * 1024 * 1024;

/// Byte ranges are relative to the selected container, not host paths.
struct SEARegion {
  std::string ID, Kind, BlobHash;
  uint64_t Offset = 0;
  Blob Content;
  bool Private = false;
  bool selectable() const {
    return Kind == "javascript_storage" || Kind == "asset";
  }
};
struct SEAExtraction {
  std::string ID, ArtifactID, BlobHash, Profile;
  std::string ContainerFormat, Architecture;
  uint64_t ResourceOffset = 0, ResourceSize = 0;
  uint32_t Flags = 0, AssetCount = 0;
  std::string SourceArtifactID;
  std::vector<SEARegion> Regions;
};

/// Explicit layout compatibility only. Never executes Node, maps native code,
/// decodes V8 snapshots/caches or authenticates the producer/runtime version.
SEAExtraction extractSEA(const Artifact &Input, std::string_view Profile);
std::vector<std::string> seaProfiles();
} // namespace neverd::web
