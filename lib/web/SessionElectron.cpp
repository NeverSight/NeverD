//===- SessionElectron.cpp - Electron analysis publication -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Electron analysis publication.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"

namespace neverd::web {
namespace {
llvm::json::Object sourceSummary(const ElectronSource &A, uint64_t Revision) {
  return llvm::json::Object{{"schema_version", 1},
                            {"status", "ok"},
                            {"revision", std::to_string(Revision)},
                            {"source_id", A.SourceID},
                            {"electron_source_id", A.ID},
                            {"profile", std::string(ElectronSourceProfile)},
                            {"analysis_status", A.Status},
                            {"reason", A.Reason},
                            {"binding_analysis_id", A.BindingID},
                            {"module_analysis_id", A.ModuleID},
                            {"origin_analysis_id", A.OriginID},
                            {"value_analysis_id", A.ValueID},
                            {"origin_status", A.OriginStatus},
                            {"origin_reason", A.OriginReason},
                            {"value_status", A.ValueStatus},
                            {"boundary_count", A.Boundaries.size()},
                            {"steps", A.Steps},
                            {"framework_verified", false},
                            {"runtime_targets_verified", false},
                            {"permission_analysis", "not_analyzed"},
                            {"channel_correlation", "not_analyzed"},
                            {"coverage", "explicit_module_origin_syntax"},
                            {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::analyzeElectronManifest(std::string_view Revision,
                                             std::string_view ArtifactID) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto View = State->artifactView(ArtifactID);
  if (!View)
    throw Error("unknown_artifact");
  if (View->Content.size() > MaxElectronManifestBytes)
    throw Error("electron_manifest_budget_exceeded");
  auto Existing = State->ElectronManifests.find(std::string(ArtifactID));
  std::optional<ElectronManifest> Pending;
  if (Existing == State->ElectronManifests.end()) {
    if (State->ElectronManifests.size() >= 16)
      throw Error("electron_manifest_cache_budget_exceeded");
    const auto Namespace = State->memberNamespace(ArtifactID);
    Pending = web::analyzeElectronManifest(
        Namespace ? *Namespace : State->Published, ArtifactID,
        View->Content.read(0, View->Content.size(), MaxElectronManifestBytes));
  }
  const auto &A = Pending ? *Pending : Existing->second;
  auto Reply = json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(State->Revision)},
      {"manifest_id", A.ID},
      {"artifact_id", A.ArtifactID},
      {"blob_sha256", View->BlobHash},
      {"origin", llvm::json::Object(View->Origin)},
      {"profile", std::string(ElectronManifestProfile)},
      {"framework_evidence", "caller_selected_profile"},
      {"framework_verified", false},
      {"runtime_version_verified", false},
      {"name_present", A.NamePresent},
      {"version_present", A.VersionPresent},
      {"product_name_present", A.ProductNamePresent},
      {"electron_dependency_present", A.ElectronDependencyPresent},
      {"main_value_status", A.MainValueStatus},
      {"main_link_status", A.MainLinkStatus},
      {"main_artifact_id", A.MainArtifactID.empty()
                               ? llvm::json::Value(nullptr)
                               : llvm::json::Value(A.MainArtifactID)},
      {"entry_source_type_candidate", A.SourceType},
      {"runtime_entry_verified", false},
      {"declarations_redacted", true},
      {"redaction_policy", "metadata-only-v1"}});
  if (Pending)
    State->ElectronManifests.emplace(std::string(ArtifactID),
                                     std::move(*Pending));
  return Reply;
}

std::string Session::analyzeElectronSource(std::string_view Revision,
                                           std::string_view SourceID) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Source = State->Sources.find(std::string(SourceID));
  if (Source == State->Sources.end())
    throw Error("unknown_source");
  const auto Cached = State->ElectronSources.find(Source->first);
  if (Cached != State->ElectronSources.end())
    return json(sourceSummary(Cached->second, State->Revision));
  auto Binding = State->Bindings.find(Source->first);
  if (Binding == State->Bindings.end())
    Binding =
        State->Bindings
            .emplace(Source->first, web::analyzeSourceBindings(Source->second))
            .first;
  const auto Modules =
      web::analyzeSourceModules(Source->second, Binding->second);
  const auto Origins =
      web::analyzeSourceOrigins(Source->second, Binding->second, Modules);
  const auto Values = web::analyzeSourceValues(Source->second);
  auto A = web::analyzeElectronSource(Source->second, Modules, Origins, Values);
  auto Reply = json(sourceSummary(A, State->Revision));
  State->ElectronSources.emplace(Source->first, std::move(A));
  return Reply;
#endif
}

std::string Session::electronSourceRecords(std::string_view Revision,
                                           std::string_view SourceID,
                                           uint64_t Offset,
                                           uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Source = State->Sources.find(std::string(SourceID));
  if (Source == State->Sources.end())
    throw Error("unknown_source");
  const auto Found = State->ElectronSources.find(Source->first);
  if (Found == State->ElectronSources.end())
    throw Error("electron_source_not_analyzed");
  const auto &A = Found->second;
  if (!Limit || Limit > 512 || Offset > A.Boundaries.size())
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(A.Boundaries.size(), Offset + Limit);
  auto Node = [&](uint32_t I) -> llvm::json::Value {
    return I == NoSourceIndex
               ? llvm::json::Value(nullptr)
               : llvm::json::Value(Source->second.Nodes.at(I).ID);
  };
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &B = A.Boundaries[I];
    const auto &N = Source->second.Nodes[B.Node];
    Items.emplace_back(
        llvm::json::Object{{"boundary_id", B.ID},
                           {"kind", B.Kind},
                           {"node_id", N.ID},
                           {"byte_offset", std::to_string(N.Start)},
                           {"byte_length", std::to_string(N.End - N.Start)},
                           {"module_request_index", B.Request},
                           {"module_analysis_id", A.ModuleID},
                           {"origin_evidence", B.OriginEvidence},
                           {"construction_node_id", Node(B.ConstructionNode)},
                           {"value_node_id", Node(B.ValueNode)},
                           {"callback_or_api_node_id", Node(B.CallbackNode)},
                           {"value_status", B.ValueStatus},
                           {"optional", B.Optional},
                           {"runtime_target_verified", false},
                           {"reachability", "not_analyzed"},
                           {"value_redacted", true}});
  }
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(State->Revision)},
      {"source_id", A.SourceID},
      {"electron_source_id", A.ID},
      {"items", std::move(Items)},
      {"offset", Offset},
      {"page_complete", End == A.Boundaries.size()},
      {"next_offset", End == A.Boundaries.size() ? llvm::json::Value(nullptr)
                                                 : llvm::json::Value(End)},
      {"redaction_policy", "metadata-only-v1"}});
#endif
}
} // namespace neverd::web
