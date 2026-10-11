//===- SEASDKTests.cpp - SEA API and consumer regressions ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Publication, redaction, nested origins and runtime-free CLI tests.
///
//===----------------------------------------------------------------------===//

#include "NativeFixture.h"
#include "PackageArchiveFixture.h"
#include "SEAFixture.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <filesystem>
#include <fstream>

namespace {
using namespace neverd::web::sea_test;
using Value = llvm::json::Value;
constexpr std::string_view Profile = "node-sea-22.15.0-blob-le64-v1";
Value take(const char *Owned) {
  if (!Owned)
    throw std::runtime_error("missing response");
  const std::string Text(Owned);
  neverd_free_string(Owned);
  EXPECT_EQ(Text.find("CANARY"), std::string::npos);
  auto V = llvm::json::parse(Text);
  if (!V) {
    llvm::consumeError(V.takeError());
    throw std::runtime_error("json");
  }
  return std::move(*V);
}
std::string field(const Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
std::string code(const Value &V) {
  EXPECT_EQ(field(V, "status"), "error");
  return V.getAsObject()->getObject("error")->getString("code")->str();
}
class WebSEASDK : public ::testing::Test {
protected:
  neverd_web_session_t S = nullptr;
  std::filesystem::path Root;
  std::string Path, Revision, Artifact;
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX capture required";
#endif
    llvm::SmallString<128> D;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-sea-sdk", D));
    Root = D.str().str();
    Path = (Root / "CANARY_INPUT").string();
    S = neverd_web_session_create();
    ASSERT_NE(S, nullptr);
  }
  void TearDown() override {
    neverd_web_session_destroy(S);
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }
  void capture(std::string_view Bytes) {
    {
      std::ofstream F(Path, std::ios::binary);
      F.write(Bytes.data(), Bytes.size());
    }
    open(Path);
  }
  void open(const std::string &Selected) {
    const auto P = take(neverd_web_import_preview_json(
        S, Selected.data(), Selected.size(), nullptr, 0));
    const auto T = field(P, "preview_token");
    Revision = field(take(neverd_web_import_commit_json(S, T.data(), T.size())),
                     "revision");
    Artifact = field((*take(neverd_web_artifacts_json(S, Revision.data(),
                                                      Revision.size(), 0, 1))
                           .getAsObject()
                           ->getArray("items"))[0],
                     "artifact_id");
  }
  Value extract(std::string_view P = Profile, std::string_view A = {}) {
    if (A.empty())
      A = Artifact;
    return take(neverd_web_sea_extract_json(S, Revision.data(), Revision.size(),
                                            A.data(), A.size(), P.data(),
                                            P.size()));
  }
  Value page(const std::string &ID, uint64_t Offset = 0, uint64_t Limit = 128) {
    return take(neverd_web_sea_records_json(S, Revision.data(), Revision.size(),
                                            ID.data(), ID.size(), Offset,
                                            Limit));
  }
};
TEST_F(WebSEASDK, PrivateMetadataAndOpaqueRegionsStayPrivate) {
  capture(blob(12));
  const auto E = extract();
  ASSERT_EQ(field(E, "status"), "ok");
  EXPECT_EQ(field(E, "runtime_activation"), "not_checked");
  const auto ID = field(E, "extraction_id");
  EXPECT_EQ(field(extract(), "extraction_id"), ID);
  auto Page = page(ID);
  for (const auto &R : *Page.getAsObject()->getArray("items")) {
    const auto Kind = field(R, "kind");
    if (Kind == "code_path" || Kind == "asset_key") {
      EXPECT_TRUE(R.getAsObject()->get("blob_sha256")->getAsNull().has_value());
      EXPECT_TRUE(
          R.getAsObject()->get("selection_id")->getAsNull().has_value());
    }
    if (Kind == "v8_code_cache") {
      const auto Region = field(R, "region_id");
      EXPECT_EQ(code(take(neverd_web_source_analyze_json(
                    S, Revision.data(), Revision.size(), Region.data(),
                    Region.size(), "commonjs", 8))),
#if NEVERD_TEST_WEB_JAVASCRIPT
                "unknown_artifact"
#else
                "capability_unavailable"
#endif
      );
    }
  }
  const auto Source = field(E, "source_artifact_id");
  const auto Parsed = take(neverd_web_source_analyze_json(
      S, Revision.data(), Revision.size(), Source.data(), Source.size(),
      "commonjs", 8));
#if NEVERD_TEST_WEB_JAVASCRIPT
  ASSERT_EQ(field(Parsed, "parse_status"), "parsed");
  const auto SID = field(Parsed, "source_id");
  const auto Anchor = take(
      neverd_web_source_anchor_json(S, Revision.data(), Revision.size(),
                                    SID.data(), SID.size(), 0, 6, nullptr, 0));
  const auto *Storage = Anchor.getAsObject()->getObject("storage");
  ASSERT_NE(Storage, nullptr);
  EXPECT_EQ(Storage->getString("kind"), "sea_region");
  EXPECT_EQ(Storage->getString("storage_artifact_id"), Artifact);
  EXPECT_EQ(Storage->getString("mapping"), "byte_identity");
#else
  EXPECT_EQ(code(Parsed), "capability_unavailable");
#endif
  capture(blob(2));
  const auto Snapshot = extract();
  EXPECT_TRUE(Snapshot.getAsObject()
                  ->get("source_artifact_id")
                  ->getAsNull()
                  .has_value());
  EXPECT_EQ(field(Snapshot, "source_status"), "snapshot_opaque");
}
TEST_F(WebSEASDK, FailureIsAtomicAndRevisionRevokesDerivedSelections) {
  capture(blob() + "trailing");
  EXPECT_EQ(code(extract()), "sea_trailing_bytes");
  EXPECT_EQ(field(take(neverd_web_metadata_json(S)), "analysis_status"),
            "not_analyzed");
  EXPECT_EQ(code(take(neverd_web_sea_extract_json(
                S, Revision.data(), Revision.size(), nullptr, 64,
                Profile.data(), Profile.size()))),
            "invalid_buffer");
  capture(blob());
  const auto E = extract();
  const auto ID = field(E, "extraction_id");
  EXPECT_EQ(code(page(ID, 0, 0)), "invalid_page");
  EXPECT_EQ(code(page(ID, 0, 129)), "invalid_page");
  EXPECT_EQ(code(page(ID, UINT64_MAX)), "invalid_page");
  EXPECT_EQ(code(extract("node-sea-latest")), "sea_unsupported_profile");
  const auto Old = Revision;
  capture(blob());
  EXPECT_EQ(code(take(neverd_web_sea_records_json(S, Old.data(), Old.size(),
                                                  ID.data(), ID.size(), 0, 1))),
            "stale_revision");
  EXPECT_EQ(code(page(ID)), "unknown_sea_extraction");
}
TEST_F(WebSEASDK, CacheBudgetCountsExtractionsAndResetsOnImport) {
  for (unsigned I = 0; I < 5; ++I)
    std::ofstream(Root / std::to_string(I), std::ios::binary)
        << blob(0, std::to_string(I));
  open(Root.string());
  const auto A = take(
      neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 1, 5));
  const auto &Items = *A.getAsObject()->getArray("items");
  ASSERT_EQ(Items.size(), 5);
  for (unsigned I = 0; I < 4; ++I)
    EXPECT_EQ(field(extract(Profile, field(Items[I], "artifact_id")), "status"),
              "ok");
  EXPECT_EQ(code(extract(Profile, field(Items[4], "artifact_id"))),
            "sea_cache_budget_exceeded");
  open((Root / "4").string());
  EXPECT_EQ(field(extract(), "status"), "ok");
}
TEST_F(WebSEASDK, AssetsUseExistingMapPackageAndNativeConsumers) {
  std::string B;
  append(B, 0x143da20, 4);
  append(B, 8, 4);
  string(B, "CANARY");
  string(B, "");
  append(B, 3);
  string(B, "CANARY_MAP");
  string(B,
         R"({"version":3,"sources":["CANARY"],"names":[],"mappings":"AAAA"})");
  string(B, "CANARY_PACKAGE");
  string(
      B,
      R"({"name":"CANARY","version":"1.0.0","scripts":{"install":"CANARY"}})");
  string(B, "CANARY_NATIVE");
  string(B, neverd::web::test::nativeELF());
  capture(B);
  const auto E = extract();
  const auto Page = page(field(E, "extraction_id"));
  std::vector<std::string> Assets;
  for (const auto &R : *Page.getAsObject()->getArray("items"))
    if (field(R, "kind") == "asset")
      Assets.push_back(field(R, "selection_id"));
  ASSERT_EQ(Assets.size(), 3);
  EXPECT_EQ(field(take(neverd_web_source_map_analyze_json(
                      S, Revision.data(), Revision.size(), Assets[0].data(),
                      Assets[0].size())),
                  "status"),
            "ok");
  const auto Package = take(neverd_web_packages_analyze_json(
      S, Revision.data(), Revision.size(), Assets[1].data(), Assets[1].size(),
      "package-json", 12));
  EXPECT_EQ(field(Package, "status"), "ok");
  EXPECT_EQ(Package.getAsObject()->getBoolean("directory_inventory"), false);
  neverd_session_t Native = nullptr;
  const auto N = take(
      neverd_web_native_open_json(S, Revision.data(), Revision.size(),
                                  Assets[2].data(), Assets[2].size(), &Native));
  EXPECT_EQ(field(N, "status"), "ok");
  ASSERT_NE(Native, nullptr);
  EXPECT_EQ(N.getAsObject()->getObject("origin")->getString("kind"),
            "sea_region");
  neverd_session_destroy(Native);
}
#if NEVERD_TEST_WEB_JAVASCRIPT
TEST_F(WebSEASDK, NestedCompressedContainerKeepsOriginalCoordinateSpace) {
  using namespace neverd::web::test;
  for (const auto *Format : {"tar", "tgz"}) {
    const auto Caps = take(neverd_web_capabilities_json());
    bool Available = false;
    for (const auto &A : *Caps.getAsObject()->getArray("analysis"))
      if (field(A, "kind") == "package_archive")
        Available =
            A.getAsObject()->getBoolean("available").value_or(false) &&
            (std::string_view(Format) == "tar" ||
             A.getAsObject()->getBoolean("gzip_available").value_or(false));
    if (!Available)
      continue;
    const auto Tar = finishTar(tarMember("package/sea", blob()));
    capture(std::string_view(Format) == "tgz" ? storedGzip(Tar) : Tar);
    const auto Archive = take(neverd_web_package_archive_extract_json(
        S, Revision.data(), Revision.size(), Artifact.data(), Artifact.size(),
        Format, 3));
    const auto AID = field(Archive, "archive_id");
    const auto Members = take(neverd_web_package_archive_records_json(
        S, Revision.data(), Revision.size(), AID.data(), AID.size(), 0, 1));
    const auto MID =
        field((*Members.getAsObject()->getArray("items"))[0], "member_id");
    const auto E = extract(Profile, MID);
    const auto Source = field(E, "source_artifact_id");
    const auto Parsed = take(neverd_web_source_analyze_json(
        S, Revision.data(), Revision.size(), Source.data(), Source.size(),
        "commonjs", 8));
    ASSERT_EQ(field(Parsed, "parse_status"), "parsed");
    const auto SID = field(Parsed, "source_id");
    const auto A = take(neverd_web_source_anchor_json(
        S, Revision.data(), Revision.size(), SID.data(), SID.size(), 0, 6,
        nullptr, 0));
    const auto *Storage = A.getAsObject()->getObject("storage");
    ASSERT_NE(Storage, nullptr);
    EXPECT_EQ(Storage->getString("storage_artifact_id"), Artifact);
    const auto Offset =
        std::stoull(Storage->getString("container_byte_offset")->str()) + 512;
    if (std::string_view(Format) == "tgz") {
      EXPECT_EQ(Storage->getString("mapping"), "containing_compressed_frame");
      EXPECT_EQ(Storage->getString("expanded_byte_offset"),
                std::to_string(Offset));
      EXPECT_TRUE(Storage->get("byte_offset")->getAsNull().has_value());
    } else
      EXPECT_EQ(Storage->getString("byte_offset"), std::to_string(Offset));
  }
}
#endif
#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebSEASDK, CLIRequiresNoRuntimeOrExternalExecutable) {
  capture(blob());
  const auto Out = (Root / "out").string(), Err = (Root / "err").string();
  const llvm::StringRef Env[]{"PATH=/neverd-no-external-tools", "LC_ALL=C"};
  const std::optional<llvm::StringRef> Redirects[]{std::nullopt, Out, Err};
  for (const auto *Command : {"sea", "sea-source"}) {
    const llvm::StringRef Args[]{NEVERD_WEB_TEST_CLI, "web", Command, Path,
                                 Profile};
    const bool NoJS = !NEVERD_TEST_WEB_JAVASCRIPT &&
                      std::string_view(Command) == "sea-source";
    ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Env,
                                        Redirects, 30),
              NoJS ? 1 : 0);
    std::ifstream F(Out), E(Err);
    const std::string Text{std::istreambuf_iterator<char>(F), {}};
    const std::string Errors{std::istreambuf_iterator<char>(E), {}};
    EXPECT_EQ(Text.find("CANARY"), std::string::npos);
    EXPECT_EQ(Errors.find("CANARY"), std::string::npos);
    EXPECT_NE(Text.find("extraction_id"), std::string::npos);
  }
}
#endif
} // namespace
