#include "PathPolicy.h"
#include "SessionInternal.h"

namespace neverd::web {
namespace {
llvm::json::Object summary(const AsarExtraction &E, uint64_t Revision) {
  uint64_t Available = 0, Missing = 0;
  for (const auto &M : E.Members) {
    Available += M.available();
    Missing += M.Kind != "directory" && !M.available();
  }
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"extraction_id", E.ID},
      {"artifact_id", E.ArtifactID},
      {"unpacked_directory_id", E.UnpackedDirectoryID.empty()
                                    ? llvm::json::Value(nullptr)
                                    : llvm::json::Value(E.UnpackedDirectoryID)},
      {"profile", std::string(AsarProfile)},
      {"path_policy", std::string(ArchivePathProfile)},
      {"layout_status", "compatible"},
      {"extraction_status", Missing ? "partial" : "complete"},
      {"coverage", "declared_members"},
      {"data_offset", std::to_string(E.DataOffset)},
      {"unreferenced_payload_bytes",
       std::to_string(E.UnreferencedPayloadBytes)},
      {"member_count", E.Members.size()},
      {"available_file_count", Available},
      {"unavailable_member_count", Missing},
      {"producer_version_verified", false},
      {"authenticates_publisher", false},
      {"executes_input", false},
      {"follows_links", false},
      {"host_companion_discovery", false},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::extractAsar(std::string_view Revision,
                                 std::string_view ArtifactID,
                                 std::string_view UnpackedID) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  for (const auto &[ID, E] : State->AsarExtractions)
    if (E.ArtifactID == ArtifactID && E.UnpackedDirectoryID == UnpackedID)
      return json(summary(E, State->Revision));
  if (State->AsarExtractions.size() >= 4)
    throw Error("asar_cache_budget_exceeded");
  auto E = web::extractAsar(State->Published, ArtifactID, UnpackedID);
  auto Reply = json(summary(E, State->Revision));
  const auto ID = E.ID;
  State->AsarExtractions.emplace(ID, std::move(E));
  return Reply;
}

std::string Session::asarRecords(std::string_view Revision,
                                 std::string_view ExtractionID, uint64_t Offset,
                                 uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->AsarExtractions.find(std::string(ExtractionID));
  if (Found == State->AsarExtractions.end())
    throw Error("unknown_asar_extraction");
  const auto &E = Found->second;
  if (!Limit || Limit > 512 || Offset > E.Members.size())
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(E.Members.size(), Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &M = E.Members[I];
    Items.emplace_back(llvm::json::Object{
        {"member_id", M.ID},
        {"parent_id", M.ParentID},
        {"member_index", I},
        {"kind", M.Kind},
        {"availability", M.Status},
        {"storage", M.Unpacked ? "unpacked" : "packed"},
        {"byte_length", std::to_string(M.Size)},
        {"storage_artifact_id", M.StorageArtifactID.empty()
                                    ? llvm::json::Value(nullptr)
                                    : llvm::json::Value(M.StorageArtifactID)},
        {"byte_offset", M.Kind == "file" && !M.StorageArtifactID.empty()
                            ? llvm::json::Value(std::to_string(M.Offset))
                            : llvm::json::Value(nullptr)},
        {"blob_sha256", M.BlobHash.empty() ? llvm::json::Value(nullptr)
                                           : llvm::json::Value(M.BlobHash)},
        {"integrity_status", M.IntegrityStatus},
        {"executable_claim", M.Executable},
        {"name_redacted", true},
        {"link_target_redacted", M.Kind == "link"}});
  }
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(State->Revision)},
      {"extraction_id", E.ID},
      {"items", std::move(Items)},
      {"offset", Offset},
      {"page_complete", End == E.Members.size()},
      {"next_offset", End == E.Members.size() ? llvm::json::Value(nullptr)
                                              : llvm::json::Value(End)},
      {"redaction_policy", "metadata-only-v1"}});
}
} // namespace neverd::web
