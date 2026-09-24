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

#include "neverd/emulation/DriverProfile.h"
#include "neverd/sdk/NeverDCAPIEmulation.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

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

TEST_F(DriverScenarioPublic,
       CAPIAndCLIRejectMalformedCancellationBeforeImageLoading) {
  for (
      const char *Scenario :
      {R"({"requests":[{"kind":"read","cancel_after_100ns":-1}]})",
       R"({"requests":[{"kind":"read","cancel_after_100ns":1.0}]})",
       R"({"requests":[{"kind":"read","cancel_after_100ns":1e0}]})",
       R"({"requests":[{"kind":"read","cancel_after_100ns":true}]})",
       R"({"requests":[{"kind":"read","cancel_after_100ns":null}]})",
       R"({"requests":[{"kind":"read","cancel_after_100ns":"0"}]})",
       R"({"requests":[{"kind":"read","cancel_after_100ns":9223372036854775808}]})",
       R"({"requests":[{"kind":"read","cancel_after_100ns":18446744073709551616}]})",
       R"({"requests":[{"kind":"create","cancel_after_100ns":0}]})",
       R"({"requests":[{"kind":"cleanup","cancel_after_100ns":0}]})",
       R"({"requests":[{"kind":"close","cancel_after_100ns":0}]})",
       R"({"cancel_after_100ns":0})",
       R"({"requests":[{"kind":"read","cancel_requested_at_100ns":0}]})",
       R"({"requests":[{"kind":"read","cancel_after_100ns":0,"cancel_after_100n\u0073":1}]})"}) {
    SCOPED_TRACE(Scenario);
    EXPECT_EQ(
        neverd_emulate_driver_scenario_json(
            Session, "missing-cancellation-public.sys", Scenario, nullptr),
        nullptr);
    const auto Diagnostic = error();
    EXPECT_NE(Diagnostic.find("driver scenario:"), std::string::npos);
    EXPECT_NE(Diagnostic.find("cancel_"), std::string::npos);
    EXPECT_FALSE(neverd_session_is_loaded(Session));
    EXPECT_TRUE(
        runCLI(Scenario, 1, "success", "missing-cancellation-public.sys")
            .empty());
    std::ifstream Errors(Directory / "error.txt");
    const std::string CLIDiagnostic(std::istreambuf_iterator<char>(Errors), {});
    EXPECT_NE(CLIDiagnostic.find("driver scenario:"), std::string::npos);
    EXPECT_NE(CLIDiagnostic.find("cancel_"), std::string::npos);
  }
}

TEST_F(DriverScenarioPublic,
       CAPIAndCLIAcceptCancellationBoundariesBeforeFailedInitialization) {
  constexpr char Scenario[] = R"({"requests":[
    {"kind":"read","cancel_after_100ns":0},
    {"kind":"write","cancel_after_100ns":1},
    {"kind":"ioctl","code":0,"cancel_after_100ns":9223372036854775807}
  ]})";
  for (bool CLI : {false, true}) {
    SCOPED_TRACE(CLI);
    auto Parsed = llvm::json::parse(
        CLI ? runCLI(Scenario, 2, "failure")
            : takeString(neverd_emulate_driver_scenario_json(
                  Session, fixture("failure").c_str(), Scenario, nullptr)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("stop_reason"), "returned");
    EXPECT_EQ(Report->getBoolean("nt_success"), false);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    EXPECT_TRUE(Requests->empty());
    EXPECT_TRUE(error().empty());
  }
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

TEST_F(DriverScenarioPublic,
       CAPIAndCLIReportNullCancellationForUnconfiguredRequests) {
  for (bool CLI : {false, true}) {
    SCOPED_TRACE(CLI);
    auto Parsed = llvm::json::parse(
        CLI ? runCLI(LifecycleScenario, 0, "success", NEVERD_DRIVER_IO_FIXTURE)
            : takeString(neverd_emulate_driver_scenario_json(
                  Session, NEVERD_DRIVER_IO_FIXTURE, LifecycleScenario,
                  nullptr)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 4u);
    for (const auto &Value : *Requests) {
      const auto *Request = Value.getAsObject();
      ASSERT_NE(Request, nullptr);
      const auto *Time = Request->get("cancel_requested_at_100ns");
      ASSERT_NE(Time, nullptr);
      EXPECT_EQ(Time->kind(), llvm::json::Value::Null);
    }
  }
}

TEST_F(DriverScenarioPublic,
       CAPIAndCLICompleteWDMIoctlBeforeItsCancellationDeadline) {
  for (const char *Delay : {"0", "17"}) {
    SCOPED_TRACE(Delay);
    const std::string Scenario = std::string(R"({"requests":[
          {"kind":"create","device":"\\Device\\NeverDIO"},
          {"kind":"ioctl","code":"0x222000","input":"00112233",
           "output_size":4,"cancel_after_100ns":)") +
                                 Delay + "}]}";
    for (bool CLI : {false, true}) {
      SCOPED_TRACE(CLI);
      auto Parsed = llvm::json::parse(
          CLI ? runCLI(Scenario, 0, "success", NEVERD_DRIVER_IO_FIXTURE)
              : takeString(neverd_emulate_driver_scenario_json(
                    Session, NEVERD_DRIVER_IO_FIXTURE, Scenario.c_str(),
                    nullptr)));
      ASSERT_TRUE(bool(Parsed))
          << llvm::toString(Parsed.takeError()) << error();
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("stop_reason"), "returned");
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 2u);
      const auto *IO = (*Requests)[1].getAsObject();
      ASSERT_NE(IO, nullptr);
      EXPECT_EQ(IO->getBoolean("completed"), true);
      EXPECT_EQ(IO->getInteger("io_status"), 0);
      EXPECT_EQ(IO->getString("output_hex"), "5a4b7869");
      const auto *Time = IO->get("cancel_requested_at_100ns");
      ASSERT_NE(Time, nullptr);
      EXPECT_EQ(Time->kind(), llvm::json::Value::Null);
      EXPECT_EQ(IO->getInteger("dispatch_status"), 0);
    }
  }
}

TEST_F(DriverScenarioPublic, CAPIAndCLICompletePendingWorkItemRequests) {
  for (bool UseCLI : {false, true}) {
    const auto Output = UseCLI ? runCLI(LifecycleScenario, 0, "driver_async")
                               : takeString(neverd_emulate_driver_scenario_json(
                                     Session, fixture("driver_async").c_str(),
                                     LifecycleScenario, nullptr));
    auto Parsed = llvm::json::parse(Output);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 4u);
    const auto *IO = (*Requests)[1].getAsObject();
    ASSERT_NE(IO, nullptr);
    EXPECT_EQ(IO->getInteger("dispatch_status"), 0x103);
    EXPECT_EQ(IO->getInteger("io_status"), 0);
    EXPECT_EQ(IO->getBoolean("completed"), true);
    EXPECT_EQ(IO->getString("output_hex"), "5a4b7869");
  }
}

TEST_F(DriverScenarioPublic, CAPIAndCLIBatchIndependentPendingRequests) {
  constexpr const char *Scenario = R"({"requests":[
    {"kind":"create","file":0},
    {"kind":"create","file":1},
    {"kind":"ioctl","file":0,"code":"0x222044","input":"00015aff","output_size":4,"defer_callback_drain":true},
    {"kind":"ioctl","file":1,"code":"0x222044","input":"11223344","output_size":4},
    {"kind":"cleanup","file":0},
    {"kind":"close","file":0},
    {"kind":"cleanup","file":1},
    {"kind":"close","file":1}
  ],"unload":true})";
  for (bool UseCLI : {false, true}) {
    const auto Output =
        UseCLI
            ? runCLI(Scenario, 0, "driver_async")
            : takeString(neverd_emulate_driver_scenario_json(
                  Session, fixture("driver_async").c_str(), Scenario, nullptr));
    auto Parsed = llvm::json::parse(Output);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 8u);
    for (size_t I : {2u, 3u}) {
      const auto *IO = (*Requests)[I].getAsObject();
      ASSERT_NE(IO, nullptr);
      EXPECT_EQ(IO->getInteger("dispatch_status"), 0x103);
      EXPECT_EQ(IO->getInteger("io_status"), 0);
      EXPECT_EQ(IO->getBoolean("completed"), true);
      EXPECT_EQ(IO->getString("output_hex"), I == 2 ? "5a5b00a5" : "4b78691e");
    }
  }
}

TEST_F(DriverScenarioPublic, CAPIAndCLIBatchedCancellationIsPerIRP) {
  constexpr const char *Scenario = R"({"requests":[
    {"kind":"create","file":0},
    {"kind":"create","file":1},
    {"kind":"ioctl","file":0,"code":"0x222048","input":"00015aff","output_size":4,"cancel_after_100ns":0,"defer_callback_drain":true},
    {"kind":"ioctl","file":1,"code":"0x222048","input":"11223344","output_size":4},
    {"kind":"cleanup","file":0},
    {"kind":"close","file":0},
    {"kind":"cleanup","file":1},
    {"kind":"close","file":1}
  ],"unload":true})";
  for (bool UseCLI : {false, true}) {
    const auto Output =
        UseCLI
            ? runCLI(Scenario, 2, "driver_async")
            : takeString(neverd_emulate_driver_scenario_json(
                  Session, fixture("driver_async").c_str(), Scenario, nullptr));
    auto Parsed = llvm::json::parse(Output);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("stop_reason"), "returned");
    EXPECT_EQ(Report->getBoolean("scenario_success"), false);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 8u);
    const auto *Canceled = (*Requests)[2].getAsObject();
    const auto *Completed = (*Requests)[3].getAsObject();
    ASSERT_NE(Canceled, nullptr);
    ASSERT_NE(Completed, nullptr);
    EXPECT_EQ(Canceled->getInteger("dispatch_status"), 0x103);
    EXPECT_EQ(Canceled->getInteger("io_status"), 0xc0000120);
    const auto *Cancellation = Canceled->get("cancel_requested_at_100ns");
    ASSERT_NE(Cancellation, nullptr);
    EXPECT_NE(Cancellation->kind(), llvm::json::Value::Null);
    EXPECT_EQ(Completed->getInteger("dispatch_status"), 0x103);
    EXPECT_EQ(Completed->getInteger("io_status"), 0);
    EXPECT_EQ(Completed->getString("output_hex"), "4b78691e");
  }
}

TEST_F(DriverScenarioPublic, CAPIAndCLIOverlapAsynchronousFileRequests) {
  constexpr const char *Scenario = R"({"requests":[
    {"kind":"create","asynchronous_file":true},
    {"kind":"ioctl","code":"0x22204c","input":"00015aff","output_size":4,"defer_callback_drain":true},
    {"kind":"ioctl","code":"0x22204c","input":"11223344","output_size":4},
    {"kind":"cleanup"},
    {"kind":"close"}
  ],"unload":true})";
  for (bool UseCLI : {false, true}) {
    const auto Output =
        UseCLI
            ? runCLI(Scenario, 0, "driver_async")
            : takeString(neverd_emulate_driver_scenario_json(
                  Session, fixture("driver_async").c_str(), Scenario, nullptr));
    auto Parsed = llvm::json::parse(Output);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 5u);
    for (size_t I : {1u, 2u}) {
      const auto *IO = (*Requests)[I].getAsObject();
      ASSERT_NE(IO, nullptr);
      EXPECT_EQ(IO->getInteger("dispatch_status"), 0x103);
      EXPECT_EQ(IO->getInteger("io_status"), 0);
      EXPECT_EQ(IO->getString("output_hex"), I == 1 ? "5a5b00a5" : "4b78691e");
    }
  }
}

TEST_F(DriverScenarioPublic, CLIDoesNotHideFailedAsynchronousCompletion) {
  std::string Scenario = LifecycleScenario;
  Scenario.replace(Scenario.find("0x222000"), 8, "0x222028");
  auto Parsed = llvm::json::parse(runCLI(Scenario, 2, "driver_async"));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  const auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getString("stop_reason"), "returned");
  EXPECT_EQ(Report->getBoolean("scenario_success"), false);
  EXPECT_EQ(Report->getBoolean("unload_completed"), true);
  const auto *Requests = Report->getArray("requests");
  ASSERT_NE(Requests, nullptr);
  ASSERT_EQ(Requests->size(), 4u);
  EXPECT_EQ((*Requests)[1].getAsObject()->getInteger("dispatch_status"), 0x103);
  EXPECT_EQ((*Requests)[1].getAsObject()->getInteger("io_status"), 0xc000000d);
}

TEST_F(DriverScenarioPublic, CAPIAndCLIRunTimerDpcAndResumableWaits) {
  for (const char *Code : {"0x222000", "0x22201c", "0x222020", "0x222028"}) {
    std::string Scenario = LifecycleScenario;
    Scenario.replace(Scenario.find("0x222000"), 8, Code);
    Scenario.replace(Scenario.find("\"output_size\":4"), 15,
                     "\"output_size\":8");
    for (bool UseCLI : {false, true}) {
      auto Text = UseCLI ? runCLI(Scenario, 0, "driver_dispatcher")
                         : takeString(neverd_emulate_driver_scenario_json(
                               Session, fixture("driver_dispatcher").c_str(),
                               Scenario.c_str(), nullptr));
      auto Parsed = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Parsed))
          << llvm::toString(Parsed.takeError()) << error();
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 4u);
      EXPECT_EQ((*Requests)[1].getAsObject()->getBoolean("completed"), true);
      EXPECT_EQ((*Requests)[1].getAsObject()->getInteger("io_status"), 0);
      EXPECT_EQ((*Requests)[1].getAsObject()->getInteger("information"), 8);
    }
  }
}

#ifdef NEVERD_KMDF_FIXTURE
void checkKMDFLifecycleReport(const llvm::json::Object &Report,
                              bool DuplicateDriver = false) {
  EXPECT_EQ(Report.getString("stop_reason"), "returned");
  EXPECT_EQ(Report.getInteger("nt_status"), 0);
  EXPECT_EQ(Report.getBoolean("scenario_success"), true);
  EXPECT_EQ(Report.getBoolean("unload_completed"), true);
  const auto *Messages = Report.getArray("messages");
  ASSERT_NE(Messages, nullptr);
  std::vector<std::string> Observed;
  for (const auto &Message : *Messages) {
    auto Text = Message.getAsString();
    ASSERT_TRUE(Text);
    if (Text->starts_with("KMDF lifecycle:"))
      Observed.push_back(Text->str());
  }
  std::vector<std::string> Expected{
      "KMDF lifecycle: driver created\n", "KMDF lifecycle: child cleanup\n",
      "KMDF lifecycle: child destroy\n",  "KMDF lifecycle: child released\n",
      "KMDF lifecycle: driver unload\n",  "KMDF lifecycle: driver cleanup\n",
      "KMDF lifecycle: driver destroy\n"};
  if (DuplicateDriver)
    Expected.insert(Expected.begin() + 1,
                    "KMDF lifecycle: duplicate driver checked\n");
  EXPECT_EQ(Observed, Expected);
  const auto *Calls = Report.getArray("calls");
  ASSERT_NE(Calls, nullptr);
  size_t Binds = 0, Unbinds = 0, Creates = 0, DuplicateContexts = 0;
  for (const auto &Value : *Calls) {
    const auto *Call = Value.getAsObject();
    ASSERT_NE(Call, nullptr);
    auto Name = Call->getString("name");
    if (Name == "WdfVersionBind")
      ++Binds;
    else if (Name == "WdfVersionUnbind")
      ++Unbinds;
    else if (Name == "WdfDriverCreate") {
      auto Status = Call->getString("result");
      ASSERT_TRUE(Status);
      EXPECT_TRUE(
          Status->equals_insensitive(Creates == 0 ? "0x0" : "0xC0000183"));
      ++Creates;
    } else if (Name == "WdfObjectAllocateContext" &&
               Call->getString("result") == "0x40000000") {
      ++DuplicateContexts;
    }
  }
  EXPECT_EQ(Binds, 1u);
  EXPECT_EQ(Unbinds, 1u);
  EXPECT_EQ(Creates, DuplicateDriver ? 2u : 1u);
  EXPECT_EQ(DuplicateContexts, 1u);
}

neverd_driver_options_v1 kmdfOptions(const char *Service) {
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  Options.service_name = Service;
  return Options;
}
#endif

TEST_F(DriverScenarioPublic, CAPIAndCLICompleteGenuineKMDFLifecycle) {
#ifdef NEVERD_KMDF_FIXTURE
  constexpr char Scenario[] = R"({"unload":true})";
  for (bool CLI : {false, true}) {
    SCOPED_TRACE(CLI);
    auto Parsed = llvm::json::parse(
        CLI ? runCLI(Scenario, 0, "success", NEVERD_KMDF_FIXTURE)
            : takeString(neverd_emulate_driver_scenario_json(
                  Session, NEVERD_KMDF_FIXTURE, Scenario, nullptr)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    ASSERT_NE(Parsed->getAsObject(), nullptr);
    checkKMDFLifecycleReport(*Parsed->getAsObject());
    EXPECT_TRUE(error().empty());
    EXPECT_FALSE(neverd_session_is_loaded(Session));
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_FIXTURE requires a genuine WDK-linked fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIAndCLICompleteGenuineKMDFWithActiveCFG) {
#if defined(NEVERD_KMDF_FIXTURE) && defined(NEVERD_KMDF_CFG_FIXTURE)
  constexpr char Scenario[] = R"({"unload":true,"load_address":"0x190000000"})";
  for (bool CLI : {false, true}) {
    SCOPED_TRACE(CLI);
    auto Parsed = llvm::json::parse(
        CLI ? runCLI(Scenario, 0, "success", NEVERD_KMDF_CFG_FIXTURE)
            : takeString(neverd_emulate_driver_scenario_json(
                  Session, NEVERD_KMDF_CFG_FIXTURE, Scenario, nullptr)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    ASSERT_NE(Parsed->getAsObject(), nullptr);
    checkKMDFLifecycleReport(*Parsed->getAsObject());
  }
#else
  GTEST_SKIP() << "Genuine WDK normal and active-CFG fixtures are required";
#endif
}

TEST_F(DriverScenarioPublic, CAPIPreservesDuplicateKMDFDriverStatus) {
#ifdef NEVERD_KMDF_FIXTURE
  auto Options = kmdfOptions("NeverDKmdfD");
  auto Parsed =
      llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
          Session, NEVERD_KMDF_FIXTURE, R"({"unload":true})", &Options)));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
  ASSERT_NE(Parsed->getAsObject(), nullptr);
  checkKMDFLifecycleReport(*Parsed->getAsObject(), true);
  EXPECT_TRUE(error().empty());
#else
  GTEST_SKIP() << "NEVERD_KMDF_FIXTURE requires a genuine WDK-linked fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIPreservesDocumentedKMDFCreateFailures) {
#ifdef NEVERD_KMDF_FIXTURE
  for (const auto &[Service, Status] :
       {std::pair{"NeverDKmdfS", 0xc0000004LL},
        std::pair{"NeverDKmdfA", 0xc000000dLL}}) {
    SCOPED_TRACE(Service);
    auto Options = kmdfOptions(Service);
    auto Parsed =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, NEVERD_KMDF_FIXTURE, R"({"unload":true})", &Options)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("stop_reason"), "returned");
    EXPECT_EQ(Report->getInteger("nt_status"), Status);
    EXPECT_EQ(Report->getBoolean("nt_success"), false);
    EXPECT_EQ(Report->getBoolean("scenario_success"), false);
    EXPECT_EQ(Report->getBoolean("unload_completed"), false);
    EXPECT_TRUE(error().empty());
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_FIXTURE requires a genuine WDK-linked fixture";
#endif
}

#ifdef NEVERD_KMDF_CONTROL_FIXTURE
constexpr const char KMDFControlScenario[] = R"({
  "requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl"},
    {"kind":"ioctl","code":"0x222000","input":"001122","output_size":12},
    {"kind":"read","output_size":4,"byte_offset":4294967297},
    {"kind":"write","input":"40414243","byte_offset":4294967297},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true,"load_address":"0x190000000"
})";

