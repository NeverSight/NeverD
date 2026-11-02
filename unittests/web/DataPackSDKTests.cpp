//===- DataPackSDKTests.cpp - Resource selections and shared consumers --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Privacy, nested storage, explicit decoding, exports and revision ownership.
///
//===----------------------------------------------------------------------===//

#include "DataPackFixture.h"
#include "ZipFixture.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <filesystem>
#include <fstream>
#include <optional>

namespace {
using namespace neverd::web::datapack_test;
using neverd::web::test::ZipFixture;
using Value = llvm::json::Value;
constexpr std::string_view Profile = "chromium-datapack-v5-v1";
constexpr std::string_view Codec = "gzip-rfc1952-v1";
Value take(const char *Owned) {
  if (!Owned)
    throw std::runtime_error("missing response");
  const std::string Text(Owned);
  neverd_free_string(Owned);
  EXPECT_EQ(Text.find("CANARY"), std::string::npos);
  auto V = llvm::json::parse(Text);
  if (!V)
    throw std::runtime_error(llvm::toString(V.takeError()));
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
class WebDataPackSDK : public ::testing::Test {
protected:
  neverd_web_session_t S = nullptr;
  std::filesystem::path Root;
  std::string Path, Revision, Artifact;
  void SetUp() override {
    llvm::SmallString<128> D;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-pak-sdk", D));
    Root = D.str().str();
    Path = (Root / "CANARY_INPUT").string();
    S = neverd_web_session_create();
    ASSERT_NE(S, nullptr);
  }
  void TearDown() override {
    neverd_web_session_destroy(S);
    std::error_code EC;
    if (!Root.empty())
      std::filesystem::remove_all(Root, EC);
  }
  void capture(std::string_view Bytes) {
    {
      std::ofstream Out(Path, std::ios::binary);
      Out.write(Bytes.data(), Bytes.size());
      ASSERT_TRUE(Out.good());
    }
    const auto P = take(neverd_web_import_preview_json(
        S, Path.data(), Path.size(), nullptr, 0));
    const auto Token = field(P, "preview_token");
    Revision = field(
        take(neverd_web_import_commit_json(S, Token.data(), Token.size())),
        "revision");
    const auto A = take(
        neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 0, 1));
    Artifact = field((*A.getAsObject()->getArray("items"))[0], "artifact_id");
  }
  Value extract(std::string_view Selection = {}) {
    if (Selection.empty())
      Selection = Artifact;
    return take(neverd_web_datapack_extract_json(
        S, Revision.data(), Revision.size(), Selection.data(), Selection.size(),
        Profile.data(), Profile.size()));
  }
  Value page(std::string_view ID, uint64_t Offset = 0, uint64_t Limit = 1) {
    return take(neverd_web_datapack_records_json(S, Revision.data(),
                                                 Revision.size(), ID.data(),
                                                 ID.size(), Offset, Limit));
  }
  Value decode(std::string_view Selection) {
    return take(neverd_web_datapack_decode_json(
        S, Revision.data(), Revision.size(), Selection.data(), Selection.size(),
        Codec.data(), Codec.size()));
  }
  Value source(std::string_view Selection) {
    return take(neverd_web_source_analyze_json(
        S, Revision.data(), Revision.size(), Selection.data(), Selection.size(),
        "script", 6));
  }
  Value anchor(std::string_view Source) {
    return take(neverd_web_source_anchor_json(S, Revision.data(),
                                              Revision.size(), Source.data(),
                                              Source.size(), 6, 6, nullptr, 0));
  }
  Value exported(std::string_view ID, const std::string &Output) {
    return take(neverd_web_datapack_export_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(),
        Output.data(), Output.size()));
  }
};

