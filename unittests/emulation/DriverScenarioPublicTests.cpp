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
#include "AcpiTestFixture.h"
#include "CPUIDPolicyTestFixture.h"
#include "FilterNamesTestFixture.h"
#include "HalDispatchTestFixture.h"
#include "McfgTestFixture.h"
#include "NdisConfigurationTestFixture.h"
#include "NetworkInterfacesTestFixture.h"
#include "PciConfigurationTestFixture.h"
#include "ProcessImageTestFixture.h"
#include "ProcessPebTestFixture.h"
#include "RegistryEnumerationTestFixture.h"
#include "RegistryHivesTestFixture.h"
#include "ServiceImageTestFixture.h"
#include "SmbiosTestFixture.h"
#include "fixtures/driver_kmdf_child_wake_test.h"
#include "fixtures/driver_kmdf_interrupt_test.h"
#include "fixtures/driver_nested_user.h"
#include "fixtures/driver_seh_test.h"
#include "fixtures/driver_wdm_owned_irp_test.h"
#include "gtest/gtest.h"

#include "neverd/emulation/DriverProfile.h"
#include "neverd/sdk/NeverDCAPIEmulation.h"
#include "neverd/sdk/NeverDCAPIQuery.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
#define NEVERD_DRIVER_PUBLIC_CLI_TEXT(Name, Value)                             \
  constexpr char Name[] = Value;
#include "DriverPublicCLICases.def"
#undef NEVERD_DRIVER_PUBLIC_CLI_TEXT

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
                     const char *Name = "success", const char *Image = nullptr,
                     const char *Contract = nullptr) {
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
        CompatibilityBackend + " --scenario " +
        neverd::test::shellQuote(Path.string()) +
        (Contract
             ? " --execution-contract " + neverd::test::shellQuote(Contract)
             : "") +
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

TEST_F(DriverScenarioPublic, DeclaredFileSystemStateReachesCABIAndCLIOnStop) {
  for (char Mode : {'S', 'W', 'C', 'P', 'X', 'U', 'R', 'Q'}) {
    SCOPED_TRACE(Mode);
    const std::string Scenario =
        std::string(R"({"service_name":"DeclaredFileSystem)") + Mode +
        R"(","unload":true,"filesystem":[{"device_name":"\\Device\\DeclaredVolume",)"
        R"("access_policy":"full_access","sector_size":4096,"alignment_mask":511, "nodes":[)"
        R"({"path":"\\","kind":"directory","file_id":0,"attributes":16,"allocation_size":0,)"
        R"("creation_time_100ns":0,"last_access_time_100ns":0,"last_write_time_100ns":0,"change_time_100ns":0},)"
        R"({"path":"\\Data","kind":"file","file_id":18446744073709551615,"attributes":32,)"
        R"("allocation_size":4096,"creation_time_100ns":1,"last_access_time_100ns":2,)"
        R"("last_write_time_100ns":3,"change_time_100ns":4,"data":"007f80ff"}]}]})";
    const auto FromC = takeString(neverd_emulate_driver_scenario_json(
        Session, fixture("driver_filesystem_provider").c_str(),
        Scenario.c_str(), nullptr));
    ASSERT_FALSE(FromC.empty()) << error();
    // X handles a cancelled CREATE; Q handles a short metadata buffer. The
    // report and CLI exit preserve those failed requests after driver success.
    const auto FromCLI = runCLI(Scenario,
                                Mode == 'W'                  ? 3
                                : Mode == 'X' || Mode == 'Q' ? 2
                                                             : 0,
                                "driver_filesystem_provider");
    for (const auto &Text : {FromC, FromCLI}) {
      auto Parsed = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("stop_reason"),
                Mode == 'W' ? "model_error" : "returned");
      EXPECT_EQ(Report->getBoolean("unload_completed"), Mode != 'W');
      if (Mode != 'S' && Mode != 'W') {
        const auto *Requests = Report->getArray("requests");
        ASSERT_NE(Requests, nullptr);
        ASSERT_FALSE(Requests->empty());
        for (const auto &Value : *Requests) {
          const auto &Request = *Value.getAsObject();
          EXPECT_EQ(Request.get("file")->kind(), llvm::json::Value::Null);
          EXPECT_TRUE(Request.getString("file_object"));
          EXPECT_TRUE(Request.getInteger("file_open_operation"));
          EXPECT_TRUE(Request.getString("filesystem_device"));
          EXPECT_TRUE(Request.get("filesystem_file_id"));
          EXPECT_EQ(Request.getBoolean("completed"), true);
        }
        if (Mode == 'Q') {
          struct ExpectedQuery {
            unsigned Class;
            llvm::StringRef Bytes;
          };
          const ExpectedQuery Metadata[] = {
              {4, "010000000000000002000000000000000300000000000000"
                  "04000000000000002000000000000000"},
              {5, "001000000000000004000000000000000100000000000000"},
              {6, "ffffffffffffffff"},
              {9, "0a0000005c004400610074006100"},
              {34, "010000000000000002000000000000000300000000000000"
                   "040000000000000000100000000000000400000000000000"
                   "2000000000000000"},
              {35, "2000000000000000"}};
          unsigned Index = 0;
          for (const auto &Value : *Requests) {
            const auto &Request = *Value.getAsObject();
            if (Request.getString("kind") != "query_information")
              continue;
            ASSERT_LT(Index, 16u);
            ExpectedQuery Expected = Metadata[Index % 6];
            uint32_t Status = 0;
            if (Index == 12) {
              Expected = {5, ""};
              Status = 0xc0000004;
            } else if (Index == 13) {
              Expected = {9, "0a0000005c004400"};
              Status = 0x80000005;
            } else if (Index >= 14) {
              Expected = Index == 14 ? ExpectedQuery{16, "20000000"}
                                     : ExpectedQuery{17, "ff010000"};
            }
            EXPECT_EQ(Request.getString("origin"),
                      Index < 6 ? "IoQueryFileInformation"
                                : "ZwQueryInformationFile");
            EXPECT_EQ(Request.getInteger("file_information_class"),
                      Expected.Class);
            EXPECT_EQ(Request.getInteger("io_status"), Status);
            EXPECT_EQ(Request.getInteger("information"),
                      Expected.Bytes.size() / 2);
            EXPECT_EQ(Request.getString("output_hex"), Expected.Bytes);
            ++Index;
          }
          EXPECT_EQ(Index, 16u);
        }
      }
      const auto *State = Report->getArray("filesystem");
      ASSERT_NE(State, nullptr);
      ASSERT_EQ(State->size(), 1u);
      const auto &Volume = *State->front().getAsObject();
      EXPECT_NE(Volume.getString("device_object"), "0x0");
      const auto *Nodes = Volume.getArray("nodes");
      ASSERT_NE(Nodes, nullptr);
      ASSERT_EQ(Nodes->size(), 2u);
      const auto &File = *(*Nodes)[1].getAsObject();
      EXPECT_EQ(File.getString("data"), "007f80ff");
      EXPECT_EQ(File.getInteger("end_of_file"), 4);
      EXPECT_EQ(File.getInteger("change_time_100ns"), 4);
      EXPECT_EQ(File.get("file_id")->getAsUINT64(), UINT64_MAX);
      const auto *Configuration = Report->getObject("configuration");
      ASSERT_NE(Configuration, nullptr);
      const auto *Input = Configuration->getArray("filesystem");
      ASSERT_NE(Input, nullptr);
      const auto *InputVolume = Input->front().getAsObject();
      EXPECT_EQ(InputVolume->get("device_object"), nullptr);
      EXPECT_EQ((*InputVolume->getArray("nodes"))[1].getAsObject()->get(
                    "end_of_file"),
                nullptr);
    }
  }
}

TEST_F(DriverScenarioPublic, FileSystemOmissionEmptyAndInvalidRemainDistinct) {
  for (bool Empty : {false, true}) {
    const auto Scenario = Empty ? R"({"filesystem":[]})" : "{}";
    const auto Text = takeString(neverd_emulate_driver_scenario_json(
        Session, fixture("success").c_str(), Scenario, nullptr));
    ASSERT_FALSE(Text.empty()) << error();
    auto Parsed = llvm::json::parse(Text);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    const auto *State = Parsed->getAsObject()->get("filesystem");
    ASSERT_NE(State, nullptr);
    EXPECT_EQ(State->getAsArray() != nullptr, Empty);
    if (Empty)
      EXPECT_TRUE(State->getAsArray()->empty());
    else
      EXPECT_EQ(State->kind(), llvm::json::Value::Null);
  }
  for (const auto *Scenario :
       {R"({"filesystem":null})", R"({"filesystem":[{}]})"}) {
    EXPECT_EQ(neverd_emulate_driver_scenario_json(Session, "missing.sys",
                                                  Scenario, nullptr),
              nullptr);
    EXPECT_NE(error().find("filesystem"), std::string::npos);
  }
}

TEST_F(DriverScenarioPublic, HalCachedAcpiPointerCallsReachCAPIAndCLI) {
  const auto Scenario =
      llvm::formatv("{0}", llvm::json::Value(
                               neverd::emulation::test::halDispatchScenario()))
          .str();
  for (const auto &Text : {takeString(neverd_emulate_driver_scenario_json(
                               Session, fixture("driver_hal_dispatch").c_str(),
                               Scenario.c_str(), nullptr)),
                           runCLI(Scenario, 0, "driver_hal_dispatch")}) {
    auto J = llvm::json::parse(Text);
    ASSERT_TRUE(bool(J)) << llvm::toString(J.takeError()) << error();
    const auto *R = J->getAsObject();
    ASSERT_NE(R, nullptr);
    EXPECT_EQ(R->getBoolean("scenario_success"), true);
    EXPECT_EQ(R->getBoolean("unload_completed"), true);
    unsigned Calls = 0;
    for (const auto &V : *R->getArray("calls"))
      if (V.getAsObject()->getString("name") == "HalGetCachedAcpiTable")
        ++Calls;
    EXPECT_EQ(Calls, 8u);
  }
}
TEST_F(DriverScenarioPublic, AvlGuestCallbacksReachCAPIAndCLI) {
  const char *Scenario =
      R"({"windows_version":{"major":10,"minor":0,"build":19041},"unload":true})";
  for (const auto &Text :
       {takeString(neverd_emulate_driver_scenario_json(
            Session, fixture("driver_avl").c_str(), Scenario, nullptr)),
        runCLI(Scenario, 0, "driver_avl")}) {
    auto J = llvm::json::parse(Text);
    ASSERT_TRUE(bool(J)) << llvm::toString(J.takeError()) << error();
    const auto *R = J->getAsObject();
    ASSERT_NE(R, nullptr);
    EXPECT_EQ(R->getBoolean("scenario_success"), true);
    EXPECT_EQ(R->getBoolean("unload_completed"), true);
    bool GuestPhase = false;
    for (const auto &Call : *R->getArray("calls")) {
      auto Phase = Call.getAsObject()->getString("phase");
      GuestPhase |= Phase && Phase->starts_with("callback:avl:");
    }
    EXPECT_TRUE(GuestPhase);
  }
}
TEST_F(DriverScenarioPublic, ExternalRegistryEventsReachCAPIAndCLI) {
  const std::string Scenario = R"({
    "windows_version":{"major":10,"minor":0,"build":19041},
    "unload":true,
    "registry":[{"path":"\\Registry\\Machine\\Software\\NeverD",
                 "values":[{"name":"Mode","type":4,"data":"01000000"}]}],
    "registry_events":[
      {"kind":"open","before_request":0,"handle":1,
       "path":"\\Registry\\Machine\\Software\\NeverD","desired_access":3},
      {"kind":"set_value","before_request":0,"handle":1,
       "value_name":"Mode","type":4,"data":"34120000"},
      {"kind":"query_value","before_request":0,"handle":1,
       "value_name":"Mode","output_size":16},
      {"kind":"close","before_request":0,"handle":1}
    ]})";
  const auto API = takeString(neverd_emulate_driver_scenario_json(
      Session, fixture("driver_registry_events").c_str(), Scenario.c_str(),
      nullptr));
  ASSERT_FALSE(API.empty()) << error();
  for (const auto &Text :
       {API, runCLI(Scenario, 0, "driver_registry_events")}) {
    auto J = llvm::json::parse(Text);
    ASSERT_TRUE(bool(J)) << llvm::toString(J.takeError());
    const auto *O = J->getAsObject();
    ASSERT_NE(O, nullptr);
    EXPECT_EQ(O->getBoolean("scenario_success"), true)
        << O->getString("diagnostic").value_or("").str();
    EXPECT_EQ(O->getBoolean("unload_completed"), true);
    const auto *Events = O->getArray("registry_events");
    ASSERT_NE(Events, nullptr);
    ASSERT_EQ(Events->size(), 4u);
    EXPECT_EQ((*Events)[2].getAsObject()->getString("output_hex"),
              "00000000040000000400000034120000");
    EXPECT_EQ(
        O->getObject("configuration")->getArray("registry_events")->size(), 4u);
  }
}
TEST_F(DriverScenarioPublic, RelativeRegistryActorsReachCAPIAndCLI) {
  const std::string Scenario = R"({
    "windows_version":{"major":10,"minor":0,"build":19041},"unload":true,
    "registry":[{"path":"\\Registry\\Machine\\Software\\NeverD",
                 "values":[{"name":"Mode","type":4,"data":"01000000"}]}],
    "process_events":[{"kind":"create","process_id":100,"parent_id":4}],
    "registry_events":[
      {"kind":"open","before_request":0,"actor_process_id":100,"handle":1,
       "path":"\\Registry\\Machine","desired_access":3},
      {"kind":"open","before_request":0,"actor_process_id":100,"handle":2,
       "path":"Software\\NeverD","root_handle":1,"desired_access":3},
      {"kind":"set_value","before_request":0,"actor_process_id":100,"handle":2,
       "value_name":"Mode","type":4,"data":"07000000"},
      {"kind":"query_value","before_request":0,"actor_process_id":100,"handle":2,
       "value_name":"Mode","output_size":16},
      {"kind":"close","before_request":0,"actor_process_id":100,"handle":2},
      {"kind":"close","before_request":0,"actor_process_id":100,"handle":1}
    ]})";
  const auto API = takeString(neverd_emulate_driver_scenario_json(
      Session, fixture("driver_registry_events_actor").c_str(),
      Scenario.c_str(), nullptr));
  ASSERT_FALSE(API.empty()) << error();
  for (const auto &Text :
       {API, runCLI(Scenario, 0, "driver_registry_events_actor")}) {
    auto J = llvm::json::parse(Text);
    ASSERT_TRUE(bool(J)) << llvm::toString(J.takeError());
    const auto *O = J->getAsObject();
    ASSERT_NE(O, nullptr);
    EXPECT_EQ(O->getBoolean("scenario_success"), true)
        << O->getString("diagnostic").value_or("").str();
    const auto *E = O->getArray("registry_events");
    ASSERT_NE(E, nullptr);
    ASSERT_EQ(E->size(), 6u);
    EXPECT_EQ((*E)[3].getAsObject()->getString("output_hex"),
              "00000000040000000400000007000000");
    EXPECT_EQ((*E)[1].getAsObject()->getInteger("actor_process_id"), 100);
    EXPECT_EQ((*E)[1].getAsObject()->getInteger("root_handle"), 1);
  }
}
TEST_F(DriverScenarioPublic, RequestDelaysWakeActualWorkersThroughCAPIAndCLI) {
  const std::string Scenario =
      R"({"callback_drain_policy":"foreground-ready","unload":true,
    "requests":[
      {"kind":"create","device":"\\Device\\NeverDIO","delay_before_100ns":5000},
      {"kind":"ioctl","code":2236452,"output_size":4,"delay_before_100ns":5000},
      {"kind":"ioctl","code":2236452,"output_size":4,"delay_before_100ns":20000},
      {"kind":"cleanup"},{"kind":"close"}]})";
  const auto API = takeString(neverd_emulate_driver_scenario_json(
      Session, fixture("driver_resident_worker_pacing").c_str(),
      Scenario.c_str(), nullptr));
  ASSERT_FALSE(API.empty()) << error();
  for (const auto &Text :
       {API, runCLI(Scenario, 0, "driver_resident_worker_pacing")}) {
    auto J = llvm::json::parse(Text);
    ASSERT_TRUE(bool(J)) << llvm::toString(J.takeError());
    const auto *O = J->getAsObject();
    ASSERT_NE(O, nullptr);
    EXPECT_EQ(O->getBoolean("scenario_success"), true)
        << O->getString("diagnostic").value_or("").str();
    const auto *D = O->getArray("request_delays");
    ASSERT_NE(D, nullptr);
    ASSERT_EQ(D->size(), 3u);
    EXPECT_EQ((*D)[0].getAsObject()->getInteger("completed_at_100ns"), 5000);
    EXPECT_EQ((*D)[2].getAsObject()->getInteger("completed_at_100ns"), 30000);
    const auto *R = O->getArray("requests");
    ASSERT_NE(R, nullptr);
    ASSERT_EQ(R->size(), 5u);
    EXPECT_EQ((*R)[1].getAsObject()->getString("output_hex"), "02000000");
    EXPECT_EQ((*R)[2].getAsObject()->getString("output_hex"), "04000000");
  }
}
TEST_F(DriverScenarioPublic, ExtendedProcessCreationReachesCAPIAndCLI) {
  const std::string Scenario = R"({
    "windows_version":{"major":10,"minor":0,"build":19041},"unload":true,
    "process_events":[
      {"kind":"create","process_id":100,"parent_id":4,"before_request":0,
       "create_info":{"file_open_name_available":true,
        "image_file_name":"\\??\\C:\\NeverDCreate.exe",
        "command_line":"NeverDCreate.exe --test"}},
      {"kind":"exit","process_id":100,"parent_id":4,"before_request":0}
    ]})";
  const auto API = takeString(neverd_emulate_driver_scenario_json(
      Session, fixture("driver_process_create").c_str(), Scenario.c_str(),
      nullptr));
  ASSERT_FALSE(API.empty()) << error();
  for (const auto &Text : {API, runCLI(Scenario, 0, "driver_process_create")}) {
    auto J = llvm::json::parse(Text);
    ASSERT_TRUE(bool(J)) << llvm::toString(J.takeError());
    const auto *O = J->getAsObject();
    ASSERT_NE(O, nullptr);
    EXPECT_EQ(O->getBoolean("scenario_success"), true)
        << O->getString("diagnostic").value_or("").str();
    EXPECT_EQ(O->getBoolean("unload_completed"), true);
    const auto *E = O->getArray("process_events");
    ASSERT_NE(E, nullptr);
    ASSERT_EQ(E->size(), 2u);
    EXPECT_EQ((*E)[0].getAsObject()->getInteger("creation_status"), 0);
    EXPECT_EQ((*E)[0].getAsObject()->getArray("delivered_routines")->size(),
              3u);
  }
}

