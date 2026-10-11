//===- PackageTests.cpp - Package tests --------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Package tests.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"
#include "gtest/gtest.h"

#include "neverd/web/Packages.h"

#include "llvm/Support/FileSystem.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
using namespace neverd::web;
class WebPackages : public ::testing::Test {
protected:
  std::filesystem::path Root;
  void SetUp() override {
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-packages", Directory));
    Root = Directory.str().str();
  }
  void TearDown() override {
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }
  void write(std::string_view Name, std::string_view Bytes) {
    const auto Path = Root / Name;
    std::filesystem::create_directories(Path.parent_path());
    std::ofstream F(Path, std::ios::binary);
    F.write(Bytes.data(), Bytes.size());
    ASSERT_TRUE(F.good());
  }
  PackageAnalysis inspect(std::string_view Path,
                          std::string_view Kind = "npm-lock") {
    const auto S = capture(Root.string(), Limits{});
    for (const auto &F : S.Artifacts)
      if (F.MemberPath == Path)
        return analyzePackages(S, F.ID, Kind);
    throw std::runtime_error("missing test artifact");
  }
  PackageAnalysis single(std::string_view Bytes,
                         std::string_view Kind = "npm-lock") {
    write("input.json", Bytes);
    const auto S = capture((Root / "input.json").string(), Limits{});
    return analyzePackages(S, S.Artifacts.front().ID, Kind);
  }
  const PackageInstance &at(const PackageAnalysis &A,
                            std::string_view Location) {
    for (const auto &P : A.Packages)
      if (P.Location == Location)
        return P;
    throw std::runtime_error("missing package");
  }
  const PackageDependency &edge(const PackageAnalysis &A,
                                std::string_view Location,
                                std::string_view Name,
                                std::string_view Kind = "dependencies") {
    for (const auto &E : A.Dependencies)
      if (A.Packages[E.From].Location == Location && E.RequestedName == Name &&
          E.Kind == Kind)
        return E;
    throw std::runtime_error("missing dependency");
  }
};

#ifndef _WIN32
TEST_F(WebPackages,
       V1ContainmentIsNotRootDependencyAndRequiresFindNearestCandidate) {
  const auto A = single(
      R"({"name":"app","lockfileVersion":1,"requires":true,"dependencies":{
    "a":{"version":"1.0.0","requires":{"b":"^2","missing":"*"},"dependencies":{"b":{"version":"2.0.0"}}},
    "b":{"version":"1.0.0"},"optional":{"optional":true}}})");
  ASSERT_EQ(A.Packages.size(), 5);
  EXPECT_EQ(A.RootDeclarations, "not_recorded_in_v1");
  EXPECT_EQ(A.Dependencies.size(), 2);
  const auto &E = edge(A, "node_modules/a", "b", "requires");
  ASSERT_NE(E.Candidate, NoPackageIndex);
  EXPECT_EQ(A.Packages[E.Candidate].Location, "node_modules/a/node_modules/b");
  EXPECT_EQ(E.Status, "candidate_spec_unverified");
  EXPECT_EQ(edge(A, "node_modules/a", "missing", "requires").Status,
            "candidate_not_recorded");
  EXPECT_FALSE(at(A, "node_modules/optional").Version.has_value());
  EXPECT_EQ(A.Packages[at(A, "node_modules/a/node_modules/b").Parent].Location,
            "node_modules/a");
  EXPECT_FALSE(A.DirectoryInventory);
}

TEST_F(WebPackages, SuppliedV1RootManifestAddsOnlyDeclaredDirectRequirements) {
  write(
      "package-lock.json",
      R"({"name":"app","lockfileVersion":1,"dependencies":{"a":{"version":"1"},"hoisted":{"version":"2"}}})");
  write(
      "package.json",
      R"({"name":"app","dependencies":{"a":"1"},"scripts":{"postinstall":"touch MUST_NOT_EXECUTE"}})");
  const auto A = inspect("package-lock.json");
  ASSERT_EQ(A.Dependencies.size(), 1);
  EXPECT_EQ(edge(A, "", "a").Status, "candidate_spec_unverified");
  EXPECT_EQ(A.RootDeclarations, "supplied_manifest");
  ASSERT_EQ(A.Scripts.size(), 1);
  EXPECT_EQ(A.Scripts[0].Kind, "lifecycle_declaration");
  EXPECT_FALSE(std::filesystem::exists(Root / "MUST_NOT_EXECUTE"));
}

