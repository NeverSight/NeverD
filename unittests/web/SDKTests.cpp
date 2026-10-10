#include "BunFixture.h"
#include "BunSourceMapFixture.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {
namespace fs = std::filesystem;
using llvm::json::Value;

Value take(const char *Owned) {
  if (!Owned) {
    ADD_FAILURE() << "SDK did not return an owned result";
    return llvm::json::Object{};
  }
  auto Parsed = llvm::json::parse(Owned);
  neverd_free_string(Owned);
  if (!Parsed) {
    ADD_FAILURE() << llvm::toString(Parsed.takeError());
    return llvm::json::Object{};
  }
  return std::move(*Parsed);
}

std::string field(const Value &Result, const char *Name) {
  if (const auto *Object = Result.getAsObject())
    if (const auto Text = Object->getString(Name))
      return Text->str();
  ADD_FAILURE() << "SDK response is missing " << Name;
  return {};
}

std::string errorCode(const Value &Result) {
  EXPECT_EQ(field(Result, "status"), "error");
  if (const auto *Object = Result.getAsObject())
    if (const auto *Error = Object->getObject("error"))
      return Error->getString("code").value_or("").str();
  return {};
}

class WebSDK : public ::testing::Test {
protected:
  neverd_web_session_t Session = nullptr;
  fs::path Root;
  std::string Path;
  void SetUp() override {
    Session = neverd_web_session_create();
    ASSERT_NE(Session, nullptr);
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-web-sdk", Directory));
    Root = Directory.str().str();
    Path = (Root / "input.js").string();
  }
  void TearDown() override {
    neverd_web_session_destroy(Session);
    std::error_code Error;
    fs::remove_all(Root, Error);
  }
  void write(std::string_view Source) {
    std::ofstream Out(Path, std::ios::binary);
    Out.write(Source.data(), Source.size());
    ASSERT_TRUE(Out.good());
  }
  std::string preview() {
    return field(take(neverd_web_import_preview_json(Session, Path.data(),
                                                     Path.size(), nullptr, 0)),
                 "preview_token");
  }
  Value commit(std::string_view Token) {
    return take(
        neverd_web_import_commit_json(Session, Token.data(), Token.size()));
  }
  Value page(std::string_view Revision, uint64_t Offset = 0,
             uint64_t Limit = 1) {
    return take(neverd_web_artifacts_json(Session, Revision.data(),
                                          Revision.size(), Offset, Limit));
  }
  std::string rootID(std::string_view Revision) {
    auto Page = page(Revision);
    auto *Items = Page.getAsObject()->getArray("items");
    if (!Items || Items->empty()) {
      ADD_FAILURE() << "Missing root artifact";
      return {};
    }
    return field((*Items)[0], "artifact_id");
  }
  Value analyze(std::string_view Revision, std::string_view ID,
                std::string_view Type = "script") {
    return take(neverd_web_source_analyze_json(
        Session, Revision.data(), Revision.size(), ID.data(), ID.size(),
        Type.data(), Type.size()));
  }
  Value nodes(std::string_view Revision, std::string_view ID,
              uint64_t Offset = 0, uint64_t Limit = 1) {
    return take(neverd_web_source_nodes_json(Session, Revision.data(),
                                             Revision.size(), ID.data(),
                                             ID.size(), Offset, Limit));
  }
  Value map(std::string_view Revision, std::string_view ID) {
    return take(neverd_web_source_map_analyze_json(
        Session, Revision.data(), Revision.size(), ID.data(), ID.size()));
  }
  Value bindings(std::string_view Revision, std::string_view ID) {
    return take(neverd_web_source_bindings_analyze_json(
        Session, Revision.data(), Revision.size(), ID.data(), ID.size()));
  }
  Value viewPreview(std::string_view Revision, std::string_view ID,
                    std::string_view Options = {}) {
    return take(neverd_web_source_view_preview_json(
        Session, Revision.data(), Revision.size(), ID.data(), ID.size(),
        Options.data(), Options.size()));
  }
  Value navigation(std::string_view Revision, std::string_view ID) {
    return take(neverd_web_source_navigation_analyze_json(
        Session, Revision.data(), Revision.size(), ID.data(), ID.size()));
  }
  Value navigationRecords(std::string_view Revision, std::string_view ID,
                          std::string_view Kind, uint64_t Offset = 0,
                          uint64_t Limit = 512) {
    return take(neverd_web_source_navigation_records_json(
        Session, Revision.data(), Revision.size(), ID.data(), ID.size(),
        Kind.data(), Kind.size(), Offset, Limit));
  }
  Value anchor(std::string_view Revision, std::string_view ID, uint64_t Offset,
               uint64_t Length, std::string_view View = {}) {
    return take(neverd_web_source_anchor_json(
        Session, Revision.data(), Revision.size(), ID.data(), ID.size(), Offset,
        Length, View.data(), View.size()));
  }
  Value viewCommit(std::string_view Revision, std::string_view Token) {
    return take(neverd_web_source_view_commit_json(
        Session, Revision.data(), Revision.size(), Token.data(), Token.size()));
  }
  Value viewRecords(std::string_view Revision, std::string_view ID,
                    uint64_t Offset = 0, uint64_t Limit = 512) {
    return take(neverd_web_source_view_records_json(Session, Revision.data(),
                                                    Revision.size(), ID.data(),
                                                    ID.size(), Offset, Limit));
  }
  Value viewChunk(std::string_view Revision, std::string_view ID,
                  uint64_t Offset = 0, uint64_t Limit = 65536) {
    return take(neverd_web_source_view_chunk_json(Session, Revision.data(),
                                                  Revision.size(), ID.data(),
                                                  ID.size(), Offset, Limit));
  }
  Value bindingRecords(std::string_view Revision, std::string_view ID,
                       std::string_view Kind, uint64_t Offset = 0,
                       uint64_t Limit = 512) {
    return take(neverd_web_source_binding_records_json(
        Session, Revision.data(), Revision.size(), ID.data(), ID.size(),
        Kind.data(), Kind.size(), Offset, Limit));
  }
  Value semantics(std::string_view Revision, std::string_view ID) {
    return take(neverd_web_source_semantics_analyze_json(
        Session, Revision.data(), Revision.size(), ID.data(), ID.size()));
  }
  Value semanticRecords(std::string_view Revision, std::string_view ID,
                        uint64_t Offset = 0, uint64_t Limit = 512) {
    return take(neverd_web_source_semantic_records_json(
        Session, Revision.data(), Revision.size(), ID.data(), ID.size(), Offset,
        Limit));
  }
  Value mapSources(std::string_view Revision, std::string_view ID,
                   uint64_t Offset = 0, uint64_t Limit = 512) {
    return take(neverd_web_source_map_sources_json(Session, Revision.data(),
                                                   Revision.size(), ID.data(),
                                                   ID.size(), Offset, Limit));
  }
  Value modules(std::string_view Revision, std::string_view ID) {
    return take(neverd_web_source_modules_analyze_json(
        Session, Revision.data(), Revision.size(), ID.data(), ID.size()));
  }
  Value bundles(std::string_view Revision, std::string_view ID) {
    return take(neverd_web_source_bundles_analyze_json(
        Session, Revision.data(), Revision.size(), ID.data(), ID.size()));
  }
  Value bun(std::string_view Revision, std::string_view ID) {
    return take(neverd_web_bun_extract_json(
        Session, Revision.data(), Revision.size(), ID.data(), ID.size()));
  }
  Value bunRecords(std::string_view Revision, std::string_view ID,
                   std::string_view Kind, uint64_t Offset = 0,
                   uint64_t Limit = 512) {
    return take(neverd_web_bun_records_json(
        Session, Revision.data(), Revision.size(), ID.data(), ID.size(),
        Kind.data(), Kind.size(), Offset, Limit));
  }
  Value bundleRecords(std::string_view Revision, std::string_view ID,
                      std::string_view Kind, uint64_t Offset = 0,
                      uint64_t Limit = 512) {
    return take(neverd_web_source_bundle_records_json(
        Session, Revision.data(), Revision.size(), ID.data(), ID.size(),
        Kind.data(), Kind.size(), Offset, Limit));
  }
  void writeBundle() {
    write(R"JS((() => {
      var table = {
        'SECRET_BUNDLE_KEY': (m,e,r) => { r('SECRET_SECOND'); },
        'SECRET_SECOND': (m,e,r) => { r('SECRET_BUNDLE_KEY'); }
      };
      var cache = {};
      function load(id) {
        var cached = cache[id];
        if (cached !== undefined) { return cached.exports; }
        var module = cache[id] = {exports:{}};
        table[id](module, module.exports, load);
        return module.exports;
      }
      load('SECRET_BUNDLE_KEY');
    })(); )JS");
  }
  Value moduleRecords(std::string_view Revision, std::string_view ID,
                      std::string_view Kind, uint64_t Offset = 0,
                      uint64_t Limit = 512) {
    return take(neverd_web_source_module_records_json(
        Session, Revision.data(), Revision.size(), ID.data(), ID.size(),
        Kind.data(), Kind.size(), Offset, Limit));
  }
  Value mapLookup(std::string_view Revision, std::string_view MapID,
                  std::string_view SourceID, uint64_t Byte) {
    return take(neverd_web_source_map_lookup_json(
        Session, Revision.data(), Revision.size(), MapID.data(), MapID.size(),
        SourceID.data(), SourceID.size(), Byte));
  }
};

TEST_F(WebSDK, OwnershipAndBorrowedBufferAdmission) {
  EXPECT_EQ(errorCode(take(neverd_web_metadata_json(nullptr))),
            "invalid_session");
  EXPECT_EQ(errorCode(take(neverd_web_metadata_json(Session))), "no_project");
  EXPECT_EQ(errorCode(take(neverd_web_import_preview_json(Session, nullptr, 1,
                                                          nullptr, 0))),
            "invalid_buffer");
  const char Canary[] = "SECRET_BUFFER_CANARY";
  EXPECT_EQ(errorCode(take(neverd_web_import_preview_json(
                Session, Canary, SIZE_MAX, nullptr, 0))),
            "invalid_buffer");
  EXPECT_EQ(errorCode(take(neverd_web_import_commit_json(Session, Canary,
                                                         sizeof(Canary) - 1))),
            "stale_preview");
  EXPECT_EQ(field(take(neverd_web_capabilities_json()), "status"), "ok");
  neverd_web_session_destroy(nullptr);
  neverd_free_string(nullptr);
}

