#include "SessionInternal.h"

#include <algorithm>

namespace neverd::web {
namespace {
const SourceMap &mapByID(const std::map<std::string, SourceMap> &Maps,
                         std::string_view ID) {
  const auto It = Maps.find(std::string(ID));
  if (It == Maps.end())
    throw Error("unknown_source_map");
  return It->second;
}

std::string summary(const SourceMap &Map, uint64_t Revision) {
  llvm::json::Object Result{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"map_id", Map.ID},
      {"artifact_id", Map.ArtifactID},
      {"blob_sha256", Map.BlobHash},
      {"profile", Map.Profile},
      {"format_status", "decoded"},
      {"layout", Map.Embedding ? "bun_serialized"
                 : Map.Indexed ? "index"
                               : "basic"},
      {"source_count", Map.Sources.size()},
      {"name_count", Map.Names.size()},
      {"segment_count", Map.Segments.size()},
      {"association_status", Map.Embedding ? "container_assertion" : "unbound"},
      {"coordinate_validation", "not_checked"},
      {"mapping_coverage", Map.MappedAnchorsOnly
                               ? "retained_mapped_anchors_only"
                               : "encoded_segments"},
      {"name_metadata",
       Map.MappedAnchorsOnly ? "discarded_by_producer" : "as_encoded"},
      {"unmapped_boundaries",
       Map.MappedAnchorsOnly ? "discarded_by_producer" : "as_encoded"},
      {"redaction_policy", "metadata-only-v1"}};
  if (Map.Embedding) {
    const auto &E = *Map.Embedding;
    Result["storage"] = llvm::json::Object{
        {"container_artifact_id", E.ContainerArtifactID},
        {"extraction_id", E.ExtractionID},
        {"module_id", E.ModuleID},
        {"offset", std::to_string(E.Offset)},
        {"size", std::to_string(E.Size)},
        {"mappings_offset", std::to_string(E.MappingsOffset)},
        {"mappings_size", std::to_string(E.MappingsSize)}};
    Result["generated_artifact_id"] =
        E.GeneratedArtifactID.empty()
            ? llvm::json::Value(nullptr)
            : llvm::json::Value(E.GeneratedArtifactID);
    Result["provenance_verified"] = false;
    Result["source_root_metadata"] = "discarded_by_producer";
    Result["ignore_list_metadata"] = "discarded_by_producer";
    Result["generated_file_metadata"] = "discarded_by_producer";
  }
  return json(std::move(Result));
}

uint64_t pageEnd(uint64_t Size, uint64_t Offset, uint64_t Limit) {
  if (!Limit || Limit > 512 || Offset > Size)
    throw Error("invalid_page");
  return std::min(Size, Offset + Limit);
}

llvm::json::Object page(const SourceMap &Map, uint64_t Revision,
                        uint64_t Offset, uint64_t End, uint64_t Size,
                        llvm::json::Array Items) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"map_id", Map.ID},
      {"items", std::move(Items)},
      {"offset", Offset},
      {"page_complete", End == Size},
      {"next_offset",
       End == Size ? llvm::json::Value(nullptr) : llvm::json::Value(End)},
      {"association_status", Map.Embedding ? "container_assertion" : "unbound"},
      {"mapping_coverage", Map.MappedAnchorsOnly
                               ? "retained_mapped_anchors_only"
                               : "encoded_segments"},
      {"redaction_policy", "metadata-only-v1"}};
}

llvm::json::Object position(TextPosition Position) {
  return llvm::json::Object{{"line", Position.Line},
                            {"utf16_column", Position.UTF16Column}};
}