TEST_F(WebPackages, V2AndV3RetainAliasesOptionalPeersAndWorkspaceTargets) {
  for (const auto Version : {2, 3}) {
    const auto A = single("{\"lockfileVersion\":" + std::to_string(Version) +
                          R"(,"packages":{
      "":{"name":"app","dependencies":{"alias":"npm:real@1","tool":"1","space":"file:packages/space"},"optionalDependencies":{"tool":"2"}},
      "node_modules/alias":{"name":"real","version":"1"},
      "node_modules/tool":{"version":"2","optional":true,"os":["linux"],"cpu":["arm64"],"libc":["musl"],"peerDependencies":{"peer":"*","absent":"*"},"peerDependenciesMeta":{"absent":{"optional":true}}},
      "node_modules/tool/node_modules/peer":{"version":"1"},
      "node_modules/peer":{"version":"2"},
      "node_modules/space":{"resolved":"packages/space","link":true},
      "packages/space":{"name":"space","dependencies":{"peer":"*"}}
    }})");
    const auto &Alias = at(A, "node_modules/alias");
    EXPECT_EQ(Alias.InstalledName, "alias");
    EXPECT_EQ(Alias.Name, "real");
    EXPECT_EQ(edge(A, "", "alias").SpecKind, "alias_declaration");
    EXPECT_EQ(edge(A, "", "tool").Status, "overridden_by_optional");
    EXPECT_TRUE(edge(A, "", "tool", "optionalDependencies").Conditional);
    EXPECT_EQ(edge(A, "node_modules/tool", "peer", "peerDependencies").Status,
              "peer_placement_conflict");
    EXPECT_EQ(edge(A, "node_modules/tool", "absent", "peerDependencies").Status,
              "optional_candidate_not_recorded");
    const auto &Link = at(A, "node_modules/space");
    EXPECT_EQ(Link.LinkStatus, "target_location_candidate");
    ASSERT_NE(Link.LinkTarget, NoPackageIndex);
    EXPECT_EQ(A.Packages[Link.LinkTarget].Location, "packages/space");
    const auto &Peer = edge(A, "packages/space", "peer");
    ASSERT_NE(Peer.Candidate, NoPackageIndex);
    EXPECT_EQ(A.Packages[Peer.Candidate].Location, "node_modules/peer");
    EXPECT_EQ(at(A, "node_modules/tool").Libc,
              std::vector<std::string>{"musl"});
  }
}

TEST_F(WebPackages, MissingRootMetadataAndLinksDoNotInventRuntimeTargets) {
  const auto A = single(R"({"lockfileVersion":3,"packages":{
    "node_modules/a":{"link":true,"resolved":"../outside"},
    "node_modules/b":{"link":true,"resolved":"node_modules/c"},
    "node_modules/c":{"link":true,"resolved":"node_modules/b"},
    "node_modules/d":{"link":true,"resolved":"missing"}}})");
  EXPECT_EQ(A.RootDeclarations, "root_metadata_not_recorded");
  EXPECT_EQ(at(A, "node_modules/a").LinkStatus, "unsupported_link_target");
  EXPECT_EQ(at(A, "node_modules/b").LinkStatus, "unsupported_link_chain");
  EXPECT_EQ(at(A, "node_modules/d").LinkStatus, "target_not_recorded");
}

TEST_F(WebPackages, IntegrityClaimsDistinguishMissingInvalidUnsupportedAndGit) {
  const auto A = single(R"({"lockfileVersion":1,"dependencies":{
    "valid":{"integrity":"sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="},
    "missing":{},"invalid":{"integrity":"sha256-AAAA"},
    "bad-type":{"integrity":7},"bad-padding":{"integrity":"sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB="},
    "unknown":{"integrity":"future-AAAA"},
    "git":{"version":"git+https://example.invalid/repo.git#0123456789012345678901234567890123456789","integrity":"0123456789012345678901234567890123456789"}}})");
  EXPECT_EQ(at(A, "node_modules/valid").IntegrityStatus, "declared_unverified");
  EXPECT_EQ(at(A, "node_modules/missing").IntegrityStatus, "missing");
  EXPECT_EQ(at(A, "node_modules/invalid").IntegrityStatus, "invalid");
  EXPECT_EQ(at(A, "node_modules/bad-type").IntegrityStatus, "invalid");
  EXPECT_EQ(at(A, "node_modules/bad-padding").IntegrityStatus, "invalid");
  EXPECT_EQ(at(A, "node_modules/unknown").IntegrityStatus,
            "unsupported_algorithm");
  EXPECT_EQ(at(A, "node_modules/git").IntegrityStatus,
            "git_commit_declared_unverified");
}