#ifndef _WIN32
TEST_F(WebSDK, BunSerializedMapsRetainPrivateSourcesAndUnverifiedAssociation) {
  using namespace neverd::web::test;
  write(BunFixture(true, simpleBunMap()).Bytes);
  ASSERT_EQ(field(commit(preview()), "revision"), "1");
  const auto Container = rootID("1");
  const auto Summary = bun("1", Container);
  const auto Extraction = field(Summary, "extraction_id");
  const auto Modules = bunRecords("1", Extraction, "modules");
  const auto &M = (*Modules.getAsObject()->getArray("items"))[0];
  const auto Region = field(M, "source_map_region_id");
  const auto Decoded = map("1", Region);
  if (field(Summary, "source_map_decoding") == "zstd_unavailable") {
    EXPECT_EQ(errorCode(Decoded), "bun_source_map_zstd_unavailable");
    return;
  }
  ASSERT_EQ(field(Decoded, "status"), "ok");
  EXPECT_EQ(field(Decoded, "profile"), "bun-1.4.2-serialized-source-map-v1");
  EXPECT_EQ(field(Decoded, "mapping_coverage"), "retained_mapped_anchors_only");
  EXPECT_EQ(field(Decoded, "name_metadata"), "discarded_by_producer");
  EXPECT_EQ(field(Decoded, "unmapped_boundaries"), "discarded_by_producer");
  EXPECT_EQ(field(Decoded, "association_status"), "container_assertion");
  EXPECT_EQ(Decoded.getAsObject()->getBoolean("provenance_verified"), false);
  const auto ID = field(Decoded, "map_id");
  EXPECT_EQ(field(map("1", Region), "map_id"), ID);
  const auto Sources = take(neverd_web_source_map_sources_json(
      Session, "1", 1, ID.data(), ID.size(), 0, 512));
  ASSERT_EQ(Sources.getAsObject()->getArray("items")->size(), 1);
  const auto &S = (*Sources.getAsObject()->getArray("items"))[0];
  EXPECT_TRUE(S.getAsObject()->get("ignored_claim")->getAsNull());
  EXPECT_EQ(field(S, "origin_kind"), "bun_source_map_zstd_content");
  const auto *Storage = S.getAsObject()->getObject("storage");
  ASSERT_NE(Storage, nullptr);
  EXPECT_EQ(Storage->getString("container_artifact_id"), Container);
  EXPECT_NE(Storage->getString("content_sha256"),
            S.getAsObject()->getString("blob_sha256"));
  const auto Segments = take(neverd_web_source_map_segments_json(
      Session, "1", 1, ID.data(), ID.size(), 0, 512));
  ASSERT_EQ(Segments.getAsObject()->getArray("items")->size(), 2);
  for (const auto &V : {Decoded, Sources, Segments})
    for (const auto Canary : {"SECRET_MAP", "CANARY", "https://", "secret ="})
      EXPECT_EQ(llvm::formatv("{0}", V).str().find(Canary), std::string::npos);
#if NEVERD_TEST_WEB_JAVASCRIPT
  const auto Generated = analyze("1", field(M, "source_artifact_id"), "module");
  ASSERT_EQ(field(Generated, "parse_status"), "parsed");
  auto Lookup = mapLookup("1", ID, field(Generated, "source_id"), 8);
  EXPECT_EQ(field(Lookup, "association_kind"), "container_assertion");
  const auto &Anchor = (*Lookup.getAsObject()->getArray("items"))[0];
  EXPECT_EQ(field(Anchor, "selection"), "preceding_anchor");
  EXPECT_EQ(field(Anchor, "mapping_scope"),
            "anchor_only_intervening_boundaries_unknown");
  EXPECT_EQ(field(Anchor, "original_byte"), "7");
  EXPECT_EQ(field(Anchor, "generated_byte"), "7");
  const auto Original = analyze("1", field(S, "artifact_id"), "module");
  ASSERT_EQ(field(Original, "parse_status"), "parsed");
  EXPECT_NE(field(Original, "source_id"), field(Generated, "source_id"));
  Lookup = mapLookup("1", ID, field(Original, "source_id"), 7);
  EXPECT_EQ(field(Lookup, "association_kind"), "caller_assertion");
#endif
  // A malformed replacement can still expose raw extraction evidence, but
  // cannot retain the preceding revision's decoded map or publish a new one.
  write(BunFixture(true, "SECRET_MAP").Bytes);
  ASSERT_EQ(field(commit(preview()), "revision"), "2");
  const auto Next = field(bun("2", rootID("2")), "extraction_id");
  const auto NextModules = bunRecords("2", Next, "modules");
  const auto &NextM = (*NextModules.getAsObject()->getArray("items"))[0];
  EXPECT_EQ(errorCode(map("2", field(NextM, "source_map_region_id"))),
            "source_map_source_budget_exceeded");
  EXPECT_EQ(errorCode(take(neverd_web_source_map_sources_json(
                Session, "2", 1, ID.data(), ID.size(), 0, 1))),
            "unknown_source_map");
  EXPECT_EQ(field(bunRecords("2", Next, "regions"), "status"), "ok");
  EXPECT_EQ(take(neverd_web_metadata_json(Session))
                .getAsObject()
                ->getInteger("source_map_count"),
            0);
}

TEST_F(WebSDK, BunExtractionIsImmutablePagedAndSourceAnchored) {
  const neverd::web::test::BunFixture F;
  write(F.Bytes);
  ASSERT_EQ(field(commit(preview()), "revision"), "1");
  const auto ID = rootID("1");
  const auto Summary = bun("1", ID);
  ASSERT_EQ(field(Summary, "status"), "ok");
  const auto Extraction = field(Summary, "extraction_id");
  EXPECT_EQ(Summary.getAsObject()->getBoolean("producer_version_verified"),
            false);
  EXPECT_EQ(field(bun("1", ID), "extraction_id"), Extraction);
  const auto First = bunRecords("1", Extraction, "modules", 0, 1);
  ASSERT_EQ(field(First, "status"), "ok");
  EXPECT_EQ(First.getAsObject()->getInteger("next_offset"), 1);
  const auto *Items = First.getAsObject()->getArray("items");
  ASSERT_EQ(Items->size(), 1);
  const auto SourceArtifact = field((*Items)[0], "source_artifact_id");
#if NEVERD_TEST_WEB_JAVASCRIPT
  const auto Source = analyze("1", SourceArtifact, "module");
  ASSERT_EQ(field(Source, "parse_status"), "parsed");
  EXPECT_EQ(field(Source, "artifact_id"), SourceArtifact);
  const auto SourceID = field(Source, "source_id");
  const auto B = bindings("1", SourceID);
  EXPECT_EQ(field(B, "binding_status"), "ok");
  const auto N = nodes("1", SourceID, 0, 512);
  EXPECT_FALSE(N.getAsObject()->getArray("items")->empty());
#else
  EXPECT_EQ(errorCode(analyze("1", SourceArtifact, "module")),
            "capability_unavailable");
#endif
  const auto Regions = bunRecords("1", Extraction, "regions");
  for (const auto &R : *Regions.getAsObject()->getArray("items")) {
    const auto Kind = field(R, "kind");
    if (Kind == "module_name" || Kind == "bytecode_origin") {
      EXPECT_EQ(R.getAsObject()->getBoolean("hash_redacted"), true);
      EXPECT_TRUE(R.getAsObject()->get("blob_sha256")->getAsNull());
    }
  }
  for (const auto &V : {Summary, First, Regions}) {
    const auto Text = llvm::formatv("{0}", V).str();
    for (const auto Canary : {"SECRET", "CANARY", "ORIGIN", "bunfs", "中文"})
      EXPECT_EQ(Text.find(Canary), std::string::npos);
  }
  EXPECT_EQ(errorCode(bunRecords("1", Extraction, "modules", UINT64_MAX)),
            "invalid_page");
  EXPECT_EQ(errorCode(bunRecords("1", Extraction, "modules", 0, 0)),
            "invalid_page");
  EXPECT_EQ(errorCode(bunRecords("1", Extraction, "modules", 0, 513)),
            "invalid_page");
  EXPECT_EQ(errorCode(bunRecords("1", Extraction, "CANARY")),
            "unsupported_bun_record_kind");
  EXPECT_EQ(errorCode(bun("0", ID)), "stale_revision");
  write("changed");
  EXPECT_EQ(field(bun("1", ID), "extraction_id"), Extraction);
  ASSERT_EQ(field(commit(preview()), "revision"), "2");
  EXPECT_EQ(errorCode(bunRecords("2", Extraction, "modules")),
            "unknown_bun_extraction");
  EXPECT_EQ(errorCode(bun("2", rootID("2"))), "bun_unsupported_container");
  EXPECT_EQ(take(neverd_web_metadata_json(Session))
                .getAsObject()
                ->getInteger("bun_extraction_count"),
            0);
}

TEST_F(WebSDK, BunCompilerSourcesReachTheEmbeddedParserWithDistinctProvenance) {
  uint64_t Revision = 0;
  for (const auto Name :
       {"plain", "unicode", "utf16", "asset-map", "cache-map"}) {
    SCOPED_TRACE(Name);
    std::ifstream File(std::string(NEVERD_WEB_FIXTURE_DIR) + "/bun/" + Name +
                           ".graph.bin",
                       std::ios::binary);
    ASSERT_TRUE(File.good());
    const std::string Graph{std::istreambuf_iterator<char>(File), {}};
    write(neverd::web::test::bunELF(Graph));
    const auto Rev = std::to_string(++Revision);
    ASSERT_EQ(field(commit(preview()), "revision"), Rev);
    const auto Extraction = field(bun(Rev, rootID(Rev)), "extraction_id");
    const auto Page = bunRecords(Rev, Extraction, "modules");
    const auto *Members = Page.getAsObject()->getArray("items");
    ASSERT_NE(Members, nullptr);
    for (const auto &M : *Members) {
      const auto ID = M.getAsObject()->getString("source_artifact_id");
      if (!ID)
        continue;
      EXPECT_NE(*ID, M.getAsObject()->getString("content_region_id"));
#if NEVERD_TEST_WEB_JAVASCRIPT
      const auto Source = analyze(Rev, *ID, field(M, "source_type_hint"));
      ASSERT_EQ(field(Source, "parse_status"), "parsed");
      const auto SourceID = field(Source, "source_id");
      EXPECT_EQ(field(bindings(Rev, SourceID), "binding_status"), "ok");
      EXPECT_EQ(llvm::formatv("{0}", Source).str().find("CANARY"),
                std::string::npos);
      const auto Preview = viewPreview(Rev, SourceID);
      ASSERT_EQ(field(Preview, "status"), "ok");
      EXPECT_EQ(field(Preview, "artifact_id"), *ID);
      EXPECT_EQ(field(Preview, "source_sha256"), field(Source, "blob_sha256"));
      const auto View = field(Preview, "view_id");
      ASSERT_EQ(
          field(viewCommit(Rev, field(Preview, "preview_token")), "status"),
          "ok");
      const auto Chunk = viewChunk(Rev, View);
      EXPECT_EQ(field(Chunk, "text").find("CANARY"), std::string::npos);
      EXPECT_EQ(field(Chunk, "reviewed_bytes"), "0");
      EXPECT_EQ(field(navigation(Rev, SourceID), "navigation_status"), "ok");
      const auto Start = anchor(Rev, SourceID, 0, 0, View);
      ASSERT_EQ(field(Start, "status"), "ok");
      EXPECT_EQ(
          Start.getAsObject()->getObject("storage")->getString("region_id"),
          M.getAsObject()->getString("content_region_id"));
#else
      EXPECT_EQ(errorCode(analyze(Rev, *ID, field(M, "source_type_hint"))),
                "capability_unavailable");
#endif
    }
  }
}

