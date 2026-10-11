//===- SessionInterfaces.cpp - Passive evidence publication ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit HAR preview/commit and bounded source/capture comparison pages.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"

#include <algorithm>
#include <limits>

namespace neverd::web {
namespace {
uint64_t pageEnd(uint64_t Offset, uint64_t Limit, uint64_t Size) {
  if (!Limit || Limit > 128 || Offset > Size)
    throw Error("invalid_page");
  return std::min<uint64_t>(Size, Offset + Limit);
}
void page(llvm::json::Object &R, llvm::json::Array Items, uint64_t Offset,
          uint64_t End, uint64_t Size) {
  R["items"] = std::move(Items);
  R["offset"] = Offset;
  R["page_complete"] = End == Size;
  R["next_offset"] =
      End == Size ? llvm::json::Value(nullptr) : llvm::json::Value(End);
}
llvm::json::Object endpoint(const InterfaceEndpoint &E) {
  return llvm::json::Object{
      {"status", E.Status},
      {"scheme", E.Scheme},
      {"path_segments", E.PathSegments},
      {"query_parameters", E.QueryParameters},
      {"credentials_present", E.CredentialsPresent},
      {"fragment_present", E.FragmentPresent},
      {"url_redacted", true},
      {"private_comparison_key_available", E.comparable()}};
}
llvm::json::Object fields(const HARFields &F) {
  llvm::json::Object Kinds;
  for (const auto &[Kind, Count] : F.HeaderKinds)
    Kinds[Kind] = Count;
  return llvm::json::Object{{"present", F.Present},
                            {"count", F.Count},
                            {"incomplete", F.Incomplete},
                            {"header_name_classes", std::move(Kinds)},
                            {"values_redacted", true}};
}
llvm::json::Object summary(const HARCapture &H, uint64_t Revision,
                           bool Published) {
  uint64_t Headers = 0, Cookies = 0, Query = 0, Bodies = 0, Sockets = 0;
  uint64_t URLs = 0, URLQuery = 0, PostParameters = 0;
  for (const auto &O : H.Observations) {
    Headers += O.RequestHeaders.Count + O.ResponseHeaders.Count;
    Cookies += O.RequestCookies.Count + O.ResponseCookies.Count;
    Query += O.Query.Count;
    URLQuery += O.Endpoint.QueryParameters;
    URLs += O.Endpoint.Status != "absent";
    PostParameters += O.PostParameters.Count;
    Bodies += O.RequestTextPresent + O.ResponseTextPresent;
    Sockets += O.WebSocketExtensionPresent;
  }
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"capture_id", H.ID},
      {"artifact_id", H.ArtifactID},
      {"blob_sha256", H.BlobHash},
      {"profile", std::string(HARProfile)},
      {"redaction_policy", std::string(InterfaceRedactionPolicy)},
      {"publication_status", Published ? "committed" : "preview"},
      {"observation_count", H.Observations.size()},
      {"creator_metadata_present", H.CreatorPresent},
      {"missing_requests", H.MissingRequests},
      {"missing_responses", H.MissingResponses},
      {"missing_timestamps", H.MissingTimestamps},
      {"invalid_timestamps", H.InvalidTimestamps},
      {"header_values_withheld", Headers},
      {"cookie_records_withheld", Cookies},
      {"query_records_withheld", Query},
      {"url_query_components_withheld", URLQuery},
      {"url_values_withheld", URLs},
      {"post_parameter_records_withheld", PostParameters},
      {"text_bodies_withheld", Bodies},
      {"extension_fields_ignored", H.ExtensionFields},
      {"websocket_extensions_unsupported", Sockets},
      {"urls_and_target_names", "withheld"},
      {"analysis_status", "partial"},
      {"coverage", "har_1_2_declared_http_metadata"},
      {"evidence_class", "imported_observation"},
      {"capture_authenticated", false},
      {"capture_completeness", "unknown"},
      {"bodies_decoded", false},
      {"network_access", false},
      {"executes_input", false}};
}
llvm::json::Object summary(const SourceInterfaces &A, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"interface_analysis_id", A.ID},
      {"source_id", A.SourceID},
      {"binding_analysis_id", A.BindingID},
      {"value_analysis_id", A.ValueID},
      {"profile", std::string(SourceInterfaceProfile)},
      {"analysis_status", A.Status},
      {"reason", A.Reason},
      {"interface_count", A.Records.size()},
      {"excluded_dispatches", A.ExcludedDispatches},
      {"evidence_class", "static_inference"},
      {"intrinsic_verified", false},
      {"reachability", "not_analyzed"},
      {"server_completeness", "unknown"},
      {"redaction_policy", std::string(InterfaceRedactionPolicy)}};
}
llvm::json::Object summary(const InterfaceCorrelation &C, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"correlation_id", C.ID},
      {"interface_analysis_id", C.SourceAnalysisID},
      {"capture_id", C.CaptureID},
      {"profile", std::string(InterfaceCorrelationProfile)},
      {"pair_count", C.Pairs.size()},
      {"eligible_source_candidates", C.EligibleSources},
      {"eligible_observations", C.EligibleObservations},
      {"comparison_rule", "normalized_absolute_origin_path_and_fixed_method"},
      {"ignored_fields",
       llvm::json::Array{"query", "fragment", "headers", "body", "timing"}},
      {"result_meaning", "candidate_only"},
      {"intrinsic_verified", false},
      {"source_execution_observed", false},
      {"analyst_confirmed", false},
      {"server_completeness", "unknown"},
      {"redaction_policy", std::string(InterfaceRedactionPolicy)}};
}
} // namespace

