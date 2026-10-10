#include "AsarFixture.h"
#include "NativeFixture.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"

#include <filesystem>
#include <fstream>

namespace {
using namespace neverd::web::test;
using llvm::json::Value;
namespace fs = std::filesystem;

Value take(const char *Owned) {
  if (!Owned) {
    ADD_FAILURE() << "Missing C API response";
    return nullptr;
  }
  const std::string Bytes(Owned);
  neverd_free_string(Owned);
  EXPECT_EQ(Bytes.find("CANARY"), std::string::npos);
  auto Result = llvm::json::parse(Bytes);
  if (!Result) {
    ADD_FAILURE() << llvm::toString(Result.takeError());
    return nullptr;
  }
  return std::move(*Result);
}
std::string field(const Value &V, const char *Name) {
  if (const auto *O = V.getAsObject())
    if (const auto S = O->getString(Name))
      return S->str();
  ADD_FAILURE() << "Missing field: " << Name;
  return {};
}
std::string code(const Value &V) {
  EXPECT_EQ(field(V, "status"), "error");
  const auto *O = V.getAsObject();
  return O && O->getObject("error")
             ? O->getObject("error")->getString("code").value_or("").str()
             : std::string();
}
const llvm::json::Array &items(const Value &V) {
  const auto *O = V.getAsObject();
  if (!O || !O->getArray("items"))
    throw std::runtime_error("Missing result page");
  return *O->getArray("items");
}
struct NativeHandle {
  neverd_session_t Value = nullptr;
  ~NativeHandle() { neverd_session_destroy(Value); }
};

TEST(WebAsarAvailability, MissingPolicyRefusesExtractionWithoutFallback) {
#ifdef _WIN32
  GTEST_SKIP() << "Windows web input capture is not implemented";
#endif
  const auto Caps = take(neverd_web_capabilities_json());
  for (const auto &A : *Caps.getAsObject()->getArray("analysis"))
    if (A.getAsObject()->getString("kind") == "asar_extraction" &&
        A.getAsObject()->getBoolean("available").value_or(false))
      GTEST_SKIP() << "Requires the ASAR/ICU capability to be omitted";
  struct Guard {
    neverd_web_session_t Web = neverd_web_session_create();
    fs::path Root;
    ~Guard() {
      neverd_web_session_destroy(Web);
      if (!Root.empty()) {
        std::error_code EC;
        fs::remove_all(Root, EC);
      }
    }
  } G;
  ASSERT_NE(G.Web, nullptr);
  llvm::SmallString<128> Root;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-asar-off", Root));
  G.Root = Root.str().str();
  const auto Path = (G.Root / "input").string();
  {
    std::ofstream F(Path);
    F << "CANARY_NOT_AN_ARCHIVE";
    ASSERT_TRUE(F.good());
  }
  const auto Preview = take(neverd_web_import_preview_json(
      G.Web, Path.data(), Path.size(), nullptr, 0));
  const auto Token = field(Preview, "preview_token");
  const auto Committed =
      take(neverd_web_import_commit_json(G.Web, Token.data(), Token.size()));
  const auto Revision = field(Committed, "revision");
  const auto Page = take(
      neverd_web_artifacts_json(G.Web, Revision.data(), Revision.size(), 0, 1));
  ASSERT_EQ(items(Page).size(), 1U);
  const auto ID = field(items(Page)[0], "artifact_id");
  EXPECT_EQ(code(take(neverd_web_asar_extract_json(G.Web, Revision.data(),
                                                   Revision.size(), ID.data(),
                                                   ID.size(), nullptr, 0))),
            "archive_path_policy_unavailable");
  EXPECT_EQ(take(neverd_web_metadata_json(G.Web))
                .getAsObject()
                ->getInteger("asar_extraction_count"),
            0);
}

class WebAsarSDK : public testing::Test {
protected:
  neverd_web_session_t Web = nullptr;
  fs::path Root, Input;
  std::string Revision;
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "Windows web input capture is not implemented";
#endif
    const auto Caps = take(neverd_web_capabilities_json());
    bool Available = false;
    for (const auto &A : *Caps.getAsObject()->getArray("analysis"))
      if (A.getAsObject()->getString("kind") == "asar_extraction")
        Available = A.getAsObject()->getBoolean("available").value_or(false);
    if (!Available)
      GTEST_SKIP() << "Pinned native ASAR path policy is unavailable";
    Web = neverd_web_session_create();
    ASSERT_NE(Web, nullptr);
    llvm::SmallString<128> P;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-asar-sdk", P));
    Root = P.str().str();
    Input = Root / "input";
    fs::create_directory(Input);
  }
  void TearDown() override {
    neverd_web_session_destroy(Web);
    if (!Root.empty()) {
      std::error_code EC;
      fs::remove_all(Root, EC);
    }
  }
  void write(std::string_view Name, std::string_view Bytes) {
    const auto P = Input / std::string(Name);
    fs::create_directories(P.parent_path());
    std::ofstream F(P, std::ios::binary);
    F.write(Bytes.data(), Bytes.size());
    ASSERT_TRUE(F.good());
  }
  void capture(bool Directory = false) {
    const auto P = (Directory ? Input : Input / "a.asar").string();
    const auto Preview = take(
        neverd_web_import_preview_json(Web, P.data(), P.size(), nullptr, 0));
    const auto Token = field(Preview, "preview_token");
    const auto Commit =
        take(neverd_web_import_commit_json(Web, Token.data(), Token.size()));
    ASSERT_EQ(field(Commit, "status"), "ok");
    Revision = field(Commit, "revision");
  }
  Value artifacts() {
    return take(neverd_web_artifacts_json(Web, Revision.data(), Revision.size(),
                                          0, 512));
  }
  std::string artifact(std::string_view Bytes) {
    const auto Page = artifacts();
    for (const auto &A : items(Page))
      if (A.getAsObject()->getString("blob_sha256") == asarHash(Bytes))
        return field(A, "artifact_id");
    throw std::runtime_error("Missing fixture artifact");
  }
  std::string directory() {
    const auto Page = artifacts();
    for (size_t I = 1; I < items(Page).size(); ++I)
      if (items(Page)[I].getAsObject()->getString("kind") == "directory")
        return field(items(Page)[I], "artifact_id");
    throw std::runtime_error("Missing captured directory");
  }
  Value extract(std::string_view A, std::string_view D = {}) {
    return take(neverd_web_asar_extract_json(Web, Revision.data(),
                                             Revision.size(), A.data(),
                                             A.size(), D.data(), D.size()));
  }
  Value records(std::string_view E, uint64_t Offset = 0, uint64_t Limit = 512) {
    return take(neverd_web_asar_records_json(Web, Revision.data(),
                                             Revision.size(), E.data(),
                                             E.size(), Offset, Limit));
  }
  Value source(std::string_view ID) {
    return take(neverd_web_source_analyze_json(Web, Revision.data(),
                                               Revision.size(), ID.data(),
                                               ID.size(), "module", 6));
  }
  Value native(std::string_view ID, NativeHandle &N) {
    return take(neverd_web_native_open_json(
        Web, Revision.data(), Revision.size(), ID.data(), ID.size(), &N.Value));
  }
};