#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebSDK, BunMapCLIWorksWithNoExternalToolsAndRedactsSourcePaths) {
  using namespace neverd::web::test;
  write(BunFixture(true, simpleBunMap()).Bytes);
  const auto Summary = take(neverd_web_capabilities_json());
  bool Available = false;
  for (const auto &A : *Summary.getAsObject()->getArray("analysis"))
    if (A.getAsObject()->getString("kind") == "bun_serialized_source_map")
      Available = true;
  const auto Output = (Root / "bun-map-output").string();
  const auto Err = (Root / "bun-map-error").string();
  const llvm::StringRef Args[] = {NEVERD_WEB_TEST_CLI, "web", "bun-map", Path,
                                  "0"};
  const llvm::StringRef Environment[] = {"PATH=/neverd-no-external-tools",
                                         "NEVERD_SIGNATURE_CACHE=off"};
  const std::optional<llvm::StringRef> Redirects[] = {llvm::StringRef(), Output,
                                                      Err};
  EXPECT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Environment,
                                      Redirects, 30),
            Available ? 0 : 1);
  std::ifstream File(Output);
  const std::string Text{std::istreambuf_iterator<char>(File), {}};
  EXPECT_NE(Text.find(Available ? "bun-1.4.2-serialized-source-map-v1"
                                : "bun_source_map_zstd_unavailable"),
            std::string::npos);
  EXPECT_EQ(Text.find("CANARY"), std::string::npos);
  EXPECT_EQ(Text.find("SECRET_MAP"), std::string::npos);
}

TEST_F(WebSDK, BunCLIUsesNoExternalRuntimeAndKeepsNamesPrivate) {
  write(neverd::web::test::BunFixture().Bytes);
  const auto Output = (Root / "bun-output").string();
  const auto Err = (Root / "bun-error").string();
  const llvm::StringRef Args[] = {NEVERD_WEB_TEST_CLI, "web", "bun", Path};
  const llvm::StringRef Environment[] = {"PATH=/neverd-no-external-tools",
                                         "NEVERD_SIGNATURE_CACHE=off"};
  const std::optional<llvm::StringRef> Redirects[] = {llvm::StringRef(), Output,
                                                      Err};
  EXPECT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Environment,
                                      Redirects, 30),
            0);
  std::ifstream File(Output);
  std::string Text{std::istreambuf_iterator<char>(File), {}};
  EXPECT_NE(Text.find("bun-1.4.2-linux-x64-elf-v1"), std::string::npos);
  EXPECT_NE(Text.find("source_artifact_id"), std::string::npos);
  EXPECT_EQ(Text.find("CANARY"), std::string::npos);
  EXPECT_EQ(Text.find("SECRET"), std::string::npos);
}
#endif

TEST_F(WebSDK, LargeOriginalImportDoesNotExpandSourceOrMapParsingBudgets) {
  write("SECRET_LARGE_ORIGINAL");
  constexpr uint64_t Size = 65ULL * 1024 * 1024 + 1;
  fs::resize_file(Path, Size);
  const auto Capabilities = take(neverd_web_capabilities_json());
  EXPECT_EQ(field(Capabilities, "artifact_storage"), "posix-unlinked-spool-v1");
  const auto Opened = commit(preview());
  ASSERT_EQ(field(Opened, "status"), "ok");
  EXPECT_EQ(field(Opened, "input_bytes"), std::to_string(Size));
  const auto ID = rootID("1");
  const auto Page = page("1");
  ASSERT_EQ(Page.getAsObject()->getArray("items")->size(), 1);
  EXPECT_EQ(field((*Page.getAsObject()->getArray("items"))[0], "size"),
            std::to_string(Size));
  EXPECT_EQ(errorCode(map("1", ID)), "json_byte_budget_exceeded");
#if NEVERD_TEST_WEB_JAVASCRIPT
  EXPECT_EQ(errorCode(analyze("1", ID)), "source_byte_budget_exceeded");
#else
  EXPECT_EQ(errorCode(analyze("1", ID)), "capability_unavailable");
#endif
  EXPECT_EQ(field(take(neverd_web_metadata_json(Session)), "analysis_status"),
            "not_analyzed");
  EXPECT_EQ(llvm::formatv("{0}", Page).str().find("SECRET"), std::string::npos);
  // A missing input on commit consumes its token without replacing this spool.
  const auto Pending = preview();
  fs::remove(Path);
  EXPECT_EQ(errorCode(commit(Pending)), "input_unavailable");
  EXPECT_EQ(rootID("1"), ID);
  EXPECT_EQ(errorCode(commit(Pending)), "stale_preview");
}

TEST_F(WebSDK, RevisionsAndPaginationPreserveThePublishedProject) {
  write("let x = 1;");
  EXPECT_EQ(field(commit(preview()), "revision"), "1");
  auto First = page("1");
  EXPECT_EQ(First.getAsObject()->getBoolean("page_complete"), true);
  EXPECT_EQ(page("1", 1).getAsObject()->getArray("items")->size(), 0);
  EXPECT_EQ(errorCode(page("1", UINT64_MAX)), "invalid_page");
  EXPECT_EQ(errorCode(page("1", 0, 0)), "invalid_page");
  const auto Pending = preview();
  write("let changed = 2;");
  EXPECT_EQ(errorCode(commit(Pending)), "input_changed");
  EXPECT_EQ(field(take(neverd_web_metadata_json(Session)), "revision"), "1");
  EXPECT_EQ(errorCode(commit(Pending)), "stale_preview");
  EXPECT_EQ(field(commit(preview()), "revision"), "2");
  EXPECT_EQ(errorCode(page("1")), "stale_revision");
}

TEST_F(WebSDK, MapInspectionKeepsPrivateEvidenceAndNeverLoadsReferences) {
  write(R"({"version":3,"file":"SECRET_MAP_CANARY.js",
    "sourceRoot":"https://SECRET_MAP_CANARY.invalid/?token=SECRET_MAP_CANARY",
    "sources":["../SECRET_MAP_CANARY.js",null,"file:///SECRET_MAP_CANARY"],
    "sourcesContent":["const SECRET_MAP_CANARY = 1;",null],
    "names":["SECRET_MAP_CANARY"],"mappings":"AAAAA"})");
  commit(preview());
  const auto Summary = map("1", rootID("1"));
  ASSERT_EQ(field(Summary, "status"), "ok");
  EXPECT_EQ(field(Summary, "association_status"), "unbound");
  EXPECT_EQ(field(Summary, "coordinate_validation"), "not_checked");
  const auto MapID = field(Summary, "map_id");
  const auto Sources = mapSources("1", MapID);
  const auto *Items = Sources.getAsObject()->getArray("items");
  ASSERT_NE(Items, nullptr);
  ASSERT_EQ(Items->size(), 3);
  EXPECT_EQ(field((*Items)[0], "content_status"), "embedded");
  EXPECT_EQ(field((*Items)[1], "content_status"), "not_supplied");
  EXPECT_EQ(field((*Items)[2], "origin_kind"), "source_map_reference");
  EXPECT_NE(field((*Items)[1], "mapped_source_id"),
            field((*Items)[2], "mapped_source_id"));
  const auto Segments = take(neverd_web_source_map_segments_json(
      Session, "1", 1, MapID.data(), MapID.size(), 0, 512));
  for (const auto *Result : {&Summary, &Sources, &Segments}) {
    std::string Serialized;
    llvm::raw_string_ostream(Serialized) << *Result;
    EXPECT_EQ(Serialized.find("SECRET_MAP_CANARY"), std::string::npos);
    EXPECT_EQ(Serialized.find(Path), std::string::npos);
  }
  EXPECT_EQ(errorCode(mapSources("1", MapID, UINT64_MAX)), "invalid_page");
  EXPECT_EQ(errorCode(mapSources("1", MapID, 0, 513)), "invalid_page");
  for (unsigned I = 0; I != 20; ++I)
    EXPECT_EQ(field(map("1", rootID("1")), "map_id"), MapID);
  EXPECT_EQ(take(neverd_web_metadata_json(Session))
                .getAsObject()
                ->getInteger("source_map_count"),
            1);
  write(R"({"version":3,"sources":[],"mappings":""})");
  commit(preview());
  EXPECT_EQ(errorCode(mapSources("1", MapID)), "stale_revision");
  EXPECT_EQ(errorCode(mapSources("2", MapID)), "unknown_source_map");
}

#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebSDK, CliLargeInventoryUsesOnlyCppAndNeverEmitsOriginalBytes) {
  write("SECRET_DISK_INPUT");
  constexpr uint64_t Size = 65ULL * 1024 * 1024 + 1;
  fs::resize_file(Path, Size);
  const auto Output = (Root / "stdout").string();
  const auto Errors = (Root / "stderr").string();
  const llvm::StringRef Args[] = {NEVERD_WEB_TEST_CLI, "web", "inspect", Path};
  const llvm::StringRef Environment[] = {"PATH=/neverd-no-external-tools",
                                         "NEVERD_SIGNATURE_CACHE=off"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Errors};
  std::string LaunchError;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Environment,
                                      Redirects, 30, 0, &LaunchError),
            0)
      << LaunchError;
  std::ifstream Stdout(Output), Stderr(Errors);
  const std::string Out{std::istreambuf_iterator<char>(Stdout), {}};
  const std::string Err{std::istreambuf_iterator<char>(Stderr), {}};
  EXPECT_EQ(Out.find("SECRET"), std::string::npos);
  EXPECT_TRUE(Err.empty()) << Err;
  std::istringstream Lines(Out);
  std::string Line;
  bool InventorySeen = false;
  while (std::getline(Lines, Line)) {
    auto Parsed = llvm::json::parse(Line);
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(field(*Parsed, "status"), "ok");
    if (const auto *Items = Parsed->getAsObject()->getArray("items")) {
      ASSERT_EQ(Items->size(), 1);
      EXPECT_EQ(field((*Items)[0], "size"), std::to_string(Size));
      EXPECT_EQ(field((*Items)[0], "kind"), "opaque");
      InventorySeen = true;
    }
  }
  EXPECT_TRUE(InventorySeen);
}

TEST_F(WebSDK, CliMapsStayOfflineAndEmitOnlyMetadata) {
  write(R"({"version":3,"sources":["https://SECRET_MAP_CLI.invalid/token"],
    "sourcesContent":["require('SECRET_MAP_CLI'); while(true) {}"],"mappings":"AAAA"})");
  const auto Output = (Root / "stdout").string();
  const auto Errors = (Root / "stderr").string();
  const llvm::StringRef Args[] = {NEVERD_WEB_TEST_CLI, "web", "map", Path};
  const llvm::StringRef Environment[] = {"PATH=/neverd-no-external-tools",
                                         "NEVERD_SIGNATURE_CACHE=off"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Errors};
  std::string LaunchError;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Environment,
                                      Redirects, 20, 0, &LaunchError),
            0)
      << LaunchError;
  std::ifstream Stdout(Output), Stderr(Errors);
  const std::string Out{std::istreambuf_iterator<char>(Stdout), {}};
  const std::string Err{std::istreambuf_iterator<char>(Stderr), {}};
  EXPECT_EQ(Out.find("SECRET_MAP_CLI"), std::string::npos);
  EXPECT_TRUE(Err.empty()) << Err;
  std::istringstream Lines(Out);
  std::string Line;
  unsigned Count = 0;
  while (std::getline(Lines, Line)) {
    auto Parsed = llvm::json::parse(Line);
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(field(*Parsed, "status"), "ok");
    ++Count;
  }
  EXPECT_EQ(Count, 4);
}
#endif

