//===- SessionPackageArchive.cpp - Archive publication -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Revision-bound archive caches and redacted member pages.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"

namespace neverd::web {
namespace {
llvm::json::Object summary(const PackageArchive &A, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"archive_id", A.ID},
      {"artifact_id", A.ArtifactID},
      {"blob_sha256", A.BlobHash},
      {"profile", std::string(PackageArchiveProfile)},
      {"format", A.Format},
      {"member_count", A.Members.size()},
      {"original_bytes", std::to_string(A.Original.size())},
      {"expanded_bytes", std::to_string(A.ExpandedBytes)},
      {"expanded_sha256", A.ExpandedHash},
      {"analysis_status", "partial"},
      {"coverage", "declared_archive_members"},
      {"follows_links", false},
      {"executes_input", false},
      {"integrity_verification", "not_performed"},
      {"authenticates_publisher", false},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::extractPackageArchive(std::string_view Revision,
                                           std::string_view ArtifactID,
                                           std::string_view Format) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  for (const auto &[ID, A] : State->PackageArchives)
    if (A.ArtifactID == ArtifactID && A.Format == Format)
      return json(summary(A, State->Revision));
  if (State->PackageArchives.size() >= 4 ||
      State->CachedArchiveBytes >= MaxPackageArchiveBytes)
    throw Error("package_archive_cache_budget_exceeded");
  // This profile admits explicitly captured originals only. Recursive archive
  // expansion needs a separate depth/aggregate contract and is not inferred.
  for (const auto &Input : State->Published.Artifacts)
    if (Input.ID == ArtifactID) {
      auto A = web::extractPackageArchive(
          Input, Format, MaxPackageArchiveBytes - State->CachedArchiveBytes);
      auto Reply = json(summary(A, State->Revision));
      const auto Bytes = A.ExpandedBytes;
      const auto ID = A.ID;
      State->PackageArchives.emplace(ID, std::move(A));
      State->CachedArchiveBytes += Bytes;
      return Reply;
    }
  throw Error("unknown_package_archive_artifact");
}

std::string Session::packageArchiveRecords(std::string_view Revision,
                                           std::string_view ArchiveID,
                                           uint64_t Offset,
                                           uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->PackageArchives.find(std::string(ArchiveID));
  if (Found == State->PackageArchives.end())
    throw Error("unknown_package_archive");
  const auto &A = Found->second;
  if (!Limit || Limit > 512 || Offset > A.Members.size())
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(A.Members.size(), Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &M = A.Members[I];
    Items.emplace_back(llvm::json::Object{
        {"member_id", M.ID},
        {"member_index", I},
        {"kind", M.Kind},
        {"availability", M.available() ? "available" : "metadata_only"},
        {"size_bytes", std::to_string(M.Size)},
        {"header_expanded_offset", std::to_string(M.HeaderOffset)},
        {"expanded_byte_offset", std::to_string(M.Offset)},
        {"original_byte_offset",
         A.Format == "tar" && M.available()
             ? llvm::json::Value(std::to_string(M.Offset))
             : llvm::json::Value(nullptr)},
        {"blob_sha256", M.available() ? llvm::json::Value(M.BlobHash)
                                      : llvm::json::Value(nullptr)},
        {"executable_claim", M.Executable},
        {"path_redacted", true},
        {"link_target_redacted", !M.Link.empty()}});
  }
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(State->Revision)},
      {"archive_id", A.ID},
      {"offset", Offset},
      {"total", A.Members.size()},
      {"items", std::move(Items)},
      {"next_offset", End < A.Members.size() ? llvm::json::Value(End)
                                             : llvm::json::Value(nullptr)},
      {"redaction_policy", "metadata-only-v1"}});
}
} // namespace neverd::web