TEST_F(WebAsarSDK,
       PackedSourcesMapsAnchorsAndNativeHandoffShareMemberIdentity) {
  const std::string JS = "export const CANARY_VALUE = 7;";
  const std::string Map =
      R"({"version":3,"sources":["CANARY.map.js"],"names":[],"mappings":"AAAA","sourcesContent":["export const CANARY_MAP=7;"]})";
  const auto ELF = nativeELF();
  const auto Archive =
      asarArchive({{"a-CANARY.js", asarFile(JS)},
                   {"b-CANARY.map", asarFile(Map, JS.size())},
                   {"c-CANARY.node", asarFile(ELF, JS.size() + Map.size())}},
                  JS + Map + ELF);
  write("a.asar", Archive);
  capture();
  const auto A = artifact(Archive);
  const auto E = extract(A);
  ASSERT_EQ(field(E, "extraction_status"), "complete");
  EXPECT_EQ(field(artifacts(), "analysis_status"), "partial");
  const auto ID = field(E, "extraction_id");
  const auto Page = records(ID);
  ASSERT_EQ(items(Page).size(), 3U);
  const auto JSID = field(items(Page)[0], "member_id");
  const auto MapID = field(items(Page)[1], "member_id");
  const auto NativeID = field(items(Page)[2], "member_id");
  const auto M = take(neverd_web_source_map_analyze_json(
      Web, Revision.data(), Revision.size(), MapID.data(), MapID.size()));
  EXPECT_EQ(field(M, "artifact_id"), MapID);
  EXPECT_EQ(field(M, "blob_sha256"), asarHash(Map));
  EXPECT_EQ(field(M, "association_status"), "unbound");
  NativeHandle N;
  const auto Handoff = native(NativeID, N);
  ASSERT_EQ(field(Handoff, "status"), "ok");
  ASSERT_NE(N.Value, nullptr);
  EXPECT_EQ(field(Handoff, "blob_sha256"), asarHash(ELF));
  const auto *Origin = Handoff.getAsObject()->getObject("origin");
  ASSERT_NE(Origin, nullptr);
  EXPECT_EQ(Origin->getString("kind"), "asar_packed_member");
  EXPECT_EQ(Origin->getString("member_id"), NativeID);
  EXPECT_EQ(Origin->getString("container_artifact_id"), A);
#if NEVERD_TEST_WEB_JAVASCRIPT
  const auto S = source(JSID);
  ASSERT_EQ(field(S, "status"), "ok");
  EXPECT_EQ(field(S, "blob_sha256"), asarHash(JS));
  const auto SourceID = field(S, "source_id");
  const auto Anchor = take(neverd_web_source_anchor_json(
      Web, Revision.data(), Revision.size(), SourceID.data(), SourceID.size(),
      7, 5, nullptr, 0));
  ASSERT_EQ(field(Anchor, "status"), "ok");
  const auto *Storage = Anchor.getAsObject()->getObject("storage");
  ASSERT_NE(Storage, nullptr);
  EXPECT_EQ(Storage->getString("member_id"), JSID);
  EXPECT_EQ(
      Storage->getString("byte_offset"),
      std::to_string(std::stoull(field(items(Page)[0], "byte_offset")) + 7));
  EXPECT_EQ(Storage->getString("byte_length"), "5");
#else
  EXPECT_EQ(code(source(JSID)), "capability_unavailable");
#endif
  // Handoff owns its bytes and origin independently of both host and web state.
  fs::remove_all(Input);
  neverd_web_session_destroy(Web);
  Web = nullptr;
  const auto Survives = take(neverd_web_native_metadata_json(N.Value));
  EXPECT_EQ(field(Survives, "handoff_id"), field(Handoff, "handoff_id"));
}

