//===- ManifestInternal.h - Captured manifest comparison --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared bounded namespace admission; framework readers own field meaning.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_WEB_DESKTOP_MANIFESTINTERNAL_H
#define NEVERD_WEB_DESKTOP_MANIFESTINTERNAL_H

#include "neverd/web/DesktopManifest.h"

#include "llvm/Support/JSON.h"

namespace neverd::web::desktop {
class ManifestContext {
  std::map<std::string_view, const Artifact *, std::less<>> Files;
  std::string ParentPath;

public:
  const Artifact *Manifest = nullptr;
  bool DirectoryNamespace = false;
  explicit ManifestContext(const Snapshot &Input, std::string_view ArtifactID);
  DesktopEntry entry(std::string_view Field, const llvm::json::Value *Value,
                     bool CommandLine = false) const;
};

/// Reports only shape; string contents and object/array keys remain private.
std::string shape(const llvm::json::Value *Value);
void readNWManifest(DesktopManifest &Result, const ManifestContext &Context,
                    const llvm::json::Object &Object);
void readVSIXManifest(DesktopManifest &Result, const ManifestContext &Context,
                      const llvm::json::Object &Object);
} // namespace neverd::web::desktop

#endif // NEVERD_WEB_DESKTOP_MANIFESTINTERNAL_H