TEST_F(WebPackages,
       LegacyAndSuppliedManifestContradictionsAreNotSilentlyAccepted) {
  write(
      "package-lock.json",
      R"({"lockfileVersion":2,"packages":{"":{"name":"app"},"node_modules/a":{"name":"a","version":"1"}},"dependencies":{"a":{"version":"2"}}})");
  write("node_modules/a/package.json", R"({"name":"different","version":"3"})");
  const auto A = inspect("package-lock.json");
  EXPECT_EQ(A.LegacyStatus, "conflicting_declarations");
  EXPECT_EQ(at(A, "node_modules/a").Version, "1");
  EXPECT_EQ(at(A, "node_modules/a").ManifestStatus, "conflicting_declarations");
  EXPECT_FALSE(at(A, "node_modules/a").ManifestArtifactID.empty());
}

TEST_F(WebPackages,
       ManifestTreeLinksOnlyExactSuppliedEntriesAndKeepsNativeHashes) {
  write(
      "package.json",
      R"({"name":"root","bin":{"run":"./bin/cli.js","escape":"../outside.js"},"main":"missing","dependencies":{"a":"1"}})");
  write("bin/cli.js", "process.exit(77);");
  write("node_modules/a/package.json",
        R"({"name":"a","version":"1","main":"native.node"})");
  write("node_modules/a/native.node", "\177ELF inert bytes");
  const auto A = inspect("package.json", "package-json");
  ASSERT_EQ(A.Packages.size(), 2);
  EXPECT_EQ(edge(A, "", "a").Status, "candidate_spec_unverified");
  EXPECT_EQ(A.Entries.size(), 4);
  unsigned Exact = 0, Outside = 0, Native = 0;
  for (const auto &E : A.Entries) {
    Exact += E.Status == "exact_file_candidate";
    Outside += E.Status == "unsupported_path";
  }
  for (const auto &F : A.Files)
    if (F.Kind == "native_image_candidate") {
      ++Native;
      EXPECT_EQ(F.Hash, sha256("\177ELF inert bytes"));
      ASSERT_NE(F.Package, NoPackageIndex);
      EXPECT_EQ(A.Packages[F.Package].Location, "node_modules/a");
    }
  EXPECT_EQ(Exact, 2);
  EXPECT_EQ(Outside, 1);
  EXPECT_EQ(Native, 1);
}

TEST_F(WebPackages,
       TwoRootsDiffAttributesFilesScriptsRequirementsAndReplacement) {
  write(
      "before/package.json",
      R"({"name":"app","version":"1","os":["linux"],"scripts":{"postinstall":"one"},"dependencies":{"a":"1"}})");
  write(
      "after/package.json",
      R"({"name":"app","version":"2","os":["darwin"],"scripts":{"postinstall":"two"},"dependencies":{"a":"2"}})");
  write("before/node_modules/a/package.json",
        R"({"name":"first","version":"1"})");
  write("after/node_modules/a/package.json",
        R"({"name":"second","version":"2"})");
  write("before/main.js", "before");
  write("after/main.js", "after");
  write("before/native.node", "old");
  const auto B = inspect("before/package.json", "package-json");
  const auto A = inspect("after/package.json", "package-json");
  const auto D = comparePackages(B, A);
  EXPECT_EQ(D.RootIdentity, "same_declared_name");
  EXPECT_EQ(D.Platform, "different_root_conditions");
  EXPECT_EQ(D.Coverage, "two_supplied_directory_inventories");
  unsigned Replacement = 0, Scripts = 0, Requirements = 0, Changed = 0,
           Absent = 0;
  for (const auto &C : D.Changes) {
    Replacement += C.Kind == "package_replacement_candidate";
    Scripts += C.Field == "scripts";
    Requirements += C.Field == "requirements";
    Changed += C.Kind == "supplied_file_changed";
    Absent += C.Kind == "file_not_supplied_after";
  }
  EXPECT_EQ(Replacement, 1);
  EXPECT_EQ(Scripts, 1);
  EXPECT_EQ(Requirements, 1);
  EXPECT_EQ(Changed, 3);
  EXPECT_EQ(Absent, 1);
  EXPECT_TRUE(comparePackages(B, B).Changes.empty());
  EXPECT_EQ(D.ID, comparePackages(B, A).ID);
}

