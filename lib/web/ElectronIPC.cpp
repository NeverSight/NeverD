#include "neverd/web/ElectronIPC.h"

#include "ElectronSelection.h"

#include "neverd/web/Session.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd::web {
ElectronIPC correlateElectronIPC(const Snapshot &Namespace,
                                 const ElectronManifest &Manifest,
                                 std::span<const ElectronIPCInput> Input) {
  const auto Selection = selectElectronSources(Namespace, Manifest, Input);
  const auto &Sources = Selection.Sources;
  ElectronIPC A;
  A.Steps = Selection.Steps;
  auto Step = [&](uint64_t N = 1) {
    if (N > MaxElectronIPCSteps - A.Steps)
      throw Error("electron_ipc_work_budget_exceeded");
    A.Steps += N;
  };
  std::vector<std::string_view> Identity{Namespace.ID, Manifest.ID,
                                         ElectronIPCProfile};
  for (const auto &I : Sources) {
    const auto &S = *I.Source;
    const auto &E = *I.Evidence;
    if (E.Status != "partial")
      ++A.UnavailableSources;
    A.Sources.push_back({S.ID, S.ArtifactID, E.ID, E.Status, E.Reason});
    Identity.push_back(S.ID);
    Identity.push_back(E.ID);
  }
  A.ID = identity("electron-ipc", Identity);
  A.NamespaceID = Namespace.ID;
  A.ManifestID = Manifest.ID;
  A.MainArtifactID = Manifest.MainArtifactID;
  // A comparison charges every examined UTF-16 code unit. Neither a hash nor
  // a normalized spelling is substituted for channel equality.
  const auto Less = [&](std::u16string_view L, std::u16string_view R) {
    for (size_t I = 0, End = std::min(L.size(), R.size()); I < End; ++I) {
      Step();
      if (L[I] != R[I])
        return L[I] < R[I];
    }
    Step();
    return L.size() < R.size();
  };
  std::map<std::u16string_view, uint32_t, decltype(Less)> Channels(Less);
  uint64_t Units = 0, Boundaries = 0;
  for (uint32_t SI = 0; SI < Sources.size(); ++SI) {
    const auto &S = *Sources[SI].Source;
    const auto &E = *Sources[SI].Evidence;
    if (E.Boundaries.size() > MaxElectronIPCEndpoints - Boundaries)
      throw Error("electron_ipc_record_budget_exceeded");
    Boundaries += E.Boundaries.size();
    std::set<std::string_view> Seen;
    for (uint32_t BI = 0; BI < E.Boundaries.size(); ++BI) {
      Step();
      const auto &B = E.Boundaries[BI];
      if (B.Node >= S.Nodes.size() ||
          (B.ValueNode != NoSourceIndex && B.ValueNode >= S.Nodes.size()) ||
          !Seen.insert(B.ID).second ||
          B.ID !=
              identity("electron-boundary", {E.ID, S.Nodes[B.Node].ID, B.Kind}))
        throw Error("electron_ipc_invalid_evidence");
      const auto Kind =
          std::find(ElectronIPCKinds.begin(), ElectronIPCKinds.end(), B.Kind);
      if (Kind == ElectronIPCKinds.end())
        continue;
      ElectronIPCEndpoint Endpoint;
      Endpoint.Source = SI;
      Endpoint.Boundary = BI;
      Endpoint.Kind = Kind - ElectronIPCKinds.begin();
      Endpoint.ID = identity("electron-ipc-endpoint", {A.ID, B.ID});
      if (B.ValueStatus == "constant_string") {
        if (B.ValueNode == NoSourceIndex)
          throw Error("electron_ipc_invalid_evidence");
        if (B.Text.size() > MaxElectronIPCUnits - Units)
          throw Error("electron_ipc_string_budget_exceeded");
        Units += B.Text.size();
        const auto [Position, New] = Channels.emplace(B.Text, Channels.size());
        Endpoint.Channel = Position->second;
        if (New) {
          ElectronIPCChannel C;
          const auto Ordinal = std::to_string(Endpoint.Channel);
          C.ID = identity("electron-ipc-channel", {A.ID, Ordinal});
          A.Channels.push_back(std::move(C));
        }
        ++A.Channels[Endpoint.Channel].Counts[Endpoint.Kind];
      } else {
        ++A.UnresolvedEndpoints;
      }
      A.Endpoints.push_back(std::move(Endpoint));
    }
  }
  return A;
}
} // namespace neverd::web
