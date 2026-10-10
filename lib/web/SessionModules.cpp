#include "SessionInternal.h"

namespace neverd::web {
namespace {
llvm::json::Object moduleSummary(const SourceModuleAnalysis &M,
                                 const SourceModuleLinks &L,
                                 const SourceBindingAnalysis &B,
                                 uint64_t Revision) {
  llvm::json::Array Diagnostics;
  for (const auto &D : M.Diagnostics)
    Diagnostics.emplace_back(llvm::json::Object{
        {"code", D.Code}, {"byte_offset", std::to_string(D.ByteOffset)}});
  llvm::json::Value Context = nullptr;
  if (L.Context) {
    const auto &C = *L.Context;
    Context = llvm::json::Object{
        {"context_id", C.ID},
        {"kind", C.Kind},
        {"html_id", C.HTMLID},
        {"html_script_id", C.ScriptID},
        {"preceding_base_id", C.BaseID.empty() ? llvm::json::Value(nullptr)
                                               : llvm::json::Value(C.BaseID)},
        {"base_status", C.Status},
        {"import_map_status", C.ImportMapStatus},
        {"import_map_analysis_id", C.ImportMapAnalysisID},
        {"runtime_base_verified", false},
        {"module_url_identity", C.ModuleURLIdentity}};
  }
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"source_id", M.SourceID},
      {"module_analysis_id", M.ID},
      {"binding_analysis_id", M.BindingID},
      {"link_analysis_id", L.ID},
      {"module_profile", std::string(JavaScriptModuleProfile)},
      {"link_profile", L.Profile},
      {"link_context", std::move(Context)},
      {"module_status", M.Status},
      {"binding_status", B.Status},
      {"link_status", L.Status},
      {"request_count", M.Requests.size()},
      {"import_count", M.Imports.size()},
      {"export_count", M.Exports.size()},
      {"attribute_count", M.Attributes.size()},
      {"steps", M.Steps},
      {"link_steps", L.Steps},
      {"diagnostics", std::move(Diagnostics)},
      {"diagnostic_count", M.DiagnosticCount},
      {"diagnostics_complete", M.DiagnosticCount == M.Diagnostics.size()},
      {"reachability", "not_analyzed"},
      {"commonjs_export_inference", "not_analyzed"},
      {"require_aliases", "not_analyzed"},
      {"foreign_export_linking", "not_analyzed"},
      {"resolves_runtime_modules", false},
      {"executes_input", false},
      {"semantic_validation_complete", false},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::analyzeSourceModules(std::string_view ExpectedRevision,
                                          std::string_view SourceID) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  const auto Found = State->Sources.find(std::string(SourceID));
  if (Found == State->Sources.end())
    throw Error("unknown_source");
  auto Binding = State->Bindings.find(Found->first);
  if (Binding == State->Bindings.end())
    Binding =
        State->Bindings
            .emplace(Found->first, web::analyzeSourceBindings(Found->second))
            .first;
  if (const auto Cached = State->Modules.find(Found->first);
      Cached != State->Modules.end())
    return json(moduleSummary(Cached->second.Evidence, Cached->second.Links,
                              Binding->second, State->Revision));
  Impl::ModuleResults Analysis;
  Analysis.Evidence = web::analyzeSourceModules(Found->second, Binding->second);
  if (const auto Inline = State->htmlSource(Found->second.ArtifactID)) {
    const auto &H = *Inline->Analysis;
    const auto Members = State->memberNamespace(H.Document.ArtifactID);
    Analysis.Links = linkHTMLSourceModules(
        Found->second, Analysis.Evidence, H.Document, H.Links, Inline->Script,
        Members ? *Members : State->Published, H.ImportMaps);
  } else {
    const auto Members = State->memberNamespace(Found->second.ArtifactID);
    Analysis.Links = linkSourceModules(Found->second, Analysis.Evidence,
                                       Members ? *Members : State->Published);
  }
  auto Reply = json(moduleSummary(Analysis.Evidence, Analysis.Links,
                                  Binding->second, State->Revision));
  State->Modules.emplace(Found->first, std::move(Analysis));
  return Reply;
#endif
}

std::string Session::sourceModuleRecords(std::string_view ExpectedRevision,
                                         std::string_view SourceID,
                                         std::string_view Kind, uint64_t Offset,
                                         uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  const auto Found = State->Sources.find(std::string(SourceID));
  if (Found == State->Sources.end())
    throw Error("unknown_source");
  const auto Cached = State->Modules.find(Found->first);
  if (Cached == State->Modules.end())
    throw Error("source_modules_not_analyzed");
  const auto &M = Cached->second.Evidence;
  const auto &L = Cached->second.Links;
  const auto &B = State->Bindings.at(Found->first);
  uint64_t Count;
  if (Kind == "requests")
    Count = M.Requests.size();
  else if (Kind == "imports")
    Count = M.Imports.size();
  else if (Kind == "exports")
    Count = M.Exports.size();
  else if (Kind == "attributes")
    Count = M.Attributes.size();
  else
    throw Error("invalid_module_record_kind");
  if (!Limit || Limit > 512 || Offset > Count)
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Count, Offset + Limit);
  auto NodeID = [&](uint32_t I) -> llvm::json::Value {
    return I == NoSourceIndex ? llvm::json::Value(nullptr)
                              : llvm::json::Value(Found->second.Nodes[I].ID);
  };
  auto BindingID = [&](uint32_t I) -> llvm::json::Value {
    return I == NoSourceIndex ? llvm::json::Value(nullptr)
                              : llvm::json::Value(B.Bindings[I].ID);
  };
  auto RequestID = [&](uint32_t I) -> llvm::json::Value {
    return I == NoSourceIndex ? llvm::json::Value(nullptr)
                              : llvm::json::Value(M.Requests[I].ID);
  };
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    llvm::json::Object Item;
    uint32_t Node;
    if (Kind == "requests") {
      const auto &R = M.Requests[I];
      Node = R.Node;
      Item = llvm::json::Object{
          {"record_id", R.ID},
          {"kind", R.Kind},
          {"specifier_node_id", NodeID(R.SpecifierNode)},
          {"specifier_status", R.SpecifierStatus},
          {"attributes_node_id", NodeID(R.AttributesNode)},
          {"callee_node_id", NodeID(R.CalleeNode)},
          {"callee_binding_id", BindingID(R.CalleeBinding)},
          {"callee_evidence", R.CalleeEvidence},
          {"argument_count", R.ArgumentCount},
          {"optional", R.Optional},
          {"runtime_target_verified", false},
          {"module_url_candidate_id",
           I < L.Requests.size() && !L.Requests[I].URLCandidateID.empty()
               ? llvm::json::Value(L.Requests[I].URLCandidateID)
               : llvm::json::Value(nullptr)},
          {"import_map_id",
           I < L.Requests.size() && !L.Requests[I].ImportMapID.empty()
               ? llvm::json::Value(L.Requests[I].ImportMapID)
               : llvm::json::Value(nullptr)},
          {"import_map_entry_id",
           I < L.Requests.size() && !L.Requests[I].ImportMapEntryID.empty()
               ? llvm::json::Value(L.Requests[I].ImportMapEntryID)
               : llvm::json::Value(nullptr)},
          {"import_map_scope_id",
           I < L.Requests.size() && !L.Requests[I].ImportMapScopeID.empty()
               ? llvm::json::Value(L.Requests[I].ImportMapScopeID)
               : llvm::json::Value(nullptr)},
          {"import_map_match",
           I < L.Requests.size() && !L.Requests[I].ImportMapMatch.empty()
               ? llvm::json::Value(L.Requests[I].ImportMapMatch)
               : llvm::json::Value(nullptr)},
          {"link_status",
           I < L.Requests.size() ? L.Requests[I].Status : "unavailable"},
          {"query_present",
           I < L.Requests.size() && L.Requests[I].Query.has_value()
               ? llvm::json::Value(*L.Requests[I].Query)
               : llvm::json::Value(nullptr)},
          {"fragment_present",
           I < L.Requests.size() && L.Requests[I].Fragment.has_value()
               ? llvm::json::Value(*L.Requests[I].Fragment)
               : llvm::json::Value(nullptr)},
          {"candidate_artifact_id",
           I < L.Requests.size() && !L.Requests[I].ArtifactID.empty()
               ? llvm::json::Value(L.Requests[I].ArtifactID)
               : llvm::json::Value(nullptr)}};
    } else if (Kind == "imports") {
      const auto &R = M.Imports[I];
      Node = R.Node;
      Item = llvm::json::Object{{"record_id", R.ID},
                                {"kind", R.Kind},
                                {"request_id", RequestID(R.Request)},
                                {"binding_id", BindingID(R.Binding)}};
    } else if (Kind == "exports") {
      const auto &R = M.Exports[I];
      Node = R.Node;
      Item = llvm::json::Object{{"record_id", R.ID},
                                {"kind", R.Kind},
                                {"request_id", RequestID(R.Request)},
                                {"binding_id", BindingID(R.Binding)},
                                {"link_status", R.LinkStatus},
                                {"conflicting", R.Conflicting}};
    } else {
      const auto &R = M.Attributes[I];
      Node = R.Node;
      Item = llvm::json::Object{{"record_id", R.ID},
                                {"kind", "import_assertion"},
                                {"request_id", RequestID(R.Request)},
                                {"conflicting", R.Conflicting}};
    }
    const auto &N = Found->second.Nodes[Node];
    Item["node_id"] = N.ID;
    Item["start_byte"] = std::to_string(N.Start);
    Item["end_byte"] = std::to_string(N.End);
    Item["names_redacted"] = true;
    Items.emplace_back(std::move(Item));
  }
  auto Reply = moduleSummary(M, L, B, State->Revision);
  Reply["record_kind"] = std::string(Kind);
  Reply["items"] = std::move(Items);
  Reply["offset"] = Offset;
  Reply["page_complete"] = End == Count;
  Reply["next_offset"] =
      End == Count ? llvm::json::Value(nullptr) : llvm::json::Value(End);
  return json(std::move(Reply));
#endif
}
} // namespace neverd::web
