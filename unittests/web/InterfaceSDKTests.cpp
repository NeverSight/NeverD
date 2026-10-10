//===- InterfaceSDKTests.cpp - Passive publication and CLI contracts ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Redaction receipts, immutable evidence, bounded caches and CLI parity.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <filesystem>
#include <fstream>

namespace {
using Value = llvm::json::Value;
void redacted(std::string_view Text) {
  EXPECT_EQ(Text.find("CANARY"), std::string_view::npos);
  EXPECT_EQ(Text.find("canary"), std::string_view::npos);
}
Value parse(std::string_view Text) {
  redacted(Text);
  auto V = llvm::json::parse(Text);
  if (!V) {
    llvm::consumeError(V.takeError());
    throw std::runtime_error("invalid response");
  }
  return std::move(*V);
}
Value take(const char *Owned) {
  if (!Owned)
    throw std::runtime_error("missing response");
  const std::string Text(Owned);
  neverd_free_string(Owned);
  return parse(Text);
}
std::string field(const Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
std::string code(const Value &V) {
  EXPECT_EQ(field(V, "status"), "error");
  const auto *E = V.getAsObject()->getObject("error");
  return E ? E->getString("code").value_or("").str() : "";
}
class WebInterfaceSDK : public ::testing::Test {
protected:
  std::filesystem::path Root;
  neverd_web_session_t S = nullptr;
  std::string Revision;
  std::vector<std::string> Artifacts;
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX capture required";
#endif
    llvm::SmallString<128> Dir;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-interface-sdk", Dir));
    Root = Dir.str().str();
    for (unsigned I = 0; I < 5; ++I) {
      std::ofstream(Root / ("a" + std::to_string(I) + ".har"))
          << R"({"log":{"version":"1.2","creator":{"name":"CANARY_CREATOR",
        "version":"CANARY_VERSION"},"entries":[{
        "startedDateTime":"CANARY_TIMESTAMP",
        "request":{"method":"POST","url":"https://CANARY_HOST/CANARY_PATH?CANARY_QUERY=CANARY_VALUE",
          "headers":[{"name":"Authorization","value":"CANARY_AUTH"},
                     {"name":"CANARY_HEADER","value":"CANARY_VALUE"}],
          "cookies":[{"name":"CANARY_COOKIE","value":"CANARY_COOKIE_VALUE"}],
          "queryString":[{"name":"CANARY_QUERY","value":"CANARY_VALUE"}],
          "postData":{"text":"CANARY_BODY"}},
        "response":{"status":200,"statusText":"CANARY_STATUS",
          "redirectURL":"https://CANARY_REDIRECT",
          "headers":[{"name":"Set-Cookie","value":"CANARY_COOKIE"}],
          "content":{"text":"CANARY_RESPONSE","encoding":"CANARY_ENCODING"}},
        "_webSocketMessages":[{"data":"CANARY_FRAME"}],
        "_requestId":"CANARY_VENDOR"}],"comment":"CANARY_COMMENT_)"
          << I << "\"}}";
      std::ofstream(Root / ("b" + std::to_string(I) + ".js"))
          << "fetch('https://CANARY_HOST/"
             "CANARY_PATH?CANARY_QUERY=CANARY_VALUE',"
             "{method:'POST',headers:{CANARY_HEADER:'CANARY_VALUE'},"
             "body:'CANARY_BODY'});";
    }
    std::ofstream(Root / "bad.har") << "CANARY_MALFORMED";
    S = neverd_web_session_create();
    ASSERT_NE(S, nullptr);
    open();
  }
  void TearDown() override {
    neverd_web_session_destroy(S);
    std::error_code EC;
    std::filesystem::remove(Root.string() + ".stdout", EC);
    std::filesystem::remove(Root.string() + ".stderr", EC);
    std::filesystem::remove_all(Root, EC);
  }
  void open() {
    const auto Path = Root.string();
    const auto P = take(neverd_web_import_preview_json(
        S, Path.data(), Path.size(), nullptr, 0));
    const auto T = field(P, "preview_token");
    Revision = field(take(neverd_web_import_commit_json(S, T.data(), T.size())),
                     "revision");
    const auto Page = take(
        neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 0, 128));
    Artifacts.clear();
    for (const auto &I : *Page.getAsObject()->getArray("items"))
      Artifacts.push_back(field(I, "artifact_id"));
    ASSERT_EQ(Artifacts.size(), 12u);
  }
  Value preview(unsigned I = 1) {
    const auto &A = Artifacts.at(I);
    return take(neverd_web_har_preview_json(S, Revision.data(), Revision.size(),
                                            A.data(), A.size()));
  }
  Value commit(std::string Token) {
    return take(neverd_web_har_commit_json(S, Revision.data(), Revision.size(),
                                           Token.data(), Token.size()));
  }
  Value records(std::string ID, uint64_t Offset = 0, uint64_t Limit = 128) {
    return take(neverd_web_har_records_json(S, Revision.data(), Revision.size(),
                                            ID.data(), ID.size(), Offset,
                                            Limit));
  }
  std::string admit(unsigned I = 1) {
    const auto P = preview(I);
    return field(commit(field(P, "preview_token")), "capture_id");
  }
  Value source(unsigned I = 6) {
    const auto &A = Artifacts.at(I);
    const auto Parsed = take(neverd_web_source_analyze_json(
        S, Revision.data(), Revision.size(), A.data(), A.size(), "module", 6));
    const auto ID = field(Parsed, "source_id");
    return take(neverd_web_interfaces_analyze_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size()));
  }
  Value compare(std::string Source, std::string Capture) {
    return take(neverd_web_interfaces_compare_json(
        S, Revision.data(), Revision.size(), Source.data(), Source.size(),
        Capture.data(), Capture.size()));
  }
};

