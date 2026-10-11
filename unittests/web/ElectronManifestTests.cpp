//===- ElectronManifestTests.cpp - Captured Electron manifests tests ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Captured Electron manifests tests.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/web/Electron.h"
#include "neverd/web/Session.h"

namespace {
using namespace neverd::web;
Snapshot input(std::string_view JSON, std::string_view Main = "app/main.js") {
  Snapshot S;
  S.ID = "captured-tree";
  Artifact Root;
  Root.ID = "root";
  Root.Directory = true;
  S.Artifacts.push_back(Root);
  Artifact Manifest;
  Manifest.ID = "manifest";
  Manifest.ParentID = Root.ID;
  Manifest.MemberPath = "app/package.json";
  Manifest.BlobHash = sha256(JSON);
  S.Artifacts.push_back(Manifest);
  Artifact Entry;
  Entry.ID = "entry";
  Entry.ParentID = Root.ID;
  Entry.MemberPath = Main;
  S.Artifacts.push_back(Entry);
  return S;
}

TEST(WebElectronManifest, DeclaredEntryUsesOnlyItsCapturedDirectoryNamespace) {
  const std::string JSON =
      R"({"main":"./main.js","type":"module","name":"private-name","version":"private-version","productName":"private-product","devDependencies":{"electron":"private-range"}})";
  auto S = input(JSON);
  const auto M = analyzeElectronManifest(S, "manifest", JSON);
  EXPECT_EQ(M.MainValueStatus, "declared_string");
  EXPECT_EQ(M.MainArtifactID, "entry");
  EXPECT_EQ(M.MainLinkStatus, "exact_admitted_file_candidate");
  EXPECT_EQ(M.SourceType, "module");
  EXPECT_TRUE(M.NamePresent && M.VersionPresent && M.ProductNamePresent &&
              M.ElectronDependencyPresent);
  EXPECT_EQ(M.ID, analyzeElectronManifest(S, "manifest", JSON).ID);
  S.Artifacts[2].MemberPath = "elsewhere/main.js";
  EXPECT_EQ(analyzeElectronManifest(S, "manifest", JSON).MainLinkStatus,
            "no_exact_admitted_file");
  S.Artifacts[2].MemberPath = "app/main.js";
  S.Artifacts[2].Directory = true;
  EXPECT_EQ(analyzeElectronManifest(S, "manifest", JSON).MainLinkStatus,
            "directory_requires_runtime_resolution");
}

TEST(WebElectronManifest,
     DefaultsAndExplicitESMExtensionsFollowThePinnedEntryRule) {
  for (const auto JSON : {"{}", "{\"main\":null}", "{\"main\":false}",
                          "{\"main\":0}", "{\"main\":\"\"}"}) {
    const auto M =
        analyzeElectronManifest(input(JSON, "app/index.js"), "manifest", JSON);
    EXPECT_EQ(M.MainValueStatus, "default_index_js");
    EXPECT_EQ(M.MainArtifactID, "entry");
    EXPECT_EQ(M.SourceType, "commonjs");
  }
  for (const auto &Pair :
       {std::pair{"{\"main\":\"entry.mjs\"}", "module"},
        std::pair{"{\"main\":\"entry.cjs\",\"type\":\"module\"}",
                  "commonjs"}}) {
    EXPECT_EQ(analyzeElectronManifest(input(Pair.first), "manifest", Pair.first)
                  .SourceType,
              Pair.second);
  }
  const std::string Unsupported = R"({"main":{"path":"private"}})";
  EXPECT_EQ(analyzeElectronManifest(input(Unsupported), "manifest", Unsupported)
                .MainValueStatus,
            "unsupported_main_type");
}

TEST(WebElectronManifest, PathsNeverEscapeOrInvokeExtensionAndIndexResolution) {
  for (const auto JSON :
       {R"({"main":"../other.js"})", R"({"main":"/private.js"})",
        R"({"main":"C:\\private.js"})", R"({"main":"a/../main.js"})",
        R"({"main":"https://private.invalid"})", R"({"main":"CON"})"}) {
    const auto M = analyzeElectronManifest(input(JSON), "manifest", JSON);
    EXPECT_TRUE(M.MainArtifactID.empty());
    EXPECT_NE(M.MainLinkStatus, "exact_admitted_file_candidate");
  }
  const std::string JSON = R"({"main":"main"})";
  EXPECT_EQ(
      analyzeElectronManifest(input(JSON), "manifest", JSON).MainLinkStatus,
      "no_exact_admitted_file");
  auto Single = input("{}");
  Single.Artifacts.erase(Single.Artifacts.begin());
  Single.Artifacts.resize(1);
  Single.Artifacts[0].MemberPath.clear();
  EXPECT_EQ(analyzeElectronManifest(Single, "manifest", "{}").MainLinkStatus,
            "no_directory_namespace");
}

TEST(WebElectronManifest, MalformedAndDuplicateJSONNeverProducesAResult) {
  for (const auto JSON : {"[]", "{", R"({"main":"a","\u006dain":"b"})"})
    EXPECT_THROW(analyzeElectronManifest(input(JSON), "manifest", JSON), Error);
  const std::string Huge(MaxElectronManifestBytes + 1, ' ');
  EXPECT_THROW(analyzeElectronManifest(input(Huge), "manifest", Huge), Error);
  EXPECT_THROW(
      analyzeElectronManifest(input("{}"), "manifest", "{\"main\":\"other\"}"),
      Error);
  auto Duplicate = input("{}");
  Duplicate.Artifacts.push_back(Duplicate.Artifacts[1]);
  EXPECT_THROW(analyzeElectronManifest(Duplicate, "manifest", "{}"), Error);
}
} // namespace
