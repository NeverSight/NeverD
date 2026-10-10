//===- PackageArchiveSDKTests.cpp - Archive API tests ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Archive consumers, compressed origins, revocation and offline CLI checks.
///
//===----------------------------------------------------------------------===//

#include "BunFixture.h"
#include "NativeFixture.h"
#include "PackageArchiveFixture.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <filesystem>
#include <fstream>

namespace {
using namespace neverd::web::test;
using Value = llvm::json::Value;
Value take(const char *Owned) {
  if (!Owned)
    throw std::runtime_error("missing response");
  const std::string Text(Owned);
  neverd_free_string(Owned);
  EXPECT_EQ(Text.find("CANARY"), std::string::npos);
  auto V = llvm::json::parse(Text);
  if (!V) {
    llvm::consumeError(V.takeError());
    throw std::runtime_error("invalid response");
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
class WebPackageArchiveSDK : public ::testing::Test {
protected:
  neverd_web_session_t S = nullptr;
  std::filesystem::path Root;
  std::string Path, Revision, Artifact;
  bool Gzip = false;
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX capture required";
#endif
    const auto Caps = take(neverd_web_capabilities_json());
    bool Available = false;
    for (const auto &A : *Caps.getAsObject()->getArray("analysis"))
      if (field(A, "kind") == "package_archive") {
        Available = A.getAsObject()->getBoolean("available").value_or(false);
        Gzip = A.getAsObject()->getBoolean("gzip_available").value_or(false);
      }
    if (!Available)
      GTEST_SKIP() << "Pinned archive path policy unavailable";
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-archive-sdk", Directory));
    Root = Directory.str().str();
    Path = (Root / "CANARY_INPUT.tgz").string();
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
      ASSERT_TRUE(F.good());
    }
    open();
  }
  void open() {
    const auto P = take(neverd_web_import_preview_json(
        S, Path.data(), Path.size(), nullptr, 0));
    const auto Token = field(P, "preview_token");
    Revision = field(
        take(neverd_web_import_commit_json(S, Token.data(), Token.size())),
        "revision");
    Artifact = field((*take(neverd_web_artifacts_json(S, Revision.data(),
                                                      Revision.size(), 0, 1))
                           .getAsObject()
                           ->getArray("items"))[0],
                     "artifact_id");
  }
  Value extract(std::string_view Format) {
    return take(neverd_web_package_archive_extract_json(
        S, Revision.data(), Revision.size(), Artifact.data(), Artifact.size(),
        Format.data(), Format.size()));
  }
  Value page(const std::string &ID, uint64_t Offset = 0, uint64_t Limit = 512) {
    return take(neverd_web_package_archive_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Offset,
        Limit));
  }
  static std::string fixture() {
    return finishTar(
        tarMember(
            "package/package.json",
            R"({"name":"CANARY_NAME","main":"a.js","scripts":{"install":"CANARY_DO_NOT_RUN"}})") +
        tarMember("package/a.js", "export const CANARY_VALUE = 7;") +
        tarMember("package/bun", BunFixture(true, {}, nativeELF()).Bytes) +
        tarMember("package/native.node", nativeELF()) +
        tarMember("package/link", "", '2', "../../CANARY_OUTSIDE"));
  }
};