constexpr const char KMDFNeitherScenario[] = R"({
  "requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl"},
    {"kind":"ioctl","code":"0x222003","input":"001122","output_size":12},
    {"kind":"read","output_size":4,"byte_offset":4294967297},
    {"kind":"write","input":"40414243","byte_offset":4294967297},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true,"load_address":"0x190000000"
})";

void checkKMDFControlReport(const llvm::json::Object &Report, char Mode) {
  EXPECT_EQ(Report.getString("stop_reason"), "returned")
      << Report.getString("diagnostic").value_or("").str();
  EXPECT_EQ(Report.getBoolean("scenario_success"), true);
  EXPECT_EQ(Report.getBoolean("unload_completed"), true);
  const auto *Requests = Report.getArray("requests");
  ASSERT_NE(Requests, nullptr);
  ASSERT_EQ(Requests->size(), 6u);
  for (size_t Index = 0; Index < Requests->size(); ++Index) {
    const auto *Request = (*Requests)[Index].getAsObject();
    ASSERT_NE(Request, nullptr);
    EXPECT_EQ(Request->getBoolean("completed"), true);
    EXPECT_EQ(Request->getInteger("io_status"), 0);
    const bool Queued = Index >= 1 && Index <= 3;
    EXPECT_EQ(Request->getInteger("dispatch_status"), Queued ? 0x103 : 0);
    EXPECT_EQ(Request->getInteger("information"), Index == 1 ? 7
                                                  : Queued   ? 4
                                                             : 0);
  }
  const auto *IOCTL = (*Requests)[1].getAsObject();
  EXPECT_EQ(IOCTL->getString("output_hex"), Mode == 'D'   ? "4b4d44445a4b78"
                                            : Mode == 'C' ? "4b4d44435a4b78"
                                            : Mode == 'W' ? "4b4d44575a4b78"
                                            : Mode == 'M' ? "4b4d444d5a4b78"
                                            : Mode == 'I' ? "4b4d44495a4b78"
                                            : Mode == 'T' ? "4b4d44545a4b78"
                                            : Mode == 'b' ? "4b4d44625a4b78"
                                                          : "4b4d44425a4b78");
  EXPECT_EQ((*Requests)[2].getAsObject()->getString("output_hex"), "60616263");
  const auto *Devices = Report.getArray("devices");
  ASSERT_NE(Devices, nullptr);
  EXPECT_TRUE(Devices->empty());
}
#endif

TEST_F(DriverScenarioPublic, CAPIAndCLICompleteGenuineKMDFControlIO) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  for (bool CLI : {false, true}) {
    SCOPED_TRACE(CLI);
    auto Parsed =
        llvm::json::parse(CLI ? runCLI(KMDFControlScenario, 0, "success",
                                       NEVERD_KMDF_CONTROL_FIXTURE)
                              : takeString(neverd_emulate_driver_scenario_json(
                                    Session, NEVERD_KMDF_CONTROL_FIXTURE,
                                    KMDFControlScenario, nullptr)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    ASSERT_NE(Parsed->getAsObject(), nullptr);
    checkKMDFControlReport(*Parsed->getAsObject(), 'B');
    EXPECT_TRUE(error().empty());
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIUsesKMDFRequestMemoryAliases) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  Options.service_name = "NeverDKmdfControlb";
  std::vector<const char *> Images{NEVERD_KMDF_CONTROL_FIXTURE};
#ifdef NEVERD_KMDF_CONTROL_CFG_FIXTURE
  Images.push_back(NEVERD_KMDF_CONTROL_CFG_FIXTURE);
#endif
  for (const char *Image : Images) {
    SCOPED_TRACE(Image);
    auto Parsed =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, Image, KMDFControlScenario, &Options)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    ASSERT_NE(Parsed->getAsObject(), nullptr);
    checkKMDFControlReport(*Parsed->getAsObject(), 'b');
    EXPECT_TRUE(error().empty());
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

#ifdef NEVERD_KMDF_PNP_FIXTURE
constexpr llvm::StringLiteral KMDFPnpLifecycleScenario = R"({
    "pnp_devices":[{"id":"kmdf-pdo","bus":"resource_free",
      "initial_device_power":"D0","initial_system_power":"working"}],
    "requests":[
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"start",
       "bus_completion":{"status":0,"delay_100ns":11}},
      {"kind":"create","device_id":"kmdf-pdo","file":7},
      {"kind":"ioctl","device_id":"kmdf-pdo","file":7,
       "code":"0x222000","input":"7a","output_size":4},
      {"kind":"cleanup","device_id":"kmdf-pdo","file":7},
      {"kind":"close","device_id":"kmdf-pdo","file":7},
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"query_remove",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"remove",
       "bus_completion":{"status":0,"delay_100ns":7}}],
    "unload":true})";

constexpr llvm::StringLiteral KMDFPnpHeldRequestScenario = R"({
    "pnp_devices":[{"id":"kmdf-pdo","bus":"resource_free",
      "initial_device_power":"D0","initial_system_power":"working"}],
    "requests":[
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"start",
       "bus_completion":{"status":0}},
      {"kind":"create","device_id":"kmdf-pdo","file":7},
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"query_stop",
       "bus_completion":{"status":0}},
      {"kind":"ioctl","device_id":"kmdf-pdo","file":7,
       "code":"0x222000","input":"7a","output_size":4,
       "defer_callback_drain":true},
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"stop",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"start",
       "bus_completion":{"status":0}},
      {"kind":"cleanup","device_id":"kmdf-pdo","file":7},
      {"kind":"close","device_id":"kmdf-pdo","file":7},
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"query_remove",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"remove",
       "bus_completion":{"status":0}}],
    "unload":true})";

constexpr llvm::StringLiteral KMDFPnpFileForwardScenario = R"({
    "pnp_devices":[{"id":"kmdf-pdo","bus":"resource_free",
      "initial_device_power":"D0","initial_system_power":"working"}],
    "requests":[
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"start",
       "bus_completion":{"status":0}},
      {"kind":"create","device_id":"kmdf-pdo","file":7,
       "bus_completion":{"status":0}},
      {"kind":"cleanup","device_id":"kmdf-pdo","file":7,
       "bus_completion":{"status":0}},
      {"kind":"close","device_id":"kmdf-pdo","file":7,
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"query_remove",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"remove",
       "bus_completion":{"status":0}}],
    "unload":true})";

constexpr llvm::StringLiteral KMDFPnpServicePrefix = "NeverDKmdfPnp";

neverd_driver_options_v1 kmdfPnpOptions(const char *Service) {
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  Options.service_name = Service;
  return Options;
}
#endif

TEST_F(DriverScenarioPublic, CAPIAndCLICompleteGenuineKMDFPnpLifecycle) {
#ifdef NEVERD_KMDF_PNP_FIXTURE

  std::vector<const char *> Images{NEVERD_KMDF_PNP_FIXTURE};
#ifdef NEVERD_KMDF_PNP_CFG_FIXTURE
  Images.push_back(NEVERD_KMDF_PNP_CFG_FIXTURE);
#endif
  for (const char *Image : Images)
    for (bool CLI : {false, true}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(CLI);
      const std::string Service = KMDFPnpServicePrefix.str() + 'H';
      auto Options = kmdfPnpOptions(Service.c_str());
      auto Parsed = llvm::json::parse(
          CLI ? runCLI(KMDFPnpLifecycleScenario.str(), 0, "success", Image)
              : takeString(neverd_emulate_driver_scenario_json(
                    Session, Image, KMDFPnpLifecycleScenario.data(),
                    &Options)));
      ASSERT_TRUE(bool(Parsed))
          << llvm::toString(Parsed.takeError()) << error();
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      ASSERT_EQ(Report->getString("stop_reason"), "returned")
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Devices = Report->getArray("pnp_devices");
      ASSERT_NE(Devices, nullptr);
      ASSERT_EQ(Devices->size(), 1u);
      const auto *Device = Devices->front().getAsObject();
      ASSERT_NE(Device, nullptr);
      EXPECT_EQ(Device->getInteger("add_device_status"), 0);
      EXPECT_EQ(Device->getString("pnp_state"), "removed");
      EXPECT_EQ(Device->getBoolean("attached"), false);
      EXPECT_EQ(Device->getBoolean("provider_present"), false);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 7u);
      const auto *IO = (*Requests)[2].getAsObject();
      ASSERT_NE(IO, nullptr);
      EXPECT_EQ(IO->getInteger("io_status"), 0);
      EXPECT_EQ(IO->getString("output_hex"), "504e507a");
      if (!CLI) {
        const auto *Messages = Report->getArray("messages");
        ASSERT_NE(Messages, nullptr);
        std::vector<std::string> PnpMessages;
        for (const auto &Message : *Messages) {
          auto Text = Message.getAsString();
          ASSERT_TRUE(Text);
          if (Text->starts_with("KMDF PnP:"))
            PnpMessages.push_back(Text->str());
        }
        EXPECT_EQ(
            PnpMessages,
            (std::vector<std::string>{
                "KMDF PnP: device ready\n", "KMDF PnP: prepare hardware\n",
                "KMDF PnP: D0 entry\n", "KMDF PnP: D0 exit\n",
                "KMDF PnP: release hardware\n", "KMDF PnP: device cleanup\n",
                "KMDF PnP: device destroy\n", "KMDF PnP: driver unload\n"}));
      }
    }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIForwardsKMDFFileLifecycleToConfiguredPDO) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  std::vector<const char *> Images{NEVERD_KMDF_PNP_FIXTURE};
#ifdef NEVERD_KMDF_PNP_CFG_FIXTURE
  Images.push_back(NEVERD_KMDF_PNP_CFG_FIXTURE);
#endif
  for (const char *Image : Images) {
    SCOPED_TRACE(Image);
    const std::string Service = KMDFPnpServicePrefix.str() + 'O';
    auto Options = kmdfPnpOptions(Service.c_str());
    auto Parsed =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, Image, KMDFPnpFileForwardScenario.data(), &Options)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("stop_reason"), "returned")
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 6u);
    for (size_t Index : {1u, 2u, 3u}) {
      const auto *Request = (*Requests)[Index].getAsObject();
      ASSERT_NE(Request, nullptr);
      EXPECT_EQ(Request->getInteger("io_status"), 0);
      EXPECT_EQ(Request->getBoolean("completed"), true);
    }
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIPowerManagedKMDFPnpQueueEntersD0) {
#ifdef NEVERD_KMDF_PNP_FIXTURE

  std::vector<const char *> Images{NEVERD_KMDF_PNP_FIXTURE};
#ifdef NEVERD_KMDF_PNP_CFG_FIXTURE
  Images.push_back(NEVERD_KMDF_PNP_CFG_FIXTURE);
#endif
  for (const char *Image : Images) {
    SCOPED_TRACE(Image);
    const std::string Service = KMDFPnpServicePrefix.str() + 'M';
    auto Options = kmdfPnpOptions(Service.c_str());
    auto Parsed =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, Image, KMDFPnpLifecycleScenario.data(), &Options)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    ASSERT_EQ(Report->getString("stop_reason"), "returned")
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 7u);
    const auto *IO = (*Requests)[2].getAsObject();
    ASSERT_NE(IO, nullptr);
    EXPECT_EQ(IO->getInteger("io_status"), 0);
    EXPECT_EQ(IO->getString("output_hex"), "504e507a");
    const auto *Calls = Report->getArray("calls");
    ASSERT_NE(Calls, nullptr);
    EXPECT_EQ(std::count_if(Calls->begin(), Calls->end(),
                            [](const auto &Call) {
                              const auto *Object = Call.getAsObject();
                              return Object && Object->getString("name") ==
                                                   "WdfIoQueueGetState";
                            }),
              4);
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIResumesStoppedKMDFRequests) {
#ifdef NEVERD_KMDF_PNP_FIXTURE

  std::vector<const char *> Images{NEVERD_KMDF_PNP_FIXTURE};
#ifdef NEVERD_KMDF_PNP_CFG_FIXTURE
  Images.push_back(NEVERD_KMDF_PNP_CFG_FIXTURE);
#endif
  for (const char *Image : Images)
    for (char Mode : {'A', 'V'}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Mode);
      const std::string Service = KMDFPnpServicePrefix.str() + Mode;
      auto Options = kmdfPnpOptions(Service.c_str());
      auto Parsed =
          llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
              Session, Image, KMDFPnpHeldRequestScenario.data(), &Options)));
      ASSERT_TRUE(bool(Parsed))
          << llvm::toString(Parsed.takeError()) << error();
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      ASSERT_EQ(Report->getString("stop_reason"), "returned")
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 10u);
      const auto *IO = (*Requests)[3].getAsObject();
      ASSERT_NE(IO, nullptr);
      EXPECT_EQ(IO->getInteger("io_status"), 0);
      EXPECT_EQ(IO->getString("output_hex"), Mode == 'A' ? "504e507a" : "");
      const auto *Calls = Report->getArray("calls");
      ASSERT_NE(Calls, nullptr);
      EXPECT_EQ(std::count_if(Calls->begin(), Calls->end(),
                              [](const auto &Call) {
                                const auto *Object = Call.getAsObject();
                                return Object &&
                                       Object->getString("name") ==
                                           "WdfRequestStopAcknowledge";
                              }),
                1);
    }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIWaitsForKMDFWorkerBeforeStop) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  std::vector<const char *> Images{NEVERD_KMDF_PNP_FIXTURE};
#ifdef NEVERD_KMDF_PNP_CFG_FIXTURE
  Images.push_back(NEVERD_KMDF_PNP_CFG_FIXTURE);
#endif
  for (const char *Image : Images)
    for (char Mode : {'B', 'D', 'Y'}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Mode);
      const std::string Service = KMDFPnpServicePrefix.str() + Mode;
      auto Options = kmdfPnpOptions(Service.c_str());
      auto Parsed =
          llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
              Session, Image, KMDFPnpHeldRequestScenario.data(), &Options)));
      ASSERT_TRUE(bool(Parsed))
          << llvm::toString(Parsed.takeError()) << error();
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      ASSERT_EQ(Report->getString("stop_reason"), "returned")
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 10u);
      const auto *IO = (*Requests)[3].getAsObject();
      ASSERT_NE(IO, nullptr);
      EXPECT_EQ(IO->getBoolean("completed"), true);
      EXPECT_EQ(IO->getInteger("io_status"), 0);
      EXPECT_EQ(IO->getString("output_hex"), "");
      const auto *Calls = Report->getArray("calls");
      ASSERT_NE(Calls, nullptr);
      EXPECT_EQ(std::count_if(Calls->begin(), Calls->end(),
                              [](const auto &Call) {
                                const auto *Object = Call.getAsObject();
                                return Object && Object->getString("name") ==
                                                     "IoFreeWorkItem";
                              }),
                1);
    }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIPreparesAssignedKMDFHardware) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  constexpr llvm::StringLiteral Scenario = R"({
    "pnp_devices":[{"id":"kmdf-pdo","bus":"register_bank",
      "initial_device_power":"D0","initial_system_power":"working",
      "resources":[{"id":"registers","raw_start":"0x200000000",
        "translated_start":"0x300000000","length":"0x1000",
        "registers":[{"offset":0,"width":4,"access":"read_only",
          "value":"0x12345678"}]}]}],
    "requests":[
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"start",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"query_stop",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"stop",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"start",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"query_remove",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"remove",
       "bus_completion":{"status":0}}],
    "unload":true})";
  std::vector<const char *> Images{NEVERD_KMDF_PNP_FIXTURE};
#ifdef NEVERD_KMDF_PNP_CFG_FIXTURE
  Images.push_back(NEVERD_KMDF_PNP_CFG_FIXTURE);
