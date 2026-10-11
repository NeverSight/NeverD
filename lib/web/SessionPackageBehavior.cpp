//===- SessionPackageBehavior.cpp - Explicit package behavior selection -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Connect retained extractor ancestry to exact package inventory members.
///
//===----------------------------------------------------------------------===//

#include "JsonReader.h"
#include "SessionInternal.h"
#include "packages/behavior/Publication.h"

#include <algorithm>

namespace neverd::web {
namespace {
std::vector<std::string> selection(std::string_view Options) {
  const auto Value = parseBoundedJSON(Options, {4096, 4, 64, 64});
  const auto *Object = Value.getAsObject();
  if (!Object || Object->size() != 2 ||
      Object->getInteger("schema_version") != 1 ||
      !Object->getArray("source_ids"))
    throw Error("invalid_package_behavior_options");
  const auto &Items = *Object->getArray("source_ids");
  if (Items.empty() || Items.size() > MaxPackageBehaviorSources)
    throw Error("package_behavior_source_budget_exceeded");
  std::vector<std::string> IDs;
  for (const auto &Item : Items) {
    const auto ID = Item.getAsString();
    if (!ID || ID->size() != 64 ||
        ID->find_first_not_of("0123456789abcdef") != ID->npos)
      throw Error("invalid_package_behavior_options");
    IDs.push_back(ID->str());
  }
  std::sort(IDs.begin(), IDs.end());
  if (std::adjacent_find(IDs.begin(), IDs.end()) != IDs.end())
    throw Error("duplicate_package_behavior_source");
  return IDs;
}
} // namespace

std::string Session::analyzePackageBehavior(std::string_view Revision,
                                            std::string_view PackageID,
                                            std::string_view Options) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto IDs = selection(Options);
  const auto P = State->PackageAnalyses.find(std::string(PackageID));
  if (P == State->PackageAnalyses.end())
    throw Error("unknown_package_analysis");
  for (const auto &[ID, A] : State->PackageBehaviors) {
    if (A.PackageAnalysisID != PackageID || A.Sources.size() != IDs.size())
      continue;
    bool Same = true;
    for (size_t I = 0; I < IDs.size(); ++I)
      Same &= A.Sources[I].SourceID == IDs[I];
    if (Same)
      return json(node_behavior::summary(A, State->Revision));
  }
  if (State->PackageBehaviors.size() >= MaxCachedPackageBehaviors)
    throw Error("package_behavior_cache_budget_exceeded");
  std::vector<PackageBehaviorInput> Inputs;
  std::vector<NodeBehavior> Temporary;
  Temporary.reserve(IDs.size());
  uint64_t SelectedBytes = 0;
  for (const auto &ID : IDs) {
    const auto S = State->Sources.find(ID);
    if (S == State->Sources.end())
      throw Error("unknown_source");
    const auto Size =
        State
            ->sourceBytes(S->second.ArtifactID, MaxJavaScriptBytes,
                          "package_behavior_source_byte_budget_exceeded")
            .size();
    if (Size > MaxPackageBehaviorBytes - SelectedBytes)
      throw Error("package_behavior_selection_budget_exceeded");
    SelectedBytes += Size;
    const auto Cached = State->NodeBehaviors.find(ID);
    if (Cached != State->NodeBehaviors.end()) {
      Inputs.push_back({&S->second, &Cached->second, Size});
    } else {
      Temporary.push_back(web::analyzeNodeBehavior(S->second));
      Inputs.push_back({&S->second, &Temporary.back(), Size});
    }
  }
  // Build retained typed ancestry once. No caller-supplied origin JSON or
  // filename heuristics can add a delivery relation.
  std::vector<PackageBehaviorOrigin> Origins;
  auto Add = [&](const std::string &Child, const std::string &Parent,
                 const std::string &Evidence, const char *Kind) {
    if (Origins.size() >= MaxPackageRecords)
      throw Error("package_behavior_inventory_budget_exceeded");
    Origins.push_back({Child, Parent, Evidence, Kind});
  };
  for (const auto &[ID, E] : State->BunExtractions) {
    for (const auto &M : E.Modules)
      if (!M.SourceArtifactID.empty())
        Add(M.SourceArtifactID, E.ArtifactID, M.ID, "bun_source");
    for (const auto &R : E.Regions)
      if (R.Kind == "asset")
        Add(R.ID, E.ArtifactID, R.ID, "bun_asset");
  }
  for (const auto &[ID, E] : State->SEAExtractions)
    for (const auto &R : E.Regions)
      if (R.selectable())
        Add(R.ID, E.ArtifactID, R.ID, "sea_region");
  for (const auto &[ID, E] : State->NexeExtractions) {
    for (const auto &M : E.Members)
      Add(M.ID, E.ArtifactID, M.ID, "nexe_member");
    for (const auto &R : E.Regions)
      if (R.selectable())
        Add(R.ID, E.ArtifactID, R.ID, "nexe_bootstrap");
  }
  auto A = web::analyzePackageBehavior(P->second, Inputs, Origins);
  auto Reply = json(node_behavior::summary(A, State->Revision));
  const auto ID = A.ID;
  State->PackageBehaviors.emplace(ID, std::move(A));
  return Reply;
#endif
}

std::string Session::packageBehaviorRecords(std::string_view Revision,
                                            std::string_view BehaviorID,
                                            std::string_view Kind,
                                            uint64_t Offset,
                                            uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->PackageBehaviors.find(std::string(BehaviorID));
  if (Found == State->PackageBehaviors.end())
    throw Error("unknown_package_behavior");
  const auto &A = Found->second;
  const auto Total = Kind == "sources"   ? A.Sources.size()
                     : Kind == "records" ? A.RecordCount
                                         : throw Error("invalid_record_kind");
  if (!Limit || Limit > 512 || Offset > Total)
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Total, Offset + Limit);
  llvm::json::Array Items;
  if (Kind == "sources") {
    for (auto I = Offset; I < End; ++I)
      Items.emplace_back(node_behavior::source(A.Sources[I]));
  } else {
    uint64_t Base = 0;
    for (const auto &S : A.Sources) {
      const auto Next = Base + S.Records.size();
      if (Offset < Next && End > Base)
        for (auto I = std::max(Offset, Base) - Base;
             I < std::min(End, Next) - Base; ++I) {
          auto Row =
              node_behavior::record(S.Records[I], S.SourceID, S.ArtifactID);
          Row["file_evidence_id"] = S.FileID;
          Row["node_behavior_id"] = S.BehaviorID;
          Items.emplace_back(std::move(Row));
        }
      Base = Next;
    }
  }
  auto Reply = node_behavior::summary(A, State->Revision);
  Reply["items"] = std::move(Items);
  Reply["record_kind"] = std::string(Kind);
  Reply["offset"] = Offset;
  Reply["next_offset"] =
      End == Total ? llvm::json::Value(nullptr) : llvm::json::Value(End);
  Reply["page_complete"] = End == Total;
  return json(std::move(Reply));
#endif
}
} // namespace neverd::web
