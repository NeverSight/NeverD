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
#include "SEAFixture.h"
#include "ZipFixture.h"
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
  static std::string zipFixture(bool Deflate) {
    const uint16_t Method = Deflate ? 8 : 0;
    return ZipFixture(
               {{"package/package.json",
                 R"({"name":"CANARY_NAME","main":"a.js","scripts":{"install":"CANARY_DO_NOT_RUN"}})",
                 Method},
                {"package/a.js", "export const CANARY_VALUE = 7;", Method},
                {"package/bun", BunFixture(true, {}, nativeELF()).Bytes,
                 Method},
                {"package/native.node", nativeELF(), Method},
                {"package/unavailable", "CANARY_OPAQUE", 0, 1}})
        .Bytes;
  }
};

TEST_F(WebPackageArchiveSDK,
       ConsumersShareMembersAndKeepCompressedCoordinates) {
  for (const std::string_view Case :
       {"tar", "tgz", "zip-stored", "zip-deflate"}) {
    const bool Zip = Case.starts_with("zip");
    const bool Compressed = Case == "tgz" || Case == "zip-deflate";
    const auto Format = Zip ? std::string_view("zip") : Case;
    if (Compressed && !Gzip)
      continue;
    capture(Zip          ? zipFixture(Compressed)
            : Compressed ? storedGzip(fixture())
                         : fixture());
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
              Compressed ? "containing_compressed_frame" : "byte_identity");
    if (Compressed) {
      EXPECT_TRUE(Storage->get("byte_offset")->getAsNull().has_value());
      EXPECT_EQ(Storage->getString("expanded_byte_offset"),
                Members[1].getAsObject()->getString("expanded_byte_offset"));
    } else if (Zip) {
      EXPECT_EQ(Storage->getString("byte_offset"),
                Members[1].getAsObject()->getString("stored_byte_offset"));
    }
    if (Zip) {
      EXPECT_EQ(Storage->getString("container_frame_offset"),
                Members[1].getAsObject()->getString("local_header_offset"));
      EXPECT_EQ(Storage->getString("container_frame_length"),
                Members[1].getAsObject()->getString("stored_frame_length"));
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

TEST_F(WebPackageArchiveSDK, ZipCoordinatesComposeThroughSEAHTMLAndBun) {
#if !NEVERD_TEST_WEB_JAVASCRIPT
  GTEST_SKIP() << "Requires JS parser";
#else
  for (const uint16_t Method : {0, 8}) {
    if (Method == 8 && !Gzip)
      continue;
    const ZipFixture F(
        {{"a.js", "let CANARY_A=1;", Method},
         {"b.js", "let CANARY_B=2;", Method},
         {"cli", neverd::web::sea_test::blob(), Method},
         {"index.html", "<script>let CANARY_HTML=3;</script>", Method},
         {"bun", BunFixture().Bytes, Method}});
    capture(F.Bytes);
    const auto A = extract("zip");
    ASSERT_EQ(field(A, "status"), "ok");
    const auto ID = field(A, "archive_id");
    const auto P = page(ID);
    const auto &Members = *P.getAsObject()->getArray("items");
    const auto Member = [&](unsigned I) {
      return field(Members[I], "member_id");
    };
    const auto Anchor = [&](const std::string &Artifact, const char *Kind) {
      const auto Parsed = take(neverd_web_source_analyze_json(
          S, Revision.data(), Revision.size(), Artifact.data(), Artifact.size(),
          Kind, std::char_traits<char>::length(Kind)));
      EXPECT_EQ(field(Parsed, "parse_status"), "parsed");
      const auto SID = field(Parsed, "source_id");
      return take(neverd_web_source_anchor_json(S, Revision.data(),
                                                Revision.size(), SID.data(),
                                                SID.size(), 0, 1, nullptr, 0));
    };
    const auto CheckOrigin = [&](const llvm::json::Object *Origin, unsigned I,
                                 bool SelectedRange = false) {
      ASSERT_NE(Origin, nullptr);
      EXPECT_EQ(Origin->getString("archive_id"), ID);
      EXPECT_EQ(Origin->getString("member_id"), Member(I));
      EXPECT_EQ(Origin->getString("container_frame_offset"),
                std::to_string(F.Local[I]));
      EXPECT_EQ(Origin->getString("container_frame_length"),
                field(Members[I], "stored_frame_length"));
      EXPECT_EQ(Origin->getString("byte_offset_basis"),
                Method == 0 ? "storage_artifact" : "expanded_stream");
      if (Method == 0) {
        EXPECT_EQ(Origin->getString("byte_offset"), std::to_string(F.Data[I]));
        EXPECT_EQ(Origin->getString("byte_length"),
                  SelectedRange ? "1" : field(Members[I], "size_bytes"));
      } else {
        EXPECT_TRUE(Origin->get("byte_offset")->getAsNull().has_value());
      }
    };
    for (unsigned I : {0, 1}) {
      const auto R = Anchor(Member(I), "script");
      const auto *Storage = R.getAsObject()->getObject("storage");
      CheckOrigin(Storage, I, true);
      ASSERT_NE(Storage, nullptr);
      EXPECT_EQ(Storage->getString("mapping"),
                Method == 0 ? "byte_identity" : "containing_compressed_frame");
      if (Method == 0)
        EXPECT_EQ(Storage->getString("byte_offset"), std::to_string(F.Data[I]));
      else {
        EXPECT_TRUE(Storage->get("byte_offset")->getAsNull().has_value());
        EXPECT_EQ(Storage->getString("expanded_byte_offset"),
                  field(Members[I], "expanded_byte_offset"));
      }
    }
    const std::string SEAProfile = "node-sea-22.15.0-blob-le64-v1";
    const auto SEAID = Member(2);
    const auto SEA = take(neverd_web_sea_extract_json(
        S, Revision.data(), Revision.size(), SEAID.data(), SEAID.size(),
        SEAProfile.data(), SEAProfile.size()));
    ASSERT_EQ(field(SEA, "status"), "ok");
    const auto SA = Anchor(field(SEA, "source_artifact_id"), "commonjs");
    const auto *SS = SA.getAsObject()->getObject("storage");
    ASSERT_NE(SS, nullptr);
    CheckOrigin(SS->getObject("container_origin"), 2);
    EXPECT_EQ(SS->getString("mapping"),
              Method == 0 ? "byte_identity" : "containing_compressed_frame");

    const auto HTMLID = Member(3);
    const auto HTML = take(neverd_web_html_analyze_json(
        S, Revision.data(), Revision.size(), HTMLID.data(), HTMLID.size()));
    const auto HID = field(HTML, "html_id");
    const auto HP = take(neverd_web_html_records_json(
        S, Revision.data(), Revision.size(), HID.data(), HID.size(), "scripts",
        7, 0, 1));
    const auto Inline =
        field((*HP.getAsObject()->getArray("items"))[0], "inline_artifact_id");
    const auto HA = Anchor(Inline, "script");
    const auto *HS = HA.getAsObject()->getObject("storage");
    ASSERT_NE(HS, nullptr);
    CheckOrigin(HS->getObject("parent_origin"), 3);
    if (Method == 0)
      EXPECT_EQ(HS->getString("byte_offset"), std::to_string(F.Data[3] + 8));
    else
      EXPECT_EQ(
          HS->getString("expanded_byte_offset"),
          std::to_string(
              std::stoull(field(Members[3], "expanded_byte_offset")) + 8));

    const auto BID = Member(4);
    const auto Bun = take(neverd_web_bun_extract_json(
        S, Revision.data(), Revision.size(), BID.data(), BID.size()));
    const auto EID = field(Bun, "extraction_id");
    const auto BP = take(neverd_web_bun_records_json(
        S, Revision.data(), Revision.size(), EID.data(), EID.size(), "modules",
        7, 0, 1));
    const auto BSID =
        field((*BP.getAsObject()->getArray("items"))[0], "source_artifact_id");
    const auto BA = Anchor(BSID, "module");
    const auto *BS = BA.getAsObject()->getObject("storage");
    ASSERT_NE(BS, nullptr);
    CheckOrigin(BS->getObject("container_origin"), 4);
    if (Method == 0) {
      const auto *Origin = BS->getObject("container_origin");
      const auto Base = std::stoull(Origin->getString("byte_offset")->str());
      const auto At = std::stoull(BS->getString("byte_offset")->str());
      const auto Size = std::stoull(BS->getString("byte_length")->str());
      EXPECT_EQ(F.Bytes.substr(Base + At, Size),
                BunFixture().Bytes.substr(At, Size));
    }
  }
#endif
}

TEST_F(WebPackageArchiveSDK, ZipFailureIsAtomicAndImportRevokesDerivedMembers) {
  const ZipFixture F({{"a.js", "let CANARY=1;"}});
  auto Bad = F.Bytes;
  Bad[F.Data[0]] ^= 1;
  capture(Bad);
  EXPECT_EQ(code(extract("zip")), "zip_crc_mismatch");
  EXPECT_EQ(field(take(neverd_web_metadata_json(S)), "analysis_status"),
            "not_analyzed");
  capture(F.Bytes);
  const auto A = extract("zip");
  const auto ID = field(A, "archive_id"), Old = Revision;
  const auto P = page(ID);
  const auto Member =
      field((*P.getAsObject()->getArray("items"))[0], "member_id");
  capture(F.Bytes);
  EXPECT_EQ(code(take(neverd_web_package_archive_records_json(
                S, Old.data(), Old.size(), ID.data(), ID.size(), 0, 1))),
            "stale_revision");
  EXPECT_EQ(code(page(ID)), "unknown_package_archive");
#if NEVERD_TEST_WEB_JAVASCRIPT
  EXPECT_EQ(code(take(neverd_web_source_analyze_json(
                S, Revision.data(), Revision.size(), Member.data(),
                Member.size(), "script", 6))),
            "unknown_artifact");
#endif
}

TEST_F(WebPackageArchiveSDK, ZipLatePayloadFailurePreservesExistingArchive) {
  const ZipFixture Good({{"good.js", "let CANARY=1;"}});
  const ZipFixture Bad(
      {{"first.js", "let CANARY=2;"}, {"second.js", "let CANARY=3;"}});
  auto Corrupt = Bad.Bytes;
  Corrupt[Bad.Data[1]] ^= 1;
  std::ofstream(Root / "0-good.zip", std::ios::binary)
      .write(Good.Bytes.data(), Good.Bytes.size());
  std::ofstream(Root / "1-bad.zip", std::ios::binary)
      .write(Corrupt.data(), Corrupt.size());
  Path = Root.string();
  open();
  const auto Inputs = take(
      neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 1, 2));
  const auto &Items = *Inputs.getAsObject()->getArray("items");
  ASSERT_EQ(Items.size(), 2);
  Artifact = field(Items[0], "artifact_id");
  const auto A = extract("zip");
  ASSERT_EQ(field(A, "status"), "ok");
  const auto ID = field(A, "archive_id");
  const auto Before = page(ID);
  Artifact = field(Items[1], "artifact_id");
  for (unsigned Attempt = 0; Attempt < 5; ++Attempt)
    EXPECT_EQ(code(extract("zip")), "zip_crc_mismatch");
  EXPECT_EQ(page(ID), Before);
  // Failed attempts never consume cache slots or revoke a retained member.
  Artifact = field(Items[0], "artifact_id");
  EXPECT_EQ(field(extract("zip"), "archive_id"), ID);
#if NEVERD_TEST_WEB_JAVASCRIPT
  const auto Member =
      field((*Before.getAsObject()->getArray("items"))[0], "member_id");
  EXPECT_EQ(field(take(neverd_web_source_analyze_json(
                      S, Revision.data(), Revision.size(), Member.data(),
                      Member.size(), "script", 6)),
                  "parse_status"),
            "parsed");
#endif
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
  const auto Out = (Root / "out").string(), Err = (Root / "err").string();
  const llvm::StringRef Env[] = {"PATH=/neverd-no-external-tools", "LC_ALL=C"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Out, Err};
  for (const llvm::StringRef Format : {"tar", "zip"}) {
    capture(Format == "tar" ? fixture() : zipFixture(false));
    for (const llvm::StringRef Command :
         {"archive", "archive-packages", "archive-bun"}) {
      llvm::SmallVector<llvm::StringRef> Args{
          NEVERD_WEB_TEST_CLI, "web", Command, Path, Format, "0"};
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
}
#endif
} // namespace
