//===- SessionPackages.cpp - Package analysis publication --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Package analysis publication.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"

namespace neverd::web {
namespace {
llvm::json::Value nullable(const std::string &Text) {
  return Text.empty() ? llvm::json::Value(nullptr) : llvm::json::Value(Text);
}
llvm::json::Object summary(const PackageAnalysis &A, uint64_t Revision) {
  return llvm::json::Object{{"schema_version", 1},
                            {"status", "ok"},
                            {"revision", std::to_string(Revision)},
                            {"package_analysis_id", A.ID},
                            {"artifact_id", A.ArtifactID},
                            {"blob_sha256", A.BlobHash},
                            {"profile", std::string(PackageProfile)},
                            {"input_kind", A.Kind},
                            {"lockfile_version", A.LockVersion},
                            {"package_count", A.Packages.size()},
                            {"dependency_count", A.Dependencies.size()},
                            {"script_count", A.Scripts.size()},
                            {"entry_count", A.Entries.size()},
                            {"entry_coverage", "explicit_main_module_bin"},
                            {"file_count", A.Files.size()},
                            {"directory_inventory", A.DirectoryInventory},
                            {"root_declarations", A.RootDeclarations},
                            {"legacy_metadata", A.LegacyStatus},
                            {"analysis_status", "partial"},
                            {"behavior_analysis", "not_analyzed"},
                            {"advisory_analysis", "not_supplied"},
                            {"provenance_verification", "not_performed"},
                            {"integrity_verification", "declarations_only"},
                            {"version_satisfaction", "not_verified"},
                            {"selected_platform", "not_selected"},
                            {"safety_verdict", "not_assessed"},
                            {"executes_input", false},
                            {"metadata_bytes", std::to_string(A.MetadataBytes)},
                            {"steps", A.Steps},
                            {"redaction_policy", "metadata-only-v1"}};
}
llvm::json::Object summary(const PackageDiff &D, uint64_t Revision) {
  return llvm::json::Object{{"schema_version", 1},
                            {"status", "ok"},
                            {"revision", std::to_string(Revision)},
                            {"package_diff_id", D.ID},
                            {"before_analysis_id", D.BeforeID},
                            {"after_analysis_id", D.AfterID},
                            {"profile", std::string(PackageProfile)},
                            {"change_count", D.Changes.size()},
                            {"root_identity", D.RootIdentity},
                            {"coverage", D.Coverage},
                            {"platform_comparison", D.Platform},
                            {"input_contract_comparison", D.InputContract},
                            {"comparison_scope", "supplied_evidence"},
                            {"release_alignment_verified", false},
                            {"safety_verdict", "not_assessed"},
                            {"redaction_policy", "metadata-only-v1"}};
}
uint64_t end(uint64_t Offset, uint64_t Limit, uint64_t Total) {
  if (!Limit || Limit > 512 || Offset > Total)
    throw Error("invalid_page");
  return std::min<uint64_t>(Total, Offset + Limit);
}
} // namespace

std::string Session::analyzePackages(std::string_view Revision,
                                     std::string_view ArtifactID,
                                     std::string_view InputKind) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  for (const auto &[ID, A] : State->PackageAnalyses)
    if (A.ArtifactID == ArtifactID && A.Kind == InputKind)
      return json(summary(A, State->Revision));
  if (State->PackageAnalyses.size() >= 4)
    throw Error("package_cache_budget_exceeded");
  const auto Namespace = State->memberNamespace(ArtifactID);
  auto A = web::analyzePackages(Namespace ? *Namespace : State->Published,
                                ArtifactID, InputKind);
  auto Reply = json(summary(A, State->Revision));
  State->PackageAnalyses.emplace(A.ID, std::move(A));
  return Reply;
}