std::string Session::previewHAR(std::string_view Revision,
                                std::string_view ArtifactID) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  // Revoke before any lookup/parsing, including unsuccessful new previews.
  State->PendingHAR.reset();
  State->HARPreviewToken.clear();
  const auto View = State->artifactView(ArtifactID);
  if (!View)
    throw Error("unknown_artifact");
  if (View->Content.size() > MaxHARBytes)
    throw Error("har_byte_budget_exceeded");
  auto H = inspectHAR(ArtifactID,
                      View->Content.read(0, View->Content.size(), MaxHARBytes));
  if (State->HARCaptures.size() >= 4 && !State->HARCaptures.count(H.ID))
    throw Error("har_cache_budget_exceeded");
  if (State->HARPreviewSequence == std::numeric_limits<uint64_t>::max())
    throw Error("har_preview_sequence_exhausted");
  const auto Sequence = State->HARPreviewSequence + 1;
  auto Token =
      identity("har-preview", {H.ID, Revision, InterfaceRedactionPolicy,
                               std::to_string(Sequence)});
  auto R = summary(H, State->Revision, false);
  R["origin"] = llvm::json::Object(View->Origin);
  R["preview_token"] = Token;
  auto Reply = json(std::move(R));
  State->PendingHAR.emplace(std::move(H));
  State->HARPreviewToken = std::move(Token);
  State->HARPreviewSequence = Sequence;
  return Reply;
}

std::string Session::commitHAR(std::string_view Revision,
                               std::string_view PreviewToken) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  auto H = std::move(State->PendingHAR);
  State->PendingHAR.reset();
  auto Token = std::move(State->HARPreviewToken);
  State->HARPreviewToken.clear();
  if (!H || Token.empty() || Token != PreviewToken)
    throw Error("invalid_har_preview");
  const auto View = State->artifactView(H->ArtifactID);
  if (!View || View->BlobHash != H->BlobHash)
    throw Error("har_input_mismatch");
  auto R = summary(*H, State->Revision, true);
  R["origin"] = llvm::json::Object(View->Origin);
  auto Reply = json(std::move(R));
  const auto ID = H->ID;
  State->HARCaptures.insert_or_assign(ID, std::move(*H));
  return Reply;
}

std::string Session::harRecords(std::string_view Revision,
                                std::string_view CaptureID, uint64_t Offset,
                                uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->HARCaptures.find(std::string(CaptureID));
  if (Found == State->HARCaptures.end())
    throw Error("har_capture_not_committed");
  const auto &H = Found->second;
  const auto End = pageEnd(Offset, Limit, H.Observations.size());
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &O = H.Observations[I];
    Items.emplace_back(llvm::json::Object{
        {"observation_id", O.ID},
        {"entry_index", I},
        {"request_id", O.RequestID},
        {"response_id", O.ResponseID},
        {"request_present", O.RequestPresent},
        {"response_present", O.ResponsePresent},
        {"timestamp_status", O.TimestampStatus},
        {"declared_start_time", O.Timestamp.empty()
                                    ? llvm::json::Value(nullptr)
                                    : llvm::json::Value(O.Timestamp)},
        {"method", O.Method},
        {"method_status", O.MethodStatus},
        {"response_status",
         O.Status ? llvm::json::Value(*O.Status) : llvm::json::Value(nullptr)},
        {"endpoint", endpoint(O.Endpoint)},
        {"request_headers", fields(O.RequestHeaders)},
        {"response_headers", fields(O.ResponseHeaders)},
        {"request_cookies", fields(O.RequestCookies)},
        {"response_cookies", fields(O.ResponseCookies)},
        {"query_fields", fields(O.Query)},
        {"post_parameters", fields(O.PostParameters)},
        {"post_data_present", O.PostDataPresent},
        {"request_text_withheld", O.RequestTextPresent},
        {"response_content_present", O.ResponseContentPresent},
        {"response_text_withheld", O.ResponseTextPresent},
        {"response_encoding", O.ResponseEncoding},
        {"websocket_extension_unsupported", O.WebSocketExtensionPresent}});
  }
  auto R = summary(H, State->Revision, true);
  page(R, std::move(Items), Offset, End, H.Observations.size());
  return json(std::move(R));
}