#endif
  for (const char *Image : Images) {
    SCOPED_TRACE(Image);
    const std::string Service = KMDFPnpServicePrefix.str() + 'R';
    auto Options = kmdfPnpOptions(Service.c_str());
    auto Parsed =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, Image, Scenario.data(), &Options)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    ASSERT_EQ(Report->getString("stop_reason"), "returned")
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 6u);
    for (const auto &Value : *Requests) {
      const auto *Request = Value.getAsObject();
      ASSERT_NE(Request, nullptr);
      EXPECT_EQ(Request->getInteger("io_status"), 0);
    }
    const auto *Calls = Report->getArray("calls");
    ASSERT_NE(Calls, nullptr);
    for (llvm::StringRef API : {"MmMapIoSpace", "MmUnmapIoSpace"})
      EXPECT_EQ(std::count_if(Calls->begin(), Calls->end(),
                              [API](const auto &Call) {
                                const auto *Object = Call.getAsObject();
                                return Object &&
                                       Object->getString("name") == API;
                              }),
                2);
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIExecutesKMDFCallerContextBeforeQueue) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  Options.service_name = "NeverDKmdfI";
  auto Parsed = llvm::json::parse(takeString(
      neverd_emulate_driver_scenario_json(Session, NEVERD_KMDF_CONTROL_FIXTURE,
                                          KMDFControlScenario, &Options)));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
  ASSERT_NE(Parsed->getAsObject(), nullptr);
  checkKMDFControlReport(*Parsed->getAsObject(), 'I');
  EXPECT_TRUE(error().empty());
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIExecutesKMDFNeitherLockedMemory) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  Options.service_name = "NeverDKmdfT";
  auto Parsed = llvm::json::parse(takeString(
      neverd_emulate_driver_scenario_json(Session, NEVERD_KMDF_CONTROL_FIXTURE,
                                          KMDFNeitherScenario, &Options)));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
  ASSERT_NE(Parsed->getAsObject(), nullptr);
  checkKMDFControlReport(*Parsed->getAsObject(), 'T');
  EXPECT_TRUE(error().empty());
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic,
       CAPIExecutesKMDFParallelQueueBatchWithBoundedLimit) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  constexpr char Scenario[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl",
     "asynchronous_file":true},
    {"kind":"ioctl","code":"0x222000","input":"00015aff",
     "output_size":8,"defer_callback_drain":true},
    {"kind":"ioctl","code":"0x222000","input":"11223344",
     "output_size":8,"defer_callback_drain":true},
    {"kind":"ioctl","code":"0x222000","input":"0102",
     "output_size":6},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  for (char Mode : {'P', 'F'}) {
    SCOPED_TRACE(Mode);
    neverd_driver_options_v1 Options{};
    Options.struct_size = sizeof(Options);
    Options.instruction_limit = 100000;
    Options.memory_limit = 64 * 1024 * 1024;
    Options.event_limit = 10000;
    Options.timeout_milliseconds = 5000;
    Options.service_name = Mode == 'P' ? "NeverDKmdfP" : "NeverDKmdfF";
    auto Parsed =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, NEVERD_KMDF_CONTROL_FIXTURE, Scenario, &Options)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("stop_reason"), "returned")
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 6u);
    for (size_t Index : {1u, 2u, 3u}) {
      const auto *IO = (*Requests)[Index].getAsObject();
      ASSERT_NE(IO, nullptr);
      EXPECT_EQ(IO->getBoolean("completed"), true);
      EXPECT_EQ(IO->getInteger("dispatch_status"), 0x103);
      EXPECT_EQ(IO->getInteger("io_status"), 0);
    }
    EXPECT_EQ((*Requests)[1].getAsObject()->getString("output_hex"),
              Mode == 'P' ? "4b4d44505a5b00a5" : "4b4d44465a5b00a5");
    EXPECT_EQ((*Requests)[2].getAsObject()->getString("output_hex"),
              Mode == 'P' ? "4b4d44504b78691e" : "4b4d44464b78691e");
    EXPECT_EQ((*Requests)[3].getAsObject()->getString("output_hex"),
              Mode == 'P' ? "4b4d44505b58" : "4b4d44465b58");
    EXPECT_TRUE(error().empty());
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic,
       CAPIExecutesKMDFSequentialQueueBackpressureAndCancellation) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  constexpr char Waiting[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl",
     "asynchronous_file":true},
    {"kind":"ioctl","code":"0x222000","input":"00015aff",
     "output_size":8,"defer_callback_drain":true},
    {"kind":"ioctl","code":"0x222000","input":"11223344",
     "output_size":8,"defer_callback_drain":true},
    {"kind":"ioctl","code":"0x222000","input":"0102",
     "output_size":6},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  constexpr char Canceled[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl",
     "asynchronous_file":true},
    {"kind":"ioctl","code":"0x222000","input":"00015aff",
     "output_size":8,"defer_callback_drain":true},
    {"kind":"ioctl","code":"0x222000","input":"11223344",
     "output_size":8,"cancel_after_100ns":0},
    {"kind":"ioctl","code":"0x222000","input":"0102",
     "output_size":6},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  for (bool Cancel : {false, true}) {
    SCOPED_TRACE(Cancel);
    neverd_driver_options_v1 Options{};
    Options.struct_size = sizeof(Options);
    Options.instruction_limit = 100000;
    Options.memory_limit = 64 * 1024 * 1024;
    Options.event_limit = 10000;
    Options.timeout_milliseconds = 5000;
    Options.service_name = "NeverDKmdfW";
    auto Parsed =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, NEVERD_KMDF_CONTROL_FIXTURE, Cancel ? Canceled : Waiting,
            &Options)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("stop_reason"), "returned")
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("scenario_success"), !Cancel);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 6u);
    EXPECT_EQ((*Requests)[1].getAsObject()->getString("output_hex"),
              "4b4d44575a5b00a5");
    EXPECT_EQ((*Requests)[2].getAsObject()->getInteger("dispatch_status"),
              0x103);
    if (Cancel) {
      EXPECT_EQ((*Requests)[2].getAsObject()->getInteger("io_status"),
                0xc0000120u);
      EXPECT_EQ((*Requests)[2].getAsObject()->getString("output_hex"), "");
    } else {
      EXPECT_EQ((*Requests)[2].getAsObject()->getInteger("io_status"), 0);
      EXPECT_EQ((*Requests)[2].getAsObject()->getString("output_hex"),
                "4b4d44574b78691e");
    }
    EXPECT_EQ((*Requests)[3].getAsObject()->getString("output_hex"),
              "4b4d44575b58");
    EXPECT_TRUE(error().empty());
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIExecutesKMDFQueueStopAndRestart) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  constexpr char Scenario[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl",
     "asynchronous_file":true},
    {"kind":"ioctl","code":"0x222000","input":"00015aff",
     "output_size":8,"defer_callback_drain":true},
    {"kind":"ioctl","code":"0x222000","input":"11223344",
     "output_size":8},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  Options.service_name = "NeverDKmdfS";
  auto Parsed =
      llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
          Session, NEVERD_KMDF_CONTROL_FIXTURE, Scenario, &Options)));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
  const auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getString("stop_reason"), "returned")
      << Report->getString("diagnostic").value_or("").str();
  EXPECT_EQ(Report->getBoolean("scenario_success"), true);
  EXPECT_EQ(Report->getBoolean("unload_completed"), true);
  const auto *Requests = Report->getArray("requests");
  ASSERT_NE(Requests, nullptr);
  ASSERT_EQ(Requests->size(), 5u);
  EXPECT_EQ((*Requests)[1].getAsObject()->getString("output_hex"),
            "4b4d44535a5b00a5");
  EXPECT_EQ((*Requests)[2].getAsObject()->getInteger("dispatch_status"), 0x103);
  EXPECT_EQ((*Requests)[2].getAsObject()->getString("output_hex"),
            "4b4d44534b78691e");
  EXPECT_TRUE(error().empty());
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIExecutesKMDFQueueStopCompletionCallback) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  constexpr char Scenario[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl",
     "asynchronous_file":true},
    {"kind":"ioctl","code":"0x222000","input":"00015aff",
     "output_size":8,"defer_callback_drain":true},
    {"kind":"ioctl","code":"0x222000","input":"11223344",
     "output_size":8},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  Options.service_name = "NeverDKmdfV";
  auto Parsed =
      llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
          Session, NEVERD_KMDF_CONTROL_FIXTURE, Scenario, &Options)));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
  const auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getString("stop_reason"), "returned")
      << Report->getString("diagnostic").value_or("").str();
  EXPECT_EQ(Report->getBoolean("scenario_success"), true);
  EXPECT_EQ(Report->getBoolean("unload_completed"), true);
  const auto *Requests = Report->getArray("requests");
  ASSERT_NE(Requests, nullptr);
  ASSERT_EQ(Requests->size(), 5u);
  EXPECT_EQ((*Requests)[1].getAsObject()->getString("output_hex"),
            "4b4d44565a5b00a5");
  EXPECT_EQ((*Requests)[2].getAsObject()->getInteger("dispatch_status"), 0x103);
  EXPECT_EQ((*Requests)[2].getAsObject()->getString("output_hex"),
            "4b4d44564b78691e");
  EXPECT_TRUE(error().empty());
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIExecutesKMDFQueueDrainAndRestart) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  constexpr char Scenario[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl",
     "asynchronous_file":true},
    {"kind":"ioctl","code":"0x222000","input":"00015aff",
     "output_size":8,"defer_callback_drain":true},
    {"kind":"ioctl","code":"0x222000","input":"11223344",
     "output_size":8},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  Options.service_name = "NeverDKmdfE";
  auto Parsed =
      llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
          Session, NEVERD_KMDF_CONTROL_FIXTURE, Scenario, &Options)));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
  const auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getString("stop_reason"), "returned")
      << Report->getString("diagnostic").value_or("").str();
  EXPECT_EQ(Report->getBoolean("scenario_success"), true);
  EXPECT_EQ(Report->getBoolean("unload_completed"), true);
  const auto *Requests = Report->getArray("requests");
  ASSERT_NE(Requests, nullptr);
  ASSERT_EQ(Requests->size(), 5u);
  EXPECT_EQ((*Requests)[1].getAsObject()->getString("output_hex"),
            "4b4d44455a5b00a5");
  EXPECT_EQ((*Requests)[2].getAsObject()->getInteger("dispatch_status"), 0x103);
  EXPECT_EQ((*Requests)[2].getAsObject()->getString("output_hex"),
            "4b4d44454b78691e");
  EXPECT_TRUE(error().empty());
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIExecutesKMDFQueuePurgeCancellation) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  constexpr char Scenario[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl",
     "asynchronous_file":true},
    {"kind":"ioctl","code":"0x222000","input":"00015aff",
     "output_size":8,"defer_callback_drain":true},
    {"kind":"ioctl","code":"0x222000","input":"11223344",
     "output_size":8},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  Options.service_name = "NeverDKmdfO";
  auto Parsed =
      llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
          Session, NEVERD_KMDF_CONTROL_FIXTURE, Scenario, &Options)));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
  const auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getString("stop_reason"), "returned")
      << Report->getString("diagnostic").value_or("").str();
  EXPECT_EQ(Report->getBoolean("unload_completed"), true);
  const auto *Requests = Report->getArray("requests");
  ASSERT_NE(Requests, nullptr);
  ASSERT_EQ(Requests->size(), 5u);
  for (size_t Index : {1u, 2u}) {
    const auto *Request = (*Requests)[Index].getAsObject();
    ASSERT_NE(Request, nullptr);
    EXPECT_EQ(Request->getInteger("dispatch_status"), 0x103);
    EXPECT_EQ(Request->getInteger("io_status"), 0xc0000120u);
    EXPECT_EQ(Request->getString("output_hex"), "");
  }
  EXPECT_TRUE(error().empty());
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIExecutesKMDFSynchronousQueueOperations) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  constexpr char Scenario[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl"},
    {"kind":"ioctl","code":"0x222000","input":"00015aff",
     "output_size":8},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  for (char Mode : {'1', '2', '3'}) {
    SCOPED_TRACE(Mode);
    neverd_driver_options_v1 Options{};
    Options.struct_size = sizeof(Options);
    Options.instruction_limit = 100000;
    Options.memory_limit = 64 * 1024 * 1024;
    Options.event_limit = 10000;
    Options.timeout_milliseconds = 5000;
    const std::string Service = std::string("NeverDKmdf") + Mode;
    Options.service_name = Service.c_str();
    auto Parsed =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, NEVERD_KMDF_CONTROL_FIXTURE, Scenario, &Options)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("stop_reason"), "returned")
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 4u);
    const auto *IO = (*Requests)[1].getAsObject();
    ASSERT_NE(IO, nullptr);
    EXPECT_EQ(IO->getBoolean("completed"), true);
    EXPECT_EQ(IO->getInteger("dispatch_status"), 0x103);
    EXPECT_EQ(IO->getInteger("io_status"), Mode == '3' ? 0xc0000120u : 0u);
    EXPECT_EQ(IO->getString("output_hex"), Mode == '1'   ? "4b4d44315a5b00a5"
                                           : Mode == '2' ? "4b4d44325a5b00a5"
                                                         : "");
    EXPECT_TRUE(error().empty());
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIExecutesKMDFStopAndPurge) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  constexpr char Scenario[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl"},
    {"kind":"ioctl","code":"0x222000","input":"00015aff",
     "output_size":8},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  for (char Mode : {'5', '6'}) {
    SCOPED_TRACE(Mode);
    neverd_driver_options_v1 Options{};
    Options.struct_size = sizeof(Options);
    Options.instruction_limit = 100000;
    Options.memory_limit = 64 * 1024 * 1024;
    Options.event_limit = 10000;
    Options.timeout_milliseconds = 5000;
    const std::string Service = std::string("NeverDKmdf") + Mode;
    Options.service_name = Service.c_str();
    auto Parsed =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, NEVERD_KMDF_CONTROL_FIXTURE, Scenario, &Options)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("stop_reason"), "returned")
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 4u);
    const auto *IO = (*Requests)[1].getAsObject();
    ASSERT_NE(IO, nullptr);
    EXPECT_EQ(IO->getBoolean("completed"), true);
    EXPECT_EQ(IO->getInteger("dispatch_status"), 0x103);
    EXPECT_EQ(IO->getInteger("io_status"), 0xc0000120u);
    EXPECT_EQ(IO->getString("output_hex"), "");
    EXPECT_TRUE(error().empty());
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIExecutesKMDFManualQueueAndRequeue) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  constexpr char Scenario[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl",
     "asynchronous_file":true},
    {"kind":"ioctl","code":"0x222000","input":"00015aff",
     "output_size":8,"defer_callback_drain":true},
    {"kind":"ioctl","code":"0x222000","input":"0102",
     "output_size":6},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  for (char Mode : {'Y', 'R'}) {
    SCOPED_TRACE(Mode);
    neverd_driver_options_v1 Options{};
    Options.struct_size = sizeof(Options);
    Options.instruction_limit = 100000;
    Options.memory_limit = 64 * 1024 * 1024;
    Options.event_limit = 10000;
    Options.timeout_milliseconds = 5000;
    Options.service_name = Mode == 'Y' ? "NeverDKmdfY" : "NeverDKmdfR";
    auto Parsed =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, NEVERD_KMDF_CONTROL_FIXTURE, Scenario, &Options)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("stop_reason"), "returned")
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 5u);
    EXPECT_EQ((*Requests)[1].getAsObject()->getString("output_hex"),
              Mode == 'Y' ? "4b4d44595a5b00a5" : "4b4d44525a5b00a5");
    EXPECT_EQ((*Requests)[2].getAsObject()->getString("output_hex"),
              Mode == 'Y' ? "4b4d44595b58" : "4b4d44525b58");
    EXPECT_TRUE(error().empty());
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic,
       CAPIExecutesKMDFCanceledOnQueueForForwardedRequest) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  constexpr char Scenario[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl",
     "asynchronous_file":true},
    {"kind":"ioctl","code":"0x222000","input":"00015aff",
     "output_size":8,"defer_callback_drain":true,"cancel_after_100ns":10},
    {"kind":"ioctl","code":"0x222000","input":"0102",
     "output_size":6},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  Options.service_name = "NeverDKmdf7";
  auto Parsed =
      llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
          Session, NEVERD_KMDF_CONTROL_FIXTURE, Scenario, &Options)));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
  const auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getString("stop_reason"), "returned")
      << Report->getString("diagnostic").value_or("").str();
  EXPECT_EQ(Report->getBoolean("unload_completed"), true);
  const auto *Requests = Report->getArray("requests");
  ASSERT_NE(Requests, nullptr);
  ASSERT_EQ(Requests->size(), 5u);
  const auto *Canceled = (*Requests)[1].getAsObject();
  ASSERT_NE(Canceled, nullptr);
  EXPECT_EQ(Canceled->getBoolean("completed"), true);
  EXPECT_EQ(Canceled->getInteger("io_status"), 0xc0000120u);
  EXPECT_EQ(Canceled->getInteger("cancel_requested_at_100ns"), 10);
  EXPECT_EQ(Canceled->getString("output_hex"), "");
  EXPECT_EQ((*Requests)[2].getAsObject()->getString("output_hex"),
            "4b4d44375b58");
  EXPECT_TRUE(error().empty());
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic,
       CAPIExecutesKMDFCanceledOnQueueAfterCallerEnqueue) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  constexpr char Scenario[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl"},
    {"kind":"ioctl","code":"0x222000","input":"00015aff",
     "output_size":8,"cancel_after_100ns":0},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  Options.service_name = "NeverDKmdf8";
  auto Parsed =
      llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
          Session, NEVERD_KMDF_CONTROL_FIXTURE, Scenario, &Options)));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
  const auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getString("stop_reason"), "returned")
      << Report->getString("diagnostic").value_or("").str();
  EXPECT_EQ(Report->getBoolean("unload_completed"), true);
  const auto *Requests = Report->getArray("requests");
  ASSERT_NE(Requests, nullptr);
  ASSERT_EQ(Requests->size(), 4u);
  const auto *Canceled = (*Requests)[1].getAsObject();
  ASSERT_NE(Canceled, nullptr);
  EXPECT_EQ(Canceled->getBoolean("completed"), true);
  EXPECT_EQ(Canceled->getInteger("io_status"), 0xc0000120u);
  EXPECT_EQ(Canceled->getInteger("cancel_requested_at_100ns"), 0);
  EXPECT_EQ(Canceled->getString("output_hex"), "");
  EXPECT_TRUE(error().empty());
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIExecutesKMDFManualQueueReadyNotification) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  constexpr char Scenario[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl"},
    {"kind":"ioctl","code":"0x222000","input":"00015aff",
     "output_size":8},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  Options.service_name = "NeverDKmdf9";
  auto Parsed =
      llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
          Session, NEVERD_KMDF_CONTROL_FIXTURE, Scenario, &Options)));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
  const auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getString("stop_reason"), "returned")
      << Report->getString("diagnostic").value_or("").str();
  EXPECT_EQ(Report->getBoolean("scenario_success"), true);
  EXPECT_EQ(Report->getBoolean("unload_completed"), true);
  const auto *Requests = Report->getArray("requests");
  ASSERT_NE(Requests, nullptr);
  ASSERT_EQ(Requests->size(), 4u);
  EXPECT_EQ((*Requests)[1].getAsObject()->getString("output_hex"),
            "4b4d44395a5b00a5");
  EXPECT_TRUE(error().empty());
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIExecutesKMDFManualQueueFindAndRetrieve) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  constexpr char Scenario[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl"},
    {"kind":"ioctl","code":"0x222000","input":"00015aff",
     "output_size":8},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  Options.service_name = "NeverDKmdf0";
  auto Parsed =
      llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
          Session, NEVERD_KMDF_CONTROL_FIXTURE, Scenario, &Options)));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
  const auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getString("stop_reason"), "returned")
      << Report->getString("diagnostic").value_or("").str();
  EXPECT_EQ(Report->getBoolean("scenario_success"), true);
  EXPECT_EQ(Report->getBoolean("unload_completed"), true);
  const auto *Requests = Report->getArray("requests");
  ASSERT_NE(Requests, nullptr);
  ASSERT_EQ(Requests->size(), 4u);
  EXPECT_EQ((*Requests)[1].getAsObject()->getString("output_hex"),
            "4b4d44305a5b00a5");
  EXPECT_TRUE(error().empty());
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIExecutesKMDFNondefaultAutomaticQueues) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  constexpr char Sequential[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl",
     "asynchronous_file":true},
    {"kind":"ioctl","code":"0x222000","input":"00015aff",
     "output_size":8,"defer_callback_drain":true},
    {"kind":"ioctl","code":"0x222000","input":"11223344",
     "output_size":8,"defer_callback_drain":true},
    {"kind":"ioctl","code":"0x222000","input":"0102",
     "output_size":6},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  constexpr char BoundedParallel[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl",
     "asynchronous_file":true},
    {"kind":"ioctl","code":"0x222000","input":"00015aff",
     "output_size":8,"defer_callback_drain":true},
    {"kind":"ioctl","code":"0x222000","input":"11223344",
     "output_size":8,"defer_callback_drain":true},
    {"kind":"ioctl","code":"0x222000","input":"01020304",
     "output_size":8,"defer_callback_drain":true},
    {"kind":"ioctl","code":"0x222000","input":"0102",
     "output_size":6},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  for (char Mode : {'A', 'G'}) {
    SCOPED_TRACE(Mode);
    neverd_driver_options_v1 Options{};
    Options.struct_size = sizeof(Options);
    Options.instruction_limit = 100000;
    Options.memory_limit = 64 * 1024 * 1024;
    Options.event_limit = 10000;
    Options.timeout_milliseconds = 5000;
    Options.service_name = Mode == 'A' ? "NeverDKmdfA" : "NeverDKmdfG";
    auto Parsed =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, NEVERD_KMDF_CONTROL_FIXTURE,
            Mode == 'A' ? Sequential : BoundedParallel, &Options)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("stop_reason"), "returned")
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), Mode == 'A' ? 6u : 7u);
    const unsigned ForwardCount = Mode == 'A' ? 2 : 3;
    for (unsigned I = 1; I <= ForwardCount; ++I) {
      const auto *Request = (*Requests)[I].getAsObject();
      ASSERT_NE(Request, nullptr);
      EXPECT_EQ(Request->getBoolean("completed"), true);
      EXPECT_EQ(Request->getInteger("dispatch_status"), 0x103);
      EXPECT_EQ(Request->getInteger("io_status"), 0);
      EXPECT_EQ(Request->getInteger("information"), 8);
    }
    EXPECT_EQ((*Requests)[1].getAsObject()->getString("output_hex"),
              Mode == 'A' ? "4b4d44415a5b00a5" : "4b4d44475a5b00a5");
    EXPECT_EQ((*Requests)[2].getAsObject()->getString("output_hex"),
              Mode == 'A' ? "4b4d44414b78691e" : "4b4d44474b78691e");
    if (Mode == 'G')
      EXPECT_EQ((*Requests)[3].getAsObject()->getString("output_hex"),
                "4b4d44475b58595e");
    EXPECT_EQ(
        (*Requests)[ForwardCount + 1].getAsObject()->getString("output_hex"),
        Mode == 'A' ? "4b4d44415b58" : "4b4d44475b58");
    EXPECT_TRUE(error().empty());
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic,
       CAPIAndCLILeaveCancellationTimeNullWhenKMDFCompletionWins) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  constexpr char Scenario[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl"},
    {"kind":"ioctl","code":"0x222000","input":"001122","output_size":12,
     "cancel_after_100ns":1},
    {"kind":"read","output_size":4,"cancel_after_100ns":1},
    {"kind":"write","input":"40414243","cancel_after_100ns":1},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  for (bool CLI : {false, true}) {
    SCOPED_TRACE(CLI);
    auto Parsed = llvm::json::parse(
        CLI ? runCLI(Scenario, 0, "success", NEVERD_KMDF_CONTROL_FIXTURE)
            : takeString(neverd_emulate_driver_scenario_json(
                  Session, NEVERD_KMDF_CONTROL_FIXTURE, Scenario, nullptr)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    checkKMDFControlReport(*Report, 'B');
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    for (const auto &Value : *Requests) {
      const auto *Request = Value.getAsObject();
      ASSERT_NE(Request, nullptr);
      const auto *Time = Request->get("cancel_requested_at_100ns");
      ASSERT_NE(Time, nullptr);
      EXPECT_EQ(Time->kind(), llvm::json::Value::Null);
    }
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic,
       CAPIAndCLIReportPreDispatchKMDFCancellationAsCompletedFailure) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  constexpr char Scenario[] = R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl"},
    {"kind":"ioctl","code":"0x222000","input":"001122","output_size":12,
     "cancel_after_100ns":0},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  for (bool CLI : {false, true}) {
    SCOPED_TRACE(CLI);
    // The CLI's existing completed-failure exit code is 2. Cancellation is a
    // completed NTSTATUS observation, not setup failure or a model error.
    auto Parsed = llvm::json::parse(
        CLI ? runCLI(Scenario, 2, "success", NEVERD_KMDF_CONTROL_FIXTURE)
            : takeString(neverd_emulate_driver_scenario_json(
                  Session, NEVERD_KMDF_CONTROL_FIXTURE, Scenario, nullptr)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("stop_reason"), "returned");
    EXPECT_EQ(Report->getInteger("nt_status"), 0);
    EXPECT_EQ(Report->getBoolean("scenario_success"), false);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    EXPECT_EQ(Report->getString("diagnostic"), "");
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 4u);
    const auto *IO = (*Requests)[1].getAsObject();
    ASSERT_NE(IO, nullptr);
    EXPECT_EQ(IO->getBoolean("completed"), true);
    EXPECT_EQ(IO->getInteger("dispatch_status"), 0x103);
    EXPECT_EQ(IO->getInteger("io_status"), 0xc0000120);
    EXPECT_EQ(IO->getInteger("cancel_requested_at_100ns"), 0);
    EXPECT_EQ(IO->getString("output_hex"), "");
    const auto *Devices = Report->getArray("devices");
    ASSERT_NE(Devices, nullptr);
    EXPECT_TRUE(Devices->empty());
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIRunsGenuineCancelCallbacksAndUnmarkRaces) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  struct Case {
    const char *Service;
    uint64_t Delay;
    bool CompletedNormally;
  };
  for (const auto &Test :
       {Case{"NeverDKmdfX", 10, false}, Case{"NeverDKmdfX", 0, false},
        Case{"NeverDKmdfH", 10, false}, Case{"NeverDKmdfU", 10, true},
        Case{"NeverDKmdfU", 30, true}}) {
    SCOPED_TRACE(Test.Service);
    SCOPED_TRACE(Test.Delay);
    neverd_driver_options_v1 Options{};
    Options.struct_size = sizeof(Options);
    Options.instruction_limit = 100000;
    Options.memory_limit = 64 * 1024 * 1024;
    Options.event_limit = 10000;
    Options.timeout_milliseconds = 5000;
    Options.service_name = Test.Service;
    const std::string Scenario =
        std::string(R"({"requests":[
          {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl"},
          {"kind":"ioctl","code":"0x222000","input":"001122",
           "output_size":12,"cancel_after_100ns":)") +
        std::to_string(Test.Delay) +
        R"(},{"kind":"cleanup"},{"kind":"close"}],"unload":true})";
    auto Parsed =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, NEVERD_KMDF_CONTROL_FIXTURE, Scenario.c_str(), &Options)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("stop_reason"), "returned")
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("scenario_success"), Test.CompletedNormally);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 4u);
    const auto *IO = (*Requests)[1].getAsObject();
    ASSERT_NE(IO, nullptr);
    EXPECT_EQ(IO->getBoolean("completed"), true);
    EXPECT_EQ(IO->getInteger("dispatch_status"), 0x103);
    EXPECT_EQ(IO->getInteger("io_status"),
              Test.CompletedNormally ? 0 : 0xc0000120);
    EXPECT_EQ(IO->getString("output_hex"),
              Test.CompletedNormally ? "4b4d44555a4b78" : "");
    const auto *Time = IO->get("cancel_requested_at_100ns");
    ASSERT_NE(Time, nullptr);
    if (Test.Delay == 30)
      EXPECT_EQ(Time->kind(), llvm::json::Value::Null);
    else
      EXPECT_EQ(Time->getAsUINT64(), Test.Delay);
    const auto *Messages = Report->getArray("messages");
    ASSERT_NE(Messages, nullptr);
    for (const auto &Message : *Messages) {
      auto Text = Message.getAsString();
      ASSERT_TRUE(Text);
      EXPECT_EQ(Text->find("failure"), llvm::StringRef::npos);
      EXPECT_EQ(Text->find("forbidden"), llvm::StringRef::npos);
    }
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIReportsGenuineRequestMetadataAndBufferedMDLs) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  Options.service_name = "NeverDKmdfM";
  auto Parsed = llvm::json::parse(takeString(
      neverd_emulate_driver_scenario_json(Session, NEVERD_KMDF_CONTROL_FIXTURE,
                                          KMDFControlScenario, &Options)));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
  const auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  checkKMDFControlReport(*Report, 'M');
  const auto *Calls = Report->getArray("calls");
  ASSERT_NE(Calls, nullptr);
  size_t InputMdls = 0, OutputMdls = 0, SetInfo = 0, GetInfo = 0;
  for (const auto &Value : *Calls) {
    const auto *Call = Value.getAsObject();
    ASSERT_NE(Call, nullptr);
    const auto Name = Call->getString("name");
    if (Name == "WdfRequestRetrieveInputWdmMdl") {
      ++InputMdls;
      EXPECT_EQ(Call->getString("result"), "0x0");
    } else if (Name == "WdfRequestRetrieveOutputWdmMdl") {
      ++OutputMdls;
      EXPECT_EQ(Call->getString("result"), "0x0");
    } else if (Name == "WdfRequestSetInformation") {
      ++SetInfo;
    } else if (Name == "WdfRequestGetInformation") {
      ++GetInfo;
    }
  }
  EXPECT_EQ(InputMdls, 2u);
  EXPECT_EQ(OutputMdls, 2u);
  EXPECT_EQ(SetInfo, 6u);
  EXPECT_EQ(GetInfo, 9u);
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIPreservesCleanupWritesToSharedIRPInformation) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  Options.service_name = "NeverDKmdfC";
  auto Parsed = llvm::json::parse(takeString(
      neverd_emulate_driver_scenario_json(Session, NEVERD_KMDF_CONTROL_FIXTURE,
                                          KMDFControlScenario, &Options)));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
  const auto *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  checkKMDFControlReport(*Report, 'C');
  const auto *Messages = Report->getArray("messages");
  ASSERT_NE(Messages, nullptr);
  bool SawInformationCheck = false;
  for (const auto &Value : *Messages) {
    const auto Message = Value.getAsString();
    ASSERT_TRUE(Message);
    EXPECT_EQ(Message->find("failure"), llvm::StringRef::npos);
    SawInformationCheck |=
        *Message == "KMDF control: C cleanup information checked\n";
  }
  EXPECT_TRUE(SawInformationCheck);
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIPreservesLegacyMarkCancellationReturnOrder) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  for (uint64_t Delay : {0u, 10u}) {
    SCOPED_TRACE(Delay);
    neverd_driver_options_v1 Options{};
    Options.struct_size = sizeof(Options);
    Options.instruction_limit = 100000;
    Options.memory_limit = 64 * 1024 * 1024;
    Options.event_limit = 10000;
    Options.timeout_milliseconds = 5000;
    Options.service_name = "NeverDKmdfL";
    const std::string Scenario =
        std::string(R"({"requests":[
          {"kind":"create","device":"\\DosDevices\\NeverDKmdfControl"},
          {"kind":"ioctl","code":"0x222000","input":"001122",
           "output_size":12,"cancel_after_100ns":)") +
        std::to_string(Delay) +
        R"(},{"kind":"cleanup"},{"kind":"close"}],"unload":true})";
    auto Parsed =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, NEVERD_KMDF_CONTROL_FIXTURE, Scenario.c_str(), &Options)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("stop_reason"), "returned")
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("scenario_success"), false);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 4u);
    const auto *IO = (*Requests)[1].getAsObject();
    ASSERT_NE(IO, nullptr);
    EXPECT_EQ(IO->getBoolean("completed"), true);
    EXPECT_EQ(IO->getInteger("io_status"), 0xc0000120);
    EXPECT_EQ(IO->getInteger("cancel_requested_at_100ns"), int64_t(Delay));
    const auto *Messages = Report->getArray("messages");
    ASSERT_NE(Messages, nullptr);
    std::vector<std::string> Observed;
    for (const auto &Message : *Messages) {
      auto Text = Message.getAsString();
      ASSERT_TRUE(Text);
      if (Text->starts_with("KMDF control:"))
        Observed.push_back(Text->str());
    }
    std::vector<std::string> Expected{
        "KMDF control: ready in mode L\n",
        "KMDF control: L before mark\n",
        "KMDF control: L cancel callback\n",
        "KMDF control: L request cleanup\n",
        "KMDF control: L cancel callback retained context\n",
        "KMDF control: L request destroy\n",
        "KMDF control: L destroy resumed\n",
        "KMDF control: driver unload\n"};
    Expected.insert(Expected.begin() + (Delay == 0 ? 7 : 2),
                    "KMDF control: L after mark\n");
    EXPECT_EQ(Observed, Expected);
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIReportsDirectAndDeferredKMDFControlIO) {
#ifdef NEVERD_KMDF_CONTROL_FIXTURE
  for (const char *Service : {"NeverDKmdfD", "NeverDKmdfW"}) {
    SCOPED_TRACE(Service);
    neverd_driver_options_v1 Options{};
    Options.struct_size = sizeof(Options);
    Options.instruction_limit = 100000;
    Options.memory_limit = 64 * 1024 * 1024;
    Options.event_limit = 10000;
    Options.timeout_milliseconds = 5000;
    Options.service_name = Service;
    auto Parsed =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, NEVERD_KMDF_CONTROL_FIXTURE, KMDFControlScenario,
            &Options)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    ASSERT_NE(Parsed->getAsObject(), nullptr);
    checkKMDFControlReport(*Parsed->getAsObject(), Service[10]);
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_CONTROL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIAndCLICompleteKMDFControlWithActiveCFG) {
#if defined(NEVERD_KMDF_CONTROL_FIXTURE) &&                                    \
    defined(NEVERD_KMDF_CONTROL_CFG_FIXTURE)
  for (bool CLI : {false, true}) {
    SCOPED_TRACE(CLI);
    auto Parsed =
        llvm::json::parse(CLI ? runCLI(KMDFControlScenario, 0, "success",
                                       NEVERD_KMDF_CONTROL_CFG_FIXTURE)
                              : takeString(neverd_emulate_driver_scenario_json(
                                    Session, NEVERD_KMDF_CONTROL_CFG_FIXTURE,
                                    KMDFControlScenario, nullptr)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    ASSERT_NE(Parsed->getAsObject(), nullptr);
    checkKMDFControlReport(*Parsed->getAsObject(), 'B');
  }
#else
  GTEST_SKIP() << "Genuine WDK normal and CFG control fixtures are required";
#endif
}

TEST_F(DriverScenarioPublic, CLIRunsOrderedLifecycleScenario) {
  auto Parsed = llvm::json::parse(
      runCLI(LifecycleScenario, 0, "success", NEVERD_DRIVER_IO_FIXTURE));
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_NE(Parsed->getAsObject(), nullptr);
  EXPECT_EQ(Parsed->getAsObject()->getBoolean("scenario_success"), true);
  EXPECT_EQ(Parsed->getAsObject()->getBoolean("unload_completed"), true);
}

TEST_F(DriverScenarioPublic, CAPIAndCLIPreserveFullWidthIOCTLInformation) {
  constexpr char Scenario[] = R"({"requests":[
    {"kind":"create","device":"\\Device\\NeverDIO"},
    {"kind":"ioctl","code":"0x222014"},
    {"kind":"ioctl","code":"0x22201c"},
    {"kind":"ioctl","code":"0x22201d"},
    {"kind":"ioctl","code":"0x22201e"},
    {"kind":"cleanup"},{"kind":"close"}
  ],"unload":true})";
  for (bool CLI : {false, true}) {
    SCOPED_TRACE(CLI);
    const std::string Text =
        CLI ? runCLI(Scenario, 0, "success", NEVERD_DRIVER_IO_FIXTURE)
            : takeString(neverd_emulate_driver_scenario_json(
                  Session, NEVERD_DRIVER_IO_FIXTURE, Scenario, nullptr));
    // The legacy numeric field remains an exact decimal JSON integer. Clients
    // whose number type cannot retain 64 bits can use information_hex instead.
    EXPECT_NE(Text.find("\"information\":18446744073709551615"),
              std::string::npos);
    auto Parsed = llvm::json::parse(Text);
    ASSERT_TRUE(static_cast<bool>(Parsed))
        << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 7u);
    for (size_t I = 1; I <= 4; ++I) {
      const auto *IO = (*Requests)[I].getAsObject();
      ASSERT_NE(IO, nullptr);
      ASSERT_NE(IO->get("information"), nullptr);
      EXPECT_EQ(IO->get("information")->getAsUINT64(),
                I == 1 ? 0x123456789abcdef0ULL : UINT64_MAX);
      EXPECT_EQ(IO->getString("information_hex"),
                I == 1 ? "0x123456789ABCDEF0" : "0xFFFFFFFFFFFFFFFF");
      EXPECT_EQ(IO->getString("output_hex"), "");
      EXPECT_EQ(IO->getBoolean("completed"), true);
    }
  }
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
    EXPECT_EQ(Report->getString("profile"),
              neverd::emulation::profile::ReportProfile);
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

