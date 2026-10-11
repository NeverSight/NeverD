//===- VSIXManifest.cpp - VS Code extension declarations ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Extension identity and entry evidence kept separate from embedded helpers.
///
//===----------------------------------------------------------------------===//

#include "ManifestInternal.h"

namespace neverd::web::desktop {
void readVSIXManifest(DesktopManifest &A, const ManifestContext &Context,
                      const llvm::json::Object &Object) {
  for (const auto *Key : {"name", "publisher", "version", "engines",
                          "extensionKind", "activationEvents", "contributes"})
    A.Declarations[Key] = shape(Object.get(Key));
  const auto *Engines = Object.getObject("engines");
  A.Declarations["engines.vscode"] =
      Engines ? shape(Engines->get("vscode")) : "not_available";
  A.Declarations["required_field_shapes"] =
      A.Declarations["name"] == "string" &&
              A.Declarations["version"] == "string" &&
              A.Declarations["publisher"] == "string" &&
              A.Declarations["engines.vscode"] == "string"
          ? "present"
          : "incomplete";
  for (const auto *Key : {"main", "browser"})
    A.Entries.push_back(Context.entry(Key, Object.get(Key)));
  if (!Context.DirectoryNamespace)
    return;
  const auto &Path = Context.Manifest->MemberPath;
  if (Path == "extension/package.json")
    A.Layout = "vsix_extension_path_candidate";
  else if (Path == "package.json")
    A.Layout = "extension_root_candidate";
  else
    A.Layout = "unrecognized_placement";
}
} // namespace neverd::web::desktop
