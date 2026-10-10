//===- SessionPackageIntegrity.cpp - Bound digest results --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Revision-bound selection of original bytes and a captured SRI declaration.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"

namespace neverd::web {
namespace {
llvm::json::Object summary(const PackageIntegrityResult &R, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"integrity_id", R.ID},
      {"profile", std::string(PackageIntegrityProfile)},
      {"artifact_id", R.ArtifactID},
      {"blob_sha256", R.BlobHash},
      {"original_bytes", std::to_string(R.Bytes)},
      {"declaration_artifact_id", R.DeclarationArtifactID},
      {"declaration_sha256", R.DeclarationHash},
      {"declaration_selection_id", R.SelectionID},
      {"declaration_kind", R.Kind},
      {"binding", "caller_selected_evidence"},
      {"byte_domain", "selected_original_artifact"},
      {"integrity_status", R.Status},
      {"algorithm", R.Algorithm.empty() ? llvm::json::Value(nullptr)
                                        : llvm::json::Value(R.Algorithm)},
      {"strongest_candidate_count", R.CandidateCount},
      {"legacy_sha1", R.Algorithm == "sha1"},
      {"bytes_match_declaration", R.Status == "match"},
      {"authenticates_publisher", false},
      {"provenance_verified", false},
      {"safety_verdict", "not_assessed"},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::verifyPackageIntegrity(std::string_view Revision,
                                            std::string_view ArtifactID,
                                            std::string_view DeclarationID,
                                            std::string_view PackageID) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const Artifact *Original = nullptr;
  for (const auto &A : State->Published.Artifacts)
    if (A.ID == ArtifactID)
      Original = &A;
  if (!Original)
    throw Error("integrity_original_not_captured");
  PackageIntegrityDeclaration Declaration;
  if (PackageID.empty()) {
    const auto Metadata = State->artifactView(DeclarationID);
    if (!Metadata)
      throw Error("unknown_integrity_metadata");
    Artifact A;
    A.ID = DeclarationID;
    A.BlobHash = Metadata->BlobHash;
    A.Content = Metadata->Content;
    Declaration = registryPackageIntegrity(A);
  } else {
    const auto It = State->PackageAnalyses.find(std::string(DeclarationID));
    if (It == State->PackageAnalyses.end())
      throw Error("unknown_package_analysis");
    Declaration = lockedPackageIntegrity(It->second, PackageID);
  }
  for (const auto &[ID, R] : State->PackageIntegrity)
    if (R.ArtifactID == ArtifactID &&
        R.SelectionID == Declaration.SelectionID &&
        R.DeclarationArtifactID == Declaration.ArtifactID &&
        R.DeclarationHash == Declaration.BlobHash && R.Kind == Declaration.Kind)
      return json(summary(R, State->Revision));
  if (State->PackageIntegrity.size() >= 16)
    throw Error("package_integrity_cache_budget_exceeded");
  auto Result = web::verifyPackageIntegrity(*Original, Declaration);
  auto Reply = json(summary(Result, State->Revision));
  const auto ID = Result.ID;
  State->PackageIntegrity.emplace(ID, std::move(Result));
  return Reply;
}
} // namespace neverd::web