TEST_F(WebPackageArchiveSDK,
       ConsumersShareMembersAndKeepCompressedCoordinates) {
  for (const auto *Format : {"tar", "tgz"}) {
    if (std::string_view(Format) == "tgz" && !Gzip)
      continue;
    capture(std::string_view(Format) == "tar" ? fixture()
                                              : storedGzip(fixture()));
    const auto A = extract(Format);
    ASSERT_EQ(field(A, "status"), "ok");
    const auto ID = field(A, "archive_id");
    EXPECT_EQ(field(extract(Format), "archive_id"), ID);
    const auto Page = page(ID);
    const auto &Members = *Page.getAsObject()->getArray("items");
    ASSERT_EQ(Members.size(), 5);
    const auto Manifest = field(Members[0], "member_id");
    const auto Graph = take(neverd_web_packages_analyze_json(
        S, Revision.data(), Revision.size(), Manifest.data(), Manifest.size(),
        "package-json", 12));
    ASSERT_EQ(field(Graph, "status"), "ok");
    EXPECT_EQ(Graph.getAsObject()->getInteger("entry_count"), 1);
#if NEVERD_TEST_WEB_JAVASCRIPT
    const auto JS = field(Members[1], "member_id");
    const auto Source =
        take(neverd_web_source_analyze_json(S, Revision.data(), Revision.size(),
                                            JS.data(), JS.size(), "module", 6));
    ASSERT_EQ(field(Source, "parse_status"), "parsed");
    const auto SID = field(Source, "source_id");
    const auto Anchor = take(neverd_web_source_anchor_json(
        S, Revision.data(), Revision.size(), SID.data(), SID.size(), 0, 6,
        nullptr, 0));
    const auto *Storage = Anchor.getAsObject()->getObject("storage");
    ASSERT_NE(Storage, nullptr);
    EXPECT_EQ(Storage->getString("kind"), "package_archive_member");
    EXPECT_EQ(Storage->getString("mapping"),
              std::string_view(Format) == "tar"
                  ? "byte_identity"
                  : "containing_compressed_frame");
    if (std::string_view(Format) == "tgz") {
      EXPECT_TRUE(Storage->get("byte_offset")->getAsNull().has_value());
      EXPECT_EQ(Storage->getString("expanded_byte_offset"),
                Members[1].getAsObject()->getString("expanded_byte_offset"));
    }
#endif
    const auto BID = field(Members[2], "member_id");
    const auto Bun = take(neverd_web_bun_extract_json(
        S, Revision.data(), Revision.size(), BID.data(), BID.size()));
    ASSERT_EQ(field(Bun, "status"), "ok");
    EXPECT_EQ(Bun.getAsObject()->getInteger("module_count"), 3);
    const auto EID = field(Bun, "extraction_id");
    const auto Modules = take(neverd_web_bun_records_json(
        S, Revision.data(), Revision.size(), EID.data(), EID.size(), "modules",
        7, 0, 3));
    const auto Asset = field((*Modules.getAsObject()->getArray("items"))[2],
                             "content_region_id");
    neverd_session_t Native = nullptr;
    const auto N =
        take(neverd_web_native_open_json(S, Revision.data(), Revision.size(),
                                         Asset.data(), Asset.size(), &Native));
    ASSERT_EQ(field(N, "status"), "ok");
    EXPECT_EQ(N.getAsObject()
                  ->getObject("origin")
                  ->getObject("container_origin")
                  ->getString("archive_id"),
              ID);
    neverd_session_destroy(Native);
    const auto Link = field(Members[4], "member_id");
    EXPECT_EQ(code(take(neverd_web_source_analyze_json(
                  S, Revision.data(), Revision.size(), Link.data(), Link.size(),
                  "module", 6))),
#if NEVERD_TEST_WEB_JAVASCRIPT
              "artifact_bytes_unavailable"
#else
              "capability_unavailable"
#endif
    );
  }
}

