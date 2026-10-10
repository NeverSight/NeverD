//===- Electron.h - Electron manifest evidence -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Electron manifest evidence.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/SourceOrigins.h"
#include "neverd/web/SourceValues.h"

namespace neverd::web {
inline constexpr std::string_view ElectronSourceProfile =
    "electron-static-boundaries-v1";
inline constexpr uint64_t MaxElectronRecords = 10000;
inline constexpr uint64_t MaxElectronSteps = 2000000;
inline constexpr std::string_view ElectronManifestProfile =
    "electron-40-package-entry-candidates-v1";
inline constexpr uint64_t MaxElectronManifestBytes = 1024 * 1024;

struct ElectronManifest {
  std::string ID, ArtifactID, MainValueStatus, MainLinkStatus, MainArtifactID;
  std::string SourceType;
  bool NamePresent = false, VersionPresent = false, ProductNamePresent = false;
  bool ElectronDependencyPresent = false;
};
/// Caller selects the profile and manifest. Only exact admitted paths within
/// its directory are compared; no framework detection or runtime resolution.
ElectronManifest analyzeElectronManifest(const Snapshot &Namespace,
                                         std::string_view ArtifactID,
                                         std::string_view Bytes);

struct ElectronBoundary {
  std::string ID, Kind, OriginEvidence;
  uint32_t Node = NoSourceIndex, Request = NoSourceIndex;
  uint32_t ValueNode = NoSourceIndex, CallbackNode = NoSourceIndex;
  uint32_t ConstructionNode = NoSourceIndex;
  bool Optional = false;
  // Private constants for subsequent explicit application-scoped correlation.
  // No global channel hash or raw text is exposed through ordinary metadata.
  std::string ValueStatus = "absent";
  std::u16string Text;
};
struct ElectronSource {
  std::string ID, SourceID, BindingID, ModuleID, OriginID, ValueID;
  std::string Status, Reason;
  std::string OriginStatus, OriginReason, ValueStatus;
  std::vector<ElectronBoundary> Boundaries;
  uint64_t Steps = 0, StringUnits = 0;
};

/// Identifies source-visible candidates rooted in explicit electron module
/// syntax. No application/framework authentication, execution, runtime callee
/// proof, environment defaults, permissions or exploitability inference.
ElectronSource analyzeElectronSource(const SourceAnalysis &Source,
                                     const SourceModuleAnalysis &Modules,
                                     const SourceOrigins &Origins,
                                     const SourceValueAnalysis &Values);
} // namespace neverd::web