TEST_F(DriverScenarioPublic, SharedChildDriverServicesReachCAPIAndCLI) {
  auto O = neverd::emulation::serviceOptions();
  llvm::json::Array Registry, Images;
  for (const auto &K : *O.Registry) {
    llvm::json::Array Values;
    for (const auto &V : K.Values)
      Values.push_back(llvm::json::Object{{"name", V.Name},
                                          {"type", V.Type},
                                          {"data", llvm::toHex(V.Data, true)}});
    Registry.push_back(
        llvm::json::Object{{"path", K.Path}, {"values", std::move(Values)}});
  }
  for (const auto &I : *O.ServiceImages)
    Images.push_back(llvm::json::Object{{"service_name", I.ServiceName},
                                        {"image_path", I.ImagePath},
                                        {"source", I.Source.generic_string()},
                                        {"load_address", I.LoadAddress}});
  llvm::json::Value Input = llvm::json::Object{
      {"windows_version",
       llvm::json::Object{{"major", 10}, {"minor", 0}, {"build", 19041}}},
      {"unload", true},
      {"registry", std::move(Registry)},
      {"service_images", std::move(Images)}};
  auto Scenario = llvm::formatv("{0}", Input).str();
  for (const auto &Text :
       {takeString(neverd_emulate_driver_scenario_json(
            Session, fixture("driver_service_lifecycle").c_str(),
            Scenario.c_str(), nullptr)),
        runCLI(Scenario, 0, "driver_service_lifecycle")}) {
    auto J = llvm::json::parse(Text);
    ASSERT_TRUE(bool(J)) << llvm::toString(J.takeError()) << error();
    const auto *R = J->getAsObject();
    ASSERT_NE(R, nullptr);
    EXPECT_EQ(R->getBoolean("scenario_success"), true)
        << R->getString("diagnostic").value_or("").str();
    EXPECT_EQ(R->getBoolean("unload_completed"), true);
    ASSERT_NE(R->getObject("configuration")->getArray("service_images"),
              nullptr);
    EXPECT_EQ(R->getObject("configuration")->getArray("service_images")->size(),
              2u);
    unsigned Entries = 0, Unloads = 0;
    for (const auto &M : *R->getArray("messages")) {
      Entries += M.getAsString() == "child service entry executed";
      Unloads += M.getAsString() == "child service unload executed";
    }
    EXPECT_EQ(Entries, 2u);
    EXPECT_EQ(Unloads, 2u);
  }
}
TEST_F(DriverScenarioPublic, NdisConfigurationOwnershipReachesCAPIAndCLI) {
  const auto Scenario =
      llvm::formatv("{0}",
                    llvm::json::Value(
                        neverd::emulation::test::ndisConfigurationScenario()))
          .str();
  for (const auto &Text :
       {takeString(neverd_emulate_driver_scenario_json(
            Session, fixture("driver_ndis_configuration").c_str(),
            Scenario.c_str(), nullptr)),
        runCLI(Scenario, 0, "driver_ndis_configuration")}) {
    auto J = llvm::json::parse(Text);
    ASSERT_TRUE(bool(J)) << llvm::toString(J.takeError()) << error();
    const auto *R = J->getAsObject();
    ASSERT_NE(R, nullptr);
    EXPECT_EQ(R->getBoolean("scenario_success"), true);
    EXPECT_EQ(R->getBoolean("unload_completed"), true);
    const auto *C =
        R->getObject("configuration")->getObject("ndis_configuration");
    ASSERT_NE(C, nullptr);
    EXPECT_EQ(C->getInteger("version"), 0x60054);
    EXPECT_EQ((*C->getArray("bindings"))[0]
                  .getAsObject()
                  ->get("handle")
                  ->getAsUINT64(),
              neverd::emulation::test::NdisBinding);
  }
}
TEST_F(DriverScenarioPublic, NdisBadBindingsFailBeforeImageLoading) {
  const char *Scenario = R"({"ndis_configuration":{"bindings":[]}})";
  auto Text = takeString(neverd_emulate_driver_scenario_json(
      Session, (Directory / "absent.sys").string().c_str(), Scenario, nullptr));
  EXPECT_TRUE(Text.empty());
  EXPECT_NE(error().find("ndis_configuration"), std::string::npos);
  EXPECT_TRUE(runCLI(Scenario, 1, "success", "does-not-exist.sys").empty());
}
TEST_F(DriverScenarioPublic, NetioObservedInterfaceRowsReachCAPIAndCLI) {
  const auto Scenario =
      llvm::formatv("{0}",
                    llvm::json::Value(
                        neverd::emulation::test::networkInterfacesScenario()))
          .str();
  for (const auto &Text :
       {takeString(neverd_emulate_driver_scenario_json(
            Session, fixture("driver_netio_interfaces").c_str(),
            Scenario.c_str(), nullptr)),
        runCLI(Scenario, 0, "driver_netio_interfaces")}) {
    auto J = llvm::json::parse(Text);
    ASSERT_TRUE(bool(J)) << llvm::toString(J.takeError()) << error();
    const auto *R = J->getAsObject();
    ASSERT_NE(R, nullptr);
    EXPECT_EQ(R->getBoolean("scenario_success"), true);
    EXPECT_EQ(R->getBoolean("unload_completed"), true);
    const auto *Rows = R->getObject("configuration")
                           ->getObject("network_interfaces")
                           ->getArray("raw");
    ASSERT_NE(Rows, nullptr);
    ASSERT_EQ(Rows->size(), 2u);
    EXPECT_EQ((*Rows)[0].getAsString(),
              llvm::toHex(neverd::emulation::test::networkRow(1, 1), true));
  }
}
TEST_F(DriverScenarioPublic, NetioBadFactsFailBeforeImageLoading) {
  const char *Scenario = R"({"network_interfaces":{"normal":[]}})";
  auto Text = takeString(neverd_emulate_driver_scenario_json(
      Session, (Directory / "absent.sys").string().c_str(), Scenario, nullptr));
  EXPECT_TRUE(Text.empty());
  EXPECT_NE(error().find("network_interfaces"), std::string::npos);
  EXPECT_TRUE(runCLI(Scenario, 1, "success", "does-not-exist.sys").empty());
}
TEST_F(DriverScenarioPublic,
       FilterManagerNamesUseScopedImportsThroughCAPIAndCLI) {
  const auto Scenario =
      llvm::formatv("{0}", llvm::json::Value(
                               neverd::emulation::test::filterNamesScenario()))
          .str();
  for (const auto &Text : {takeString(neverd_emulate_driver_scenario_json(
                               Session, fixture("driver_filter_names").c_str(),
                               Scenario.c_str(), nullptr)),
                           runCLI(Scenario, 0, "driver_filter_names")}) {
    auto J = llvm::json::parse(Text);
    ASSERT_TRUE(bool(J)) << llvm::toString(J.takeError()) << error();
    const auto *R = J->getAsObject();
    ASSERT_NE(R, nullptr);
    EXPECT_EQ(R->getBoolean("scenario_success"), true);
    EXPECT_EQ(R->getBoolean("unload_completed"), true);
    EXPECT_EQ(
        R->getObject("configuration")->getBoolean("filter_manager_file_names"),
        true);
    unsigned Calls = 0;
    for (const auto &Entry : *R->getArray("calls")) {
      auto Name = Entry.getAsObject()->getString("name");
      if (Name && Name->starts_with("Flt"))
        ++Calls;
    }
    EXPECT_EQ(Calls, 11u);
  }
}
TEST_F(DriverScenarioPublic,
       FilterManagerBadProviderFactsFailBeforeImageLoading) {
  const char *Scenario = R"({"filter_manager_file_names":true})";
  auto Text = takeString(neverd_emulate_driver_scenario_json(
      Session, (Directory / "absent.sys").string().c_str(), Scenario, nullptr));
  EXPECT_TRUE(Text.empty());
  EXPECT_NE(error().find("filter_manager_file_names"), std::string::npos);
  EXPECT_TRUE(runCLI(Scenario, 1, "success", "does-not-exist.sys").empty());
}
TEST_F(DriverScenarioPublic, RegistryHiveCommitReachesCAPIAndCLI) {
  const auto Scenario =
      llvm::formatv(
          "{0}",
          llvm::json::Value(neverd::emulation::test::registryHivesScenario()))
          .str();
  for (const auto &Text :
       {takeString(neverd_emulate_driver_scenario_json(
            Session, fixture("driver_registry_hives").c_str(), Scenario.c_str(),
            nullptr)),
        runCLI(Scenario, 0, "driver_registry_hives")}) {
    auto J = llvm::json::parse(Text);
    ASSERT_TRUE(bool(J)) << llvm::toString(J.takeError()) << error();
    const auto *R = J->getAsObject();
    ASSERT_NE(R, nullptr);
    EXPECT_EQ(R->getBoolean("scenario_success"), true);
    EXPECT_EQ(R->getBoolean("unload_completed"), true);
    const auto *Hives = R->getArray("registry_hives");
    ASSERT_NE(Hives, nullptr);
    ASSERT_EQ(Hives->size(), 2u);
    const auto *H = (*Hives)[0].getAsObject();
    ASSERT_NE(H, nullptr);
    EXPECT_EQ(H->getInteger("flush_count"), 1);
    EXPECT_EQ(H->getString("storage"), "memory-backed");
    EXPECT_EQ((*Hives)[1].getAsObject()->getInteger("flush_count"), 0);
    const auto Find = [](const llvm::json::Array &Keys, const char *Suffix) {
      for (const auto &Entry : Keys) {
        const auto *K = Entry.getAsObject();
        auto Path = K->getString("path");
        if (Path && Path->ends_with(Suffix))
          return (*K->getArray("values"))[0]
              .getAsObject()
              ->getString("data")
              ->str();
      }
      return std::string("missing");
    };
    EXPECT_EQ(Find(*H->getArray("committed"), "\\A"), "0a000000");
    EXPECT_EQ(Find(*H->getArray("committed"), "\\B"), "14000000");
    EXPECT_EQ(Find(*R->getArray("registry"), "\\B"), "63000000");
    EXPECT_EQ(Find(*(*Hives)[1].getAsObject()->getArray("committed"), "\\C"),
              "03000000");
    ASSERT_NE(R->getObject("configuration")->getArray("registry_hives"),
              nullptr);
  }
}
TEST_F(DriverScenarioPublic,
       RegistryHiveMalformedMountFailsBeforeImageLoading) {
  const char *Scenario =
      R"({"registry_hives":[{"path":"x","storage":"host"}]})";
  auto Text = takeString(neverd_emulate_driver_scenario_json(
      Session, (Directory / "absent.sys").string().c_str(), Scenario, nullptr));
  EXPECT_TRUE(Text.empty());
  EXPECT_NE(error().find("registry_hives"), std::string::npos);
  EXPECT_TRUE(runCLI(Scenario, 1, "success", "does-not-exist.sys").empty());
}
TEST_F(DriverScenarioPublic, ControlBuilderOwnershipReachesCAPIAndCLI) {
  const char *Scenario =
      R"({"windows_version":{"major":10,"minor":0,"build":19041},"irp_io_priority_hint":2,"unload":true})";
  for (const auto &Text :
       {takeString(neverd_emulate_driver_scenario_json(
            Session, fixture("driver_control_builder").c_str(), Scenario,
            nullptr)),
        runCLI(Scenario, 0, "driver_control_builder")}) {
    auto J = llvm::json::parse(Text);
    ASSERT_TRUE(bool(J)) << llvm::toString(J.takeError()) << error();
    auto *R = J->getAsObject();
    ASSERT_NE(R, nullptr);
    EXPECT_EQ(R->getBoolean("scenario_success"), true);
    EXPECT_EQ(R->getBoolean("unload_completed"), true);
    EXPECT_EQ(R->getObject("configuration")->getInteger("irp_io_priority_hint"),
              2);
    const auto *Requests = R->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 24u);
    for (const auto &Entry : *Requests) {
      const auto *Q = Entry.getAsObject();
      ASSERT_NE(Q, nullptr);
      EXPECT_EQ(Q->getString("origin"), "IoBuildDeviceIoControlRequest");
      EXPECT_EQ(Q->getBoolean("completed"), true);
      EXPECT_EQ(Q->getInteger("information"), 8);
      ASSERT_NE(Q->get("file"), nullptr);
      EXPECT_TRUE(Q->get("file")->getAsNull());
    }
  }
}
TEST_F(DriverScenarioPublic, ControlBuilderBadPriorityFailsBeforeImageLoading) {
  const char *Scenario = R"({"irp_io_priority_hint":5})";
  auto Text = takeString(neverd_emulate_driver_scenario_json(
      Session, (Directory / "absent.sys").string().c_str(), Scenario, nullptr));
  EXPECT_TRUE(Text.empty());
  EXPECT_NE(error().find("irp_io_priority_hint"), std::string::npos);
  EXPECT_TRUE(runCLI(Scenario, 1, "success", "does-not-exist.sys").empty());
}
TEST_F(DriverScenarioPublic, PciConfigurationReachesCAPIAndCLI) {
  const auto Scenario =
      llvm::formatv("{0}", llvm::json::Value(
                               neverd::emulation::pciConfigurationScenario()))
          .str();
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_pci_configuration").c_str(),
                        Scenario.c_str(), nullptr)),
                    runCLI(Scenario, 0, "driver_pci_configuration")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    auto *P = Report->getObject("pci_configuration");
    ASSERT_NE(P, nullptr);
    auto *D = P->getArray("devices");
    ASSERT_NE(D, nullptr);
    ASSERT_EQ(D->size(), 1u);
    auto Hex = (*D)[0].getAsObject()->getString("config_hex");
    ASSERT_TRUE(Hex);
    EXPECT_EQ(Hex->substr(0x100 * 2, 8), "aabbccdd");
    auto *Initial =
        Report->getObject("configuration")->getObject("pci_configuration");
    ASSERT_NE(Initial, nullptr);
    auto Original = (*Initial->getArray("devices"))[0].getAsObject()->getString(
        "config_hex");
    ASSERT_TRUE(Original);
    EXPECT_EQ(Original->substr(0x100 * 2, 8), "00010203");
  }
}
TEST_F(DriverScenarioPublic, PciMalformedFactsFailBeforeImageLoading) {
  const char *Scenario =
      R"({"windows_version":{"major":10,"minor":0,"build":19041},"pci_configuration":{"minimum_bus_number":2,"maximum_bus_number":1,"devices":[]}})";
  auto Text = takeString(neverd_emulate_driver_scenario_json(
      Session, (Directory / "absent.sys").string().c_str(), Scenario, nullptr));
  EXPECT_TRUE(Text.empty());
  EXPECT_NE(error().find("pci_configuration"), std::string::npos);
  EXPECT_TRUE(runCLI(Scenario, 1, "success", "does-not-exist.sys").empty());
}
TEST_F(DriverScenarioPublic, DynamicImageSectionsReachCAPIAndCLI) {
  const auto Source = neverd::emulation::processImageFixture();
  const std::string Scenario =
      std::string(
          R"({"windows_version":{"major":10,"minor":0,"build":19041},"unload":true,"process_inventory":[{"process_id":4,"image_name":"System"},{"process_id":100,"image_name":"client.exe","memory":[]}],"finite_filesystem":[{"drive":"C","device_name":"\\Device\\HarddiskVolume1","read_only":false,"entries":[{"path":"\\app","directory":true,"data":""},{"path":"\\app\\image.exe","directory":false,"data":")") +
      llvm::toHex(llvm::ArrayRef(Source.FileBytes), true) + R"("}]}]})";
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_image_sections").c_str(),
                        Scenario.c_str(), nullptr)),
                    runCLI(Scenario, 0, "driver_image_sections")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true) << Text;
    EXPECT_EQ(Report->getBoolean("unload_completed"), true) << Text;
  }
}

