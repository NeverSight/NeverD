#include "SessionInternal.h"

namespace neverd::web {
namespace {
llvm::json::Array diagnostics(const std::vector<SourceDiagnostic> &Entries) {
  llvm::json::Array Out;
  for (const auto &D : Entries)
    Out.emplace_back(llvm::json::Object{
        {"code", D.Code},
        {"byte_offset",
         D.ByteOffset < 0 ? llvm::json::Value(nullptr)
                          : llvm::json::Value(std::to_string(D.ByteOffset))}});
  return Out;
}
llvm::json::Object semanticSummary(const SourceValueAnalysis &V,
                                   const SourceEffectAnalysis &E,
                                   uint64_t Revision) {
  std::map<std::string, uint64_t> Counts;
  for (const auto &N : V.Nodes)
    ++Counts[N.Status];
  llvm::json::Object ValueCounts;
  for (const auto &[Status, Count] : Counts)
    ValueCounts[Status] = Count;
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"source_id", V.SourceID},
      {"value_analysis_id", V.ID},
      {"effect_analysis_id", E.ID},
      {"value_profile", std::string(JavaScriptValueProfile)},
      {"effect_profile", std::string(JavaScriptEffectProfile)},
      {"value_status", V.Status},
      {"effect_status", E.Status},
      {"value_node_count", V.Nodes.size()},
      {"effect_node_count", E.Nodes.size()},
      {"value_counts", std::move(ValueCounts)},
      {"value_steps", V.Steps},
      {"effect_steps", E.Steps},
      {"allocated_string_units", V.AllocatedStringUnits},
      {"value_diagnostics", diagnostics(V.Diagnostics)},
      {"effect_diagnostics", diagnostics(E.Diagnostics)},
      {"effect_interpretation", "conservative_may_effects"},
      {"initialization", "not_analyzed"},
      {"executes_input", false},
      {"authorizes_source_rewrites", false},
      {"semantic_validation_complete", false},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::analyzeSourceSemantics(std::string_view ExpectedRevision,
                                            std::string_view SourceID) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  const auto Found = State->Sources.find(std::string(SourceID));
  if (Found == State->Sources.end())
    throw Error("unknown_source");
  if (const auto Cached = State->Semantics.find(Found->first);
      Cached != State->Semantics.end())
    return json(semanticSummary(Cached->second.Values, Cached->second.Effects,
                                State->Revision));
  auto Binding = State->Bindings.find(Found->first);
  if (Binding == State->Bindings.end())
    Binding =
        State->Bindings
            .emplace(Found->first, web::analyzeSourceBindings(Found->second))
            .first;
  Impl::SemanticResults Analysis;
  Analysis.Values = web::analyzeSourceValues(Found->second);
  Analysis.Effects = web::analyzeSourceEffects(Found->second, Binding->second,
                                               Analysis.Values);
  auto Reply =
      json(semanticSummary(Analysis.Values, Analysis.Effects, State->Revision));
  State->Semantics.emplace(Found->first, std::move(Analysis));
  return Reply;
#endif
}

std::string Session::sourceSemanticRecords(std::string_view ExpectedRevision,
                                           std::string_view SourceID,
                                           uint64_t Offset,
                                           uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  const auto Found = State->Sources.find(std::string(SourceID));
  if (Found == State->Sources.end())
    throw Error("unknown_source");
  const auto Cached = State->Semantics.find(Found->first);
  if (Cached == State->Semantics.end())
    throw Error("source_semantics_not_analyzed");
  const auto &V = Cached->second.Values;
  const auto &E = Cached->second.Effects;
  const auto Count = std::max(V.Nodes.size(), E.Nodes.size());
  if (!Limit || Limit > 512 || Offset > Count)
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Count, Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &N = Found->second.Nodes[I];
    llvm::json::Object Item{
        {"node_id", N.ID},
        {"kind", N.Kind},
        {"start_byte", std::to_string(N.Start)},
        {"end_byte", std::to_string(N.End)},
        {"value_status",
         I < V.Nodes.size() ? V.Nodes[I].Status : "unavailable"},
        {"value_reason",
         I < V.Nodes.size() ? V.Nodes[I].Reason : "analysis_unavailable"},
        {"value_kind",
         I < V.Nodes.size() && V.Nodes[I].Value
             ? llvm::json::Value(
                   std::string(sourcePrimitiveKindName(V.Nodes[I].Value->Kind)))
             : llvm::json::Value(nullptr)},
        {"value_redacted", true}};
    if (I < E.Nodes.size()) {
      llvm::json::Array Immediate, Deferred;
      for (const auto &Name : sourceEffectNames(E.Nodes[I].Immediate))
        Immediate.emplace_back(Name);
      for (const auto &Name : sourceEffectNames(E.Nodes[I].Deferred))
        Deferred.emplace_back(Name);
      Item["immediate_effects"] = std::move(Immediate);
      Item["deferred_effects"] = std::move(Deferred);
      Item["contains_declaration"] = E.Nodes[I].ContainsDeclaration;
    } else {
      Item["immediate_effects"] = nullptr;
      Item["deferred_effects"] = nullptr;
      Item["contains_declaration"] = nullptr;
    }
    Items.emplace_back(std::move(Item));
  }
  auto Reply = semanticSummary(V, E, State->Revision);
  Reply["items"] = std::move(Items);
  Reply["offset"] = Offset;
  Reply["page_complete"] = End == Count;
  Reply["next_offset"] =
      End == Count ? llvm::json::Value(nullptr) : llvm::json::Value(End);
  return json(std::move(Reply));
#endif
}
} // namespace neverd::web
