//===- SessionView.cpp - Source display view lifecycle -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source display view lifecycle.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"

#include <algorithm>
#include <limits>

namespace neverd::web {
namespace {
llvm::json::Object viewSummary(const SourceView &V, uint64_t Revision,
                               bool Published) {
  llvm::json::Object Classes;
  for (const auto &[Kind, Count] : V.RegionCounts)
    Classes[Kind] = Count;
  llvm::json::Array Ranges;
  for (const auto &R : V.Reviewed)
    Ranges.emplace_back(
        llvm::json::Object{{"byte_offset", std::to_string(R.Start)},
                           {"byte_length", std::to_string(R.End - R.Start)}});
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"view_id", V.ID},
      {"source_id", V.SourceID},
      {"artifact_id", V.ArtifactID},
      {"source_sha256", V.SourceHash},
      {"policy_id", V.PolicyID},
      {"view_profile", std::string(SourceViewProfile)},
      {"lexeme_profile", std::string(JavaScriptLexemeProfile)},
      {"binding_analysis_id", V.BindingAnalysisID},
      {"binding_status", V.BindingStatus},
      {"source_bytes", std::to_string(V.SourceSize)},
      {"view_bytes", std::to_string(V.Text.size())},
      {"segment_count", V.Segments.size()},
      {"region_classes", std::move(Classes)},
      {"hidden_regions", V.HiddenRegions},
      {"reviewed_bytes", std::to_string(V.ReviewedBytes)},
      {"reviewed_ranges", std::move(Ranges)},
      {"local_review",
       V.Reviewed.empty() ? "no_original_ranges" : "caller_assertion"},
      {"publication_status", Published ? "committed" : "preview"},
      {"redaction_policy", "structural-with-reviewed-ranges-v1"},
      {"content_role", "untrusted_target_source"},
      {"view_kind", "display_projection"},
      {"semantic_rewrite", false}};
}
} // namespace

std::string Session::previewSourceView(std::string_view Revision,
                                       std::string_view SourceID,
                                       std::string_view Options) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  State->PendingSourceView.reset();
  State->SourceViewToken.clear();
  const auto Found = State->Sources.find(std::string(SourceID));
  if (Found == State->Sources.end())
    throw Error("unknown_source");
  if (State->SourceViews.size() >= MaxPublishedSourceViews &&
      !State->SourceViews.count(Found->first))
    throw Error("source_view_cache_budget_exceeded");
  const auto Policy = sourceViewPolicy(Options);
  auto Bindings = State->Bindings.find(Found->first);
  if (Bindings == State->Bindings.end())
    Bindings =
        State->Bindings
            .emplace(Found->first, web::analyzeSourceBindings(Found->second))
            .first;
  const auto Bytes = State->sourceBytes(
      Found->second.ArtifactID, MaxJavaScriptBytes, "source_byte_limit");
  auto View = makeSourceView(Found->second, Bindings->second, Bytes, Policy);
  if (State->SourceViewSequence == std::numeric_limits<uint64_t>::max())
    throw Error("source_view_sequence_exhausted");
  const auto Sequence = State->SourceViewSequence + 1;
  auto Token = identity("source-view-preview",
                        {View.ID, Revision, std::to_string(Sequence)});
  auto Summary = viewSummary(View, State->Revision, false);
  Summary["preview_token"] = Token;
  auto Reply = json(std::move(Summary));
  State->PendingSourceView.emplace(std::move(View));
  State->SourceViewToken = std::move(Token);
  State->SourceViewSequence = Sequence;
  return Reply;
#endif
}

std::string Session::commitSourceView(std::string_view Revision,
                                      std::string_view PreviewToken) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  // Every current-revision attempt consumes the pending candidate, including
  // an incorrect token. Failures cannot alter the published projection.
  auto Pending = std::move(State->PendingSourceView);
  State->PendingSourceView.reset();
  auto Token = std::move(State->SourceViewToken);
  State->SourceViewToken.clear();
  if (!Pending || Token.empty() || PreviewToken != Token)
    throw Error("invalid_source_view_preview");
  auto Reply = json(viewSummary(*Pending, State->Revision, true));
  const auto Key = Pending->SourceID;
  State->SourceViews.insert_or_assign(Key, std::move(*Pending));
  return Reply;