llvm::json::Object segment(const SourceMap &Map, uint64_t Index) {
  const auto &S = Map.Segments[Index];
  const auto Ordinal = std::to_string(Index);
  llvm::json::Object Item{
      {"segment_id", identity("map-segment", {Map.ID, Ordinal})},
      {"index", Index},
      {"generated", position(S.Generated)},
      {"mapped_source_id",
       S.SourceIndex ? llvm::json::Value(Map.Sources[*S.SourceIndex].ID)
                     : llvm::json::Value(nullptr)},
      {"original", S.SourceIndex ? llvm::json::Value(position(S.Original))
                                 : llvm::json::Value(nullptr)},
      {"name_present", Map.MappedAnchorsOnly
                           ? llvm::json::Value(nullptr)
                           : llvm::json::Value(S.NameIndex.has_value())},
      {"name_redacted", true}};
  return Item;
}

void addBytePosition(llvm::json::Object &Item, const char *ByteKey,
                     const char *StatusKey, const TextCoordinates &Coordinates,
                     TextPosition Position) {
  try {
    Item[ByteKey] = std::to_string(Coordinates.byteOffset(Position));
    Item[StatusKey] = "valid_boundary";
  } catch (const Error &) {
    Item[ByteKey] = nullptr;
    Item[StatusKey] = "out_of_bounds_or_non_boundary";
  }
}
} // namespace

std::string Session::analyzeSourceMap(std::string_view ExpectedRevision,
                                      std::string_view ArtifactID) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  for (const auto &[ID, Map] : State->Maps)
    if (Map.ArtifactID == ArtifactID)
      return summary(Map, State->Revision);
  if (State->Maps.size() >= 4 ||
      State->CachedMapSegments > 200000 - MaxSourceMapSegments)
    throw Error("source_map_cache_budget_exceeded");
  const BunExtraction *Embedding = nullptr;
  const BunModule *Module = nullptr;
  for (const auto &[ID, E] : State->BunExtractions)
    for (const auto &M : E.Modules)
      if (M.SourceMap != NoBunIndex &&
          E.Regions[M.SourceMap].ID == ArtifactID) {
        Embedding = &E;
        Module = &M;
      }
  auto Map =
      Embedding
          ? decodeBunSourceMap(*Embedding, *Module)
          : decodeSourceMap(ArtifactID,
                            State->sourceBytes(ArtifactID, MaxSourceMapBytes,
                                               "json_byte_budget_exceeded"));
  auto Reply = summary(Map, State->Revision);
  const auto Count = Map.Segments.size();
  const auto ID = Map.ID;
  State->Maps.emplace(ID, std::move(Map));
  State->CachedMapSegments += Count;
  return Reply;
}

std::string Session::sourceMapSources(std::string_view ExpectedRevision,
                                      std::string_view MapID, uint64_t Offset,
                                      uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  const auto &Map = mapByID(State->Maps, MapID);
  const auto End = pageEnd(Map.Sources.size(), Offset, Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &Source = Map.Sources[I];
    llvm::json::Object Item{
        {"mapped_source_id", Source.ID},
        {"source_index", I},
        {"artifact_id", Source.Contents ? llvm::json::Value(Source.ID)
                                        : llvm::json::Value(nullptr)},
        {"parent_id", Map.ArtifactID},
        {"origin_kind", Source.Storage    ? "bun_source_map_zstd_content"
                        : Source.Contents ? "source_map_sources_content"
                                          : "source_map_reference"},
        {"blob_sha256", Source.Contents
                            ? llvm::json::Value(sha256(*Source.Contents))
                            : llvm::json::Value(nullptr)},
        {"size", Source.Contents ? llvm::json::Value(
                                       std::to_string(Source.Contents->size()))
                                 : llvm::json::Value(nullptr)},
        {"content_status", Source.Contents ? "embedded" : "not_supplied"},
        {"ignored_claim", Map.MappedAnchorsOnly
                              ? llvm::json::Value(nullptr)
                              : llvm::json::Value(Source.Ignored)},
        {"name_redacted", true}};
    if (Source.Storage) {
      const auto &S = *Source.Storage;
      Item["storage"] = llvm::json::Object{
          {"container_artifact_id", Map.Embedding->ContainerArtifactID},
          {"encoding", "zstd-utf8"},
          {"name_offset", std::to_string(S.NameOffset)},
          {"name_size", std::to_string(S.NameSize)},
          {"name_hash_redacted", true},
          {"content_offset", std::to_string(S.ContentOffset)},
          {"content_size", std::to_string(S.ContentSize)},
          {"content_sha256", S.ContentHash}};
    }
    Items.emplace_back(std::move(Item));
  }
  return json(page(Map, State->Revision, Offset, End, Map.Sources.size(),
                   std::move(Items)));
}