TEST_F(DriverScenarioPublic, ProcessImageSourceAndCOWReachCAPIAndCLI) {
  auto Source = neverd::emulation::processImageFixture();
  auto Image = [&] {
    return llvm::json::Object{
        {"mapped_file", Source.MappedFile},
        {"device_name", Source.DeviceName},
        {"file_bytes_hex",
         llvm::toHex(llvm::ArrayRef(Source.FileBytes), true)}};
  };
  llvm::json::Array Processes;
  Processes.push_back(
      llvm::json::Object{{"process_id", 4}, {"image_name", "System"}});
  for (unsigned PID : {100u, 200u}) {
    llvm::json::Array Images;
    Images.push_back(Image());
    Processes.push_back(llvm::json::Object{{"process_id", PID},
                                           {"image_name", "image.exe"},
                                           {"images", std::move(Images)}});
  }
  const std::string Scenario =
      llvm::formatv("{0}",
                    llvm::json::Value(llvm::json::Object{
                        {"windows_version",
                         llvm::json::Object{
                             {"major", 10}, {"minor", 0}, {"build", 19041}}},
                        {"unload", true},
                        {"process_inventory", std::move(Processes)}}))
          .str();
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_process_images").c_str(),
                        Scenario.c_str(), nullptr)),
                    runCLI(Scenario, 0, "driver_process_images")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    const auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Config = Report->getObject("configuration");
    ASSERT_NE(Config, nullptr);
    const auto *Inventory = Config->getArray("process_inventory");
    ASSERT_NE(Inventory, nullptr);
    ASSERT_EQ(Inventory->size(), 3u);
    const auto *Process = (*Inventory)[1].getAsObject();
    ASSERT_NE(Process, nullptr);
    const auto *Images = Process->getArray("images");
    ASSERT_NE(Images, nullptr);
    ASSERT_EQ(Images->size(), 1u);
    const auto *Mapping = (*Images)[0].getAsObject();
    ASSERT_NE(Mapping, nullptr);
    EXPECT_EQ(Mapping->getString("mapped_file"), Source.MappedFile);
    EXPECT_EQ(Mapping->getString("device_name"), Source.DeviceName);
    EXPECT_EQ(Mapping->getString("file_bytes_hex"),
              llvm::toHex(llvm::ArrayRef(Source.FileBytes), true));
  }
}

TEST_F(DriverScenarioPublic, IRPInitializationPolicyReachesCAPIAndCLI) {
  for (bool Native : {false, true}) {
    std::string Scenario =
        R"({"unload":true,"windows_version":{"major":10,"minor":0,"build":19041},"requests":[{"kind":"create","device":"\\Device\\NeverDInitIRP"},{"kind":"ioctl","code":2236416,"input":"01020304","output_size":4},{"kind":"cleanup"},{"kind":"close"}])";
    if (Native)
      Scenario += R"(,"irp_initialization_policy":"windows-19041")";
    Scenario += "}";
    for (auto Text :
         {takeString(neverd_emulate_driver_scenario_json(
              Session, fixture("driver_irp_initialization").c_str(),
              Scenario.c_str(), nullptr)),
          runCLI(Scenario, Native ? 0 : 3, "driver_irp_initialization")}) {
      auto JSON = llvm::json::parse(Text);
      ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
      const auto *Report = JSON->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getBoolean("scenario_success"), Native);
      EXPECT_EQ(Report->getBoolean("unload_completed"), Native);
      const auto *Config = Report->getObject("configuration");
      ASSERT_NE(Config, nullptr);
      EXPECT_EQ(Config->getString("irp_initialization_policy"),
                Native ? "windows-19041" : "strict");
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), Native ? 4u : 1u);
      for (const auto &Value : *Requests) {
        const auto *Request = Value.getAsObject();
        ASSERT_NE(Request, nullptr);
        EXPECT_EQ(Request->getBoolean("completed"), Native);
        if (Native) {
          EXPECT_EQ(Request->getInteger("io_status"), 0);
          EXPECT_EQ(Request->getInteger("information"), 0);
        }
      }
    }
  }
}

TEST_F(DriverScenarioPublic, ThreadNotificationsReachCAPIAndCLI) {
  const char *Scenario =
      R"({"unload":true,"windows_version":{"major":10,"minor":0,"build":19041}})";
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_thread_notify").c_str(),
                        Scenario, nullptr)),
                    runCLI(Scenario, 0, "driver_thread_notify")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    const auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Events = Report->getArray("thread_notifications");
    ASSERT_NE(Events, nullptr);
    ASSERT_EQ(Events->size(), 2u);
    for (const auto &E : *Events) {
      const auto *Event = E.getAsObject();
      ASSERT_NE(Event, nullptr);
      EXPECT_EQ(Event->getBoolean("completed"), true);
      const auto *Delivered = Event->getArray("delivered_routines");
      ASSERT_NE(Delivered, nullptr);
      EXPECT_EQ(Delivered->size(), 2u);
    }
    EXPECT_EQ((*Events)[0].getAsObject()->getBoolean("create"), true);
    EXPECT_EQ((*Events)[1].getAsObject()->getBoolean("create"), false);
  }
}
TEST_F(DriverScenarioPublic, ForegroundProcessMemoryReachesCAPIAndCLI) {
  const char *Scenario = R"({"unload":true,
    "windows_version":{"major":10,"minor":0,"build":19041},
    "process_inventory":[{"process_id":4,"image_name":"System"},
      {"process_id":100,"parent_id":4,"image_name":"A.exe","memory":[
        {"base":4194304,"size":4096,"bytes_hex":"11111111","access":"read_write"},
        {"base":5242880,"size":4096,"bytes_hex":"4d5a","access":"read_only"}]},
      {"process_id":200,"parent_id":4,"image_name":"B.exe","memory":[
        {"base":4194304,"size":4096,"bytes_hex":"22222222","access":"read_write"}]}]})";
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_process_memory").c_str(),
                        Scenario, nullptr)),
                    runCLI(Scenario, 0, "driver_process_memory")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    const auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Config = Report->getObject("configuration");
    ASSERT_NE(Config, nullptr);
    const auto *Inventory = Config->getArray("process_inventory");
    ASSERT_NE(Inventory, nullptr);
    ASSERT_GT(Inventory->size(), 1u);
    EXPECT_TRUE((*Inventory)[1].getAsObject()->getArray("memory"));
  }
}
TEST_F(DriverScenarioPublic, ProcessExitStatusLifecycleReachesCAPIAndCLI) {
  const char *Scenario = R"({"unload":true,
    "windows_version":{"major":10,"minor":0,"build":19041},
    "process_events":[{"kind":"create","process_id":100,"parent_id":4},
      {"kind":"exit","process_id":100,"parent_id":4,"before_request":3,"exit_status":3221225477}],
    "requests":[{"kind":"create","requestor_process_id":100},
      {"kind":"cleanup","requestor_process_id":100},{"kind":"close","requestor_process_id":100}]})";
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_process_exit_status").c_str(),
                        Scenario, nullptr)),
                    runCLI(Scenario, 0, "driver_process_exit_status")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    auto *Events = Report->getArray("process_events");
    ASSERT_NE(Events, nullptr);
    ASSERT_EQ(Events->size(), 2u);
    EXPECT_EQ((*Events)[1].getAsObject()->getInteger("exit_status"),
              3221225477LL);
  }
}

TEST_F(DriverScenarioPublic, ContiguousMemoryReachesCAPIAndCLI) {
  const std::string Scenario = R"({"unload":true,"page_tables":true,
    "windows_version":{"major":10,"minor":0,"build":19041}})";
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_contiguous").c_str(),
                        Scenario.c_str(), nullptr)),
                    runCLI(Scenario, 0, "driver_contiguous")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true) << Text;
    EXPECT_EQ(Report->getBoolean("unload_completed"), true) << Text;
  }
}

TEST_F(DriverScenarioPublic, ProcessorQueriesAndDataReachCAPIAndCLI) {
  const std::string Scenario = R"({"unload":true})";
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_processors_native").c_str(),
                        Scenario.c_str(), nullptr)),
                    runCLI(Scenario, 0, "driver_processors_native")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true) << Text;
    EXPECT_EQ(Report->getBoolean("unload_completed"), true) << Text;
  }
}

TEST_F(DriverScenarioPublic, RTLInputServicesReachCAPIAndCLI) {
  const std::string Scenario = R"({"unload":true})";
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_rtl_inputs").c_str(),
                        Scenario.c_str(), nullptr)),
                    runCLI(Scenario, 0, "driver_rtl_inputs")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true) << Text;
    EXPECT_EQ(Report->getBoolean("unload_completed"), true) << Text;
  }
}
TEST_F(DriverScenarioPublic,
       RegistryEnumerationAndFilterNotificationsReachCAPIAndCLI) {
  auto O = neverd::emulation::test::registryEnumerationOptions();
  llvm::json::Array Keys;
  for (const auto &K : *O.Registry) {
    llvm::json::Array Values;
    for (const auto &V : K.Values)
      Values.push_back(llvm::json::Object{{"name", V.Name},
                                          {"type", V.Type},
                                          {"data", llvm::toHex(V.Data, true)}});
    llvm::json::Object Key{{"path", K.Path}, {"values", std::move(Values)}};
    if (K.Information)
      Key["information"] =
          llvm::json::Object{{"last_write_time", K.Information->LastWriteTime},
                             {"class_name", K.Information->ClassName}};
    if (K.SubkeyOrder) {
      llvm::json::Array Names;
      for (const auto &N : *K.SubkeyOrder)
        Names.push_back(N);
      Key["subkey_order"] = std::move(Names);
    }
    Keys.push_back(std::move(Key));
  }
  const std::string Scenario =
      llvm::formatv("{0}",
                    llvm::json::Value(llvm::json::Object{
                        {"windows_version",
                         llvm::json::Object{
                             {"major", 10}, {"minor", 0}, {"build", 19041}}},
                        {"unload", true},
                        {"registry", std::move(Keys)}}))
          .str();
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_registry_enumeration").c_str(),
                        Scenario.c_str(), nullptr)),
                    runCLI(Scenario, 0, "driver_registry_enumeration")}) {
    auto JSON = llvm::cantFail(llvm::json::parse(Text));
    auto *Report = JSON.getAsObject();
    EXPECT_EQ(Report->getBoolean("scenario_success"), true) << Text;
    EXPECT_EQ(Report->getBoolean("unload_completed"), true) << Text;
    auto *Messages = Report->getArray("messages");
    ASSERT_NE(Messages, nullptr);
    ASSERT_EQ(Messages->size(), 1u);
    EXPECT_EQ((*Messages)[0].getAsString(),
              "registry enumeration and filters passed");
  }
}
TEST_F(DriverScenarioPublic, NlsOemDataReachesCAPIAndCLIWithExplicitBooleans) {
  for (bool Value : {false, true}) {
    const std::string Scenario =
        Value ? R"({"unload":true,"nls_oem_multibyte":true})"
              : R"({"unload":true,"nls_oem_multibyte":false})";
    for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                          Session, fixture("driver_nls_oem").c_str(),
                          Scenario.c_str(), nullptr)),
                      runCLI(Scenario, 0, "driver_nls_oem")}) {
      auto JSON = llvm::cantFail(llvm::json::parse(Text));
      auto *Report = JSON.getAsObject();
      EXPECT_EQ(Report->getBoolean("scenario_success"), true) << Text;
      EXPECT_EQ(Report->getBoolean("unload_completed"), true) << Text;
      EXPECT_EQ(
          Report->getObject("configuration")->getBoolean("nls_oem_multibyte"),
          Value);
    }
  }
}
TEST_F(DriverScenarioPublic, DevicePropertiesReachCAPIAndCLIThroughAddDevice) {
  const std::string Scenario =
      R"({"unload":true,"pnp_devices":[{"id":"port","bus":"resource_free",
    "initial_device_power":"D0","initial_system_power":"working","device_properties":[
      {"property":1,"bytes_hex":"4e00440000000000"},{"property":2,"bytes_hex":null},
      {"property":14,"bytes_hex":"07000000"},{"property":17,"bytes_hex":"2a000000"}]}],
    "requests":[{"kind":"pnp","device_id":"port","minor":"start","bus_completion":{"status":0,"delay_100ns":3}},
                {"kind":"pnp","device_id":"port","minor":"query_remove","bus_completion":{"status":0}},{"kind":"pnp","device_id":"port","minor":"remove","bus_completion":{"status":0,"delay_100ns":3}}]})";
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_device_properties").c_str(),
                        Scenario.c_str(), nullptr)),
                    runCLI(Scenario, 0, "driver_device_properties")}) {
    auto JSON = llvm::cantFail(llvm::json::parse(Text));
    auto *Report = JSON.getAsObject();
    EXPECT_EQ(Report->getBoolean("scenario_success"), true) << Text;
    EXPECT_EQ(Report->getBoolean("unload_completed"), true) << Text;
    auto *Devices = Report->getArray("pnp_devices");
    ASSERT_NE(Devices, nullptr);
    ASSERT_EQ(Devices->size(), 1u);
    EXPECT_EQ((*Devices)[0].getAsObject()->getBoolean("provider_present"),
              false);
    auto *Messages = Report->getArray("messages");
    ASSERT_NE(Messages, nullptr);
    ASSERT_EQ(Messages->size(), 1u);
    EXPECT_EQ((*Messages)[0].getAsString(), "PDO property queries passed");
  }
}
TEST_F(DriverScenarioPublic, ProcessPebAndOwnedLoaderBytesReachCAPIAndCLI) {
  using namespace neverd::emulation;
  auto O = test::processPebOptions();
  llvm::json::Array Inventory, Requests;
  for (const auto &P : *O.ProcessInventory) {
    llvm::json::Object Record{{"process_id", P.ProcessID},
                              {"image_name", P.ImageName}};
    if (P.PebAddress)
      Record["peb_address"] = *P.PebAddress;
    if (P.Memory) {
      const auto &R = P.Memory->at(0);
      Record["memory"] = llvm::json::Array{
          llvm::json::Object{{"base", R.Base},
                             {"size", R.Size},
                             {"bytes_hex", llvm::toHex(R.Bytes, true)},
                             {"access", "read_write"}}};
    }
    Inventory.push_back(std::move(Record));
  }
  for (const char *Kind : {"create", "cleanup", "close"})
    Requests.push_back(
        llvm::json::Object{{"kind", Kind}, {"requestor_process_id", 100}});
  const auto Scenario =
      llvm::formatv("{0}",
                    llvm::json::Value(llvm::json::Object{
                        {"unload", true},
                        {"windows_version",
                         llvm::json::Object{
                             {"major", 10}, {"minor", 0}, {"build", 19041}}},
                        {"process_inventory", std::move(Inventory)},
                        {"requests", std::move(Requests)}}))
          .str();
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_process_peb").c_str(),
                        Scenario.c_str(), nullptr)),
                    runCLI(Scenario, 0, "driver_process_peb")}) {
    auto JSON = llvm::cantFail(llvm::json::parse(Text));
    auto *Report = JSON.getAsObject();
    EXPECT_EQ(Report->getBoolean("scenario_success"), true) << Text;
    EXPECT_EQ(Report->getBoolean("unload_completed"), true) << Text;
    auto *Messages = Report->getArray("messages");
    ASSERT_NE(Messages, nullptr);
    ASSERT_EQ(Messages->size(), 1u);
    EXPECT_EQ((*Messages)[0].getAsString(), "process PEB observations passed");
  }
}
TEST_F(DriverScenarioPublic, ProcessSectionBaseReachesCAPIAndCLI) {
  const char *Scenario = R"({"unload":true,
    "windows_version":{"major":10,"minor":0,"build":19041},
    "process_inventory":[
      {"process_id":4,"image_name":"System","section_base_address":0},
      {"process_id":100,"parent_id":4,"image_name":"Client.exe",
       "section_base_address":5368709120}],
    "requests":[{"kind":"create","requestor_process_id":100},
                {"kind":"cleanup","requestor_process_id":100},
                {"kind":"close","requestor_process_id":100}]})";
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_process_section_base").c_str(),
                        Scenario, nullptr)),
                    runCLI(Scenario, 0, "driver_process_section_base")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    auto *Messages = Report->getArray("messages");
    ASSERT_NE(Messages, nullptr);
    ASSERT_EQ(Messages->size(), 1u);
    EXPECT_EQ((*Messages)[0].getAsString(), "process section bases passed");
  }
}