#if NEVERD_TEST_WEB_JAVASCRIPT
TEST_F(WebSDK, BundlePartitionsArePrivateBoundedCachedAndRevisionScoped) {
  writeBundle();
  commit(preview());
  const auto Source = field(analyze("1", rootID("1")), "source_id");
  EXPECT_EQ(errorCode(bundleRecords("1", Source, "modules")),
            "source_bundles_not_analyzed");
  const auto Summary = bundles("1", Source);
  EXPECT_EQ(field(Summary, "bundle_status"), "ok");
  EXPECT_EQ(Summary.getAsObject()->getInteger("module_count"), 2);
  EXPECT_EQ(Summary.getAsObject()->getBoolean("producer_verified"), false);
  EXPECT_EQ(Summary.getAsObject()->getBoolean("runtime_targets_verified"),
            false);
  EXPECT_EQ(field(bundles("1", Source), "bundle_analysis_id"),
            field(Summary, "bundle_analysis_id"));
  for (const auto Kind : {"bundles", "modules", "dependencies", "regions"}) {
    const auto Page = bundleRecords("1", Source, Kind);
    EXPECT_EQ(llvm::formatv("{0}", Page).str().find("SECRET"),
              std::string::npos);
    EXPECT_EQ(Page.getAsObject()->getBoolean("page_complete"), true);
  }
  const auto First = bundleRecords("1", Source, "modules", 0, 1);
  EXPECT_EQ(First.getAsObject()->getBoolean("page_complete"), false);
  EXPECT_EQ(First.getAsObject()->getInteger("next_offset"), 1);
  const auto &M = *(*First.getAsObject()->getArray("items"))[0].getAsObject();
  ASSERT_TRUE(bool(M.getString("body_sha256")));
  EXPECT_EQ(M.getString("body_sha256")->size(), 64);
  EXPECT_EQ(M.getBoolean("module_key_redacted"), true);
  const auto Edges = bundleRecords("1", Source, "dependencies");
  ASSERT_EQ(Edges.getAsObject()->getArray("items")->size(), 3);
  EXPECT_EQ(
      (*Edges.getAsObject()->getArray("items"))[1].getAsObject()->getString(
          "target_module_id"),
      M.getString("record_id"));
  EXPECT_EQ(errorCode(bundleRecords("1", Source, "modules", 0, 0)),
            "invalid_page");
  EXPECT_EQ(errorCode(bundleRecords("1", Source, "modules", 3, 1)),
            "invalid_page");
  EXPECT_EQ(errorCode(bundleRecords("1", Source, "modules", 0, 513)),
            "invalid_page");
  const auto Invalid = bundleRecords("1", Source, "SECRET_KIND");
  EXPECT_EQ(errorCode(Invalid), "invalid_bundle_record_kind");
  EXPECT_EQ(llvm::formatv("{0}", Invalid).str().find("SECRET"),
            std::string::npos);
  write("const changed = 1;");
  EXPECT_EQ(field(bundles("1", Source), "bundle_analysis_id"),
            field(Summary, "bundle_analysis_id"));
  commit(preview());
  EXPECT_EQ(errorCode(bundles("1", Source)), "stale_revision");
  EXPECT_EQ(errorCode(bundles("2", Source)), "unknown_source");
  EXPECT_EQ(take(neverd_web_metadata_json(Session))
                .getAsObject()
                ->getInteger("source_bundle_analysis_count"),
            0);
}

TEST_F(WebSDK, ModuleEvidenceLinksOnlyAdmittedFilesAndKeepsNamesPrivate) {
  write("import SECRET_MODULE from './SECRET_DEP.js' assert { secret: "
        "'SECRET_ASSERT' }; "
        "export { SECRET_MODULE as SECRET_EXPORT }; "
        "import('./DYNAMIC_SECRET.js');");
  {
    std::ofstream Out(Root / "SECRET_DEP.js");
    Out << "export default 1;";
  }
  const auto Directory = Root.string();
  const auto Token =
      field(take(neverd_web_import_preview_json(Session, Directory.data(),
                                                Directory.size(), nullptr, 0)),
            "preview_token");
  commit(Token);
  const auto Inventory = page("1", 0, 512);
  const auto &Items = *Inventory.getAsObject()->getArray("items");
  ASSERT_EQ(Items.size(), 3);
  // Snapshot members are sorted: directory, SECRET_DEP.js, input.js.
  const auto Target = field(Items[1], "artifact_id");
  const auto Source = field(
      analyze("1", field(Items[2], "artifact_id"), "module"), "source_id");
  EXPECT_EQ(errorCode(moduleRecords("1", Source, "requests")),
            "source_modules_not_analyzed");
  const auto Summary = modules("1", Source);
  EXPECT_EQ(field(Summary, "module_status"), "ok");
  EXPECT_EQ(field(Summary, "link_status"), "ok");
  EXPECT_EQ(Summary.getAsObject()->getBoolean("resolves_runtime_modules"),
            false);
  EXPECT_EQ(field(Summary, "module_analysis_id"),
            field(modules("1", Source), "module_analysis_id"));
  for (const auto Kind : {"requests", "imports", "exports", "attributes"}) {
    const auto Records = moduleRecords("1", Source, Kind);
    EXPECT_EQ(llvm::formatv("{0}", Records).str().find("SECRET"),
              std::string::npos);
    EXPECT_EQ(Records.getAsObject()->getBoolean("page_complete"), true);
  }
  const auto Records = moduleRecords("1", Source, "requests", 0, 1);
  const auto &First =
      *(*Records.getAsObject()->getArray("items"))[0].getAsObject();
  EXPECT_EQ(First.getString("candidate_artifact_id"), Target);
  EXPECT_EQ(First.getString("link_status"), "exact_admitted_file_candidate");
  EXPECT_EQ(First.getBoolean("runtime_target_verified"), false);
  EXPECT_EQ(Records.getAsObject()->getBoolean("page_complete"), false);
  const auto Second = moduleRecords("1", Source, "requests", 1, 1);
  EXPECT_EQ(
      (*Second.getAsObject()->getArray("items"))[0].getAsObject()->getString(
          "link_status"),
      "dynamic_import_boundary");
  EXPECT_EQ(errorCode(moduleRecords("1", Source, "requests", 0, 0)),
            "invalid_page");
  EXPECT_EQ(errorCode(moduleRecords("1", Source, "requests", 0, 513)),
            "invalid_page");
  EXPECT_EQ(errorCode(moduleRecords("1", Source, "requests", 3, 1)),
            "invalid_page");
  const auto InvalidKind = moduleRecords("1", Source, "SECRET_KIND");
  EXPECT_EQ(errorCode(InvalidKind), "invalid_module_record_kind");
  EXPECT_EQ(llvm::formatv("{0}", InvalidKind).str().find("SECRET"),
            std::string::npos);
  // Mutation of admitted files after publication cannot change candidate links.
  fs::remove(Root / "SECRET_DEP.js");
  EXPECT_EQ(field(modules("1", Source), "link_analysis_id"),
            field(Summary, "link_analysis_id"));
  commit(preview());
  EXPECT_EQ(errorCode(modules("1", Source)), "stale_revision");
  EXPECT_EQ(errorCode(modules("2", Source)), "unknown_source");
  const auto Metadata = take(neverd_web_metadata_json(Session));
  EXPECT_EQ(Metadata.getAsObject()->getInteger("source_module_analysis_count"),
            0);
}

TEST_F(WebSDK, BindingQueriesAreBoundedPrivateAndRevisionScoped) {
  const std::string Canary = "SECRET_BINDING_CANARY_91";
  write("const " + Canary + " = '" + Canary + "'; function f(){return " +
        Canary + ";}");
  commit(preview());
  const auto Artifact = rootID("1");
  const auto SourceID = field(analyze("1", Artifact), "source_id");
  EXPECT_EQ(errorCode(bindingRecords("1", SourceID, "references")),
            "source_bindings_not_analyzed");
  const auto Summary = bindings("1", SourceID);
  ASSERT_EQ(field(Summary, "binding_status"), "ok");
  EXPECT_EQ(Summary.getAsObject()->getBoolean("semantic_validation_complete"),
            false);
  EXPECT_EQ(field(analyze("1", Artifact), "semantic_analysis"), "partial");
  EXPECT_EQ(field(nodes("1", SourceID), "semantic_analysis"), "partial");
  for (const auto Kind : {"scopes", "bindings", "declarations", "references"}) {
    const auto Page = bindingRecords("1", SourceID, Kind);
    ASSERT_EQ(field(Page, "status"), "ok");
    EXPECT_FALSE(Page.getAsObject()->getArray("items")->empty());
    std::string Serialized;
    llvm::raw_string_ostream(Serialized) << Page;
    EXPECT_EQ(Serialized.find(Canary), std::string::npos);
    EXPECT_EQ(Serialized.find(Path), std::string::npos);
  }
  const auto Refs = bindingRecords("1", SourceID, "references");
  const auto *Items = Refs.getAsObject()->getArray("items");
  ASSERT_EQ(Items->size(), 1);
  EXPECT_EQ(field((*Items)[0], "resolution"), "lexical_binding");
  const auto First = bindingRecords("1", SourceID, "bindings", 0, 1);
  EXPECT_EQ(First.getAsObject()->getBoolean("page_complete"), false);
  EXPECT_EQ(errorCode(bindingRecords("1", SourceID, "references", UINT64_MAX)),
            "invalid_page");
  EXPECT_EQ(errorCode(bindingRecords("1", SourceID, "references", 0, 513)),
            "invalid_page");
  EXPECT_EQ(errorCode(bindingRecords("1", SourceID, "source_text")),
            "unsupported_binding_record_kind");
  EXPECT_EQ(field(bindings("1", SourceID), "binding_analysis_id"),
            field(Summary, "binding_analysis_id"));
  EXPECT_EQ(take(neverd_web_metadata_json(Session))
                .getAsObject()
                ->getInteger("source_binding_analysis_count"),
            1);
  write("const newer = 2;");
  commit(preview());
  EXPECT_EQ(errorCode(bindingRecords("1", SourceID, "references")),
            "stale_revision");
  EXPECT_EQ(errorCode(bindings("2", SourceID)), "unknown_source");
  EXPECT_EQ(take(neverd_web_metadata_json(Session))
                .getAsObject()
                ->getInteger("source_binding_analysis_count"),
            0);
}

