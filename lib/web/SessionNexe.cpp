//===- SessionNexe.cpp - nexe evidence publication ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Revision-bound extractions with private namespaces and bounded record pages.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"

namespace neverd::web {
namespace {
llvm::json::Object summary(const NexeExtraction &E, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"extraction_id", E.ID},
      {"artifact_id", E.ArtifactID},
      {"container_sha256", E.BlobHash},
      {"profile", std::string(NexeProfile)},
      {"layout_status", "compatible"},
      {"producer_version_verified", false},
      {"native_prefix_validation", "not_checked"},
      {"signature_status", "not_checked"},
      {"runtime_activation", "not_checked"},
      {"resource_offset", std::to_string(E.ResourceOffset)},
      {"resource_size", std::to_string(E.ResourceSize)},
      {"referenced_resource_bytes", std::to_string(E.ReferencedBytes)},
      {"unreferenced_resource_bytes",
       std::to_string(E.ResourceSize - E.ReferencedBytes)},
      {"resource_coverage", E.ReferencedBytes == E.ResourceSize
                                ? "all_bytes_indexed"
                                : "unreferenced_bytes_retained"},
      {"unique_member_ranges", E.UniqueMemberRanges},
      {"member_count", E.Members.size()},
      {"region_count", E.Regions.size()},
      {"region_coverage", "full_container_partition"},
      {"entry_point", "not_inferred"},
      {"v8_decoding", "opaque"},
      {"reader_steps", std::to_string(E.Steps)},
      {"scanned_bytes", std::to_string(E.ScannedBytes)},
      {"hashed_bytes", std::to_string(E.HashedBytes)},
      {"executes_input", false},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::extractNexe(std::string_view Revision,
                                 std::string_view ArtifactID,
                                 std::string_view Profile) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  if (Profile != NexeProfile)
    throw Error("nexe_unsupported_profile");
  uint64_t PrivateBytes = 0;
  for (const auto &[ID, E] : State->NexeExtractions) {
    if (E.ArtifactID == ArtifactID)
      return json(summary(E, State->Revision));
    PrivateBytes += E.PrivateBytes;
  }
  if (State->NexeExtractions.size() >= MaxCachedNexeExtractions)
    throw Error("nexe_cache_budget_exceeded");
  const auto View = State->artifactView(ArtifactID);
  if (!View)
    throw Error("unknown_artifact");
  Artifact A;
  A.ID = ArtifactID;
  A.BlobHash = View->BlobHash;
  A.Content = View->Content;
  auto E = web::extractNexe(A, Profile);
  if (E.PrivateBytes > MaxCachedNexePrivateBytes - PrivateBytes)
    throw Error("nexe_cache_metadata_budget_exceeded");
  auto Reply = json(summary(E, State->Revision));
  const auto ID = E.ID;
  State->NexeExtractions.emplace(ID, std::move(E));
  return Reply;
}

std::string Session::nexeRecords(std::string_view Revision,
                                 std::string_view ExtractionID,
                                 std::string_view Kind, uint64_t Offset,
                                 uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->NexeExtractions.find(std::string(ExtractionID));
  if (Found == State->NexeExtractions.end())
    throw Error("unknown_nexe_extraction");
  const auto &E = Found->second;
  const auto Total = Kind == "members"   ? E.Members.size()
                     : Kind == "regions" ? E.Regions.size()
                                         : throw Error("invalid_record_kind");
  if (!Limit || Limit > MaxNexePageRecords || Offset > Total)
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Total, Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    if (Kind == "members") {
      const auto &M = E.Members[I];
      Items.emplace_back(llvm::json::Object{
          {"member_id", M.ID},
          {"member_index", I},
          {"storage_range_id", M.RangeID},
          {"parent_artifact_id", E.ArtifactID},
          {"namespace_id", E.ID},
          {"blob_sha256", M.BlobHash},
          {"offset", std::to_string(M.Offset)},
          {"resource_byte_offset", std::to_string(M.Offset - E.ResourceOffset)},
          {"size", std::to_string(M.Content.size())},
          {"selection_id", M.ID},
          {"kind", "file"},
          {"path_redacted", true}});
    } else {
      const auto &R = E.Regions[I];
      Items.emplace_back(llvm::json::Object{
          {"region_id", R.ID},
          {"region_index", I},
          {"parent_artifact_id", E.ArtifactID},
          {"kind", R.Kind},
          {"blob_sha256", R.BlobHash},
          {"offset", std::to_string(R.Offset)},
          {"size", std::to_string(R.Content.size())},
          {"selection_id", R.selectable() ? llvm::json::Value(R.ID)
                                          : llvm::json::Value(nullptr)}});
    }
  }
  auto Reply = summary(E, State->Revision);
  Reply["record_kind"] = std::string(Kind);
  Reply["items"] = std::move(Items);
  Reply["offset"] = Offset;
  Reply["page_complete"] = End == Total;
  Reply["next_offset"] =
      End == Total ? llvm::json::Value(nullptr) : llvm::json::Value(End);
  return json(std::move(Reply));
}
} // namespace neverd::web