TEST_F(WebDataPackSDK, MetadataAliasPagingAndExplicitSourceSelection) {
  capture(pack({{7, "const CANARY=1;"}, {13, "\x7fopaque"}}, {{9, 0}}));
  const auto E = extract();
  ASSERT_EQ(field(E, "status"), "ok");
  EXPECT_EQ(field(E, "framework_identity"), "not_inferred");
  EXPECT_EQ(field(E, "resource_names"), "not_supplied");
  const auto ID = field(E, "extraction_id");
  EXPECT_EQ(field(extract(), "extraction_id"), ID);
  const auto P = page(ID);
  EXPECT_EQ(P.getAsObject()->getInteger("next_offset"), 1);
  const auto &Row = (*P.getAsObject()->getArray("items"))[0];
  const auto Selection = field(Row, "selection_id");
  const auto AliasPage = page(ID, 2);
  const auto &Alias = (*AliasPage.getAsObject()->getArray("items"))[0];
  EXPECT_EQ(field(Alias, "kind"), "alias");
  EXPECT_EQ(field(Alias, "canonical_selection_id"), Selection);
  EXPECT_NE(field(Alias, "selection_id"), Selection);
  EXPECT_EQ(field(Alias, "stored_sha256"), field(Row, "stored_sha256"));
  const auto Parsed = source(Selection);
#if NEVERD_TEST_WEB_JAVASCRIPT
  ASSERT_EQ(field(Parsed, "parse_status"), "parsed");
  const auto A = anchor(field(Parsed, "source_id"));
  const auto *Storage = A.getAsObject()->getObject("storage");
  ASSERT_NE(Storage, nullptr);
  EXPECT_EQ(Storage->getString("kind"), "datapack_stored_resource");
  EXPECT_EQ(Storage->getString("mapping"), "byte_identity");
  EXPECT_EQ(Storage->getString("byte_offset"), "40");
#else
  EXPECT_EQ(code(Parsed), "capability_unavailable");
#endif
  EXPECT_EQ(code(page(ID, 0, 0)), "invalid_page");
  EXPECT_EQ(code(page(ID, 4)), "invalid_page");
  EXPECT_EQ(code(page(ID, 0, 513)), "invalid_page");
}

TEST_F(WebDataPackSDK, ExplicitDecodingKeepsOriginalAndExpandedOrigins) {
  const auto B = pack({{7, gzipStored("const CANARY=1;")}, {13, "invalid"}});
  capture(B);
  const auto E = extract();
  ASSERT_EQ(field(E, "status"), "ok");
  const auto ID = field(E, "extraction_id");
  const auto P = page(ID);
  const auto Selection =
      field((*P.getAsObject()->getArray("items"))[0], "selection_id");
  const auto D = decode(Selection);
  if (field(D, "status") == "error") {
    EXPECT_EQ(code(D), "gzip_decoder_unavailable");
    return;
  }
  const auto Decoded = field(D, "selection_id");
  EXPECT_NE(Decoded, Selection);
  EXPECT_EQ(field(decode(Selection), "selection_id"), Decoded);
  const auto Parsed = source(Decoded);
#if NEVERD_TEST_WEB_JAVASCRIPT
  ASSERT_EQ(field(Parsed, "parse_status"), "parsed");
  const auto A = anchor(field(Parsed, "source_id"));
  const auto *Storage = A.getAsObject()->getObject("storage");
  ASSERT_NE(Storage, nullptr);
  EXPECT_EQ(Storage->getString("kind"), "datapack_decoded_resource");
  EXPECT_EQ(Storage->getString("mapping"), "containing_compressed_frame");
  EXPECT_EQ(Storage->getString("expanded_byte_offset"), "6");
  EXPECT_TRUE(Storage->get("byte_offset")->getAsNull().has_value());
  const auto *Stored = Storage->getObject("stored_origin");
  ASSERT_NE(Stored, nullptr);
  EXPECT_EQ(Stored->getString("container_byte_offset"), "30");
  EXPECT_EQ(Stored->getString("storage_artifact_id"), Artifact);
#else
  EXPECT_EQ(code(Parsed), "capability_unavailable");
#endif
  const auto BadPage = page(ID, 1);
  const auto Bad =
      field((*BadPage.getAsObject()->getArray("items"))[0], "selection_id");
  EXPECT_EQ(code(decode(Bad)), "gzip_invalid_stream");
  const auto Metadata = take(neverd_web_metadata_json(S));
  EXPECT_EQ(
      Metadata.getAsObject()->getInteger("decoded_datapack_resource_count"), 1);
  const auto Output = (Root / "export").string();
  EXPECT_EQ(field(exported(ID, Output), "export_status"), "complete");
  EXPECT_EQ(code(exported(ID, Output)), "export_destination_exists");
  std::ifstream F(Root / "export" / "decoded-0.bin", std::ios::binary);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(F), {}),
            "const CANARY=1;");
  EXPECT_TRUE(std::filesystem::exists(Root / "export" / "manifest.json"));
}

