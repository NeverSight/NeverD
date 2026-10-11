//===- NexeSDKTests.cpp - nexe selection and consumer contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Metadata privacy, namespace isolation, nested storage and offline adapters.
///
//===----------------------------------------------------------------------===//

#include "NativeFixture.h"
#include "NexeFixture.h"
#include "ZipFixture.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
using Value = llvm::json::Value;
using namespace neverd::web::nexe_test;
using namespace neverd::web::test;
constexpr std::string_view Profile = "nexe-4.0.0-rc.7-eof-resources-v1";
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
std::string field(const Value &V, const char *Key) {
  return V.getAsObject()->getString(Key).value_or("").str();
}
std::string code(const Value &V) {
  EXPECT_EQ(field(V, "status"), "error");
  const auto *E = V.getAsObject()->getObject("error");
  return E ? E->getString("code").value_or("").str() : "";
}
class WebNexeSDK : public ::testing::Test {
protected:
  static constexpr std::string_view Script =
      "/* x */const fs=require('node:fs');"
      "fs.writeFileSync('CANARY_PATH','CANARY_DATA');";
  static constexpr std::string_view Manifest =
      R"({"name":"CANARY_PACKAGE","version":"1.0.0","main":"a.js"})";
  neverd_web_session_t S = nullptr;
  std::filesystem::path Root;
  std::string Path, Revision;
  std::vector<std::string> Artifacts;
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX capture profile required";
#endif
    const auto C = take(neverd_web_capabilities_json());
    bool Available = false;
    if (const auto *Ops = C.getAsObject()->getArray("operations"))
      for (const auto &Op : *Ops)
        Available |= Op.getAsString() == "nexe_extract";
    if (!Available)
      GTEST_SKIP() << "nexe namespace policy unavailable";
    llvm::SmallString<128> D;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-nexe-sdk", D));
    Root = D.str().str();
    Path = (Root / "CANARY_INPUT").string();
    S = neverd_web_session_create();
    ASSERT_NE(S, nullptr);
  }
  void TearDown() override {
    neverd_web_session_destroy(S);
    if (!Root.empty()) {
      std::error_code EC;
      std::filesystem::remove_all(Root, EC);
    }
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
    const auto Token = field(P, "preview_token");
    Revision = field(
        take(neverd_web_import_commit_json(S, Token.data(), Token.size())),
        "revision");
    auto Page = take(
        neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 0, 128));
    Artifacts.clear();
    for (const auto &A : *Page.getAsObject()->getArray("items"))
      Artifacts.push_back(field(A, "artifact_id"));
  }
  Value extract(std::string_view A = {}, std::string_view P = Profile) {
    if (A.empty())
      A = Artifacts.at(0);
    return take(neverd_web_nexe_extract_json(S, Revision.data(),
                                             Revision.size(), A.data(),
                                             A.size(), P.data(), P.size()));
  }
  Value page(const std::string &ID, std::string_view Kind = "members",
             uint64_t Offset = 0, uint64_t Limit = 128) {
    return take(neverd_web_nexe_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Kind.data(),
        Kind.size(), Offset, Limit));
  }
  Value source(const std::string &A) {
    return take(neverd_web_source_analyze_json(S, Revision.data(),
                                               Revision.size(), A.data(),
                                               A.size(), "commonjs", 8));
  }
  Value packages(const std::string &A) {
    return take(neverd_web_packages_analyze_json(S, Revision.data(),
                                                 Revision.size(), A.data(),
                                                 A.size(), "package-json", 12));
  }
  Value aggregate(const std::string &P, const std::string &Source) {
    const auto Options =
        "{\"schema_version\":1,\"source_ids\":[\"" + Source + "\"]}";
    return take(neverd_web_package_behavior_analyze_json(
        S, Revision.data(), Revision.size(), P.data(), P.size(), Options.data(),
        Options.size()));
  }
  Value behaviorSources(const Value &V) {
    const auto ID = field(V, "package_behavior_id");
    return take(neverd_web_package_behavior_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), "sources", 7,
        0, 128));
  }
  std::vector<Value> members(const std::string &ID) {
    auto P = page(ID);
    const auto *Items = P.getAsObject()->getArray("items");
    if (!Items)
      throw std::runtime_error("missing members");
    return {Items->begin(), Items->end()};
  }
};
TEST_F(WebNexeSDK, PrivateMetadataAndExplicitSourceSelection) {
  capture(bundle({{"CANARY.js", std::string(Script)},
                  {"asset", std::string("\0\xff", 2)}}));
  const auto E = extract();
  ASSERT_EQ(field(E, "status"), "ok");
  EXPECT_EQ(field(E, "native_prefix_validation"), "not_checked");
  EXPECT_EQ(field(E, "signature_status"), "not_checked");
  const auto ID = field(E, "extraction_id");
  EXPECT_EQ(field(extract(), "extraction_id"), ID);
  EXPECT_EQ(page(ID, "members", 0, 1).getAsObject()->getInteger("next_offset"),
            1);
  auto Rows = members(ID);
  ASSERT_EQ(Rows.size(), 2U);
  const auto Parsed = source(field(Rows[0], "selection_id"));
#if NEVERD_TEST_WEB_JAVASCRIPT
  EXPECT_EQ(field(Parsed, "parse_status"), "parsed");
#else
  EXPECT_EQ(code(Parsed), "capability_unavailable");
#endif
  auto Regions = page(ID, "regions");
  for (const auto &R : *Regions.getAsObject()->getArray("items")) {
    if (field(R, "kind") == "bootstrap_storage") {
      const auto B = source(field(R, "selection_id"));
#if NEVERD_TEST_WEB_JAVASCRIPT
      EXPECT_EQ(field(B, "parse_status"), "parsed");
#else
      EXPECT_EQ(code(B), "capability_unavailable");
#endif
    } else {
      EXPECT_TRUE(
          R.getAsObject()->get("selection_id")->getAsNull().has_value());
    }
  }
}
TEST_F(WebNexeSDK, PagesAtomicFailuresAndRevisionRevocation) {
  capture(bundle({{"a.js", std::string(Script)}}) + "trailing");
  EXPECT_EQ(code(extract()), "nexe_footer_not_found");
  EXPECT_EQ(field(take(neverd_web_metadata_json(S)), "analysis_status"),
            "not_analyzed");
  capture(bundle({{"a.js", std::string(Script)}}));
  const auto ID = field(extract(), "extraction_id");
  for (const auto &Bad :
       {page(ID, "members", 0, 0), page(ID, "members", 0, 513),
        page(ID, "members", UINT64_MAX)})
    EXPECT_EQ(code(Bad), "invalid_page");
  EXPECT_EQ(code(page(ID, "other")), "invalid_record_kind");
  EXPECT_EQ(code(extract({}, "nexe-latest")), "nexe_unsupported_profile");
  EXPECT_EQ(code(take(neverd_web_nexe_extract_json(
                S, Revision.data(), Revision.size(), nullptr, 64,
                Profile.data(), Profile.size()))),
            "invalid_buffer");
  const auto Old = Revision;
  open(Path);
  EXPECT_EQ(code(take(neverd_web_nexe_records_json(S, Old.data(), Old.size(),
                                                   ID.data(), ID.size(),
                                                   "members", 7, 0, 1))),
            "stale_revision");
  EXPECT_EQ(code(page(ID)), "unknown_nexe_extraction");
}
TEST_F(WebNexeSDK, CacheLimitPreservesPreviouslyPublishedSelections) {
  for (unsigned I = 0; I < 5; ++I)
    std::ofstream(Root / ("item" + std::to_string(I)), std::ios::binary)
        << bundle({{"a.js", std::string(Script)}});
  open(Root.string());
  ASSERT_EQ(Artifacts.size(), 6U);
  std::vector<std::string> IDs;
  for (unsigned I = 1; I < 5; ++I) {
    const auto E = extract(Artifacts[I]);
    ASSERT_EQ(field(E, "status"), "ok");
    IDs.push_back(field(E, "extraction_id"));
  }
  EXPECT_EQ(code(extract(Artifacts[5])), "nexe_cache_budget_exceeded");
  for (const auto &ID : IDs)
    EXPECT_EQ(field(page(ID), "status"), "ok");
  open(Root.string());
  EXPECT_EQ(field(extract(Artifacts[5]), "status"), "ok");
}
TEST_F(WebNexeSDK, AggregatePrivateBudgetIsAtomicBeforeTheCountLimit) {
  std::string Parent;
  for (unsigned I = 0; I < 60; ++I)
    Parent += std::string(63, 'a') + '/';
  std::vector<std::pair<std::string, std::string>> Files;
  for (unsigned I = 0; I < 20; ++I)
    Files.emplace_back(Parent + std::to_string(I), "");
  const auto B = bundle(Files);
  for (unsigned I = 0; I < 4; ++I)
    std::ofstream(Root / ("item" + std::to_string(I)), std::ios::binary) << B;
  open(Root.string());
  ASSERT_EQ(Artifacts.size(), 5U);
  std::vector<std::string> IDs;
  for (unsigned I = 1; I < 4; ++I) {
    const auto E = extract(Artifacts[I]);
    ASSERT_EQ(field(E, "status"), "ok");
    IDs.push_back(field(E, "extraction_id"));
  }
  EXPECT_EQ(code(extract(Artifacts[4])), "nexe_cache_metadata_budget_exceeded");
  for (const auto &ID : IDs)
    EXPECT_EQ(members(ID).size(), 20U);
  const auto Metadata = take(neverd_web_metadata_json(S));
  EXPECT_EQ(Metadata.getAsObject()->getInteger("nexe_extraction_count"), 3);
}
TEST_F(WebNexeSDK, StoredAndDeflateZIPComposeNonzeroSourceCoordinates) {
#if !NEVERD_TEST_WEB_JAVASCRIPT
  GTEST_SKIP() << "Source anchors require the parser";
#endif
  const auto B = bundle({{"a.js", std::string(Script)},
                         {"helper", nativeELF()},
                         {"package.json", std::string(Manifest)}});
  for (const uint16_t Method : {0, 8}) {
    const ZipFixture Z({{"pad", "12345", Method}, {"app", B, Method}});
    capture(Z.Bytes);
    const auto A = Artifacts[0];
    const auto Archive = take(neverd_web_package_archive_extract_json(
        S, Revision.data(), Revision.size(), A.data(), A.size(), "zip", 3));
    ASSERT_EQ(field(Archive, "status"), "ok");
    const auto ArchiveID = field(Archive, "archive_id");
    auto AP = take(neverd_web_package_archive_records_json(
        S, Revision.data(), Revision.size(), ArchiveID.data(), ArchiveID.size(),
        1, 1));
    const auto &ArchiveRow = (*AP.getAsObject()->getArray("items"))[0];
    if (field(ArchiveRow, "unavailable_reason") == "deflate_unavailable") {
      EXPECT_EQ(Method, 8);
      continue;
    }
    const auto Container = field(ArchiveRow, "member_id");
    const auto E = extract(Container);
    ASSERT_EQ(field(E, "status"), "ok");
    const auto Rows = members(field(E, "extraction_id"));
    ASSERT_EQ(Rows.size(), 3U);
    const auto Source = field(source(field(Rows[0], "member_id")), "source_id");
    ASSERT_FALSE(Source.empty());
    const auto MemberAt = std::stoull(field(Rows[0], "offset"));
    constexpr uint64_t Offset = 7, Length = 5;
    auto Anchor = take(neverd_web_source_anchor_json(
        S, Revision.data(), Revision.size(), Source.data(), Source.size(),
        Offset, Length, nullptr, 0));
    const auto *Storage = Anchor.getAsObject()->getObject("storage");
    ASSERT_NE(Storage, nullptr);
    EXPECT_EQ(Storage->getString("kind"), "nexe_member");
    EXPECT_EQ(Storage->getString("container_artifact_id"), Container);
    EXPECT_EQ(Storage->getString("storage_artifact_id"), A);
    EXPECT_EQ(Storage->getString("container_byte_offset"),
              std::to_string(MemberAt));
    EXPECT_EQ(Storage->getString("member_byte_length"),
              std::to_string(Script.size()));
    const auto *Origin = Storage->getObject("container_origin");
    ASSERT_NE(Origin, nullptr);
    EXPECT_EQ(Origin->getString("kind"), "package_archive_member");
    EXPECT_EQ(Origin->getString("container_artifact_id"), A);
    EXPECT_EQ(Origin->getString("member_id"), Container);
    EXPECT_EQ(Origin->getString("container_frame_offset"),
              std::to_string(Z.Local[1]));
    if (Method == 0) {
      EXPECT_EQ(Storage->getString("mapping"), "byte_identity");
      EXPECT_EQ(Storage->getString("byte_offset"),
                std::to_string(Z.Data[1] + MemberAt + Offset));
      EXPECT_EQ(Storage->getString("byte_length"), std::to_string(Length));
      EXPECT_EQ(Storage->getString("member_byte_offset"),
                std::to_string(Z.Data[1] + MemberAt));
      EXPECT_EQ(Origin->getString("byte_offset"), std::to_string(Z.Data[1]));
      EXPECT_EQ(Origin->getString("byte_length"), std::to_string(B.size()));
    } else {
      EXPECT_EQ(Storage->getString("mapping"), "containing_compressed_frame");
      EXPECT_TRUE(Storage->get("byte_offset")->getAsNull().has_value());
      EXPECT_TRUE(Storage->get("byte_length")->getAsNull().has_value());
      EXPECT_EQ(Storage->getString("expanded_byte_offset"),
                std::to_string(5 + MemberAt + Offset));
      EXPECT_EQ(Storage->getString("expanded_byte_length"),
                std::to_string(Length));
      EXPECT_EQ(Origin->getString("expanded_byte_offset"), "5");
      EXPECT_EQ(Origin->getString("expanded_byte_length"),
                std::to_string(B.size()));
    }
    const auto P = packages(field(Rows[2], "member_id"));
    EXPECT_EQ(field(P, "status"), "ok");
    neverd_session_t Native = nullptr;
    const auto Helper = field(Rows[1], "member_id");
    const auto N = take(
        neverd_web_native_open_json(S, Revision.data(), Revision.size(),
                                    Helper.data(), Helper.size(), &Native));
    EXPECT_EQ(field(N, "status"), "ok");
    EXPECT_NE(Native, nullptr);
    neverd_session_destroy(Native);
  }
}
TEST_F(WebNexeSDK, EqualMemberBytesCannotCrossPackageNamespacesOrInheritEntry) {
#if !NEVERD_TEST_WEB_JAVASCRIPT
  GTEST_SKIP() << "Package behavior requires the parser";
#endif
  const auto B = bundle(
      {{"a.js", std::string(Script)}, {"package.json", std::string(Manifest)}});
  std::ofstream(Root / "first", std::ios::binary) << B;
  std::ofstream(Root / "second", std::ios::binary) << B;
  std::ofstream(Root / "package.json")
      << R"({"name":"CANARY_OUTER","main":"first"})";
  open(Root.string());
  ASSERT_EQ(Artifacts.size(), 4U);
  auto X = members(field(extract(Artifacts[1]), "extraction_id"));
  auto Y = members(field(extract(Artifacts[3]), "extraction_id"));
  const auto XS = field(source(field(X[0], "member_id")), "source_id");
  const auto YS = field(source(field(Y[0], "member_id")), "source_id");
  EXPECT_EQ(field(X[0], "blob_sha256"), field(Y[0], "blob_sha256"));
  EXPECT_NE(field(X[0], "member_id"), field(Y[0], "member_id"));
  const auto XP =
      field(packages(field(X[1], "member_id")), "package_analysis_id");
  EXPECT_EQ(code(aggregate(XP, YS)),
            "package_behavior_source_outside_inventory");
  const auto Direct = aggregate(XP, XS);
  ASSERT_EQ(field(Direct, "status"), "ok");
  auto DP = behaviorSources(Direct);
  const auto &D = (*DP.getAsObject()->getArray("items"))[0];
  EXPECT_EQ(field(D, "relation"), "direct_file");
  const auto OP = field(packages(Artifacts[2]), "package_analysis_id");
  const auto Embedded = aggregate(OP, XS);
  ASSERT_EQ(field(Embedded, "status"), "ok");
  auto EP = behaviorSources(Embedded);
  const auto &Row = (*EP.getAsObject()->getArray("items"))[0];
  EXPECT_EQ(field(Row, "relation"), "embedded_source");
  EXPECT_EQ(field(Row, "delivery_artifact_id"), Artifacts[1]);
  EXPECT_EQ(Row.getAsObject()->getInteger("declared_entry_count"), 0);
  EXPECT_EQ(field(Row, "package_ownership"), "not_inferred");
  EXPECT_TRUE(Row.getAsObject()->get("package_id")->getAsNull().has_value());
}
TEST_F(WebNexeSDK,
       OptionalProducerSourcesBootstrapAndManifestUseSharedConsumers) {
#if !NEVERD_TEST_WEB_JAVASCRIPT
  GTEST_SKIP() << "Source qualification requires the parser";
#endif
  const auto *RootPath = std::getenv("NEVERD_NEXE_4RC7_CORPUS");
  if (!RootPath)
    GTEST_SKIP()
        << "Set NEVERD_NEXE_4RC7_CORPUS to the pinned producer outputs";
  for (const auto *Name :
       {"linux-x64-fixture", "mac-x64-fixture", "windows-x64-fixture.exe"}) {
    open((std::filesystem::path(RootPath) / Name).string());
    const auto E = extract();
    ASSERT_EQ(field(E, "status"), "ok");
    const auto ID = field(E, "extraction_id");
    const auto M = members(ID);
    ASSERT_EQ(M.size(), 4U);
    EXPECT_EQ(field(source(field(M[1], "selection_id")), "parse_status"),
              "parsed");
    EXPECT_EQ(field(source(field(M[2], "selection_id")), "parse_status"),
              "parsed");
    EXPECT_EQ(field(packages(field(M[3], "selection_id")), "status"), "ok");
    auto R = page(ID, "regions", 1, 1);
    const auto B =
        field((*R.getAsObject()->getArray("items"))[0], "selection_id");
    EXPECT_EQ(field(source(B), "parse_status"), "parsed");
  }
}
#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebNexeSDK, CLIExtractsAndParsesWithAnUnusablePATH) {
  const auto B = bundle({{"a.js", std::string(Script)}});
  capture(B);
  const std::string Out = (Root / "out").string(),
                    Err = (Root / "err").string();
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Out, Err};
  const llvm::StringRef Env[] = {"PATH=/neverd-no-external-tools",
                                 "HOME=/neverd-no-home"};
  for (const bool Archive : {false, true}) {
    if (Archive)
      capture(ZipFixture({{"app", B, 0}}).Bytes);
    for (const bool Source : {false, true}) {
      std::vector<std::string> Owned{
          NEVERD_WEB_TEST_CLI, "web",
          Archive ? (Source ? "archive-nexe-source" : "archive-nexe")
                  : (Source ? "nexe-source" : "nexe"),
          Path, std::string(Profile)};
      if (Archive)
        Owned.insert(Owned.end(), {"zip", "0", "0"});
      else
        Owned.push_back("0");
      if (Source)
        Owned.insert(Owned.end(), {"0", "commonjs"});
      std::vector<llvm::StringRef> Args(Owned.begin(), Owned.end());
      std::ofstream(Out, std::ios::trunc).close();
      std::ofstream(Err, std::ios::trunc).close();
      EXPECT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args,
                                          llvm::ArrayRef(Env), Redirects, 30),
                Source && !NEVERD_TEST_WEB_JAVASCRIPT ? 1 : 0);
      std::ifstream F(Out), Errors(Err);
      const std::string Text{std::istreambuf_iterator<char>(F), {}};
      const std::string ErrorText{std::istreambuf_iterator<char>(Errors), {}};
      EXPECT_EQ(Text.find("CANARY"), std::string::npos);
      EXPECT_EQ(ErrorText.find("CANARY"), std::string::npos);
      EXPECT_NE(Text.find("extraction_id"), std::string::npos);
    }
  }
}
#endif
} // namespace