TEST_F(DriverScenarioPublic, RegistryScenarioRunsThroughCAPIAndCLI) {
  constexpr char Scenario[] = R"({"registry":[{
    "path":"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\NeverDDriver",
    "values":[{"name":"Mode","type":4,"data":"78563412"}]
  }],"unload":true})";
  for (bool CLI : {false, true}) {
    auto Parsed =
        llvm::json::parse(CLI ? runCLI(Scenario, 0, "driver_registry")
                              : takeString(neverd_emulate_driver_scenario_json(
                                    Session, fixture("driver_registry").c_str(),
                                    Scenario, nullptr)));
    ASSERT_TRUE(static_cast<bool>(Parsed))
        << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Registry = Report->getArray("registry");
    ASSERT_NE(Registry, nullptr);
    bool Observed = false;
    for (const auto &Item : *Registry) {
      const auto *Key = Item.getAsObject();
      ASSERT_NE(Key, nullptr);
      const auto *Values = Key->getArray("values");
      ASSERT_NE(Values, nullptr);
      for (const auto &Value : *Values) {
        const auto *Record = Value.getAsObject();
        ASSERT_NE(Record, nullptr);
        if (Record->getString("name") == "Observed") {
          Observed = true;
          EXPECT_EQ(Record->getInteger("type"), 4);
          EXPECT_EQ(Record->getString("data"), "78563412");
        }
      }
    }
    EXPECT_TRUE(Observed);
  }
}