TEST_F(DriverScenarioPublic, FileHintsAndProcessorStallReachCAPIAndCLI) {
  const std::string Scenario = R"({"unload":true,
    "windows_version":{"major":10,"minor":0,"build":19041},
    "system_clock_increment_100ns":156250,
    "finite_filesystem":[{"drive":"C","device_name":"\\Device\\NeverDVolumeC",
      "read_only":false,"aliases":[{"name":"\\SystemRoot","path":"\\data"}],
      "entries":[{"path":"\\data","directory":true,"data":""},
        {"path":"\\data\\hello.bin","directory":false,"data":"0102030405"}]}]})";
  for (const char *Name : {"driver_filesystem", "driver_clock_increment"}) {
    for (auto Text :
         {takeString(neverd_emulate_driver_scenario_json(
              Session, fixture(Name).c_str(), Scenario.c_str(), nullptr)),
          runCLI(Scenario, 0, Name)}) {
      auto JSON = llvm::json::parse(Text);
      ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
      auto *Report = JSON->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getBoolean("scenario_success"), true) << Text;
      EXPECT_EQ(Report->getBoolean("unload_completed"), true) << Text;
    }
  }
}
TEST_F(DriverScenarioPublic, NamedSectionViewsReachCAPIAndCLI) {
  const char *Scenario = R"({"unload":true,"section_namespace":{
    "directories":["\\BaseNamedObjects"],"sections":[{
      "name":"\\BaseNamedObjects\\Shared","size":69632,
      "bytes_hex":"1234","protection":4}]}})";
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_named_sections").c_str(),
                        Scenario, nullptr)),
                    runCLI(Scenario, 0, "driver_named_sections")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    auto *Messages = Report->getArray("messages");
    ASSERT_NE(Messages, nullptr);
    ASSERT_EQ(Messages->size(), 1u);
    EXPECT_EQ((*Messages)[0].getAsString(),
              "named section shared views passed");
    auto *Namespace = Report->getObject("section_namespace");
    ASSERT_NE(Namespace, nullptr);
    auto *Sections = Namespace->getArray("sections");
    ASSERT_NE(Sections, nullptr);
    ASSERT_EQ(Sections->size(), 1u);
    auto *Section = (*Sections)[0].getAsObject();
    ASSERT_NE(Section, nullptr);
    EXPECT_EQ(Section->getInteger("size"), 69632);
    auto Bytes = Section->getString("bytes_hex");
    ASSERT_TRUE(Bytes);
    EXPECT_EQ(Bytes->size(), 69632u * 2);
    EXPECT_TRUE(Bytes->starts_with("78563412"));
    auto *Configuration = Report->getObject("configuration");
    ASSERT_NE(Configuration, nullptr);
    auto *Initial = Configuration->getObject("section_namespace");
    ASSERT_NE(Initial, nullptr);
    auto *InitialSections = Initial->getArray("sections");
    ASSERT_NE(InitialSections, nullptr);
    ASSERT_EQ(InitialSections->size(), 1u);
    EXPECT_EQ((*InitialSections)[0].getAsObject()->getString("bytes_hex"),
              "1234");
  }
}

TEST_F(DriverScenarioPublic, BugCheckReportsReachCAPIAndCLIWithoutReturning) {
  const std::string Scenario = R"({"unload":true})";
  for (const char *Name : {"driver_bugcheck_terminal", "driver_bugcheck_simple",
                           "driver_bugcheck_unload", "driver_bugcheck_thread",
                           "driver_bugcheck_seh"}) {
    for (auto Text : {takeString(neverd_emulate_driver_backend_json(
                          Session, fixture(Name).c_str(), Scenario.c_str(),
                          nullptr, "unicorn", "checked-x64-v1")),
                      runCLI(Scenario, 3, Name, nullptr, "checked-x64-v1")}) {
      auto JSON = llvm::json::parse(Text);
      ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
      auto *Report = JSON->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("stop_reason"), "bugcheck") << Text;
      EXPECT_EQ(Report->getBoolean("scenario_success"), false) << Text;
      EXPECT_EQ(Report->getBoolean("unload_completed"), false) << Text;
      auto *Bug = Report->getObject("bugcheck");
      ASSERT_NE(Bug, nullptr);
      EXPECT_EQ(Bug->getInteger("code"), int64_t(0xdead0042u));
      EXPECT_EQ(Bug->getBoolean("dump_callbacks_executed"), false);
      if (std::string_view(Name).ends_with("simple"))
        EXPECT_TRUE(Bug->get("parameters")->getAsNull());
      else {
        auto *Parameters = Bug->getArray("parameters");
        ASSERT_NE(Parameters, nullptr);
        ASSERT_EQ(Parameters->size(), 4u);
        EXPECT_EQ((*Parameters)[1].getAsString(), "0xFFFFFFFFFFFFFFFF");
        EXPECT_EQ((*Parameters)[3].getAsString(), "0x8000000000000001");
      }
    }
  }
}

TEST_F(DriverScenarioPublic, ReservedSystemMappingsReachCAPIAndCLI) {
  const std::string Scenario = R"({"unload":true,"page_tables":true,
    "windows_version":{"major":10,"minor":0,"build":19041}})";
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_reserved_mapping").c_str(),
                        Scenario.c_str(), nullptr)),
                    runCLI(Scenario, 0, "driver_reserved_mapping")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true) << Text;
    EXPECT_EQ(Report->getBoolean("unload_completed"), true) << Text;
  }
}
TEST_F(DriverScenarioPublic, AnsiCharacterConversionReachesCAPIAndCLI) {
  const char *Scenario = R"({"unload":true})";
  for (auto Text :
       {takeString(neverd_emulate_driver_scenario_json(
            Session, fixture("driver_ansi_char").c_str(), Scenario, nullptr)),
        runCLI(Scenario, 0, "driver_ansi_char")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    auto *Messages = Report->getArray("messages");
    ASSERT_NE(Messages, nullptr);
    ASSERT_EQ(Messages->size(), 1u);
    EXPECT_EQ((*Messages)[0].getAsString(), "ANSI character conversion passed");
  }
}

TEST_F(DriverScenarioPublic, InitialSystemProcessDataReachesCAPIAndCLI) {
  const char *Scenario = R"({"unload":true})";
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_process_data").c_str(),
                        Scenario, nullptr)),
                    runCLI(Scenario, 0, "driver_process_data")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    auto *Messages = Report->getArray("messages");
    ASSERT_NE(Messages, nullptr);
    ASSERT_EQ(Messages->size(), 1u);
    EXPECT_EQ((*Messages)[0].getAsString(), "System process data passed");
  }
}

TEST_F(DriverScenarioPublic, ProcessInformationReachesCAPIAndCLI) {
  const char *Scenario = R"({"unload":true,
    "windows_version":{"major":10,"minor":0,"build":19041},
    "process_inventory":[{"process_id":4,"image_name":"System",
      "image_file_name_hex":"53797374656d000000000000000000"},
    {"process_id":100,"parent_id":4,"image_name":"Client.exe",
      "image_file_name_hex":"436c69656e742e6578650000000000"}]})";
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_process_information").c_str(),
                        Scenario, nullptr)),
                    runCLI(Scenario, 0, "driver_process_information")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    auto *Messages = Report->getArray("messages");
    ASSERT_NE(Messages, nullptr);
    ASSERT_EQ(Messages->size(), 1u);
    EXPECT_EQ((*Messages)[0].getAsString(), "process snapshot passed");
    auto *Inventory =
        Report->getObject("configuration")->getArray("process_inventory");
    ASSERT_NE(Inventory, nullptr);
    EXPECT_EQ(Inventory->size(), 2u);
  }
}

TEST_F(DriverScenarioPublic, IntegerUnicodeConversionReachesCAPIAndCLI) {
  const char *Scenario = R"({"unload":true})";
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_integer_unicode").c_str(),
                        Scenario, nullptr)),
                    runCLI(Scenario, 0, "driver_integer_unicode")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    auto *Messages = Report->getArray("messages");
    ASSERT_NE(Messages, nullptr);
    ASSERT_EQ(Messages->size(), 1u);
    EXPECT_EQ((*Messages)[0].getAsString(),
              "integer Unicode conversion passed");
  }
}

TEST_F(DriverScenarioPublic, LargeKernelPoolReachesCAPIAndCLI) {
  const char *Scenario = R"({"kernel_memory_size":8388608,"unload":true,
    "windows_version":{"major":10,"minor":0,"build":19041},"page_tables":true})";
  for (auto Text :
       {takeString(neverd_emulate_driver_scenario_json(
            Session, fixture("driver_large_pool").c_str(), Scenario, nullptr)),
        runCLI(Scenario, 0, "driver_large_pool")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    EXPECT_EQ(
        Report->getObject("configuration")->getInteger("kernel_memory_size"),
        8388608);
  }
  EXPECT_EQ(
      neverd_emulate_driver_scenario_json(
          Session, "missing.sys", R"({"kernel_memory_size":8388609})", nullptr),
      nullptr);
  EXPECT_NE(error().find("kernel_memory_size"), std::string::npos);
}

TEST_F(DriverScenarioPublic, ResidentWorkerLifecycleReachesCAPIAndCLI) {
  const char *Scenario = R"({"callback_drain_policy":"foreground-ready",
    "unload":true,"requests":[
    {"kind":"create","device":"\\Device\\NeverDIO"},
    {"kind":"ioctl","code":2236416,"input":"00112233","output_size":4},
    {"kind":"ioctl","code":2236448,"output_size":4},
    {"kind":"cleanup"},{"kind":"close"}]})";
  for (auto Text : {takeString(neverd_emulate_driver_scenario_json(
                        Session, fixture("driver_resident_worker").c_str(),
                        Scenario, nullptr)),
                    runCLI(Scenario, 0, "driver_resident_worker")}) {
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError()) << error();
    auto *Report = JSON->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    EXPECT_EQ(
        Report->getObject("configuration")->getString("callback_drain_policy"),
        "foreground-ready");
    auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 5u);
    EXPECT_EQ((*Requests)[1].getAsObject()->getString("output_hex"),
              "5a4b7869");
    EXPECT_EQ((*Requests)[2].getAsObject()->getString("output_hex"),
              "a0a1a2a3");
    EXPECT_EQ((*Requests)[2].getAsObject()->getInteger("dispatch_status"),
              0x103);
  }
  EXPECT_EQ(neverd_emulate_driver_scenario_json(
                Session, "missing.sys", R"({"callback_drain_policy":"skip"})",
                nullptr),
            nullptr);
  EXPECT_NE(error().find("callback_drain_policy"), std::string::npos);
}

TEST_F(DriverScenarioPublic, ExplicitClockIncrementReachesCAPIAndCLI) {
  const char *Scenario =
      R"({"system_clock_increment_100ns":156250,"unload":true})";
  auto CAPI = llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
      Session, fixture("driver_clock_increment").c_str(), Scenario, nullptr)));
  ASSERT_TRUE(bool(CAPI)) << llvm::toString(CAPI.takeError()) << error();
  auto CLI = llvm::json::parse(runCLI(Scenario, 0, "driver_clock_increment"));
  ASSERT_TRUE(bool(CLI)) << llvm::toString(CLI.takeError());
  for (auto *Report : {CAPI->getAsObject(), CLI->getAsObject()}) {
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getInteger("nt_status"), 0);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    EXPECT_EQ(Report->getObject("configuration")
                  ->getInteger("system_clock_increment_100ns"),
              156250);
  }
  EXPECT_EQ(neverd_emulate_driver_scenario_json(
                Session, "missing.sys", R"({"system_clock_increment_100ns":0})",
                nullptr),
            nullptr);
  EXPECT_NE(error().find("system_clock_increment_100ns"), std::string::npos);
}

TEST_F(DriverScenarioPublic, ExplicitSecurePciFactsReachCAPIAndCLI) {
  for (bool Enabled : {false, true}) {
    const std::string Scenario =
        std::string(
            R"({"windows_version":{"major":10,"minor":0,"build":19041},"unload":true,"secure_pci_enabled":)") +
        (Enabled ? "true}" : "false}");
    auto CAPI =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, fixture("driver_secure_pci").c_str(), Scenario.c_str(),
            nullptr)));
    ASSERT_TRUE(bool(CAPI)) << llvm::toString(CAPI.takeError()) << error();
    auto CLI = llvm::json::parse(runCLI(Scenario, 0, "driver_secure_pci"));
    ASSERT_TRUE(bool(CLI)) << llvm::toString(CLI.takeError());
    for (auto *Report : {CAPI->getAsObject(), CLI->getAsObject()}) {
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getInteger("nt_status"), 0);
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      EXPECT_EQ(
          Report->getObject("configuration")->getBoolean("secure_pci_enabled"),
          Enabled);
      const auto *Messages = Report->getArray("messages");
      ASSERT_NE(Messages, nullptr);
      ASSERT_EQ(Messages->size(), 1u);
      EXPECT_EQ((*Messages)[0].getAsString(),
                Enabled ? "secure-pci:enabled" : "secure-pci:disabled");
    }
  }
  EXPECT_EQ(neverd_emulate_driver_scenario_json(
                Session, "missing.sys", R"({"secure_pci_enabled":0})", nullptr),
            nullptr);
  EXPECT_NE(error().find("secure_pci_enabled"), std::string::npos);
}

