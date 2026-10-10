//===- SessionStreams.cpp - Reviewed passive transcript publication -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Revision-bound preview receipts, bounded caches and redacted record pages.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"

#include <algorithm>
#include <limits>

namespace neverd::web {
namespace {
llvm::json::Object summary(const StreamCapture &C, uint64_t Revision,
                           bool Committed) {
  uint64_t Malformed = 0, Unfinished = 0, Dispatched = 0, MissingDirection = 0;
  for (const auto &R : C.Records) {
    Malformed += R.Kind == "malformed_json" || R.RPC == "invalid_message" ||
                 R.RPC == "invalid_envelope";
    Unfinished += !R.Terminated;
    Dispatched += R.Dispatched;
    MissingDirection += R.Direction == "absent" || R.Direction == "invalid";
  }
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"stream_capture_id", C.ID},
      {"artifact_id", C.ArtifactID},
      {"blob_sha256", C.BlobHash},
      {"profile", C.Profile},
      {"redaction_policy", std::string(StreamRedactionPolicy)},
      {"redaction_receipt",
       identity("stream-redaction-receipt",
                {C.BlobHash, C.Profile, StreamRedactionPolicy})},
      {"publication_status", Committed ? "committed" : "preview"},
      {"record_count", C.Records.size()},
      {"json_nodes_examined", C.JSONWork},
      {"malformed_records", Malformed},
      {"unterminated_records", Unfinished},
      {"dispatched_sse_events", Dispatched},
      {"records_without_direction", MissingDirection},
      {"recorded_pair_candidates", C.PairCount},
      {"relation_status", C.RelationStatus},
      {"bom_present", C.BOM},
      {"all_input_values", "withheld"},
      {"names_ids_sessions_timestamps", "withheld"},
      {"evidence_class", "imported_observation"},
      {"analysis_status", "partial"},
      {"capture_authenticated", false},
      {"capture_completeness", "unknown"},
      {"protocol_negotiation_verified", false},
      {"method_schemas_validated", false},
      {"source_execution_observed", false},
      {"analyst_confirmed", false},
      {"network_access", false},
      {"executes_input", false}};
}
} // namespace

std::string Session::previewStream(std::string_view Revision,
                                   std::string_view ArtifactID,
                                   std::string_view Profile) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  State->PendingStream.reset();
  State->StreamPreviewToken.clear();
  const auto View = State->artifactView(ArtifactID);
  if (!View)
    throw Error("unknown_artifact");
  if (View->Content.size() > MaxStreamBytes)
    throw Error("stream_byte_budget_exceeded");
  auto C = inspectStream(
      ArtifactID, View->Content.read(0, View->Content.size(), MaxStreamBytes),
      Profile);
  if (State->StreamCaptures.size() >= 4 && !State->StreamCaptures.count(C.ID))
    throw Error("stream_cache_budget_exceeded");
  if (State->StreamPreviewSequence == std::numeric_limits<uint64_t>::max())
    throw Error("stream_preview_sequence_exhausted");
  const auto Sequence = State->StreamPreviewSequence + 1;
  auto Token =
      identity("stream-preview", {C.ID, Revision, StreamRedactionPolicy,
                                  std::to_string(Sequence)});
  auto R = summary(C, State->Revision, false);
  R["origin"] = llvm::json::Object(View->Origin);
  R["preview_token"] = Token;
  auto Reply = json(std::move(R));
  State->PendingStream.emplace(std::move(C));
  State->StreamPreviewToken = std::move(Token);
  State->StreamPreviewSequence = Sequence;
  return Reply;
}

std::string Session::commitStream(std::string_view Revision,
                                  std::string_view PreviewToken) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  auto C = std::move(State->PendingStream);
  State->PendingStream.reset();
  auto Token = std::move(State->StreamPreviewToken);
  State->StreamPreviewToken.clear();
  if (!C || Token.empty() || Token != PreviewToken)
    throw Error("invalid_stream_preview");
  const auto View = State->artifactView(C->ArtifactID);
  if (!View || View->BlobHash != C->BlobHash)
    throw Error("stream_input_mismatch");
  auto R = summary(*C, State->Revision, true);
  R["origin"] = llvm::json::Object(View->Origin);
  auto Reply = json(std::move(R));
  const auto ID = C->ID;
  State->StreamCaptures.insert_or_assign(ID, std::move(*C));
  return Reply;
}

std::string Session::streamRecords(std::string_view Revision,
                                   std::string_view CaptureID, uint64_t Offset,
                                   uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->StreamCaptures.find(std::string(CaptureID));
  if (Found == State->StreamCaptures.end())
    throw Error("stream_capture_not_committed");
  const auto &C = Found->second;
  if (!Limit || Limit > 128 || Offset > C.Records.size())
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(C.Records.size(), Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &R = C.Records[I];
    Items.emplace_back(llvm::json::Object{
        {"record_id", R.ID},
        {"record_index", I},
        {"kind", R.Kind},
        {"byte_offset", std::to_string(R.Offset)},
        {"byte_length", std::to_string(R.Length)},
        {"first_line", R.FirstLine},
        {"line_count", R.LineCount},
        {"terminated", R.Terminated},
        {"json_kind", R.JSONKind},
        {"rpc_shape", R.RPC},
        {"method_class", R.Method},
        {"recorded_id_kind", R.IDKind},
        {"direction", R.Direction},
        {"recorded_session_present", R.SessionPresent},
        {"timestamp_status", R.Timestamp},
        {"params_present", R.ParamsPresent},
        {"result_present", R.ResultPresent},
        {"error_present", R.ErrorPresent},
        {"relation", R.Relation},
        {"peer_record_id", R.Peer ? llvm::json::Value(C.Records[*R.Peer].ID)
                                  : llvm::json::Value(nullptr)},
        {"sse_dispatched", R.Dispatched},
        {"sse_event_class", R.Event},
        {"sse_data_lines", R.DataLines},
        {"sse_joined_data_bytes", R.DataBytes},
        {"sse_comments", R.Comments},
        {"sse_ignored_fields", R.IgnoredFields},
        {"sse_id_fields", R.IDFields},
        {"sse_invalid_id_fields", R.InvalidIDFields},
        {"sse_retry_fields", R.RetryFields},
        {"sse_id_buffer_nonempty", R.EventIDPresent},
        {"sse_id_reset", R.EventIDReset},
        {"all_input_values_withheld", true}});
  }
  auto R = summary(C, State->Revision, true);
  R["items"] = std::move(Items);
  R["offset"] = Offset;
  R["page_complete"] = End == C.Records.size();
  R["next_offset"] = End == C.Records.size() ? llvm::json::Value(nullptr)
                                             : llvm::json::Value(End);
  R["relation_rule"] =
      "recorded_session_typed_id_opposite_direction_unique_order";
  return json(std::move(R));
}
} // namespace neverd::web
