//===- DriverEmulationPublicTests.cpp - Public execution tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Public execution tests.
///
//===----------------------------------------------------------------------===//

#include "../TestProcess.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIEmulation.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <utility>

namespace {
#define NEVERD_DRIVER_PUBLIC_CLI_TEXT(Name, Value)                             \
  constexpr char Name[] = Value;
#include "DriverPublicCLICases.def"
#undef NEVERD_DRIVER_PUBLIC_CLI_TEXT

std::filesystem::path fixture(const char *Name) {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         (std::string(Name) + ".sys");
}

std::string takeString(const char *Text) {
  if (!Text)
    return {};
  std::string Copy(Text);
  neverd_free_string(Text);
  return Copy;
}

neverd_driver_options_v1 options() {
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  return Options;
}

class DriverEmulationPublic : public ::testing::Test {
protected:
  neverd_session_t Session = nullptr;
  std::filesystem::path Directory;

  void SetUp() override {
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
    neverd_session_set_debug_info_enabled(Session, 0);
    llvm::SmallString<128> Temporary;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-driver-public",
                                                      Temporary));
    Directory = Temporary.str().str();
  }

  void TearDown() override {
    neverd_session_destroy(Session);
    if (!Directory.empty()) {
      std::error_code Ignored;
      std::filesystem::remove_all(Directory, Ignored);
    }
  }

  void load(const char *Name) {
    ASSERT_EQ(neverd_session_load(Session, fixture(Name).string().c_str()), 1)
        << takeString(neverd_last_error(Session));
  }

  std::string lastError() { return takeString(neverd_last_error(Session)); }

  std::string runCLIPath(const std::filesystem::path &Path, const char *Limit,
                         int ExpectedExit, const char *Timeout = nullptr,
                         const char *Events = nullptr,
                         const char *Contract = nullptr,
                         const char *Scenario = nullptr) {
    const auto Out = Directory / "stdout.json";
    const auto Err = Directory / "stderr.txt";
    const auto ScenarioPath = Directory / "scenario.json";
    if (Scenario) {
      std::ofstream File(ScenarioPath);
      File << Scenario;
    }
    const std::string Command =
        neverd::test::shellQuote(NEVERD_DRIVER_CLI) + " emulate-driver " +
        neverd::test::shellQuote(Path.string()) + CompatibilityBackend +
        " --instruction-limit " + neverd::test::shellQuote(Limit) +
        (Timeout ? " --timeout-ms " + neverd::test::shellQuote(Timeout) : "") +
        (Events ? " --event-limit " + neverd::test::shellQuote(Events) : "") +
        (Contract
             ? " --execution-contract " + neverd::test::shellQuote(Contract)
             : "") +
        (Scenario
             ? " --scenario " + neverd::test::shellQuote(ScenarioPath.string())
             : "") +
        neverd::test::redirectOutput(Out.string(), Err.string());
    const int Exit =
        neverd::test::systemExitCode(neverd::test::runShellCommand(Command));
    std::ifstream ErrorFile(Err);
    const std::string Error((std::istreambuf_iterator<char>(ErrorFile)), {});
    EXPECT_EQ(Exit, ExpectedExit) << Error;
    std::ifstream OutputFile(Out);
    return std::string((std::istreambuf_iterator<char>(OutputFile)), {});
  }

  std::string runCLI(const char *Name, const char *Limit, int ExpectedExit,
                     const char *Contract = nullptr) {
    return runCLIPath(fixture(Name), Limit, ExpectedExit, nullptr, nullptr,
                      Contract);
  }
};

TEST_F(DriverEmulationPublic, BugCheckBorrowedNameIsVisibleThroughCAPIAndCLI) {
  auto O = options();
  const char *Scenario =
      R"({"windows_version":{"major":10,"minor":0,"build":19041}})";
  auto CAPI = llvm::json::parse(takeString(neverd_emulate_driver_backend_json(
      Session, fixture("driver_bugcheck_borrowed_name").string().c_str(),
      Scenario, &O, "unicorn", "checked-x64-v1")));
  ASSERT_TRUE(bool(CAPI)) << llvm::toString(CAPI.takeError()) << lastError();
  auto CLI = llvm::json::parse(
      runCLIPath(fixture("driver_bugcheck_borrowed_name"), "10000", 0, nullptr,
                 nullptr, "checked-x64-v1", Scenario));
  ASSERT_TRUE(bool(CLI)) << llvm::toString(CLI.takeError());
  for (auto *Report : {CAPI->getAsObject(), CLI->getAsObject()}) {
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getInteger("nt_status"), 0);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    auto *Callbacks = Report->getArray("bugcheck_callbacks");
    ASSERT_NE(Callbacks, nullptr);
    ASSERT_EQ(Callbacks->size(), 1u);
    EXPECT_EQ(
        (*Callbacks)[0].getAsObject()->getBoolean("component_storage_released"),
        true);
  }
}

