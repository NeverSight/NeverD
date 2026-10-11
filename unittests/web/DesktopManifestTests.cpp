//===- DesktopManifestTests.cpp - Desktop manifest contracts --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exact entry ownership, grammar boundaries and independent framework
/// profiles.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/web/DesktopManifest.h"
#include "neverd/web/Error.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

namespace {
using namespace neverd::web;
Snapshot input(std::string_view JSON,
               std::string_view Path = "extension/package.json") {
  Snapshot S;
  S.ID = "namespace";
  Artifact Root;
  Root.ID = "root";
  Root.Directory = true;
  S.Artifacts.push_back(Root);
  Artifact M;
  M.ID = "manifest";
  M.MemberPath = Path;
  M.BlobHash = sha256(JSON);
  S.Artifacts.push_back(M);
  return S;
}
void member(Snapshot &S, std::string_view ID, std::string_view Path,
            bool Directory = false) {
  Artifact A;
  A.ID = ID;
  A.MemberPath = Path;
  A.Directory = Directory;
  S.Artifacts.push_back(A);
}
std::string declaration(std::string_view Key, std::string_view Path) {
  return llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                  {llvm::StringRef(Key), std::string(Path)}}))
      .str();
}

TEST(WebDesktopManifest, ProfilesHaveIndependentIdentitiesAndExactParentScope) {
  const auto JSON = R"({"name":"secret","version":"1","publisher":"secret",
    "engines":{"vscode":"^1"},"main":"./out/main.js","browser":"web.js"})";
  auto S = input(JSON);
  member(S, "outer", "out/main.js");
  member(S, "inner", "extension/out/main.js");
  member(S, "browser", "extension/web.js");
  const auto V = analyzeDesktopManifest(S, "manifest", JSON, "vsix");
  EXPECT_EQ(V.Profile, VSIXManifestProfile);
  EXPECT_EQ(V.Layout, "vsix_extension_path_candidate");
  EXPECT_EQ(V.Declarations.at("required_field_shapes"), "present");
  ASSERT_EQ(V.Entries.size(), 2);
  EXPECT_EQ(V.Entries[0].ArtifactID, "inner");
  EXPECT_EQ(V.Entries[1].ArtifactID, "browser");
  EXPECT_EQ(V.Entries[0].ContentKind, "javascript_filename_candidate");
  const auto N = analyzeDesktopManifest(S, "manifest", JSON, "nwjs");
  EXPECT_NE(N.ID, V.ID);
  EXPECT_NE(N.Entries[0].ID, V.Entries[0].ID);
  EXPECT_EQ(N.Entries[0].ArtifactID, "inner");
  EXPECT_EQ(N.Entries.size(), 5);
  EXPECT_EQ(N.Layout, "unrecognized_placement");
  S.ID = "other-container";
  EXPECT_NE(analyzeDesktopManifest(S, "manifest", JSON, "vsix").ID, V.ID);
}

TEST(WebDesktopManifest, DocumentedNWDirectoryLayoutsKeepSeparateDeclarations) {
  const auto JSON = R"({"name":"app","version":"private","main":"index.html",
    "node-main":"node.cjs","bg-script":"background.js","nodejs":false,
    "inject_js_start":"start.js","inject_js_end":"end.js"})";
  for (const auto *Prefix :
       {"", "package.nw/", "app.nw/", "App.app/Contents/Resources/app.nw/"}) {
    auto S = input(JSON, std::string(Prefix) + "package.json");
    for (const auto *File :
         {"index.html", "node.cjs", "background.js", "start.js", "end.js"})
      member(S, File, std::string(Prefix) + File);
    const auto A = analyzeDesktopManifest(S, "manifest", JSON, "nwjs");
    EXPECT_NE(A.Layout, "unrecognized_placement");
    EXPECT_EQ(A.Declarations.at("nodejs"), "false");
    EXPECT_EQ(A.Declarations.at("required_field_shapes"), "present");
    for (const auto &E : A.Entries)
      EXPECT_EQ(E.LinkStatus, "exact_admitted_file_candidate");
    EXPECT_EQ(A.Entries[0].ContentKind, "html_filename_candidate");
  }
}

TEST(WebDesktopManifest, URLsAndCommandLinesNeverBecomeLiteralFilenameMatches) {
  for (const auto *Path :
       {"index.html#route", "index.html?token=secret", "a%2fb.html",
        "https://secret.invalid", "//host/x", "data:text/html,secret",
        " index.html", "a\tb.html", "../index.html", "a/../index.html",
        "/index.html", "a\\index.html", "a//index.html", "CON"}) {
    const auto JSON = declaration("main", Path);
    auto S = input(JSON, "package.json");
    member(S, "confuser", Path);
    const auto A = analyzeDesktopManifest(S, "manifest", JSON, "nwjs");
    EXPECT_TRUE(A.Entries[0].ArtifactID.empty()) << Path;
  }
  for (const auto *Path : {"--inspect node.js", "'node.js'", "node file.js",
                           "node.js --flag", "\"node.js\"", "-node.js"}) {
    const auto JSON = declaration("node-main", Path);
    auto S = input(JSON, "package.json");
    member(S, "confuser", Path);
    const auto A = analyzeDesktopManifest(S, "manifest", JSON, "nwjs");
    EXPECT_EQ(A.Entries[1].LinkStatus, "command_line_syntax_not_analyzed");
    EXPECT_TRUE(A.Entries[1].ArtifactID.empty());
  }
}

