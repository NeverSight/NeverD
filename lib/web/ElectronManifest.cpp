#include "Internal.h"
#include "JsonReader.h"

#include "neverd/web/Electron.h"

namespace neverd::web {
ElectronManifest analyzeElectronManifest(const Snapshot &Input,
                                         std::string_view ArtifactID,
                                         std::string_view Bytes) {
  if (Input.Artifacts.size() > Limits::HardEntries + 1)
    throw Error("electron_manifest_namespace_budget_exceeded");
  const Artifact *Manifest = nullptr;
  for (const auto &Entry : Input.Artifacts)
    if (Entry.ID == ArtifactID) {
      if (Manifest)
        throw Error("electron_manifest_ambiguous_namespace");
      Manifest = &Entry;
    }
  if (!Manifest || Manifest->Directory)
    throw Error("unknown_manifest_artifact");
  if (Bytes.size() > MaxElectronManifestBytes)
    throw Error("electron_manifest_budget_exceeded");
  const auto Hash = sha256(Bytes);
  if (Manifest->BlobHash != Hash)
    throw Error("electron_manifest_hash_mismatch");
  const auto JSON =
      parseBoundedJSON(Bytes, {MaxElectronManifestBytes, 32, 20000, 4096});
  const auto *Object = JSON.getAsObject();
  if (!Object)
    throw Error("electron_manifest_not_object");
  ElectronManifest A;
  A.ArtifactID = ArtifactID;
  A.ID = identity("electron-manifest",
                  {Input.ID, ArtifactID, Hash, ElectronManifestProfile});
  A.NamePresent = Object->get("name") != nullptr;
  A.VersionPresent = Object->get("version") != nullptr;
  A.ProductNamePresent = Object->get("productName") != nullptr;
  for (const auto Key :
       {"dependencies", "devDependencies", "optionalDependencies"})
    if (const auto *Dependencies = Object->getObject(Key))
      A.ElectronDependencyPresent |= Dependencies->get("electron") != nullptr;
  std::string Main;
  const auto *Value = Object->get("main");
  const bool Default = !Value || Value->getAsNull().has_value() ||
                       Value->getAsBoolean() == false ||
                       Value->getAsNumber() == 0 || Value->getAsString() == "";
  if (Default) {
    Main = "index.js";
    A.MainValueStatus = "default_index_js";
  } else if (const auto Text = Value->getAsString()) {
    Main = Text->str();
    A.MainValueStatus = "declared_string";
  } else {
    A.MainValueStatus = "unsupported_main_type";
    A.MainLinkStatus = "not_compared";
    A.SourceType = "unknown";
    return A;
  }
  A.SourceType =
      Main.ends_with(".mjs") ||
              (Object->getString("type") == "module" && !Main.ends_with(".cjs"))
          ? "module"
          : "commonjs";
  if (Main.starts_with("./"))
    Main.erase(0, 2);
  if (Main.empty() || Main.starts_with('/') ||
      Main.find('\\') != std::string::npos) {
    A.MainLinkStatus = "unsupported_path_syntax";
    return A;
  }
  std::string_view Remaining = Main;
  while (!Remaining.empty()) {
    const auto Slash = Remaining.find('/');
    const auto Part = Remaining.substr(0, Slash);
    if (Part == "." || Part == "..") {
      A.MainLinkStatus = "outside_manifest_path_profile";
      return A;
    }
    try {
      validateMemberName(Part);
    } catch (const Error &) {
      A.MainLinkStatus = "unsafe_entry_path";
      return A;
    }
    if (Slash == std::string_view::npos)
      break;
    Remaining.remove_prefix(Slash + 1);
    if (Remaining.empty()) {
      A.MainLinkStatus = "unsupported_path_syntax";
      return A;
    }
  }
  if (Manifest->MemberPath.empty() || Input.Artifacts.empty() ||
      !Input.Artifacts[0].Directory) {
    A.MainLinkStatus = "no_directory_namespace";
    return A;
  }
  const auto Slash = Manifest->MemberPath.rfind('/');
  const auto Target =
      (Slash == std::string::npos ? std::string()
                                  : Manifest->MemberPath.substr(0, Slash + 1)) +
      Main;
  A.MainLinkStatus = "no_exact_admitted_file";
  bool Found = false;
  for (const auto &Entry : Input.Artifacts)
    if (Entry.MemberPath == Target) {
      if (Found)
        throw Error("electron_manifest_ambiguous_namespace");
      Found = true;
      if (Entry.Directory) {
        A.MainLinkStatus = "directory_requires_runtime_resolution";
      } else {
        A.MainArtifactID = Entry.ID;
        A.MainLinkStatus = "exact_admitted_file_candidate";
      }
    }
  return A;
}
} // namespace neverd::web
