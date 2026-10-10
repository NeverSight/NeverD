//===- PackageReader.cpp - Bounded Node package metadata ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded Node package metadata.
///
//===----------------------------------------------------------------------===//

#include "../JsonReader.h"

#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"
#include "neverd/web/Packages.h"

#include <algorithm>

namespace neverd::web {
namespace {
using Object = llvm::json::Object;
using Value = llvm::json::Value;

std::optional<std::string> text(const Object &O, llvm::StringRef Key) {
  const auto *V = O.get(Key);
  if (!V)
    return {};
  const auto S = V->getAsString();
  if (!S)
    throw Error("package_invalid_field_type");
  return S->str();
}
bool flag(const Object &O, llvm::StringRef Key) {
  const auto *V = O.get(Key);
  if (!V)
    return false;
  const auto B = V->getAsBoolean();
  if (!B)
    throw Error("package_invalid_field_type");
  return *B;
}
const Object *object(const Object &O, llvm::StringRef Key) {
  const auto *V = O.get(Key);
  if (!V)
    return nullptr;
  const auto *R = V->getAsObject();
  if (!R)
    throw Error("package_invalid_field_type");
  return R;
}
std::vector<std::string> strings(const Object &O, llvm::StringRef Key) {
  std::vector<std::string> Result;
  if (const auto *V = O.get(Key)) {
    if (const auto S = V->getAsString())
      return {S->str()};
    const auto *Array = V->getAsArray();
    if (!Array)
      throw Error("package_invalid_field_type");
    for (const auto &Item : *Array) {
      const auto S = Item.getAsString();
      if (!S)
        throw Error("package_invalid_field_type");
      Result.push_back(S->str());
    }
    std::sort(Result.begin(), Result.end());
  }
  return Result;
}
bool confined(std::string_view Path, bool Empty = false) {
  if (Path.empty())
    return Empty;
  if (Path.size() > 4096 || Path.front() == '/' || Path.back() == '/' ||
      Path.find_first_of("\\:") != std::string_view::npos)
    return false;
  unsigned Depth = 0;
  for (unsigned char C : Path)
    if (C < 32 || C == 127)
      return false;
  while (!Path.empty()) {
    const auto End = Path.find('/');
    const auto Part = Path.substr(0, End);
    if (Part.empty() || Part == "." || Part == ".." || ++Depth > 64)
      return false;
    if (End == std::string_view::npos)
      break;
    Path.remove_prefix(End + 1);
  }
  return true;
}
bool packageName(std::string_view Name) {
  if (!confined(Name) || Name == "node_modules")
    return false;
  const auto Slash = Name.find('/');
  return Name.front() == '@'
             ? Slash > 1 && Slash != std::string_view::npos &&
                   Name.find('/', Slash + 1) == std::string_view::npos
             : Slash == std::string_view::npos;
}
std::string installedName(std::string_view Location) {
  const auto Marker = Location.rfind("node_modules/");
  if (Marker == std::string_view::npos ||
      (Marker && Location[Marker - 1] != '/'))
    return {};
  const auto Name = Location.substr(Marker + 13);
  return packageName(Name) ? std::string(Name) : std::string();
}
std::string parent(std::string_view Path) {
  const auto Slash = Path.rfind('/');
  return Slash == std::string_view::npos ? std::string()
                                         : std::string(Path.substr(0, Slash));
}
std::string join(std::string_view Base, std::string_view Tail) {
  return Base.empty() ? std::string(Tail)
                      : std::string(Base) + "/" + std::string(Tail);
}
std::string specKind(std::string_view V) {
  if (V.starts_with("npm:"))
    return "alias_declaration";
  if (V.starts_with("workspace:"))
    return "workspace_declaration";
  if (V.starts_with("file:") || V.starts_with("link:"))
    return "file_declaration";
  if (V.starts_with("git") || V.starts_with("github:") ||
      V.starts_with("gitlab:") || V.starts_with("bitbucket:"))
    return "git_declaration";
  if (V.starts_with("https:") || V.starts_with("http:"))
    return "url_declaration";
  return "opaque_specification";
}
bool gitCommit(std::string_view V) {
  return V.size() == 40 && V.find_first_not_of("0123456789abcdefABCDEF") ==
                               std::string_view::npos;
}
std::string integrity(std::string_view Value, bool Git) {
  if (Git && gitCommit(Value))
    return "git_commit_declared_unverified";
  if (Value.empty())
    return "invalid";
  bool Supported = false, Unsupported = false;
  while (!Value.empty()) {
    const auto Start = Value.find_first_not_of(" \t\r\n");
    if (Start == std::string_view::npos)
      break;
    Value.remove_prefix(Start);
    const auto End = Value.find_first_of(" \t\r\n");
    const auto Token = Value.substr(0, End);
    const auto Dash = Token.find('-');
    if (Dash == 0 || Dash == std::string_view::npos)
      return "invalid";
    const auto Algorithm = Token.substr(0, Dash);
    auto Encoded = Token.substr(Dash + 1);
    if (Encoded.find('?') != std::string_view::npos)
      return "unsupported_sri_options";
    const uint64_t Length = Algorithm == "sha512"   ? 64
                            : Algorithm == "sha384" ? 48
                            : Algorithm == "sha256" ? 32
                            : Algorithm == "sha1"   ? 20
                                                    : 0;
    constexpr std::string_view Alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (Encoded.empty() || Encoded.size() % 4)
      return "invalid";
    const auto Padding = Encoded.ends_with("==")  ? 2
                         : Encoded.ends_with('=') ? 1
                                                  : 0;
    const auto Data = Encoded.substr(0, Encoded.size() - Padding);
    if (Data.empty() ||
        Data.find_first_not_of(Alphabet) != std::string_view::npos ||
        (Padding && (Alphabet.find(Data.back()) & (Padding == 2 ? 15 : 3))) ||
        (Length && Encoded.size() / 4 * 3 - Padding != Length))
      return "invalid";
    Supported |= Length != 0;
    Unsupported |= Length == 0;
    if (End == std::string_view::npos)
      break;
    Value.remove_prefix(End);
  }
  if (!Supported)
    return Unsupported ? "unsupported_algorithm" : "invalid";
  return "declared_unverified";
}

class PackageReader {
  PackageAnalysis A;
  std::map<std::string, const Artifact *> Members;
  std::map<std::string, uint32_t> Locations;
  std::map<std::string, Value> Documents;
  const Artifact *Selected = nullptr;