TEST_F(WebDataPackSDK, StoredAndDeflateZIPRetainTheirDistinctOffsetBasis) {
  const auto B = pack({{7, "const CANARY=1;"}}, {{9, 0}});
  for (const uint16_t Method : {0, 8}) {
    const ZipFixture Z({{"pad", "12345", Method}, {"app.pak", B, Method}});
    capture(Z.Bytes);
    const auto Archive = take(neverd_web_package_archive_extract_json(
        S, Revision.data(), Revision.size(), Artifact.data(), Artifact.size(),
        "zip", 3));
    if (field(Archive, "status") == "error") {
      EXPECT_EQ(code(Archive), "package_archive_path_policy_unavailable");
      return;
    }
    const auto ArchiveID = field(Archive, "archive_id");
    const auto AP = take(neverd_web_package_archive_records_json(
        S, Revision.data(), Revision.size(), ArchiveID.data(), ArchiveID.size(),
        1, 1));
    const auto &AR = (*AP.getAsObject()->getArray("items"))[0];
    if (field(AR, "unavailable_reason") == "deflate_unavailable")
      continue;
    const auto E = extract(field(AR, "member_id"));
    ASSERT_EQ(field(E, "status"), "ok");
    const auto P = page(field(E, "extraction_id"));
    const auto Selection =
        field((*P.getAsObject()->getArray("items"))[0], "selection_id");
    const auto Parsed = source(Selection);
#if NEVERD_TEST_WEB_JAVASCRIPT
    ASSERT_EQ(field(Parsed, "parse_status"), "parsed");
    const auto A = anchor(field(Parsed, "source_id"));
    const auto *Storage = A.getAsObject()->getObject("storage");
    ASSERT_NE(Storage, nullptr);
    EXPECT_EQ(Storage->getString("container_byte_offset"), "28");
    EXPECT_EQ(Storage->getString("storage_artifact_id"), Artifact);
    if (Method == 0) {
      EXPECT_EQ(Storage->getString("byte_offset"),
                std::to_string(Z.Data[1] + 28 + 6));
    } else {
      EXPECT_TRUE(Storage->get("byte_offset")->getAsNull().has_value());
      EXPECT_EQ(Storage->getString("expanded_byte_offset"), "39");
    }
    ASSERT_NE(Storage->getObject("container_origin"), nullptr);
#else
    EXPECT_EQ(code(Parsed), "capability_unavailable");
#endif
  }
}

TEST_F(WebDataPackSDK, FailedIndexesAndRevisionReplacementPublishNoSelections) {
  capture(pack({{7, "a"}}, {{9, 1}}));
  EXPECT_EQ(code(extract()), "datapack_alias_index_out_of_range");
  EXPECT_EQ(take(neverd_web_metadata_json(S))
                .getAsObject()
                ->getInteger("datapack_count"),
            0);
  capture(pack({{7, gzipStored("const CANARY=1;")}}));
  const auto ID = field(extract(), "extraction_id");
  const auto P = page(ID);
  const auto Selection =
      field((*P.getAsObject()->getArray("items"))[0], "selection_id");
  const auto D = decode(Selection);
  const auto Old = Revision;
  capture(pack({{7, "replacement"}}));
  EXPECT_EQ(code(take(neverd_web_datapack_records_json(
                S, Old.data(), Old.size(), ID.data(), ID.size(), 0, 1))),
            "stale_revision");
  EXPECT_EQ(code(page(ID)), "unknown_datapack_extraction");
  EXPECT_EQ(code(decode(Selection)), "unknown_datapack_resource");
  EXPECT_EQ(take(neverd_web_metadata_json(S))
                .getAsObject()
                ->getInteger("decoded_datapack_resource_count"),
            0);
  if (field(D, "status") == "ok")
#if NEVERD_TEST_WEB_JAVASCRIPT
    EXPECT_EQ(code(source(field(D, "selection_id"))), "unknown_artifact");
#else
    EXPECT_EQ(code(source(field(D, "selection_id"))), "capability_unavailable");
#endif
}

