//===- SessionSEA.cpp - Node SEA evidence publication ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Revision-bound SEA extractions and metadata-only region pages.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"

namespace neverd::web {
namespace {
llvm::json::Object summary(const SEAExtraction &E, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"extraction_id", E.ID},
      {"artifact_id", E.ArtifactID},
      {"container_sha256", E.BlobHash},
      {"profile", E.Profile},
      {"container_format", E.ContainerFormat},
      {"architecture", E.Architecture},
      {"layout_status", "compatible"},
      {"producer_version_verified", false},
      {"runtime_activation", "not_checked"},
      {"resource_offset", std::to_string(E.ResourceOffset)},
      {"resource_size", std::to_string(E.ResourceSize)},
      {"flags", E.Flags},
      {"asset_count", E.AssetCount},
      {"region_count", E.Regions.size()},
      {"source_artifact_id", E.SourceArtifactID.empty()
                                 ? llvm::json::Value(nullptr)
                                 : llvm::json::Value(E.SourceArtifactID)},
      {"source_status", E.Flags & 2 ? "snapshot_opaque" : "stored_commonjs"},
      {"v8_decoding", "opaque"},
      {"native_code_analysis", "not_analyzed"},
      {"region_coverage", "full_container_partition"},
      {"executes_input", false},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::extractSEA(std::string_view Revision,
                                std::string_view ArtifactID,
                                std::string_view Profile) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  for (const auto &[ID, E] : State->SEAExtractions)
    if (E.ArtifactID == ArtifactID && E.Profile == Profile)
      return json(summary(E, State->Revision));
  if (State->SEAExtractions.size() >= 4)
    throw Error("sea_cache_budget_exceeded");
  const auto View = State->artifactView(ArtifactID);
  if (!View)
    throw Error("unknown_artifact");
  Artifact A;
  A.ID = ArtifactID;
  A.BlobHash = View->BlobHash;
  A.Content = View->Content;
  auto E = web::extractSEA(A, Profile);
  auto Reply = json(summary(E, State->Revision));
  const auto ID = E.ID;
  State->SEAExtractions.emplace(ID, std::move(E));
  return Reply;
}

std::string Session::seaRecords(std::string_view Revision,
                                std::string_view ExtractionID, uint64_t Offset,
                                uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->SEAExtractions.find(std::string(ExtractionID));
  if (Found == State->SEAExtractions.end())
    throw Error("unknown_sea_extraction");
  const auto &E = Found->second;
  if (!Limit || Limit > 128 || Offset > E.Regions.size())
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(E.Regions.size(), Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &R = E.Regions[I];
    Items.emplace_back(llvm::json::Object{
        {"region_id", R.ID},
        {"region_index", I},
        {"parent_artifact_id", E.ArtifactID},
        {"kind", R.Kind},
        {"blob_sha256", R.Private ? llvm::json::Value(nullptr)
                                  : llvm::json::Value(R.BlobHash)},
        {"hash_redacted", R.Private},
        {"offset", std::to_string(R.Offset)},
        {"size", std::to_string(R.Content.size())},
        {"selection_id", R.selectable() ? llvm::json::Value(R.ID)
                                        : llvm::json::Value(nullptr)}});
  }
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(State->Revision)},
      {"extraction_id", E.ID},
      {"items", std::move(Items)},
      {"offset", Offset},
      {"page_complete", End == E.Regions.size()},
      {"next_offset", End == E.Regions.size() ? llvm::json::Value(nullptr)
                                              : llvm::json::Value(End)},
      {"redaction_policy", "metadata-only-v1"}});
}
} // namespace neverd::web
