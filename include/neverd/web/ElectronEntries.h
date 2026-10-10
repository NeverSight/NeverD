#pragma once

#include "neverd/web/ElectronIPC.h"
#include "neverd/web/SourcePaths.h"

namespace neverd::web {
inline constexpr std::string_view ElectronEntryProfile =
    "electron-captured-entry-candidates-v1";
inline constexpr uint64_t MaxElectronEntrySteps = 4000000;
struct ElectronEntryInput {
  const SourceAnalysis *Source = nullptr;
  const ElectronSource *Evidence = nullptr;
  const SourceBindingAnalysis *Bindings = nullptr;
  const SourceModuleAnalysis *Modules = nullptr;
  const SourceOrigins *Origins = nullptr;
  const SourceValueAnalysis *Values = nullptr;
};
struct ElectronEntrySource {
  std::string SourceID, ArtifactID, EvidenceID, PathID, PathStatus, PathReason;
};
struct ElectronEntry {
  std::string ID, Status, TargetArtifactID, TargetSourceID;
  uint32_t Source = 0, Boundary = 0;
  uint32_t RootNode = NoSourceIndex, OperationNode = NoSourceIndex;
};
struct ElectronEntries {
  std::string ID, NamespaceID, ManifestID, MainArtifactID;
  std::vector<ElectronEntrySource> Sources;
  std::vector<ElectronEntry> Entries;
  uint64_t Steps = 0, LinkedFiles = 0, UnavailablePaths = 0;
};
/// Uses the same explicit source selection as IPC; only compares exact
/// namespace members. No target read, execution, import closure or automatic
/// HTML/JS analysis. A linked occurrence remains an unverified runtime entry.
ElectronEntries
associateElectronEntries(const Snapshot &Namespace,
                         const ElectronManifest &Manifest,
                         std::span<const ElectronEntryInput> Input);
} // namespace neverd::web