TEST_F(DriverEmulationPublic, RejectsNullAndUnloadedSessions) {
  EXPECT_EQ(neverd_emulate_driver_json(nullptr, nullptr, nullptr), nullptr);
  EXPECT_EQ(neverd_emulate_driver_json(Session, nullptr, nullptr), nullptr);
  EXPECT_NE(lastError().find("loaded session"), std::string::npos);
}

TEST_F(DriverEmulationPublic, ExplicitPathUsesFreshUnloadedSession) {
  EXPECT_EQ(neverd_emulate_driver_json(Session, "", nullptr), nullptr);
  EXPECT_NE(lastError().find("path must not be empty"), std::string::npos);
  auto Parsed = llvm::json::parse(takeString(neverd_emulate_driver_json(
      Session, fixture("success").string().c_str(), nullptr)));
  ASSERT_TRUE(static_cast<bool>(Parsed))
      << llvm::toString(Parsed.takeError()) << " " << lastError();
  auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getString("stop_reason"), "returned");
  EXPECT_EQ(Report->getBoolean("nt_success"), true);
  EXPECT_FALSE(neverd_session_is_loaded(Session));
  EXPECT_TRUE(lastError().empty());
}

TEST_F(DriverEmulationPublic, RejectsInvalidOptionsAndClearsEarlierErrors) {
  load("success");
  auto Options = options();
  --Options.struct_size;
  EXPECT_EQ(neverd_emulate_driver_json(Session, nullptr, &Options), nullptr);
  EXPECT_NE(lastError().find("struct_size"), std::string::npos);
  Options = options();
  for (auto *Budget : {&Options.instruction_limit, &Options.memory_limit,
                       &Options.event_limit, &Options.timeout_milliseconds}) {
    const auto Previous = *Budget;
    *Budget = 0;
    EXPECT_EQ(neverd_emulate_driver_json(Session, nullptr, &Options), nullptr);
    EXPECT_NE(lastError().find("positive"), std::string::npos);
    *Budget = Previous;
  }
  auto Report =
      takeString(neverd_emulate_driver_json(Session, nullptr, nullptr));
  EXPECT_FALSE(Report.empty()) << lastError();
  EXPECT_TRUE(lastError().empty());
}

TEST_F(DriverEmulationPublic, NullOptionsReturnOwnedSuccessJSON) {
  // A session's analysis restriction must not restrict the execution image.
  neverd_session_restrict_function(Session, 0x180001000ULL);
  load("success");
  auto Parsed = llvm::json::parse(
      takeString(neverd_emulate_driver_json(Session, nullptr, nullptr)));
  ASSERT_TRUE(static_cast<bool>(Parsed))
      << llvm::toString(Parsed.takeError()) << " " << lastError();
  auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getString("stop_reason"), "returned");
  EXPECT_EQ(Report->getInteger("nt_status"), 0);
  EXPECT_EQ(Report->getBoolean("nt_success"), true);
  EXPECT_TRUE(neverd_session_is_loaded(Session));
}

TEST_F(DriverEmulationPublic, FailingStatusIsAReportWithNoSessionError) {
  load("failure");
  auto Parsed = llvm::json::parse(
      takeString(neverd_emulate_driver_json(Session, nullptr, nullptr)));
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getString("stop_reason"), "returned");
  EXPECT_EQ(Report->getInteger("nt_status"), 0xc0000001LL);
  EXPECT_EQ(Report->getBoolean("nt_success"), false);
  EXPECT_TRUE(lastError().empty());
}

TEST_F(DriverEmulationPublic, ReportsExactInstructionBudgetWithoutNTStatus) {
  load("loop");
  auto Options = options();
  Options.instruction_limit = 50;
  auto Parsed = llvm::json::parse(
      takeString(neverd_emulate_driver_json(Session, nullptr, &Options)));
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getString("stop_reason"), "instruction_limit");
  EXPECT_EQ(Report->getInteger("instructions"), 50);
  EXPECT_FALSE(Report->getInteger("nt_status"));
  EXPECT_TRUE(lastError().empty());
}