TEST_F(DriverScenarioPublic, InvalidRegistryFailsBeforeTheImageIsOpened) {
  constexpr char Scenario[] = R"({"registry":[{"path":"relative"}]})";
  EXPECT_EQ(neverd_emulate_driver_scenario_json(
                Session, "missing-registry-public.sys", Scenario, nullptr),
            nullptr);
  EXPECT_NE(error().find("driver scenario:"), std::string::npos);
  EXPECT_TRUE(
      runCLI(Scenario, 1, "success", "missing-registry-public.sys").empty());
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

#ifdef NEVERD_WDM_STACK_FIXTURE
std::string wdmStackScenario(bool Direct = false) {
  return std::string(R"({"requests":[
    {"kind":"create","device":"\\DosDevices\\NeverDWdmStack"},
    {"kind":"ioctl","code":")") +
         (Direct ? "0x222002" : "0x222000") +
         R"(","input":"00010203","output_size":4},
    {"kind":"cleanup"},{"kind":"close"}],"unload":true,
    "load_address":"0x190000000"})";
}

void checkWdmStackReport(const llvm::json::Object &Report, char Mode) {
  EXPECT_EQ(Report.getString("stop_reason"), "returned")
      << Report.getString("diagnostic").value_or("").str();
  EXPECT_EQ(Report.getBoolean("scenario_success"), true);
  EXPECT_EQ(Report.getBoolean("unload_completed"), true);
  const auto *Requests = Report.getArray("requests");
  ASSERT_NE(Requests, nullptr);
  ASSERT_EQ(Requests->size(), 4u);
  for (size_t I = 0; I != Requests->size(); ++I) {
    const auto *Request = (*Requests)[I].getAsObject();
    ASSERT_NE(Request, nullptr);
    EXPECT_EQ(Request->getBoolean("completed"), true);
    EXPECT_EQ(Request->getString("device"), "\\Device\\NeverDWdmStack");
    EXPECT_EQ(Request->getInteger("io_status"), 0);
    const bool Pending = I == 1 && Mode == 'I';
    EXPECT_EQ(Request->getInteger("dispatch_status"), Pending ? 0x103 : 0);
    EXPECT_EQ(Request->getInteger("information"), I == 1 ? 4 : 0);
  }
  constexpr char Hex[] = "0123456789abcdef";
  std::string Output = "53";
  Output += Hex[static_cast<unsigned char>(Mode) >> 4];
  Output += Hex[Mode & 15];
  Output += "444d";
  EXPECT_EQ((*Requests)[1].getAsObject()->getString("output_hex"), Output);
  const auto *Messages = Report.getArray("messages");
  ASSERT_NE(Messages, nullptr);
  for (const auto &Value : *Messages) {
    const auto Text = Value.getAsString();
    ASSERT_TRUE(Text);
    EXPECT_EQ(Text->find("failure"), llvm::StringRef::npos);
  }
  const auto *Devices = Report.getArray("devices");
  ASSERT_NE(Devices, nullptr);
  EXPECT_TRUE(Devices->empty());
}
#endif

TEST_F(DriverScenarioPublic, CAPIAndCLIExecuteGenuineWdmStackForwarding) {
#ifdef NEVERD_WDM_STACK_FIXTURE
  for (bool CLI : {false, true}) {
    SCOPED_TRACE(CLI);
    auto Parsed = llvm::json::parse(
        CLI ? runCLI(wdmStackScenario(), 0, "success", NEVERD_WDM_STACK_FIXTURE)
            : takeString(neverd_emulate_driver_scenario_json(
                  Session, NEVERD_WDM_STACK_FIXTURE, wdmStackScenario().c_str(),
                  nullptr)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    ASSERT_NE(Parsed->getAsObject(), nullptr);
    checkWdmStackReport(*Parsed->getAsObject(), 'S');
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_STACK_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIExecutesWdmRetainedAndNestedCompletion) {
#ifdef NEVERD_WDM_STACK_FIXTURE
  std::vector<const char *> Images{NEVERD_WDM_STACK_FIXTURE};
#ifdef NEVERD_WDM_STACK_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_STACK_CFG_FIXTURE);
#endif
  for (const auto *Image : Images)
    for (char Mode : {'W', 'D', 'N', 'I'}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Mode);
      const std::string Service = std::string("NeverDWdmStack") + Mode;
      neverd_driver_options_v1 Options{};
      Options.struct_size = sizeof(Options);
      Options.instruction_limit = 100000;
      Options.memory_limit = 64 * 1024 * 1024;
      Options.event_limit = 10000;
      Options.timeout_milliseconds = 5000;
      Options.service_name = Service.c_str();
      const auto Scenario = wdmStackScenario(Mode == 'D');
      auto Parsed =
          llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
              Session, Image, Scenario.c_str(), &Options)));
      ASSERT_TRUE(bool(Parsed))
          << llvm::toString(Parsed.takeError()) << error();
      ASSERT_NE(Parsed->getAsObject(), nullptr);
      const auto &Report = *Parsed->getAsObject();
      checkWdmStackReport(Report, Mode);
      const auto *Calls = Report.getArray("calls");
      ASSERT_NE(Calls, nullptr);
      unsigned Forwards = 0, Completes = 0;
      bool SawPendingReturn = false;
      for (const auto &Value : *Calls) {
        const auto *Call = Value.getAsObject();
        ASSERT_NE(Call, nullptr);
        if (Call->getString("name") == "IofCallDriver") {
          ++Forwards;
          SawPendingReturn |= Call->getString("result") == "0x103";
        }
        if (Call->getString("name") == "IofCompleteRequest")
          ++Completes;
      }
      EXPECT_EQ(Forwards, 4u);
      EXPECT_EQ(Completes, Mode == 'I' ? 4u : 5u);
      EXPECT_EQ(SawPendingReturn, Mode != 'N');
    }
#else
  GTEST_SKIP() << "NEVERD_WDM_STACK_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIAndCLIExecuteExplicitDelayedPnpLifecycle) {
#ifdef NEVERD_WDM_PNP_FIXTURE
  const std::string Scenario = R"({
    "pnp_devices":[{"id":"resource0","bus":"resource_free",
      "initial_device_power":"D0","initial_system_power":"working"}],
    "requests":[
      {"kind":"pnp","device_id":"resource0","minor":"start",
       "bus_completion":{"status":0,"delay_100ns":10}},
      {"kind":"create","device_id":"resource0","file":9},
      {"kind":"ioctl","file":9,"code":"0x222000","output_size":4},
      {"kind":"cleanup","file":9},{"kind":"close","file":9},
      {"kind":"pnp","device_id":"resource0","minor":"query_remove",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"resource0","minor":"remove",
       "bus_completion":{"status":0,"delay_100ns":20}}],
    "unload":true})";
  std::vector<const char *> Images{NEVERD_WDM_PNP_FIXTURE};
#ifdef NEVERD_WDM_PNP_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_PNP_CFG_FIXTURE);
#endif
  for (const auto *Image : Images)
    for (bool CLI : {false, true}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(CLI);
      auto Parsed = llvm::json::parse(
          CLI ? runCLI(Scenario, 0, "success", Image)
              : takeString(neverd_emulate_driver_scenario_json(
                    Session, Image, Scenario.c_str(), nullptr)));
      ASSERT_TRUE(bool(Parsed))
          << llvm::toString(Parsed.takeError()) << error();
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("stop_reason"), "returned");
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Devices = Report->getArray("pnp_devices");
      ASSERT_NE(Devices, nullptr);
      ASSERT_EQ(Devices->size(), 1u);
      const auto *Device = Devices->front().getAsObject();
      ASSERT_NE(Device, nullptr);
      EXPECT_EQ(Device->getString("id"), "resource0");
      EXPECT_EQ(Device->getInteger("add_device_status"), 0);
      EXPECT_EQ(Device->getString("pnp_state"), "removed");
      EXPECT_EQ(Device->getBoolean("provider_present"), false);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 7u);
      for (size_t Index : {0u, 5u, 6u}) {
        const auto *Request = (*Requests)[Index].getAsObject();
        ASSERT_NE(Request, nullptr);
        EXPECT_EQ(Request->getString("device_id"), "resource0");
        ASSERT_NE(Request->get("file"), nullptr);
        EXPECT_EQ(Request->get("file")->kind(), llvm::json::Value::Null);
        const auto *Pnp = Request->getObject("pnp");
        ASSERT_NE(Pnp, nullptr);
        EXPECT_EQ(Pnp->getInteger("bus_status"), 0);
        EXPECT_EQ(Pnp->getInteger("bus_received_at_100ns"), Index ? 10 : 0);
        EXPECT_EQ(Pnp->getInteger("bus_completed_at_100ns"),
                  Index == 6 ? 30 : 10);
      }
      EXPECT_EQ((*Requests)[2].getAsObject()->getString("output_hex"),
                "504e5021");
      EXPECT_EQ((*Requests)[2].getAsObject()->getInteger("file"), 9);
    }
#else
  GTEST_SKIP() << "NEVERD_WDM_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, ResourceRequeryStatusFailsBeforeImageLoading) {
  const std::string Scenario = R"({
    "pnp_devices":[{"id":"resource0","bus":"resource_free",
      "initial_device_power":"D0","initial_system_power":"working"}],
    "requests":[{"kind":"pnp","device_id":"resource0","minor":"query_stop",
      "bus_completion":{"status":"0x119"}}]})";
  EXPECT_EQ(neverd_emulate_driver_scenario_json(Session,
                                                "missing-pnp-status-image.sys",
                                                Scenario.c_str(), nullptr),
            nullptr);
  EXPECT_NE(error().find("resource requery"), std::string::npos);
  EXPECT_TRUE(
      runCLI(Scenario, 1, "success", "missing-pnp-status-image.sys").empty());
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
}

TEST_F(DriverScenarioPublic, CAPIAndCLIExecuteStopRestartAndSurpriseLifecycle) {
#ifdef NEVERD_WDM_PNP_FIXTURE
  const std::string Scenario = R"({
    "pnp_devices":[{"id":"resource0","bus":"resource_free",
      "initial_device_power":"D0","initial_system_power":"working"}],
    "requests":[
      {"kind":"pnp","device_id":"resource0","minor":"start",
       "bus_completion":{"status":0}},
      {"kind":"create","device_id":"resource0","file":9},
      {"kind":"pnp","device_id":"resource0","minor":"query_stop",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"resource0","minor":"cancel_stop",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"resource0","minor":"query_stop",
       "bus_completion":{"status":0,"delay_100ns":7}},
      {"kind":"pnp","device_id":"resource0","minor":"stop",
       "bus_completion":{"status":0}},
      {"kind":"ioctl","file":9,"code":"0x222000","output_size":4},
      {"kind":"pnp","device_id":"resource0","minor":"start",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"resource0","minor":"query_remove",
       "bus_completion":{"status":0}},
      {"kind":"ioctl","file":9,"code":"0x222000","output_size":4},
      {"kind":"pnp","device_id":"resource0","minor":"cancel_remove",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"resource0","minor":"surprise_removal",
       "bus_completion":{"status":0,"delay_100ns":13}},
      {"kind":"ioctl","file":9,"code":"0x222000","output_size":4},
      {"kind":"cleanup","file":9},{"kind":"close","file":9},
      {"kind":"pnp","device_id":"resource0","minor":"remove",
       "bus_completion":{"status":0}}],
    "unload":true})";
  std::vector<const char *> Images{NEVERD_WDM_PNP_FIXTURE};
#ifdef NEVERD_WDM_PNP_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_PNP_CFG_FIXTURE);
#endif
  const struct {
    size_t Index;
    const char *Minor;
    const char *Before;
    const char *After;
  } Transitions[] = {
      {0, "start", "not_started", "started"},
      {2, "query_stop", "started", "stop_pending"},
      {3, "cancel_stop", "stop_pending", "started"},
      {4, "query_stop", "started", "stop_pending"},
      {5, "stop", "stop_pending", "stopped"},
      {7, "start", "stopped", "started"},
      {8, "query_remove", "started", "remove_pending"},
      {10, "cancel_remove", "remove_pending", "started"},
      {11, "surprise_removal", "started", "surprise_removed"},
      {15, "remove", "surprise_removed", "removed"},
  };
  for (const auto *Image : Images)
    for (bool CLI : {false, true}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(CLI);
      auto Parsed = llvm::json::parse(
          CLI ? runCLI(Scenario, 2, "success", Image)
              : takeString(neverd_emulate_driver_scenario_json(
                    Session, Image, Scenario.c_str(), nullptr)));
      ASSERT_TRUE(bool(Parsed))
          << llvm::toString(Parsed.takeError()) << error();
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("profile"),
                neverd::emulation::profile::ReportProfile);
      EXPECT_EQ(Report->getString("stop_reason"), "returned");
      EXPECT_EQ(Report->getInteger("nt_status"), 0);
      EXPECT_EQ(Report->getBoolean("scenario_success"), false);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 16u);
      for (const auto &Expected : Transitions) {
        SCOPED_TRACE(Expected.Index);
        const auto *Request = (*Requests)[Expected.Index].getAsObject();
        ASSERT_NE(Request, nullptr);
        EXPECT_EQ(Request->getBoolean("completed"), true);
        EXPECT_EQ(Request->getInteger("io_status"), 0);
        const auto *Pnp = Request->getObject("pnp");
        ASSERT_NE(Pnp, nullptr);
        EXPECT_EQ(Pnp->getString("minor"), Expected.Minor);
        EXPECT_EQ(Pnp->getString("state_before"), Expected.Before);
        EXPECT_EQ(Pnp->getString("state_after"), Expected.After);
        EXPECT_EQ(Pnp->getInteger("bus_status"), 0);
        const int64_t Completed = Expected.Index < 4    ? 0
                                  : Expected.Index < 11 ? 7
                                                        : 20;
        EXPECT_EQ(Pnp->getInteger("bus_completed_at_100ns"), Completed);
      }
      for (size_t Index : {6u, 9u}) {
        const auto *IO = (*Requests)[Index].getAsObject();
        ASSERT_NE(IO, nullptr);
        EXPECT_EQ(IO->getInteger("io_status"), 0);
        EXPECT_EQ(IO->getString("output_hex"), "504e5021");
      }
      const auto *SurpriseIO = (*Requests)[12].getAsObject();
      ASSERT_NE(SurpriseIO, nullptr);
      EXPECT_EQ(SurpriseIO->getBoolean("completed"), true);
      EXPECT_EQ(SurpriseIO->getInteger("dispatch_status"), 0xc000000eu);
      EXPECT_EQ(SurpriseIO->getInteger("io_status"), 0xc000000eu);
      EXPECT_EQ(SurpriseIO->getInteger("information"), 0);
      EXPECT_EQ(SurpriseIO->getString("output_hex"), "");
      const auto *Devices = Report->getArray("pnp_devices");
      ASSERT_NE(Devices, nullptr);
      ASSERT_EQ(Devices->size(), 1u);
      EXPECT_EQ(Devices->front().getAsObject()->getString("pnp_state"),
                "removed");
      EXPECT_EQ(Devices->front().getAsObject()->getBoolean("provider_present"),
                false);
    }
#else
  GTEST_SKIP() << "NEVERD_WDM_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic,
       CAPIAndCLIRejectIncompletePowerFactsBeforeLoading) {
  constexpr char Prefix[] = R"({"pnp_devices":[{"id":"power0",
    "bus":"resource_free","initial_device_power":"D0",
    "initial_system_power":"working"}],"requests":[)";
  for (
      const char *Request :
      {R"({"kind":"power","device_id":"power0","minor":"set","power_type":"device","power_state":"D3","power_action":"sleep","bus_completion":{"status":0}})",
       R"({"kind":"power","device_id":"power0","minor":"query","power_type":"system","power_state":"working","power_action":"sleep","system_context":0,"bus_completion":{"status":0}})",
       R"({"kind":"power","device_id":"power0","minor":"set","power_type":"device","power_state":"D1","power_action":"none","system_context":0,"bus_completion":{"status":0}})",
       R"({"kind":"power","device_id":"power0","minor":"set","power_type":"device","power_state":"D3","power_action":"sleep","system_context":4294967296,"bus_completion":{"status":0}})"}) {
    const std::string Scenario = std::string(Prefix) + Request + "]}";
    SCOPED_TRACE(Scenario);
    EXPECT_EQ(neverd_emulate_driver_scenario_json(Session,
                                                  "missing-power-public.sys",
                                                  Scenario.c_str(), nullptr),
              nullptr);
    EXPECT_NE(error().find("driver scenario:"), std::string::npos);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    EXPECT_TRUE(
        runCLI(Scenario, 1, "success", "missing-power-public.sys").empty());
    std::ifstream Errors(Directory / "error.txt");
    const std::string Diagnostic(std::istreambuf_iterator<char>(Errors), {});
    EXPECT_NE(Diagnostic.find("driver scenario:"), std::string::npos);
  }
}

TEST_F(DriverScenarioPublic, CAPIAndCLIObserveIndependentPowerChildren) {
#ifdef NEVERD_WDM_POWER_FIXTURE
  constexpr char Scenario[] = R"({
    "load_address":"0x190000000","unload":true,
    "pnp_devices":[{"id":"power0","bus":"resource_free",
      "initial_device_power":"D0","initial_system_power":"working",
      "initial_reported_device_power":"D3","requested_device_power":[
        {"minor":"query","power_type":"device","power_state":"D3",
         "power_action":"sleep","system_context":"0x14400",
         "bus_completion":{"status":0,"delay_100ns":11}},
        {"minor":"set","power_type":"device","power_state":"D3",
         "power_action":"sleep","system_context":"0x14400",
         "bus_completion":{"status":0,"delay_100ns":11}},
        {"minor":"set","power_type":"device","power_state":"D0",
         "power_action":"sleep","system_context":"0x41100",
         "bus_completion":{"status":0,"delay_100ns":11}}]}],
    "requests":[
      {"kind":"pnp","device_id":"power0","minor":"start",
       "bus_completion":{"status":0}},
      {"kind":"power","device_id":"power0","minor":"query",
       "power_type":"system","power_state":"sleeping3","power_action":"sleep",
       "system_context":"0x14400","bus_completion":{"status":0,"delay_100ns":7}},
      {"kind":"power","device_id":"power0","minor":"set",
       "power_type":"system","power_state":"sleeping3","power_action":"sleep",
       "system_context":"0x14400","bus_completion":{"status":0,"delay_100ns":7}},
      {"kind":"power","device_id":"power0","minor":"set",
       "power_type":"system","power_state":"working","power_action":"sleep",
       "system_context":"0x41100","bus_completion":{"status":0,"delay_100ns":7}},
      {"kind":"pnp","device_id":"power0","minor":"query_remove",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"power0","minor":"remove",
       "bus_completion":{"status":0}}]})";
  std::vector<const char *> Images{NEVERD_WDM_POWER_FIXTURE};
