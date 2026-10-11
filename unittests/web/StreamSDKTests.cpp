//===- StreamSDKTests.cpp - Transcript publication and adapter checks
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preview gates, private-value canaries, cache lifetime and CLI receipts.
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
constexpr auto Profile = "recorded-jsonrpc-2.0-jsonl-v1";
constexpr auto Fixture =
    R"({"session":"CANARY_SESSION","direction":"client_to_server","timestamp":"CANARY_TIME","message":{"jsonrpc":"2.0","id":"CANARY_ID","method":"CANARY_METHOD","params":{"CANARY_KEY":"CANARY_SECRET"}}})"
    "\n"
    R"({"session":"CANARY_SESSION","direction":"server_to_client","message":{"jsonrpc":"2.0","id":"CANARY_ID","result":"CANARY_RESULT"}})"
    "\n";
Value parse(std::string_view Text) {
  EXPECT_EQ(Text.find("CANARY"), std::string_view::npos);
  auto V = llvm::json::parse(Text);
  if (!V) {
    llvm::consumeError(V.takeError());
    throw std::runtime_error("invalid reply");
  }
  return std::move(*V);
}
Value take(const char *Owned) {
  if (!Owned)
    throw std::runtime_error("missing reply");
  const std::string Text(Owned);
  neverd_free_string(Owned);
  return parse(Text);
}
std::string field(const Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
std::string code(const Value &V) {
  const auto *E = V.getAsObject()->getObject("error");
  return E ? E->getString("code").value_or("").str() : "";
}
class WebStreamSDK : public ::testing::Test {
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
        llvm::sys::fs::createUniqueDirectory("neverd-stream-sdk", Dir));
    Root = Dir.str().str();
    for (unsigned I = 0; I < 5; ++I)
      std::ofstream(Root / ("a" + std::to_string(I) + ".jsonl")) << Fixture;
    std::ofstream(Root / "bad") << "\xff";
    S = neverd_web_session_create();
    ASSERT_NE(S, nullptr);
    open();
  }
  void TearDown() override {
    neverd_web_session_destroy(S);
    std::error_code EC;
    std::filesystem::remove(Root.string() + ".out", EC);
    std::filesystem::remove(Root.string() + ".err", EC);
    std::filesystem::remove_all(Root, EC);
  }
  void open() {
    const auto Path = Root.string();
    const auto P = take(neverd_web_import_preview_json(
        S, Path.data(), Path.size(), nullptr, 0));
    const auto Token = field(P, "preview_token");
    Revision = field(
        take(neverd_web_import_commit_json(S, Token.data(), Token.size())),
        "revision");
    const auto Page = take(
        neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 0, 128));
    Artifacts.clear();
    for (const auto &I : *Page.getAsObject()->getArray("items"))
      Artifacts.push_back(field(I, "artifact_id"));
    ASSERT_EQ(Artifacts.size(), 7u);
  }
  Value preview(unsigned Index = 1, std::string_view P = Profile) {
    const auto &ID = Artifacts.at(Index);
    return take(neverd_web_stream_preview_json(S, Revision.data(),
                                               Revision.size(), ID.data(),
                                               ID.size(), P.data(), P.size()));
  }
  Value commit(std::string Token) {
    return take(neverd_web_stream_commit_json(
        S, Revision.data(), Revision.size(), Token.data(), Token.size()));
  }
  Value records(std::string ID, uint64_t Offset = 0, uint64_t Limit = 128) {
    return take(neverd_web_stream_records_json(S, Revision.data(),
                                               Revision.size(), ID.data(),
                                               ID.size(), Offset, Limit));
  }
  std::string admit(unsigned Index = 1) {
    return field(commit(field(preview(Index), "preview_token")),
                 "stream_capture_id");
  }
};

TEST_F(WebStreamSDK, PreviewTokensGatePublicationAndAreConsumedOnEveryAttempt) {
  auto P = preview();
  const auto ID = field(P, "stream_capture_id"), T = field(P, "preview_token");
  EXPECT_EQ(code(records(ID)), "stream_capture_not_committed");
  EXPECT_EQ(code(preview(6)), "invalid_stream_encoding");
  EXPECT_EQ(code(commit(T)), "invalid_stream_preview");
  P = preview();
  const auto Older = field(P, "preview_token");
  const auto Newer = field(preview(), "preview_token");
  EXPECT_NE(Older, Newer);
  EXPECT_EQ(code(commit(Older)), "invalid_stream_preview");
  EXPECT_EQ(code(commit(Newer)), "invalid_stream_preview");
  P = preview();
  const auto Accepted = field(P, "preview_token");
  EXPECT_EQ(field(commit(Accepted), "publication_status"), "committed");
  EXPECT_EQ(code(commit(Accepted)), "invalid_stream_preview");
  EXPECT_EQ(records(ID).getAsObject()->getArray("items")->size(), 2u);
}

