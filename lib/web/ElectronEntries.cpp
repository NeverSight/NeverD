//===- ElectronEntries.cpp - Captured Electron entry candidates --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Captured Electron entry candidates.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/ElectronEntries.h"

#include "ElectronSelection.h"

#include "neverd/web/Error.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd::web {
ElectronEntries
associateElectronEntries(const Snapshot &Namespace,
                         const ElectronManifest &Manifest,
                         std::span<const ElectronEntryInput> Input) {
  if (Input.empty() || Input.size() > MaxElectronIPCSources)
    throw Error("electron_ipc_source_budget_exceeded");
  std::vector<ElectronIPCInput> ScopeInput;
  std::map<std::string_view, const ElectronEntryInput *> Models;
  for (const auto &I : Input) {
    if (!I.Source || !I.Evidence || !I.Bindings || !I.Modules || !I.Origins ||
        !I.Values)
      throw Error("electron_entry_invalid_evidence");
    if (!Models.emplace(I.Source->ID, &I).second)
      throw Error("electron_ipc_duplicate_source_artifact");
    if (I.Evidence->BindingID != I.Bindings->ID ||
        I.Evidence->ModuleID != I.Modules->ID ||
        I.Evidence->OriginID != I.Origins->ID ||
        I.Evidence->ValueID != I.Values->ID)
      throw Error("electron_entry_invalid_evidence");
    ScopeInput.push_back({I.Source, I.Evidence});
  }
  const auto Scope = selectElectronSources(Namespace, Manifest, ScopeInput);
  ElectronEntries A;
  A.Steps = Scope.Steps;
  A.NamespaceID = Namespace.ID;
  A.ManifestID = Manifest.ID;
  A.MainArtifactID = Manifest.MainArtifactID;
  const auto Step = [&](uint64_t N = 1) {
    if (N > MaxElectronEntrySteps - A.Steps)
      throw Error("electron_entry_budget_exceeded");
    A.Steps += N;
  };
  const auto ComparePaths = [&](std::string_view L, std::string_view R) {
    const auto Common = std::min(L.size(), R.size());
    for (size_t I = 0; I < Common; ++I) {
      Step();
      const auto Left = static_cast<unsigned char>(L[I]);
      const auto Right = static_cast<unsigned char>(R[I]);
      if (Left != Right)
        return Left < Right;
    }
    Step();
    return L.size() < R.size();
  };
  std::map<std::string_view, const Artifact *> ByID;
  std::map<std::string_view, const Artifact *, decltype(ComparePaths)> ByPath(
      ComparePaths);
  std::map<std::string_view, std::string_view> SelectedSources;
  for (const auto &Artifact : Namespace.Artifacts) {
    Step();
    ByID.emplace(Artifact.ID, &Artifact);
    ByPath.emplace(Artifact.MemberPath, &Artifact);
  }
  for (const auto &I : Scope.Sources)
    SelectedSources.emplace(I.Source->ArtifactID, I.Source->ID);
  uint64_t Boundaries = 0;
  for (uint32_t SI = 0; SI < Scope.Sources.size(); ++SI) {
    const auto &S = *Scope.Sources[SI].Source;
    const auto &E = *Scope.Sources[SI].Evidence;
    const auto &I = *Models.at(S.ID);
    if (E.Boundaries.size() > MaxElectronRecords - Boundaries)
      throw Error("electron_entry_budget_exceeded");
    Boundaries += E.Boundaries.size();
    std::vector<uint32_t> Requested;
    std::set<std::string_view> Seen;
    for (const auto &B : E.Boundaries) {
      Step();
      if (B.Node >= S.Nodes.size() ||
          (B.ValueNode != NoSourceIndex && B.ValueNode >= S.Nodes.size()) ||
          (B.ConstructionNode != NoSourceIndex &&
           B.ConstructionNode >= S.Nodes.size()) ||
          !Seen.insert(B.ID).second ||
          B.ID !=
              identity("electron-boundary", {E.ID, S.Nodes[B.Node].ID, B.Kind}))
        throw Error("electron_entry_invalid_evidence");
      if ((B.Kind == "window_preload" || B.Kind == "renderer_file" ||
           B.Kind == "renderer_url") &&
          B.ValueNode != NoSourceIndex)
        Requested.push_back(B.ValueNode);
    }
    const CapturedPathContext Context{Namespace.ID, Manifest.ID,
                                      ByID.at(S.ArtifactID)->MemberPath,
                                      Scope.Directory};
    const auto Paths = analyzeSourcePaths(
        S, *I.Bindings, *I.Modules, *I.Origins, *I.Values, Context, Requested);
    Step(Paths.Steps);
    A.Sources.push_back(
        {S.ID, S.ArtifactID, E.ID, Paths.ID, Paths.Status, Paths.Reason});
    if (Paths.Status != "partial")
      ++A.UnavailablePaths;
    for (uint32_t BI = 0; BI < E.Boundaries.size(); ++BI) {
      Step();
      const auto &B = E.Boundaries[BI];
      if (B.Kind != "window_preload" && B.Kind != "renderer_file" &&
          B.Kind != "renderer_url")
        continue;
      ElectronEntry Record;
      Record.Source = SI;
      Record.Boundary = BI;
      if (B.ValueNode == NoSourceIndex)
        Record.Status = "missing_path_expression";
      else if (Paths.Status != "partial")
        Record.Status = Paths.Reason;
      else {
        const auto &Path = *Paths.Nodes.at(B.ValueNode);
        // Reusing one aliased value can still normalize a long path once per
        // entry. Charge that work even when the expression result was cached.
        Step(Path.Text.size() + Scope.Directory.size() + 1);
        Record.RootNode = Path.RootNode;
        Record.OperationNode = Path.OperationNode;
        const auto Target = capturedEntryTarget(Path, Scope.Directory, B.Kind);
        Record.Status = Target.Status;
        if (Target.Status == "captured_path_candidate") {
          const auto Found = ByPath.find(Target.Path);
          if (Found == ByPath.end())
            Record.Status = "no_exact_admitted_file";
          else if (Found->second->Directory)
            Record.Status = "directory_not_entry_file";
          else {
            Record.Status = "exact_admitted_file_candidate";
            Record.TargetArtifactID = Found->second->ID;
            if (const auto Selected = SelectedSources.find(Found->second->ID);
                Selected != SelectedSources.end())
              Record.TargetSourceID = Selected->second;
            ++A.LinkedFiles;
          }
        }
      }
      A.Entries.push_back(std::move(Record));
    }
  }
  std::vector<std::string_view> Fields{Namespace.ID, Manifest.ID,
                                       ElectronEntryProfile};
  for (const auto &S : A.Sources) {
    Fields.push_back(S.SourceID);
    Fields.push_back(S.EvidenceID);
    Fields.push_back(S.PathID);
  }
  A.ID = identity("electron-entries", Fields);
  for (auto &E : A.Entries) {
    const auto &Source = A.Sources[E.Source];
    const auto &Boundary =
        Models.at(Source.SourceID)->Evidence->Boundaries[E.Boundary];
    E.ID = identity("electron-entry", {A.ID, Boundary.ID});
  }
  return A;
}
} // namespace neverd::web