#ifdef NEVERD_WDM_POWER_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_POWER_CFG_FIXTURE);
#endif
  for (const char *Image : Images)
    for (bool CLI : {false, true}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(CLI);
      auto Parsed = llvm::json::parse(
          CLI ? runCLI(Scenario, 0, "success", Image)
              : takeString(neverd_emulate_driver_scenario_json(
                    Session, Image, Scenario, nullptr)));
      ASSERT_TRUE(bool(Parsed))
          << llvm::toString(Parsed.takeError()) << error();
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("profile"),
                neverd::emulation::profile::ReportProfile);
      EXPECT_EQ(Report->getString("stop_reason"), "returned")
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 9u);
      std::vector<std::string> IRPs;
      unsigned Scenarios = 0, Children = 0;
      for (const auto &Value : *Requests) {
        const auto *Request = Value.getAsObject();
        ASSERT_NE(Request, nullptr);
        EXPECT_EQ(Request->getString("device_id"), "power0");
        EXPECT_EQ(Request->getBoolean("completed"), true);
        EXPECT_EQ(Request->getInteger("io_status"), 0);
        EXPECT_EQ(Request->getInteger("information"), 0);
        ASSERT_TRUE(Request->getString("irp"));
        const auto IRP = Request->getString("irp")->str();
        for (const auto &Previous : IRPs)
          EXPECT_NE(IRP, Previous);
        IRPs.push_back(IRP);
        const bool Child = Request->getString("origin") == "PoRequestPowerIrp";
        if (Child) {
          EXPECT_EQ(Request->getInteger("response_index"), Children);
          ++Children;
        } else {
          EXPECT_EQ(Request->getString("origin"), "scenario");
          ASSERT_NE(Request->get("response_index"), nullptr);
          EXPECT_EQ(Request->get("response_index")->kind(),
                    llvm::json::Value::Null);
          ++Scenarios;
        }
        if (Request->getString("kind") != "power")
          continue;
        ASSERT_NE(Request->get("file"), nullptr);
        EXPECT_EQ(Request->get("file")->kind(), llvm::json::Value::Null);
        const auto *Power = Request->getObject("power");
        ASSERT_NE(Power, nullptr);
        EXPECT_EQ(Power->getString("power_type"), Child ? "device" : "system");
        EXPECT_EQ(Power->getString("power_action"), "sleep");
        EXPECT_EQ(Power->getInteger("bus_status"), 0);
        ASSERT_TRUE(Power->getInteger("bus_received_at_100ns"));
        ASSERT_TRUE(Power->getInteger("bus_completed_at_100ns"));
        EXPECT_EQ(*Power->getInteger("bus_completed_at_100ns") -
                      *Power->getInteger("bus_received_at_100ns"),
                  Child ? 11 : 7);
        if (Child)
          EXPECT_TRUE(Power->getString("requested_device_object"));
      }
      EXPECT_EQ(Scenarios, 6u);
      EXPECT_EQ(Children, 3u);
      const auto *Devices = Report->getArray("pnp_devices");
      ASSERT_NE(Devices, nullptr);
      ASSERT_EQ(Devices->size(), 1u);
      const auto *Device = Devices->front().getAsObject();
      ASSERT_NE(Device, nullptr);
      EXPECT_EQ(Device->getString("pnp_state"), "removed");
      EXPECT_EQ(Device->getString("device_power"), "D0");
      EXPECT_EQ(Device->getString("system_power"), "working");
      EXPECT_EQ(Device->getBoolean("provider_present"), false);
      const auto *Calls = Report->getArray("calls");
      ASSERT_NE(Calls, nullptr);
      unsigned PoRequests = 0;
      for (const auto &Value : *Calls) {
        const auto *Call = Value.getAsObject();
        ASSERT_NE(Call, nullptr);
        if (Call->getString("name") == "PoRequestPowerIrp") {
          ++PoRequests;
          EXPECT_EQ(Call->getString("result"), "0x103");
        }
      }
      EXPECT_EQ(PoRequests, 3u);
    }
#else
  GTEST_SKIP() << "NEVERD_WDM_POWER_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIAndCLIExecuteResumableRemoveLockDrain) {
#ifdef NEVERD_WDM_REMOVE_LOCK_FIXTURE
  struct ImageCase {
    const char *Path;
    const char *LockSize;
  };
  std::vector<ImageCase> Images{{NEVERD_WDM_REMOVE_LOCK_FIXTURE, "0x20"}};
#ifdef NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE
  Images.push_back({NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE, "0x20"});
#endif
#ifdef NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE
  Images.push_back({NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE, "0x78"});
#endif
#ifdef NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE
  Images.push_back({NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE, "0x78"});
#endif
  for (const auto &Image : Images)
    for (unsigned Delay : {0u, 20u})
      for (bool CLI : {false, true}) {
        SCOPED_TRACE(Image.Path);
        SCOPED_TRACE(Delay);
        SCOPED_TRACE(CLI);
        const std::string Scenario =
            std::string(R"({"load_address":"0x190000000","unload":true,
              "pnp_devices":[{"id":"remove0","bus":"resource_free",
                "initial_device_power":"D0","initial_system_power":"working"}],
              "requests":[
                {"kind":"pnp","device_id":"remove0","minor":"start",
                 "bus_completion":{"status":0}},
                {"kind":"pnp","device_id":"remove0","minor":"query_remove",
                 "bus_completion":{"status":0}},
                {"kind":"pnp","device_id":"remove0","minor":"remove",
                 "bus_completion":{"status":0,"delay_100ns":)") +
            std::to_string(Delay) + "}}]}";
        auto Parsed = llvm::json::parse(
            CLI ? runCLI(Scenario, 0, "success", Image.Path)
                : takeString(neverd_emulate_driver_scenario_json(
                      Session, Image.Path, Scenario.c_str(), nullptr)));
        ASSERT_TRUE(bool(Parsed))
            << llvm::toString(Parsed.takeError()) << error();
        const auto *Report = Parsed->getAsObject();
        ASSERT_NE(Report, nullptr);
        EXPECT_EQ(Report->getString("profile"),
                  neverd::emulation::profile::ReportProfile);
        EXPECT_EQ(Report->getString("stop_reason"), "returned")
            << Report->getString("diagnostic").value_or("").str();
        EXPECT_EQ(Report->getBoolean("scenario_success"), true);
        EXPECT_EQ(Report->getBoolean("unload_completed"), true);
        const auto *Requests = Report->getArray("requests");
        ASSERT_NE(Requests, nullptr);
        ASSERT_EQ(Requests->size(), 3u);
        for (const auto &Value : *Requests) {
          const auto *Request = Value.getAsObject();
          ASSERT_NE(Request, nullptr);
          EXPECT_EQ(Request->getString("device_id"), "remove0");
          EXPECT_EQ(Request->getString("origin"), "scenario");
          EXPECT_EQ(Request->getBoolean("completed"), true);
          EXPECT_EQ(Request->getInteger("io_status"), 0);
          EXPECT_EQ(Request->getInteger("information"), 0);
          EXPECT_EQ(Request->getString("output_hex"), "");
        }
        const auto *Remove = Requests->back().getAsObject();
        ASSERT_NE(Remove, nullptr);
        const auto *Pnp = Remove->getObject("pnp");
        ASSERT_NE(Pnp, nullptr);
        EXPECT_EQ(Pnp->getString("minor"), "remove");
        EXPECT_EQ(Pnp->getString("state_before"), "remove_pending");
        EXPECT_EQ(Pnp->getString("state_after"), "removed");
        EXPECT_EQ(Pnp->getInteger("bus_status"), 0);
        EXPECT_EQ(Pnp->getInteger("bus_received_at_100ns"), 0);
        EXPECT_EQ(Pnp->getInteger("bus_completed_at_100ns"), Delay);
        const auto *Devices = Report->getArray("devices");
        ASSERT_NE(Devices, nullptr);
        EXPECT_TRUE(Devices->empty());
        const auto *Providers = Report->getArray("pnp_devices");
        ASSERT_NE(Providers, nullptr);
        ASSERT_EQ(Providers->size(), 1u);
        const auto *Provider = Providers->front().getAsObject();
        ASSERT_NE(Provider, nullptr);
        EXPECT_EQ(Provider->getString("pnp_state"), "removed");
        EXPECT_EQ(Provider->getBoolean("attached"), false);
        EXPECT_EQ(Provider->getBoolean("provider_present"), false);
        const auto *Calls = Report->getArray("calls");
        ASSERT_NE(Calls, nullptr);
        unsigned Initialized = 0, Acquired = 0, Released = 0, Drained = 0;
        for (const auto &Value : *Calls) {
          const auto *Call = Value.getAsObject();
          ASSERT_NE(Call, nullptr);
          const auto Name = Call->getString("name");
          const bool Init = Name == "IoInitializeRemoveLockEx";
          const bool Acquire = Name == "IoAcquireRemoveLockEx";
          const bool Release = Name == "IoReleaseRemoveLockEx";
          const bool Drain = Name == "IoReleaseRemoveLockAndWaitEx";
          if (!Init && !Acquire && !Release && !Drain)
            continue;
          const auto *Arguments = Call->getArray("arguments");
          ASSERT_NE(Arguments, nullptr);
          ASSERT_EQ(Arguments->size(), Init || Acquire ? 5u : 3u);
          EXPECT_EQ(Arguments->back().getAsString(), Image.LockSize);
          Initialized += Init;
          Acquired += Acquire;
          Released += Release;
          Drained += Drain;
        }
        EXPECT_EQ(Initialized, 1u);
        EXPECT_GE(Acquired, 2u);
        EXPECT_GE(Released, 1u);
        EXPECT_EQ(Drained, 1u);
        const auto *Messages = Report->getArray("messages");
        ASSERT_NE(Messages, nullptr);
        std::string Trace;
        for (const auto &Value : *Messages) {
          const auto Text = Value.getAsString();
          ASSERT_TRUE(Text);
          EXPECT_EQ(Text->find("failure"), llvm::StringRef::npos);
          Trace += Text->str();
        }
        size_t Position = 0;
        for (const char *Marker :
             {"Remove lock: worker last release",
              "Remove lock: worker waits after release",
              "Remove lock: drain returned",
              "Remove lock: device deletion requested",
              "Remove lock: worker resumed after deletion"}) {
          Position = Trace.find(Marker, Position);
          ASSERT_NE(Position, std::string::npos) << Marker << "\n" << Trace;
          Position += std::char_traits<char>::length(Marker);
        }
      }
#else
  GTEST_SKIP()
      << "NEVERD_WDM_REMOVE_LOCK_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, RejectsInvalidRegisterFactsBeforeImageLoading) {
  for (const char *Registers :
       {R"([{"offset":0,"width":8,"access":"read_write","value":0}])",
        R"([{"offset":0,"width":1,"access":"read_write","value":256}])",
        R"([{"offset":2,"width":4,"access":"read_write","value":0}])",
        R"([{"offset":0,"width":4,"access":"read_write","value":0},
             {"offset":2,"width":2,"access":"read_only","value":0}])",
        R"([{"offset":0,"width":4,"access":"write_only","value":0}])"}) {
    const std::string Scenario =
        std::string(R"({"pnp_devices":[{"id":"r","bus":"register_bank",
        "initial_device_power":"D0","initial_system_power":"working",
        "resources":[{"id":"bank","raw_start":0,"translated_start":4096,
        "length":8,"registers":)") +
        Registers + "}]}]}";
    EXPECT_EQ(neverd_emulate_driver_scenario_json(Session,
                                                  "missing-resource-driver.sys",
                                                  Scenario.c_str(), nullptr),
              nullptr);
    EXPECT_NE(error().find("driver scenario:"), std::string::npos);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
}

TEST_F(DriverScenarioPublic,
       CAPIAndCLIExecuteRegisterBanksAcrossStopAndRestart) {
#ifdef NEVERD_WDM_RESOURCE_FIXTURE
  std::vector<const char *> Images{NEVERD_WDM_RESOURCE_FIXTURE};
#ifdef NEVERD_WDM_RESOURCE_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_RESOURCE_CFG_FIXTURE);
#endif
  const std::string Scenario = R"({
    "load_address":"0x190000000","unload":true,
    "pnp_devices":[{"id":"register0","bus":"register_bank",
      "initial_device_power":"D0","initial_system_power":"working",
      "resources":[{"id":"bank0","raw_start":"0x200000000",
        "translated_start":"0x300000000","length":"0x1000","registers":[
          {"offset":0,"width":1,"access":"read_write","value":"0x12"},
          {"offset":2,"width":2,"access":"read_write","value":"0x3456"},
          {"offset":4,"width":4,"access":"read_write","value":"0x789abcde"},
          {"offset":8,"width":4,"access":"read_only","value":"0x10203040"},
          {"offset":"0x10","width":4,"access":"read_write","value":1},
          {"offset":"0x14","width":4,"access":"read_write","value":2},
          {"offset":"0x18","width":4,"access":"read_write","value":3},
          {"offset":"0x1c","width":4,"access":"read_write","value":4},
          {"offset":"0xffc","width":4,"access":"read_write","value":5}]}]}],
    "requests":[
      {"kind":"pnp","device_id":"register0","minor":"start",
       "bus_completion":{"status":0,"delay_100ns":11}},
      {"kind":"create","device_id":"register0","file":1},
      {"kind":"ioctl","file":1,"code":"0x222000","output_size":32},
      {"kind":"cleanup","file":1},{"kind":"close","file":1},
      {"kind":"pnp","device_id":"register0","minor":"query_stop",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"register0","minor":"stop",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"register0","minor":"start",
       "bus_completion":{"status":0,"delay_100ns":5}},
      {"kind":"create","device_id":"register0","file":2},
      {"kind":"ioctl","file":2,"code":"0x222000","output_size":32},
      {"kind":"cleanup","file":2},{"kind":"close","file":2},
      {"kind":"pnp","device_id":"register0","minor":"query_remove",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"register0","minor":"remove",
       "bus_completion":{"status":0,"delay_100ns":7}}]})";
  for (const auto *Image : Images)
    for (bool CLI : {false, true}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(CLI);
      auto Parsed = llvm::json::parse(
          CLI ? runCLI(Scenario, 0, "success", Image)
              : takeString(neverd_emulate_driver_scenario_json(
                    Session, Image, Scenario.c_str(), nullptr)));
      ASSERT_TRUE(bool(Parsed))
          << llvm::toString(Parsed.takeError()) << error();
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("profile"),
                neverd::emulation::profile::ReportProfile);
      EXPECT_EQ(Report->getString("stop_reason"), "returned")
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 14u);
      for (const auto &Value : *Requests) {
        const auto *Request = Value.getAsObject();
        ASSERT_NE(Request, nullptr);
        EXPECT_EQ(Request->getBoolean("completed"), true);
        EXPECT_EQ(Request->getInteger("io_status"), 0);
      }
      EXPECT_EQ((*Requests)[2].getAsObject()->getInteger("information"), 32);
      EXPECT_EQ(
          (*Requests)[2].getAsObject()->getString("output_hex"),
          "1300000057340000dfbc9a78403020100b0000000c0000000d0000000e000000");
      EXPECT_EQ((*Requests)[9].getAsObject()->getInteger("information"), 32);
      EXPECT_EQ(
          (*Requests)[9].getAsObject()->getString("output_hex"),
          "1400000058340000e0bc9a784030201015000000160000001700000018000000");
      EXPECT_EQ((*Requests)[0].getAsObject()->getObject("pnp")->getInteger(
                    "bus_completed_at_100ns"),
                11);
      EXPECT_EQ((*Requests)[7].getAsObject()->getObject("pnp")->getInteger(
                    "bus_completed_at_100ns"),
                16);
      const auto *Configured =
          Report->getObject("configuration")->getArray("pnp_devices");
      ASSERT_NE(Configured, nullptr);
      const auto *Resources =
          Configured->front().getAsObject()->getArray("resources");
      ASSERT_NE(Resources, nullptr);
      ASSERT_EQ(Resources->size(), 1u);
      const auto *Resource = Resources->front().getAsObject();
      EXPECT_EQ(Resource->getString("raw_start"), "0x200000000");
      EXPECT_EQ(Resource->getString("translated_start"), "0x300000000");
      EXPECT_EQ(Resource->getArray("registers")
                    ->front()
                    .getAsObject()
                    ->getInteger("value"),
                0x12);
      const auto *Providers = Report->getArray("pnp_devices");
      ASSERT_NE(Providers, nullptr);
      ASSERT_EQ(Providers->size(), 1u);
      EXPECT_EQ(Providers->front().getAsObject()->getString("pnp_state"),
                "removed");
      EXPECT_EQ(
          Providers->front().getAsObject()->getBoolean("provider_present"),
          false);
      EXPECT_TRUE(Report->getArray("devices")->empty());
      unsigned Map = 0, MapEx = 0, Unmap = 0;
      for (const auto &Value : *Report->getArray("calls")) {
        const auto *Call = Value.getAsObject();
        const auto Name = Call->getString("name");
        Map += Name == "MmMapIoSpace";
        MapEx += Name == "MmMapIoSpaceEx";
        Unmap += Name == "MmUnmapIoSpace";
      }
      EXPECT_EQ(Map, 2u);
      EXPECT_EQ(MapEx, 4u);
      EXPECT_EQ(Unmap, 6u);
      std::string Trace;
      for (const auto &Message : *Report->getArray("messages")) {
        auto Text = Message.getAsString();
        ASSERT_TRUE(Text);
        EXPECT_EQ(Text->find("failure"), llvm::StringRef::npos);
        Trace += Text->str();
      }
      size_t Position = 0;
      for (const char *Marker : {"mapped unit=1 start=1", "unmapped unit=1",
                                 "mapped unit=1 start=2", "unmapped unit=1"}) {
        Position = Trace.find(Marker, Position);
        ASSERT_NE(Position, std::string::npos) << Marker << "\n" << Trace;
        Position += std::char_traits<char>::length(Marker);
      }
    }
