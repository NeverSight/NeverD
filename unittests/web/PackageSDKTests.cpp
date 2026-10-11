//===- PackageSDKTests.cpp - Package tests -----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Package tests.
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
Value take(const char *Owned) {
  if (!Owned)
    throw std::runtime_error("missing owned response");
  std::string Text(Owned);
  neverd_free_string(Owned);
  EXPECT_EQ(Text.find("CANARY"), std::string::npos);
  auto Parsed = llvm::json::parse(Text);
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    throw std::runtime_error("invalid response");
  }
  return std::move(*Parsed);
}
std::string field(const Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
std::string code(const Value &V) {
  EXPECT_EQ(field(V, "status"), "error");
  return V.getAsObject()->getObject("error")->getString("code")->str();
}
class WebPackageSDK : public ::testing::Test {
protected:
  std::filesystem::path Root;
  neverd_web_session_t S = nullptr;
  void SetUp() override {
    S = neverd_web_session_create();
    ASSERT_NE(S, nullptr);
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-package-sdk", Directory));
    Root = Directory.str().str();
    std::filesystem::create_directory(Root / "before");
    std::filesystem::create_directory(Root / "after");
    std::ofstream(Root / "before/package.json")
        << R"({"name":"CANARY_PRIVATE_PACKAGE","version":"1","scripts":{"postinstall":"CANARY_COMMAND"},"dependencies":{"CANARY_DEP":"https://CANARY_TOKEN@example.invalid"}})";
    std::ofstream(Root / "after/package.json")
        << R"({"name":"CANARY_PRIVATE_PACKAGE","version":"2","scripts":{"postinstall":"CANARY_CHANGED_COMMAND"},"optionalDependencies":{"CANARY_DEP":"file:../CANARY_OUTSIDE"}})";
  }
  void TearDown() override {
    neverd_web_session_destroy(S);
    std::error_code EC;
    std::filesystem::remove(Root.string() + ".stdout", EC);
    std::filesystem::remove(Root.string() + ".stderr", EC);
    std::filesystem::remove_all(Root, EC);
  }
  std::string open() {
    const auto Path = Root.string();
    const auto P = take(neverd_web_import_preview_json(
        S, Path.data(), Path.size(), nullptr, 0));
    const auto Token = field(P, "preview_token");
    const auto M =
        take(neverd_web_import_commit_json(S, Token.data(), Token.size()));
    return field(M, "revision");
  }
  std::string artifact(const std::string &Revision, uint64_t Index) {
    const auto Page = take(neverd_web_artifacts_json(
        S, Revision.data(), Revision.size(), Index, 1));
    const auto &Items = *Page.getAsObject()->getArray("items");
    if (Items.size() != 1)
      throw std::runtime_error("missing artifact");
    return field(Items[0], "artifact_id");
  }
  Value analyze(const std::string &R, const std::string &ID) {
    return take(neverd_web_packages_analyze_json(
        S, R.data(), R.size(), ID.data(), ID.size(), "package-json", 12));
  }
};

#ifndef _WIN32
TEST_F(WebPackageSDK,
       QueriesAndDiffKeepPrivateValuesAndBindBothSidesToRevision) {
  const auto R = open();
  const auto Before = artifact(R, 4), After = artifact(R, 2);
  const auto B = analyze(R, Before), A = analyze(R, After);
  ASSERT_EQ(field(B, "status"), "ok");
  ASSERT_EQ(field(A, "status"), "ok");
  EXPECT_EQ(field(B, "safety_verdict"), "not_assessed");
  const auto BID = field(B, "package_analysis_id"),
             AID = field(A, "package_analysis_id");
  EXPECT_EQ(field(analyze(R, Before), "package_analysis_id"), BID);
  for (const std::string K :
       {"packages", "dependencies", "scripts", "entries", "files"}) {
    const auto Page = take(
        neverd_web_package_records_json(S, R.data(), R.size(), BID.data(),
                                        BID.size(), K.data(), K.size(), 0, 1));
    EXPECT_EQ(field(Page, "status"), "ok");
    EXPECT_EQ(field(Page, "record_kind"), K);
  }
  const auto D = take(neverd_web_packages_compare_json(
      S, R.data(), R.size(), BID.data(), BID.size(), AID.data(), AID.size()));
  ASSERT_EQ(field(D, "status"), "ok");
  EXPECT_GT(D.getAsObject()->getInteger("change_count").value_or(0), 0);
  const auto DID = field(D, "package_diff_id");
  const auto Page = take(neverd_web_package_diff_records_json(
      S, R.data(), R.size(), DID.data(), DID.size(), 0, 512));
  EXPECT_EQ(field(Page, "status"), "ok");
  EXPECT_TRUE(Page.getAsObject()->get("next_offset")->getAsNull().has_value());
  EXPECT_EQ(
      code(take(neverd_web_package_records_json(
          S, R.data(), R.size(), BID.data(), BID.size(), "packages", 8, 0, 0))),
      "invalid_page");
  EXPECT_EQ(code(take(neverd_web_package_records_json(
                S, R.data(), R.size(), BID.data(), BID.size(), "CANARY_KIND",
                11, 0, 1))),
            "invalid_record_kind");
  const auto NewR = open();
  EXPECT_EQ(code(take(neverd_web_package_diff_records_json(
                S, R.data(), R.size(), DID.data(), DID.size(), 0, 1))),
            "stale_revision");
  EXPECT_EQ(code(take(neverd_web_package_diff_records_json(
                S, NewR.data(), NewR.size(), DID.data(), DID.size(), 0, 1))),
            "unknown_package_diff");
  EXPECT_EQ(code(take(neverd_web_packages_compare_json(
                S, NewR.data(), NewR.size(), BID.data(), BID.size(), AID.data(),
                AID.size()))),
            "unknown_package_analysis");
}

TEST_F(WebPackageSDK, BadProfilesBuffersAndMalformedInputsDoNotPublishGraphs) {
  const auto R = open(), ID = artifact(R, 4);
  EXPECT_EQ(code(take(neverd_web_packages_analyze_json(
                S, R.data(), R.size(), ID.data(), ID.size(), "CANARY", 6))),
            "package_unsupported_input_profile");
  EXPECT_EQ(code(take(neverd_web_packages_analyze_json(
                S, R.data(), R.size(), nullptr, 64, "npm-lock", 8))),
            "invalid_buffer");
  EXPECT_EQ(code(take(neverd_web_packages_analyze_json(
                S, R.data(), R.size(), ID.data(), ID.size(), "npm-lock", 8))),
            "package_unsupported_lock_version");
  EXPECT_EQ(field(take(neverd_web_metadata_json(S)), "analysis_status"),
            "not_analyzed");
  EXPECT_EQ(field(analyze(R, ID), "status"), "ok");
  EXPECT_EQ(field(take(neverd_web_metadata_json(S)), "analysis_status"),
            "partial");
}

#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebPackageSDK,
       CLIInspectsAndComparesWithoutExternalExecutablesOrDisclosure) {
  const auto Input = (Root / "before/package.json").string();
  const auto Directory = Root.string();
  const auto Out = Root.string() + ".stdout", Err = Root.string() + ".stderr";
  const llvm::StringRef Env[] = {"PATH=/neverd-no-external-tools", "LC_ALL=C"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Out, Err};
  auto Run = [&](llvm::ArrayRef<llvm::StringRef> Args) {
    ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Env,
                                        Redirects, 30),
              0);
    std::ifstream F(Out), E(Err);
    const std::string Text{std::istreambuf_iterator<char>(F), {}};
    const std::string Errors{std::istreambuf_iterator<char>(E), {}};
    EXPECT_EQ(Text.find("CANARY"), std::string::npos);
    EXPECT_EQ(Errors.find("CANARY"), std::string::npos);
    EXPECT_NE(Text.find("package_analysis_id"), std::string::npos);
    EXPECT_NE(Text.find("not_assessed"), std::string::npos);
  };
  const llvm::StringRef Inspect[] = {NEVERD_WEB_TEST_CLI, "web", "packages",
                                     Input, "package-json"};
  Run(Inspect);
  // Keep output outside the selected directory: saved stdout must not shift
  // the artifact indices of the two immutable input roots.
  std::filesystem::remove(Out);
  std::filesystem::remove(Err);
  const llvm::StringRef Diff[] = {NEVERD_WEB_TEST_CLI,
                                  "web",
                                  "package-diff",
                                  Directory,
                                  "package-json",
                                  "4",
                                  "2"};
  Run(Diff);
}
#endif
#endif
} // namespace