std::string Session::analyzeInterfaces(std::string_view Revision,
                                       std::string_view SourceID) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->Sources.find(std::string(SourceID));
  if (Found == State->Sources.end())
    throw Error("unknown_source");
  for (const auto &[ID, A] : State->InterfaceSources)
    if (A.SourceID == SourceID)
      return json(summary(A, State->Revision));
  if (State->InterfaceSources.size() >= 4)
    throw Error("interface_cache_budget_exceeded");
  auto B = State->Bindings.find(Found->first);
  if (B == State->Bindings.end())
    B = State->Bindings
            .emplace(Found->first, web::analyzeSourceBindings(Found->second))
            .first;
  const auto Values = web::analyzeSourceValues(Found->second);
  auto A = web::analyzeSourceInterfaces(Found->second, B->second, Values);
  auto Reply = json(summary(A, State->Revision));
  State->InterfaceSources.emplace(A.ID, std::move(A));
  return Reply;
#endif
}

std::string Session::interfaceRecords(std::string_view Revision,
                                      std::string_view AnalysisID,
                                      uint64_t Offset, uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->InterfaceSources.find(std::string(AnalysisID));
  if (Found == State->InterfaceSources.end())
    throw Error("unknown_interface_analysis");
  const auto &A = Found->second;
  const auto &S = State->Sources.at(A.SourceID);
  const auto End = pageEnd(Offset, Limit, A.Records.size());
  auto Node = [&](uint32_t I) {
    return I == NoSourceIndex ? llvm::json::Value(nullptr)
                              : llvm::json::Value(S.Nodes.at(I).ID);
  };
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &C = A.Records[I];
    const auto &N = S.Nodes.at(C.Node);
    Items.emplace_back(llvm::json::Object{
        {"interface_id", C.ID},
        {"kind", C.Kind},
        {"node_id", N.ID},
        {"source_byte_offset", std::to_string(N.Start)},
        {"source_byte_length", std::to_string(N.End - N.Start)},
        {"url_node_id", Node(C.URLNode)},
        {"options_node_id", Node(C.OptionsNode)},
        {"method_node_id", Node(C.MethodNode)},
        {"headers_node_id", Node(C.HeadersNode)},
        {"body_node_id", Node(C.BodyNode)},
        {"optional_call", C.Optional},
        {"method", C.Method},
        {"method_status", C.MethodStatus},
        {"url_status", C.URLStatus},
        {"options_status", C.OptionsStatus},
        {"template_expression_count", C.TemplateHoles},
        {"endpoint", endpoint(C.Endpoint)}});
  }
  auto R = summary(A, State->Revision);
  page(R, std::move(Items), Offset, End, A.Records.size());
  return json(std::move(R));
#endif
}

std::string Session::compareInterfaces(std::string_view Revision,
                                       std::string_view AnalysisID,
                                       std::string_view CaptureID) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  for (const auto &[ID, C] : State->InterfaceCorrelations)
    if (C.SourceAnalysisID == AnalysisID && C.CaptureID == CaptureID)
      return json(summary(C, State->Revision));
  if (State->InterfaceCorrelations.size() >= 4)
    throw Error("interface_correlation_cache_budget_exceeded");
  const auto S = State->InterfaceSources.find(std::string(AnalysisID));
  const auto H = State->HARCaptures.find(std::string(CaptureID));
  if (S == State->InterfaceSources.end())
    throw Error("unknown_interface_analysis");
  if (H == State->HARCaptures.end())
    throw Error("har_capture_not_committed");
  auto C = web::correlateInterfaces(S->second, H->second);
  auto Reply = json(summary(C, State->Revision));
  State->InterfaceCorrelations.emplace(C.ID, std::move(C));
  return Reply;
#endif
}

std::string Session::interfaceCorrelationRecords(std::string_view Revision,
                                                 std::string_view CorrelationID,
                                                 uint64_t Offset,
                                                 uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found =
      State->InterfaceCorrelations.find(std::string(CorrelationID));
  if (Found == State->InterfaceCorrelations.end())
    throw Error("unknown_interface_correlation");
  const auto &C = Found->second;
  const auto &S = State->InterfaceSources.at(C.SourceAnalysisID);
  const auto &H = State->HARCaptures.at(C.CaptureID);
  const auto End = pageEnd(Offset, Limit, C.Pairs.size());
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &P = C.Pairs[I];
    const auto &O = H.Observations.at(P.Observation);
    Items.emplace_back(
        llvm::json::Object{{"pair_id", P.ID},
                           {"interface_id", S.Records.at(P.Source).ID},
                           {"observation_id", O.ID},
                           {"request_id", O.RequestID},
                           {"response_id", O.ResponseID},
                           {"source_evidence_class", "static_inference"},
                           {"capture_evidence_class", "imported_observation"}});
  }
  auto R = summary(C, State->Revision);
  page(R, std::move(Items), Offset, End, C.Pairs.size());
  return json(std::move(R));
#endif
}
} // namespace neverd::web