#else
  GTEST_SKIP() << "NEVERD_WDM_RESOURCE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, RejectsInvalidInterruptFactsBeforeImageLoading) {
  for (const auto &[Field, Fact] :
       std::vector<std::pair<const char *, const char *>>{
           {"translated_level", "2"},
           {"translated_affinity", "2"},
           {"mode", "\"level_sensitive\""},
           {"share", "\"shared\""}}) {
    auto Value = llvm::json::parse(R"({"pnp_devices":[{
      "id":"interrupt0","bus":"register_bank","initial_device_power":"D0",
      "initial_system_power":"working","interrupts":[{
      "id":"line0","raw_vector":17,"raw_level":7,"raw_affinity":1,
      "translated_vector":145,"translated_level":5,"translated_affinity":1,
      "mode":"latched","share":"device_exclusive"}]}]})");
    auto Replacement = llvm::json::parse(Fact);
    ASSERT_TRUE(bool(Value));
    ASSERT_TRUE(bool(Replacement));
    auto *IRQ = (*(*Value->getAsObject()->getArray("pnp_devices"))[0]
                      .getAsObject()
                      ->getArray("interrupts"))[0]
                    .getAsObject();
    (*IRQ)[Field] = std::move(*Replacement);
    const std::string Scenario = llvm::formatv("{0}", *Value).str();
    EXPECT_EQ(
        neverd_emulate_driver_scenario_json(
            Session, "missing-interrupt-driver.sys", Scenario.c_str(), nullptr),
        nullptr);
    EXPECT_NE(error().find("driver scenario:"), std::string::npos);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
  for (const char *Request : {R"({"kind":"create","interrupt_events":[]})",
                              R"({"kind":"ioctl","code":0,"interrupt_events":[{
             "after_100ns":0,"device_id":"missing","interrupt_id":"line0"}]})"}) {
    const std::string Scenario =
        std::string("{\"requests\":[") + Request + "]}";
    EXPECT_EQ(
        neverd_emulate_driver_scenario_json(
            Session, "missing-interrupt-driver.sys", Scenario.c_str(), nullptr),
        nullptr);
    EXPECT_NE(error().find("driver scenario:"), std::string::npos);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
}

TEST_F(DriverScenarioPublic,
       CAPIAndCLIExecuteExplicitInterruptAndDpcCompletion) {
#ifdef NEVERD_WDM_INTERRUPT_FIXTURE
  std::vector<const char *> Images{NEVERD_WDM_INTERRUPT_FIXTURE};
#ifdef NEVERD_WDM_INTERRUPT_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_INTERRUPT_CFG_FIXTURE);
#endif
  const std::string Scenario = R"({
    "load_address":"0x190000000","unload":true,
    "pnp_devices":[{"id":"interrupt0","bus":"register_bank",
      "initial_device_power":"D0","initial_system_power":"working",
      "resources":[{"id":"counter","raw_start":"0x200000000",
        "translated_start":"0x300000000","length":"0x1000",
        "registers":[{"offset":0,"width":4,"access":"read_write","value":0}]}],
      "interrupts":[{"id":"line0","raw_vector":17,"raw_level":7,
        "raw_affinity":1,"translated_vector":145,"translated_level":5,
        "translated_affinity":1,"mode":"latched","share":"device_exclusive"}]}],
    "requests":[
      {"kind":"pnp","device_id":"interrupt0","minor":"start",
       "bus_completion":{"status":0,"delay_100ns":11}},
      {"kind":"create","device_id":"interrupt0","file":1},
      {"kind":"ioctl","file":1,"code":"0x222000","output_size":32,
       "interrupt_events":[{"after_100ns":7,"device_id":"interrupt0","interrupt_id":"line0"}]},
      {"kind":"cleanup","file":1},{"kind":"close","file":1},
      {"kind":"pnp","device_id":"interrupt0","minor":"query_remove",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"interrupt0","minor":"remove",
       "bus_completion":{"status":0,"delay_100ns":3}}]})";
  for (const char *Image : Images)
    for (bool CLI : {false, true}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(CLI);
      auto Parsed = llvm::json::parse(
          CLI ? runCLI(Scenario, 0, "success", Image)
              : takeString(neverd_emulate_driver_scenario_json(
                    Session, Image, Scenario.c_str(), nullptr)));
      ASSERT_TRUE(bool(Parsed))
          << llvm::toString(Parsed.takeError()) << error();
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("profile"),
                neverd::emulation::profile::ReportProfile);
      EXPECT_EQ(Report->getString("stop_reason"), "returned")
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 7u);
      for (const auto &Value : *Requests) {
        const auto *Request = Value.getAsObject();
        ASSERT_NE(Request, nullptr);
        EXPECT_EQ(Request->getBoolean("completed"), true);
        EXPECT_EQ(Request->getInteger("io_status"), 0);
      }
      const auto *Ioctl = (*Requests)[2].getAsObject();
      EXPECT_EQ(Ioctl->getInteger("dispatch_status"), 0x103);
      EXPECT_EQ(Ioctl->getInteger("information"), 32);
      EXPECT_EQ(
          Ioctl->getString("output_hex"),
          "0100000001000000010000000100000002000000010000000100000005000000");
      const auto *Interrupts = Report->getArray("interrupts");
      ASSERT_NE(Interrupts, nullptr);
      ASSERT_EQ(Interrupts->size(), 1u);
      const auto *IRQ = Interrupts->front().getAsObject();
      ASSERT_NE(IRQ, nullptr);
      EXPECT_EQ(IRQ->getInteger("source_request_index"), 2);
      EXPECT_EQ(IRQ->getInteger("event_index"), 0);
      EXPECT_EQ(IRQ->getString("device_id"), "interrupt0");
      EXPECT_EQ(IRQ->getString("interrupt_id"), "line0");
      EXPECT_EQ(IRQ->getInteger("epoch"), 1);
      EXPECT_EQ(IRQ->getInteger("due_at_100ns"), 18);
      EXPECT_EQ(IRQ->getInteger("occurred_at_100ns"), 18);
      EXPECT_EQ(IRQ->getInteger("delivered_at_100ns"), 18);
      EXPECT_EQ(IRQ->getInteger("returned_at_100ns"), 18);
      EXPECT_EQ(IRQ->getInteger("return_value"), 1);
      EXPECT_EQ(IRQ->getBoolean("claimed"), true);
      ASSERT_TRUE(IRQ->getString("interrupt_object"));
      EXPECT_NE(IRQ->getString("interrupt_object"), "0x0");
      EXPECT_TRUE(IRQ->get("undelivered_reason")->getAsNull());
      EXPECT_FALSE(IRQ->get("io_status"));
      const auto *Config = Report->getObject("configuration");
      ASSERT_NE(Config, nullptr);
      const auto *Events = Config->getArray("interrupt_events");
      ASSERT_NE(Events, nullptr);
      ASSERT_EQ(Events->size(), 1u);
      EXPECT_EQ(
          Events->front().getAsObject()->getInteger("source_request_index"), 2);
      EXPECT_EQ(Events->front().getAsObject()->getInteger("after_100ns"), 7);
      const auto *Provider =
          Report->getArray("pnp_devices")->front().getAsObject();
      EXPECT_EQ(Provider->getString("pnp_state"), "removed");
      EXPECT_EQ(Provider->getBoolean("provider_present"), false);
      EXPECT_TRUE(Report->getArray("devices")->empty());
      unsigned Connect = 0, Disconnect = 0, Sync = 0, Acquire = 0, Release = 0;
      for (const auto &Value : *Report->getArray("calls")) {
        const auto *Call = Value.getAsObject();
        const auto Name = Call->getString("name");
        Connect += Name == "IoConnectInterrupt";
        Disconnect += Name == "IoDisconnectInterrupt";
        Sync += Name == "KeSynchronizeExecution";
        Acquire += Name == "KeAcquireInterruptSpinLock";
        Release += Name == "KeReleaseInterruptSpinLock";
        if (Name == "IoConnectInterrupt")
          EXPECT_EQ(Call->getArray("arguments")->size(), 11u);
      }
      EXPECT_EQ(Connect, 1u);
      EXPECT_EQ(Disconnect, 1u);
      EXPECT_EQ(Sync, 3u);
      EXPECT_EQ(Acquire, 1u);
      EXPECT_EQ(Release, 1u);
      for (const auto &Message : *Report->getArray("messages")) {
        const auto Text = Message.getAsString();
        ASSERT_TRUE(Text);
        EXPECT_EQ(Text->find("failure"), llvm::StringRef::npos);
      }
    }
#else
  GTEST_SKIP() << "NEVERD_WDM_INTERRUPT_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIAndCLIObserveDmaRamBeforeExplicitInterrupt) {
#ifdef NEVERD_WDM_DMA_FIXTURE
  std::vector<const char *> Images{NEVERD_WDM_DMA_FIXTURE};
#ifdef NEVERD_WDM_DMA_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_DMA_CFG_FIXTURE);
#endif
  const std::string Scenario =
      R"dma({"load_address":"0x190000000","unload":true,"pnp_devices":[{"id":"dma0","bus":"register_bank","initial_device_power":"D0","initial_system_power":"working","interrupts":[{"id":"line0","raw_vector":17,"raw_level":7,"raw_affinity":1,"translated_vector":145,"translated_level":5,"translated_affinity":1,"mode":"latched","share":"device_exclusive"}],"dma":{"address_bits":64,"maximum_length":4096,"map_registers":4,"alignment":1,"logical_base":"0x40000000","logical_length":65536,"scatter_gather":true}}],"requests":[{"kind":"pnp","device_id":"dma0","minor":"start","bus_completion":{"status":0,"delay_100ns":11}},{"kind":"create","device_id":"dma0","file":1},{"kind":"ioctl","file":1,"code":"0x222000","output_size":32,"interrupt_events":[{"after_100ns":7,"device_id":"dma0","interrupt_id":"line0"}],"dma_events":[{"after_100ns":5,"device_id":"dma0","logical_address":"0x40000000","direction":"read_memory","length":16},{"after_100ns":7,"device_id":"dma0","logical_address":"0x40000000","direction":"write_memory","length":16,"data_hex":"a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"}]},{"kind":"cleanup","file":1},{"kind":"close","file":1},{"kind":"pnp","device_id":"dma0","minor":"query_remove","bus_completion":{"status":0,"delay_100ns":0}},{"kind":"pnp","device_id":"dma0","minor":"remove","bus_completion":{"status":0,"delay_100ns":3}}]})dma";
  for (const char *Image : Images)
    for (bool CLI : {false, true}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(CLI);
      auto Parsed = llvm::json::parse(
          CLI ? runCLI(Scenario, 0, "success", Image)
              : takeString(neverd_emulate_driver_scenario_json(
                    Session, Image, Scenario.c_str(), nullptr)));
      ASSERT_TRUE(bool(Parsed))
          << llvm::toString(Parsed.takeError()) << error();
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("profile"),
                neverd::emulation::profile::ReportProfile);
      EXPECT_EQ(Report->getString("stop_reason"), "returned")
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Transfers = Report->getArray("dma_transfers");
      ASSERT_NE(Transfers, nullptr);
      ASSERT_EQ(Transfers->size(), 2u);
      for (unsigned I = 0; I < 2; ++I) {
        const auto *Transfer = (*Transfers)[I].getAsObject();
        ASSERT_NE(Transfer, nullptr);
        EXPECT_EQ(Transfer->getInteger("source_request_index"), 2);
        EXPECT_EQ(Transfer->getInteger("event_index"), I);
        EXPECT_EQ(Transfer->getInteger("epoch"), 1);
        EXPECT_EQ(Transfer->getInteger("length"), 16);
        EXPECT_EQ(Transfer->getInteger("completed_at_100ns"), I ? 18 : 16);
        EXPECT_EQ(Transfer->getInteger("occurred_at_100ns"), I ? 18 : 16);
        EXPECT_EQ(Transfer->getString("logical_address"), "0x40000000");
        EXPECT_EQ(Transfer->getString("direction"),
                  I ? "write_memory" : "read_memory");
        EXPECT_TRUE(Transfer->get("failure_reason")->getAsNull());
        EXPECT_TRUE(Transfer->getString("adapter"));
        EXPECT_TRUE(Transfer->getString("mapping"));
        EXPECT_EQ(Transfer->getString("data_hex"),
                  I ? "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
                    : "1112131415161718191a1b1c1d1e1f20");
      }
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 7u);
      const auto *IO = (*Requests)[2].getAsObject();
      ASSERT_NE(IO, nullptr);
      EXPECT_EQ(IO->getInteger("dispatch_status"), 0x103);
      EXPECT_EQ(IO->getBoolean("completed"), true);
      EXPECT_EQ(
          IO->getString("output_hex"),
          "01000000010000000000000001000000a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");
      const auto *IRQ = Report->getArray("interrupts");
      ASSERT_NE(IRQ, nullptr);
      ASSERT_EQ(IRQ->size(), 1u);
      EXPECT_EQ(IRQ->front().getAsObject()->getInteger("delivered_at_100ns"),
                18);
      const auto *Config = Report->getObject("configuration");
      ASSERT_NE(Config, nullptr);
      ASSERT_NE(Config->getArray("dma_events"), nullptr);
      EXPECT_EQ(Config->getArray("dma_events")->size(), 2u);
      EXPECT_TRUE(Report->getArray("devices")->empty());
    }
#else
  GTEST_SKIP() << "NEVERD_WDM_DMA_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, DMAInvalidFieldFailsAtPublicScenarioBoundary) {
  for (
      const char *Scenario :
      {R"({"requests":[{"kind":"create","dma_events":[]}]})",
       R"({"requests":[{"kind":"ioctl","code":0,"dma_events":[{"after_100ns":0,"device_id":"missing","logical_address":1,"direction":"write_memory","length":4,"data_hex":"00"}]}]})"}) {
    EXPECT_EQ(neverd_emulate_driver_scenario_json(
                  Session, fixture("success").c_str(), Scenario, nullptr),
              nullptr);
    const auto Message = error();
    EXPECT_TRUE(Message.find("DMA") != std::string::npos ||
                Message.find("dma_events") != std::string::npos)
        << Message;
  }
}

TEST_F(DriverScenarioPublic, CAPIAndCLIFlushChannelFragmentsBeforeCompletion) {
#ifdef NEVERD_WDM_DMA_CHANNEL_FIXTURE
  std::vector<const char *> Images{NEVERD_WDM_DMA_CHANNEL_FIXTURE};
#ifdef NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE);
#endif
  const std::string Scenario =
      R"channel({"load_address":"0x190000000","unload":true,"pnp_devices":[{"id":"dma0","bus":"register_bank","initial_device_power":"D0","initial_system_power":"working","interrupts":[{"id":"line0","raw_vector":17,"raw_level":7,"raw_affinity":1,"translated_vector":145,"translated_level":5,"translated_affinity":1,"mode":"latched","share":"device_exclusive"}],"dma":{"address_bits":64,"maximum_length":8192,"map_registers":4,"alignment":1,"logical_base":"0x40000000","logical_length":262144,"scatter_gather":true}}],"requests":[{"kind":"pnp","device_id":"dma0","minor":"start","bus_completion":{"status":0,"delay_100ns":11}},{"kind":"create","device_id":"dma0","file":1},{"kind":"ioctl","file":1,"code":"0x222000","output_size":48,"interrupt_events":[{"after_100ns":7,"device_id":"dma0","interrupt_id":"line0"}],"dma_events":[{"after_100ns":7,"device_id":"dma0","logical_address":"0x40000ff8","direction":"write_memory","length":32,"data_hex":"a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf"}]},{"kind":"cleanup","file":1},{"kind":"close","file":1},{"kind":"pnp","device_id":"dma0","minor":"query_remove","bus_completion":{"status":0,"delay_100ns":0}},{"kind":"pnp","device_id":"dma0","minor":"remove","bus_completion":{"status":0,"delay_100ns":3}}]})channel";
  for (const char *Image : Images)
    for (bool CLI : {false, true}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(CLI);
      auto Parsed = llvm::json::parse(
          CLI ? runCLI(Scenario, 0, "success", Image)
              : takeString(neverd_emulate_driver_scenario_json(
                    Session, Image, Scenario.c_str(), nullptr)));
      ASSERT_TRUE(bool(Parsed))
          << llvm::toString(Parsed.takeError()) << error();
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("profile"),
                neverd::emulation::profile::ReportProfile);
      EXPECT_EQ(Report->getString("stop_reason"), "returned")
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Transfers = Report->getArray("dma_transfers");
      ASSERT_NE(Transfers, nullptr);
      ASSERT_EQ(Transfers->size(), 1u);
      const auto *Transfer = Transfers->front().getAsObject();
      EXPECT_EQ(Transfer->getInteger("source_request_index"), 2);
      EXPECT_EQ(Transfer->getInteger("length"), 32);
      EXPECT_EQ(Transfer->getInteger("completed_at_100ns"), 18);
      uint64_t Address = 0;
      ASSERT_FALSE(Transfer->getString("logical_address")
                       .value_or("")
                       .getAsInteger(0, Address));
      EXPECT_EQ(Address, 0x40000ff8u);
      EXPECT_EQ(
          Transfer->getString("data_hex"),
          "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf");
      EXPECT_TRUE(Transfer->get("failure_reason")->getAsNull());
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 7u);
      const auto *IO = (*Requests)[2].getAsObject();
      EXPECT_EQ(IO->getInteger("dispatch_status"), 0x103);
      EXPECT_EQ(IO->getInteger("information"), 48);
      EXPECT_EQ(
          IO->getString("output_hex"),
          "01000000010000000100000001000000"
          "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf");
      const auto *Interrupts = Report->getArray("interrupts");
      ASSERT_NE(Interrupts, nullptr);
      ASSERT_EQ(Interrupts->size(), 1u);
      EXPECT_EQ(
          Interrupts->front().getAsObject()->getInteger("delivered_at_100ns"),
          18);
      unsigned Allocate = 0, Map = 0, Flush = 0, Free = 0, Cache = 0;
      for (const auto &Value : *Report->getArray("calls")) {
        const auto *Call = Value.getAsObject();
        const auto Name = Call->getString("name");
        Allocate += Name == "AllocateAdapterChannel";
        Map += Name == "MapTransfer";
        Flush += Name == "FlushAdapterBuffers";
        Free += Name == "FreeMapRegisters";
        Cache += Name == "KeFlushIoBuffers";
        if (Name == "AllocateAdapterChannel")
          EXPECT_EQ(Call->getArray("arguments")->size(), 5u);
        if (Name == "MapTransfer" || Name == "FlushAdapterBuffers")
          EXPECT_EQ(Call->getArray("arguments")->size(), 6u);
      }
      EXPECT_EQ(Allocate, 1u);
      EXPECT_EQ(Map, 2u);
      EXPECT_EQ(Flush, 1u);
      EXPECT_EQ(Free, 1u);
      EXPECT_EQ(Cache, 1u);
      EXPECT_TRUE(Report->getArray("devices")->empty());
      for (const auto &Message : *Report->getArray("messages"))
        EXPECT_EQ(Message.getAsString()->find("failure"),
                  llvm::StringRef::npos);
    }
