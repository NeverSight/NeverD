//===- Manifest.cpp - Bounded desktop manifest admission ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Immutable manifest identity and exact captured-file comparison.
///
//===----------------------------------------------------------------------===//

#include "../Internal.h"
#include "../JsonReader.h"
#include "ManifestInternal.h"

#include <algorithm>
#include <set>

namespace neverd::web::desktop {
std::string shape(const llvm::json::Value *V) {
  if (!V)
    return "absent";
  if (const auto S = V->getAsString())
    return S->empty() ? "empty_string" : "string";
  if (const auto B = V->getAsBoolean())
    return *B ? "true" : "false";
  if (V->getAsNull())
    return "null";
  if (V->getAsObject())
    return "object";
  if (V->getAsArray())
    return "array";
  return "number";
}

ManifestContext::ManifestContext(const Snapshot &Input,
                                 std::string_view ArtifactID) {
  if (Input.Artifacts.size() > Limits::HardEntries + 1)
    throw Error("desktop_manifest_namespace_budget_exceeded");
  DirectoryNamespace =
      !Input.Artifacts.empty() && Input.Artifacts.front().Directory;
  uint64_t Bytes = 0;
  std::set<std::string_view> IDs;
  for (const auto &A : Input.Artifacts) {
    if (A.ID.size() > 64 || A.MemberPath.size() > 4096 ||
        A.MemberPath.size() > 8 * 1024 * 1024 - Bytes)
      throw Error("desktop_manifest_namespace_budget_exceeded");
    Bytes += A.MemberPath.size();
    if (!IDs.insert(A.ID).second ||
        (!A.MemberPath.empty() && !Files.emplace(A.MemberPath, &A).second))
      throw Error("desktop_manifest_ambiguous_namespace");
    if (A.ID == ArtifactID)
      Manifest = &A;
  }
  if (!Manifest || Manifest->Directory)
    throw Error("unknown_manifest_artifact");
  if (!DirectoryNamespace)
    return;
  const auto Slash = Manifest->MemberPath.rfind('/');
  const auto Name = std::string_view(Manifest->MemberPath)
                        .substr(Slash == std::string::npos ? 0 : Slash + 1);
  if (Name != "package.json")
    throw Error("desktop_manifest_name_not_in_profile");
  if (Slash != std::string::npos)
    ParentPath = Manifest->MemberPath.substr(0, Slash + 1);
}

DesktopEntry ManifestContext::entry(std::string_view Field,
                                    const llvm::json::Value *Value,
                                    bool CommandLine) const {
  DesktopEntry E;
  E.Field = Field;
  E.ValueStatus = shape(Value);
  if (E.ValueStatus != "string") {
    E.LinkStatus = E.ValueStatus == "absent"         ? "not_declared"
                   : E.ValueStatus == "empty_string" ? "empty_entry"
                                                     : "unsupported_entry_type";
    return E;
  }
  auto Path = Value->getAsString()->str();
  if (Path.size() > 4096) {
    E.LinkStatus = "path_budget_exceeded";
    return E;
  }
  if (CommandLine && (Path.starts_with('-') ||
                      Path.find_first_of(" \t\r\n\"'") != std::string::npos)) {
    E.LinkStatus = "command_line_syntax_not_analyzed";
    return E;
  }
  // Do not confuse URL decoding, fragments or normalization with a literal
  // filename. The narrower profile is also conservative for path-only fields.
  if (Path.find_first_of(":%?#") != std::string::npos ||
      std::any_of(Path.begin(), Path.end(),
                  [](unsigned char C) { return C <= 0x20 || C == 0x7f; })) {
    E.LinkStatus = "url_or_path_syntax_not_analyzed";
    return E;
  }
  if (Path.starts_with("./"))
    Path.erase(0, 2);
  if (Path.empty() || Path.front() == '/' || Path.back() == '/' ||
      Path.find('\\') != std::string::npos) {
    E.LinkStatus = "unsupported_path_syntax";
    return E;
  }
  std::string_view Remaining = Path;
  while (!Remaining.empty()) {
    const auto Slash = Remaining.find('/');
    const auto Part = Remaining.substr(0, Slash);
    try {
      validateMemberName(Part);
    } catch (const Error &) {
      E.LinkStatus = "unsafe_entry_path";
      return E;
    }
    if (Slash == std::string::npos)
      break;
    Remaining.remove_prefix(Slash + 1);
  }
  if (!DirectoryNamespace) {
    E.LinkStatus = "no_directory_namespace";
    return E;
  }
  const auto It = Files.find(ParentPath + Path);
  if (It == Files.end()) {
    E.LinkStatus = "no_exact_admitted_file";
    return E;
  }
  if (It->second->Directory) {
    E.LinkStatus = "directory_requires_runtime_resolution";
    return E;
  }
  E.LinkStatus = "exact_admitted_file_candidate";
  E.ArtifactID = It->second->ID;
  if (Path.ends_with(".js") || Path.ends_with(".mjs") || Path.ends_with(".cjs"))
    E.ContentKind = "javascript_filename_candidate";
  else if (Path.ends_with(".html") || Path.ends_with(".htm"))
    E.ContentKind = "html_filename_candidate";
  return E;
}
} // namespace neverd::web::desktop

namespace neverd::web {
DesktopManifest analyzeDesktopManifest(const Snapshot &Input,
                                       std::string_view ArtifactID,
                                       std::string_view Bytes,
                                       std::string_view Kind) {
  if (Kind != "nwjs" && Kind != "vsix")
    throw Error("unsupported_desktop_manifest_profile");
  if (Bytes.size() > MaxDesktopManifestBytes)
    throw Error("desktop_manifest_byte_budget_exceeded");
  const desktop::ManifestContext Context(Input, ArtifactID);
  const auto Hash = sha256(Bytes);
  if (Hash != Context.Manifest->BlobHash)
    throw Error("desktop_manifest_hash_mismatch");
  const auto Parsed =
      parseBoundedJSON(Bytes, {MaxDesktopManifestBytes, 32, 20000, 65536});
  const auto *Object = Parsed.getAsObject();
  if (!Object)
    throw Error("desktop_manifest_not_object");
  DesktopManifest A;
  A.ArtifactID = ArtifactID;
  A.Framework = Kind == "nwjs" ? "nwjs" : "vscode_extension";
  A.Profile = Kind == "nwjs" ? NWManifestProfile : VSIXManifestProfile;
  A.ID = identity("desktop-manifest", {Input.ID, ArtifactID, Hash, A.Profile});
  A.Layout = "not_observed";
  if (Kind == "nwjs")
    desktop::readNWManifest(A, Context, *Object);
  else
    desktop::readVSIXManifest(A, Context, *Object);
  for (auto &E : A.Entries)
    E.ID = identity("desktop-entry", {A.ID, E.Field});
  return A;
}
} // namespace neverd::web