TEST_F(WebPackageArchiveSDK, FailureDoesNotPublishAndRevisionRevokesMembers) {
  auto Bad = fixture();
  Bad.back() = 'x';
  capture(Bad);
  EXPECT_EQ(code(extract("tar")), "package_archive_trailing_tar_data");
  EXPECT_EQ(field(take(neverd_web_metadata_json(S)), "analysis_status"),
            "not_analyzed");
  EXPECT_EQ(code(take(neverd_web_package_archive_extract_json(
                S, Revision.data(), Revision.size(), nullptr, 64, "tar", 3))),
            "invalid_buffer");
  capture(fixture());
  const auto A = extract("tar");
  const auto ID = field(A, "archive_id");
  const auto P = page(ID);
  const auto Member =
      field((*P.getAsObject()->getArray("items"))[1], "member_id");
  EXPECT_EQ(code(take(neverd_web_package_integrity_verify_json(
                S, Revision.data(), Revision.size(), Member.data(),
                Member.size(), Artifact.data(), Artifact.size(), nullptr, 0))),
            "integrity_original_not_captured");
  EXPECT_EQ(code(page(ID, 0, 0)), "invalid_page");
  EXPECT_EQ(code(page(ID, 0, 513)), "invalid_page");
  const auto OldR = Revision;
  capture(fixture());
  EXPECT_EQ(code(take(neverd_web_package_archive_records_json(
                S, OldR.data(), OldR.size(), ID.data(), ID.size(), 0, 1))),
            "stale_revision");
  EXPECT_EQ(code(page(ID)), "unknown_package_archive");
  EXPECT_EQ(
      code(take(neverd_web_bun_extract_json(S, Revision.data(), Revision.size(),
                                            Member.data(), Member.size()))),
      "unknown_artifact");
}

#if NEVERD_TEST_WEB_JAVASCRIPT
TEST_F(WebPackageArchiveSDK,
       NestedBunAssetAnchorNamesItsStorageCoordinateSpace) {
  for (const auto *Format : {"tar", "tgz"}) {
    if (std::string_view(Format) == "tgz" && !Gzip)
      continue;
    const auto Tar = finishTar(
        tarMember("package/bun",
                  BunFixture(true, {}, "export const CANARY_NESTED=7;").Bytes));
    capture(std::string_view(Format) == "tar" ? Tar : storedGzip(Tar));
    const auto A = extract(Format);
    const auto P = page(field(A, "archive_id"));
    const auto MID =
        field((*P.getAsObject()->getArray("items"))[0], "member_id");
    const auto B = take(neverd_web_bun_extract_json(
        S, Revision.data(), Revision.size(), MID.data(), MID.size()));
    const auto EID = field(B, "extraction_id");
    const auto Modules = take(neverd_web_bun_records_json(
        S, Revision.data(), Revision.size(), EID.data(), EID.size(), "modules",
        7, 0, 3));
    const auto Asset = field((*Modules.getAsObject()->getArray("items"))[2],
                             "content_region_id");
    const auto Source = take(neverd_web_source_analyze_json(
        S, Revision.data(), Revision.size(), Asset.data(), Asset.size(),
        "module", 6));
    ASSERT_EQ(field(Source, "parse_status"), "parsed");
    const auto SID = field(Source, "source_id");
    const auto Anchor = take(neverd_web_source_anchor_json(
        S, Revision.data(), Revision.size(), SID.data(), SID.size(), 0, 6,
        nullptr, 0));
    const auto *Storage = Anchor.getAsObject()->getObject("storage");
    ASSERT_NE(Storage, nullptr);
    EXPECT_EQ(Storage->getString("storage_artifact_id"), Artifact);
    EXPECT_EQ(Storage->getString("container_artifact_id"), MID);
    const auto Relative =
        std::stoull(Storage->getString("container_byte_offset")->str());
    if (std::string_view(Format) == "tar") {
      EXPECT_EQ(Storage->getString("byte_offset_basis"), "storage_artifact");
      EXPECT_EQ(Storage->getString("byte_offset"),
                std::to_string(512 + Relative));
    } else {
      EXPECT_EQ(Storage->getString("byte_offset_basis"), "expanded_stream");
      EXPECT_TRUE(Storage->get("byte_offset")->getAsNull().has_value());
      EXPECT_EQ(Storage->getString("expanded_byte_offset"),
                std::to_string(512 + Relative));
    }
  }
}
#endif

