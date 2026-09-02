//===- SafetyCLIContractTests.cpp - Safety command-line contracts --------===//

#include "../TestProcess.h"
#include "gtest/gtest.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#ifndef NEVERD_BINARY
#define NEVERD_BINARY "neverd"
#endif

#ifndef SAFETY_FIXTURE_ROOT
#define SAFETY_FIXTURE_ROOT ""
#endif

namespace {

namespace fs = std::filesystem;

class SafetyCLI : public ::testing::Test {
protected:
  struct ProcessResult {
    int ExitCode = -1;
    std::string StandardOutput;
    std::string StandardError;
  };

  void SetUp() override {
    Directory =
        fs::temp_directory_path() /
        ("neverd-safety-cli-" +
         std::to_string(neverd::test::currentProcessId()) + "-" +
         ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::create_directories(Directory);
  }

  void TearDown() override {
    std::error_code Error;
    fs::remove_all(Directory, Error);
  }

  ProcessResult run(llvm::StringRef Arguments) const {
    const fs::path StandardOutput = Directory / "stdout.json";
    const fs::path StandardError = Directory / "stderr.txt";
    const std::string Command =
        neverd::test::shellQuote(NEVERD_BINARY) + " " + Arguments.str() +
        neverd::test::redirectOutput(StandardOutput.string(),
                                     StandardError.string());
    ProcessResult Result;
    Result.ExitCode =
        neverd::test::systemExitCode(neverd::test::runShellCommand(Command));
    Result.StandardOutput = read(StandardOutput);
    Result.StandardError = read(StandardError);
    return Result;
  }

  static std::string fixture() {
#ifdef _WIN32
    return std::string(SAFETY_FIXTURE_ROOT) + "/safety_cases_pe_x64.exe";
#elif defined(__APPLE__)
    return std::string(SAFETY_FIXTURE_ROOT) + "/safety_cases_macho_x64";
#else
    return std::string(SAFETY_FIXTURE_ROOT) + "/safety_cases_elf_x64";
#endif
  }

  static llvm::json::Object parseReport(const ProcessResult &Result) {
    llvm::Expected<llvm::json::Value> Parsed =
        llvm::json::parse(Result.StandardOutput);
    EXPECT_TRUE(static_cast<bool>(Parsed)) << Result.StandardError;
    if (!Parsed)
      return {};
    const llvm::json::Object *Object = Parsed->getAsObject();
    EXPECT_NE(Object, nullptr);
    return Object ? std::move(*Object) : llvm::json::Object{};
  }

  static const llvm::json::Object *
  findingInFunction(const llvm::json::Object &Report, llvm::StringRef Name) {
    const llvm::json::Array *Findings = Report.getArray("findings");
    if (!Findings)
      return nullptr;
    for (const llvm::json::Value &Value : *Findings) {
      const llvm::json::Object *Finding = Value.getAsObject();
      if (!Finding)
        continue;
      const std::optional<llvm::StringRef> Function =
          Finding->getString("function");
      if (Function && (*Function == Name || *Function == ("_" + Name).str()))
        return Finding;
    }
    return nullptr;
  }

private:
  static std::string read(const fs::path &Path) {
    std::ifstream Stream(Path, std::ios::binary);
    return {std::istreambuf_iterator<char>(Stream),
            std::istreambuf_iterator<char>()};
  }

  fs::path Directory;
};

TEST_F(SafetyCLI, AcceptsInterproceduralBudgetsForAuditAndHunt) {
  for (llvm::StringRef Track :
       {llvm::StringRef("audit"), llvm::StringRef("hunt")}) {
    const ProcessResult Result = run(Track.str() +
                                     " --json --max-call-depth=1 "
                                     "--max-summary-iterations=1 " +
                                     neverd::test::shellQuote(fixture()));
    const llvm::json::Object Report = parseReport(Result);
    EXPECT_TRUE(Report.getBoolean("ok").value_or(false))
        << Result.StandardError;
  }
}

TEST_F(SafetyCLI, BudgetsSeparateControlDepthFromAttackerSummary) {
  const auto Hunt = [&](llvm::StringRef Options) {
    return parseReport(run("hunt --json " + Options.str() + " " +
                           neverd::test::shellQuote(fixture())));
  };
  const llvm::json::Object Default = Hunt("");
  const llvm::json::Object Depth = Hunt("--max-call-depth=1");
  const llvm::json::Object Summary = Hunt("--max-summary-iterations=1");

  const llvm::json::Object *DefaultFinding =
      findingInFunction(Default, "interproc_forward_inner");
  const llvm::json::Object *DepthFinding =
      findingInFunction(Depth, "interproc_forward_inner");
  const llvm::json::Object *SummaryFinding =
      findingInFunction(Summary, "interproc_forward_inner");
  ASSERT_NE(DefaultFinding, nullptr);
  ASSERT_NE(DepthFinding, nullptr);
  ASSERT_NE(SummaryFinding, nullptr);

  const llvm::json::Object *DefaultReach =
      DefaultFinding->getObject("reachability");
  const llvm::json::Object *DepthReach =
      DepthFinding->getObject("reachability");
  const llvm::json::Object *SummaryReach =
      SummaryFinding->getObject("reachability");
  ASSERT_NE(DefaultReach, nullptr);
  ASSERT_NE(DepthReach, nullptr);
  ASSERT_NE(SummaryReach, nullptr);
  EXPECT_EQ(DefaultReach->getString("status"), "REACHABLE");
  EXPECT_EQ(DefaultReach->getString("attacker_control"), "TAINTED");
  EXPECT_EQ(DepthReach->getString("status"), "UNKNOWN");
  EXPECT_EQ(DepthReach->getString("attacker_control"), "UNKNOWN");
  EXPECT_EQ(SummaryReach->getString("status"), "REACHABLE");
  EXPECT_EQ(SummaryReach->getString("attacker_control"), "UNKNOWN");
  EXPECT_TRUE(SummaryReach->getBoolean("budget_hit").value_or(false));
}

} // namespace
