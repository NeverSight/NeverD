//===- SessionAnchor.cpp - Source and storage anchor queries -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source and storage anchor queries.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"

namespace neverd::web {
std::string Session::sourceAnchor(std::string_view Revision,
                                  std::string_view SourceID, uint64_t Offset,
                                  uint64_t Length,
                                  std::string_view ViewID) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->Sources.find(std::string(SourceID));
  if (Found == State->Sources.end())
    throw Error("unknown_source");
  const auto &S = Found->second;
  if (Offset > MaxJavaScriptBytes || Length > MaxJavaScriptBytes - Offset)
    throw Error("invalid_source_position");
  const SourceView *View = nullptr;
  if (!ViewID.empty()) {
    const auto It = State->SourceViews.find(S.ID);
    if (It == State->SourceViews.end() || It->second.ID != ViewID)
      throw Error("source_view_not_committed");
    View = &It->second;
  }
  const BunExtraction *Extraction = nullptr;
  const BunModule *Module = nullptr;
  for (const auto &[ID, E] : State->BunExtractions)
    for (const auto &M : E.Modules)
      if (M.SourceArtifactID == S.ArtifactID && !M.SourceArtifactID.empty()) {
        Extraction = &E;
        Module = &M;
      }
  std::string Bytes;
  llvm::json::Object Storage;
  if (Extraction) {
    auto Projection = bunSourceRange(*Extraction, *Module, Offset, Length,
                                     MaxJavaScriptBytes);
    Bytes = std::move(Projection.Text);
    const auto &R = Extraction->Regions[Module->Contents];
    Storage = llvm::json::Object{
        {"kind", "bun_source_member"},
        {"byte_offset_basis", "selected_bun_container"},
        {"container_artifact_id", Extraction->ArtifactID},
        {"extraction_id", Extraction->ID},
        {"module_id", Module->ID},
        {"region_id", R.ID},
        {"region_sha256", R.BlobHash},
        {"byte_offset", std::to_string(Projection.Offset)},
        {"byte_length", std::to_string(Projection.Size)},
        {"member_byte_offset", std::to_string(R.Offset)},
        {"member_byte_length", std::to_string(R.Content.size())},
        {"stored_encoding", Module->Encoding == 2   ? "utf16le"
                            : Module->Encoding == 1 ? "latin1"
                                                    : "binary"},
        {"text_encoding", "utf8"},
        {"mapping",
         Module->Encoding ? "unicode_boundary_conversion" : "byte_identity"},
        {"producer_version_verified", false}};
    if (auto Container = State->artifactView(Extraction->ArtifactID))
      Storage["container_origin"] = std::move(Container->Origin);
  } else {
    Bytes = State->sourceBytes(S.ArtifactID, MaxJavaScriptBytes,
                               "source_byte_budget_exceeded");
    if (auto Direct = State->artifactView(S.ArtifactID)) {
      Storage = std::move(Direct->Origin);
      Storage["blob_sha256"] = Direct->BlobHash;
      if (Direct->DirectStorage) {
        Storage["mapping"] = "byte_identity";
        Storage["member_byte_offset"] = std::to_string(Direct->StorageOffset);
        Storage["member_byte_length"] = std::to_string(Direct->Content.size());
        Storage["byte_offset"] = std::to_string(Direct->StorageOffset + Offset);
        Storage["byte_length"] = std::to_string(Length);
      } else {
        Storage["mapping"] = "containing_compressed_frame";
        Storage["byte_offset"] = nullptr;
        Storage["byte_length"] = nullptr;
        Storage["expanded_byte_offset"] =
            std::to_string(Direct->StorageOffset + Offset);
        Storage["expanded_byte_length"] = std::to_string(Length);
      }
    }
    if (Storage.empty())
      for (const auto &[ID, Map] : State->Maps)
        for (uint64_t I = 0; I < Map.Sources.size(); ++I) {
          const auto &M = Map.Sources[I];
          if (M.ID != S.ArtifactID)
            continue;
          Storage = llvm::json::Object{
              {"kind",
               M.Storage ? "bun_map_compressed_source" : "source_map_content"},
              {"map_id", Map.ID},
              {"map_artifact_id", Map.ArtifactID},
              {"source_index", I},
              {"mapping", M.Storage ? "containing_compressed_frame"
                                    : "encoded_member_not_located"},
              {"byte_offset", nullptr},
              {"byte_length", nullptr}};
          if (M.Storage) {
            if (!Map.Embedding)
              throw Error("invalid_source_storage_model");
            Storage["container_artifact_id"] =
                Map.Embedding->ContainerArtifactID;
            Storage["extraction_id"] = Map.Embedding->ExtractionID;
            Storage["module_id"] = Map.Embedding->ModuleID;
            Storage["frame_sha256"] = M.Storage->ContentHash;
            Storage["byte_offset"] = std::to_string(M.Storage->ContentOffset);
            Storage["byte_length"] = std::to_string(M.Storage->ContentSize);
            Storage["provenance_verified"] = false;
          }
        }
  }
  if (Storage.empty())
    throw Error("source_storage_unavailable");
  if (sha256(Bytes) != S.BlobHash)
    throw Error("source_anchor_bytes_mismatch");
  const TextCoordinates Coordinates(Bytes);
  const auto Start = Coordinates.position(Offset);
  const auto End = Coordinates.position(Offset + Length);
  llvm::json::Object Reply{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(State->Revision)},
      {"anchor_profile", std::string(SourceAnchorProfile)},
      {"anchor_id", identity("source-anchor",
                             {S.ID, SourceAnchorProfile, std::to_string(Offset),
                              std::to_string(Length)})},
      {"source_id", S.ID},
      {"source_parse_status", S.ParseStatus},
      {"artifact_id", S.ArtifactID},
      {"source_sha256", S.BlobHash},
      {"byte_offset", std::to_string(Offset)},
      {"byte_length", std::to_string(Length)},
      {"start", llvm::json::Object{{"line", Start.Line},
                                   {"utf16_column", Start.UTF16Column}}},
      {"end", llvm::json::Object{{"line", End.Line},
                                 {"utf16_column", End.UTF16Column}}},
      {"storage", std::move(Storage)},
      {"view", nullptr},
      {"redaction_policy", "metadata-only-v1"}};
  if (View) {
    const auto R = locateSourceView(*View, Offset, Offset + Length);
    Reply["view"] = llvm::json::Object{
        {"view_id", View->ID},
        {"policy_id", View->PolicyID},
        {"mapping", R.Mapping},
        {"byte_offset", std::to_string(R.Start)},
        {"byte_length", std::to_string(R.End - R.Start)},
        {"source_cover_byte_offset", std::to_string(R.SourceStart)},
        {"source_cover_byte_length",
         std::to_string(R.SourceEnd - R.SourceStart)},
        {"first_segment", R.FirstSegment},
        {"last_segment_exclusive", R.LastSegment}};
  }
  return json(std::move(Reply));
#endif
}
} // namespace neverd::web