std::string Session::sourceMapSegments(std::string_view ExpectedRevision,
                                       std::string_view MapID, uint64_t Offset,
                                       uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  const auto &Map = mapByID(State->Maps, MapID);
  const auto End = pageEnd(Map.Segments.size(), Offset, Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I)
    Items.emplace_back(segment(Map, I));
  return json(page(Map, State->Revision, Offset, End, Map.Segments.size(),
                   std::move(Items)));
}

std::string Session::lookupSourceMap(std::string_view ExpectedRevision,
                                     std::string_view MapID,
                                     std::string_view GeneratedSourceID,
                                     uint64_t ByteOffset) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  const auto &Map = mapByID(State->Maps, MapID);
  const auto It = State->Sources.find(std::string(GeneratedSourceID));
  if (It == State->Sources.end())
    throw Error("unknown_source");
  const auto &Source = It->second;
  const auto Bytes = State->sourceBytes(Source.ArtifactID, MaxJavaScriptBytes,
                                        "source_byte_budget_exceeded");
  const TextCoordinates Coordinates(Bytes);
  const auto Position = Coordinates.position(ByteOffset);
  const auto Anchors = sourceMapAnchors(Map, Position);
  if (Anchors.size() > 512)
    throw Error("source_map_candidate_budget_exceeded");
  llvm::json::Array Items;
  std::map<uint64_t, std::unique_ptr<TextCoordinates>> OriginalCoordinates;
  for (const auto I : Anchors) {
    const auto &Anchor = Map.Segments[I];
    auto Item = segment(Map, I);
    Item["selection"] =
        Anchor.Generated == Position ? "exact_anchor" : "preceding_anchor";
    if (Map.MappedAnchorsOnly)
      Item["mapping_scope"] = "anchor_only_intervening_boundaries_unknown";
    addBytePosition(Item, "generated_byte", "generated_position_validation",
                    Coordinates, Anchor.Generated);
    Item["original_byte"] = nullptr;
    Item["original_position_validation"] =
        Anchor.SourceIndex ? "content_not_supplied" : "unmapped";
    if (Anchor.SourceIndex) {
      const auto &Mapped = Map.Sources[*Anchor.SourceIndex];
      if (Mapped.Contents) {
        auto [Original, Inserted] =
            OriginalCoordinates.try_emplace(*Anchor.SourceIndex);
        if (Inserted) {
          try {
            Original->second =
                std::make_unique<TextCoordinates>(*Mapped.Contents);
          } catch (const Error &) {
            // Remember rejected coordinate tables too; duplicate anchors
            // cannot repeatedly rebuild the same over-budget content.
          }
        }
        if (Original->second)
          addBytePosition(Item, "original_byte", "original_position_validation",
                          *Original->second, Anchor.Original);
        else
          Item["original_position_validation"] =
              "content_outside_coordinate_profile";
      }
    }
    Items.emplace_back(std::move(Item));
  }
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(State->Revision)},
      {"map_id", Map.ID},
      {"generated_source_id", Source.ID},
      {"query_byte", std::to_string(ByteOffset)},
      {"items", std::move(Items)},
      {"checked_scope", "returned_anchors"},
      {"association_kind",
       Map.Embedding && Map.Embedding->GeneratedArtifactID == Source.ArtifactID
           ? "container_assertion"
           : "caller_assertion"},
      {"mapping_coverage", Map.MappedAnchorsOnly
                               ? "retained_mapped_anchors_only"
                               : "encoded_segments"},
      {"provenance_verified", false},
      {"redaction_policy", "metadata-only-v1"}});
#endif
}
} // namespace neverd::web