TEST_F(WebSDK,
       MapLookupChecksBoundariesWithoutInventingAssociationOrInterpolation) {
  write(R"({"version":3,"sources":["generated.js","original.js","missing.js"],
    "sourcesContent":["\"😀\";\r\nx", "x;\n", null],
    "mappings":"ACAA,EAAA;ACAA"})");
  commit(preview());
  const auto Summary = map("1", rootID("1"));
  const auto MapID = field(Summary, "map_id");
  auto Sources = mapSources("1", MapID);
  const auto *Items = Sources.getAsObject()->getArray("items");
  ASSERT_NE(Items, nullptr);
  ASSERT_EQ(Items->size(), 3);
  const auto Generated = field((*Items)[0], "artifact_id");
  const auto Missing = field((*Items)[2], "mapped_source_id");
  EXPECT_EQ(errorCode(analyze("1", Missing)), "source_content_not_supplied");
  const auto Source = analyze("1", Generated);
  ASSERT_EQ(field(Source, "parse_status"), "parsed");
  const auto SourceID = field(Source, "source_id");
  const auto AtZero = mapLookup("1", MapID, SourceID, 0);
  EXPECT_EQ(field(AtZero, "association_kind"), "caller_assertion");
  EXPECT_EQ(AtZero.getAsObject()->getBoolean("provenance_verified"), false);
  const auto *Exact = AtZero.getAsObject()->getArray("items");
  ASSERT_NE(Exact, nullptr);
  ASSERT_EQ(Exact->size(), 1);
  EXPECT_EQ(field((*Exact)[0], "selection"), "exact_anchor");
  EXPECT_EQ(field((*Exact)[0], "original_byte"), "0");
  const auto AfterEmoji = mapLookup("1", MapID, SourceID, 5);
  const auto *Preceding = AfterEmoji.getAsObject()->getArray("items");
  ASSERT_NE(Preceding, nullptr);
  ASSERT_EQ(Preceding->size(), 1);
  EXPECT_EQ(field((*Preceding)[0], "selection"), "preceding_anchor");
  EXPECT_EQ(field((*Preceding)[0], "original_byte"), "0");
  EXPECT_EQ(field((*Preceding)[0], "generated_position_validation"),
            "out_of_bounds_or_non_boundary");
  const auto LineTwo = mapLookup("1", MapID, SourceID, 9);
  const auto *NoContent = LineTwo.getAsObject()->getArray("items");
  ASSERT_NE(NoContent, nullptr);
  ASSERT_EQ(NoContent->size(), 1);
  EXPECT_EQ(field((*NoContent)[0], "original_position_validation"),
            "content_not_supplied");
  EXPECT_EQ(errorCode(mapLookup("1", MapID, SourceID, 2)),
            "invalid_source_position");
  EXPECT_EQ(errorCode(mapLookup("1", MapID, SourceID, 8)),
            "invalid_source_position");
  EXPECT_EQ(errorCode(mapLookup("1", MapID, SourceID, UINT64_MAX)),
            "invalid_source_position");
}

#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebSDK, CliBundleRecoveryUsesOnlyCppAndKeepsOriginalEvidencePrivate) {
  writeBundle();
  const auto Output = (Root / "stdout").string();
  const auto Errors = (Root / "stderr").string();
  const llvm::StringRef Args[] = {NEVERD_WEB_TEST_CLI, "web", "bundles", Path,
                                  "script"};
  const llvm::StringRef Environment[] = {"PATH=/neverd-no-external-tools",
                                         "NEVERD_SIGNATURE_CACHE=off"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Errors};
  std::string LaunchError;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Environment,
                                      Redirects, 20, 0, &LaunchError),
            0)
      << LaunchError;
  std::ifstream Stdout(Output), Stderr(Errors);
  const std::string Out{std::istreambuf_iterator<char>(Stdout), {}};
  const std::string Err{std::istreambuf_iterator<char>(Stderr), {}};
  EXPECT_EQ(Out.find("SECRET"), std::string::npos);
  EXPECT_TRUE(Err.empty());
  std::istringstream Lines(Out);
  std::string Line;
  unsigned Pages = 0;
  while (std::getline(Lines, Line)) {
    auto Parsed = llvm::json::parse(Line);
    ASSERT_TRUE(bool(Parsed));
    const auto *O = Parsed->getAsObject();
    ASSERT_NE(O, nullptr);
    if (O->getArray("items") && O->getString("bundle_status")) {
      ++Pages;
      EXPECT_EQ(O->getString("bundle_status"), "ok");
      EXPECT_EQ(O->getBoolean("producer_verified"), false);
    }
  }
  EXPECT_EQ(Pages, 4);
}

TEST_F(WebSDK, CliModuleAnalysisIsCppOnlyAndDoesNotEmitTargetNames) {
  write("import SECRET_DEFAULT from './SECRET_PACKAGE.js'; export { "
        "SECRET_DEFAULT }; "
        "import('./SECRET_DYNAMIC.js');");
  const auto Output = (Root / "stdout").string();
  const auto Errors = (Root / "stderr").string();
  const llvm::StringRef Args[] = {NEVERD_WEB_TEST_CLI, "web", "modules", Path,
                                  "module"};
  const llvm::StringRef Environment[] = {"PATH=/neverd-no-external-tools",
                                         "NEVERD_SIGNATURE_CACHE=off"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Errors};
  std::string LaunchError;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Environment,
                                      Redirects, 20, 0, &LaunchError),
            0)
      << LaunchError;
  std::ifstream Stdout(Output), Stderr(Errors);
  const std::string Out{std::istreambuf_iterator<char>(Stdout), {}};
  const std::string Err{std::istreambuf_iterator<char>(Stderr), {}};
  EXPECT_EQ(Out.find("SECRET"), std::string::npos);
  EXPECT_TRUE(Err.empty());
  unsigned Pages = 0;
  std::istringstream Lines(Out);
  std::string Line;
  while (std::getline(Lines, Line)) {
    auto Parsed = llvm::json::parse(Line);
    ASSERT_TRUE(bool(Parsed));
    const auto *O = Parsed->getAsObject();
    ASSERT_NE(O, nullptr);
    if (O->getArray("items") && O->getString("module_status")) {
      ++Pages;
      EXPECT_EQ(O->getBoolean("resolves_runtime_modules"), false);
      if (O->getString("record_kind") == "requests") {
        const auto &First = *(*O->getArray("items"))[0].getAsObject();
        EXPECT_EQ(First.getString("link_status"),
                  "directory_origin_unavailable");
      }
    }
  }
  EXPECT_EQ(Pages, 4);
}

TEST_F(WebSDK, CliSemanticAnalysisUsesTheCppBackendAndOmitsPrimitiveValues) {
  write("const SECRET_VALUE_CLI = 'SECRET' + '_VALUE'; "
        "function f() { return unknown(); } (1n / 0n);");
  const auto Output = (Root / "stdout").string();
  const auto Errors = (Root / "stderr").string();
  const llvm::StringRef Args[] = {NEVERD_WEB_TEST_CLI, "web", "semantics", Path,
                                  "script"};
  const llvm::StringRef Environment[] = {"PATH=/neverd-no-external-tools",
                                         "NEVERD_SIGNATURE_CACHE=off"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Errors};
  std::string LaunchError;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Environment,
                                      Redirects, 20, 0, &LaunchError),
            0)
      << LaunchError;
  std::ifstream Stdout(Output), Stderr(Errors);
  const std::string Out{std::istreambuf_iterator<char>(Stdout), {}};
  const std::string Err{std::istreambuf_iterator<char>(Stderr), {}};
  EXPECT_EQ(Out.find("SECRET"), std::string::npos);
  EXPECT_TRUE(Err.empty());
  bool SawSemanticPage = false;
  std::istringstream Lines(Out);
  std::string Line;
  while (std::getline(Lines, Line)) {
    auto Parsed = llvm::json::parse(Line);
    ASSERT_TRUE(bool(Parsed));
    const auto *O = Parsed->getAsObject();
    ASSERT_NE(O, nullptr);
    if (O->getArray("items") && O->getString("effect_status")) {
      SawSemanticPage = true;
      EXPECT_EQ(O->getBoolean("authorizes_source_rewrites"), false);
      EXPECT_EQ(O->getString("effect_status"), "ok");
    }
  }
  EXPECT_TRUE(SawSemanticPage);
}

TEST_F(WebSDK, CliBindingAnalysisUsesTheCppBackendAndOmitsNames) {
  write("const SECRET_BINDING_CLI = 1; function f() { return "
        "SECRET_BINDING_CLI; }");
  const auto Output = (Root / "stdout").string();
  const auto Errors = (Root / "stderr").string();
  const llvm::StringRef Args[] = {NEVERD_WEB_TEST_CLI, "web", "bindings", Path,
                                  "script"};
  const llvm::StringRef Environment[] = {"PATH=/neverd-no-external-tools",
                                         "NEVERD_SIGNATURE_CACHE=off"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Errors};
  std::string LaunchError;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Environment,
                                      Redirects, 20, 0, &LaunchError),
            0)
      << LaunchError;
  std::ifstream Stdout(Output), Stderr(Errors);
  const std::string Out{std::istreambuf_iterator<char>(Stdout), {}};
  const std::string Err{std::istreambuf_iterator<char>(Stderr), {}};
  EXPECT_EQ(Out.find("SECRET_BINDING_CLI"), std::string::npos);
  EXPECT_TRUE(Err.empty());
  unsigned Pages = 0;
  std::istringstream Lines(Out);
  std::string Line;
  while (std::getline(Lines, Line)) {
    auto Parsed = llvm::json::parse(Line);
    ASSERT_TRUE(bool(Parsed));
    const auto *O = Parsed->getAsObject();
    ASSERT_NE(O, nullptr);
    if (O->getString("record_kind")) {
      ++Pages;
      EXPECT_EQ(O->getString("binding_status"), "ok");
      EXPECT_EQ(O->getBoolean("semantic_validation_complete"), false);
    }
  }
  EXPECT_EQ(Pages, 4);
}

TEST_F(WebSDK, CliParsesOfflineWithoutAPathToExternalTools) {
  const std::string Canary = "SECRET_CLI_CANARY_239";
  write("// " + Canary + "\nwhile (true) {}\nrequire('" + Canary + "');");
  const auto Output = (Root / "stdout").string();
  const auto Errors = (Root / "stderr").string();
  const llvm::StringRef Args[] = {NEVERD_WEB_TEST_CLI, "web", "source", Path,
                                  "commonjs"};
  const llvm::StringRef Environment[] = {"PATH=/neverd-no-external-tools",
                                         "NEVERD_SIGNATURE_CACHE=off"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Errors};
  std::string LaunchError;
  const auto Status = llvm::sys::ExecuteAndWait(
      NEVERD_WEB_TEST_CLI, Args, Environment, Redirects, 20, 0, &LaunchError);
  ASSERT_EQ(Status, 0) << LaunchError;
  std::ifstream Stdout(Output), Stderr(Errors);
  const std::string Out{std::istreambuf_iterator<char>(Stdout), {}};
  const std::string Err{std::istreambuf_iterator<char>(Stderr), {}};
  EXPECT_EQ(Out.find(Canary), std::string::npos);
  EXPECT_EQ(Err.find(Canary), std::string::npos);
  EXPECT_TRUE(Err.empty()) << Err;
  bool SummarySeen = false, NodesSeen = false;
  std::istringstream Lines(Out);
  std::string Line;
  while (std::getline(Lines, Line)) {
    auto Parsed = llvm::json::parse(Line);
    ASSERT_TRUE(bool(Parsed));
    ASSERT_NE(Parsed->getAsObject(), nullptr);
    if (Parsed->getAsObject()->getInteger("node_count")) {
      SummarySeen = true;
      EXPECT_EQ(field(*Parsed, "parse_status"), "parsed");
    }
    if (const auto *Items = Parsed->getAsObject()->getArray("items")) {
      NodesSeen = true;
      EXPECT_FALSE(Items->empty());
      EXPECT_EQ(field(*Parsed, "semantic_analysis"), "not_analyzed");
    }
  }
  EXPECT_TRUE(SummarySeen);
  EXPECT_TRUE(NodesSeen);
}
#endif

