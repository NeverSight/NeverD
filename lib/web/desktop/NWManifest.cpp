//===- NWManifest.cpp - NW.js manifest declarations -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// NW.js entry candidates without URL, command-line or runtime interpretation.
///
//===----------------------------------------------------------------------===//

#include "ManifestInternal.h"

namespace neverd::web::desktop {
void readNWManifest(DesktopManifest &A, const ManifestContext &Context,
                    const llvm::json::Object &Object) {
  for (const auto *Key : {"name", "version", "nodejs"})
    A.Declarations[Key] = shape(Object.get(Key));
  for (const auto *Key :
       {"main", "node-main", "bg-script", "inject_js_start", "inject_js_end"})
    A.Entries.push_back(Context.entry(Key, Object.get(Key),
                                      std::string_view(Key) == "node-main"));
  A.Declarations["required_field_shapes"] =
      A.Declarations["name"] == "string" &&
              A.Entries.front().ValueStatus == "string"
          ? "present"
          : "incomplete";
  if (!Context.DirectoryNamespace)
    return;
  const auto &Path = Context.Manifest->MemberPath;
  if (Path == "package.json")
    A.Layout = "manifest_root_candidate";
  else if (Path == "Contents/Resources/app.nw/package.json" ||
           Path.ends_with("/Contents/Resources/app.nw/package.json"))
    A.Layout = "macos_app_nw_directory_candidate";
  else if (Path == "package.nw/package.json" || Path == "app.nw/package.json")
    A.Layout = "nw_directory_candidate";
  else
    A.Layout = "unrecognized_placement";
}
} // namespace neverd::web::desktop
