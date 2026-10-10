//===- PackageDiff.cpp - Package evidence comparison -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Package evidence comparison.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/Error.h"
#include "neverd/web/Packages.h"

#include <algorithm>
#include <tuple>

namespace neverd::web {
PackageDiff comparePackages(const PackageAnalysis &B,
                            const PackageAnalysis &A) {
  if (B.Packages.empty() || A.Packages.empty() || B.Kind != A.Kind)
    throw Error("package_diff_incompatible_inputs");
  for (const auto *P : {&B, &A})
    if (P->Packages.size() > MaxPackageInstances ||
        P->Scripts.size() > MaxPackageRecords ||
        P->Entries.size() > MaxPackageRecords ||
        P->Files.size() > MaxPackageRecords)
      throw Error("package_diff_budget_exceeded");
  PackageDiff D;
  D.BeforeID = B.ID;
  D.AfterID = A.ID;
  D.ID = identity("package-diff", {B.ID, A.ID, PackageProfile});
  const auto &BR = B.Packages.front(), &AR = A.Packages.front();
  D.RootIdentity = !BR.Name || !AR.Name ? "name_not_supplied"
                   : BR.Name == AR.Name ? "same_declared_name"
                                        : "different_declared_name";
  D.Coverage = B.DirectoryInventory && A.DirectoryInventory
                   ? "two_supplied_directory_inventories"
                   : "partial_supplied_files";
  const auto SamePlatformCoverage = [](const PackageInstance &B,
                                       const PackageInstance &A) {
    return B.OSSource == A.OSSource && B.CPUSource == A.CPUSource &&
           B.LibcSource == A.LibcSource;
  };
  D.Platform = !SamePlatformCoverage(BR, AR)
                   ? "different_root_condition_coverage"
               : BR.OS == AR.OS && BR.CPU == AR.CPU && BR.Libc == AR.Libc
                   ? "same_root_conditions_host_not_selected"
                   : "different_root_conditions";
  const bool SameContract = B.LockVersion == A.LockVersion;
  D.InputContract = SameContract
                        ? "same_metadata_contract"
                        : "different_lock_versions_metadata_not_compared";
  auto Change = [&](std::string_view Kind, std::string_view Field, uint32_t BI,
                    uint32_t AI) {
    if (D.Changes.size() >= MaxPackageRecords)
      throw Error("package_diff_budget_exceeded");
    D.Changes.push_back(
        {identity("package-change",
                  {D.ID, Kind, Field, std::to_string(BI), std::to_string(AI)}),
         std::string(Kind), std::string(Field), BI, AI});
  };
  std::map<std::string, uint32_t> Before, After;
  auto Scripts = [](const PackageAnalysis &P) {
    std::vector<std::map<std::string, std::string>> R(P.Packages.size());
    for (const auto &S : P.Scripts) {
      if (S.Package >= R.size())
        throw Error("package_diff_invalid_model");
      R[S.Package].emplace(S.Name, S.Command);
    }
    return R;
  };
  auto Entries = [](const PackageAnalysis &P) {
    std::vector<std::vector<std::tuple<std::string, std::string, std::string>>>
        R(P.Packages.size());
    for (const auto &E : P.Entries) {
      if (E.Package >= R.size())
        throw Error("package_diff_invalid_model");
      R[E.Package].emplace_back(E.Kind, E.Name, E.Path);
    }
    for (auto &Items : R)
      std::sort(Items.begin(), Items.end());
    return R;
  };
  const auto BS = Scripts(B), AS = Scripts(A);
  const auto BE = Entries(B), AE = Entries(A);
  for (uint32_t I = 0; I < B.Packages.size(); ++I)
    Before.emplace(B.Packages[I].Location, I);
  for (uint32_t I = 0; I < A.Packages.size(); ++I)
    After.emplace(A.Packages[I].Location, I);
  for (const auto &[Location, BI] : Before) {
    const auto Found = After.find(Location);
    if (Found == After.end()) {
      Change("package_not_recorded_after", "location", BI, NoPackageIndex);
      continue;
    }
    const auto AI = Found->second;
    const auto &BP = B.Packages[BI], &AP = A.Packages[AI];
    if (BP.NameSource != AP.NameSource ||
        BP.Name.has_value() != AP.Name.has_value())
      Change("declaration_coverage_changed", "declared_name", BI, AI);
    else if (BP.Name && AP.Name && BP.Name != AP.Name)
      Change("package_replacement_candidate", "declared_name", BI, AI);
    else if (BP.Name != AP.Name)
      Change("package_declaration_changed", "declared_name", BI, AI);
    auto Field = [&](bool Changed, const char *Name) {
      if (Changed)
        Change("package_declaration_changed", Name, BI, AI);
    };
    if (BP.VersionKind == AP.VersionKind &&
        BP.VersionSource == AP.VersionSource)
      Field(BP.Version != AP.Version, "version");
    else
      Change("declaration_coverage_changed", "version_contract", BI, AI);
    if (SameContract) {
      Field(BP.Resolved != AP.Resolved || BP.OriginKind != AP.OriginKind,
            "resolved_origin");
      Field(BP.LegacyFetchSpec != AP.LegacyFetchSpec,
            "legacy_fetch_specification");
    }
    Field(BP.Integrity != AP.Integrity ||
              BP.IntegrityStatus != AP.IntegrityStatus,
          "integrity");
    if (SamePlatformCoverage(BP, AP))
      Field(BP.OS != AP.OS || BP.CPU != AP.CPU || BP.Libc != AP.Libc,
            "platform_conditions");
    else
      Change("declaration_coverage_changed", "platform_conditions", BI, AI);
    Field(BP.Dev != AP.Dev || BP.Optional != AP.Optional ||
              BP.DevOptional != AP.DevOptional || BP.Peer != AP.Peer ||
              BP.InBundle != AP.InBundle,
          "installation_flags");
    Field(BP.Link != AP.Link || BP.LinkStatus != AP.LinkStatus, "link");
    if (SameContract)
      Field(BP.HasInstallScript != AP.HasInstallScript, "install_script_claim");
    if (SameContract && (BI || B.RootDeclarations == A.RootDeclarations))
      Field(BP.Requirements != AP.Requirements ||
                BP.PeerOptional != AP.PeerOptional,
            "requirements");
    else
      Change("declaration_coverage_changed", "input_contract", BI, AI);
    if (BP.ManifestArtifactID.empty() != AP.ManifestArtifactID.empty()) {
      Change("declaration_coverage_changed", "manifest_evidence", BI, AI);
    } else if (!BP.ManifestArtifactID.empty()) {
      Field(BP.ManifestStatus != AP.ManifestStatus, "manifest_evidence");
      Field(BS[BI] != AS[AI], "scripts");
      Field(BE[BI] != AE[AI], "entry_declarations");
    }
  }
  for (const auto &[Location, AI] : After)
    if (!Before.contains(Location))
      Change("package_not_recorded_before", "location", NoPackageIndex, AI);
  Before.clear();
  After.clear();
  for (uint32_t I = 0; I < B.Files.size(); ++I)
    Before.emplace(B.Files[I].Path, I);
  for (uint32_t I = 0; I < A.Files.size(); ++I)
    After.emplace(A.Files[I].Path, I);
  for (const auto &[Path, BI] : Before) {
    const auto Found = After.find(Path);
    if (Found == After.end())
      Change("file_not_supplied_after", "file_bytes", BI, NoPackageIndex);
    else if (B.Files[BI].Hash != A.Files[Found->second].Hash)
      Change("supplied_file_changed", "file_bytes", BI, Found->second);
  }
  for (const auto &[Path, AI] : After)
    if (!Before.contains(Path))
      Change("file_not_supplied_before", "file_bytes", NoPackageIndex, AI);
  return D;
}
} // namespace neverd::web