TEST_F(WebAsarSDK,
       ExplicitUnpackedSourcesKeepExternalOriginsAndVirtualImports) {
  const std::string JS = "import './CANARY-dep.js'; export const x=1;";
  const std::string Dep = "export const y=2;";
  const auto ELF = nativeELF();
  const auto Archive = asarArchive({{"CANARY-dep.js", asarFile(Dep, 0, true)},
                                    {"main.js", asarFile(JS)},
                                    {"native.node", asarFile(ELF, 0, true)}},
                                   JS);
  write("a.asar", Archive);
  write("chosen/CANARY-dep.js", Dep);
  write("chosen/native.node", ELF);
  capture(true);
  const auto A = artifact(Archive), D = directory();
  const auto Missing = extract(A);
  EXPECT_EQ(field(Missing, "extraction_status"), "partial");
  const auto E = extract(A, D);
  EXPECT_EQ(field(E, "extraction_status"), "complete");
  EXPECT_NE(field(E, "extraction_id"), field(Missing, "extraction_id"));
  const auto Page = records(field(E, "extraction_id"));
  ASSERT_EQ(items(Page).size(), 3U);
  const auto DepID = field(items(Page)[0], "member_id");
  const auto MainID = field(items(Page)[1], "member_id");
  const auto NativeID = field(items(Page)[2], "member_id");
  NativeHandle N;
  const auto Handoff = native(NativeID, N);
  ASSERT_EQ(field(Handoff, "status"), "ok");
  const auto *Origin = Handoff.getAsObject()->getObject("origin");
  ASSERT_NE(Origin, nullptr);
  EXPECT_EQ(Origin->getString("kind"), "asar_unpacked_member");
  EXPECT_EQ(Origin->getString("storage_artifact_id"), artifact(ELF));
  EXPECT_EQ(Origin->getString("byte_offset"), "0");
#if NEVERD_TEST_WEB_JAVASCRIPT
  const auto S = source(MainID), U = source(DepID);
  const auto SourceID = field(S, "source_id"),
             DepSource = field(U, "source_id");
  const auto Summary = take(neverd_web_source_modules_analyze_json(
      Web, Revision.data(), Revision.size(), SourceID.data(), SourceID.size()));
  ASSERT_EQ(field(Summary, "status"), "ok");
  const auto Links = take(neverd_web_source_module_records_json(
      Web, Revision.data(), Revision.size(), SourceID.data(), SourceID.size(),
      "requests", 8, 0, 20));
  ASSERT_EQ(items(Links).size(), 1U);
  EXPECT_EQ(field(items(Links)[0], "candidate_artifact_id"), DepID);
  EXPECT_EQ(field(items(Links)[0], "link_status"),
            "exact_admitted_file_candidate");
  const auto Anchor = take(neverd_web_source_anchor_json(
      Web, Revision.data(), Revision.size(), DepSource.data(), DepSource.size(),
      7, 5, nullptr, 0));
  const auto *Storage = Anchor.getAsObject()->getObject("storage");
  ASSERT_NE(Storage, nullptr);
  EXPECT_EQ(Storage->getString("storage_artifact_id"), artifact(Dep));
  EXPECT_EQ(Storage->getString("container_artifact_id"), A);
  EXPECT_EQ(Storage->getString("byte_offset"), "7");
#endif
}

