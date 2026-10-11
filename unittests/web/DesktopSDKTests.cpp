//===- DesktopSDKTests.cpp - Desktop manifest API contracts ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Directory/ZIP entries, offline consumers, redaction and revision lifetime.
///
//===----------------------------------------------------------------------===//

#include "AsarFixture.h"
#include "BunFixture.h"
#include "SEAFixture.h"
#include "ZipFixture.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
using namespace neverd::web::test;
using Value = llvm::json::Value;
namespace fs = std::filesystem;
Value take(const char *Owned) {
  if (!Owned)
    throw std::runtime_error("missing desktop result");
  const std::string Text(Owned);
  neverd_free_string(Owned);
  EXPECT_EQ(Text.find("CANARY"), std::string::npos);
  auto Parsed = llvm::json::parse(Text);
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    throw std::runtime_error("invalid desktop result");
  }
  return std::move(*Parsed);
}
std::string field(const Value &V, const char *Key) {
  return V.getAsObject()->getString(Key).value_or("").str();
}
std::string code(const Value &V) {
  EXPECT_EQ(field(V, "status"), "error");
  return V.getAsObject()->getObject("error")->getString("code")->str();
}
const Value &entry(const Value &V, size_t I = 0) {
  return (*V.getAsObject()->getArray("entries"))[I];
}
bool zipAvailable(bool Deflate = false) {
  auto V = take(neverd_web_capabilities_json());
  for (const auto &A : *V.getAsObject()->getArray("analysis"))
    if (field(A, "kind") == "package_archive")
      return A.getAsObject()->getBoolean("available").value_or(false) &&
             (!Deflate || A.getAsObject()
                              ->getBoolean("zip_deflate_available")
                              .value_or(false));
  return false;
}
class WebDesktopSDK : public ::testing::Test {
protected:
  neverd_web_session_t S = nullptr;
  fs::path Root, Input;
  std::string Revision;
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX capture qualification required";
#endif
    S = neverd_web_session_create();
    ASSERT_NE(S, nullptr);
    llvm::SmallString<128> P;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-desktop", P));
    Root = P.str().str();
    Input = Root / "input";
    fs::create_directory(Input);
  }
  void TearDown() override {
    neverd_web_session_destroy(S);
    std::error_code EC;
    if (!Root.empty())
      fs::remove_all(Root, EC);
  }
  void write(std::string_view Name, std::string_view Bytes) {
    const auto P = Input / std::string(Name);
    fs::create_directories(P.parent_path());
    std::ofstream F(P, std::ios::binary);
    F.write(Bytes.data(), Bytes.size());
    ASSERT_TRUE(F.good());
  }
  void capture(fs::path Path = {}) {
    const auto P = (Path.empty() ? Input : Path).string();
    const auto Preview =
        take(neverd_web_import_preview_json(S, P.data(), P.size(), nullptr, 0));
    const auto Token = field(Preview, "preview_token");
    Revision = field(
        take(neverd_web_import_commit_json(S, Token.data(), Token.size())),
        "revision");
  }
  Value artifacts() {
    return take(
        neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 0, 128));
  }
  std::string artifact(std::string_view Bytes) {
    const auto Page = artifacts();
    for (const auto &A : *Page.getAsObject()->getArray("items"))
      if (field(A, "blob_sha256") == asarHash(Bytes))
        return field(A, "artifact_id");
    throw std::runtime_error("missing fixture artifact");
  }
  Value manifest(std::string_view ID, std::string_view Kind) {
    return take(neverd_web_desktop_manifest_analyze_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Kind.data(),
        Kind.size()));
  }
  Value archive(std::string_view ID) {
    const auto A = take(neverd_web_package_archive_extract_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), "zip", 3));
    const auto Archive = field(A, "archive_id");
    return take(neverd_web_package_archive_records_json(
        S, Revision.data(), Revision.size(), Archive.data(), Archive.size(), 0,
        128));
  }
};