TEST_F(WebPackageArchiveSDK, NativeOwnerSurvivesInputAndWebSessionDestruction) {
  if (!Gzip)
    GTEST_SKIP() << "Native zlib unavailable";
  capture(storedGzip(fixture()));
  const auto A = extract("tgz");
  const auto P = page(field(A, "archive_id"));
  const auto Member =
      field((*P.getAsObject()->getArray("items"))[3], "member_id");
  neverd_session_t Native = nullptr;
  const auto N =
      take(neverd_web_native_open_json(S, Revision.data(), Revision.size(),
                                       Member.data(), Member.size(), &Native));
  ASSERT_EQ(field(N, "status"), "ok");
  std::filesystem::remove(Path);
  capture("new snapshot");
  neverd_web_session_destroy(S);
  S = nullptr;
  const auto Retained = take(neverd_web_native_metadata_json(Native));
  EXPECT_EQ(field(Retained, "status"), "ok");
  EXPECT_EQ(
      Retained.getAsObject()->getObject("origin")->getString("archive_id"),
      field(A, "archive_id"));
  const auto Analyzed = take(neverd_web_native_analyze_json(Native));
  EXPECT_EQ(field(Analyzed, "pipeline_status"), "succeeded");
  neverd_session_destroy(Native);
}

TEST_F(WebPackageArchiveSDK, FourArchiveCacheAllowsHitsAndReleasesOnImport) {
  const auto Tar = fixture();
  for (unsigned I = 0; I < 5; ++I)
    std::ofstream(Root / (std::to_string(I) + ".tar"), std::ios::binary)
        .write(Tar.data(), Tar.size());
  Path = Root.string();
  open();
  const auto P = take(
      neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 0, 6));
  const auto &Items = *P.getAsObject()->getArray("items");
  ASSERT_EQ(Items.size(), 6);
  for (unsigned I = 1; I <= 4; ++I) {
    Artifact = field(Items[I], "artifact_id");
    EXPECT_EQ(field(extract("tar"), "status"), "ok");
  }
  Artifact = field(Items[5], "artifact_id");
  EXPECT_EQ(code(extract("tar")), "package_archive_cache_budget_exceeded");
  Artifact = field(Items[1], "artifact_id");
  EXPECT_EQ(field(extract("tar"), "status"), "ok");
  open();
  Artifact = field(Items[5], "artifact_id");
  EXPECT_EQ(field(extract("tar"), "status"), "ok");
}

#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebPackageArchiveSDK, CLIUsesCapturedMembersWithAnUnusablePath) {
  capture(fixture());
  const auto Out = (Root / "out").string(), Err = (Root / "err").string();
  const llvm::StringRef Env[] = {"PATH=/neverd-no-external-tools", "LC_ALL=C"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Out, Err};
  for (const llvm::StringRef Command :
       {"archive", "archive-packages", "archive-bun"}) {
    llvm::SmallVector<llvm::StringRef> Args{
        NEVERD_WEB_TEST_CLI, "web", Command, Path, "tar", "0"};
    if (Command != "archive")
      Args.push_back(Command == "archive-bun" ? "2" : "0");
    if (Command == "archive-packages")
      Args.push_back("package-json");
    ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Env,
                                        Redirects, 30),
              0);
    std::ifstream F(Out), E(Err);
    const std::string Text{std::istreambuf_iterator<char>(F), {}};
    const std::string Errors{std::istreambuf_iterator<char>(E), {}};
    EXPECT_EQ(Text.find("CANARY"), std::string::npos);
    EXPECT_EQ(Errors.find("CANARY"), std::string::npos);
    EXPECT_NE(Text.find("archive_id"), std::string::npos);
    if (Command == "archive-packages")
      EXPECT_NE(Text.find("package_analysis_id"), std::string::npos);
    if (Command == "archive-bun")
      EXPECT_NE(Text.find("extraction_id"), std::string::npos);
  }
}
#endif
} // namespace
