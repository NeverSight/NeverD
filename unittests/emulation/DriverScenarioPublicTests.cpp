//===- DriverScenarioPublicTests.cpp - Public scenario tests --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Tests bounded scenario input across the C ABI and CLI.
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

namespace {

std::string takeString(const char *Text) {
  if (!Text)
    return {};
  std::string Copy(Text);
  neverd_free_string(Text);
  return Copy;
}

class DriverScenarioPublic : public ::testing::Test {
protected:
  neverd_session_t Session = nullptr;
  std::filesystem::path Directory;

  void SetUp() override {
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
    llvm::SmallString<128> Temporary;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-scenario-public",
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

  std::string fixture(const char *Name) {
    return (std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
            (std::string(Name) + ".sys"))
        .string();
  }

  std::string error() { return takeString(neverd_last_error(Session)); }

  std::string runCLI(const std::string &Scenario, int ExpectedExit,
                     const char *Name = "success",
                     const char *Image = nullptr) {
    const auto Path = Directory / "scenario.json";
    const auto Out = Directory / "output.json";
    const auto Err = Directory / "error.txt";
    {
      std::ofstream File(Path, std::ios::binary);
      File.write(Scenario.data(), Scenario.size());
    }
    const std::string Command =
        neverd::test::shellQuote(NEVERD_DRIVER_CLI) + " emulate-driver " +
        neverd::test::shellQuote(Image ? Image : fixture(Name)) +
        " --scenario " + neverd::test::shellQuote(Path.string()) +
        neverd::test::redirectOutput(Out.string(), Err.string());
    int Exit =
        neverd::test::systemExitCode(neverd::test::runShellCommand(Command));
    std::ifstream Errors(Err);
    EXPECT_EQ(Exit, ExpectedExit)
        << std::string(std::istreambuf_iterator<char>(Errors), {});
    std::ifstream Output(Out);
    return std::string(std::istreambuf_iterator<char>(Output), {});
  }
};

TEST_F(DriverScenarioPublic, EmptyScenarioPreservesInitializationOnlyBehavior) {
  auto Parsed =
      llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
          Session, fixture("success").c_str(), "{}", nullptr)));
  ASSERT_TRUE(static_cast<bool>(Parsed))
      << llvm::toString(Parsed.takeError()) << error();
  const auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getString("stop_reason"), "returned");
  EXPECT_EQ(Report->getBoolean("nt_success"), true);
  EXPECT_EQ(Report->getBoolean("scenario_success"), true);
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
}

TEST_F(DriverScenarioPublic,
       RejectsNullMalformedAndOversizedScenarioBeforeLoad) {
  EXPECT_EQ(neverd_emulate_driver_scenario_json(Session, "missing.sys", nullptr,
                                                nullptr),
            nullptr);
  EXPECT_NE(error().find("required"), std::string::npos);
  for (
      const std::string &JSON :
      {std::string("{"), std::string(R"({"typo":true})"),
       std::string(
           R"({"requests":[{"kind":"read","byte_offset":"0x7fffffffffffffff","output_size":1}]})"),
       std::string(
           R"({"requests":[{"kind":"write","byte_offset":9223372036854775807,"input":"ab"}]})"),
       std::string(
           R"({"requests":[{"kind":"ioctl","code":0,"direct_input":"aa","output_size":1}]})"),
       std::string(NEVERD_DRIVER_SCENARIO_JSON_LIMIT + 1, ' ')}) {
    SCOPED_TRACE(JSON.size() > 256 ? "oversized scenario" : JSON);
    EXPECT_EQ(neverd_emulate_driver_scenario_json(Session, "missing.sys",
                                                  JSON.c_str(), nullptr),
              nullptr);
    EXPECT_NE(error().find("scenario"), std::string::npos);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
}

TEST_F(DriverScenarioPublic, CLIParsesScenarioAndKeepsJSONOutput) {
  auto Parsed =
      llvm::json::parse(runCLI(R"({"requests":[],"unload":false})", 0));
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_NE(Parsed->getAsObject(), nullptr);
  EXPECT_EQ(Parsed->getAsObject()->getBoolean("scenario_success"), true);
}

TEST_F(DriverScenarioPublic, CLIRejectsUnknownFieldsNULAndOversizedFiles) {
  EXPECT_TRUE(runCLI(R"({"typo":true})", 1).empty());
  EXPECT_TRUE(runCLI(std::string("{}\0ignored", 10), 1).empty());
  EXPECT_TRUE(runCLI(std::string(NEVERD_DRIVER_SCENARIO_JSON_LIMIT + 1, ' '), 1)
                  .empty());
}

constexpr const char LifecycleScenario[] = R"({
  "requests":[
    {"kind":"create","device":"\\Device\\NeverDIO"},
    {"kind":"ioctl","code":"0x222000","input":"00112233","output_size":4},
    {"kind":"cleanup"}, {"kind":"close"}
  ],
  "unload":true
})";