TEST_F(DriverScenarioPublic, ExplicitMcfgFirmwareReachesCAPIAndCLI) {
  ASSERT_EQ(neverd_session_load(Session,
                                fixture("driver_kernel_debugger_mcfg").c_str()),
            1)
      << error();
  auto Exports = llvm::json::parse(takeString(neverd_exports_json(Session)));
  ASSERT_TRUE(bool(Exports)) << llvm::toString(Exports.takeError());
  uint64_t CountAddress = 0, PointerAddress = 0;
  for (const auto &Value : *Exports->getAsArray()) {
    const auto *E = Value.getAsObject();
    auto Name = E->getString("name"), Address = E->getString("addr");
    if (!Name || !Address)
      continue;
    uint64_t VA = 0;
    ASSERT_FALSE(Address->getAsInteger(0, VA));
    // The test PE uses its standard preferred base; the driver reference has
    // a distinct, fixed guest kernel base. The original export RVA is retained.
    VA = 0xfffff80000000000ULL + (VA - 0x180000000ULL);
    if (*Name == "HalpPciMcfgTableCount")
      CountAddress = VA;
    if (*Name == "HalpPciMcfgTable")
      PointerAddress = VA;
  }
  ASSERT_NE(CountAddress, 0u);
  ASSERT_NE(PointerAddress, 0u);
  for (const auto &F :
       {neverd::emulation::DriverPciMcfg{}, mcgffixture::table()}) {
    llvm::json::Object Mcfg{{"present", F.Present}};
    if (F.Present)
      Mcfg["table_hex"] = llvm::toHex(F.Table, true);
    auto Parameters = mcgffixture::parameters(CountAddress, PointerAddress,
                                              F.Present ? 2 : 0, F.Present);
    llvm::json::Array Values;
    for (const auto &V : Parameters.Values)
      Values.push_back(llvm::json::Object{
          {"name", V.Name}, {"type", 11}, {"data", llvm::toHex(V.Data, true)}});
    llvm::json::Object O{
        {"windows_version",
         llvm::json::Object{{"major", 10}, {"minor", 0}, {"build", 19041}}},
        {"kernel_image", fixture("driver_kernel_debugger_mcfg")},
        {"kernel_pdb", (std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                        "driver_kernel_debugger_mcfg.pdb")
                           .string()},
        {"kernel_exports", llvm::json::Object{{"RtlInitUnicodeString", true},
                                              {"ZwOpenKey", true},
                                              {"ZwQueryValueKey", true},
                                              {"ZwClose", true},
                                              {"DbgPrint", true}}},
        {"pci_mcfg", std::move(Mcfg)},
        {"unload", true},
        {"registry",
         llvm::json::Array{llvm::json::Object{{"path", Parameters.Path},
                                              {"values", std::move(Values)}}}}};
    const auto Scenario =
        llvm::formatv("{0}", llvm::json::Value(std::move(O))).str();
    auto CAPI =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, fixture("driver_mcfg").c_str(), Scenario.c_str(),
            nullptr)));
    ASSERT_TRUE(bool(CAPI)) << llvm::toString(CAPI.takeError()) << error();
    auto CLI = llvm::json::parse(runCLI(Scenario, 0, "driver_mcfg"));
    ASSERT_TRUE(bool(CLI)) << llvm::toString(CLI.takeError());
    for (auto *Report : {CAPI->getAsObject(), CLI->getAsObject()}) {
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getBoolean("scenario_success"), true)
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      EXPECT_EQ(Report->getObject("configuration")
                    ->getObject("pci_mcfg")
                    ->getBoolean("present"),
                F.Present);
    }
  }
  EXPECT_EQ(neverd_emulate_driver_scenario_json(
                Session, "missing.sys",
                R"({"pci_mcfg":{"present":true,"table_hex":"00"}})", nullptr),
            nullptr);
  EXPECT_NE(error().find("pci_mcfg"), std::string::npos);
}

TEST_F(DriverScenarioPublic,
       NamedDriverObjectsReachCAPIAndCLIWithRealReferences) {
  const char *Scenario =
      R"({"windows_version":{"major":10,"minor":0,"build":19041},"driver_namespace":{"empty_drivers":["\\Driver\\pci"]},"unload":true})";
  auto CAPI = llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
      Session, fixture("driver_object_names").c_str(), Scenario, nullptr)));
  ASSERT_TRUE(bool(CAPI)) << llvm::toString(CAPI.takeError()) << error();
  auto CLI = llvm::json::parse(runCLI(Scenario, 0, "driver_object_names"));
  ASSERT_TRUE(bool(CLI)) << llvm::toString(CLI.takeError());
  for (auto *Report : {CAPI->getAsObject(), CLI->getAsObject()}) {
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true)
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Inventory =
        Report->getObject("configuration")->getObject("driver_namespace");
    ASSERT_NE(Inventory, nullptr);
    EXPECT_EQ(Inventory->getArray("empty_drivers")->size(), 1u);
    unsigned Queries = 0;
    for (const auto &C : *Report->getArray("calls")) {
      auto *Event = C.getAsObject();
      if (Event->getString("name") != "ObReferenceObjectByName")
        continue;
      EXPECT_EQ(Event->getArray("arguments")->size(), 8u);
      EXPECT_TRUE(Event->getString("detail")->starts_with("\\"));
      ++Queries;
    }
    EXPECT_EQ(Queries, 5u);
  }
  EXPECT_EQ(neverd_emulate_driver_scenario_json(
                Session, "missing.sys",
                R"({"driver_namespace":{"empty_drivers":["not-absolute"]}})",
                nullptr),
            nullptr);
  EXPECT_NE(error().find("driver_namespace"), std::string::npos);
}

TEST_F(DriverScenarioPublic, SmbiosTwoPhaseQueriesReachCAPIAndCLI) {
  auto F = neverd::emulation::smbiosfixture::table();
  const auto Scenario =
      "{\"windows_version\":{\"major\":10,\"minor\":0,\"build\":19041},"
      "\"unload\":true,\"smbios\":{\"raw_hex\":\"" +
      llvm::toHex(F.Raw, true) + "\"}}";
  auto CAPI = llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
      Session, fixture("driver_smbios").c_str(), Scenario.c_str(), nullptr)));
  ASSERT_TRUE(bool(CAPI)) << llvm::toString(CAPI.takeError()) << error();
  auto CLI = llvm::json::parse(runCLI(Scenario, 0, "driver_smbios"));
  ASSERT_TRUE(bool(CLI)) << llvm::toString(CLI.takeError());
  for (const auto *Report : {CAPI->getAsObject(), CLI->getAsObject()}) {
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true)
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    EXPECT_EQ(Report->getObject("configuration")
                  ->getObject("smbios")
                  ->getString("raw_hex"),
              llvm::toHex(F.Raw, true));
    unsigned Queries = 0;
    for (const auto &C : *Report->getArray("calls"))
      if (C.getAsObject()->getString("name") == "ZwQuerySystemInformation")
        ++Queries;
    EXPECT_EQ(Queries, 3u);
  }
  EXPECT_EQ(
      neverd_emulate_driver_scenario_json(
          Session, "missing.sys", R"({"smbios":{"raw_hex":"00"}})", nullptr),
      nullptr);
  EXPECT_NE(error().find("smbios"), std::string::npos);
}

TEST_F(DriverScenarioPublic, DmaGuardPolicyStatesReachCAPIAndCLI) {
  for (bool Enabled : {false, true}) {
    const std::string Scenario =
        std::string(
            "{\"windows_version\":{\"major\":10,\"minor\":0,\"build\":19041},"
            "\"unload\":true,\"dma_guard_policy_enabled\":") +
        (Enabled ? "true}" : "false}");
    auto CAPI =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, fixture("driver_dma_guard").c_str(), Scenario.c_str(),
            nullptr)));
    ASSERT_TRUE(bool(CAPI)) << llvm::toString(CAPI.takeError()) << error();
    auto CLI = llvm::json::parse(runCLI(Scenario, 0, "driver_dma_guard"));
    ASSERT_TRUE(bool(CLI)) << llvm::toString(CLI.takeError());
    for (const auto *Report : {CAPI->getAsObject(), CLI->getAsObject()}) {
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getBoolean("scenario_success"), true)
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      EXPECT_EQ(Report->getObject("configuration")
                    ->getBoolean("dma_guard_policy_enabled"),
                Enabled);
      ASSERT_EQ(Report->getArray("messages")->size(), 1u);
      EXPECT_EQ((*Report->getArray("messages"))[0].getAsString(),
                std::string("DMA guard enabled=") + (Enabled ? "1" : "0"));
    }
  }
  EXPECT_EQ(
      neverd_emulate_driver_scenario_json(
          Session, "missing.sys", R"({"dma_guard_policy_enabled":0})", nullptr),
      nullptr);
  EXPECT_NE(error().find("dma_guard_policy_enabled"), std::string::npos);
}

TEST_F(DriverScenarioPublic,
       AcpiInventoryReachesCAPIAndCLIWithCapturedQueries) {
  const auto F = neverd::emulation::acpifixture::inventory();
  llvm::json::Array Tables;
  for (const auto &B : F.Tables)
    Tables.push_back(llvm::toHex(B, true));
  const auto Scenario =
      llvm::formatv(
          "{0}", llvm::json::Value(llvm::json::Object{
                     {"windows_version", llvm::json::Object{{"major", 10},
                                                            {"minor", 0},
                                                            {"build", 19041}}},
                     {"unload", true},
                     {"acpi_tables",
                      llvm::json::Object{{"tables_hex", std::move(Tables)}}}}))
          .str();
  auto CAPI = llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
      Session, fixture("driver_acpi_tables").c_str(), Scenario.c_str(),
      nullptr)));
  ASSERT_TRUE(bool(CAPI)) << llvm::toString(CAPI.takeError()) << error();
  auto CLI = llvm::json::parse(runCLI(Scenario, 0, "driver_acpi_tables"));
  ASSERT_TRUE(bool(CLI)) << llvm::toString(CLI.takeError());
  for (const auto *Report : {CAPI->getAsObject(), CLI->getAsObject()}) {
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true)
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Out = Report->getObject("configuration")
                          ->getObject("acpi_tables")
                          ->getArray("tables_hex");
    ASSERT_EQ(Out->size(), F.Tables.size());
    for (size_t I = 0; I < Out->size(); ++I)
      EXPECT_EQ((*Out)[I].getAsString(), llvm::toHex(F.Tables[I], true));
    unsigned Count = 0;
    for (const auto &Call : *Report->getArray("calls")) {
      const auto *C = Call.getAsObject();
      if (C->getString("name") == "ZwQuerySystemInformation") {
        EXPECT_TRUE(C->getString("detail")->starts_with("ACPI action="));
        ++Count;
      }
    }
    EXPECT_EQ(Count, 6u);
  }
  EXPECT_EQ(neverd_emulate_driver_scenario_json(
                Session, "missing.sys",
                R"({"acpi_tables":{"tables_hex":["00"]}})", nullptr),
            nullptr);
  EXPECT_NE(error().find("acpi_tables"), std::string::npos);
}

TEST_F(DriverScenarioPublic, CodeIntegrityObservationsReachCAPIAndCLIUnmasked) {
  for (uint32_t Value : {0u, 0x401u, 0x200000u, UINT32_MAX}) {
    const auto Scenario =
        std::string("{\"windows_version\":{\"major\":10,\"minor\":0,\"build\":"
                    "19041},\"unload\":true,\"code_integrity_options\":") +
        std::to_string(Value) + "}";
    for (const auto &JSON :
         {takeString(neverd_emulate_driver_scenario_json(
              Session, fixture("driver_code_integrity").c_str(),
              Scenario.c_str(), nullptr)),
          runCLI(Scenario, 0, "driver_code_integrity")}) {
      auto R = llvm::json::parse(JSON);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError()) << error();
      const auto *Report = R->getAsObject();
      EXPECT_EQ(Report->getBoolean("scenario_success"), true)
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      EXPECT_EQ(Report->getObject("configuration")
                    ->getInteger("code_integrity_options"),
                int64_t(Value));
      ASSERT_EQ(Report->getArray("messages")->size(), 1u);
      EXPECT_EQ((*Report->getArray("messages"))[0].getAsString(),
                "CI options=" + std::to_string(Value));
      unsigned Calls = 0;
      for (const auto &C : *Report->getArray("calls"))
        if (C.getAsObject()->getString("name") == "NtQuerySystemInformation" ||
            C.getAsObject()->getString("name") == "ZwQuerySystemInformation")
          ++Calls;
      EXPECT_EQ(Calls, 6u);
    }
  }
  EXPECT_EQ(neverd_emulate_driver_scenario_json(
                Session, "missing.sys",
                R"({"code_integrity_options":4294967296})", nullptr),
            nullptr);
  EXPECT_NE(error().find("code_integrity_options"), std::string::npos);
}

TEST_F(DriverScenarioPublic, ThreadTerminationQueriesReachCAPIAndCLI) {
  const std::string Scenario = R"({"unload":true})";
  for (const auto &JSON :
       {takeString(neverd_emulate_driver_scenario_json(
            Session, fixture("driver_thread_termination").c_str(),
            Scenario.c_str(), nullptr)),
        runCLI(Scenario, 0, "driver_thread_termination")}) {
    auto R = llvm::json::parse(JSON);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError()) << error();
    const auto *Report = R->getAsObject();
    EXPECT_EQ(Report->getBoolean("scenario_success"), true)
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    unsigned Live = 0, Terminating = 0;
    for (const auto &C : *Report->getArray("calls")) {
      const auto *Call = C.getAsObject();
      if (Call->getString("name") != "PsIsThreadTerminating")
        continue;
      Live += Call->getString("result") == "0x0";
      Terminating += Call->getString("result") == "0x1";
    }
    EXPECT_GE(Live, 4u);
    EXPECT_GE(Terminating, 3u);
  }
}

TEST_F(DriverScenarioPublic, AnsiUppercaseQueriesReachCAPIAndCLI) {
  const std::string Scenario = R"({"unload":true})";
  for (const auto &JSON : {takeString(neverd_emulate_driver_scenario_json(
                               Session, fixture("driver_ansi_upper").c_str(),
                               Scenario.c_str(), nullptr)),
                           runCLI(Scenario, 0, "driver_ansi_upper")}) {
    auto R = llvm::json::parse(JSON);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError()) << error();
    const auto *Report = R->getAsObject();
    EXPECT_EQ(Report->getBoolean("scenario_success"), true)
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    ASSERT_EQ(Report->getArray("messages")->size(), 1u);
    EXPECT_EQ((*Report->getArray("messages"))[0].getAsString(),
              "ANSI uppercase byte contract passed");
    unsigned Strings = 0, Characters = 0;
    for (const auto &C : *Report->getArray("calls")) {
      const auto Name = C.getAsObject()->getString("name");
      Strings += Name == "RtlUpperString";
      Characters += Name == "RtlUpperChar";
    }
    EXPECT_EQ(Strings, 3u);
    EXPECT_EQ(Characters, 256u);
  }
}

TEST_F(DriverScenarioPublic, AnsiCompareAndAppendReachCAPIAndCLI) {
  const std::string Scenario = R"({"unload":true})";
  for (const auto &JSON :
       {takeString(neverd_emulate_driver_scenario_json(
            Session, fixture("driver_ansi_operations").c_str(),
            Scenario.c_str(), nullptr)),
        runCLI(Scenario, 0, "driver_ansi_operations")}) {
    auto R = llvm::json::parse(JSON);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError()) << error();
    const auto *Report = R->getAsObject();
    EXPECT_EQ(Report->getBoolean("scenario_success"), true)
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    ASSERT_EQ(Report->getArray("messages")->size(), 1u);
    EXPECT_EQ((*Report->getArray("messages"))[0].getAsString(),
              "ANSI compare/append passed");
    unsigned Compared = 0, Appended = 0;
    for (const auto &C : *Report->getArray("calls")) {
      const auto Name = C.getAsObject()->getString("name");
      Compared += Name == "RtlCompareString";
      Appended += Name == "RtlAppendStringToString";
    }
    EXPECT_EQ(Compared, 19u);
    EXPECT_EQ(Appended, 4u);
  }
}

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

