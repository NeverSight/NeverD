#include "SessionInternal.h"

namespace neverd::web {
namespace {
llvm::json::Object bindingSummary(const SourceBindingAnalysis &Analysis,
                                  uint64_t Revision) {
  llvm::json::Array Diagnostics;
  for (const auto &D : Analysis.Diagnostics)
    Diagnostics.emplace_back(llvm::json::Object{
        {"code", D.Code},
        {"byte_offset",
         D.ByteOffset < 0 ? llvm::json::Value(nullptr)
                          : llvm::json::Value(std::to_string(D.ByteOffset))}});
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"source_id", Analysis.SourceID},
      {"binding_analysis_id", Analysis.ID},
      {"binding_profile", std::string(JavaScriptBindingProfile)},
      {"binding_status", Analysis.Status},
      {"scope_count", Analysis.Scopes.size()},
      {"binding_count", Analysis.Bindings.size()},
      {"declaration_count", Analysis.Declarations.size()},
      {"reference_count", Analysis.References.size()},
      {"steps", Analysis.Steps},
      {"diagnostics", std::move(Diagnostics)},
      {"diagnostic_count", Analysis.DiagnosticCount},
      {"diagnostics_complete",
       Analysis.DiagnosticCount == Analysis.Diagnostics.size()},
      {"runtime_values", "not_analyzed"},
      {"initialization", "not_analyzed"},
      {"semantic_validation_complete", false},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::analyzeSourceBindings(std::string_view ExpectedRevision,
                                           std::string_view SourceID) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  const auto Found = State->Sources.find(std::string(SourceID));
  if (Found == State->Sources.end())
    throw Error("unknown_source");
  const auto Existing = State->Bindings.find(Found->first);
  if (Existing != State->Bindings.end())
    return json(bindingSummary(Existing->second, State->Revision));
  // One bounded analysis per cached source. Source/node cache admission also
  // bounds the total retained bindings, scopes, declarations and references.
  auto Analysis = web::analyzeSourceBindings(Found->second);
  auto Reply = json(bindingSummary(Analysis, State->Revision));
  State->Bindings.emplace(Found->first, std::move(Analysis));
  return Reply;
#endif
}

std::string Session::sourceBindingRecords(std::string_view ExpectedRevision,
                                          std::string_view SourceID,
                                          std::string_view RecordKind,
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
  const auto Existing = State->Bindings.find(Found->first);
  if (Existing == State->Bindings.end())
    throw Error("source_bindings_not_analyzed");
  const auto &Source = Found->second;
  const auto &A = Existing->second;
  const auto NodeID = [&](uint32_t Index) -> llvm::json::Value {
    return Index == NoSourceIndex ? llvm::json::Value(nullptr)
                                  : llvm::json::Value(Source.Nodes[Index].ID);
  };
  const auto ScopeID = [&](uint32_t Index) -> llvm::json::Value {
    return Index == NoSourceIndex ? llvm::json::Value(nullptr)
                                  : llvm::json::Value(A.Scopes[Index].ID);
  };
  uint64_t Count = 0;
  if (RecordKind == "scopes")
    Count = A.Scopes.size();
  else if (RecordKind == "bindings")
    Count = A.Bindings.size();
  else if (RecordKind == "declarations")
    Count = A.Declarations.size();
  else if (RecordKind == "references")
    Count = A.References.size();
  else
    throw Error("unsupported_binding_record_kind");
  if (!Limit || Limit > 512 || Offset > Count)
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Count, Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    if (RecordKind == "scopes") {
      const auto &S = A.Scopes[I];
      Items.emplace_back(llvm::json::Object{
          {"scope_id", S.ID},
          {"parent_scope_id", ScopeID(S.Parent)},
          {"node_id", NodeID(S.Node)},
          {"kind", S.Kind},
          {"strict", S.Strict},
          {"variable_scope_id", ScopeID(S.VariableScope)},
          {"per_iteration", S.PerIteration},
          {"object_environment", S.ObjectEnvironment},
          {"possible_direct_eval", S.PossibleDirectEval},
          {"possible_eval_declarations", S.PossibleEvalDeclarations}});
    } else if (RecordKind == "bindings") {
      const auto &B = A.Bindings[I];
      Items.emplace_back(
          llvm::json::Object{{"binding_id", B.ID},
                             {"scope_id", ScopeID(B.Scope)},
                             {"kind", B.Kind},
                             {"conflicting", B.Conflicting},
                             {"immutable_binding", B.Immutable},
                             {"has_temporal_dead_zone", B.HasTemporalDeadZone},
                             {"implicit", B.Implicit},
                             {"name_redacted", true}});
    } else if (RecordKind == "declarations") {
      const auto &D = A.Declarations[I];
      Items.emplace_back(llvm::json::Object{
          {"binding_id", A.Bindings[D.Binding].ID},
          {"node_id", NodeID(D.Node)},
          {"occurrence_scope_id", ScopeID(D.OccurrenceScope)},
          {"kind", D.Kind}});
    } else {
      const auto &R = A.References[I];
      Items.emplace_back(llvm::json::Object{
          {"reference_id", R.ID},
          {"node_id", NodeID(R.Node)},
          {"scope_id", ScopeID(R.Scope)},
          {"access", R.Access},
          {"resolution", R.Resolution},
          {"binding_id", R.Binding == NoSourceIndex
                             ? llvm::json::Value(nullptr)
                             : llvm::json::Value(A.Bindings[R.Binding].ID)},
          {"name_redacted", true}});
    }
  }
  auto Reply = bindingSummary(A, State->Revision);
  Reply["record_kind"] = std::string(RecordKind);
  Reply["items"] = std::move(Items);
  Reply["offset"] = Offset;
  Reply["page_complete"] = End == Count;
  Reply["next_offset"] =
      End == Count ? llvm::json::Value(nullptr) : llvm::json::Value(End);
  return json(std::move(Reply));
#endif
}
} // namespace neverd::web
