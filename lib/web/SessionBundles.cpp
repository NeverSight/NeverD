#include "SessionInternal.h"

namespace neverd::web {
namespace {
llvm::json::Object bundleSummary(const SourceBundleAnalysis &A,
                                 uint64_t Revision) {
  llvm::json::Array Diagnostics;
  for (const auto &D : A.Diagnostics)
    Diagnostics.emplace_back(llvm::json::Object{
        {"code", D.Code}, {"byte_offset", std::to_string(D.ByteOffset)}});
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"source_id", A.SourceID},
      {"binding_analysis_id", A.BindingID},
      {"binding_status", A.BindingStatus},
      {"bundle_analysis_id", A.ID},
      {"bundle_profile", std::string(JavaScriptBundleProfile)},
      {"bundle_status", A.Status},
      {"bundle_count", A.Bundles.size()},
      {"module_count", A.Modules.size()},
      {"dependency_count", A.Dependencies.size()},
      {"region_count", A.Regions.size()},
      {"steps", A.Steps},
      {"diagnostics", std::move(Diagnostics)},
      {"diagnostic_count", A.DiagnosticCount},
      {"diagnostics_complete", A.DiagnosticCount == A.Diagnostics.size()},
      {"recovery", "original_source_partitions"},
      {"source_coverage", "selected_factories_and_bootstrap"},
      {"producer_verified", false},
      {"runtime_targets_verified", false},
      {"entry_module_reconstruction", "not_analyzed"},
      {"alias_analysis", "not_analyzed"},
      {"dependency_coverage", "lexical_role_calls_and_dynamic_name_candidates"},
      {"property_mutation_analysis", "not_analyzed"},
      {"reachability", "not_analyzed"},
      {"executes_input", false},
      {"authorizes_source_rewrites", false},
      {"semantic_validation_complete", false},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::analyzeSourceBundles(std::string_view ExpectedRevision,
                                          std::string_view SourceID) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  const auto Found = State->Sources.find(std::string(SourceID));
  if (Found == State->Sources.end())
    throw Error("unknown_source");
  if (const auto Cached = State->Bundles.find(Found->first);
      Cached != State->Bundles.end())
    return json(bundleSummary(Cached->second, State->Revision));
  auto B = State->Bindings.find(Found->first);
  if (B == State->Bindings.end())
    B = State->Bindings
            .emplace(Found->first, web::analyzeSourceBindings(Found->second))
            .first;
  auto A = web::analyzeSourceBundles(
      Found->second, B->second,
      State->sourceBytes(Found->second.ArtifactID, MaxJavaScriptBytes,
                         "source_byte_budget_exceeded"));
  auto Reply = json(bundleSummary(A, State->Revision));
  State->Bundles.emplace(Found->first, std::move(A));
  return Reply;
#endif
}

std::string Session::sourceBundleRecords(std::string_view ExpectedRevision,
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
  const auto Cached = State->Bundles.find(Found->first);
  if (Cached == State->Bundles.end())
    throw Error("source_bundles_not_analyzed");
  const auto &A = Cached->second;
  const auto &B = State->Bindings.at(Found->first);
  uint64_t Count;
  if (Kind == "bundles")
    Count = A.Bundles.size();
  else if (Kind == "modules")
    Count = A.Modules.size();
  else if (Kind == "dependencies")
    Count = A.Dependencies.size();
  else if (Kind == "regions")
    Count = A.Regions.size();
  else
    throw Error("invalid_bundle_record_kind");
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
  auto ModuleID = [&](uint32_t I) -> llvm::json::Value {
    return I == NoSourceIndex ? llvm::json::Value(nullptr)
                              : llvm::json::Value(A.Modules[I].ID);
  };
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    llvm::json::Object Item;
    uint32_t Node, Bundle;
    if (Kind == "bundles") {
      const auto &R = A.Bundles[I];
      Node = R.Node;
      Bundle = I;
      Item = llvm::json::Object{
          {"record_id", R.ID},
          {"table_kind", R.TableKind},
          {"table_node_id", NodeID(R.TableNode)},
          {"loader_node_id", NodeID(R.LoaderNode)},
          {"table_binding_id", BindingID(R.TableBinding)},
          {"loader_binding_id", BindingID(R.LoaderBinding)},
          {"table_binding_written", R.TableBindingWritten},
          {"loader_binding_written", R.LoaderBindingWritten}};
    } else if (Kind == "modules") {
      const auto &R = A.Modules[I];
      Node = R.Node;
      Bundle = R.Bundle;
      Item = llvm::json::Object{
          {"record_id", R.ID},
          {"kind", R.Kind},
          {"body_node_id", NodeID(R.Body)},
          {"wrapper_sha256", R.WrapperHash},
          {"body_sha256", R.BodyHash.empty() ? llvm::json::Value(nullptr)
                                             : llvm::json::Value(R.BodyHash)},
          {"require_binding_id", BindingID(R.RequireBinding)},
          {"require_binding_written", R.RequireBindingWritten},
          {"module_key_redacted", true}};
      if (R.Body != NoSourceIndex) {
        Item["body_start_byte"] =
            std::to_string(Found->second.Nodes[R.Body].Start);
        Item["body_end_byte"] = std::to_string(Found->second.Nodes[R.Body].End);
      } else {
        Item["body_start_byte"] = nullptr;
        Item["body_end_byte"] = nullptr;
      }
    } else if (Kind == "dependencies") {
      const auto &R = A.Dependencies[I];
      Node = R.Node;
      Bundle = R.Bundle;
      Item =
          llvm::json::Object{{"record_id", R.ID},
                             {"link_status", R.Status},
                             {"caller_module_id", ModuleID(R.CallerModule)},
                             {"target_module_id", ModuleID(R.TargetModule)},
                             {"callee_binding_id", BindingID(R.CalleeBinding)},
                             {"runtime_target_verified", false}};
    } else {
      const auto &R = A.Regions[I];
      Node = R.Node;
      Bundle = R.Bundle;
      Item = llvm::json::Object{{"record_id", R.ID}, {"kind", R.Kind}};
    }
    const auto &N = Found->second.Nodes[Node];
    Item["bundle_id"] = A.Bundles[Bundle].ID;
    Item["node_id"] = N.ID;
    Item["start_byte"] = std::to_string(N.Start);
    Item["end_byte"] = std::to_string(N.End);
    Items.emplace_back(std::move(Item));
  }
  auto Reply = bundleSummary(A, State->Revision);
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
