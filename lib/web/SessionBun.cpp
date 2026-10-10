#include "SessionInternal.h"

namespace neverd::web {
namespace {
llvm::json::Object summary(const BunExtraction &E, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"extraction_id", E.ID},
      {"artifact_id", E.ArtifactID},
      {"profile", E.Profile},
      {"container_format", E.ContainerFormat},
      {"platform", E.Platform},
      {"architecture", E.Architecture},
      {"layout_status", "compatible"},
      {"producer_version_verified", false},
      {"graph_offset", std::to_string(E.GraphOffset)},
      {"graph_size", std::to_string(E.GraphSize)},
      {"module_count", E.Modules.size()},
      {"region_count", E.Regions.size()},
      {"entry_point_index", E.EntryPoint},
      {"startup_count", E.StartupCount},
      {"flags", E.Flags},
      {"executes_input", false},
      {"source_map_decoding",
       bunSourceMapAvailable() ? "available_on_request" : "zstd_unavailable"},
      {"bytecode_decoding", "opaque"},
      {"native_code_analysis", "not_analyzed"},
      {"text_projection", "strict-unicode-utf8-v1"},
      {"region_coverage", "declared_ranges_and_native_remainder"},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::extractBun(std::string_view ExpectedRevision,
                                std::string_view ArtifactID) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  for (const auto &[ID, E] : State->BunExtractions)
    if (E.ArtifactID == ArtifactID)
      return json(summary(E, State->Revision));
  if (State->BunExtractions.size() >= 4)
    throw Error("bun_cache_budget_exceeded");
  for (const auto &A : State->Published.Artifacts)
    if (A.ID == ArtifactID) {
      auto E = web::extractBun(A);
      auto Reply = json(summary(E, State->Revision));
      const auto ID = E.ID;
      State->BunExtractions.emplace(ID, std::move(E));
      return Reply;
    }
  throw Error("unknown_artifact");
}

std::string Session::bunRecords(std::string_view ExpectedRevision,
                                std::string_view ExtractionID,
                                std::string_view RecordKind, uint64_t Offset,
                                uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  const auto Found = State->BunExtractions.find(std::string(ExtractionID));
  if (Found == State->BunExtractions.end())
    throw Error("unknown_bun_extraction");
  const auto &E = Found->second;
  if (RecordKind != "modules" && RecordKind != "regions")
    throw Error("unsupported_bun_record_kind");
  const auto Count =
      RecordKind == "modules" ? E.Modules.size() : E.Regions.size();
  if (!Limit || Limit > 512 || Offset > Count)
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Count, Offset + Limit);
  auto Ref = [&](uint32_t Index) -> llvm::json::Value {
    return Index == NoBunIndex ? llvm::json::Value(nullptr)
                               : llvm::json::Value(E.Regions[Index].ID);
  };
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    if (RecordKind == "modules") {
      const auto &M = E.Modules[I];
      Items.emplace_back(llvm::json::Object{
          {"module_id", M.ID},
          {"module_index", I},
          {"name_region_id", Ref(M.Name)},
          {"content_region_id", Ref(M.Contents)},
          {"source_map_region_id", Ref(M.SourceMap)},
          {"bytecode_region_id", Ref(M.Bytecode)},
          {"module_info_region_id", Ref(M.ModuleInfo)},
          {"bytecode_origin_region_id", Ref(M.BytecodeOrigin)},
          {"source_artifact_id", M.SourceArtifactID.empty()
                                     ? llvm::json::Value(nullptr)
                                     : llvm::json::Value(M.SourceArtifactID)},
          {"stored_encoding", M.Encoding == 2   ? "utf16le"
                              : M.Encoding == 1 ? "latin1"
                                                : "binary"},
          {"loader_id", M.Loader},
          {"source_type_hint", M.Format == 1   ? "module"
                               : M.Format == 2 ? "commonjs"
                                               : "none"},
          {"side", M.Side ? "client" : "server"},
          {"name_redacted", true}});
    } else {
      const auto &R = E.Regions[I];
      const bool PrivateName =
          R.Kind == "module_name" || R.Kind == "bytecode_origin";
      Items.emplace_back(llvm::json::Object{
          {"region_id", R.ID},
          {"parent_artifact_id", E.ArtifactID},
          {"kind", R.Kind},
          {"blob_sha256", PrivateName ? llvm::json::Value(nullptr)
                                      : llvm::json::Value(R.BlobHash)},
          {"hash_redacted", PrivateName},
          {"offset", std::to_string(R.Offset)},
          {"size", std::to_string(R.Content.size())},
          {"module_index", R.Module == NoBunIndex
                               ? llvm::json::Value(nullptr)
                               : llvm::json::Value(R.Module)}});
    }
  }
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(State->Revision)},
      {"extraction_id", E.ID},
      {"record_kind", std::string(RecordKind)},
      {"items", std::move(Items)},
      {"offset", Offset},
      {"page_complete", End == Count},
      {"next_offset",
       End == Count ? llvm::json::Value(nullptr) : llvm::json::Value(End)},
      {"redaction_policy", "metadata-only-v1"}});
}
} // namespace neverd::web