TEST_F(WebInterfaceSDK, PreviewIsRequiredAndTokensAreSingleUse) {
  auto P = preview();
  const auto ID = field(P, "capture_id"), T = field(P, "preview_token");
  EXPECT_EQ(field(P, "publication_status"), "preview");
  EXPECT_EQ(code(records(ID)), "har_capture_not_committed");
  EXPECT_EQ(field(preview(11), "status"), "error");
  EXPECT_EQ(code(commit(T)), "invalid_har_preview");
  P = preview();
  const auto Older = field(P, "preview_token");
  const auto Newer = field(preview(), "preview_token");
  EXPECT_NE(Older, Newer);
  EXPECT_EQ(code(commit(Older)), "invalid_har_preview");
  EXPECT_EQ(code(commit(Newer)), "invalid_har_preview");
  P = preview();
  const auto Accepted = field(P, "preview_token");
  const auto H = commit(Accepted);
  EXPECT_EQ(field(H, "publication_status"), "committed");
  EXPECT_EQ(code(commit(Accepted)), "invalid_har_preview");
  EXPECT_EQ(records(ID).getAsObject()->getArray("items")->size(), 1u);
}

TEST_F(WebInterfaceSDK, MetadataAndErrorsNeverEchoSensitiveValues) {
  const auto P = preview();
  EXPECT_EQ(P.getAsObject()->getInteger("text_bodies_withheld"), 2);
  EXPECT_EQ(P.getAsObject()->getInteger("invalid_timestamps"), 1);
  const auto ID = field(commit(field(P, "preview_token")), "capture_id");
  const auto Page = records(ID);
  const auto &O = *(*Page.getAsObject()->getArray("items"))[0].getAsObject();
  EXPECT_EQ(O.getString("method"), "POST");
  EXPECT_EQ(O.getInteger("response_status"), 200);
  EXPECT_EQ(O.getString("response_encoding"), "unsupported_redacted");
  EXPECT_TRUE(O.getBoolean("websocket_extension_unsupported").value_or(false));
  EXPECT_FALSE(
      Page.getAsObject()->getBoolean("capture_authenticated").value_or(true));
  EXPECT_EQ(code(records(ID, 0, 129)), "invalid_page");
  EXPECT_EQ(code(records(ID, 2)), "invalid_page");
  EXPECT_EQ(code(records("CANARY_UNKNOWN")), "har_capture_not_committed");
  EXPECT_EQ(code(take(neverd_web_har_preview_json(
                S, Revision.data(), Revision.size(), nullptr, 64))),
            "invalid_buffer");
  const auto Hash = field(P, "blob_sha256");
  std::ofstream(Root / "a0.har") << "CANARY_REPLACED";
  EXPECT_EQ(field(preview(), "blob_sha256"), Hash);
  const auto OldRevision = Revision;
  open();
  EXPECT_EQ(code(take(neverd_web_har_records_json(S, OldRevision.data(),
                                                  OldRevision.size(), ID.data(),
                                                  ID.size(), 0, 1))),
            "stale_revision");
  EXPECT_EQ(code(records(ID)), "har_capture_not_committed");
  EXPECT_EQ(field(preview(), "status"), "error");
}

