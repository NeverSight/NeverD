//===- PackageIntegritySDKTests.cpp - Bound verification API tests -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Registry and lock declarations, revision binding, redaction and CLI parity.
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
class WebPackageIntegritySDK : public ::testing::Test {
protected:
  std::filesystem::path Root;
  neverd_web_session_t S = nullptr;
  std::string Revision, Original, Registry, Lock, Analysis, Package;
  static constexpr const char *SRI =
      "sha256-ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=";
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX capture required";
#endif
    llvm::SmallString<128> Dir;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-integrity-sdk", Dir));
    Root = Dir.str().str();
    std::ofstream(Root / "a.bin", std::ios::binary) << "abc";
    std::ofstream(Root / "b.json") << "{\"name\":\"CANARY_NAME\",\"dist\":{"
                                      "\"tarball\":\"https://CANARY_URL\","
                                      "\"integrity\":\""
                                   << SRI << "\"}}";
    std::ofstream Out(Root / "c.lock");
    Out << R"({"lockfileVersion":3,"packages":{"":{"name":"CANARY_ROOT"})";
    for (unsigned I = 0; I < 18; ++I)
      Out << ",\"node_modules/p" << I << "\":{\"integrity\":\"" << SRI << "\"}";
    Out << "}}";
    Out.close();
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
    const auto Token = field(P, "preview_token");
    Revision = field(
        take(neverd_web_import_commit_json(S, Token.data(), Token.size())),
        "revision");
    const auto Page = take(
        neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 0, 4));
    const auto &Items = *Page.getAsObject()->getArray("items");
    ASSERT_EQ(Items.size(), 4);
    Original = field(Items[1], "artifact_id");
    Registry = field(Items[2], "artifact_id");
    Lock = field(Items[3], "artifact_id");
  }
  Value verify(const std::string &Declaration, const std::string &PID = {}) {
    return take(neverd_web_package_integrity_verify_json(
        S, Revision.data(), Revision.size(), Original.data(), Original.size(),
        Declaration.data(), Declaration.size(), PID.data(), PID.size()));
  }
  Value packages() {
    const auto G = take(neverd_web_packages_analyze_json(
        S, Revision.data(), Revision.size(), Lock.data(), Lock.size(),
        "npm-lock", 8));
    EXPECT_EQ(field(G, "status"), "ok");
    Analysis = field(G, "package_analysis_id");
    return take(neverd_web_package_records_json(
        S, Revision.data(), Revision.size(), Analysis.data(), Analysis.size(),
        "packages", 8, 0, 512));
  }
};

TEST_F(WebPackageIntegritySDK,
       RegistryAndLockBindCapturedOriginalsWithoutDisclosure) {
  const auto R = verify(Registry);
  ASSERT_EQ(field(R, "status"), "ok");
  EXPECT_EQ(field(R, "integrity_status"), "match");
  EXPECT_EQ(field(R, "byte_domain"), "selected_original_artifact");
  EXPECT_EQ(field(R, "declaration_artifact_id"), Registry);
  EXPECT_EQ(field(R, "safety_verdict"), "not_assessed");
  EXPECT_FALSE(
      R.getAsObject()->getBoolean("authenticates_publisher").value_or(true));
  EXPECT_EQ(field(verify(Registry), "integrity_id"), field(R, "integrity_id"));
  const auto P = packages();
  const auto PID =
      field((*P.getAsObject()->getArray("items"))[1], "package_id");
  const auto L = verify(Analysis, PID);
  ASSERT_EQ(field(L, "integrity_status"), "match");
  EXPECT_EQ(field(L, "declaration_kind"), "lock_package");
  EXPECT_EQ(field(L, "declaration_artifact_id"), Lock);
  EXPECT_EQ(field(L, "declaration_selection_id"), PID);
  EXPECT_EQ(code(verify(Analysis, "CANARY_UNKNOWN")),
            "unknown_package_instance");
  EXPECT_EQ(code(take(neverd_web_package_integrity_verify_json(
                S, Revision.data(), Revision.size(), nullptr, 64,
                Registry.data(), Registry.size(), nullptr, 0))),
            "invalid_buffer");
  const auto Old = Revision, OldAnalysis = Analysis;
  open();
  EXPECT_EQ(code(take(neverd_web_package_integrity_verify_json(
                S, Old.data(), Old.size(), Original.data(), Original.size(),
                Registry.data(), Registry.size(), nullptr, 0))),
            "stale_revision");
  EXPECT_EQ(code(verify(OldAnalysis, PID)), "unknown_package_analysis");
  EXPECT_EQ(field(verify(Registry), "integrity_status"), "match");
}

TEST_F(WebPackageIntegritySDK,
       CacheIsBoundedAndReimportDiscardsPriorVerifications) {
  const auto P = packages();
  const auto &Items = *P.getAsObject()->getArray("items");
  ASSERT_EQ(Items.size(), 19);
  for (unsigned I = 1; I <= 16; ++I) {
    const auto PID = field(Items[I], "package_id");
    EXPECT_EQ(field(verify(Analysis, PID), "integrity_status"), "match");
  }
  EXPECT_EQ(code(verify(Analysis, field(Items[17], "package_id"))),
            "package_integrity_cache_budget_exceeded");
  EXPECT_EQ(field(verify(Analysis, field(Items[1], "package_id")),
                  "integrity_status"),
            "match");
  open();
  EXPECT_EQ(field(verify(Registry), "integrity_status"), "match");
}

#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebPackageIntegritySDK,
       CLIMatchesBothDeclarationKindsWithoutExternalTools) {
  const auto Path = Root.string();
  const auto Out = Path + ".stdout", Err = Path + ".stderr";
  const llvm::StringRef Env[] = {"PATH=/neverd-no-external-tools", "LC_ALL=C"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Out, Err};
  for (const bool Lock : {false, true}) {
    llvm::SmallVector<llvm::StringRef> Args{NEVERD_WEB_TEST_CLI,
                                            "web",
                                            "integrity",
                                            Path,
                                            "1",
                                            Lock ? "npm-lock" : "registry-dist",
                                            Lock ? "3" : "2"};
    if (Lock)
      Args.push_back("1");
    ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Env,
                                        Redirects, 30),
              0);
    std::ifstream F(Out), E(Err);
    const std::string Text{std::istreambuf_iterator<char>(F), {}};
    const std::string Errors{std::istreambuf_iterator<char>(E), {}};
    EXPECT_EQ(Text.find("CANARY"), std::string::npos);
    EXPECT_EQ(Errors.find("CANARY"), std::string::npos);
    EXPECT_NE(Text.find("\"integrity_status\":\"match\""), std::string::npos);
  }
  std::ofstream(Root / "a.bin", std::ios::binary) << "changed";
  const llvm::StringRef Args[]{
      NEVERD_WEB_TEST_CLI, "web", "integrity", Path, "1", "registry-dist", "2"};
  EXPECT_EQ(
      llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Env, Redirects, 30),
      1);
}
#endif
} // namespace
