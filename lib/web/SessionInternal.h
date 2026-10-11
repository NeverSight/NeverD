//===- SessionInternal.h - Private session state and caches ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private session state and caches.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "ArtifactView.h"
#include "BunSourceMap.h"
#include "Internal.h"

#include "neverd/web/Asar.h"
#include "neverd/web/Bun.h"
#include "neverd/web/DesktopManifest.h"
#include "neverd/web/Electron.h"
#include "neverd/web/ElectronEntries.h"
#include "neverd/web/ElectronIPC.h"
#include "neverd/web/HTML.h"
#include "neverd/web/Interfaces.h"
#include "neverd/web/PackageArchive.h"
#include "neverd/web/PackageIntegrity.h"
#include "neverd/web/Packages.h"
#include "neverd/web/SEA.h"
#include "neverd/web/Session.h"
#include "neverd/web/Source.h"
#include "neverd/web/SourceBindings.h"
#include "neverd/web/SourceBundles.h"
#include "neverd/web/SourceEffects.h"
#include "neverd/web/SourceMap.h"
#include "neverd/web/SourceModules.h"
#include "neverd/web/SourceNavigation.h"
#include "neverd/web/SourceView.h"
#include "neverd/web/Streams.h"

#include <map>
#include <mutex>

namespace neverd::web {
struct Session::Impl {
  mutable std::mutex Mutex;
  Snapshot Published;
  std::string CandidateID;
  Limits Budget;
  std::string Path;
  std::string Token;
  uint64_t Revision = 0;
  uint64_t PreviewSequence = 0;
  std::map<std::string, SourceAnalysis> Sources;
  std::map<std::string, SourceBindingAnalysis> Bindings;
  std::map<std::string, SourceNavigation> Navigation;
  struct SemanticResults {
    SourceValueAnalysis Values;
    SourceEffectAnalysis Effects;
  };
  std::map<std::string, SemanticResults> Semantics;
  struct ModuleResults {
    SourceModuleAnalysis Evidence;
    SourceModuleLinks Links;
  };
  std::map<std::string, ModuleResults> Modules;
  std::map<std::string, SourceBundleAnalysis> Bundles;
  std::map<std::string, BunExtraction> BunExtractions;
  std::map<std::string, SEAExtraction> SEAExtractions;
  std::map<std::string, AsarExtraction> AsarExtractions;
  std::map<std::string, ElectronManifest> ElectronManifests;
  std::map<std::pair<std::string, std::string>, DesktopManifest>
      DesktopManifests;
  std::map<std::string, ElectronSource> ElectronSources;
  std::map<std::string, ElectronIPC> ElectronIPCs;
  std::map<std::string, ElectronEntries> ElectronEntryAnalyses;
  std::map<std::string, PackageAnalysis> PackageAnalyses;
  std::map<std::string, PackageDiff> PackageDiffs;
  std::map<std::string, PackageArchive> PackageArchives;
  std::map<std::string, PackageIntegrityResult> PackageIntegrity;
  std::map<std::string, HARCapture> HARCaptures;
  std::map<std::string, StreamCapture> StreamCaptures;
  std::optional<StreamCapture> PendingStream;
  std::string StreamPreviewToken;
  uint64_t StreamPreviewSequence = 0;
  std::optional<HARCapture> PendingHAR;
  std::string HARPreviewToken;
  uint64_t HARPreviewSequence = 0;
  std::map<std::string, SourceInterfaces> InterfaceSources;
  std::map<std::string, InterfaceCorrelation> InterfaceCorrelations;
  uint64_t CachedArchiveBytes = 0;
  struct HTMLResults {
    HTMLDocument Document;
    HTMLLinks Links;
    HTMLImportMaps ImportMaps;
  };
  std::map<std::string, HTMLResults> HTMLDocuments;
  struct HTMLSourceSelection {
    const HTMLResults *Analysis;
    uint32_t Script;
  };
  std::optional<HTMLSourceSelection>
  htmlSource(std::string_view SelectionID) const;
  std::optional<ArtifactView> artifactView(std::string_view SelectionID) const;
  std::optional<Snapshot> memberNamespace(std::string_view SelectionID) const;
  uint64_t CachedNodes = 0;
  uint64_t CachedLexemes = 0;
  std::map<std::string, SourceMap> Maps;
  uint64_t CachedMapSegments = 0;
  // At most two published source views and one metadata-only pending view.
  // Replacing one source's policy revokes its previous view ID.
  std::map<std::string, SourceView> SourceViews;
  std::optional<SourceView> PendingSourceView;
  std::string SourceViewToken;
  uint64_t SourceViewSequence = 0;

  const char *analysisStatus() const {
    return Sources.empty() && Maps.empty() && BunExtractions.empty() &&
                   SEAExtractions.empty() && AsarExtractions.empty() &&
                   ElectronManifests.empty() && DesktopManifests.empty() &&
                   HTMLDocuments.empty() && PackageAnalyses.empty() &&
                   PackageArchives.empty() && PackageIntegrity.empty() &&
                   HARCaptures.empty() && InterfaceSources.empty() &&
                   StreamCaptures.empty()
               ? "not_analyzed"
               : "partial";
  }

  void requireRevision(std::string_view Expected) const {
    if (!Revision)
      throw Error("no_project");
    if (Expected != std::to_string(Revision))
      throw Error("stale_revision");
  }
  std::string sourceBytes(std::string_view ArtifactID, uint64_t MaxBytes,
                          const char *LimitError) const {
    if (auto View = artifactView(ArtifactID)) {
      if (View->Content.size() > MaxBytes)
        throw Error(LimitError);
      return View->Content.read(0, View->Content.size(), MaxBytes);
    }
    for (const auto &[ID, E] : BunExtractions)
      for (const auto &M : E.Modules)
        if (M.SourceArtifactID == ArtifactID && !M.SourceArtifactID.empty())
          return bunSourceBytes(E, M, MaxBytes);
    for (const auto &[ID, Map] : Maps)
      for (const auto &Source : Map.Sources)
        if (Source.ID == ArtifactID) {
          if (!Source.Contents)
            throw Error("source_content_not_supplied");
          if (Source.Contents->size() > MaxBytes)
            throw Error(LimitError);
          return *Source.Contents;
        }
    throw Error("unknown_artifact");
  }
};
} // namespace neverd::web