TEST_F(WebAsarSDK, UnavailableMembersCannotSupplySourceMapOrNativeBytes) {
  const std::string JS = "export const x=1;";
  const auto Archive =
      asarArchive({{"bad.js", asarFile(JS)},
                   {"missing.js", asarFile(JS, 0, true)},
                   {"link", llvm::json::Object{{"link", "bad.js"}}}},
                  std::string(JS.size(), 'x'));
  write("a.asar", Archive);
  capture();
  const auto E = extract(artifact(Archive));
  EXPECT_EQ(field(E, "extraction_status"), "partial");
  const auto Page = records(field(E, "extraction_id"));
  ASSERT_EQ(items(Page).size(), 3U);
  for (const auto &Member : items(Page)) {
    const auto ID = field(Member, "member_id");
    NativeHandle N;
    EXPECT_EQ(code(native(ID, N)), "artifact_bytes_unavailable");
    EXPECT_EQ(N.Value, nullptr);
    EXPECT_EQ(
        code(take(neverd_web_source_map_analyze_json(
            Web, Revision.data(), Revision.size(), ID.data(), ID.size()))),
        "artifact_bytes_unavailable");
#if NEVERD_TEST_WEB_JAVASCRIPT
    EXPECT_EQ(code(source(ID)), "artifact_bytes_unavailable");
#endif
  }
}

TEST_F(WebAsarSDK, FailedExtractionsDoNotPublishAndCachedQueriesBindRevision) {
  const auto Archive = asarArchive({{"a", asarFile("ok")}}, "ok");
  write("a.asar", Archive);
  write("bad.asar", "invalid");
  for (unsigned I = 0; I < 4; ++I)
    fs::create_directory(Input / ("d" + std::to_string(I)));
  capture(true);
  const auto A = artifact(Archive);
  EXPECT_EQ(code(extract(artifact("invalid"))), "asar_truncated_header");
  EXPECT_EQ(take(neverd_web_metadata_json(Web))
                .getAsObject()
                ->getInteger("asar_extraction_count"),
            0);
  const auto E = extract(A);
  const auto ID = field(E, "extraction_id");
  EXPECT_EQ(field(extract(A), "extraction_id"), ID);
  EXPECT_EQ(items(records(ID, 1, 1)).size(), 0U);
  EXPECT_EQ(code(records(ID, 0, 0)), "invalid_page");
  EXPECT_EQ(code(records(ID, UINT64_MAX, 1)), "invalid_page");
  EXPECT_EQ(code(records(ID, 0, 513)), "invalid_page");
  EXPECT_EQ(code(extract(A, A)), "asar_invalid_unpacked_directory");
  const auto Page = artifacts();
  unsigned Seen = 0;
  for (size_t I = 1; I < items(Page).size(); ++I)
    if (items(Page)[I].getAsObject()->getString("kind") == "directory") {
      const auto Next = extract(A, field(items(Page)[I], "artifact_id"));
      if (++Seen <= 3)
        EXPECT_EQ(field(Next, "status"), "ok");
      else
        EXPECT_EQ(code(Next), "asar_cache_budget_exceeded");
    }
  EXPECT_EQ(Seen, 4U);
  const auto Previous = Revision;
  write("a.asar", asarArchive({{"b", asarFile("new")}}, "new"));
  capture();
  EXPECT_NE(Revision, Previous);
  EXPECT_EQ(code(records(ID)), "unknown_asar_extraction");
  EXPECT_EQ(
      code(take(neverd_web_asar_records_json(
          Web, Previous.data(), Previous.size(), ID.data(), ID.size(), 0, 1))),
      "stale_revision");
  EXPECT_EQ(take(neverd_web_metadata_json(Web))
                .getAsObject()
                ->getInteger("asar_extraction_count"),
            0);
}