TEST(WebDesktopManifest,
     MissingUnsupportedAndExtensionlessEntriesStayDistinct) {
  for (const auto *JSON : {"{}", R"({"contributes":{"secret":{}}})"}) {
    const auto A =
        analyzeDesktopManifest(input(JSON), "manifest", JSON, "vsix");
    EXPECT_EQ(A.Entries[0].LinkStatus, "not_declared");
    EXPECT_EQ(A.Entries[1].LinkStatus, "not_declared");
  }
  const auto JSON = R"({"main":"main","browser":{"secret":"secret.js"}})";
  auto S = input(JSON);
  member(S, "no-fallback", "extension/main.js");
  auto A = analyzeDesktopManifest(S, "manifest", JSON, "vsix");
  EXPECT_EQ(A.Entries[0].LinkStatus, "no_exact_admitted_file");
  EXPECT_EQ(A.Entries[1].LinkStatus, "unsupported_entry_type");
  member(S, "directory", "extension/main", true);
  A = analyzeDesktopManifest(S, "manifest", JSON, "vsix");
  EXPECT_EQ(A.Entries[0].LinkStatus, "directory_requires_runtime_resolution");
  S.Artifacts = {S.Artifacts[1]};
  S.Artifacts[0].MemberPath.clear();
  A = analyzeDesktopManifest(S, "manifest", JSON, "vsix");
  EXPECT_EQ(A.Layout, "not_observed");
  EXPECT_EQ(A.Entries[0].LinkStatus, "no_directory_namespace");
  for (const auto *Value : {"null", "false", "17", "[]", "{}"}) {
    const auto Invalid = std::string("{\"main\":") + Value + "}";
    const auto Result =
        analyzeDesktopManifest(input(Invalid), "manifest", Invalid, "nwjs");
    EXPECT_EQ(Result.Entries[0].LinkStatus, "unsupported_entry_type");
  }
}

TEST(WebDesktopManifest, MalformedHashNameNamespaceAndBudgetsRefuse) {
  for (const auto *JSON : {"[1]", "{", R"({"main":"x","\u006dain":"y"})"})
    EXPECT_THROW(analyzeDesktopManifest(input(JSON), "manifest", JSON, "vsix"),
                 Error);
  EXPECT_THROW(analyzeDesktopManifest(input("{}"), "manifest", "[]", "vsix"),
               Error);
  EXPECT_THROW(analyzeDesktopManifest(input("{}"), "manifest", "{}", "guess"),
               Error);
  EXPECT_THROW(analyzeDesktopManifest(input("{}", "other.json"), "manifest",
                                      "{}", "nwjs"),
               Error);
  auto S = input("{}");
  member(S, "duplicate-path", "extension/package.json");
  EXPECT_THROW(analyzeDesktopManifest(S, "manifest", "{}", "vsix"), Error);
  S = input("{}");
  member(S, "manifest", "other/package.json");
  EXPECT_THROW(analyzeDesktopManifest(S, "manifest", "{}", "vsix"), Error);
  const std::string Huge(MaxDesktopManifestBytes + 1, ' ');
  EXPECT_THROW(analyzeDesktopManifest(input(Huge), "manifest", Huge, "vsix"),
               Error);
  EXPECT_EQ(analyzeDesktopManifest(input("{}", "node_modules/x/package.json"),
                                   "manifest", "{}", "vsix")
                .Layout,
            "unrecognized_placement");
}

TEST(WebDesktopManifest, JSONAndNamespaceWorkHaveIndependentBounds) {
  auto Nested = std::string(40, '[') + "0" + std::string(40, ']');
  auto JSON = "{\"contributes\":" + Nested + "}";
  EXPECT_THROW(analyzeDesktopManifest(input(JSON), "manifest", JSON, "vsix"),
               Error);
  JSON = "{\"contributes\":[0";
  for (unsigned I = 0; I < 20000; ++I)
    JSON += ",0";
  JSON += "]}";
  EXPECT_THROW(analyzeDesktopManifest(input(JSON), "manifest", JSON, "vsix"),
               Error);
  auto S = input("{}");
  member(S, "long-path", std::string(4097, 'x'));
  EXPECT_THROW(analyzeDesktopManifest(S, "manifest", "{}", "vsix"), Error);
  S = input("{}");
  S.Artifacts.resize(10002);
  EXPECT_THROW(analyzeDesktopManifest(S, "manifest", "{}", "vsix"), Error);
  S = input("{}");
  for (unsigned I = 0; I < 2100; ++I)
    member(S, std::to_string(I), std::string(4000, 'x') + std::to_string(I));
  EXPECT_THROW(analyzeDesktopManifest(S, "manifest", "{}", "vsix"), Error);
}
} // namespace
