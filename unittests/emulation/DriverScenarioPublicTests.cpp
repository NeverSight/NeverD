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
       CAPIAndCLIRejectScheduledWDMCancellationBeforeDispatch) {
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
          CLI ? runCLI(Scenario, 3, "success", NEVERD_DRIVER_IO_FIXTURE)
              : takeString(neverd_emulate_driver_scenario_json(
                    Session, NEVERD_DRIVER_IO_FIXTURE, Scenario.c_str(),
                    nullptr)));
      ASSERT_TRUE(bool(Parsed))
          << llvm::toString(Parsed.takeError()) << error();
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("stop_reason"), "model_error");
      EXPECT_NE(
          Report->getString("diagnostic")
              .value_or("")
              .find("scheduled cancellation requires a KMDF control request"),
          llvm::StringRef::npos);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 2u);
      const auto *IO = (*Requests)[1].getAsObject();
      ASSERT_NE(IO, nullptr);
      EXPECT_EQ(IO->getBoolean("completed"), false);
      EXPECT_EQ(IO->getString("irp"), "0x0");
      const auto *Time = IO->get("cancel_requested_at_100ns");
      ASSERT_NE(Time, nullptr);
      EXPECT_EQ(Time->kind(), llvm::json::Value::Null);
      const auto *Dispatch = IO->get("dispatch_status");
      ASSERT_NE(Dispatch, nullptr);
      EXPECT_EQ(Dispatch->kind(), llvm::json::Value::Null);
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
    EXPECT_EQ(Report->getString("profile"), "wdm-x64-scheduled-v6");
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

} // namespace