TEST_F(DriverScenarioPublic, DebuggerDataDeclarationReachesCABIAndCLI) {
  for (bool Enabled : {false, true}) {
    const auto Scenario =
        std::string(
            R"({"service_name":"NeverDData1","unload":true,"debugger_state":{"enabled":)") +
        (Enabled ? "true" : "false") + R"(,"not_present":true}})";
    const auto FromC = takeString(neverd_emulate_driver_scenario_json(
        Session, fixture("driver_data").c_str(), Scenario.c_str(), nullptr));
    ASSERT_FALSE(FromC.empty()) << error();
    const auto FromCLI = runCLI(Scenario, 0, "driver_data");
    for (const auto &Text : {FromC, FromCLI}) {
      auto Parsed = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("stop_reason"), "returned");
      EXPECT_EQ(Report->getInteger("nt_status"), 0);
      const auto *Messages = Report->getArray("messages");
      ASSERT_NE(Messages, nullptr);
      ASSERT_EQ(Messages->size(), 1u);
      EXPECT_EQ((*Messages)[0].getAsString(),
                Enabled ? "debugger bytes: 1,1" : "debugger bytes: 0,1");
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *State =
          Report->getObject("configuration")->getObject("debugger_state");
      ASSERT_NE(State, nullptr);
      EXPECT_EQ(State->getBoolean("enabled"), Enabled);
      EXPECT_EQ(State->getBoolean("not_present"), true);
    }
  }
  EXPECT_EQ(neverd_emulate_driver_scenario_json(
                Session, "missing.sys",
                R"({"debugger_state":{"enabled":true}})", nullptr),
            nullptr);
  EXPECT_NE(error().find("debugger_state"), std::string::npos);
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

TEST_F(DriverScenarioPublic, CAPIAndCLIUseOnlyExplicitCPUIDObservations) {
  const std::string Scenario = R"({"cpuid":[{"leaf":1073741824,"subleaf":3,
    "eax":4275878552,"ebx":2147483649,"ecx":19088743,"edx":4294967295}]})";
  for (const auto &JSON : {takeString(neverd_emulate_driver_scenario_json(
                               Session, fixture("driver_cpuid").c_str(),
                               Scenario.c_str(), nullptr)),
                           runCLI(Scenario, 0, "driver_cpuid")}) {
    auto Parsed = llvm::json::parse(JSON);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    const auto *Configuration = Report->getObject("configuration");
    ASSERT_NE(Configuration, nullptr);
    const auto *Queries = Configuration->getArray("cpuid");
    ASSERT_NE(Queries, nullptr);
    ASSERT_EQ(Queries->size(), 1u);
    EXPECT_EQ((*Queries)[0].getAsObject()->getInteger("edx"), 4294967295LL);
  }
  auto Missing = llvm::json::parse(runCLI("{}", 3, "driver_cpuid"));
  ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
  EXPECT_EQ(Missing->getAsObject()->getString("stop_reason"),
            "unsupported_instruction");
}

TEST_F(DriverScenarioPublic, CPUIDPoliciesResolveGuestFactsThroughCAPIAndCLI) {
  auto O = neverd::emulation::cpuidpolicyfixture::options();
  llvm::json::Array Queries;
  for (const auto &Query : O.CPUID) {
    llvm::json::Object Entry{{"leaf", Query.Leaf},
                             {"eax", Query.Registers[0]},
                             {"ebx", Query.Registers[1]},
                             {"ecx", Query.Registers[2]},
                             {"edx", Query.Registers[3]}};
    if (Query.Subleaf)
      Entry["subleaf"] = *Query.Subleaf;
    Queries.emplace_back(std::move(Entry));
  }
  const auto Scenario =
      llvm::formatv(
          "{0}", llvm::json::Value(llvm::json::Object{
                     {"cpuid", std::move(Queries)},
                     {"unload", true},
                     {"cpuid_policy",
                      llvm::json::Object{{"invalid_leaf", "intel-max-basic-v1"},
                                         {"hypervisor", "neverd-v1"}}}}))
          .str();
  for (const auto &JSON : {takeString(neverd_emulate_driver_scenario_json(
                               Session, fixture("driver_cpuid_policy").c_str(),
                               Scenario.c_str(), nullptr)),
                           runCLI(Scenario, 0, "driver_cpuid_policy")}) {
    auto R = llvm::json::parse(JSON);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError()) << error();
    const auto *Report = R->getAsObject();
    EXPECT_EQ(Report->getBoolean("scenario_success"), true)
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Config = Report->getObject("configuration");
    EXPECT_EQ(Config->getArray("cpuid")->size(), O.CPUID.size());
    const auto *P = Config->getObject("cpuid_policy");
    ASSERT_NE(P, nullptr);
    EXPECT_EQ(P->getString("invalid_leaf"), "intel-max-basic-v1");
    EXPECT_EQ(P->getString("hypervisor"), "neverd-v1");
  }
  EXPECT_EQ(neverd_emulate_driver_scenario_json(
                Session, "missing.sys",
                R"({"cpuid_policy":{"invalid_leaf":"intel-max-basic-v1"}})",
                nullptr),
            nullptr);
  EXPECT_NE(error().find("cpuid_policy"), std::string::npos);
}

TEST_F(DriverScenarioPublic, CAPIAndCLIKeepProcessReferenceUntilUnload) {
  for (const auto &JSON :
       {takeString(neverd_emulate_driver_scenario_json(
            Session, fixture("driver_process_lookup").c_str(),
            R"({"unload":true})", nullptr)),
        runCLI(R"({"unload":true})", 0, "driver_process_lookup")}) {
    auto Parsed = llvm::json::parse(JSON);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
  }
}

TEST_F(DriverScenarioPublic, CAPIAndCLIWalkGuestTablesAndNativeMDLAliases) {
  const std::string Scenario =
      R"({"page_tables":true,"mdl_mapping_policy":"windows-19041", "unload":true,
           "windows_version":{"major":10,"minor":0,"build":19041}})";
  for (const auto &JSON :
       {takeString(neverd_emulate_driver_scenario_json(
            Session, fixture("driver_paging_nonpaged").c_str(),
            Scenario.c_str(), nullptr)),
        runCLI(Scenario, 0, "driver_paging_nonpaged")}) {
    auto Parsed = llvm::json::parse(JSON);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    auto *Configuration = Report->getObject("configuration");
    ASSERT_NE(Configuration, nullptr);
    EXPECT_EQ(Configuration->getBoolean("page_tables"), true);
    EXPECT_EQ(Configuration->getString("mdl_mapping_policy"), "windows-19041");
  }
}

TEST_F(DriverScenarioPublic, CLIRejectsUnknownFieldsNULAndOversizedFiles) {
  EXPECT_TRUE(runCLI(R"({"typo":true})", 1).empty());
  EXPECT_TRUE(runCLI(std::string("{}\0ignored", 10), 1).empty());
  EXPECT_TRUE(runCLI(std::string(NEVERD_DRIVER_SCENARIO_JSON_LIMIT + 1, ' '), 1)
                  .empty());
}

TEST_F(DriverScenarioPublic,
       CAPIAndCLIRejectKernelGeneratedRequestsBeforeImageLoading) {
  constexpr char Image[] = "missing-kernel-request-public.sys";
  for (const char *Scenario :
       {R"({"requests":[{"kind":"internal_ioctl","code":3}]})",
        R"({"requests":[{"kind":"unlock_all","file":1}]})"}) {
    SCOPED_TRACE(Scenario);
    EXPECT_EQ(
        neverd_emulate_driver_scenario_json(Session, Image, Scenario, nullptr),
        nullptr);
    EXPECT_NE(error().find("kernel-generated"), std::string::npos);
    EXPECT_FALSE(neverd_session_is_loaded(Session));
    EXPECT_TRUE(runCLI(Scenario, 1, "success", Image).empty());
    std::ifstream Errors(Directory / "error.txt");
    const std::string Diagnostic(std::istreambuf_iterator<char>(Errors), {});
    EXPECT_NE(Diagnostic.find("kernel-generated"), std::string::npos);
  }
}

TEST_F(DriverScenarioPublic, CAPIAndCLIReportIndependentDriverAllocatedIRP) {
#ifdef NEVERD_WDM_OWNED_IRP_FIXTURE
  const uint8_t Input[] = {OwnedIrpWorker, 1};
  llvm::json::Object Scenario{
      {"service_name", "NeverDOwnedIrp"},
      {"unload", true},
      {"requests",
       llvm::json::Array{
           llvm::json::Object{{"kind", "create"},
                              {"device", "\\Device\\NeverDOwnedIrp"},
                              {"file", 1}},
           llvm::json::Object{{"kind", "ioctl"},
                              {"file", 1},
                              {"code", uint32_t(OwnedIrpSubmitIoctl)},
                              {"input", llvm::toHex(Input, true)}},
           llvm::json::Object{
               {"kind", "ioctl"},
               {"file", 1},
               {"code", uint32_t(OwnedIrpSnapshotIoctl)},
               {"output_size", OwnedIrpSnapshotWords * sizeof(uint32_t)}},
           llvm::json::Object{{"kind", "cleanup"}, {"file", 1}},
           llvm::json::Object{{"kind", "close"}, {"file", 1}}}}};
  const auto ScenarioJSON =
      llvm::formatv("{0}", llvm::json::Value(std::move(Scenario))).str();
  for (const auto *Image : {
           NEVERD_WDM_OWNED_IRP_FIXTURE,
#ifdef NEVERD_WDM_OWNED_IRP_CFG_FIXTURE
           NEVERD_WDM_OWNED_IRP_CFG_FIXTURE,
#endif
       }) {
    SCOPED_TRACE(Image);
    const auto API = takeString(neverd_emulate_driver_scenario_json(
        Session, Image, ScenarioJSON.c_str(), nullptr));
    ASSERT_FALSE(API.empty()) << error();
    for (const auto &Text : {API, runCLI(ScenarioJSON, 0, nullptr, Image)}) {
      auto JSON = llvm::json::parse(Text);
      ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
      const auto *Report = JSON->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("stop_reason"), "returned")
          << Report->getString("diagnostic").value_or("").str();
      EXPECT_EQ(Report->getBoolean("scenario_success"), true);
      EXPECT_EQ(Report->getBoolean("unload_completed"), true);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 6u);
      size_t Children = 0, Snapshots = 0;
      for (const auto &Request : *Requests) {
        const auto *Row = Request.getAsObject();
        ASSERT_NE(Row, nullptr);
        EXPECT_EQ(Row->getBoolean("completed"), true);
        EXPECT_EQ(Row->getInteger("io_status"), 0);
        if (Row->getString("origin") == "driver_allocated_irp") {
          ++Children;
          EXPECT_EQ(Row->getString("kind"), "internal_ioctl");
          EXPECT_EQ(Row->getInteger("code"), OwnedIrpInternalIoctl);
          EXPECT_EQ(Row->getInteger("dispatch_status"), 0x103);
          EXPECT_EQ(Row->getInteger("information"), 0);
          EXPECT_EQ(Row->getString("output_hex"), "");
          for (const auto *Field :
               {"file", "requestor_process_id", "response_index",
                "cancel_requested_at_100ns"}) {
            SCOPED_TRACE(Field);
            ASSERT_NE(Row->get(Field), nullptr);
            EXPECT_EQ(Row->get(Field)->kind(), llvm::json::Value::Null);
          }
        } else {
          EXPECT_EQ(Row->getString("origin"), "scenario");
          EXPECT_EQ(Row->getInteger("file"), 1);
        }
        if (Row->getInteger("code") != OwnedIrpSnapshotIoctl)
          continue;
        ++Snapshots;
        const auto Hex = Row->getString("output_hex");
        ASSERT_TRUE(Hex);
        ASSERT_EQ(Hex->size(), OwnedIrpSnapshotWords * sizeof(uint32_t) * 2);
        const auto Word = [&](size_t Index) {
          uint32_t Value = 0;
          for (size_t I = 0; I != sizeof(Value); ++I) {
            uint32_t Byte = 0;
            EXPECT_FALSE(Hex->substr((Index * sizeof(Value) + I) * 2, 2)
                             .getAsInteger(16, Byte));
            Value |= Byte << (I * 8);
          }
          return Value;
        };
        EXPECT_EQ(Word(OwnedIrpFailures), 0u);
        EXPECT_EQ(Word(OwnedIrpAllocations), 1u);
        EXPECT_EQ(Word(OwnedIrpCompletions), 1u);
        EXPECT_EQ(Word(OwnedIrpFrees), 1u);
        EXPECT_EQ(Word(OwnedIrpWorkers), 1u);
      }
      EXPECT_EQ(Children, 1u);
      EXPECT_EQ(Snapshots, 1u);
    }
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_OWNED_IRP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIAndCLIRejectMalformedUsbFactsBeforeLoading) {
  constexpr char Image[] = "missing-usb-public-preflight.sys";
  for (const char *Usb :
       {R"({"role":"hub"})", R"({"role":"composite_function"})",
        R"({"role":"independent_function","remote_wake":true})",
        R"({"role":"independent_function","remote_wake":0})",
        R"({"role":"independent_function","device_wake":"D3"})",
        R"({"role":"independent_function","device_wake":null})",
        R"({"role":"composite_parent","device_wake":"D2"})"}) {
    SCOPED_TRACE(Usb);
    const std::string Scenario =
        R"({"pnp_devices":[{"id":"port","bus":"resource_free",
          "initial_device_power":"D0","initial_system_power":"working",
          "usb_idle":)" +
        std::string(Usb) + "}]}";
    EXPECT_EQ(neverd_emulate_driver_scenario_json(Session, Image,
                                                  Scenario.c_str(), nullptr),
              nullptr);
    EXPECT_NE(error().find("usb_idle"), std::string::npos);
    EXPECT_FALSE(neverd_session_is_loaded(Session));
    EXPECT_TRUE(runCLI(Scenario, 1, "success", Image).empty());
    std::ifstream Errors(Directory / "error.txt");
    const std::string Diagnostic(std::istreambuf_iterator<char>(Errors), {});
    EXPECT_NE(Diagnostic.find("usb_idle"), std::string::npos);
  }
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
constexpr int64_t DelayedKMDFFileCompletion100ns = 17;
constexpr int64_t KMDFFileSendTimeout100ns = 5;
constexpr int64_t ExpectedIOTimeoutStatus = 0xc00000b5;

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
  for (const char *Image : Images)
    for (char Mode : {'O', 'p', 's', 'a'}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Mode);
      const std::string Service = KMDFPnpServicePrefix.str() + Mode;
      auto Options = kmdfPnpOptions(Service.c_str());
      auto Scenario = llvm::json::parse(KMDFPnpFileForwardScenario);
      ASSERT_TRUE(bool(Scenario)) << llvm::toString(Scenario.takeError());
      {
        auto *Requests = Scenario->getAsObject()->getArray("requests");
        ASSERT_NE(Requests, nullptr);
        for (size_t Index : {1u, 2u, 3u}) {
          auto *Completion =
              (*Requests)[Index].getAsObject()->getObject("bus_completion");
          ASSERT_NE(Completion, nullptr);
          (*Completion)["delay_100ns"] = DelayedKMDFFileCompletion100ns;
        }
      }
      const std::string ScenarioJSON = llvm::formatv("{0}", *Scenario).str();
      auto Parsed =
          llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
              Session, Image, ScenarioJSON.c_str(), &Options)));
      ASSERT_TRUE(bool(Parsed))
          << llvm::toString(Parsed.takeError()) << error();
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
      {
        const auto *Start = (*Requests)[0].getAsObject()->getObject("pnp");
        const auto *Remove = (*Requests)[4].getAsObject()->getObject("pnp");
        ASSERT_NE(Start, nullptr);
        ASSERT_NE(Remove, nullptr);
        ASSERT_TRUE(Start->getInteger("bus_completed_at_100ns"));
        ASSERT_TRUE(Remove->getInteger("bus_received_at_100ns"));
        EXPECT_GE(*Remove->getInteger("bus_received_at_100ns"),
                  *Start->getInteger("bus_completed_at_100ns") +
                      3 * DelayedKMDFFileCompletion100ns);
      }
    }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIDeliversKMDFFileSendTimeout) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  std::vector<const char *> Images{NEVERD_KMDF_PNP_FIXTURE};
#ifdef NEVERD_KMDF_PNP_CFG_FIXTURE
  Images.push_back(NEVERD_KMDF_PNP_CFG_FIXTURE);
