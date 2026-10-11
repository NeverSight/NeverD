//===- SessionDesktop.cpp - Desktop manifest publication ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Revision-bound, metadata-only NW.js and VSIX manifest results.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"

#include <algorithm>

namespace neverd::web {
std::string Session::analyzeDesktopManifest(std::string_view Revision,
                                            std::string_view ArtifactID,
                                            std::string_view Kind) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto View = State->artifactView(ArtifactID);
  if (!View)
    throw Error("unknown_artifact");
  if (View->Content.size() > MaxDesktopManifestBytes)
    throw Error("desktop_manifest_byte_budget_exceeded");
  const auto Key = std::make_pair(std::string(ArtifactID), std::string(Kind));
  const auto Cached = State->DesktopManifests.find(Key);
  std::optional<DesktopManifest> Pending;
  if (Cached == State->DesktopManifests.end()) {
    if (State->DesktopManifests.size() >= 16)
      throw Error("desktop_manifest_cache_budget_exceeded");
    auto Namespace = State->memberNamespace(ArtifactID);
    if (!Namespace &&
        std::none_of(State->Published.Artifacts.begin(),
                     State->Published.Artifacts.end(),
                     [&](const auto &F) { return F.ID == ArtifactID; })) {
      Snapshot Single;
      Single.ID = identity("desktop-selected-bytes",
                           {State->Published.ID, ArtifactID, View->BlobHash});
      Artifact A;
      A.ID = ArtifactID;
      A.BlobHash = View->BlobHash;
      A.Content = View->Content;
      Single.Artifacts.push_back(std::move(A));
      Namespace = std::move(Single);
    }
    Pending = web::analyzeDesktopManifest(
        Namespace ? *Namespace : State->Published, ArtifactID,
        View->Content.read(0, View->Content.size(), MaxDesktopManifestBytes),
        Kind);
  }
  const auto &A = Pending ? *Pending : Cached->second;
  llvm::json::Object Declarations;
  for (const auto &[Name, Status] : A.Declarations)
    Declarations[Name] = Status;
  llvm::json::Array Entries;
  for (const auto &E : A.Entries)
    Entries.emplace_back(llvm::json::Object{
        {"entry_id", E.ID},
        {"field", E.Field},
        {"value_status", E.ValueStatus},
        {"link_status", E.LinkStatus},
        {"artifact_id", E.ArtifactID.empty() ? llvm::json::Value(nullptr)
                                             : llvm::json::Value(E.ArtifactID)},
        {"content_kind", E.ContentKind},
        {"runtime_entry_verified", false}});
  auto Reply =
      json(llvm::json::Object{{"schema_version", 1},
                              {"status", "ok"},
                              {"revision", std::to_string(State->Revision)},
                              {"manifest_id", A.ID},
                              {"artifact_id", A.ArtifactID},
                              {"blob_sha256", View->BlobHash},
                              {"origin", llvm::json::Object(View->Origin)},
                              {"profile", A.Profile},
                              {"framework_candidate", A.Framework},
                              {"layout_candidate", A.Layout},
                              {"framework_evidence", "caller_selected_profile"},
                              {"framework_verified", false},
                              {"runtime_version_verified", false},
                              {"runtime_entry_verified", false},
                              {"version_compatibility", "not_analyzed"},
                              {"permission_analysis", "not_analyzed"},
                              {"activation_analysis", "not_analyzed"},
                              {"helper_version_association", "not_analyzed"},
                              {"declarations", std::move(Declarations)},
                              {"entries", std::move(Entries)},
                              {"declarations_redacted", true},
                              {"executes_input", false},
                              {"resolves_external_references", false},
                              {"redaction_policy", "metadata-only-v1"}});
  if (Pending)
    State->DesktopManifests.emplace(Key, std::move(*Pending));
  return Reply;
}
} // namespace neverd::web