TEST_F(WebPackages,
       MalformedMetadataAndUnsupportedVersionsFailBeforePublication) {
  for (
      const auto *Bytes :
      {R"({"lockfileVersion":4})", R"({"lockfileVersion":1.5})",
       R"({"lockfileVersion":3,"packages":{"../outside":{}}})",
       R"({"lockfileVersion":3,"packages":{"":{"dependencies":{"../bad":"1"}}}})",
       R"({"lockfileVersion":3,"packages":{"":{"cpu":7}}})",
       R"({"lockfileVersion":3,"packages":{"":{"link":1}}})",
       R"({"lockfileVersion":1,"dependencies":{"a":{"version":1}}})",
       R"({"lockfileVersion":3,"packages":{},"packages":{}})",
       R"({"lockfileVersion":3,"packages":[]})"})
    EXPECT_THROW(single(Bytes), Error);
  EXPECT_THROW(single("{}", "yarn"), Error);
  EXPECT_THROW(single(std::string(MaxPackageMetadataBytes + 1, ' ')), Error);
}

TEST_F(WebPackages, WholeGraphInstanceBudgetRejectsPartialOutput) {
  std::string Bytes = R"({"lockfileVersion":3,"packages":{"":{})";
  for (uint64_t I = 0; I < MaxPackageInstances; ++I)
    Bytes += ",\"node_modules/p" + std::to_string(I) + "\":{}";
  Bytes += "}}";
  EXPECT_THROW(single(Bytes), Error);
}

TEST_F(WebPackages, NestedExampleMetadataIsNotAnInstalledPackageInstance) {
  write("package.json", R"({"name":"root"})");
  write("node_modules/a/package.json", R"({"name":"a"})");
  write("node_modules/a/example/package.json", R"({"name":"example"})");
  write("not_node_modules/b/package.json", R"({"name":"not-installed"})");
  const auto A = inspect("package.json", "package-json");
  ASSERT_EQ(A.Packages.size(), 2);
  EXPECT_EQ(A.Packages[1].Location, "node_modules/a");
}

TEST_F(WebPackages, BinAliasChangeAndDifferentLockContractsRemainVisible) {
  const auto B =
      single(R"({"name":"app","bin":{"old":"cli.js"}})", "package-json");
  const auto A =
      single(R"({"name":"app","bin":{"new":"cli.js"}})", "package-json");
  const auto D = comparePackages(B, A);
  ASSERT_EQ(D.Changes.size(), 1);
  EXPECT_EQ(D.Changes.front().Field, "entry_declarations");
  const auto V1 = single(
      R"({"lockfileVersion":1,"dependencies":{"a":{"requires":{"b":"1"}},"b":{"version":"1"}}})");
  const auto V3 = single(
      R"({"lockfileVersion":3,"packages":{"":{},"node_modules/a":{"dependencies":{"b":"1"}},"node_modules/b":{"version":"1"}}})");
  const auto Versions = comparePackages(V1, V3);
  EXPECT_EQ(Versions.InputContract,
            "different_lock_versions_metadata_not_compared");
  for (const auto &C : Versions.Changes)
    EXPECT_NE(C.Field, "requirements");
}