#endif
  for (char Mode : {'t', 'q'})
    for (const char *Image : Images) {
      SCOPED_TRACE(Image);
      const std::string Service = KMDFPnpServicePrefix.str() + Mode;
      auto Options = kmdfPnpOptions(Service.c_str());
      auto Scenario = llvm::json::parse(KMDFPnpFileForwardScenario);
      ASSERT_TRUE(bool(Scenario)) << llvm::toString(Scenario.takeError());
      auto *Requests = Scenario->getAsObject()->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      auto *Create = (*Requests)[1].getAsObject();
      ASSERT_NE(Create, nullptr);
      auto *Completion = Create->getObject("bus_completion");
      ASSERT_NE(Completion, nullptr);
      (*Completion)["delay_100ns"] = DelayedKMDFFileCompletion100ns;
      for (auto It = Requests->begin(); It != Requests->end();) {
        const auto *Object = It->getAsObject();
        if (Object && (Object->getString("kind") == "cleanup" ||
                       Object->getString("kind") == "close"))
          It = Requests->erase(It);
        else
          ++It;
      }
      const std::string ScenarioJSON = llvm::formatv("{0}", *Scenario).str();
      auto Parsed =
          llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
              Session, Image, ScenarioJSON.c_str(), &Options)));
      ASSERT_TRUE(bool(Parsed))
          << llvm::toString(Parsed.takeError()) << error();
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("stop_reason"), "returned")
          << Report->getString("diagnostic").value_or("").str();
      const auto *Output = Report->getArray("requests");
      ASSERT_NE(Output, nullptr);
      ASSERT_EQ(Output->size(), 4u);
      const auto *Open = (*Output)[1].getAsObject();
      ASSERT_NE(Open, nullptr);
      EXPECT_EQ(Open->getInteger("io_status"), ExpectedIOTimeoutStatus);
      EXPECT_EQ(Open->getBoolean("completed"), true);
      const auto *Start = (*Output)[0].getAsObject()->getObject("pnp");
      const auto *Remove = (*Output)[2].getAsObject()->getObject("pnp");
      ASSERT_NE(Start, nullptr);
      ASSERT_NE(Remove, nullptr);
      ASSERT_TRUE(Start->getInteger("bus_completed_at_100ns"));
      ASSERT_TRUE(Remove->getInteger("bus_received_at_100ns"));
      EXPECT_GE(*Remove->getInteger("bus_received_at_100ns"),
                *Start->getInteger("bus_completed_at_100ns") +
                    KMDFFileSendTimeout100ns);
    }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIReportsFrameworkSystemPowerChildren) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  constexpr char Scenario[] = R"({
    "load_address":"0x190000000","unload":true,
    "pnp_devices":[{"id":"kmdf0","bus":"resource_free",
      "initial_device_power":"D0","initial_system_power":"working",
      "requested_device_power":[
        {"minor":"set","power_type":"device","power_state":"D3",
         "power_action":"sleep","system_context":0,
         "bus_completion":{"status":0,"delay_100ns":7}},
        {"minor":"set","power_type":"device","power_state":"D0",
         "power_action":"none","system_context":0,
         "bus_completion":{"status":0,"delay_100ns":13}}]}],
    "requests":[
      {"kind":"pnp","device_id":"kmdf0","minor":"start",
       "bus_completion":{"status":0}},
      {"kind":"power","device_id":"kmdf0","minor":"set",
       "power_type":"system","power_state":"sleeping3",
       "power_action":"sleep","system_context":0,
       "bus_completion":{"status":0,"delay_100ns":5}},
      {"kind":"power","device_id":"kmdf0","minor":"set",
       "power_type":"system","power_state":"working",
       "power_action":"none","system_context":0,
       "bus_completion":{"status":0,"delay_100ns":11}},
      {"kind":"pnp","device_id":"kmdf0","minor":"query_remove",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"kmdf0","minor":"remove",
       "bus_completion":{"status":0}}]})";
  const std::string Service = KMDFPnpServicePrefix.str() + '6';
  auto Options = kmdfPnpOptions(Service.c_str());
  auto Parsed =
      llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
          Session, NEVERD_KMDF_PNP_FIXTURE, Scenario, &Options)));
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
  unsigned Children = 0;
  for (const auto &Value : *Requests) {
    const auto *Request = Value.getAsObject();
    ASSERT_NE(Request, nullptr);
    EXPECT_EQ(Request->getBoolean("completed"), true);
    EXPECT_EQ(Request->getInteger("io_status"), 0);
    if (Request->getString("origin") == "framework_power_policy") {
      EXPECT_EQ(Request->getInteger("response_index"), Children++);
      const auto *Power = Request->getObject("power");
      ASSERT_NE(Power, nullptr);
      EXPECT_EQ(Power->getString("power_type"), "device");
    }
  }
  EXPECT_EQ(Children, 2u);
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic,
       CAPIFrameworkInterruptDpcAndWorkItemCompleteRequest) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  constexpr llvm::StringLiteral Scenario = R"({
    "load_address":"0x190000000","unload":true,
    "pnp_devices":[{"id":"kmdf-pdo","bus":"register_bank",
      "initial_device_power":"D0","initial_system_power":"working",
      "interrupts":[{"id":"framework-interrupt","raw_vector":17,
        "raw_level":7,"raw_affinity":1,"translated_vector":145,
        "translated_level":5,"translated_affinity":1,
        "mode":"latched","share":"device_exclusive"}]}],
    "requests":[
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"start",
        "bus_completion":{"status":0}},
      {"kind":"create","device_id":"kmdf-pdo","file":7},
      {"kind":"ioctl","device_id":"kmdf-pdo","file":7,
        "code":"0x222000","output_size":16,"interrupt_events":[{
          "after_100ns":7,"device_id":"kmdf-pdo",
          "interrupt_id":"framework-interrupt"}]},
      {"kind":"cleanup","file":7},{"kind":"close","file":7},
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"query_remove",
        "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"kmdf-pdo","minor":"remove",
        "bus_completion":{"status":0}}]})";
  std::vector<const char *> Images{NEVERD_KMDF_PNP_FIXTURE};
#ifdef NEVERD_KMDF_PNP_CFG_FIXTURE
  Images.push_back(NEVERD_KMDF_PNP_CFG_FIXTURE);
#endif
  for (const auto *Image : Images)
    for (char Mode : {KmdfInterruptLine, KmdfInterruptPassive,
                      KmdfInterruptPassiveCleanup, KmdfInterruptAutomatic}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Mode);
      const std::string Service = KMDFPnpServicePrefix.str() + Mode;
      auto Options = kmdfPnpOptions(Service.c_str());
      auto Parsed =
          llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
              Session, Image, Scenario.data(), &Options)));
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
      ASSERT_EQ(Requests->size(), 7u);
      const auto *IO = (*Requests)[2].getAsObject();
      ASSERT_NE(IO, nullptr);
      EXPECT_EQ(IO->getString("output_hex"),
                "01000000010000000100000002000000");
      const auto *Events = Report->getArray("interrupts");
      ASSERT_NE(Events, nullptr);
      ASSERT_EQ(Events->size(), 1u);
      const auto *Event = Events->front().getAsObject();
      ASSERT_NE(Event, nullptr);
      EXPECT_EQ(Event->getInteger("return_value"), 1);
      EXPECT_TRUE(Event->getInteger("delivered_at_100ns"));
      EXPECT_TRUE(Event->getInteger("returned_at_100ns"));
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
           {"share", "\"invalid\""},
           {"retrigger_after_100ns", "3"}}) {
    SCOPED_TRACE(Field);
    SCOPED_TRACE(Fact);
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

TEST_F(DriverScenarioPublic, CAPIExecutesExplicitMessageAssignmentsAndIds) {
#ifdef NEVERD_WDM_INTERRUPT_FIXTURE
  std::vector<const char *> Images{NEVERD_WDM_INTERRUPT_FIXTURE};
#ifdef NEVERD_WDM_INTERRUPT_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_INTERRUPT_CFG_FIXTURE);
#endif
  constexpr char Scenario[] = R"({
    "load_address":"0x190000000","unload":true,
    "pnp_devices":[{"id":"interrupt0","bus":"register_bank",
      "initial_device_power":"D0","initial_system_power":"working",
      "resources":[{"id":"counter","raw_start":"0x200000000",
        "translated_start":"0x300000000","length":"0x1000",
        "registers":[{"offset":0,"width":4,"access":"read_write","value":0}]}],
      "interrupts":[{"id":"line0","raw_vector":17,"raw_level":0,
        "raw_affinity":1,"translated_vector":145,"translated_level":5,
        "translated_affinity":1,"mode":"latched","share":"device_exclusive",
        "messages":[
          {"message_address":"0xfee01000","message_data":"0x123400",
           "translated_vector":145,"translated_level":5,"translated_affinity":1,
           "polarity":"rising_edge"},
          {"message_address":"0xfee01000","message_data":"0x123401",
           "translated_vector":146,"translated_level":7,"translated_affinity":1,
           "polarity":"falling_edge"}]}]}],
    "requests":[
      {"kind":"pnp","device_id":"interrupt0","minor":"start",
       "bus_completion":{"status":0}},
      {"kind":"create","device_id":"interrupt0","file":1},
      {"kind":"ioctl","file":1,"code":"0x222000","output_size":32,
       "interrupt_events":[{"after_100ns":7,"device_id":"interrupt0",
                            "interrupt_id":"line0","message_id":1}]},
      {"kind":"cleanup","file":1},{"kind":"close","file":1},
      {"kind":"pnp","device_id":"interrupt0","minor":"query_remove",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"interrupt0","minor":"remove",
       "bus_completion":{"status":0}}]})";
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  Options.service_name = "NeverDInterruptM";
  for (const char *Image : Images) {
    SCOPED_TRACE(Image);
    auto Parsed =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, Image, Scenario, &Options)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("stop_reason"), "returned")
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    const auto *Configuration = Report->getObject("configuration");
    ASSERT_NE(Configuration, nullptr);
    const auto *Devices = Configuration->getArray("pnp_devices");
    ASSERT_NE(Devices, nullptr);
    ASSERT_EQ(Devices->size(), 1u);
    const auto *Resources =
        Devices->front().getAsObject()->getArray("interrupts");
    ASSERT_NE(Resources, nullptr);
    const auto *Messages =
        Resources->front().getAsObject()->getArray("messages");
    ASSERT_NE(Messages, nullptr);
    ASSERT_EQ(Messages->size(), 2u);
    EXPECT_EQ((*Messages)[1].getAsObject()->getInteger("message_data"),
              0x123401);
    EXPECT_EQ((*Messages)[1].getAsObject()->getString("polarity"),
              "falling_edge");
    const auto *Inputs = Configuration->getArray("interrupt_events");
    ASSERT_NE(Inputs, nullptr);
    ASSERT_EQ(Inputs->size(), 1u);
    EXPECT_EQ(Inputs->front().getAsObject()->getInteger("message_id"), 1);
    const auto *Events = Report->getArray("interrupts");
    ASSERT_NE(Events, nullptr);
    ASSERT_EQ(Events->size(), 1u);
    const auto *Event = Events->front().getAsObject();
    EXPECT_EQ(Event->getInteger("message_id"), 1);
    EXPECT_EQ(Event->getInteger("return_value"), 1);
    const auto *Handlers = Event->getArray("handlers");
    ASSERT_NE(Handlers, nullptr);
    ASSERT_EQ(Handlers->size(), 1u);
    EXPECT_EQ(Handlers->front().getAsObject()->getInteger("message_id"), 1);
    EXPECT_EQ(Handlers->front().getAsObject()->getInteger("delivery_index"), 0);
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_INTERRUPT_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIExecutesExplicitLevelAssertionAndDeassertion) {
#ifdef NEVERD_WDM_INTERRUPT_FIXTURE
  std::vector<const char *> Images{NEVERD_WDM_INTERRUPT_FIXTURE};
#ifdef NEVERD_WDM_INTERRUPT_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_INTERRUPT_CFG_FIXTURE);
#endif
  constexpr char Scenario[] = R"({
    "load_address":"0x190000000","unload":true,
    "pnp_devices":[{"id":"interrupt0","bus":"register_bank",
      "initial_device_power":"D0","initial_system_power":"working",
      "resources":[{"id":"counter","raw_start":"0x200000000",
        "translated_start":"0x300000000","length":"0x1000",
        "registers":[{"offset":0,"width":4,"access":"read_write","value":0}]}],
      "interrupts":[{"id":"line0","raw_vector":17,"raw_level":7,
        "raw_affinity":1,"translated_vector":145,"translated_level":5,
        "translated_affinity":1,"mode":"level_sensitive",
        "share":"device_exclusive","retrigger_after_100ns":3}]}],
    "requests":[
      {"kind":"pnp","device_id":"interrupt0","minor":"start",
       "bus_completion":{"status":0}},
      {"kind":"create","device_id":"interrupt0","file":1},
      {"kind":"ioctl","file":1,"code":"0x222000","output_size":32,
       "interrupt_events":[
         {"after_100ns":7,"device_id":"interrupt0","interrupt_id":"line0","action":"assert"},
         {"after_100ns":15,"device_id":"interrupt0","interrupt_id":"line0","action":"deassert"}]},
      {"kind":"cleanup","file":1},{"kind":"close","file":1},
      {"kind":"pnp","device_id":"interrupt0","minor":"query_remove",
       "bus_completion":{"status":0}},
      {"kind":"pnp","device_id":"interrupt0","minor":"remove",
       "bus_completion":{"status":0}}]})";
  neverd_driver_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.instruction_limit = 100000;
  Options.memory_limit = 64 * 1024 * 1024;
  Options.event_limit = 10000;
  Options.timeout_milliseconds = 5000;
  Options.service_name = "NeverDInterruptD";
  for (const char *Image : Images) {
    SCOPED_TRACE(Image);
    auto Parsed =
        llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
            Session, Image, Scenario, &Options)));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError()) << error();
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("stop_reason"), "returned")
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Configuration = Report->getObject("configuration");
    ASSERT_NE(Configuration, nullptr);
    const auto *Devices = Configuration->getArray("pnp_devices");
    ASSERT_NE(Devices, nullptr);
    ASSERT_EQ(Devices->size(), 1u);
    const auto *Resources =
        Devices->front().getAsObject()->getArray("interrupts");
    ASSERT_NE(Resources, nullptr);
    ASSERT_EQ(Resources->size(), 1u);
    const auto *Resource = Resources->front().getAsObject();
    EXPECT_EQ(Resource->getString("mode"), "level_sensitive");
    EXPECT_EQ(Resource->getInteger("retrigger_after_100ns"), 3);
    const auto *Inputs = Configuration->getArray("interrupt_events");
    ASSERT_NE(Inputs, nullptr);
    ASSERT_EQ(Inputs->size(), 2u);
    EXPECT_EQ((*Inputs)[0].getAsObject()->getString("action"), "assert");
    EXPECT_EQ((*Inputs)[1].getAsObject()->getString("action"), "deassert");
    const auto *Events = Report->getArray("interrupts");
    ASSERT_NE(Events, nullptr);
    ASSERT_EQ(Events->size(), 2u);
    const auto *Assert = (*Events)[0].getAsObject();
    EXPECT_EQ(Assert->getString("action"), "assert");
    EXPECT_EQ(Assert->getInteger("occurred_at_100ns"), 7);
    const auto *Handlers = Assert->getArray("handlers");
    ASSERT_NE(Handlers, nullptr);
    ASSERT_EQ(Handlers->size(), 3u);
    for (size_t I = 0; I < Handlers->size(); ++I) {
      const auto *Handler = (*Handlers)[I].getAsObject();
      ASSERT_NE(Handler, nullptr);
      EXPECT_EQ(Handler->getInteger("delivery_index"), I);
      EXPECT_EQ(Handler->getInteger("delivered_at_100ns"), 7 + I * 3);
      EXPECT_EQ(Handler->getInteger("returned_at_100ns"), 7 + I * 3);
      EXPECT_EQ(Handler->getInteger("return_value"), I == 0 ? 1 : 0);
      EXPECT_EQ(Handler->getBoolean("claimed"), I == 0);
    }
    const auto *Deassert = (*Events)[1].getAsObject();
    EXPECT_EQ(Deassert->getString("action"), "deassert");
    EXPECT_EQ(Deassert->getInteger("occurred_at_100ns"), 15);
    EXPECT_TRUE(Deassert->getArray("handlers")->empty());
    for (const char *Field : {"delivered_at_100ns", "returned_at_100ns",
                              "interrupt_object", "return_value", "claimed"})
      EXPECT_TRUE(Deassert->get(Field)->getAsNull()) << Field;
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_INTERRUPT_FIXTURE requires a genuine WDK fixture";
#endif
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