TEST_F(WebInterfaceSDK, CaptureCacheAndImportRevocationAreBounded) {
  std::vector<std::string> IDs;
  for (unsigned I = 1; I <= 4; ++I)
    IDs.push_back(admit(I));
  EXPECT_EQ(code(preview(5)), "har_cache_budget_exceeded");
  EXPECT_EQ(admit(1), IDs[0]);
  const auto Pending = field(preview(1), "preview_token");
  open();
  EXPECT_EQ(code(commit(Pending)), "invalid_har_preview");
  EXPECT_EQ(code(records(IDs[0])), "har_capture_not_committed");
  EXPECT_FALSE(admit(5).empty());
}

TEST_F(WebInterfaceSDK, PreviewCountsURLQueriesAndFormFieldsWithoutTextBodies) {
  std::ofstream(Root / "a0.har") << R"({"log":{"version":"1.2","entries":[{
    "request":{"url":"https://CANARY_HOST/path?token=CANARY&email=CANARY",
    "postData":{"params":[{"name":"password","value":"CANARY"}]}}},
    {"request":{"url":"/relative?token=CANARY"}}]}})";
  open();
  const auto P = preview();
  EXPECT_EQ(P.getAsObject()->getInteger("query_records_withheld"), 0);
  EXPECT_EQ(P.getAsObject()->getInteger("text_bodies_withheld"), 0);
  EXPECT_EQ(P.getAsObject()->getInteger("url_query_components_withheld"), 3);
  EXPECT_EQ(P.getAsObject()->getInteger("url_values_withheld"), 2);
  EXPECT_EQ(P.getAsObject()->getInteger("post_parameter_records_withheld"), 1);
}

#if NEVERD_TEST_WEB_JAVASCRIPT
TEST_F(WebInterfaceSDK, StaticAndObservedEvidenceJoinWithoutUpgradingClaims) {
  const auto A = source();
  const auto Analysis = field(A, "interface_analysis_id");
  EXPECT_EQ(A.getAsObject()->getInteger("interface_count"), 1);
  EXPECT_EQ(field(A, "evidence_class"), "static_inference");
  auto P = preview();
  const auto HID = field(P, "capture_id");
  EXPECT_EQ(code(compare(Analysis, HID)), "har_capture_not_committed");
  commit(field(P, "preview_token"));
  const auto C = compare(Analysis, HID);
  EXPECT_EQ(C.getAsObject()->getInteger("pair_count"), 1);
  EXPECT_FALSE(
      C.getAsObject()->getBoolean("source_execution_observed").value_or(true));
  const auto CID = field(C, "correlation_id");
  const auto Page = take(neverd_web_interface_records_json(
      S, Revision.data(), Revision.size(), Analysis.data(), Analysis.size(), 0,
      128));
  const auto &Entry = (*Page.getAsObject()->getArray("items"))[0];
  EXPECT_FALSE(field(Entry, "node_id").empty());
  EXPECT_FALSE(field(Entry, "body_node_id").empty());
  const auto Pairs = take(neverd_web_interface_correlation_records_json(
      S, Revision.data(), Revision.size(), CID.data(), CID.size(), 0, 128));
  const auto &Pair = (*Pairs.getAsObject()->getArray("items"))[0];
  EXPECT_EQ(field(Pair, "interface_id"), field(Entry, "interface_id"));
  const auto Observations = records(HID);
  EXPECT_EQ(field(Pair, "observation_id"),
            field((*Observations.getAsObject()->getArray("items"))[0],
                  "observation_id"));
  EXPECT_EQ(field(Pair, "capture_evidence_class"), "imported_observation");
  open();
  EXPECT_EQ(code(compare(Analysis, HID)), "unknown_interface_analysis");
  EXPECT_EQ(
      code(take(neverd_web_interface_correlation_records_json(
          S, Revision.data(), Revision.size(), CID.data(), CID.size(), 0, 1))),
      "unknown_interface_correlation");
}