TEST_F(WebSDK, SemanticPagesAreBoundedPrivateCachedAndRevisionScoped) {
  write("const SECRET_VALUE = 'SECRET' + '_VALUE'; "
        "function f(x = deferred()) {return x;} false && skipped();");
  commit(preview());
  const auto Source = analyze("1", rootID("1"));
  const auto ID = field(Source, "source_id");
  EXPECT_EQ(errorCode(semanticRecords("1", ID)),
            "source_semantics_not_analyzed");
  const auto Summary = semantics("1", ID);
  EXPECT_EQ(field(Summary, "value_status"), "ok");
  EXPECT_EQ(field(Summary, "effect_status"), "ok");
  EXPECT_EQ(Summary.getAsObject()->getBoolean("authorizes_source_rewrites"),
            false);
  EXPECT_EQ(field(semantics("1", ID), "value_analysis_id"),
            field(Summary, "value_analysis_id"));
  auto Meta = take(neverd_web_metadata_json(Session));
  EXPECT_EQ(Meta.getAsObject()->getInteger("source_semantic_analysis_count"),
            1);
  EXPECT_EQ(Meta.getAsObject()->getInteger("source_binding_analysis_count"), 1);
  const auto First = semanticRecords("1", ID, 0, 1);
  EXPECT_EQ(First.getAsObject()->getBoolean("page_complete"), false);
  const auto All = semanticRecords("1", ID);
  const auto *Items = All.getAsObject()->getArray("items");
  ASSERT_NE(Items, nullptr);
  EXPECT_EQ(Items->size(), *Source.getAsObject()->getInteger("node_count"));
  bool SawString = false, SawFunction = false;
  for (const auto &Item : *Items) {
    const auto *O = Item.getAsObject();
    ASSERT_NE(O, nullptr);
    EXPECT_EQ(O->getBoolean("value_redacted"), true);
    EXPECT_EQ(O->get("value"), nullptr);
    if (O->getString("value_kind") == "string")
      SawString = true;
    if (O->getString("kind") == "FunctionDeclaration") {
      SawFunction = true;
      ASSERT_NE(O->getArray("deferred_effects"), nullptr);
      EXPECT_FALSE(O->getArray("deferred_effects")->empty());
    }
  }
  EXPECT_TRUE(SawString);
  EXPECT_TRUE(SawFunction);
  for (const auto *R : {&Summary, &All}) {
    std::string Serialized;
    llvm::raw_string_ostream(Serialized) << *R;
    EXPECT_EQ(Serialized.find("SECRET"), std::string::npos);
    EXPECT_EQ(Serialized.find(Path), std::string::npos);
  }
  EXPECT_EQ(errorCode(semanticRecords("1", ID, 0, 0)), "invalid_page");
  EXPECT_EQ(errorCode(semanticRecords("1", ID, 0, 513)), "invalid_page");
  EXPECT_EQ(errorCode(semanticRecords("1", ID, UINT64_MAX, 1)), "invalid_page");
  commit(preview());
  EXPECT_EQ(errorCode(semanticRecords("1", ID)), "stale_revision");
  EXPECT_EQ(errorCode(semanticRecords("2", ID)), "unknown_source");
  EXPECT_EQ(errorCode(semantics("2", ID)), "unknown_source");
  Meta = take(neverd_web_metadata_json(Session));
  EXPECT_EQ(Meta.getAsObject()->getInteger("source_semantic_analysis_count"),
            0);
}

TEST_F(WebSDK, UnavailableEffectsAreNullRatherThanEmptyEffectProofs) {
  // Exhaust lexical-resolution work while retaining the independently bounded
  // value inventory. Missing effect records must not mean no effects.
  std::string Text(40, '{');
  for (unsigned I = 0; I < 40000; ++I)
    Text += "absent;";
  Text += std::string(40, '}') + "1 + 2;";
  write(Text);
  commit(preview());
  const auto Source = analyze("1", rootID("1"));
  ASSERT_EQ(field(Source, "parse_status"), "parsed");
  const auto ID = field(Source, "source_id");
  const auto Summary = semantics("1", ID);
  EXPECT_EQ(field(Summary, "effect_status"), "unavailable");
  const auto Page = semanticRecords("1", ID);
  const auto *Items = Page.getAsObject()->getArray("items");
  ASSERT_NE(Items, nullptr);
  ASSERT_FALSE(Items->empty());
  for (const auto &Item : *Items) {
    EXPECT_EQ(Item.getAsObject()->get("immediate_effects")->kind(),
              Value::Null);
    EXPECT_EQ(Item.getAsObject()->get("contains_declaration")->kind(),
              Value::Null);
  }
}

TEST_F(WebSDK, NavigationIsPagedPrivateAndDoesNotAssertRuntimeTargets) {
  write("function SECRET_FN(SECRET_ARG) { return SECRET_ARG; } "
        "SECRET_FN('SECRET_VALUE');");
  commit(preview());
  const auto Source = field(analyze("1", rootID("1")), "source_id");
  EXPECT_EQ(errorCode(navigationRecords("1", Source, "functions")),
            "source_navigation_not_analyzed");
  const auto Summary = navigation("1", Source);
  EXPECT_EQ(field(Summary, "navigation_status"), "ok");
  EXPECT_EQ(field(Summary, "runtime_call_graph"), "not_analyzed");
  EXPECT_EQ(Summary.getAsObject()->getInteger("function_count"), 1);
  EXPECT_EQ(Summary.getAsObject()->getInteger("call_count"), 1);
  const auto Functions = navigationRecords("1", Source, "functions");
  const auto Calls = navigationRecords("1", Source, "calls");
  const auto Refs = navigationRecords("1", Source, "references", 0, 1);
  ASSERT_NE(Functions.getAsObject()->getArray("items"), nullptr);
  ASSERT_EQ(Functions.getAsObject()->getArray("items")->size(), 1);
  ASSERT_NE(Calls.getAsObject()->getArray("items"), nullptr);
  ASSERT_EQ(Calls.getAsObject()->getArray("items")->size(), 1);
  const auto &F = (*Functions.getAsObject()->getArray("items"))[0];
  const auto &C = (*Calls.getAsObject()->getArray("items"))[0];
  EXPECT_EQ(field(C, "callee_binding_id"), field(F, "name_binding_id"));
  EXPECT_EQ(field(C, "runtime_target"), "not_proven");
  EXPECT_EQ(Refs.getAsObject()->getBoolean("page_complete"), false);
  EXPECT_EQ(errorCode(navigationRecords("1", Source, "targets")),
            "unsupported_navigation_record_kind");
  EXPECT_EQ(errorCode(navigationRecords("1", Source, "calls", 0, 513)),
            "invalid_page");
  EXPECT_EQ(field(navigation("1", Source), "navigation_id"),
            field(Summary, "navigation_id"));
  for (const auto *R : {&Summary, &Functions, &Calls, &Refs})
    EXPECT_EQ(llvm::formatv("{0}", *R).str().find("SECRET"), std::string::npos);
  commit(preview());
  EXPECT_EQ(errorCode(navigation("1", Source)), "stale_revision");
  EXPECT_EQ(errorCode(navigation("2", Source)), "unknown_source");
}

TEST_F(WebSDK,
       SourceAnchorsDistinguishOriginalBytesAndRedactedDisplayCoverage) {
  const std::string Text = "const SECRET = 'PRIVATE😀';\r\n";
  write(Text);
  commit(preview());
  const auto Source = field(analyze("1", rootID("1")), "source_id");
  const auto At = Text.find("😀");
  auto P = viewPreview("1", Source);
  const auto View = field(P, "view_id");
  EXPECT_EQ(errorCode(anchor("1", Source, At, 4, View)),
            "source_view_not_committed");
  viewCommit("1", field(P, "preview_token"));
  write("const = ;");
  const auto Location = anchor("1", Source, At, 4, View);
  ASSERT_EQ(field(Location, "status"), "ok");
  const auto *O = Location.getAsObject(), *Storage = O->getObject("storage"),
             *Display = O->getObject("view");
  ASSERT_NE(Storage, nullptr);
  ASSERT_NE(Display, nullptr);
  EXPECT_EQ(Storage->getString("mapping"), "byte_identity");
  EXPECT_EQ(Storage->getString("byte_offset"), std::to_string(At));
  EXPECT_EQ(O->getObject("start")->getInteger("utf16_column"), At);
  EXPECT_EQ(O->getObject("end")->getInteger("utf16_column"), At + 2);
  EXPECT_EQ(Display->getString("mapping"), "region_cover");
  const auto Start = std::stoull(Display->getString("byte_offset")->str());
  const auto Size = std::stoull(Display->getString("byte_length")->str());
  EXPECT_EQ(field(viewChunk("1", View, Start, Size), "text"), "[string]");
  EXPECT_EQ(llvm::formatv("{0}", Location).str().find("PRIVATE"),
            std::string::npos);
  EXPECT_EQ(llvm::formatv("{0}", Location).str().find("SECRET"),
            std::string::npos);
  EXPECT_EQ(errorCode(anchor("1", Source, At + 1, 0)),
            "invalid_source_position");
  EXPECT_EQ(errorCode(anchor("1", Source, At, 3)), "invalid_source_position");
  EXPECT_EQ(errorCode(anchor("1", Source, Text.find('\n'), 0)),
            "invalid_source_position");
  EXPECT_EQ(errorCode(anchor("1", Source, UINT64_MAX, 1)),
            "invalid_source_position");
  const auto Point = anchor("1", Source, At, 0, View);
  EXPECT_EQ(Point.getAsObject()->getObject("view")->getString("mapping"),
            "region_cover");
  const auto EOFPoint = anchor("1", Source, Text.size(), 0, View);
  EXPECT_EQ(EOFPoint.getAsObject()->getObject("view")->getString("mapping"),
            "exact_boundary");
  const auto Clear =
      "{\"schema_version\":1,\"locally_reviewed\":true,\"reviewed_ranges\":[{"
      "\"byte_offset\":\"0\",\"byte_length\":\"" +
      std::to_string(Text.size()) + "\"}]}";
  P = viewPreview("1", Source, Clear);
  viewCommit("1", field(P, "preview_token"));
  EXPECT_EQ(errorCode(anchor("1", Source, At, 4, View)),
            "source_view_not_committed");
  const auto Reviewed = anchor("1", Source, At, 4, field(P, "view_id"));
  EXPECT_EQ(Reviewed.getAsObject()->getObject("view")->getString("mapping"),
            "byte_identity");
  EXPECT_EQ(Reviewed.getAsObject()->getObject("view")->getString("byte_offset"),
            std::to_string(At));
  commit(preview());
  EXPECT_EQ(errorCode(anchor("1", Source, 0, 0)), "stale_revision");
  EXPECT_EQ(errorCode(anchor("2", Source, 0, 0)), "unknown_source");
}

