//===- ElectronIPC.h - Scoped Electron IPC evidence --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Scoped Electron IPC evidence.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Electron.h"

#include <array>
#include <span>

namespace neverd::web {
inline constexpr std::string_view ElectronIPCProfile =
    "electron-scoped-channel-candidates-v1";
inline constexpr uint64_t MaxElectronIPCSources = 16;
inline constexpr uint64_t MaxElectronIPCEndpoints = 10000;
inline constexpr uint64_t MaxElectronIPCUnits = 1024 * 1024;
inline constexpr uint64_t MaxElectronIPCSteps = 2000000;
inline constexpr std::array<std::string_view, 8> ElectronIPCKinds = {
    "ipc_main_handle",   "ipc_renderer_invoke",  "ipc_main_listen",
    "ipc_renderer_send", "ipc_webcontents_send", "ipc_renderer_listen",
    "ipc_main_remove",   "ipc_renderer_remove"};

struct ElectronIPCInput {
  const SourceAnalysis *Source = nullptr;
  const ElectronSource *Evidence = nullptr;
};
struct ElectronIPCSource {
  std::string SourceID, ArtifactID, EvidenceID, Status, Reason;
};
struct ElectronIPCEndpoint {
  std::string ID;
  uint32_t Source = 0, Boundary = 0, Kind = 0;
  uint32_t Channel = NoSourceIndex;
};
struct ElectronIPCChannel {
  std::string ID;
  std::array<uint64_t, ElectronIPCKinds.size()> Counts{};
};
struct ElectronIPC {
  std::string ID, NamespaceID, ManifestID, MainArtifactID;
  std::vector<ElectronIPCSource> Sources;
  std::vector<ElectronIPCEndpoint> Endpoints;
  std::vector<ElectronIPCChannel> Channels;
  uint64_t Steps = 0, UnresolvedEndpoints = 0, UnavailableSources = 0;
};

/// Explicit selection only. Every source must be an exact captured member in
/// the manifest's directory subtree; the declared main candidate is required.
/// Constant channel equality is exact UTF-16 equality, without text or isolated
/// channel hashes in the result. Counts describe possible pairs, never runtime
/// routing, execution order, process roles or application completeness.
/// Invalid evidence and budgets fail atomically with a fixed Error code.
ElectronIPC correlateElectronIPC(const Snapshot &Namespace,
                                 const ElectronManifest &Manifest,
                                 std::span<const ElectronIPCInput> Sources);
} // namespace neverd::web
