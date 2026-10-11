//===- Nexe.h - Offline nexe resource evidence ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit nexe envelope compatibility with immutable resource occurrences.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Artifact.h"

namespace neverd::web {
inline constexpr std::string_view NexeProfile =
    "nexe-4.0.0-rc.7-eof-resources-v1";
inline constexpr uint64_t MaxNexeInputBytes = 256ULL * 1024 * 1024;
inline constexpr uint64_t MaxNexeBootstrapBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxNexeIndexBytes = 1024 * 1024;
inline constexpr uint64_t MaxNexePrivateBytes = 4 * 1024 * 1024;
inline constexpr uint64_t MaxCachedNexePrivateBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxNexeSteps = 2000000;
inline constexpr uint32_t MaxNexeMembers = 4096;
inline constexpr uint32_t MaxCachedNexeExtractions = 4;
inline constexpr uint32_t MaxNexePageRecords = 512;

struct NexeRegion {
  std::string ID, Kind, BlobHash;
  uint64_t Offset = 0;
  Blob Content;
  bool selectable() const { return Kind == "bootstrap_storage"; }
};

struct NexeMember {
  std::string ID, RangeID, BlobHash;
  /// Private namespace key, never a host output path or public record value.
  std::string Path;
  uint64_t Offset = 0;
  Blob Content;
};

struct NexeExtraction {
  std::string ID, ArtifactID, BlobHash;
  uint64_t ResourceOffset = 0, ResourceSize = 0, ReferencedBytes = 0;
  uint64_t PrivateBytes = 0, Steps = 0, ScannedBytes = 0, HashedBytes = 0;
  uint32_t UniqueMemberRanges = 0;
  std::vector<NexeRegion> Regions;
  std::vector<NexeMember> Members;
};

bool nexeAvailable();
/// Fixed container grammar only. Never executes bootstrap code or authenticates
/// the producer, native prefix, signatures, Node version or entry point.
NexeExtraction extractNexe(const Artifact &Input, std::string_view Profile);
Snapshot nexeNamespace(const NexeExtraction &E);
} // namespace neverd::web
