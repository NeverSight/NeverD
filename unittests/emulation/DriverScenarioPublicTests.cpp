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
  for (const std::string &JSON :
       {std::string("{"), std::string(R"({"typo":true})"),
        std::string(NEVERD_DRIVER_SCENARIO_JSON_LIMIT + 1, ' ')}) {
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

} // namespace