TEST_F(WebStreamSDK, PrivateValuesStayExcludedAndCapturedBytesRemainImmutable) {
  auto P = preview();
  EXPECT_EQ(P.getAsObject()->getInteger("recorded_pair_candidates"), 1);
  const auto Hash = field(P, "blob_sha256");
  const auto ID = field(commit(field(P, "preview_token")), "stream_capture_id");
  const auto Page = records(ID);
  const auto &Items = *Page.getAsObject()->getArray("items");
  ASSERT_EQ(Items.size(), 2u);
  EXPECT_EQ(field(Items[0], "peer_record_id"), field(Items[1], "record_id"));
  EXPECT_EQ(field(Items[0], "timestamp_status"), "string_withheld");
  EXPECT_FALSE(Page.getAsObject()
                   ->getBoolean("protocol_negotiation_verified")
                   .value_or(true));
  EXPECT_EQ(code(records(ID, 0, 129)), "invalid_page");
  EXPECT_EQ(code(records(ID, 3)), "invalid_page");
  EXPECT_EQ(code(records("CANARY_ID")), "stream_capture_not_committed");
  EXPECT_EQ(code(preview(1, "CANARY_PROFILE")), "unsupported_stream_profile");
  std::ofstream(Root / "a0.jsonl") << "CANARY_REPLACED";
  EXPECT_EQ(field(preview(), "blob_sha256"), Hash);
  const auto OldRevision = Revision;
  open();
  EXPECT_EQ(code(take(neverd_web_stream_records_json(
                S, OldRevision.data(), OldRevision.size(), ID.data(), ID.size(),
                0, 1))),
            "stale_revision");
  EXPECT_EQ(code(records(ID)), "stream_capture_not_committed");
  EXPECT_NE(field(preview(), "blob_sha256"), Hash);
}

TEST_F(WebStreamSDK, CachesRevisionsAndInterpretationReceiptsStayDistinct) {
  const auto Generic = preview(1, "jsonl-metadata-v1");
  const auto Recorded = preview();
  EXPECT_EQ(field(Generic, "blob_sha256"), field(Recorded, "blob_sha256"));
  EXPECT_NE(field(Generic, "redaction_receipt"),
            field(Recorded, "redaction_receipt"));
  std::vector<std::string> IDs;
  for (unsigned I = 1; I <= 4; ++I)
    IDs.push_back(admit(I));
  EXPECT_EQ(code(preview(5)), "stream_cache_budget_exceeded");
  EXPECT_EQ(admit(), IDs[0]);
  const auto Token = field(preview(), "preview_token");
  open();
  EXPECT_EQ(code(commit(Token)), "invalid_stream_preview");
  EXPECT_EQ(code(records(IDs[0])), "stream_capture_not_committed");
  EXPECT_FALSE(admit(5).empty());
}

TEST_F(WebStreamSDK, SSEAndLogCanariesNeverEnterPublicMetadata) {
  std::ofstream(Root / "a0.jsonl")
      << "id: CANARY_ID\nevent: CANARY_EVENT\n: CANARY_COMMENT\n"
         "CANARY_FIELD: CANARY_VALUE\ndata: CANARY_PAYLOAD\n\n"
         "data: CANARY_UNFINISHED";
  std::ofstream(Root / "a1.jsonl")
      << "CANARY_LOG\n{\"CANARY_KEY\":\"CANARY_SECRET\"}\n";
  open();
  auto P = preview(1, "sse-utf8-metadata-v1");
  EXPECT_EQ(P.getAsObject()->getInteger("dispatched_sse_events"), 1);
  auto ID = field(commit(field(P, "preview_token")), "stream_capture_id");
  auto Page = records(ID);
  EXPECT_EQ(Page.getAsObject()->getArray("items")->size(), 2u);
  EXPECT_EQ(field((*Page.getAsObject()->getArray("items"))[1], "kind"),
            "sse_incomplete");
  P = preview(2, "diagnostic-lines-v1");
  ID = field(commit(field(P, "preview_token")), "stream_capture_id");
  Page = records(ID);
  EXPECT_EQ(field((*Page.getAsObject()->getArray("items"))[0], "kind"),
            "opaque_line");
}

#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebStreamSDK, CLIReceiptBindsBytesProfileAndPolicyWithoutExternalTools) {
  const auto Path = Root.string(), Out = Path + ".out", Err = Path + ".err";
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
    EXPECT_EQ(Output.find("CANARY"), std::string::npos);
    EXPECT_EQ(Errors.find("CANARY"), std::string::npos);
    return Status;
  };
  ASSERT_EQ(Run({"stream-preview", Path, Profile, "1"}), 0);
  const auto P = parse(Output);
  const auto Policy = field(P, "redaction_policy");
  const auto Receipt = field(P, "redaction_receipt");
  EXPECT_EQ(Run({"stream-import", Path, Profile, "1"}), 2);
  EXPECT_EQ(
      Run({"stream-import", Path, "jsonl-metadata-v1", "1", Policy, Receipt}),
      1);
  EXPECT_EQ(Output.find("record_id"), std::string::npos);
  EXPECT_EQ(
      Run({"stream-import", Path, Profile, "1", "CANARY_POLICY", Receipt}), 1);
  ASSERT_EQ(Run({"stream-import", Path, Profile, "1", Policy, Receipt}), 0);
  EXPECT_NE(Output.find("peer_record_id"), std::string::npos);
  std::ofstream(Root / "a0.jsonl", std::ios::app) << " ";
  EXPECT_EQ(Run({"stream-import", Path, Profile, "1", Policy, Receipt}), 1);
  EXPECT_NE(Errors.find("stream_preview_receipt_mismatch"), std::string::npos);
}
#endif
} // namespace