#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebAsarSDK, CLISourceMapNativeAndRefusalsWorkWithoutExternalTools) {
  const std::string JS = "export const CANARY_SECRET=7;";
  const std::string Map =
      R"({"version":3,"sources":[],"names":[],"mappings":""})";
  const auto ELF = nativeELF();
  write("a.asar",
        asarArchive({{"a-CANARY.js", asarFile(JS)},
                     {"b.map", asarFile(Map, JS.size())},
                     {"c.node", asarFile(ELF, JS.size() + Map.size())}},
                    JS + Map + ELF));
  const auto Path = (Input / "a.asar").string();
  const auto Out = (Root / "stdout").string(), Err = (Root / "stderr").string();
  auto Run = [&](std::vector<std::string> Arguments, int Expected,
                 std::string_view Contains) {
    std::vector<llvm::StringRef> Args{NEVERD_WEB_TEST_CLI, "web"};
    for (const auto &A : Arguments)
      Args.emplace_back(A);
    const llvm::StringRef Env[]{"PATH=/neverd-no-external-tools",
                                "NEVERD_SIGNATURE_CACHE=off"};
    const std::optional<llvm::StringRef> Redirects[]{llvm::StringRef(), Out,
                                                     Err};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Env,
                                        Redirects, 30),
              Expected);
    std::string Combined;
    for (const auto &P : {Out, Err}) {
      std::ifstream F(P);
      Combined.append(std::istreambuf_iterator<char>(F), {});
    }
    EXPECT_EQ(Combined.find("CANARY"), std::string::npos);
    EXPECT_NE(Combined.find(Contains), std::string::npos);
  };
  Run({"asar", Path}, 0, "asar-pickle-json-v1");
  Run({"asar-map", Path, "0", "-", "1"}, 0, "format_status");
  Run({"asar-native", Path, "0", "-", "2"}, 0, "asar_packed_member");
#if NEVERD_TEST_WEB_JAVASCRIPT
  Run({"asar-source", Path, "0", "-", "0", "module"}, 0, "source_id");
  Run({"asar-view", Path, "0", "-", "0", "module"}, 0, "view_id");
  Run({"asar-navigate", Path, "0", "-", "0", "module"}, 0, "navigation_id");
  Run({"asar-anchor", Path, "0", "-", "0", "module", "7", "5"}, 0,
      "asar_packed_member");
#else
  Run({"asar-source", Path, "0", "-", "0", "module"}, 1,
      "capability_unavailable");
#endif
  Run({"asar", Path, "00"}, 2, "invalid_asar_selection");
  Run({"asar", Path, "18446744073709551616"}, 2, "invalid_asar_selection");
  Run({"asar-source", Path, "0", "-", "3", "module"}, 1,
      "artifact_bytes_unavailable");
  Run({"asar", Path, "0", "0"}, 1, "asar_invalid_unpacked_directory");
  // Output captures are outside Input, so each CLI imports this same tree.
  write("a.asar", asarArchive({{"a.js", asarFile(JS)},
                               {"b.map", asarFile(Map, JS.size())},
                               {"c.node", asarFile(ELF, 0, true)}},
                              JS + Map));
  write("chosen/c.node", ELF);
  Run({"asar-native", Input.string(), "1", "2", "2"}, 0,
      "asar_unpacked_member");
  Run({"asar-native", Input.string(), "1", "-", "2"}, 1,
      "artifact_bytes_unavailable");
}
#endif
} // namespace