TEST_F(WebDesktopSDK, ProfilesRetainExactNamespaceAndRedactDeclarations) {
  const std::string JSON = R"({"main":"./out/main.js","name":"CANARY_NAME",
    "version":"CANARY_VERSION","publisher":"CANARY_PUBLISHER",
    "engines":{"vscode":"CANARY_COMPAT"},"nodejs":false,
    "activationEvents":["CANARY_EVENT"],"contributes":{"CANARY_KEY":{}}})";
  write("extension/package.json", JSON);
  write("extension/out/main.js", "export const inside = 1;");
  write("out/main.js", "export const outside = 2;");
  capture();
  const auto MID = artifact(JSON);
  const auto V = manifest(MID, "vsix");
  const auto N = manifest(MID, "nwjs");
  ASSERT_EQ(field(V, "status"), "ok");
  ASSERT_EQ(field(N, "status"), "ok");
  EXPECT_NE(field(V, "manifest_id"), field(N, "manifest_id"));
  EXPECT_EQ(field(manifest(MID, "vsix"), "manifest_id"),
            field(V, "manifest_id"));
  const auto ID = artifact("export const inside = 1;");
  EXPECT_EQ(field(entry(V), "artifact_id"), ID);
  EXPECT_EQ(field(entry(N), "artifact_id"), ID);
  EXPECT_EQ(field(V, "layout_candidate"), "vsix_extension_path_candidate");
  EXPECT_EQ(V.getAsObject()->getBoolean("framework_verified"), false);
  EXPECT_EQ(V.getAsObject()->getBoolean("runtime_entry_verified"), false);
  EXPECT_EQ(field(V, "activation_analysis"), "not_analyzed");
  const auto Source = take(neverd_web_source_analyze_json(
      S, Revision.data(), Revision.size(), ID.data(), ID.size(), "module", 6));
#if NEVERD_TEST_WEB_JAVASCRIPT
  EXPECT_EQ(field(Source, "parse_status"), "parsed");
#else
  EXPECT_EQ(code(Source), "capability_unavailable");
#endif
  const auto OldRevision = Revision;
  capture();
  EXPECT_EQ(code(take(neverd_web_desktop_manifest_analyze_json(
                S, OldRevision.data(), OldRevision.size(), MID.data(),
                MID.size(), "vsix", 4))),
            "stale_revision");
  EXPECT_EQ(take(neverd_web_metadata_json(S))
                .getAsObject()
                ->getInteger("desktop_manifest_count"),
            0);
}

TEST_F(WebDesktopSDK, NWDirectoryLayoutsLinkHTMLWithoutJavaScriptParser) {
  const std::string JSON = R"({"name":"CANARY_APP","main":"index.html"})";
  const std::string HTML = "<script>const x = 1;</script>";
  for (const auto *Prefix :
       {"", "package.nw/", "app.nw/", "App.app/Contents/Resources/app.nw/"}) {
    fs::remove_all(Input);
    write(std::string(Prefix) + "package.json", JSON);
    write(std::string(Prefix) + "index.html", HTML);
    capture();
    const auto A = manifest(artifact(JSON), "nwjs");
    ASSERT_EQ(field(A, "status"), "ok");
    EXPECT_NE(field(A, "layout_candidate"), "unrecognized_placement");
    const auto ID = field(entry(A), "artifact_id");
    EXPECT_EQ(ID, artifact(HTML));
    const auto H = take(neverd_web_html_analyze_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size()));
    EXPECT_EQ(field(H, "status"), "ok");
  }
}

TEST_F(WebDesktopSDK, ZIPManifestEntriesAndEmbeddedHelperKeepContainerOrigins) {
  if (!zipAvailable())
    GTEST_SKIP() << "Archive policy unavailable";
  const std::string JSON = R"({"main":"entry.js","browser":"web.js",
    "version":"CANARY_EXTENSION_VERSION"})";
  const BunFixture B;
  for (const uint16_t Method : {0, 8}) {
    if (Method == 8 && !zipAvailable(true))
      continue;
    const ZipFixture Z({{"extension/package.json", JSON, Method},
                        {"extension/entry.js", "export const x = 1;", Method},
                        {"entry.js", "export const wrong = 2;", Method},
                        {"extension/web.js", "export const web = 3;", Method},
                        {"extension/helper", B.Bytes, Method},
                        {"extension/opaque.jsc", "CANARY_OPAQUE", 99}});
    write("app.vsix", Z.Bytes);
    capture(Input / "app.vsix");
    const auto Page = archive(artifact(Z.Bytes));
    const auto &Items = *Page.getAsObject()->getArray("items");
    ASSERT_EQ(Items.size(), 6);
    const auto M = manifest(field(Items[0], "member_id"), "vsix");
    ASSERT_EQ(field(M, "status"), "ok");
    EXPECT_EQ(field(entry(M), "artifact_id"), field(Items[1], "member_id"));
    EXPECT_EQ(field(entry(M, 1), "artifact_id"), field(Items[3], "member_id"));
    EXPECT_EQ(M.getAsObject()->getObject("origin")->getString("format"), "zip");
    const auto Helper = field(Items[4], "member_id");
    const auto Bun = take(neverd_web_bun_extract_json(
        S, Revision.data(), Revision.size(), Helper.data(), Helper.size()));
    EXPECT_EQ(Bun.getAsObject()->getInteger("module_count"), 3);
    EXPECT_EQ(field(M, "helper_version_association"), "not_analyzed");
    EXPECT_EQ(code(manifest(field(Items[5], "member_id"), "vsix")),
              "artifact_bytes_unavailable");
    const auto NW = manifest(field(Items[0], "member_id"), "nwjs");
    EXPECT_EQ(field(entry(NW), "artifact_id"), field(Items[1], "member_id"));
  }
}

