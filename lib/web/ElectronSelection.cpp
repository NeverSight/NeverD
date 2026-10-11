//===- ElectronSelection.cpp - Shared Electron evidence selection ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared Electron evidence selection.
///
//===----------------------------------------------------------------------===//

#include "ElectronSelection.h"

#include "JsonReader.h"
#include "SourceModel.h"

#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd::web {
std::vector<std::string> electronSourceSelection(std::string_view Options) {
  const auto Parsed = parseBoundedJSON(Options, {4096, 4, 64, 64});
  const auto *Object = Parsed.getAsObject();
  if (!Object || Object->size() != 2 ||
      Object->getInteger("schema_version") != 1 ||
      !Object->getArray("source_ids"))
    throw Error("invalid_electron_ipc_options");
  const auto &IDs = *Object->getArray("source_ids");
  if (IDs.empty() || IDs.size() > MaxElectronIPCSources)
    throw Error("electron_ipc_source_budget_exceeded");
  std::vector<std::string> Result;
  Result.reserve(IDs.size());
  for (const auto &Item : IDs) {
    const auto ID = Item.getAsString();
    if (!ID || ID->empty() || ID->size() > 64)
      throw Error("invalid_electron_ipc_options");
    Result.push_back(ID->str());
  }
  return Result;
}

ElectronSelection
selectElectronSources(const Snapshot &Namespace,
                      const ElectronManifest &Manifest,
                      std::span<const ElectronIPCInput> Input) {
  if (Input.empty() || Input.size() > MaxElectronIPCSources)
    throw Error("electron_ipc_source_budget_exceeded");
  if (Namespace.Artifacts.size() > Limits::HardEntries + 1)
    throw Error("electron_ipc_namespace_budget_exceeded");
  ElectronSelection A;
  auto Step = [&](uint64_t N = 1) {
    if (N > MaxElectronIPCSteps - A.Steps)
      throw Error("electron_ipc_work_budget_exceeded");
    A.Steps += N;
  };
  std::map<std::string_view, const Artifact *> Artifacts;
  std::set<std::string_view> Paths;
  for (const auto &Entry : Namespace.Artifacts) {
    Step();
    if (!Artifacts.emplace(Entry.ID, &Entry).second ||
        !Paths.insert(Entry.MemberPath).second)
      throw Error("electron_ipc_ambiguous_namespace");
  }
  const auto FindArtifact = [&](std::string_view ID) -> const Artifact & {
    const auto Found = Artifacts.find(ID);
    if (Found == Artifacts.end() || Found->second->Directory)
      throw Error("electron_ipc_source_outside_namespace");
    return *Found->second;
  };
  const auto &ManifestArtifact = FindArtifact(Manifest.ArtifactID);
  if (Namespace.Artifacts.empty() || !Namespace.Artifacts[0].Directory ||
      ManifestArtifact.MemberPath.empty())
    throw Error("electron_ipc_directory_namespace_required");
  if (Manifest.ID !=
      identity("electron-manifest",
               {Namespace.ID, Manifest.ArtifactID, ManifestArtifact.BlobHash,
                ElectronManifestProfile}))
    throw Error("electron_ipc_manifest_mismatch");
  if (Manifest.MainLinkStatus != "exact_admitted_file_candidate" ||
      Manifest.MainArtifactID.empty())
    throw Error("electron_ipc_main_candidate_required");
  const auto Slash = ManifestArtifact.MemberPath.rfind('/');
  A.Directory = Slash == std::string::npos
                    ? std::string()
                    : ManifestArtifact.MemberPath.substr(0, Slash + 1);
  A.Sources.assign(Input.begin(), Input.end());
  for (const auto &I : A.Sources)
    if (!I.Source || !I.Evidence)
      throw Error("electron_ipc_invalid_evidence");
  std::sort(
      A.Sources.begin(), A.Sources.end(),
      [](const auto &L, const auto &R) { return L.Source->ID < R.Source->ID; });
  std::set<std::string_view> SelectedArtifacts;
  bool HasMain = false;
  for (const auto &I : A.Sources) {
    const auto &S = *I.Source;
    const auto &E = *I.Evidence;
    Step(validateSourceModel(S));
    const auto &Artifact = FindArtifact(S.ArtifactID);
    if (S.BlobHash != Artifact.BlobHash || S.ID != E.SourceID ||
        E.ID != identity("electron-source", {S.ID, E.ModuleID, E.OriginID,
                                             E.ValueID, ElectronSourceProfile}))
      throw Error("electron_ipc_invalid_evidence");
    if (!Artifact.MemberPath.starts_with(A.Directory))
      throw Error("electron_ipc_source_outside_manifest_scope");
    if (!SelectedArtifacts.insert(S.ArtifactID).second)
      throw Error("electron_ipc_duplicate_source_artifact");
    if (S.ArtifactID == Manifest.MainArtifactID) {
      HasMain = true;
      if (S.SourceType != Manifest.SourceType)
        throw Error("electron_ipc_main_source_type_mismatch");
    }
    if (E.Status != "partial" &&
        (!E.Boundaries.empty() ||
         (E.Status != "unavailable" && E.Status != "budget_exceeded")))
      throw Error("electron_ipc_invalid_evidence");
  }
  if (!HasMain)
    throw Error("electron_ipc_main_source_required");
  return A;
}
} // namespace neverd::web
