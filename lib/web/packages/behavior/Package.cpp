//===- Package.cpp - Exact source to package evidence joins -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve selected-source coverage and distinguish ownership from delivery.
///
//===----------------------------------------------------------------------===//

#include "Rules.h"

#include "neverd/web/Error.h"
#include "neverd/web/NodeBehavior.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd::web {

PackageBehavior
analyzePackageBehavior(const PackageAnalysis &P,
                       std::span<const PackageBehaviorInput> Input,
                       std::span<const PackageBehaviorOrigin> Origins) {
  if (Input.empty() || Input.size() > MaxPackageBehaviorSources)
    throw Error("package_behavior_source_budget_exceeded");
  if (P.Packages.size() > MaxPackageInstances ||
      P.Files.size() > MaxPackageRecords ||
      P.Entries.size() > MaxPackageRecords ||
      Origins.size() > MaxPackageRecords)
    throw Error("package_behavior_inventory_budget_exceeded");
  PackageBehavior A;
  A.PackageAnalysisID = P.ID;
  auto Step = [&](uint64_t Count = 1) {
    if (Count > MaxPackageBehaviorSteps - A.Steps)
      throw Error("package_behavior_work_budget_exceeded");
    A.Steps += Count;
  };
  auto ID = [&](std::string_view Value) {
    Step();
    if (Value.size() != 64 ||
        Value.find_first_not_of("0123456789abcdef") != Value.npos)
      throw Error("invalid_package_behavior_identity");
    Step(Value.size());
  };
  ID(P.ID);
  std::map<std::string_view, const PackageFile *> Files;
  std::map<std::string_view, const PackageBehaviorOrigin *> Parents;
  std::map<std::string_view, std::vector<std::string_view>> Entries;
  for (const auto &F : P.Files) {
    ID(F.ID);
    ID(F.ArtifactID);
    if ((F.Package != NoPackageIndex && F.Package >= P.Packages.size()) ||
        !Files.emplace(F.ArtifactID, &F).second)
      throw Error("invalid_package_behavior_inventory");
  }
  for (const auto &O : Origins) {
    ID(O.ArtifactID);
    ID(O.ParentArtifactID);
    ID(O.EvidenceID);
    if (O.Kind != "bun_source" && O.Kind != "bun_asset" &&
        O.Kind != "sea_region" && O.Kind != "nexe_member" &&
        O.Kind != "nexe_bootstrap")
      throw Error("invalid_package_behavior_origin");
    if (!Parents.emplace(O.ArtifactID, &O).second)
      throw Error("ambiguous_package_behavior_origin");
  }
  for (const auto &E : P.Entries) {
    ID(E.ID);
    if (!E.TargetArtifactID.empty()) {
      ID(E.TargetArtifactID);
      Entries[E.TargetArtifactID].push_back(E.ID);
    }
  }
  std::vector<PackageBehaviorInput> Sources(Input.begin(), Input.end());
  for (const auto &I : Sources) {
    if (!I.Source || !I.Behavior)
      throw Error("invalid_package_behavior_input");
    ID(I.Source->ID);
    ID(I.Source->ArtifactID);
    ID(I.Behavior->ID);
  }
  std::sort(Sources.begin(), Sources.end(), [&](const auto &X, const auto &Y) {
    Step(64);
    return X.Source->ID < Y.Source->ID;
  });
  std::set<std::string_view> Selected;
  std::string IdentityMaterial;
  uint64_t Bytes = 0, Nodes = 0;
  for (const auto &I : Sources) {
    const auto &S = *I.Source;
    const auto &B = *I.Behavior;
    if (!Selected.insert(S.ArtifactID).second)
      throw Error("duplicate_package_behavior_source");
    if (I.SourceBytes > MaxPackageBehaviorBytes - Bytes ||
        S.Nodes.size() > MaxPackageBehaviorNodes - Nodes ||
        B.Records.size() > MaxPackageBehaviorRecords - A.RecordCount)
      throw Error("package_behavior_selection_budget_exceeded");
    Bytes += I.SourceBytes;
    Nodes += S.Nodes.size();
    if (B.SourceID != S.ID || B.ArtifactID != S.ArtifactID ||
        B.SourceHash != S.BlobHash || B.SourceType != S.SourceType ||
        B.ID != node_behavior::analysisID(S.ID) ||
        (B.Status != "partial" && B.Status != "unavailable" &&
         B.Status != "budget_exceeded") ||
        (B.Status != "partial" && !B.Records.empty()))
      throw Error("package_behavior_evidence_mismatch");
    PackageBehaviorSource Row;
    Row.SourceID = S.ID;
    Row.ArtifactID = S.ArtifactID;
    Row.BehaviorID = B.ID;
    Row.Status = B.Status;
    Row.Reason = B.Reason;
    Row.DeliveryArtifactID = S.ArtifactID;
    std::set<std::string_view> Seen;
    while (!Files.count(Row.DeliveryArtifactID)) {
      Step();
      if (Row.OriginIDs.size() >= MaxPackageBehaviorAncestry)
        throw Error("package_behavior_ancestry_budget_exceeded");
      const auto Found = Parents.find(Row.DeliveryArtifactID);
      if (Found == Parents.end())
        throw Error("package_behavior_source_outside_inventory");
      const auto &O = *Found->second;
      if (!Seen.insert(O.ArtifactID).second)
        throw Error("package_behavior_origin_cycle");
      Row.OriginIDs.push_back(O.EvidenceID);
      Row.DeliveryArtifactID = O.ParentArtifactID;
    }
    const auto &F = *Files.at(Row.DeliveryArtifactID);
    Row.FileID = F.ID;
    if (F.Package != NoPackageIndex) {
      ID(P.Packages[F.Package].ID);
      Row.PackageID = P.Packages[F.Package].ID;
    }
    Row.Relation = Row.OriginIDs.empty() ? "direct_file" : "embedded_source";
    if (Row.OriginIDs.empty()) {
      if (F.Hash != S.BlobHash || F.Size != I.SourceBytes)
        throw Error("package_behavior_file_mismatch");
      const auto Found = Entries.find(S.ArtifactID);
      if (Found != Entries.end()) {
        Row.EntryCount = Found->second.size();
        for (auto Entry : Found->second) {
          if (Row.EntryIDs.size() >= MaxPackageBehaviorEntries)
            break;
          Step(Entry.size());
          Row.EntryIDs.emplace_back(Entry);
        }
      }
    }
    // Account retained anchor strings; never copy arbitrary payload values.
    for (const auto &R : B.Records) {
      Step(1 + R.ID.size() + R.NodeID.size() + R.CalleeID.size() +
           R.ReceiverID.size() + R.RequestID.size() + R.RequestNodeID.size() +
           R.ImportID.size() + R.API.size() + R.Kind.size() +
           R.OriginEvidence.size() + R.Encoding.size());
      if (R.Arguments.size() > MaxNodeBehaviorArguments)
        throw Error("invalid_package_behavior_record");
      for (const auto &Arg : R.Arguments)
        Step(1 + Arg.NodeID.size());
    }
    Row.Records = B.Records;
    A.RecordCount += Row.Records.size();
    A.UnavailableSources += B.Status != "partial";
    Step(B.ID.size() + F.ID.size());
    IdentityMaterial += B.ID;
    IdentityMaterial += F.ID;
    for (const auto &Origin : Row.OriginIDs) {
      Step(Origin.size());
      IdentityMaterial += Origin;
    }
    // A fixed-size boundary retains each source's separate ancestry sequence.
    IdentityMaterial += identity("package-behavior-source-end", {S.ID});
    A.Sources.push_back(std::move(Row));
  }
  A.ID = identity("package-behavior",
                  {P.ID, NodeBehaviorProfile, IdentityMaterial});
  return A;
}

} // namespace neverd::web