TEST_F(WebDesktopSDK, OpaqueKeyAssetsDoNotBorrowCapturedNeighbors) {
  const std::string JSON = R"({"main":"main.js","name":"CANARY_ASSET"})";
  const BunFixture B(false, {}, JSON);
  write("helper", B.Bytes);
  write("main.js", "export const outer = 2;");
  capture();
  const auto ID = artifact(B.Bytes);
  const auto Bun = take(neverd_web_bun_extract_json(
      S, Revision.data(), Revision.size(), ID.data(), ID.size()));
  const auto EID = field(Bun, "extraction_id");
  const auto Modules = take(
      neverd_web_bun_records_json(S, Revision.data(), Revision.size(),
                                  EID.data(), EID.size(), "modules", 7, 2, 1));
  const auto Asset = field((*Modules.getAsObject()->getArray("items"))[0],
                           "content_region_id");
  const auto A = manifest(Asset, "nwjs");
  ASSERT_EQ(field(A, "status"), "ok");
  EXPECT_EQ(field(entry(A), "link_status"), "no_directory_namespace");
  EXPECT_EQ(field(A, "layout_candidate"), "not_observed");

  namespace Sea = neverd::web::sea_test;
  std::string Blob;
  Sea::append(Blob, 0x143da20, 4);
  Sea::append(Blob, 8, 4);
  Sea::string(Blob, "CANARY_MAIN");
  Sea::string(Blob, "const x = 1;");
  Sea::append(Blob, 1);
  Sea::string(Blob, "package.json");
  Sea::string(Blob, JSON);
  write("sea.blob", Blob);
  capture();
  const auto SeaID = artifact(Blob);
  const std::string Profile = "node-sea-22.15.0-blob-le64-v1";
  const auto Extraction = take(neverd_web_sea_extract_json(
      S, Revision.data(), Revision.size(), SeaID.data(), SeaID.size(),
      Profile.data(), Profile.size()));
  const auto ExtractionID = field(Extraction, "extraction_id");
  const auto Regions = take(neverd_web_sea_records_json(
      S, Revision.data(), Revision.size(), ExtractionID.data(),
      ExtractionID.size(), 0, 128));
  unsigned Assets = 0;
  for (const auto &R : *Regions.getAsObject()->getArray("items"))
    if (field(R, "kind") == "asset") {
      ++Assets;
      const auto M = manifest(field(R, "region_id"), "vsix");
      EXPECT_EQ(field(M, "status"), "ok");
      EXPECT_EQ(field(entry(M), "link_status"), "no_directory_namespace");
    }
  EXPECT_EQ(Assets, 1);
}

TEST_F(WebDesktopSDK, ErrorsAreAtomicAndCacheIsProfileBounded) {
  for (unsigned I = 0; I < 9; ++I)
    write(std::to_string(I) + "/package.json",
          "{\"name\":\"CANARY_" + std::to_string(I) + "\"}");
  write("bad/package.json", "{");
  capture();
  const auto Bad = artifact("{");
  for (unsigned I = 0; I < 20; ++I)
    EXPECT_EQ(field(manifest(Bad, "vsix"), "status"), "error");
  for (unsigned I = 0; I < 8; ++I) {
    const auto ID = artifact("{\"name\":\"CANARY_" + std::to_string(I) + "\"}");
    EXPECT_EQ(field(manifest(ID, "vsix"), "status"), "ok");
    EXPECT_EQ(field(manifest(ID, "nwjs"), "status"), "ok");
  }
  EXPECT_EQ(code(manifest(artifact(R"({"name":"CANARY_8"})"), "vsix")),
            "desktop_manifest_cache_budget_exceeded");
  EXPECT_EQ(
      field(manifest(artifact(R"({"name":"CANARY_0"})"), "vsix"), "status"),
      "ok");
  EXPECT_EQ(take(neverd_web_metadata_json(S))
                .getAsObject()
                ->getInteger("desktop_manifest_count"),
            16);
}