TEST_F(WebDataPackSDK, DecodedCacheLimitPreservesAlreadyPublishedBytes) {
  std::vector<std::pair<uint16_t, std::string>> Resources;
  for (uint16_t I = 0; I < 17; ++I)
    Resources.emplace_back(I + 1, gzipStored("const CANARY=1;"));
  capture(pack(Resources));
  const auto ID = field(extract(), "extraction_id");
  std::string First;
  for (uint64_t I = 0; I < 17; ++I) {
    const auto P = page(ID, I);
    const auto Selection =
        field((*P.getAsObject()->getArray("items"))[0], "selection_id");
    const auto D = decode(Selection);
    if (!I && field(D, "status") == "error") {
      EXPECT_EQ(code(D), "gzip_decoder_unavailable");
      return;
    }
    if (I == 16)
      EXPECT_EQ(code(D), "datapack_decoded_cache_budget_exceeded");
    else
      EXPECT_EQ(field(D, "status"), "ok");
    if (!I)
      First = Selection;
  }
  EXPECT_EQ(field(decode(First), "status"), "ok");
}

#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebDataPackSDK, CLIUsesSharedConsumersWithAnUnusableExternalPATH) {
  const auto Capabilities = take(neverd_web_capabilities_json());
  const auto *Analyses = Capabilities.getAsObject()->getArray("analysis");
  ASSERT_NE(Analyses, nullptr);
  std::optional<bool> ArchiveAvailable;
  for (const auto &Analysis : *Analyses)
    if (field(Analysis, "kind") == "package_archive")
      ArchiveAvailable = Analysis.getAsObject()->getBoolean("available");
  ASSERT_TRUE(ArchiveAvailable.has_value());
  const auto B = pack({{7, gzipStored("const CANARY=1;")}});
  const llvm::StringRef Env[] = {"PATH=/neverd-no-external-tools"};
  for (const bool Archive : {false, true}) {
    capture(Archive ? ZipFixture({{"app.pak", B, 0}}).Bytes : B);
    for (const bool Source : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "archive=" << Archive << ", source=" << Source
                   << ", archive_available=" << *ArchiveAvailable);
      // LLVM redirects do not truncate an existing file. Keep shorter refusal
      // replies separate from the successful command's longer output.
      const auto Case = std::string(Archive ? "archive" : "direct") +
                        (Source ? "-source" : "-index");
      const auto Out = (Root / ("out-" + Case)).string();
      const auto Err = (Root / ("err-" + Case)).string();
      const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Out,
                                                          Err};
      std::vector<std::string> Owned{
          NEVERD_WEB_TEST_CLI, "web",
          Archive ? (Source ? "archive-pak-source" : "archive-pak")
                  : (Source ? "pak-source" : "pak"),
          Path, std::string(Profile)};
      if (Archive)
        Owned.insert(Owned.end(), {"zip", "0", "0"});
      else
        Owned.push_back("0");
      if (Source)
        Owned.insert(Owned.end(), {"0", "gzip", "script"});
      const std::vector<llvm::StringRef> Args(Owned.begin(), Owned.end());
      const auto Exit = llvm::sys::ExecuteAndWait(
          NEVERD_WEB_TEST_CLI, Args, llvm::ArrayRef(Env), Redirects, 30);
      const bool ArchiveOmitted = Archive && !*ArchiveAvailable;
      EXPECT_EQ(Exit, ArchiveOmitted || (Source && !NEVERD_TEST_WEB_JAVASCRIPT)
                          ? 1
                          : 0);
      std::ifstream F(Out), Errors(Err);
      const std::string Text(std::istreambuf_iterator<char>(F), {});
      EXPECT_EQ(Text.find("CANARY"), std::string::npos);
      if (ArchiveOmitted) {
        auto Parsed = llvm::json::parse(Text);
        ASSERT_TRUE(bool(Parsed)) << Text;
        EXPECT_EQ(code(*Parsed), "package_archive_path_policy_unavailable");
      } else {
        EXPECT_NE(Text.find("extraction_id"), std::string::npos);
      }
      EXPECT_EQ(std::string(std::istreambuf_iterator<char>(Errors), {}), "");
    }
  }
}
#endif
} // namespace