  void step(uint64_t Count = 1) {
    if (Count > 2000000 - A.Steps)
      throw Error("package_analysis_budget_exceeded");
    A.Steps += Count;
  }
  const Object &document(const Artifact &F) {
    const auto Found = Documents.find(F.ID);
    if (Found != Documents.end())
      return *Found->second.getAsObject();
    if (F.Content.size() > MaxPackageMetadataBytes - A.MetadataBytes)
      throw Error("package_metadata_budget_exceeded");
    A.MetadataBytes += F.Content.size();
    const auto Bytes =
        F.Content.read(0, F.Content.size(), MaxPackageMetadataBytes);
    if (sha256(Bytes) != F.BlobHash)
      throw Error("package_metadata_hash_mismatch");
    auto JSON =
        parseBoundedJSON(Bytes, {MaxPackageMetadataBytes, 64, 200000, 65536});
    if (!JSON.getAsObject())
      throw Error("package_metadata_not_object");
    return *Documents.emplace(F.ID, std::move(JSON))
                .first->second.getAsObject();
  }
  void requirements(PackageInstance &P, const Object &O, bool Legacy) {
    for (const auto *Key :
         {"dependencies", "devDependencies", "optionalDependencies",
          "peerDependencies", "requires"}) {
      if (Legacy != (std::string_view(Key) == "requires"))
        continue;
      const auto *Map = object(O, Key);
      if (!Map)
        continue;
      for (const auto &[Name, V] : *Map) {
        step();
        if (!packageName(Name.str()))
          throw Error("package_invalid_dependency_name");
        const auto Spec = V.getAsString();
        if (!Spec)
          throw Error("package_invalid_field_type");
        P.Requirements[Key].emplace(Name.str(), Spec->str());
      }
    }
    if (const auto *Meta = object(O, "peerDependenciesMeta"))
      for (const auto &[Name, V] : *Meta) {
        const auto *O = V.getAsObject();
        if (!O || !packageName(Name.str()))
          throw Error("package_invalid_field_type");
        P.PeerOptional.emplace(Name.str(), flag(*O, "optional"));
      }
  }
  uint32_t instance(std::string Location, const Object &O, bool Legacy) {
    if (A.Packages.size() >= MaxPackageInstances)
      throw Error("package_instance_budget_exceeded");
    if (!confined(Location, true))
      throw Error("package_invalid_location");
    PackageInstance P;
    P.Location = std::move(Location);
    P.InstalledName = installedName(P.Location);
    P.Name = text(O, "name");
    P.Version = text(O, "version");
    const auto Source = A.Kind == "npm-lock" ? "lockfile" : "manifest";
    if (P.Name)
      P.NameSource = Source;
    if (Legacy && P.Version && specKind(*P.Version) != "opaque_specification") {
      P.LegacyFetchSpec = std::move(P.Version);
      P.Version.reset();
    }
    if (P.Version)
      P.VersionKind = "package_version_declaration";
    if (P.Version)
      P.VersionSource = Source;
    P.Resolved = text(O, "resolved");
    P.OS = strings(O, "os");
    P.CPU = strings(O, "cpu");
    P.Libc = strings(O, "libc");
    if (O.get("os"))
      P.OSSource = Source;
    if (O.get("cpu"))
      P.CPUSource = Source;
    if (O.get("libc"))
      P.LibcSource = Source;
    P.Dev = flag(O, "dev");
    P.Optional = flag(O, "optional");
    P.DevOptional = flag(O, "devOptional");
    P.Peer = flag(O, "peer");
    P.Link = flag(O, "link");
    P.HasInstallScript = flag(O, "hasInstallScript");
    P.InBundle = flag(O, "inBundle") || flag(O, "bundled");
    if (P.Resolved)
      P.OriginKind = specKind(*P.Resolved);
    else if (P.LegacyFetchSpec)
      P.OriginKind = specKind(*P.LegacyFetchSpec);
    const bool Git = P.OriginKind == "git_declaration" ||
                     (P.LegacyFetchSpec &&
                      specKind(*P.LegacyFetchSpec) == "git_declaration");
    if (const auto *V = O.get("integrity")) {
      if (const auto S = V->getAsString()) {
        P.Integrity = S->str();
        P.IntegrityStatus = integrity(*P.Integrity, Git);
      } else {
        P.IntegrityStatus = "invalid";
      }
    }
    requirements(P, O, Legacy);
    const auto Index = uint32_t(A.Packages.size());
    P.ID = identity("package-instance", {A.ID, std::to_string(Index)});
    if (!Locations.emplace(P.Location, Index).second)
      throw Error("package_duplicate_location");
    A.Packages.push_back(std::move(P));
    return Index;
  }
  void legacy(const Object &O, std::string_view Base, uint64_t Depth) {
    if (Depth > 64)
      throw Error("package_analysis_budget_exceeded");
    std::map<std::string, const Object *> Ordered;
    for (const auto &[Name, V] : O) {
      const auto *P = V.getAsObject();
      if (!P || !packageName(Name.str()))
        throw Error("package_invalid_legacy_dependency");
      Ordered.emplace(Name.str(), P);
    }
    for (const auto &[Name, P] : Ordered) {
      step();
      const auto Location = join(Base, "node_modules/" + Name);
      instance(Location, *P, true);
      if (const auto *Nested = object(*P, "dependencies"))
        legacy(*Nested, Location, Depth + 1);
    }
  }
  void compareLegacy(const Object &O, std::string_view Base, uint64_t Depth) {
    if (Depth > 64)
      throw Error("package_analysis_budget_exceeded");
    for (const auto &[Name, V] : O) {
      step();
      const auto *P = V.getAsObject();
      if (!P || !packageName(Name.str()))
        throw Error("package_invalid_legacy_dependency");
      const auto Location = join(Base, "node_modules/" + Name.str());
      const auto F = Locations.find(Location);
      if (F == Locations.end()) {
        A.LegacyStatus = "conflicting_declarations";
      } else {
        const auto &Primary = A.Packages[F->second];
        const auto Version = text(*P, "version"),
                   Resolved = text(*P, "resolved");
        const auto *I = P->get("integrity");
        // In the legacy tree a version can instead be a fetch specification.
        // That declaration does not contradict an instance's package version.
        const bool ComparableVersion =
            Version && specKind(*Version) == "opaque_specification";
        const bool ComparableOrigin =
            Resolved && Primary.Resolved &&
            specKind(*Resolved) != "opaque_specification" &&
            specKind(*Resolved) == specKind(*Primary.Resolved);
        if ((ComparableVersion && Primary.Version &&
             Version != Primary.Version) ||
            (ComparableOrigin && Resolved != Primary.Resolved) ||
            (I &&
             (!I->getAsString() ||
              (Primary.Integrity && I->getAsString() != *Primary.Integrity))))
          A.LegacyStatus = "conflicting_declarations";
      }
      if (const auto *Nested = object(*P, "dependencies"))
        compareLegacy(*Nested, Location, Depth + 1);
    }
  }
  void manifest(uint32_t Index, const Artifact &F, bool Primary) {
    const auto &O = document(F);
    auto &P = A.Packages[Index];
    P.ManifestArtifactID = F.ID;
    P.ManifestStatus = "supplied_declarations";
    const auto Name = text(O, "name"), Version = text(O, "version");
    const bool ComparableVersion =
        P.VersionKind == "package_version_declaration";
    if (!Primary && ((Name && P.Name && Name != P.Name) ||
                     (Version && ComparableVersion && Version != P.Version)))
      P.ManifestStatus = "conflicting_declarations";
    if (!P.Name && Name) {
      P.Name = Name;
      P.NameSource = "manifest";
    }
    if (!P.Version && Version) {
      P.Version = Version;
      P.VersionKind = "package_version_declaration";
      P.VersionSource = "manifest";
    }
    auto Platform = [&](const char *Key, std::vector<std::string> &Values,
                        std::string &Source) {
      if (!O.get(Key))
        return;
      auto Supplied = strings(O, Key);
      if (Source == "missing") {
        Values = std::move(Supplied);
        Source = "manifest";
      } else if (Values != Supplied) {
        P.ManifestStatus = "conflicting_declarations";
      }
    };
    Platform("os", P.OS, P.OSSource);
    Platform("cpu", P.CPU, P.CPUSource);
    Platform("libc", P.Libc, P.LibcSource);
    if (Primary ||
        (Index == 0 && (A.LockVersion == 1 ||
                        A.RootDeclarations == "root_metadata_not_recorded"))) {
      P.Requirements.clear();
      requirements(P, O, false);
      if (Index == 0)
        A.RootDeclarations = "supplied_manifest";
    } else if (A.LockVersion >= 2 &&
               (Index || A.RootDeclarations != "root_metadata_not_recorded")) {
      PackageInstance Declared;
      requirements(Declared, O, false);
      auto Effective = [](const PackageInstance &Instance,
                          const std::string &Kind) {
        std::map<std::string, std::string> Result;
        if (const auto F = Instance.Requirements.find(Kind);
            F != Instance.Requirements.end())
          Result = F->second;
        if (Kind == "dependencies")
          if (const auto F = Instance.Requirements.find("optionalDependencies");
              F != Instance.Requirements.end())
            for (const auto &[Name, Spec] : F->second)
              Result.erase(Name);
        return Result;
      };
      // A lockfile need not retain development requirements for dependencies.
      for (const auto &Kind : {"dependencies", "optionalDependencies",
                               "peerDependencies", "devDependencies"}) {
        if (Index && std::string_view(Kind) == "devDependencies")
          continue;
        // npm removes optional overrides from its normalized production map.
        // Compare effective declarations without discarding original evidence.
        if (Effective(P, Kind) != Effective(Declared, Kind))
          P.ManifestStatus = "conflicting_declarations";
      }
      if (P.PeerOptional != Declared.PeerOptional)
        P.ManifestStatus = "conflicting_declarations";
    }
    if (const auto *Scripts = object(O, "scripts")) {
      std::map<std::string, std::string> Ordered;
      for (const auto &[Name, V] : *Scripts) {
        const auto Command = V.getAsString();
        if (!Command)
          throw Error("package_invalid_field_type");
        Ordered.emplace(Name.str(), Command->str());
      }
      for (const auto &[Key, Command] : Ordered) {
        if (A.Scripts.size() >= MaxPackageRecords)
          throw Error("package_record_budget_exceeded");
        const bool Lifecycle = Key == "preinstall" || Key == "install" ||
                               Key == "postinstall" || Key == "prepare" ||
                               Key == "prepublish" || Key == "prepublishOnly" ||
                               Key == "prepack" || Key == "postpack" ||
                               Key == "publish" || Key == "postpublish";
        A.Scripts.push_back(
            {identity("package-script",
                      {A.ID, P.ID, F.ID, std::to_string(A.Scripts.size())}),
             F.ID, Key, Command,
             Lifecycle ? "lifecycle_declaration" : "script_declaration",
             Index});
      }
    }
    auto Entry = [&](std::string_view Kind, std::string Path,
                     std::string Name = {}) {
      if (A.Entries.size() >= MaxPackageRecords)
        throw Error("package_record_budget_exceeded");
      PackageEntry E;
      E.ID = identity("package-entry", {A.ID, P.ID, F.ID, Kind,
                                        std::to_string(A.Entries.size())});
      E.ArtifactID = F.ID;
      E.Package = Index;
      E.Kind = Kind;
      E.Name = std::move(Name);
      E.Path = std::move(Path);
      std::string_view Relative = E.Path;
      if (Relative.starts_with("./"))
        Relative.remove_prefix(2);
      E.Status = "unsupported_path";
      if (confined(Relative)) {
        E.Status =
            A.DirectoryInventory ? "not_supplied" : "no_directory_inventory";
        const auto Member = Members.find(join(P.Location, Relative));
        if (Member != Members.end() && !Member->second->Directory) {
          E.TargetArtifactID = Member->second->ID;
          E.Status = "exact_file_candidate";
        }
      }
      A.Entries.push_back(std::move(E));
    };
    for (const auto *Key : {"main", "module"})
      if (auto S = text(O, Key))
        Entry(Key, *S);
    if (const auto *Bin = O.get("bin")) {
      if (const auto S = Bin->getAsString())
        Entry("bin", S->str());
      else if (const auto *Map = Bin->getAsObject()) {
        std::map<std::string, std::string> Ordered;
        for (const auto &[Name, V] : *Map) {
          const auto S = V.getAsString();
          if (!S)
            throw Error("package_invalid_field_type");
          Ordered.emplace(Name.str(), S->str());
        }
        for (const auto &[Name, Path] : Ordered)
          Entry("bin", Path, Name);
      } else {
        throw Error("package_invalid_field_type");
      }
    }
  }
  void resolve() {
    for (uint32_t I = 0; I < A.Packages.size(); ++I) {
      auto &P = A.Packages[I];
      auto Up = parent(P.Location);
      while (!P.Location.empty()) {
        step();
        if (const auto F = Locations.find(Up); F != Locations.end()) {
          P.Parent = F->second;
          break;
        }
        if (Up.empty())
          break;
        Up = parent(Up);
      }
      if (P.Link) {
        P.LinkStatus = "target_not_recorded";
        auto Path = P.Resolved.value_or("");
        if (Path.starts_with("file:"))
          Path.erase(0, 5);
        if (!confined(Path))
          P.LinkStatus = "unsupported_link_target";
        else if (const auto F = Locations.find(Path); F != Locations.end()) {
          P.LinkTarget = F->second;
          P.LinkStatus = F->second == I || A.Packages[F->second].Link
                             ? "unsupported_link_chain"
                             : "target_location_candidate";
        }
      }
    }
    for (uint32_t I = 0; I < A.Packages.size(); ++I) {
      const auto &P = A.Packages[I];
      for (const auto &[Kind, Map] : P.Requirements)
        for (const auto &[Name, Spec] : Map) {
          step();
          if (A.Dependencies.size() >= MaxPackageEdges)
            throw Error("package_dependency_budget_exceeded");
          PackageDependency E;
          E.ID = identity(
              "package-dependency",
              {A.ID, P.ID, Kind, std::to_string(A.Dependencies.size())});
          E.From = I;
          E.Kind = Kind;
          E.RequestedName = Name;
          E.Spec = Spec;
          E.SpecKind = specKind(Spec);
          E.Optional =
              Kind == "optionalDependencies" ||
              (Kind == "peerDependencies" && P.PeerOptional.contains(Name) &&
               P.PeerOptional.at(Name));
          E.Status = "candidate_not_recorded";
          if (Kind == "dependencies" &&
              P.Requirements.contains("optionalDependencies") &&
              P.Requirements.at("optionalDependencies").contains(Name)) {
            E.Status = "overridden_by_optional";
          } else if (P.Link) {
            E.Status = "link_declarations_not_resolved";
          } else {
            auto Base = P.Location;
            for (;;) {
              step();
              const auto Last = Base.substr(Base.rfind('/') == std::string::npos
                                                ? 0
                                                : Base.rfind('/') + 1);
              if (Last != "node_modules") {
                const auto Found =
                    Locations.find(join(Base, "node_modules/" + Name));
                if (Found != Locations.end()) {
                  E.Candidate = Found->second;
                  E.Status = Kind == "peerDependencies" &&
                                     !P.InstalledName.empty() &&
                                     Base == P.Location
                                 ? "peer_placement_conflict"
                                 : "candidate_spec_unverified";
                  const auto &Target = A.Packages[Found->second];
                  E.Conditional = Target.Optional || !Target.OS.empty() ||
                                  !Target.CPU.empty() || !Target.Libc.empty();
                  break;
                }
              }
              if (Base.empty())
                break;
              Base = parent(Base);
            }
          }
          if (E.Optional && E.Status == "candidate_not_recorded")
            E.Status = "optional_candidate_not_recorded";
          E.Conditional |= E.Optional;
          A.Dependencies.push_back(std::move(E));
        }
    }
  }
  void files() {
    for (const auto &[Path, F] : Members) {
      step();
      if (F->Directory)
        continue;
      PackageFile R;
      R.ID = identity("package-file", {A.ID, F->ID});
      R.ArtifactID = F->ID;
      R.Hash = F->BlobHash;
      R.Path = Path;
      R.Size = F->Content.size();
      R.Kind = "other";
      if (Path.ends_with(".js") || Path.ends_with(".cjs") ||
          Path.ends_with(".mjs"))
        R.Kind = "javascript";
      else if (Path.ends_with(".node"))
        R.Kind = "native_addon_candidate";
      else if (Path.ends_with(".json"))
        R.Kind = "json";
      if (R.Size >= 4) {
        const auto Magic = F->Content.read(0, 4);
        if (Magic == "\177ELF" || Magic.starts_with("MZ") ||
            Magic == std::string("\xcf\xfa\xed\xfe", 4) ||
            Magic == std::string("\xca\xfe\xba\xbe", 4))
          R.Kind = "native_image_candidate";
      }
      auto Base = parent(Path);
      for (;;) {
        step();
        if (const auto F = Locations.find(Base); F != Locations.end()) {
          R.Package = F->second;
          break;
        }
        // An unrecorded installed package is an evidence boundary. Its files
        // cannot be attributed to a recorded package higher in the tree.
        if (!installedName(Base).empty())
          break;
        if (Base.empty())
          break;
        Base = parent(Base);
      }
      A.Files.push_back(std::move(R));
    }
  }

public:
  PackageReader(const Snapshot &Input, std::string_view ArtifactID,
                std::string_view Kind) {
    if (Kind != "npm-lock" && Kind != "package-json")
      throw Error("package_unsupported_input_profile");
    if (Input.Artifacts.size() > Limits::HardEntries + 1)
      throw Error("package_namespace_budget_exceeded");
    for (const auto &F : Input.Artifacts)
      if (F.ID == ArtifactID) {
        if (Selected)
          throw Error("package_ambiguous_namespace");
        Selected = &F;
      }
    if (!Selected || Selected->Directory)
      throw Error("unknown_package_artifact");
    A.ArtifactID = ArtifactID;
    A.BlobHash = Selected->BlobHash;
    A.Kind = Kind;
    A.ID = identity("package-analysis",
                    {Input.ID, ArtifactID, A.BlobHash, Kind, PackageProfile});
    A.RootPrefix = parent(Selected->MemberPath);
    // npm's hidden v3 lock is stored below node_modules, but its instance
    // locations are still relative to the project root.
    if (Kind == "npm-lock" &&
        (Selected->MemberPath == "node_modules/.package-lock.json" ||
         Selected->MemberPath.ends_with("/node_modules/.package-lock.json")))
      A.RootPrefix = parent(A.RootPrefix);
    A.DirectoryInventory = !Input.Artifacts.empty() &&
                           Input.Artifacts.front().Directory &&
                           !Selected->MemberPath.empty();
    if (A.DirectoryInventory) {
      const auto Prefix = A.RootPrefix.empty() ? "" : A.RootPrefix + "/";
      for (const auto &F : Input.Artifacts)
        if (F.MemberPath.starts_with(Prefix)) {
          const auto Relative = F.MemberPath.substr(Prefix.size());
          if (!confined(Relative, F.Directory) ||
              !Members.emplace(Relative, &F).second)
            throw Error("package_ambiguous_namespace");
        }
    }
  }
  PackageAnalysis run() {
    const auto &O = document(*Selected);
    if (A.Kind == "npm-lock") {
      const auto Version = O.getInteger("lockfileVersion");
      if (!Version || *Version < 1 || *Version > 3)
        throw Error("package_unsupported_lock_version");
      A.LockVersion = *Version;
      if (*Version == 1) {
        auto Root = Object(O);
        Root.erase("dependencies");
        instance("", Root, false);
        // Top-level dependencies are the installed tree, not root requirements.
        A.Packages[0].Requirements.clear();
        A.RootDeclarations = "not_recorded_in_v1";
        if (const auto *Dependencies = object(O, "dependencies"))
          legacy(*Dependencies, "", 0);
        A.LegacyStatus = "authoritative_v1_tree";
      } else {
        const auto *Packages = object(O, "packages");
        if (!Packages)
          throw Error("package_missing_instance_table");
        std::map<std::string, const Object *> Ordered;
        for (const auto &[Path, V] : *Packages) {
          const auto *P = V.getAsObject();
          if (!P)
            throw Error("package_invalid_field_type");
          Ordered.emplace(Path.str(), P);
        }
        if (!Ordered.contains("")) {
          instance("", Object{}, false);
          A.RootDeclarations = "root_metadata_not_recorded";
        } else {
          A.RootDeclarations = "lock_root_declarations";
        }
        for (const auto &[Path, P] : Ordered)
          instance(Path, *P, false);
        A.LegacyStatus =
            O.get("dependencies") ? "legacy_tree_partially_compared" : "absent";
        if (const auto *Legacy = object(O, "dependencies"))
          compareLegacy(*Legacy, "", 0);
      }
      for (uint32_t I = 0; I < A.Packages.size(); ++I) {
        const auto &P = A.Packages[I];
        if (P.Link)
          continue;
        const auto M = Members.find(join(P.Location, "package.json"));
        if (M != Members.end() && !M->second->Directory)
          manifest(I, *M->second, false);
      }
    } else {
      instance("", O, false);
      A.LegacyStatus = "not_applicable";
      manifest(0, *Selected, true);
      for (const auto &[Path, F] : Members)
        if (Path.ends_with("/package.json") && !F->Directory) {
          const auto Location = parent(Path);
          if (installedName(Location).empty())
            continue;
          const auto I = instance(Location, document(*F), false);
          manifest(I, *F, true);
        }
    }
    resolve();
    files();
    return std::move(A);
  }
};
} // namespace

PackageAnalysis analyzePackages(const Snapshot &Input,
                                std::string_view ArtifactID,
                                std::string_view Kind) {
  return PackageReader(Input, ArtifactID, Kind).run();
}
} // namespace neverd::web