#endif
}

std::string Session::sourceViewRecords(std::string_view Revision,
                                       std::string_view ViewID, uint64_t Offset,
                                       uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const SourceView *V = nullptr;
  for (const auto &[ID, View] : State->SourceViews)
    if (View.ID == ViewID)
      V = &View;
  const bool Published = V != nullptr;
  if (!V && State->PendingSourceView && State->PendingSourceView->ID == ViewID)
    V = &*State->PendingSourceView;
  if (!V)
    throw Error("unknown_source_view");
  if (!Limit || Limit > 512 || Offset > V->Segments.size())
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(V->Segments.size(), Offset + Limit);
  const auto &Bindings = State->Bindings.at(V->SourceID);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &S = V->Segments[I];
    Items.emplace_back(llvm::json::Object{
        {"segment_id",
         identity("source-view-segment", {V->ID, std::to_string(I)})},
        {"kind", S.Kind},
        {"source_byte_offset", std::to_string(S.SourceStart)},
        {"source_byte_length", std::to_string(S.SourceEnd - S.SourceStart)},
        {"view_byte_offset", std::to_string(S.Start)},
        {"view_byte_length", std::to_string(S.End - S.Start)},
        {"mapping", S.Identity ? "byte_identity" : "whole_region"},
        {"locally_reviewed", S.Reviewed},
        {"binding_id",
         S.Binding == NoSourceIndex
             ? llvm::json::Value(nullptr)
             : llvm::json::Value(Bindings.Bindings.at(S.Binding).ID)}});
  }
  auto Reply = viewSummary(*V, State->Revision, Published);
  Reply["items"] = std::move(Items);
  Reply["offset"] = Offset;
  Reply["page_complete"] = End == V->Segments.size();
  Reply["next_offset"] = End == V->Segments.size() ? llvm::json::Value(nullptr)
                                                   : llvm::json::Value(End);
  return json(std::move(Reply));
#endif
}

std::string Session::sourceViewChunk(std::string_view Revision,
                                     std::string_view ViewID, uint64_t Offset,
                                     uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const SourceView *V = nullptr;
  for (const auto &[ID, View] : State->SourceViews)
    if (View.ID == ViewID)
      V = &View;
  if (!V)
    throw Error("source_view_not_committed");
  if (!Limit || Limit > MaxSourceViewChunk ||
      !sourceByteBoundary(V->Text, Offset))
    throw Error("invalid_source_view_chunk");
  auto End = std::min<uint64_t>(V->Text.size(), Offset + Limit);
  while (!sourceByteBoundary(V->Text, End))
    --End;
  if (End == Offset && Offset != V->Text.size())
    throw Error("source_view_chunk_too_small");
  const auto First =
      std::lower_bound(V->Segments.begin(), V->Segments.end(), Offset,
                       [](const auto &S, uint64_t At) { return S.End <= At; });
  const auto Last =
      std::lower_bound(First, V->Segments.end(), End,
                       [](const auto &S, uint64_t At) { return S.Start < At; });
  auto Reply = viewSummary(*V, State->Revision, true);
  Reply["byte_offset"] = std::to_string(Offset);
  Reply["byte_length"] = std::to_string(End - Offset);
  Reply["text"] = V->Text.substr(Offset, End - Offset);
  Reply["first_segment"] = uint64_t(First - V->Segments.begin());
  Reply["last_segment_exclusive"] = uint64_t(Last - V->Segments.begin());
  Reply["page_complete"] = End == V->Text.size();
  Reply["next_byte_offset"] = End == V->Text.size()
                                  ? llvm::json::Value(nullptr)
                                  : llvm::json::Value(std::to_string(End));
  return json(std::move(Reply));
#endif
}
} // namespace neverd::web