TEST_F(DriverScenarioPublic, CAPIGSCookieFailureCannotBecomeSuccessfulSEH) {
#ifdef NEVERD_WDM_SEH_FIXTURE
  std::vector<const char *> Images{NEVERD_WDM_SEH_FIXTURE};
#ifdef NEVERD_WDM_SEH_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_SEH_CFG_FIXTURE);
#endif
  for (const auto *Image : Images)
    for (char Mode :
         {SehGSCookie, SehGSAlignedCookie, SehGSCorruptCookie,
          SehGSAlignedCorruptCookie, SehGSStandaloneCookie,
          SehGSStandaloneAlignedCookie, SehGSStandaloneCorruptCookie,
          SehGSStandaloneAlignedCorruptCookie}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Mode);
      const bool Corrupt = Mode == SehGSCorruptCookie ||
                           Mode == SehGSAlignedCorruptCookie ||
                           Mode == SehGSStandaloneCorruptCookie ||
                           Mode == SehGSStandaloneAlignedCorruptCookie;
      const std::string Service = std::string("NeverDSEH") + Mode;
      neverd_driver_options_v1 Options{};
      Options.struct_size = sizeof(Options);
      Options.instruction_limit =
          neverd::emulation::profile::DefaultInstructionLimit;
      Options.memory_limit = neverd::emulation::profile::DefaultMemoryLimit;
      Options.event_limit = neverd::emulation::profile::DefaultEventLimit;
      Options.timeout_milliseconds =
          neverd::emulation::profile::DefaultTimeoutMilliseconds;
      Options.service_name = Service.c_str();
      auto Parsed =
          llvm::json::parse(takeString(neverd_emulate_driver_scenario_json(
              Session, Image, R"({"load_address":"0x190000000","unload":true})",
              &Options)));
      ASSERT_TRUE(bool(Parsed))
          << llvm::toString(Parsed.takeError()) << error();
      const auto *Report = Parsed->getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString("stop_reason"),
                Corrupt ? "model_error" : "returned");
      EXPECT_EQ(Report->getBoolean("scenario_success"), !Corrupt);
      EXPECT_EQ(Report->getBoolean("unload_completed"), !Corrupt);
      if (Corrupt)
        EXPECT_NE(Report->getString("diagnostic")
                      .value_or("")
                      .find("GS security cookie check failed"),
                  llvm::StringRef::npos);
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

TEST_F(DriverScenarioPublic, CAPIAndCLIExecuteNestedUserMemoryExample) {
#ifdef NEVERD_WDM_NEITHER_FIXTURE
  std::ifstream Input(NEVERD_DRIVER_NESTED_SCENARIO);
  ASSERT_TRUE(Input.is_open());
  const std::string Scenario(std::istreambuf_iterator<char>(Input), {});
  for (const char *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       }) {
    for (const char *Base : {"0x180000000", "0x190000000"}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Base);
      auto Document = llvm::json::parse(Scenario);
      ASSERT_TRUE(bool(Document)) << llvm::toString(Document.takeError());
      (*Document->getAsObject())["load_address"] = Base;
      const std::string JSON = llvm::formatv("{0}", *Document).str();
      const std::string API = takeString(neverd_emulate_driver_scenario_json(
          Session, Image, JSON.c_str(), nullptr));
      ASSERT_FALSE(API.empty()) << error();
      for (const auto &Text : {API, runCLI(JSON, 0, nullptr, Image)}) {
        auto Parsed = llvm::json::parse(Text);
        ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
        const auto *Report = Parsed->getAsObject();
        ASSERT_NE(Report, nullptr);
        EXPECT_EQ(Report->getString("stop_reason"), "returned")
            << Report->getString("diagnostic").value_or("").str();
        EXPECT_EQ(Report->getBoolean("scenario_success"), true);
        EXPECT_EQ(Report->getBoolean("unload_completed"), true);
        const auto *Configuration = Report->getObject("configuration");
        ASSERT_NE(Configuration, nullptr);
        const auto *Memory = Configuration->getArray("user_memory");
        ASSERT_NE(Memory, nullptr);
        ASSERT_EQ(Memory->size(), 2u);
        const auto *Requests = Report->getArray("requests");
        ASSERT_NE(Requests, nullptr);
        ASSERT_EQ(Requests->size(), 5u);
        std::string PreviousPayload;
        for (size_t Index : {1u, 2u}) {
          const auto *Request = (*Requests)[Index].getAsObject();
          ASSERT_NE(Request, nullptr);
          EXPECT_EQ(Request->getInteger("io_status"), 0);
          EXPECT_EQ(Request->getBoolean("completed"), true);
          EXPECT_EQ(Request->getString("output_hex"), "");
          const auto *Buffers = Request->getArray("user_buffers");
          ASSERT_NE(Buffers, nullptr);
          ASSERT_EQ(Buffers->size(), 3u);
          const auto *Descriptor = (*Buffers)[0].getAsObject();
          const auto *Payload = (*Buffers)[1].getAsObject();
          const auto *Sink = (*Buffers)[2].getAsObject();
          ASSERT_NE(Descriptor, nullptr);
          ASSERT_NE(Payload, nullptr);
          ASSERT_NE(Sink, nullptr);
          EXPECT_EQ(Payload->getString("id"), "payload");
          EXPECT_EQ(Payload->getString("access"), "read_only");
          EXPECT_EQ(Payload->getString("backing_hex"),
                    "a5a5a501020304a5a5a5a5a5");
          EXPECT_EQ(Sink->getString("backing_hex"), "a5a5a5a531323334a5a5a5a5");
          const auto Address = Payload->getString("address");
          ASSERT_TRUE(Address);
          EXPECT_NE(Address->str(), PreviousPayload);
          PreviousPayload = Address->str();
          uint64_t PayloadAddress = 0;
          ASSERT_FALSE(Address->getAsInteger(0, PayloadAddress));
          const auto Backing = Descriptor->getString("backing_hex");
          ASSERT_TRUE(Backing);
          ASSERT_EQ(Backing->size(), sizeof(DriverNestedBuffer) * 2);
          uint64_t Pointer = 0;
          for (unsigned I = 0; I < sizeof(uint64_t); ++I) {
            unsigned Byte = 0;
            ASSERT_FALSE(Backing->substr(I * 2, 2).getAsInteger(16, Byte));
            Pointer |= uint64_t(Byte) << (I * 8);
          }
          EXPECT_EQ(Pointer, PayloadAddress + NestedPayloadOffset);
          EXPECT_EQ(
              Backing->take_front(sizeof(uint64_t) * 2),
              Backing->substr(sizeof(uint64_t) * 2, sizeof(uint64_t) * 2));
          for (const auto &Buffer : *Buffers)
            EXPECT_EQ(Buffer.getAsObject()->getBoolean("revoked"), Index == 2);
        }
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

TEST_F(DriverScenarioPublic, CAPIAndCLIExecuteFrameworkLocks) {
#ifdef NEVERD_KMDF_LOCK_FIXTURE
  const std::string Scenario =
      R"({"load_address":"0x190000000","unload":true})";
  for (const char *Image : {NEVERD_KMDF_LOCK_FIXTURE,
#ifdef NEVERD_KMDF_LOCK_CFG_FIXTURE
                            NEVERD_KMDF_LOCK_CFG_FIXTURE
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
      const auto *Messages = Report->getArray("messages");
      ASSERT_NE(Messages, nullptr);
      ASSERT_EQ(Messages->size(), 5u);
      EXPECT_EQ((*Messages)[0].getAsString(), "KMDF locks: worker completed\n");
      EXPECT_EQ((*Messages)[1].getAsString(),
                "KMDF locks: APC waiter completed\n");
      EXPECT_EQ((*Messages)[2].getAsString(),
                "KMDF locks: APC owner completed\n");
      EXPECT_EQ((*Messages)[3].getAsString(), "KMDF locks: complete\n");
      EXPECT_EQ((*Messages)[4].getAsString(), "KMDF locks: unload\n");
    }
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_LOCK_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, InvalidServiceNamesFailBeforeLoadingTheImage) {
  for (const char *Scenario :
       {R"({"service_name":""})", R"({"service_name":"Invalid/Service"})",
        R"({"service_name":1})"}) {
    EXPECT_EQ(neverd_emulate_driver_scenario_json(Session, "missing.sys",
                                                  Scenario, nullptr),
              nullptr);
    EXPECT_NE(error().find("service"), std::string::npos);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
}

TEST_F(DriverScenarioPublic, CAPIAndCLIExecuteFrameworkIdleWake) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  const std::string Scenario = R"policy({
  "service_name": "NeverDKmdfPowerA-",
  "load_address": "0x190000000",
  "unload": true,
  "pnp_devices": [
    {
      "id": "policy0",
      "bus": "resource_free",
      "initial_device_power": "D0",
      "initial_system_power": "working",
      "wake_capabilities": {
        "s0": true,
        "sx": true
      },
      "requested_device_power": [
        {
          "minor": "set",
          "power_type": "device",
          "power_state": "D3",
          "power_action": "none",
          "system_context": 0,
          "bus_completion": {
            "status": 0,
            "delay_100ns": 3
          }
        },
        {
          "minor": "set",
          "power_type": "device",
          "power_state": "D0",
          "power_action": "none",
          "system_context": 0,
          "bus_completion": {
            "status": 0,
            "delay_100ns": 3
          }
        }
      ]
    }
  ],
  "requests": [
    {
      "kind": "pnp",
      "device_id": "policy0",
      "minor": "start",
      "bus_completion": {
        "status": 0
      }
    },
    {
      "kind": "create",
      "device_id": "policy0",
      "file": 1
    },
    {
      "kind": "ioctl",
      "device_id": "policy0",
      "file": 1,
      "code": "0x222000",
      "input": "00",
      "output_size": 24,
      "power_policy_events": [
        {
          "device_id": "policy0",
          "after_100ns": 0,
          "action": "idle"
        },
        {
          "device_id": "policy0",
          "after_100ns": 10017,
          "action": "wake"
        }
      ]
    },
    {
      "kind": "ioctl",
      "device_id": "policy0",
      "file": 1,
      "code": "0x222000",
      "input": "00",
      "output_size": 24
    },
    {
      "kind": "cleanup",
      "device_id": "policy0",
      "file": 1
    },
    {
      "kind": "close",
      "device_id": "policy0",
      "file": 1
    },
    {
      "kind": "pnp",
      "device_id": "policy0",
      "minor": "query_remove",
      "bus_completion": {
        "status": 0
      }
    },
    {
      "kind": "pnp",
      "device_id": "policy0",
      "minor": "remove",
      "bus_completion": {
        "status": 0
      }
    }
  ]
})policy";
  for (const char *Image : {NEVERD_KMDF_PNP_FIXTURE,
#ifdef NEVERD_KMDF_PNP_CFG_FIXTURE
                            NEVERD_KMDF_PNP_CFG_FIXTURE
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
      const auto *Events = Report->getArray("power_policy_events");
      ASSERT_NE(Events, nullptr);
      ASSERT_EQ(Events->size(), 2u);
      EXPECT_EQ((*Events)[1].getAsObject()->getInteger("occurred_at_100ns"),
                10017);
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      ASSERT_EQ(Requests->size(), 11u);
      unsigned WakeRequests = 0, PowerRequests = 0;
      for (const auto &Request : *Requests) {
        const auto *Row = Request.getAsObject();
        ASSERT_NE(Row, nullptr);
        EXPECT_EQ(Row->getBoolean("completed"), true);
        EXPECT_EQ(Row->getInteger("io_status"), 0);
        if (Row->getString("origin") == "framework_power_policy")
          ++PowerRequests;
        if (Row->getString("origin") != "framework_wait_wake")
          continue;
        ++WakeRequests;
        EXPECT_EQ(Row->getInteger("dispatch_status"), 0x103);
        const auto *Power = Row->getObject("power");
        ASSERT_NE(Power, nullptr);
        EXPECT_EQ(Power->getString("minor"), "wait_wake");
        EXPECT_EQ(Power->getInteger("bus_received_at_100ns"), 10000);
        EXPECT_EQ(Power->getInteger("bus_completed_at_100ns"), 10017);
      }
      EXPECT_EQ(WakeRequests, 1u);
      EXPECT_EQ(PowerRequests, 2u);
    }
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverScenarioPublic, CAPIAndCLIExecuteD2SleepThroughRealD0) {
#ifdef NEVERD_KMDF_CHILD_WAKE_FIXTURE
  std::ifstream Stream(NEVERD_DRIVER_D2_SCENARIO);
  ASSERT_TRUE(Stream.good());
  const std::string Scenario((std::istreambuf_iterator<char>(Stream)),
                             std::istreambuf_iterator<char>());
  std::vector<const char *> Images{NEVERD_KMDF_CHILD_WAKE_FIXTURE};
#ifdef NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE
  Images.push_back(NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE);
#endif
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
      const auto *Requests = Report->getArray("requests");
      ASSERT_NE(Requests, nullptr);
      std::vector<std::string> Before, After;
      unsigned Snapshots = 0, D2Requests = 0;
      for (const auto &Value : *Requests) {
        const auto *Request = Value.getAsObject();
        ASSERT_NE(Request, nullptr);
        EXPECT_EQ(Request->getBoolean("completed"), true);
        EXPECT_EQ(Request->getInteger("io_status"), 0);
        if (const auto *Power = Request->getObject("power")) {
          if (Request->getString("origin") == "framework_power_policy") {
            Before.push_back(
                Power->getString("device_state_before").value_or("").str());
            After.push_back(
                Power->getString("device_state_after").value_or("").str());
            ASSERT_TRUE(Power->getInteger("bus_received_at_100ns"));
            ASSERT_TRUE(Power->getInteger("bus_completed_at_100ns"));
            EXPECT_GT(*Power->getInteger("bus_completed_at_100ns"),
                      *Power->getInteger("bus_received_at_100ns"));
          }
          if (Power->getString("power_type") == "device" &&
              Power->getString("power_state") == "D2") {
            ++D2Requests;
            EXPECT_EQ(Power->getString("device_state_before"), "D0");
            EXPECT_EQ(Power->getString("device_state_after"), "D2");
          }
        }
        if (Request->getString("kind") != "ioctl")
          continue;
        std::string Bytes;
        ASSERT_TRUE(llvm::tryGetFromHex(
            Request->getString("output_hex").value_or(""), Bytes));
        ASSERT_EQ(Bytes.size(), KmdfChildWakeSnapshotWords * sizeof(uint32_t));
        auto Word = [&](unsigned Index) {
          uint32_t Value = 0;
          for (unsigned I = 0; I < sizeof(Value); ++I)
            Value |= uint32_t(uint8_t(Bytes[Index * sizeof(Value) + I]))
                     << (I * 8);
          return Value;
        };
        EXPECT_EQ(Word(KmdfChildWakeFailures), 0u);
        EXPECT_EQ(Word(KmdfChildWakeD0Entries), Snapshots + 1);
        EXPECT_EQ(Word(KmdfChildWakeD0Exits), std::min(Snapshots + 1, 2u));
        EXPECT_EQ(Word(KmdfChildWakeInD0), uint32_t(Snapshots == 2));
        ++Snapshots;
      }
      EXPECT_EQ(D2Requests, 1u);
      EXPECT_EQ(Snapshots, 3u);
      EXPECT_EQ(Before, (std::vector<std::string>{"D2", "D0", "D3"}));
      EXPECT_EQ(After, (std::vector<std::string>{"D0", "D3", "D0"}));
    }
  }
#else
  GTEST_SKIP()
      << "NEVERD_KMDF_CHILD_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

} // namespace
