//===- SessionElectronEntries.cpp - Electron entry publication ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Electron entry publication.
///
//===----------------------------------------------------------------------===//

#include "ElectronSelection.h"
#include "SessionInternal.h"

namespace neverd::web {
namespace {
llvm::json::Object entrySummary(const ElectronEntries &A, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"electron_entries_id", A.ID},
      {"namespace_id", A.NamespaceID},
      {"manifest_id", A.ManifestID},
      {"main_artifact_id", A.MainArtifactID},
      {"profile", std::string(ElectronEntryProfile)},
      {"path_profile", std::string(CapturedPathProfile)},
      {"analysis_status", "partial"},
      {"scope_evidence", "caller_selected_manifest_and_sources"},
      {"selection_complete", false},
      {"source_count", A.Sources.size()},
      {"entry_count", A.Entries.size()},
      {"linked_file_candidate_count", A.LinkedFiles},
      {"unavailable_path_source_count", A.UnavailablePaths},
      {"steps", A.Steps},
      {"runtime_entries_verified", false},
      {"runtime_path_bases_verified", false},
      {"html_analysis", "not_analyzed"},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::analyzeElectronEntries(std::string_view Revision,
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
  struct Scratch {
    SourceModuleAnalysis Modules;
    SourceOrigins Origins;
    SourceValueAnalysis Values;
  };
  std::vector<Scratch> Models;
  Models.reserve(IDs.size());
  std::vector<ElectronEntryInput> Inputs;
  Inputs.reserve(IDs.size());
  for (const auto &ID : IDs) {
    const auto Source = State->Sources.find(ID);
    if (Source == State->Sources.end())
      throw Error("unknown_source");
    const auto Evidence = State->ElectronSources.find(ID);
    if (Evidence == State->ElectronSources.end())
      throw Error("electron_source_not_analyzed");
    const auto Bindings = State->Bindings.find(ID);
    if (Bindings == State->Bindings.end())
      throw Error("source_bindings_not_analyzed");
    const auto &S = Source->second;
    const auto &B = Bindings->second;
    auto &M = Models.emplace_back();
    M.Modules = web::analyzeSourceModules(S, B);
    M.Origins = web::analyzeSourceOrigins(S, B, M.Modules);
    M.Values = web::analyzeSourceValues(S);
    Inputs.push_back(
        {&S, &Evidence->second, &B, &M.Modules, &M.Origins, &M.Values});
  }
  const auto Namespace = State->memberNamespace(ArtifactID);
  auto A = associateElectronEntries(Namespace ? *Namespace : State->Published,
                                    Manifest->second, Inputs);
  if (!State->ElectronEntryAnalyses.count(A.ID) &&
      State->ElectronEntryAnalyses.size() >= 4)
    throw Error("electron_entries_cache_budget_exceeded");
  auto Reply = json(entrySummary(A, State->Revision));
  State->ElectronEntryAnalyses.emplace(A.ID, std::move(A));
  return Reply;
#endif
}

std::string Session::electronEntryRecords(std::string_view Revision,
                                          std::string_view AnalysisID,
                                          std::string_view Kind,
                                          uint64_t Offset,
                                          uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->ElectronEntryAnalyses.find(std::string(AnalysisID));
  if (Found == State->ElectronEntryAnalyses.end())
    throw Error("unknown_electron_entries_analysis");
  const auto &A = Found->second;
  const auto Size = Kind == "sources"   ? A.Sources.size()
                    : Kind == "entries" ? A.Entries.size()
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
          {"path_analysis_id", S.PathID},
          {"path_analysis_status", S.PathStatus},
          {"path_reason", S.PathReason},
          {"main_entry_candidate", S.ArtifactID == A.MainArtifactID},
          {"process_role", "not_verified"}});
      continue;
    }
    const auto &E = A.Entries[I];
    const auto &Selected = A.Sources.at(E.Source);
    const auto &Source = State->Sources.at(Selected.SourceID);
    const auto &Evidence = State->ElectronSources.at(Selected.SourceID);
    const auto &B = Evidence.Boundaries.at(E.Boundary);
    const auto &N = Source.Nodes.at(B.Node);
    const auto NodeID = [&](uint32_t Index) -> llvm::json::Value {
      return Index == NoSourceIndex
                 ? llvm::json::Value(nullptr)
                 : llvm::json::Value(Source.Nodes.at(Index).ID);
    };
    Items.emplace_back(llvm::json::Object{
        {"entry_id", E.ID},
        {"kind", B.Kind},
        {"link_status", E.Status},
        {"source_id", Selected.SourceID},
        {"artifact_id", Selected.ArtifactID},
        {"electron_source_id", Selected.EvidenceID},
        {"boundary_id", B.ID},
        {"node_id", N.ID},
        {"byte_offset", std::to_string(N.Start)},
        {"byte_length", std::to_string(N.End - N.Start)},
        {"value_node_id", NodeID(B.ValueNode)},
        {"construction_node_id", NodeID(B.ConstructionNode)},
        {"path_analysis_id", Selected.PathID},
        {"path_root_node_id", NodeID(E.RootNode)},
        {"path_operation_node_id", NodeID(E.OperationNode)},
        {"target_artifact_id", E.TargetArtifactID.empty()
                                   ? llvm::json::Value(nullptr)
                                   : llvm::json::Value(E.TargetArtifactID)},
        {"selected_target_source_id",
         E.TargetSourceID.empty() ? llvm::json::Value(nullptr)
                                  : llvm::json::Value(E.TargetSourceID)},
        {"runtime_entry_verified", false},
        {"runtime_path_base_verified", false},
        {"path_redacted", true}});
  }
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(State->Revision)},
      {"electron_entries_id", A.ID},
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