TEST_F(WebSDK, BunAnchorsUseEncodedMemberRangesAndWholeCompressedFrames) {
  using namespace neverd::web::test;
  const BunFixture F(true, simpleBunMap());
  write(F.Bytes);
  commit(preview());
  const auto Extraction = field(bun("1", rootID("1")), "extraction_id");
  const auto Modules = bunRecords("1", Extraction, "modules");
  ASSERT_NE(Modules.getAsObject()->getArray("items"), nullptr);
  ASSERT_GE(Modules.getAsObject()->getArray("items")->size(), 2);
  const auto &First = (*Modules.getAsObject()->getArray("items"))[0];
  const auto &Unicode = (*Modules.getAsObject()->getArray("items"))[1];
  const auto Source =
      field(analyze("1", field(Unicode, "source_artifact_id"), "module"),
            "source_id");
  const std::string Text = "export const x = '中文🌱';";
  const auto At = Text.find("中");
  const auto Base = 4104 + get(F.Graph, F.Table + 52 + 8, 4);
  const auto Location = anchor("1", Source, At, 3);
  ASSERT_EQ(field(Location, "status"), "ok");
  const auto *Storage = Location.getAsObject()->getObject("storage");
  ASSERT_NE(Storage, nullptr);
  EXPECT_EQ(Storage->getString("stored_encoding"), "utf16le");
  EXPECT_EQ(Storage->getString("mapping"), "unicode_boundary_conversion");
  EXPECT_EQ(Storage->getString("byte_offset"), std::to_string(Base + 2 * At));
  EXPECT_EQ(Storage->getString("byte_length"), "2");
  const auto Map = map("1", field(First, "source_map_region_id"));
  if (field(Map, "status") == "error") {
    EXPECT_EQ(errorCode(Map), "bun_source_map_zstd_unavailable");
    return;
  }
  const auto Sources = mapSources("1", field(Map, "map_id"));
  ASSERT_NE(Sources.getAsObject()->getArray("items"), nullptr);
  ASSERT_FALSE(Sources.getAsObject()->getArray("items")->empty());
  const auto &Original = (*Sources.getAsObject()->getArray("items"))[0];
  const auto OriginalSource =
      analyze("1", field(Original, "artifact_id"), "module");
  ASSERT_EQ(field(OriginalSource, "parse_status"), "parsed");
  const auto OriginalID = field(OriginalSource, "source_id");
  const auto A = anchor("1", OriginalID, 0, 1),
             B = anchor("1", OriginalID, 1, 1);
  const auto *AS = A.getAsObject()->getObject("storage"),
             *BS = B.getAsObject()->getObject("storage");
  ASSERT_NE(AS, nullptr);
  ASSERT_NE(BS, nullptr);
  const auto *Expected = Original.getAsObject()->getObject("storage");
  EXPECT_EQ(AS->getString("mapping"), "containing_compressed_frame");
  EXPECT_EQ(AS->getString("byte_offset"),
            Expected->getString("content_offset"));
  EXPECT_EQ(AS->getString("byte_length"), Expected->getString("content_size"));
  EXPECT_EQ(AS->getString("byte_offset"), BS->getString("byte_offset"));
  EXPECT_EQ(AS->getString("byte_length"), BS->getString("byte_length"));
}

TEST_F(WebSDK, EmbeddedJsonSourceAnchorsDoNotInventRawBytePositions) {
  write(
      R"({"version":3,"sources":["SECRET_PATH"],"sourcesContent":["const SECRET = '\u4e2d';"],"names":[],"mappings":"AAAA"})");
  commit(preview());
  const auto Map = map("1", rootID("1"));
  const auto Sources = mapSources("1", field(Map, "map_id"));
  ASSERT_NE(Sources.getAsObject()->getArray("items"), nullptr);
  ASSERT_FALSE(Sources.getAsObject()->getArray("items")->empty());
  const auto ID =
      field((*Sources.getAsObject()->getArray("items"))[0], "artifact_id");
  const auto Source = field(analyze("1", ID), "source_id");
  const auto A = anchor("1", Source, 0, 5);
  ASSERT_EQ(field(A, "status"), "ok");
  const auto *Storage = A.getAsObject()->getObject("storage");
  ASSERT_NE(Storage, nullptr);
  EXPECT_EQ(Storage->getString("mapping"), "encoded_member_not_located");
  EXPECT_EQ(Storage->getString("map_artifact_id"), rootID("1"));
  EXPECT_EQ(Storage->get("byte_offset")->kind(), llvm::json::Value::Null);
  EXPECT_EQ(llvm::formatv("{0}", A).str().find("SECRET"), std::string::npos);
}

#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebSDK, CliNavigationAndAnchorsStayCppOnlyAndPrivate) {
  const std::string Text = "function SECRET_FN(SECRET_ARG) { return "
                           "SECRET_ARG; } SECRET_FN('SECRET_VALUE');";
  write(Text);
  const auto Output = (Root / "stdout").string(),
             Errors = (Root / "stderr").string();
  const llvm::StringRef Environment[] = {"PATH=/neverd-no-external-tools",
                                         "NEVERD_SIGNATURE_CACHE=off"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Errors};
  const auto Offset = std::to_string(Text.find("SECRET_ARG"));
  for (const auto Command : {"navigate", "anchor"}) {
    std::vector<llvm::StringRef> Args{NEVERD_WEB_TEST_CLI, "web", Command, Path,
                                      "script"};
    if (std::string_view(Command) == "anchor") {
      Args.push_back(Offset);
      Args.push_back("10");
    }
    std::string LaunchError;
    ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Environment,
                                        Redirects, 20, 0, &LaunchError),
              0)
        << LaunchError;
    std::ifstream Stdout(Output), Stderr(Errors);
    const std::string Out{std::istreambuf_iterator<char>(Stdout), {}},
        Err{std::istreambuf_iterator<char>(Stderr), {}};
    EXPECT_EQ(Out.find("SECRET"), std::string::npos);
    EXPECT_TRUE(Err.empty());
    if (std::string_view(Command) == "navigate")
      EXPECT_NE(Out.find("not_proven"), std::string::npos);
    else {
      EXPECT_NE(Out.find("source-storage-anchor-v1"), std::string::npos);
      EXPECT_NE(Out.find("binding_"), std::string::npos);
    }
  }
}

TEST_F(WebSDK, CliBunNavigationViewsAndAnchorsUseTheSharedCppPipeline) {
  using namespace neverd::web::test;
  BunFixture F;
  const auto Start = 4104 + get(F.Graph, F.Table + 8, 4);
  const auto Size = get(F.Graph, F.Table + 12, 4);
  std::string Code = "function SECRET_FN(){}SECRET_FN();";
  ASSERT_LE(Code.size(), Size);
  Code.resize(Size, ' ');
  F.Bytes.replace(Start, Size, Code);
  write(F.Bytes);
  const auto Output = (Root / "stdout").string(),
             Errors = (Root / "stderr").string();
  const llvm::StringRef Environment[] = {"PATH=/neverd-no-external-tools",
                                         "NEVERD_SIGNATURE_CACHE=off"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Errors};
  const auto CJK = std::to_string(std::string("export const x = '").size());
  for (const auto Command :
       {"bun-navigate", "bun-view", "bun-anchor", "bun-electron"}) {
    const bool Anchor = std::string_view(Command) == "bun-anchor";
    std::vector<llvm::StringRef> Args{
        NEVERD_WEB_TEST_CLI, "web",   Command, Path,
        Anchor ? "1" : "0",  "module"};
    if (Anchor) {
      Args.push_back(CJK);
      Args.push_back("3");
    }
    std::string LaunchError;
    ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Environment,
                                        Redirects, 20, 0, &LaunchError),
              0)
        << LaunchError;
    std::ifstream Stdout(Output), Stderr(Errors);
    const std::string Out{std::istreambuf_iterator<char>(Stdout), {}},
        Err{std::istreambuf_iterator<char>(Stderr), {}};
    EXPECT_EQ(Out.find("SECRET"), std::string::npos);
    EXPECT_EQ(Out.find("中文"), std::string::npos);
    EXPECT_TRUE(Err.empty());
    if (Anchor) {
      EXPECT_NE(Out.find("unicode_boundary_conversion"), std::string::npos);
      EXPECT_NE(Out.find("[string]"), std::string::npos);
    } else if (std::string_view(Command) == "bun-electron")
      EXPECT_NE(Out.find("electron_source_id"), std::string::npos);
    else if (std::string_view(Command) == "bun-navigate")
      EXPECT_NE(Out.find("not_proven"), std::string::npos);
    else
      EXPECT_NE(Out.find("binding_"), std::string::npos);
  }
}
#endif

TEST_F(WebSDK, SourceViewsRequirePreviewCommitAndRevokeReplacedPolicies) {
  const std::string Text = "const SECRET_VIEW_NAME = 'SECRET_VIEW_VALUE';\r\n";
  write(Text);
  commit(preview());
  const auto Source = field(analyze("1", rootID("1")), "source_id");
  write("const = ;"); // Only captured source bytes can enter the projection.
  const auto Preview = viewPreview("1", Source);
  ASSERT_EQ(field(Preview, "status"), "ok");
  EXPECT_FALSE(Preview.getAsObject()->get("text"));
  const auto View = field(Preview, "view_id");
  const auto Token = field(Preview, "preview_token");
  EXPECT_EQ(errorCode(viewChunk("1", View)), "source_view_not_committed");
  const auto Records = viewRecords("1", View, 0, 1);
  EXPECT_EQ(field(Records, "publication_status"), "preview");
  EXPECT_EQ(Records.getAsObject()->getBoolean("page_complete"), false);
  EXPECT_EQ(errorCode(viewRecords("1", View, 0, 513)), "invalid_page");
  EXPECT_EQ(errorCode(viewCommit("0", Token)), "stale_revision");
  EXPECT_EQ(field(viewCommit("1", Token), "publication_status"), "committed");
  EXPECT_EQ(errorCode(viewCommit("1", Token)), "invalid_source_view_preview");
  const auto Chunk = viewChunk("1", View);
  EXPECT_EQ(field(Chunk, "text").find("SECRET"), std::string::npos);
  EXPECT_NE(field(Chunk, "text").find("binding_"), std::string::npos);
  for (const auto *Response : {&Preview, &Records, &Chunk}) {
    std::string Encoded;
    llvm::raw_string_ostream(Encoded) << *Response;
    EXPECT_EQ(Encoded.find("SECRET"), std::string::npos);
    EXPECT_EQ(Encoded.find(Path), std::string::npos);
  }
  const auto Review =
      "{\"schema_version\":1,\"locally_reviewed\":true,\"reviewed_ranges\":[{"
      "\"byte_offset\":\"0\",\"byte_length\":\"" +
      std::to_string(Text.size()) + "\"}]}";
  const auto Clear = viewPreview("1", Source, Review);
  const auto ClearID = field(Clear, "view_id");
  EXPECT_NE(ClearID, View);
  EXPECT_FALSE(Clear.getAsObject()->get("text"));
  EXPECT_EQ(errorCode(viewChunk("1", ClearID)), "source_view_not_committed");
  viewCommit("1", field(Clear, "preview_token"));
  EXPECT_EQ(field(viewChunk("1", ClearID), "text"), Text);
  EXPECT_EQ(errorCode(viewChunk("1", View)), "source_view_not_committed");
  EXPECT_EQ(errorCode(viewRecords("1", View)), "unknown_source_view");
  auto Replacement = viewPreview("1", Source);
  EXPECT_EQ(errorCode(viewCommit("1", "wrong")), "invalid_source_view_preview");
  EXPECT_EQ(errorCode(viewCommit("1", field(Replacement, "preview_token"))),
            "invalid_source_view_preview");
  EXPECT_EQ(field(viewChunk("1", ClearID), "text"), Text);
  Replacement = viewPreview("1", Source);
  EXPECT_EQ(
      errorCode(viewPreview("1", Source, R"({"schema_version":1,"bad":true})")),
      "invalid_source_view_policy");
  EXPECT_EQ(errorCode(viewCommit("1", field(Replacement, "preview_token"))),
            "invalid_source_view_preview");
  Replacement = viewPreview("1", Source);
  viewCommit("1", field(Replacement, "preview_token"));
  EXPECT_EQ(errorCode(viewChunk("1", ClearID)), "source_view_not_committed");
  commit(preview());
  EXPECT_EQ(errorCode(viewChunk("1", View)), "stale_revision");
  EXPECT_EQ(errorCode(viewChunk("2", View)), "source_view_not_committed");
  EXPECT_EQ(errorCode(viewPreview("2", Source)), "unknown_source");
}

