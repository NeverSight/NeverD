//===- PackageIntegrity.h - Original package digest evidence -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit declaration-to-original binding, independent of archive decoding.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_WEB_PACKAGEINTEGRITY_H
#define NEVERD_WEB_PACKAGEINTEGRITY_H

#include "neverd/web/Packages.h"

namespace neverd::web {
inline constexpr std::string_view PackageIntegrityProfile =
    "npm-original-sri-v1";
struct PackageIntegrityDeclaration {
  std::string ArtifactID, BlobHash, SelectionID, Kind;
  std::optional<std::string> Value;
  bool Git = false;
};
struct PackageIntegrityResult {
  std::string ID, ArtifactID, BlobHash, DeclarationArtifactID, DeclarationHash,
      SelectionID, Kind, Status, Algorithm;
  uint64_t Bytes = 0, CandidateCount = 0;
};
/// Canonical padded base64 only, no options. This is an explicit npm evidence
/// subset; it does not claim browser SRI parsing or resource-loading semantics.
std::string packageIntegrityDeclarationStatus(std::string_view Value,
                                              bool Git = false);
PackageIntegrityDeclaration registryPackageIntegrity(const Artifact &Metadata);
PackageIntegrityDeclaration lockedPackageIntegrity(const PackageAnalysis &A,
                                                   std::string_view PackageID);
PackageIntegrityResult
verifyPackageIntegrity(const Artifact &Original,
                       const PackageIntegrityDeclaration &Declaration);
} // namespace neverd::web
#endif // NEVERD_WEB_PACKAGEINTEGRITY_H