TEST_F(WebPackages, NpmCLI1190FullLockWhenSupplied) {
  const auto *Path = std::getenv("NEVERD_NPM_CLI_1190_LOCK");
  if (!Path)
    GTEST_SKIP() << "Pinned npm CLI v11.9.0 lockfile not supplied";
  const auto S = capture(Path, Limits{});
  ASSERT_EQ(S.Artifacts.size(), 1);
  ASSERT_EQ(S.Artifacts[0].BlobHash,
            "b07e0fba031168ad2f8923c3b15d188f23b50799229f9ba456ed0abdfb427ab4");
  const auto A = analyzePackages(S, S.Artifacts[0].ID, "npm-lock");
  EXPECT_EQ(A.LockVersion, 3);
  EXPECT_EQ(A.Packages.size(), 1230);
  EXPECT_EQ(A.Dependencies.size(), 2426);
  EXPECT_EQ(A.MetadataBytes, 400837);
}

TEST_F(WebPackages, PlatformConditionsAcceptStringOrArray) {
  const auto A =
      single(R"({"os":"linux","cpu":"x64","libc":"glibc"})", "package-json");
  const auto B = single(R"({"os":["linux"],"cpu":["x64"],"libc":["glibc"]})",
                        "package-json");
  EXPECT_EQ(A.Packages.front().OS, B.Packages.front().OS);
  EXPECT_EQ(A.Packages.front().CPU, B.Packages.front().CPU);
  EXPECT_EQ(A.Packages.front().Libc, B.Packages.front().Libc);
  EXPECT_TRUE(comparePackages(A, B).Changes.empty());
}

TEST_F(WebPackages, LegacyFetchSpecIsNotAnActualPackageVersion) {
  write("package-lock.json", R"({"lockfileVersion":2,"packages":{
    "":{},"node_modules/a":{"version":"1.0.0","resolved":"https://example.invalid/a.tgz"},
    "node_modules/b":{"version":"2.0.0","resolved":"git+https://example.invalid/b.git#deadbeef"}},
    "dependencies":{"a":{"version":"https://example.invalid/a.tgz"},
      "b":{"version":"git+https://example.invalid/b.git#deadbeef"}}})");
  EXPECT_EQ(inspect("package-lock.json").LegacyStatus,
            "legacy_tree_partially_compared");
  write("package-lock.json", R"({"lockfileVersion":1,"dependencies":{
    "a":{"version":"https://example.invalid/a.tgz"}}})");
  write("node_modules/a/package.json", R"({"name":"a","version":"1.0.0"})");
  const auto Legacy = inspect("package-lock.json");
  EXPECT_EQ(at(Legacy, "node_modules/a").ManifestStatus,
            "supplied_declarations");
  const auto Modern = single(R"({"lockfileVersion":3,"packages":{
    "":{},"node_modules/a":{"version":"1.0.0","resolved":"https://example.invalid/a.tgz"}}})");
  for (const auto &C : comparePackages(Legacy, Modern).Changes) {
    EXPECT_NE(C.Field, "version");
    EXPECT_NE(C.Field, "resolved_origin");
  }
}

TEST_F(WebPackages,
       MissingManifestDeclarationsAndRootDevConflictRemainVisible) {
  for (const auto *Field : {"dependencies", "optionalDependencies",
                            "peerDependencies", "devDependencies"}) {
    const auto Declared = std::string("{\"") + Field + "\":{\"a\":\"1\"}}";
    write("package-lock.json",
          "{\"lockfileVersion\":3,\"packages\":{\"\":" + Declared + "}}");
    write("package.json", "{}");
    EXPECT_EQ(inspect("package-lock.json").Packages.front().ManifestStatus,
              "conflicting_declarations")
        << Field;
    write("package-lock.json", R"({"lockfileVersion":3,"packages":{"":{}}})");
    write("package.json", Declared);
    EXPECT_EQ(inspect("package-lock.json").Packages.front().ManifestStatus,
              "conflicting_declarations")
        << Field;
  }
  write("package-lock.json", R"({"lockfileVersion":3,"packages":{
    "":{"peerDependenciesMeta":{"a":{"optional":true}}}}})");
  write("package.json", "{}");
  EXPECT_EQ(inspect("package-lock.json").Packages.front().ManifestStatus,
            "conflicting_declarations");
}

