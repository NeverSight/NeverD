//===- Packages.h - Offline Node package evidence ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Offline Node package evidence.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Artifact.h"

#include <map>
#include <optional>

namespace neverd::web {
inline constexpr std::string_view PackageProfile = "node-package-evidence-v1";
inline constexpr uint64_t MaxPackageMetadataBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxPackageInstances = 4096;
inline constexpr uint64_t MaxPackageEdges = 32768;
inline constexpr uint64_t MaxPackageRecords = 32768;
inline constexpr uint32_t NoPackageIndex = UINT32_MAX;

// Text fields are private evidence. Ordinary SDK/CLI/worker pages publish only
// fixed categories, presence, evidence identities and relationships.
struct PackageInstance {
  std::string ID, Location, InstalledName, ManifestArtifactID;
  std::optional<std::string> Name, Version, Resolved, Integrity;
  std::optional<std::string> LegacyFetchSpec;
  std::string OriginKind = "missing", IntegrityStatus = "missing";
  std::string VersionKind = "missing";
  std::string NameSource = "missing", VersionSource = "missing";
  std::string OSSource = "missing", CPUSource = "missing",
              LibcSource = "missing";
  std::string ManifestStatus = "not_supplied", LinkStatus = "not_a_link";
  std::vector<std::string> OS, CPU, Libc;
  bool Dev = false, Optional = false, DevOptional = false, Peer = false;
  bool Link = false, HasInstallScript = false, InBundle = false;
  // Namespace ancestry is distinct from npm's physical installation parent.
  uint32_t Parent = NoPackageIndex, LinkTarget = NoPackageIndex;
  // Sorted declarations permit comparison without substituting graph IDs.
  std::map<std::string, std::map<std::string, std::string>> Requirements;
  std::map<std::string, bool> PeerOptional;
};
struct PackageDependency {
  std::string ID, Kind, RequestedName, Spec, SpecKind, Status;
  uint32_t From = NoPackageIndex, Candidate = NoPackageIndex;
  bool Optional = false, Conditional = false;
};
struct PackageScript {
  std::string ID, ArtifactID, Name, Command, Kind;
  uint32_t Package = NoPackageIndex;
};
struct PackageEntry {
  std::string ID, ArtifactID, Kind, Name, Path, TargetArtifactID, Status;
  uint32_t Package = NoPackageIndex;
};
struct PackageFile {
  std::string ID, ArtifactID, Hash, Path, Kind;
  uint64_t Size = 0;
  uint32_t Package = NoPackageIndex;
};
struct PackageAnalysis {
  std::string ID, ArtifactID, BlobHash, Kind, RootPrefix;
  std::string RootDeclarations, LegacyStatus;
  uint64_t LockVersion = 0, MetadataBytes = 0, Steps = 0;
  bool DirectoryInventory = false;
  std::vector<PackageInstance> Packages;
  std::vector<PackageDependency> Dependencies;
  std::vector<PackageScript> Scripts;
  std::vector<PackageEntry> Entries;
  std::vector<PackageFile> Files;
};

/// Kind is explicitly "npm-lock" or "package-json". Only admitted immutable
/// members are consulted. Version satisfaction and runtime loading are never
/// inferred from a lockfile placement or an exact local filename match.
PackageAnalysis analyzePackages(const Snapshot &Input,
                                std::string_view ArtifactID,
                                std::string_view Kind);

struct PackageChange {
  std::string ID, Kind, Field;
  uint32_t Before = NoPackageIndex, After = NoPackageIndex;
};
struct PackageDiff {
  std::string ID, BeforeID, AfterID, RootIdentity, Coverage, Platform,
      InputContract;
  std::vector<PackageChange> Changes;
};
PackageDiff comparePackages(const PackageAnalysis &Before,
                            const PackageAnalysis &After);
} // namespace neverd::web