TEST_F(DriverScenarioPublic, CAPICompletesBufferedIOAndUnloads) {
  auto Parsed =
      llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
          Session, NEVERD_DRIVER_IO_FIXTURE, LifecycleScenario, nullptr)));
  ASSERT_TRUE(static_cast<bool>(Parsed))
      << llvm::toString(Parsed.takeError()) << error();
  const auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getString("stop_reason"), "returned");
  EXPECT_EQ(Report->getBoolean("scenario_success"), true);
  EXPECT_EQ(Report->getBoolean("unload_completed"), true);
  const auto *Requests = Report->getArray("requests");
  ASSERT_NE(Requests, nullptr);
  ASSERT_EQ(Requests->size(), 4u);
  const auto *IO = (*Requests)[1].getAsObject();
  ASSERT_NE(IO, nullptr);
  EXPECT_EQ(IO->getString("kind"), "ioctl");
  EXPECT_EQ(IO->getBoolean("completed"), true);
  EXPECT_EQ(IO->getInteger("io_status"), 0);
  EXPECT_EQ(IO->getString("output_hex"), "5a4b7869");
}

TEST_F(DriverScenarioPublic, CLIRunsOrderedLifecycleScenario) {
  auto Parsed = llvm::json::parse(
      runCLI(LifecycleScenario, 0, "success", NEVERD_DRIVER_IO_FIXTURE));
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_NE(Parsed->getAsObject(), nullptr);
  EXPECT_EQ(Parsed->getAsObject()->getBoolean("scenario_success"), true);
  EXPECT_EQ(Parsed->getAsObject()->getBoolean("unload_completed"), true);
}

TEST_F(DriverScenarioPublic, CAPIAndCLIExecuteDirectBuffersWithFileIdentity) {
  const char *Scenario = R"({"requests":[
    {"kind":"create","device":"\\Device\\NeverDDirect","file":7},
    {"kind":"ioctl","file":7,"code":"0x222002","input":"10",
     "direct_input":"010203","output_size":5},
    {"kind":"cleanup","file":7},{"kind":"close","file":7}
  ],"unload":true})";
  for (bool CLI : {false, true}) {
    auto Parsed =
        llvm::json::parse(CLI ? runCLI(Scenario, 0, "driver_direct")
                              : takeString(neverd_emulate_driver_scenario_json(
                                    Session, fixture("driver_direct").c_str(),
                                    Scenario, nullptr)));
    ASSERT_TRUE(static_cast<bool>(Parsed))
        << llvm::toString(Parsed.takeError());
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("profile"), "wdm-x64-synchronous-v2");
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 4u);
    const auto *IO = (*Requests)[1].getAsObject();
    ASSERT_NE(IO, nullptr);
    EXPECT_EQ(IO->getInteger("file"), 7);
    EXPECT_EQ(IO->getString("output_hex"), "1112131010");
  }
}

TEST_F(DriverScenarioPublic, ExplicitExportAbsenceControlsTheGuestBranch) {
  auto Parsed = llvm::json::parse(runCLI(
      R"({"kernel_exports":{"ExAllocatePool2":false}})", 2, "driver_runtime"));
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  const auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getString("stop_reason"), "returned");
  EXPECT_EQ(Report->getInteger("nt_status"), 0xc000000dULL);
  const auto *Configuration = Report->getObject("configuration");
  ASSERT_NE(Configuration, nullptr);
  const auto *Exports = Configuration->getObject("kernel_exports");
  ASSERT_NE(Exports, nullptr);
  EXPECT_EQ(Exports->getBoolean("ExAllocatePool2"), false);
  const auto *Calls = Report->getArray("calls");
  ASSERT_NE(Calls, nullptr);
  ASSERT_EQ(Calls->size(), 2u);
  EXPECT_EQ((*Calls)[1].getAsObject()->getString("name"),
            "MmGetSystemRoutineAddress");
  EXPECT_EQ((*Calls)[1].getAsObject()->getString("result"), "0x0");
}

TEST_F(DriverScenarioPublic, CLIPreservesTheFirstStructuredMemoryFault) {
  auto Parsed = llvm::json::parse(runCLI("{}", 3, "fault"));
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  const auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getString("stop_reason"), "memory_fault");
  const auto *Fault = Report->getObject("fault");
  ASSERT_NE(Fault, nullptr);
  EXPECT_EQ(Fault->getString("kind"), "unmapped_memory");
  EXPECT_EQ(Fault->getString("pc"), Report->getString("pc"));
  EXPECT_EQ(Fault->getString("address"), "0x12345000");
  EXPECT_EQ(Fault->getString("access"), "write");
  EXPECT_EQ(Fault->getInteger("size"), 8);
  ASSERT_NE(Fault->get("interrupt"), nullptr);
  EXPECT_EQ(Fault->get("interrupt")->kind(), llvm::json::Value::Null);
}

} // namespace