TEST_F(WebPackages, MissingManifestIsCoverageNotAnEmptyScriptOrEntrySet) {
  const auto *Lock = R"({"lockfileVersion":3,"packages":{"":{"name":"app"}}})";
  write("before/package-lock.json", Lock);
  write("after/package-lock.json", Lock);
  write("after/package.json",
        R"({"name":"app","scripts":{"postinstall":"inert"},"main":"cli.js"})");
  const auto D = comparePackages(inspect("before/package-lock.json"),
                                 inspect("after/package-lock.json"));
  unsigned Coverage = 0;
  for (const auto &C : D.Changes) {
    EXPECT_NE(C.Field, "scripts");
    EXPECT_NE(C.Field, "entry_declarations");
    Coverage += C.Kind == "declaration_coverage_changed" &&
                C.Field == "manifest_evidence";
  }
  EXPECT_EQ(Coverage, 1);
}

TEST_F(WebPackages, FileOwnershipStopsAtUnrecordedPackageBoundaries) {
  write("package-lock.json", R"({"lockfileVersion":3,"packages":{
    "":{},"node_modules/known":{}}})");
  write("node_modules/unlisted/native.node", "inert");
  write("node_modules/known/node_modules/@scope/unlisted/index.js", "inert");
  write("node_modules/known/known.js", "inert");
  const auto A = inspect("package-lock.json");
  unsigned Unknown = 0, Known = 0;
  for (const auto &F : A.Files) {
    if (F.Path.find("unlisted/") != std::string::npos) {
      EXPECT_EQ(F.Package, NoPackageIndex);
      ++Unknown;
    } else if (F.Path.ends_with("/known.js")) {
      EXPECT_NE(F.Package, NoPackageIndex);
      ++Known;
    }
  }
  EXPECT_EQ(Unknown, 2);
  EXPECT_EQ(Known, 1);
}

TEST_F(WebPackages, WorkspaceTargetPeerHasItsOwnTopInstallationContext) {
  const auto A = single(R"({"lockfileVersion":3,"packages":{
    "":{},"node_modules/w":{"link":true,"resolved":"packages/w"},
    "packages/w":{"peerDependencies":{"p":"*"}},
    "packages/w/node_modules/p":{"version":"1"}}})");
  EXPECT_EQ(edge(A, "packages/w", "p", "peerDependencies").Status,
            "candidate_spec_unverified");
}

TEST_F(WebPackages, HiddenLockfileLocationsUseTheProjectRoot) {
  write("app/node_modules/.package-lock.json", R"({"lockfileVersion":3,
    "packages":{"node_modules/a":{"version":"1"}}})");
  write("app/package.json", R"({"dependencies":{"a":"1"}})");
  write("app/node_modules/a/package.json", R"({"name":"a","version":"1"})");
  const auto A = inspect("app/node_modules/.package-lock.json");
  EXPECT_EQ(A.RootPrefix, "app");
  EXPECT_EQ(A.RootDeclarations, "supplied_manifest");
  EXPECT_EQ(at(A, "node_modules/a").ManifestStatus, "supplied_declarations");
  EXPECT_EQ(edge(A, "", "a").Status, "candidate_spec_unverified");
  for (const auto &F : A.Files)
    if (F.Path == "node_modules/a/package.json") {
      ASSERT_NE(F.Package, NoPackageIndex);
      EXPECT_EQ(A.Packages[F.Package].Location, "node_modules/a");
    }
}

TEST_F(WebPackages, SuppliedManifestFillsMissingVersionAndPlatformEvidence) {
  for (const auto *Prefix : {"before", "after"})
    write(std::string(Prefix) + "/node_modules/.package-lock.json",
          R"({"lockfileVersion":3,"packages":{}})");
  write("before/package.json", R"({"name":"app","version":"1","os":"linux"})");
  write("after/package.json", R"({"name":"app","version":"2","os":"darwin"})");
  const auto B = inspect("before/node_modules/.package-lock.json");
  const auto A = inspect("after/node_modules/.package-lock.json");
  EXPECT_EQ(B.Packages.front().VersionSource, "manifest");
  EXPECT_EQ(B.Packages.front().OSSource, "manifest");
  EXPECT_EQ(B.Packages.front().Version, "1");
  const auto D = comparePackages(B, A);
  EXPECT_EQ(D.Platform, "different_root_conditions");
  unsigned Version = 0, Platform = 0;
  for (const auto &C : D.Changes) {
    Version += C.Field == "version";
    Platform += C.Field == "platform_conditions";
  }
  EXPECT_EQ(Version, 1);
  EXPECT_EQ(Platform, 1);
  write("package-lock.json", R"({"lockfileVersion":3,"packages":{
    "":{"os":["linux"],"cpu":"x64","libc":"glibc"}}})");
  write("package.json", R"({"os":"darwin","cpu":"arm64","libc":"musl"})");
  const auto Conflict = inspect("package-lock.json");
  EXPECT_EQ(Conflict.Packages.front().ManifestStatus,
            "conflicting_declarations");
  EXPECT_EQ(Conflict.Packages.front().OSSource, "lockfile");
}