TEST_F(DriverEmulationPublic, ReopensSessionFileAndPreservesSessionOnFailure) {
  const auto Copy = Directory / "removed.sys";
  ASSERT_TRUE(std::filesystem::copy_file(fixture("success"), Copy));
  ASSERT_EQ(neverd_session_load(Session, Copy.string().c_str()), 1);
  ASSERT_TRUE(std::filesystem::remove(Copy));
  EXPECT_EQ(neverd_emulate_driver_json(Session, nullptr, nullptr), nullptr);
  EXPECT_FALSE(lastError().empty());
  EXPECT_TRUE(neverd_session_is_loaded(Session));
}

TEST_F(DriverEmulationPublic, CLISeparatesSuccessFailureAndIncomplete) {
  for (auto [Name, Exit] : {std::pair{"success", 0}, std::pair{"failure", 2},
                            std::pair{"unknown", 3}}) {
    SCOPED_TRACE(Name);
    auto Parsed = llvm::json::parse(runCLI(Name, "100000", Exit));
    ASSERT_TRUE(static_cast<bool>(Parsed))
        << llvm::toString(Parsed.takeError());
    ASSERT_NE(Parsed->getAsObject(), nullptr);
    EXPECT_EQ(Parsed->getAsObject()->getString("stop_reason"),
              Exit == 3 ? "unsupported_api" : "returned");
  }
}

TEST_F(DriverEmulationPublic, CLIRejectsZeroBudgetBeforeExecution) {
  EXPECT_TRUE(runCLI("success", "0", 1).empty());
}

TEST_F(DriverEmulationPublic,
       CLIHonorsExplicitDriverTimeoutAndRejectsInvalidLimits) {
  for (const char *Value : {"0", "18446744073709551615"})
    EXPECT_TRUE(runCLIPath(fixture("success"), "100000", 1, Value).empty());
  auto Returned =
      llvm::json::parse(runCLIPath(fixture("success"), "100000", 0, "30000"));
  ASSERT_TRUE(bool(Returned)) << llvm::toString(Returned.takeError());
  const auto *Configuration =
      Returned->getAsObject()->getObject("configuration");
  ASSERT_NE(Configuration, nullptr);
  EXPECT_EQ(Configuration->getInteger("timeout_milliseconds"), 30000);
  // Isolate the timeout budget: a delayed backend timer must not let the
  // default 10,000 recorded loop writes exhaust the event budget first.
  auto Timed = llvm::json::parse(
      runCLIPath(fixture("loop"), "1000000000", 3, "1", "16777216"));
  ASSERT_TRUE(bool(Timed)) << llvm::toString(Timed.takeError());
  EXPECT_EQ(Timed->getAsObject()->getString("stop_reason"), "timeout");
}

TEST_F(DriverEmulationPublic,
       CLIHonorsEventBudgetAndRejectsZeroBeforeExecution) {
  EXPECT_TRUE(
      runCLIPath(fixture("success"), "100000", 1, nullptr, "0").empty());
  EXPECT_TRUE(
      runCLIPath(fixture("success"), "100000", 1, nullptr, "16777217").empty());
  auto Limited = llvm::json::parse(
      runCLIPath(fixture("success"), "100000", 3, nullptr, "1"));
  ASSERT_TRUE(bool(Limited)) << llvm::toString(Limited.takeError());
  EXPECT_EQ(Limited->getAsObject()->getString("stop_reason"), "event_limit");
  EXPECT_EQ(Limited->getAsObject()
                ->getObject("configuration")
                ->getInteger("event_limit"),
            1);
  auto Returned = llvm::json::parse(
      runCLIPath(fixture("success"), "100000", 0, nullptr, "16777216"));
  ASSERT_TRUE(bool(Returned)) << llvm::toString(Returned.takeError());
  EXPECT_EQ(Returned->getAsObject()
                ->getObject("configuration")
                ->getInteger("event_limit"),
            16777216);
}
TEST_F(DriverEmulationPublic, CLIRejectsMalformedImageAtStrictLoadBoundary) {
  const auto Path = Directory / "malformed.sys";
  {
    std::ofstream Output(Path, std::ios::binary);
    Output.write("MZ\0\0", 4);
    ASSERT_TRUE(Output);
  }
  EXPECT_TRUE(runCLIPath(Path, "100000", 1).empty());
  std::ifstream ErrorFile(Directory / "stderr.txt");
  const std::string Error((std::istreambuf_iterator<char>(ErrorFile)), {});
  EXPECT_FALSE(Error.empty());
}

} // namespace