#else
  GTEST_SKIP()
      << "NEVERD_WDM_DMA_CHANNEL_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIAndCLIResumeGenuineConstantExceptionHandler) {
#ifdef NEVERD_WDM_SEH_FIXTURE
  std::vector<const char *> Images{NEVERD_WDM_SEH_FIXTURE};
#ifdef NEVERD_WDM_SEH_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_SEH_CFG_FIXTURE);
#endif
  const std::string Scenario = R"seh({
    "load_address": "0x190000000",
    "unload": true
  })seh";
  for (const char *Image : Images) {
    SCOPED_TRACE(Image);
    const std::string API = takeString(neverd_emulate_driver_scenario_json(
        Session, Image, Scenario.c_str(), nullptr));
    ASSERT_FALSE(API.empty()) << error();
    for (const auto &JSON : {API, runCLI(Scenario, 0, nullptr, Image)}) {
      auto Parsed = llvm::json::parse(JSON);
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("stop_reason"), "returned")
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getInteger("nt_status"), 0);
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Fault = Report->get("fault");
      ASSERT_NE(Fault, nullptr);
      EXPECT_EQ(Fault->kind(), llvm::json::Value::Null);
      const auto *Calls = Report->getArray("calls");
      ASSERT_NE(Calls, nullptr);
      unsigned Raised = 0;
      for (const auto &Value : *Calls) {
        const auto *Call = Value.getAsObject();
        ASSERT_NE(Call, nullptr);
        if (Call->getString("name") != "ExRaiseStatus")
          continue;
        ++Raised;
        const auto *Return = Call->get("result");
        ASSERT_NE(Return, nullptr);
        EXPECT_EQ(Return->kind(), llvm::json::Value::Null);
        EXPECT_NE(Call->getString("detail").value_or("").find(
                      "raised guest exception"),
                  llvm::StringRef::npos);
      }
      EXPECT_EQ(Raised, 1u);
      const auto *Messages = Report->getArray("messages");
      ASSERT_NE(Messages, nullptr);
      bool Caught = false;
      for (const auto &Message : *Messages) {
        auto Text = Message.getAsString();
        ASSERT_TRUE(Text);
        Caught |= Text->contains("direct caught code=c000009a");
        EXPECT_FALSE(Text->contains("failure"));
      }
      EXPECT_TRUE(Caught);
    }
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_SEH_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIAndCLIExecuteGenuineNeitherUserBuffers) {
#ifdef NEVERD_WDM_NEITHER_FIXTURE
  std::vector<const char *> Images{NEVERD_WDM_NEITHER_FIXTURE};
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_NEITHER_CFG_FIXTURE);
#endif
  const std::string Scenario = R"neither({
    "load_address": "0x190000000", "unload": true,
    "requests": [
      {"kind":"create","device":"\\Device\\NeverDNeither"},
      {"kind":"ioctl","code":"0x222003","input":"01020304","output_size":4},
      {"kind":"ioctl","code":"0x222013","input":"01020304","output_size":4},
      {"kind":"cleanup"}, {"kind":"close"}
    ]
  })neither";
  for (const char *Image : Images) {
    SCOPED_TRACE(Image);
    const std::string API = takeString(neverd_emulate_driver_scenario_json(
        Session, Image, Scenario.c_str(), nullptr));
    ASSERT_FALSE(API.empty()) << error();
    for (const auto &JSON : {API, runCLI(Scenario, 0, nullptr, Image)}) {
      auto Parsed = llvm::json::parse(JSON);
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("stop_reason"), "returned")
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Fault = Report->get("fault");
      ASSERT_NE(Fault, nullptr);
      EXPECT_EQ(Fault->kind(), llvm::json::Value::Null);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 5u);
      for (const auto &[Index, Expected] :
           {std::pair<size_t, const char *>{1, "5b58595e"},
            std::pair<size_t, const char *>{2, "02030405"}}) {
        const auto *Request = (*Requests)[Index].getAsObject();
        ASSERT_NE(Request, nullptr);
        EXPECT_EQ(Request->getString("output_hex"), Expected);
        EXPECT_EQ(Request->getInteger("io_status"), 0);
        EXPECT_EQ(Request->getBoolean("completed"), true);
      }
    }
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_NEITHER_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIAndCLIObserveNeitherUserPageFaults) {
#ifdef NEVERD_WDM_NEITHER_FIXTURE
  const std::string Scenario = R"neither({
    "load_address": "0x190000000", "unload": true,
    "requests": [
      {"kind":"create","device":"\\Device\\NeverDNeither"},
      {"kind":"ioctl","code":"0x222003","input":"01020304",
       "output_size":4,"user_input_access":"no_access"},
      {"kind":"cleanup"}, {"kind":"close"}
    ]
  })neither";
  for (const char *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       }) {
    SCOPED_TRACE(Image);
    const std::string API = takeString(neverd_emulate_driver_scenario_json(
        Session, Image, Scenario.c_str(), nullptr));
    ASSERT_FALSE(API.empty()) << error();
    for (const auto &JSON : {API, runCLI(Scenario, 2, nullptr, Image)}) {
      auto Parsed = llvm::json::parse(JSON);
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("stop_reason"), "returned")
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("scenario_success"), false);
      const auto *Configuration = Report->getObject("configuration");
      ASSERT_NE(Configuration, nullptr);
      const auto *Access = Configuration->getArray("user_page_access");
      ASSERT_NE(Access, nullptr);
      ASSERT_EQ(Access->size(), 1u);
      const auto *AccessRow = (*Access)[0].getAsObject();
      ASSERT_NE(AccessRow, nullptr);
      EXPECT_EQ(AccessRow->getInteger("source_request_index"), 1);
      EXPECT_EQ(AccessRow->getString("user_input_access"), "no_access");
      EXPECT_FALSE(AccessRow->get("user_output_access"));
      const auto *Fault = Report->get("fault");
      ASSERT_NE(Fault, nullptr);
      EXPECT_EQ(Fault->kind(), llvm::json::Value::Null);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_GE(Requests->size(), 2u);
      const auto *Request = (*Requests)[1].getAsObject();
      ASSERT_NE(Request, nullptr);
      EXPECT_EQ(Request->getInteger("io_status"), 0xc0000005LL);
      EXPECT_EQ(Request->getBoolean("completed"), true);
    }
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_NEITHER_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIAndCLIExecuteNeitherReadWrite) {
#ifdef NEVERD_WDM_NEITHER_FIXTURE
  const std::string Scenario = R"neither({
    "load_address":"0x190000000", "unload":true,
    "requests":[
      {"kind":"create","device":"\\Device\\NeverDNeither"},
      {"kind":"write","input":"01020304","user_input_access":"read_only"},
      {"kind":"read","output_size":4},
      {"kind":"cleanup"},{"kind":"close"}
    ]
  })neither";
  for (const char *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       }) {
    SCOPED_TRACE(Image);
    const std::string API = takeString(neverd_emulate_driver_scenario_json(
        Session, Image, Scenario.c_str(), nullptr));
    ASSERT_FALSE(API.empty()) << error();
    for (const auto &JSON : {API, runCLI(Scenario, 0, nullptr, Image)}) {
      auto Parsed = llvm::json::parse(JSON);
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("stop_reason"), "returned")
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 5u);
      const auto *Write = (*Requests)[1].getAsObject();
      const auto *Read = (*Requests)[2].getAsObject();
      ASSERT_NE(Write, nullptr);
      ASSERT_NE(Read, nullptr);
      EXPECT_EQ(Write->getInteger("information"), 4);
      EXPECT_EQ(Read->getString("output_hex"), "70717273");
      const auto *Configuration = Report->getObject("configuration");
      ASSERT_NE(Configuration, nullptr);
      const auto *Access = Configuration->getArray("user_page_access");
      ASSERT_NE(Access, nullptr);
      ASSERT_EQ(Access->size(), 1u);
      const auto *AccessRow = (*Access)[0].getAsObject();
      ASSERT_NE(AccessRow, nullptr);
      EXPECT_EQ(AccessRow->getInteger("source_request_index"), 1);
      EXPECT_EQ(AccessRow->getString("user_input_access"), "read_only");
    }
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_NEITHER_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIAndCLIKeepLockedNeitherPagesInWorker) {
#ifdef NEVERD_WDM_NEITHER_FIXTURE
  const std::string Scenario = R"neither({
    "load_address":"0x190000000", "unload":true,
    "requests":[
      {"kind":"create","device":"\\Device\\NeverDNeither"},
      {"kind":"ioctl","code":"0x22201b","input":"01020304","output_size":4},
      {"kind":"cleanup"},{"kind":"close"}
    ]
  })neither";
  for (const char *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       }) {
    SCOPED_TRACE(Image);
    const std::string API = takeString(neverd_emulate_driver_scenario_json(
        Session, Image, Scenario.c_str(), nullptr));
    ASSERT_FALSE(API.empty()) << error();
    for (const auto &JSON : {API, runCLI(Scenario, 0, nullptr, Image)}) {
      auto Parsed = llvm::json::parse(JSON);
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("stop_reason"), "returned")
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 4u);
      const auto *Request = (*Requests)[1].getAsObject();
      ASSERT_NE(Request, nullptr);
      EXPECT_EQ(Request->getInteger("dispatch_status"), 0x103);
      EXPECT_EQ(Request->getInteger("io_status"), 0);
      EXPECT_EQ(Request->getString("output_hex"), "11121314");
    }
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_NEITHER_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIAndCLIRevokeUserVAAfterLockingNeitherPages) {
#ifdef NEVERD_WDM_NEITHER_FIXTURE
  const std::string RevocationScenario = R"neither({
    "load_address":"0x190000000", "unload":true,
    "requests":[
      {"kind":"create","device":"\\Device\\NeverDNeither"},
      {"kind":"ioctl","code":"0x22201b","input":"01020304",
       "output_size":4,"user_unmap_after_dispatch":true},
      {"kind":"cleanup"},{"kind":"close"}
    ]
  })neither";
  const std::string ExitScenario = R"neither({
    "load_address":"0x190000000", "unload":true,
    "requests":[
      {"kind":"create","device":"\\Device\\NeverDNeither",
       "requestor_process_id":4112},
      {"kind":"ioctl","code":"0x22201b","input":"01020304",
       "output_size":4,"requestor_process_id":4112,
       "requestor_exit_after_dispatch":true},
      {"kind":"cleanup","requestor_process_id":4112},
      {"kind":"close","requestor_process_id":4112}
    ]
  })neither";
  for (const char *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       }) {
    SCOPED_TRACE(Image);
    for (const std::string &Scenario : {RevocationScenario, ExitScenario}) {
      SCOPED_TRACE(Scenario);
      const std::string API = takeString(neverd_emulate_driver_scenario_json(
          Session, Image, Scenario.c_str(), nullptr));
      ASSERT_FALSE(API.empty()) << error();
      for (const auto &JSON : {API, runCLI(Scenario, 0, nullptr, Image)}) {
        auto Parsed = llvm::json::parse(JSON);
        ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
        const auto *Report = Parsed->getAsObject();
        ASSERT_NE(Report, nullptr);
        EXPECT_EQ(Report->getString("stop_reason"), "returned")
            << Report->getString("diagnostic").value_or("").str();
        EXPECT_EQ(Report->getBoolean("scenario_success"), true);
        EXPECT_EQ(Report->getBoolean("unload_completed"), true);
        const auto *Requests = Report->getArray("requests");
        ASSERT_NE(Requests, nullptr);
        ASSERT_EQ(Requests->size(), 4u);
        const auto *Request = (*Requests)[1].getAsObject();
        ASSERT_NE(Request, nullptr);
        EXPECT_EQ(Request->getInteger("dispatch_status"), 0x103);
        EXPECT_EQ(Request->getInteger("io_status"), 0);
        EXPECT_EQ(Request->getInteger("information"), 4);
        EXPECT_EQ(Request->getString("output_hex"), "");
        EXPECT_EQ(Request->getInteger("requestor_process_id"),
                  Scenario == ExitScenario ? 4112 : 4096);
      }
    }
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_NEITHER_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIAndCLIRejectRawNeitherWorkerAddress) {
#ifdef NEVERD_WDM_NEITHER_FIXTURE
  const std::string Scenario = R"neither({
    "load_address":"0x190000000",
    "requests":[
      {"kind":"create","device":"\\Device\\NeverDNeither"},
      {"kind":"ioctl","code":"0x22201f","input":"01020304","output_size":4}
    ]
  })neither";
  for (const char *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       }) {
    SCOPED_TRACE(Image);
    const std::string API = takeString(neverd_emulate_driver_scenario_json(
        Session, Image, Scenario.c_str(), nullptr));
    ASSERT_FALSE(API.empty()) << error();
    for (const auto &JSON : {API, runCLI(Scenario, 3, nullptr, Image)}) {
      auto Parsed = llvm::json::parse(JSON);
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("stop_reason"), "model_error");
      EXPECT_NE(Report->getString("diagnostic")
                    .value_or("")
                    .find("user address access requires the requesting "
                          "process"),
                llvm::StringRef::npos);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 2u);
      const auto *Request = (*Requests)[1].getAsObject();
      ASSERT_NE(Request, nullptr);
      EXPECT_EQ(Request->getInteger("dispatch_status"), 0x103);
      EXPECT_EQ(Request->getBoolean("completed"), false);
    }
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_NEITHER_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIAndCLIExecuteWDMPendingCancellation) {
#ifdef NEVERD_WDM_NEITHER_FIXTURE
  const std::string Scenario = R"neither({
    "load_address":"0x190000000", "unload":true,
    "requests":[
      {"kind":"create","device":"\\Device\\NeverDNeither"},
      {"kind":"ioctl","code":"0x222023","input":"01020304",
       "output_size":4,"cancel_after_100ns":0},
      {"kind":"cleanup"},{"kind":"close"}
    ]
  })neither";
  for (const char *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       }) {
    SCOPED_TRACE(Image);
    const std::string API = takeString(neverd_emulate_driver_scenario_json(
        Session, Image, Scenario.c_str(), nullptr));
    ASSERT_FALSE(API.empty()) << error();
    for (const auto &JSON : {API, runCLI(Scenario, 2, nullptr, Image)}) {
      auto Parsed = llvm::json::parse(JSON);
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("stop_reason"), "returned")
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("scenario_success"), false);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 4u);
      const auto *Request = (*Requests)[1].getAsObject();
      ASSERT_NE(Request, nullptr);
      EXPECT_EQ(Request->getInteger("dispatch_status"), 0x103);
      EXPECT_EQ(Request->getInteger("io_status"), 0xc0000120LL);
      EXPECT_EQ(Request->getInteger("cancel_requested_at_100ns"), 0);
      EXPECT_EQ(Request->getBoolean("completed"), true);
      EXPECT_EQ(Request->getString("output_hex"), "");
    }
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_NEITHER_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIAndCLIExecuteRequestorProcessAttachment) {
#ifdef NEVERD_WDM_NEITHER_FIXTURE
  const std::string Scenario = R"neither({
    "load_address":"0x190000000", "unload":true,
    "requests":[
      {"kind":"create","device":"\\Device\\NeverDNeither"},
      {"kind":"ioctl","code":"0x222033","input":"01020304",
       "output_size":4,"requestor_process_id":12336},
      {"kind":"cleanup"},{"kind":"close"}
    ]
  })neither";
  for (const char *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       }) {
    SCOPED_TRACE(Image);
    const std::string API = takeString(neverd_emulate_driver_scenario_json(
        Session, Image, Scenario.c_str(), nullptr));
    ASSERT_FALSE(API.empty()) << error();
    for (const auto &JSON : {API, runCLI(Scenario, 0, nullptr, Image)}) {
      auto Parsed = llvm::json::parse(JSON);
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("stop_reason"), "returned")
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 4u);
      const auto *Request = (*Requests)[1].getAsObject();
      ASSERT_NE(Request, nullptr);
      EXPECT_EQ(Request->getInteger("dispatch_status"), 0x103);
      EXPECT_EQ(Request->getInteger("io_status"), 0);
      EXPECT_EQ(Request->getInteger("information"), 4);
      EXPECT_EQ(Request->getString("output_hex"), "21222324");
      EXPECT_EQ(Request->getInteger("requestor_process_id"), 12336);
    }
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_NEITHER_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIAndCLIExecuteSynchronizationAndSystemThread) {
#ifdef NEVERD_WDM_NEITHER_FIXTURE
  const std::string Template = R"spin({
    "unload":true,
    "requests":[
      {"kind":"create","device":"\\Device\\NeverDNeither"},
      {"kind":"ioctl","code":"0x222037","input":"01020304",
       "output_size":4},
      {"kind":"cleanup"},{"kind":"close"}
    ]
  })spin";
  const std::pair<const char *, const char *> Cases[] = {
      {"0x222037", "5a"}, {"0x22203b", "6b"}, {"0x22203f", "7c"},
      {"0x222043", "8d"}, {"0x222047", "9e"}, {"0x22204b", "af"},
      {"0x22204f", "b1"}};
  for (const auto &[Code, Output] : Cases) {
    std::string Scenario = Template;
    Scenario.replace(Scenario.find("0x222037"), 8, Code);
    for (const char *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                              NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
         }) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Code);
      const std::string API = takeString(neverd_emulate_driver_scenario_json(
          Session, Image, Scenario.c_str(), nullptr));
      ASSERT_FALSE(API.empty()) << error();
      for (const auto &JSON : {API, runCLI(Scenario, 0, nullptr, Image)}) {
        auto Parsed = llvm::json::parse(JSON);
        ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
        const auto *Report = Parsed->getAsObject();
        ASSERT_NE(Report, nullptr);
        EXPECT_EQ(Report->getString("stop_reason"), "returned")
            << Report->getString("diagnostic").value_or("").str();
        EXPECT_EQ(Report->getBoolean("scenario_success"), true);
        EXPECT_EQ(Report->getBoolean("unload_completed"), true);
        const auto *Requests = Report->getArray("requests");
        ASSERT_NE(Requests, nullptr);
        ASSERT_EQ(Requests->size(), 4u);
        const auto *Request = (*Requests)[1].getAsObject();
        ASSERT_NE(Request, nullptr);
        EXPECT_EQ(Request->getInteger("io_status"), 0);
        EXPECT_EQ(Request->getInteger("information"), 1);
        EXPECT_EQ(Request->getString("output_hex"), Output);
      }
    }
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_NEITHER_FIXTURE requires a genuine WDK fixture";
#endif
}

} // namespace