TEST_F(WebPackages, LinkAliasDoesNotRemoveAnExistingInstallationParent) {
  const auto A = single(R"({"lockfileVersion":3,"packages":{
    "":{},"node_modules/alias":{"link":true,"resolved":"node_modules/a"},
    "node_modules/a":{"peerDependencies":{"p":"*"}},
    "node_modules/a/node_modules/p":{"version":"1"}}})");
  EXPECT_EQ(edge(A, "node_modules/a", "p", "peerDependencies").Status,
            "peer_placement_conflict");
}

TEST_F(WebPackages, NormalizedOptionalOverridesDoNotConflictWithManifest) {
  write("package-lock.json", R"({"lockfileVersion":3,"packages":{
    "":{"optionalDependencies":{"a":"2"}},"node_modules/a":{"version":"2"}}})");
  write("package.json",
        R"({"dependencies":{"a":"1"},"optionalDependencies":{"a":"2"}})");
  EXPECT_EQ(inspect("package-lock.json").Packages.front().ManifestStatus,
            "supplied_declarations");
  write("package.json",
        R"({"dependencies":{"a":"1"},"optionalDependencies":{"a":"3"}})");
  EXPECT_EQ(inspect("package-lock.json").Packages.front().ManifestStatus,
            "conflicting_declarations");
}

TEST_F(WebPackages, PublicRecordIdentitiesDoNotHashIndividualPrivateText) {
  const auto A =
      single(R"({"name":"app","dependencies":{"private-dependency":"1"},
    "scripts":{"private-script":"inert"}})",
             "package-json");
  ASSERT_EQ(A.Packages.size(), 1);
  ASSERT_EQ(A.Dependencies.size(), 1);
  ASSERT_EQ(A.Scripts.size(), 1);
  const auto &P = A.Packages.front();
  EXPECT_NE(P.ID, identity("package-instance", {A.ID, P.Location}));
  EXPECT_NE(A.Dependencies.front().ID,
            identity("package-dependency",
                     {A.ID, P.ID, "dependencies", "private-dependency"}));
  EXPECT_NE(
      A.Scripts.front().ID,
      identity("package-script", {A.ID, P.ID, A.ArtifactID, "private-script"}));
}

TEST_F(WebPackages, NewlySuppliedIdentityAndConditionsAreCoverageChanges) {
  const auto *Lock =
      R"({"lockfileVersion":3,"packages":{"":{},"node_modules/a":{}}})";
  write("before/package-lock.json", Lock);
  write("after/package-lock.json", Lock);
  write("after/node_modules/a/package.json",
        R"({"name":"a","version":"1","os":"linux"})");
  const auto D = comparePackages(inspect("before/package-lock.json"),
                                 inspect("after/package-lock.json"));
  unsigned Evidence = 0;
  for (const auto &C : D.Changes)
    if (C.Field == "declared_name" || C.Field == "version_contract" ||
        C.Field == "platform_conditions") {
      EXPECT_EQ(C.Kind, "declaration_coverage_changed");
      ++Evidence;
    }
  EXPECT_EQ(Evidence, 3);
}

TEST_F(WebPackages, RelativeLegacyRegistryOriginsRemainUncompared) {
  const auto A = single(R"({"lockfileVersion":2,"packages":{
    "":{},"node_modules/a":{"version":"1.0.0","resolved":"https://registry.npmjs.org/a/-/a-1.0.0.tgz"}},
    "dependencies":{"a":{"version":"1.0.0","resolved":"a/-/a-1.0.0.tgz"}}})");
  EXPECT_EQ(A.LegacyStatus, "legacy_tree_partially_compared");
}
#endif
} // namespace
