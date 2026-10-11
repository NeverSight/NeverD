//===- DesktopManifest.h - Desktop manifest evidence ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded NW.js and VS Code extension declarations and captured entry files.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_WEB_DESKTOPMANIFEST_H
#define NEVERD_WEB_DESKTOPMANIFEST_H

#include "neverd/web/Artifact.h"

#include <map>

namespace neverd::web {
inline constexpr std::string_view NWManifestProfile =
    "nwjs-manifest-file-candidates-v1";
inline constexpr std::string_view VSIXManifestProfile =
    "vscode-extension-manifest-file-candidates-v1";
inline constexpr uint64_t MaxDesktopManifestBytes = 1024 * 1024;

struct DesktopEntry {
  std::string ID, Field, ValueStatus, LinkStatus, ArtifactID;
  std::string ContentKind = "unknown";
};

struct DesktopManifest {
  std::string ID, ArtifactID, Profile, Framework, Layout;
  // Keys and values are fixed vocabularies, never target-derived text.
  std::map<std::string, std::string> Declarations;
  std::vector<DesktopEntry> Entries;
};

/// Select nwjs or vsix explicitly. Namespace must contain only admitted bytes
/// belonging to the same captured root/container. No runtime resolution,
/// installation, framework/version authentication or remote access occurs.
DesktopManifest analyzeDesktopManifest(const Snapshot &Namespace,
                                       std::string_view ArtifactID,
                                       std::string_view Bytes,
                                       std::string_view Kind);
} // namespace neverd::web

#endif // NEVERD_WEB_DESKTOPMANIFEST_H