TEST_F(WebDesktopSDK, ClaudeCode21296VSIXEntriesWhenSupplied) {
  const auto *Path = std::getenv("NEVERD_CLAUDE_CODE_21296_VSIX_LINUX_X64");
  if (!Path || !zipAvailable(true))
    GTEST_SKIP() << "Optional pinned VSIX or archive policy unavailable";
  capture(Path);
  const auto A = artifacts();
  const auto &Original = (*A.getAsObject()->getArray("items"))[0];
  ASSERT_EQ(field(Original, "blob_sha256"),
            "31176c4a2a29144673170dfb9affc2589af997d01532f1f7f97f8b93999a3d11");
  const auto Page = archive(field(Original, "artifact_id"));
  const auto &Members = *Page.getAsObject()->getArray("items");
  ASSERT_EQ(Members.size(), 29);
  // Test every available JSON candidate using its captured bytes; the unique
  // package.json location is admitted by the manifest reader, not guessed here.
  unsigned Matches = 0;
  for (size_t Index = 0; Index < Members.size(); ++Index) {
    const auto &M = Members[Index];
    if (field(M, "kind") != "file")
      continue;
    const auto Result = manifest(field(M, "member_id"), "vsix");
    if (field(Result, "status") != "ok")
      continue;
    ++Matches;
    EXPECT_EQ(field(Result, "layout_candidate"),
              "vsix_extension_path_candidate");
    EXPECT_EQ(field(entry(Result), "link_status"),
              "exact_admitted_file_candidate");
    EXPECT_EQ(
        Result.getAsObject()->getObject("declarations")->getString("version"),
        "string");
    std::cout << "Pinned VSIX manifest index: " << Index << '\n';
    std::cout << "Pinned VSIX main artifact: "
              << field(entry(Result), "artifact_id") << '\n';
  }
  EXPECT_EQ(Matches, 1);
}

#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebDesktopSDK, CLISelectsDirectoryAndZIPWithoutExternalTools) {
  if (!zipAvailable())
    GTEST_SKIP() << "Archive policy unavailable";
  const std::string JSON = R"({"name":"CANARY_NAME","main":"entry.js"})";
  const ZipFixture Z({{"package.json", JSON}, {"entry.js", "const x = 1;"}});
  write("package.json", JSON);
  write("entry.js", "const x = 1;");
  write("app.nw", Z.Bytes);
  capture();
  const auto Page = artifacts();
  const auto &Captured = *Page.getAsObject()->getArray("items");
  size_t ManifestIndex = 0;
  while (ManifestIndex < Captured.size() &&
         field(Captured[ManifestIndex], "blob_sha256") != asarHash(JSON))
    ++ManifestIndex;
  ASSERT_LT(ManifestIndex, Captured.size());
  const auto Index = std::to_string(ManifestIndex);
  const auto ExpectedEntry = artifact("const x = 1;");
  const std::string CLI = NEVERD_WEB_TEST_CLI;
  const std::string Single = (Input / "package.json").string();
  const std::string Directory = Input.string();
  const std::string Archive = (Input / "app.nw").string();
  const llvm::StringRef Environment[] = {"PATH=/neverd-no-external-tools"};
  for (const auto *Kind : {"nwjs", "vsix"}) {
    for (unsigned Mode = 0; Mode < 3; ++Mode) {
      const auto Stem = std::string(Kind) + "-" + std::to_string(Mode);
      const auto Out = (Root / (Stem + ".stdout")).string();
      const auto Err = (Root / (Stem + ".stderr")).string();
      const std::optional<llvm::StringRef> Redirects[] = {"", Out, Err};
      std::vector<llvm::StringRef> Args{CLI, "web"};
      if (Mode == 2)
        Args.insert(Args.end(),
                    {"archive-desktop", Archive, "zip", "0", "0", Kind});
      else if (Mode == 1)
        Args.insert(Args.end(), {"desktop-manifest", Directory, Kind, Index});
      else
        Args.insert(Args.end(), {"desktop-manifest", Single, Kind});
      std::string Error;
      EXPECT_EQ(llvm::sys::ExecuteAndWait(CLI, Args, Environment, Redirects, 30,
                                          0, &Error),
                0)
          << Error;
      std::ifstream F(Out), E(Err);
      const std::string Text{std::istreambuf_iterator<char>(F), {}};
      const std::string Errors{std::istreambuf_iterator<char>(E), {}};
      EXPECT_EQ(Text.find("CANARY"), std::string::npos);
      EXPECT_EQ(Errors.find("CANARY"), std::string::npos);
      EXPECT_NE(Text.find("manifest_id"), std::string::npos);
      std::istringstream Lines(Text);
      std::string Line;
      unsigned Manifests = 0;
      while (std::getline(Lines, Line)) {
        auto V = llvm::json::parse(Line);
        ASSERT_TRUE(bool(V))
            << "mode=" << Mode << " kind=" << Kind << " line=" << Line;
        if (!V->getAsObject()->getString("manifest_id"))
          continue;
        ++Manifests;
        EXPECT_EQ(field(entry(*V), "link_status"),
                  Mode == 0 ? "no_directory_namespace"
                            : "exact_admitted_file_candidate");
        if (Mode == 1)
          EXPECT_EQ(field(entry(*V), "artifact_id"), ExpectedEntry);
      }
      EXPECT_EQ(Manifests, 1);
    }
  }
}
#endif
} // namespace