std::string Session::packageRecords(std::string_view Revision,
                                    std::string_view AnalysisID,
                                    std::string_view Kind, uint64_t Offset,
                                    uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->PackageAnalyses.find(std::string(AnalysisID));
  if (Found == State->PackageAnalyses.end())
    throw Error("unknown_package_analysis");
  const auto &A = Found->second;
  auto Node = [&](uint32_t Index) -> llvm::json::Value {
    return Index == NoPackageIndex ? llvm::json::Value(nullptr)
                                   : llvm::json::Value(A.Packages.at(Index).ID);
  };
  llvm::json::Array Items;
  uint64_t Total = 0, End = 0;
  if (Kind == "packages") {
    Total = A.Packages.size();
    End = end(Offset, Limit, Total);
    for (auto I = Offset; I < End; ++I) {
      const auto &P = A.Packages[I];
      Items.emplace_back(llvm::json::Object{
          {"package_id", P.ID},
          {"location_parent_instance_id", Node(P.Parent)},
          {"link_target_id", Node(P.LinkTarget)},
          {"link_status", P.LinkStatus},
          {"manifest_artifact_id", nullable(P.ManifestArtifactID)},
          {"manifest_status", P.ManifestStatus},
          {"name_present", P.Name.has_value()},
          {"version_present", P.Version.has_value()},
          {"version_kind", P.VersionKind},
          {"name_evidence_source", P.NameSource},
          {"version_evidence_source", P.VersionSource},
          {"legacy_fetch_specification_present", P.LegacyFetchSpec.has_value()},
          {"installed_alias_present", !P.InstalledName.empty()},
          {"origin_kind", P.OriginKind},
          {"integrity_status", P.IntegrityStatus},
          {"integrity_verified", false},
          {"dev", P.Dev},
          {"optional", P.Optional},
          {"dev_optional", P.DevOptional},
          {"peer", P.Peer},
          {"link", P.Link},
          {"in_bundle", P.InBundle},
          {"install_script_claim", P.HasInstallScript},
          {"os_condition_count", P.OS.size()},
          {"os_evidence_source", P.OSSource},
          {"cpu_condition_count", P.CPU.size()},
          {"cpu_evidence_source", P.CPUSource},
          {"libc_condition_count", P.Libc.size()},
          {"libc_evidence_source", P.LibcSource},
          {"platform_match", "not_evaluated"},
          {"metadata_redacted", true}});
    }
  } else if (Kind == "dependencies") {
    Total = A.Dependencies.size();
    End = end(Offset, Limit, Total);
    for (auto I = Offset; I < End; ++I) {
      const auto &E = A.Dependencies[I];
      Items.emplace_back(
          llvm::json::Object{{"dependency_id", E.ID},
                             {"from_package_id", Node(E.From)},
                             {"candidate_package_id", Node(E.Candidate)},
                             {"kind", E.Kind},
                             {"spec_kind", E.SpecKind},
                             {"resolution_status", E.Status},
                             {"optional", E.Optional},
                             {"conditional", E.Conditional},
                             {"spec_satisfaction_verified", false},
                             {"runtime_target_verified", false},
                             {"declarations_redacted", true}});
    }
  } else if (Kind == "scripts") {
    Total = A.Scripts.size();
    End = end(Offset, Limit, Total);
    for (auto I = Offset; I < End; ++I) {
      const auto &S = A.Scripts[I];
      Items.emplace_back(
          llvm::json::Object{{"script_id", S.ID},
                             {"package_id", Node(S.Package)},
                             {"artifact_id", S.ArtifactID},
                             {"kind", S.Kind},
                             {"behavior_analysis", "not_analyzed"},
                             {"command_redacted", true},
                             {"executed", false}});
    }
  } else if (Kind == "entries") {
    Total = A.Entries.size();
    End = end(Offset, Limit, Total);
    for (auto I = Offset; I < End; ++I) {
      const auto &E = A.Entries[I];
      Items.emplace_back(llvm::json::Object{
          {"entry_id", E.ID},
          {"package_id", Node(E.Package)},
          {"artifact_id", E.ArtifactID},
          {"kind", E.Kind},
          {"link_status", E.Status},
          {"target_artifact_id", nullable(E.TargetArtifactID)},
          {"runtime_target_verified", false},
          {"path_redacted", true}});
    }
  } else if (Kind == "files") {
    Total = A.Files.size();
    End = end(Offset, Limit, Total);
    for (auto I = Offset; I < End; ++I) {
      const auto &F = A.Files[I];
      Items.emplace_back(
          llvm::json::Object{{"file_evidence_id", F.ID},
                             {"artifact_id", F.ArtifactID},
                             {"package_id", Node(F.Package)},
                             {"blob_sha256", F.Hash},
                             {"size_bytes", std::to_string(F.Size)},
                             {"kind", F.Kind},
                             {"path_redacted", true}});
    }
  } else {
    throw Error("invalid_record_kind");
  }
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(State->Revision)},
      {"package_analysis_id", A.ID},
      {"record_kind", std::string(Kind)},
      {"offset", Offset},
      {"total", Total},
      {"items", std::move(Items)},
      {"next_offset",
       End < Total ? llvm::json::Value(End) : llvm::json::Value(nullptr)},
      {"redaction_policy", "metadata-only-v1"}});
}

std::string Session::comparePackages(std::string_view Revision,
                                     std::string_view BeforeID,
                                     std::string_view AfterID) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto B = State->PackageAnalyses.find(std::string(BeforeID));
  const auto A = State->PackageAnalyses.find(std::string(AfterID));
  if (B == State->PackageAnalyses.end() || A == State->PackageAnalyses.end())
    throw Error("unknown_package_analysis");
  for (const auto &[ID, D] : State->PackageDiffs)
    if (D.BeforeID == BeforeID && D.AfterID == AfterID)
      return json(summary(D, State->Revision));
  if (State->PackageDiffs.size() >= 4)
    throw Error("package_diff_cache_budget_exceeded");
  auto D = web::comparePackages(B->second, A->second);
  auto Reply = json(summary(D, State->Revision));
  State->PackageDiffs.emplace(D.ID, std::move(D));
  return Reply;
}

std::string Session::packageDiffRecords(std::string_view Revision,
                                        std::string_view DiffID,
                                        uint64_t Offset, uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->PackageDiffs.find(std::string(DiffID));
  if (Found == State->PackageDiffs.end())
    throw Error("unknown_package_diff");
  const auto &D = Found->second;
  const auto &B = State->PackageAnalyses.at(D.BeforeID),
             &A = State->PackageAnalyses.at(D.AfterID);
  const auto End = end(Offset, Limit, D.Changes.size());
  auto Evidence = [](const PackageAnalysis &A, uint32_t I,
                     bool File) -> llvm::json::Value {
    if (I == NoPackageIndex)
      return nullptr;
    return File ? A.Files.at(I).ID : A.Packages.at(I).ID;
  };
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &C = D.Changes[I];
    const bool File = C.Field == "file_bytes";
    Items.emplace_back(
        llvm::json::Object{{"change_id", C.ID},
                           {"kind", C.Kind},
                           {"field", C.Field},
                           {"before_evidence_id", Evidence(B, C.Before, File)},
                           {"after_evidence_id", Evidence(A, C.After, File)},
                           {"disposition", "unreviewed"},
                           {"values_redacted", true}});
  }
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(State->Revision)},
      {"package_diff_id", D.ID},
      {"offset", Offset},
      {"total", D.Changes.size()},
      {"items", std::move(Items)},
      {"next_offset", End < D.Changes.size() ? llvm::json::Value(End)
                                             : llvm::json::Value(nullptr)},
      {"redaction_policy", "metadata-only-v1"}});
}
} // namespace neverd::web