TEST_F(WebSDK, ReviewedChunksPreserveUnicodeAndLineBoundariesWithRangeMapping) {
  const std::string Text = "'😀';\r\n";
  write(Text);
  commit(preview());
  const auto Source = field(analyze("1", rootID("1")), "source_id");
  auto Preview = viewPreview(
      "1", Source,
      R"({"schema_version":1,"locally_reviewed":true,"reviewed_ranges":[{"byte_offset":"0","byte_length":"9"}]})");
  const auto ID = field(Preview, "view_id");
  viewCommit("1", field(Preview, "preview_token"));
  EXPECT_EQ(field(viewChunk("1", ID, 0, 4), "text"), "'");
  EXPECT_EQ(errorCode(viewChunk("1", ID, 1, 3)), "source_view_chunk_too_small");
  EXPECT_EQ(field(viewChunk("1", ID, 1, 4), "text"), "😀");
  EXPECT_EQ(errorCode(viewChunk("1", ID, 2, 4)), "invalid_source_view_chunk");
  EXPECT_EQ(field(viewChunk("1", ID, 5, 3), "text"), "';");
  EXPECT_EQ(errorCode(viewChunk("1", ID, 7, 1)), "source_view_chunk_too_small");
  EXPECT_EQ(errorCode(viewChunk("1", ID, 8, 2)), "invalid_source_view_chunk");
  EXPECT_EQ(field(viewChunk("1", ID, 7, 2), "text"), "\r\n");
  EXPECT_EQ(field(viewChunk("1", ID, 9, 1), "text"), "");
  EXPECT_EQ(errorCode(viewChunk("1", ID, UINT64_MAX, 1)),
            "invalid_source_view_chunk");
  EXPECT_EQ(errorCode(viewChunk("1", ID, 0, 0)), "invalid_source_view_chunk");
  EXPECT_EQ(errorCode(viewChunk("1", ID, 0, 65537)),
            "invalid_source_view_chunk");
  const auto Records = viewRecords("1", ID);
  const auto *Items = Records.getAsObject()->getArray("items");
  ASSERT_NE(Items, nullptr);
  ASSERT_EQ(Items->size(), 3);
  uint64_t At = 0;
  for (const auto &Item : *Items) {
    EXPECT_EQ(field(Item, "source_byte_offset"), std::to_string(At));
    EXPECT_EQ(field(Item, "view_byte_offset"), std::to_string(At));
    EXPECT_EQ(field(Item, "mapping"), "byte_identity");
    At += std::stoull(field(Item, "source_byte_length"));
  }
  EXPECT_EQ(At, Text.size());
  const auto Middle = viewChunk("1", ID, 1, 4);
  EXPECT_EQ(Middle.getAsObject()->getInteger("first_segment"), 0);
  EXPECT_EQ(Middle.getAsObject()->getInteger("last_segment_exclusive"), 1);
}

TEST_F(WebSDK, ViewCacheLimitAllowsPolicyReplacementButBoundsPublishedSources) {
  write("const SECRET = 1;");
  commit(preview());
  const auto Root = rootID("1");
  std::vector<std::string> Sources;
  for (const auto Type : {"script", "module", "commonjs"})
    Sources.push_back(field(analyze("1", Root, Type), "source_id"));
  for (unsigned I = 0; I < 2; ++I) {
    auto P = viewPreview("1", Sources[I]);
    ASSERT_EQ(field(P, "status"), "ok");
    EXPECT_EQ(field(viewCommit("1", field(P, "preview_token")), "status"),
              "ok");
  }
  EXPECT_EQ(errorCode(viewPreview("1", Sources[2])),
            "source_view_cache_budget_exceeded");
  auto P = viewPreview("1", Sources[0]);
  EXPECT_EQ(field(viewCommit("1", field(P, "preview_token")), "status"), "ok");
  const auto Meta = take(neverd_web_metadata_json(Session));
  EXPECT_EQ(Meta.getAsObject()->getInteger("source_view_count"), 2);
}

#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebSDK, CliSourceViewsUseOnlyCppAndPublishOnlyTheStructuralPolicy) {
  write("const SECRET_VIEW = `SECRET_TEMPLATE`; /* SECRET_COMMENT */");
  const auto Output = (Root / "stdout").string();
  const auto Errors = (Root / "stderr").string();
  const llvm::StringRef Args[] = {NEVERD_WEB_TEST_CLI, "web", "view", Path,
                                  "script"};
  const llvm::StringRef Environment[] = {"PATH=/neverd-no-external-tools",
                                         "NEVERD_SIGNATURE_CACHE=off"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Errors};
  std::string LaunchError;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Environment,
                                      Redirects, 20, 0, &LaunchError),
            0)
      << LaunchError;
  std::ifstream Stdout(Output), Stderr(Errors);
  const std::string Out{std::istreambuf_iterator<char>(Stdout), {}};
  const std::string Err{std::istreambuf_iterator<char>(Stderr), {}};
  EXPECT_EQ(Out.find("SECRET"), std::string::npos);
  EXPECT_TRUE(Err.empty());
  bool SawPreview = false, SawRanges = false, SawText = false;
  std::istringstream Lines(Out);
  std::string Line;
  while (std::getline(Lines, Line)) {
    auto Parsed = llvm::json::parse(Line);
    ASSERT_TRUE(bool(Parsed));
    const auto *O = Parsed->getAsObject();
    ASSERT_NE(O, nullptr);
    if (O->getString("publication_status") == "preview") {
      EXPECT_FALSE(O->get("text"));
      SawPreview = true;
    }
    if (O->get("text")) {
      EXPECT_EQ(O->getString("publication_status"), "committed");
      EXPECT_EQ(O->getString("reviewed_bytes"), "0");
      SawText = true;
    }
    if (O->get("items") && O->get("view_id"))
      SawRanges = true;
  }
  EXPECT_TRUE(SawPreview && SawRanges && SawText);
}
#endif

TEST_F(WebSDK, SourceQueriesUsePinnedBytesAndDoNotExposeNamesOrText) {
  const std::string Canary = "SECRET_SOURCE_CANARY_ab1";
  write("const " + Canary + " = '" + Canary + "';");
  commit(preview());
  const auto ArtifactID = rootID("1");
  // A later host edit must not change the imported source or query identity.
  write("const = invalid syntax;");
  const auto Summary = analyze("1", ArtifactID);
  ASSERT_EQ(field(Summary, "parse_status"), "parsed");
  const auto SourceID = field(Summary, "source_id");
  auto First = nodes("1", SourceID);
  EXPECT_EQ(First.getAsObject()->getBoolean("page_complete"), false);
  const auto All = nodes("1", SourceID, 0, 512);
  EXPECT_EQ(All.getAsObject()->getBoolean("page_complete"), true);
  EXPECT_EQ(field(All, "semantic_analysis"), "not_analyzed");
  for (const auto *Result : {&Summary, &All}) {
    std::string Serialized;
    llvm::raw_string_ostream(Serialized) << *Result;
    EXPECT_EQ(Serialized.find(Canary), std::string::npos);
    EXPECT_EQ(Serialized.find(Path), std::string::npos);
  }
  for (unsigned I = 0; I != 20; ++I)
    EXPECT_EQ(field(analyze("1", ArtifactID), "source_id"), SourceID);
  auto Meta = take(neverd_web_metadata_json(Session));
  EXPECT_EQ(field(Meta, "revision"), "1");
  EXPECT_EQ(Meta.getAsObject()->getInteger("source_inventory_count"), 1);
  EXPECT_EQ(field(Meta, "analysis_status"), "partial");
  EXPECT_EQ(errorCode(analyze("1", ArtifactID, "auto")),
            "unsupported_source_type");
  EXPECT_EQ(errorCode(nodes("1", SourceID, 0, 513)), "invalid_page");
  commit(preview());
  EXPECT_EQ(errorCode(nodes("1", SourceID)), "stale_revision");
  EXPECT_EQ(errorCode(nodes("2", SourceID)), "unknown_source");
}

TEST_F(WebSDK, RejectedSyntaxHasDiagnosticsAndNoPartialNodeInventory) {
  write("const SECRET_INVALID_CANARY = ;");
  commit(preview());
  const auto Summary = analyze("1", rootID("1"));
  EXPECT_EQ(field(Summary, "status"), "ok");
  EXPECT_EQ(field(Summary, "parse_status"), "invalid_syntax");
  EXPECT_FALSE(Summary.getAsObject()->getArray("diagnostics")->empty());
  const auto Nodes = nodes("1", field(Summary, "source_id"));
  EXPECT_TRUE(Nodes.getAsObject()->getArray("items")->empty());
  EXPECT_EQ(Nodes.getAsObject()->getBoolean("page_complete"), true);
}
#else
TEST_F(WebSDK, DisabledParserHasNoImplicitExternalFallback) {
  write("const x = 1;");
  commit(preview());
  EXPECT_EQ(errorCode(analyze("1", rootID("1"))), "capability_unavailable");
  EXPECT_EQ(errorCode(mapLookup("1", "missing-map", "missing-source", 0)),
            "capability_unavailable");
  EXPECT_EQ(errorCode(bindings("1", "missing-source")),
            "capability_unavailable");
  EXPECT_EQ(errorCode(bindingRecords("1", "missing-source", "references")),
            "capability_unavailable");
  EXPECT_EQ(errorCode(semantics("1", "missing-source")),
            "capability_unavailable");
  EXPECT_EQ(errorCode(semanticRecords("1", "missing-source")),
            "capability_unavailable");
  EXPECT_EQ(errorCode(modules("1", "missing-source")),
            "capability_unavailable");
  EXPECT_EQ(errorCode(bundles("1", "missing-source")),
            "capability_unavailable");
  EXPECT_EQ(errorCode(bundleRecords("1", "missing-source", "modules")),
            "capability_unavailable");
  EXPECT_EQ(errorCode(moduleRecords("1", "missing-source", "requests")),
            "capability_unavailable");
  EXPECT_EQ(errorCode(viewPreview("1", "source")), "capability_unavailable");
  EXPECT_EQ(errorCode(viewCommit("1", "token")), "capability_unavailable");
  EXPECT_EQ(errorCode(viewRecords("1", "view")), "capability_unavailable");
  EXPECT_EQ(errorCode(viewChunk("1", "view")), "capability_unavailable");
  EXPECT_EQ(errorCode(navigation("1", "source")), "capability_unavailable");
  EXPECT_EQ(errorCode(navigationRecords("1", "source", "functions")),
            "capability_unavailable");
  EXPECT_EQ(errorCode(anchor("1", "source", 0, 0)), "capability_unavailable");
}
#endif
#endif
} // namespace