TEST_F(WebInterfaceSDK, SourceAndCorrelationCachesAreBounded) {
  std::vector<std::string> Sources, Captures;
  for (unsigned I = 0; I < 4; ++I) {
    Sources.push_back(field(source(6 + I), "interface_analysis_id"));
    Captures.push_back(admit(1 + I));
  }
  EXPECT_EQ(code(source(10)), "interface_cache_budget_exceeded");
  for (const auto &H : Captures)
    EXPECT_EQ(compare(Sources[0], H).getAsObject()->getInteger("pair_count"),
              1);
  EXPECT_EQ(code(compare(Sources[1], Captures[0])),
            "interface_correlation_cache_budget_exceeded");
  EXPECT_EQ(
      compare(Sources[0], Captures[0]).getAsObject()->getInteger("pair_count"),
      1);
}
#endif

#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebInterfaceSDK, CLIRequiresMatchingPreviewHashAndPolicyWithoutTools) {
  const auto Path = Root.string(), Out = Path + ".stdout",
             Err = Path + ".stderr";
  const llvm::StringRef Env[] = {"PATH=/neverd-no-external-tools", "LC_ALL=C"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Out, Err};
  std::string Output, Errors;
  auto Run = [&](std::vector<std::string> Arguments) {
    llvm::SmallVector<llvm::StringRef> Args{NEVERD_WEB_TEST_CLI, "web"};
    for (const auto &A : Arguments)
      Args.push_back(A);
    const auto Status = llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args,
                                                  Env, Redirects, 30);
    std::ifstream O(Out), E(Err);
    Output.assign(std::istreambuf_iterator<char>(O), {});
    Errors.assign(std::istreambuf_iterator<char>(E), {});
    redacted(Output);
    redacted(Errors);
    return Status;
  };
  ASSERT_EQ(Run({"har-preview", Path, "1"}), 0);
  const auto P = parse(Output);
  const auto Policy = field(P, "redaction_policy");
  const auto Hash = field(P, "blob_sha256");
  EXPECT_EQ(Run({"har-import", Path, "1"}), 2);
  EXPECT_EQ(Run({"har-import", Path, "1", Policy, std::string(64, '0')}), 1);
  EXPECT_EQ(Output.find("observation_id"), std::string::npos);
  ASSERT_EQ(Run({"har-import", Path, "1", Policy, Hash}), 0);
  EXPECT_NE(Output.find("observation_id"), std::string::npos);
#if NEVERD_TEST_WEB_JAVASCRIPT
  EXPECT_EQ(Run({"interfaces", Path, "module", "6"}), 0);
  EXPECT_NE(Output.find("\"evidence_class\":\"static_inference\""),
            std::string::npos);
  EXPECT_EQ(
      Run({"interface-correlate", Path, "module", "6", "1", Policy, Hash}), 0);
  EXPECT_NE(Output.find("\"pair_count\":1"), std::string::npos);
#endif
  std::ofstream(Root / "a0.har", std::ios::app) << " ";
  EXPECT_EQ(Run({"har-import", Path, "1", Policy, Hash}), 1);
  EXPECT_NE(Errors.find("har_preview_receipt_mismatch"), std::string::npos);
}
#endif
} // namespace
