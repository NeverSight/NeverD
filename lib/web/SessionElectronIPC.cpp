#include "ElectronSelection.h"
#include "SessionInternal.h"

namespace neverd::web {
namespace {
llvm::json::Object ipcSummary(const ElectronIPC &A, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"electron_ipc_id", A.ID},
      {"namespace_id", A.NamespaceID},
      {"manifest_id", A.ManifestID},
      {"main_artifact_id", A.MainArtifactID},
      {"profile", std::string(ElectronIPCProfile)},
      {"analysis_status", "partial"},
      {"scope_evidence", "caller_selected_manifest_and_sources"},
      {"selection_complete", false},
      {"source_count", A.Sources.size()},
      {"unavailable_source_count", A.UnavailableSources},
      {"endpoint_count", A.Endpoints.size()},
      {"channel_count", A.Channels.size()},
      {"unresolved_endpoint_count", A.UnresolvedEndpoints},
      {"channel_comparison", "exact_utf16"},
      {"steps", A.Steps},
      {"runtime_routing_verified", false},
      {"process_roles_verified", false},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::analyzeElectronIPC(std::string_view Revision,
                                        std::string_view ArtifactID,
                                        std::string_view Options) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto IDs = electronSourceSelection(Options);
  const auto Manifest = State->ElectronManifests.find(std::string(ArtifactID));
  if (Manifest == State->ElectronManifests.end())
    throw Error("electron_manifest_not_analyzed");
  std::vector<ElectronIPCInput> Sources;
  for (const auto &ID : IDs) {
    const auto Source = State->Sources.find(ID);
    if (Source == State->Sources.end())
      throw Error("unknown_source");
    const auto Evidence = State->ElectronSources.find(Source->first);
    if (Evidence == State->ElectronSources.end())
      throw Error("electron_source_not_analyzed");
    Sources.push_back({&Source->second, &Evidence->second});
  }
  const auto Namespace = State->memberNamespace(ArtifactID);
  auto A = correlateElectronIPC(Namespace ? *Namespace : State->Published,
                                Manifest->second, Sources);
  if (!State->ElectronIPCs.count(A.ID) && State->ElectronIPCs.size() >= 4)
    throw Error("electron_ipc_cache_budget_exceeded");
  auto Reply = json(ipcSummary(A, State->Revision));
  State->ElectronIPCs.emplace(A.ID, std::move(A));
  return Reply;
#endif
}

std::string Session::electronIPCRecords(std::string_view Revision,
                                        std::string_view AnalysisID,
                                        std::string_view Kind, uint64_t Offset,
                                        uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->ElectronIPCs.find(std::string(AnalysisID));
  if (Found == State->ElectronIPCs.end())
    throw Error("unknown_electron_ipc_analysis");
  const auto &A = Found->second;
  const auto Size = Kind == "sources"     ? A.Sources.size()
                    : Kind == "channels"  ? A.Channels.size()
                    : Kind == "endpoints" ? A.Endpoints.size()
                                          : throw Error("invalid_record_kind");
  if (!Limit || Limit > 512 || Offset > Size)
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Size, Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    if (Kind == "sources") {
      const auto &S = A.Sources[I];
      Items.emplace_back(llvm::json::Object{
          {"source_id", S.SourceID},
          {"artifact_id", S.ArtifactID},
          {"electron_source_id", S.EvidenceID},
          {"analysis_status", S.Status},
          {"reason", S.Reason},
          {"main_entry_candidate", S.ArtifactID == A.MainArtifactID},
          {"process_role", "not_verified"}});
    } else if (Kind == "channels") {
      const auto &C = A.Channels[I];
      llvm::json::Object Counts;
      for (size_t K = 0; K < ElectronIPCKinds.size(); ++K)
        Counts[llvm::StringRef(ElectronIPCKinds[K])] = C.Counts[K];
      Items.emplace_back(llvm::json::Object{
          {"channel_id", C.ID},
          {"endpoint_counts", std::move(Counts)},
          {"invoke_handle_candidate_pairs", C.Counts[0] * C.Counts[1]},
          {"renderer_send_main_listen_candidate_pairs",
           C.Counts[2] * C.Counts[3]},
          {"webcontents_send_renderer_listen_candidate_pairs",
           C.Counts[4] * C.Counts[5]},
          {"runtime_routing_verified", false},
          {"handler_removal_order", "not_analyzed"},
          {"value_redacted", true}});
    } else {
      const auto &E = A.Endpoints[I];
      const auto &Selected = A.Sources.at(E.Source);
      const auto &Source = State->Sources.at(Selected.SourceID);
      const auto &Evidence = State->ElectronSources.at(Selected.SourceID);
      const auto &B = Evidence.Boundaries.at(E.Boundary);
      const auto &N = Source.Nodes.at(B.Node);
      Items.emplace_back(llvm::json::Object{
          {"endpoint_id", E.ID},
          {"channel_id", E.Channel == NoSourceIndex
                             ? llvm::json::Value(nullptr)
                             : llvm::json::Value(A.Channels.at(E.Channel).ID)},
          {"kind", std::string(ElectronIPCKinds.at(E.Kind))},
          {"source_id", Selected.SourceID},
          {"artifact_id", Selected.ArtifactID},
          {"electron_source_id", Selected.EvidenceID},
          {"boundary_id", B.ID},
          {"node_id", N.ID},
          {"byte_offset", std::to_string(N.Start)},
          {"byte_length", std::to_string(N.End - N.Start)},
          {"value_status", B.ValueStatus},
          {"runtime_target_verified", false}});
    }
  }
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(State->Revision)},
      {"electron_ipc_id", A.ID},
      {"record_kind", std::string(Kind)},
      {"items", std::move(Items)},
      {"offset", Offset},
      {"page_complete", End == Size},
      {"next_offset",
       End == Size ? llvm::json::Value(nullptr) : llvm::json::Value(End)},
      {"redaction_policy", "metadata-only-v1"}});
#endif
}
} // namespace neverd::web
