//===- DriverPnpScenarioTests.cpp - Explicit PnP input and observations
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// PnP schema preflight and report contracts, independent of guest execution.
///
//===----------------------------------------------------------------------===//

#include "DriverScenario.h"
#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"

#include <utility>

namespace neverd::emulation {
namespace {

constexpr char DeviceJSON[] = R"({"id":"port-0","bus":"resource_free",
  "initial_device_power":"D0","initial_system_power":"working"})";

DriverPnpDevice pnpDevice(std::string ID = "port-0") {
  return {std::move(ID), DriverBusKind::ResourceFree, DevicePowerState::D0,
          SystemPowerState::Working};
}

DriverRequest pnpRequest(DevicePnpRequest Minor = DevicePnpRequest::Start) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = "port-0";
  Request.Pnp = DriverPnpOperation{Minor, DriverBusCompletion{0, 0}};
  return Request;
}

std::string scenario(llvm::StringRef Request,
                     llvm::StringRef Device = DeviceJSON) {
  return "{\"pnp_devices\":[" + Device.str() + "],\"requests\":[" +
         Request.str() + "]}";
}

void invalidJSON(llvm::StringRef Text) {
  SCOPED_TRACE(Text.str());
  auto Syntax = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Syntax)) << llvm::toString(Syntax.takeError());
  auto Parsed = driverOptionsFromScenarioJSON(Text);
  ASSERT_FALSE(bool(Parsed));
  EXPECT_EQ(llvm::toString(Parsed.takeError()).find("driver scenario:"), 0u);
}

void invalidNative(const DriverOptions &Options, llvm::StringRef Text) {
  auto Error = validateDriverScenario(Options);
  ASSERT_TRUE(bool(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find(Text.str()),
            std::string::npos);
}

TEST(DriverPnpScenario, ParsesExplicitDevicesOperationsAndOrdinaryDeviceIDs) {
  auto Parsed = driverOptionsFromScenarioJSON(scenario(R"(
    {"kind":"pnp","device_id":"port-0","minor":"start",
     "bus_completion":{"status":"0x0","delay_100ns":17}},
    {"kind":"create","device_id":"port-0","file":9},
    {"kind":"pnp","device_id":"port-0","minor":"query_remove",
     "bus_completion":{"status":3221225473}},
    {"kind":"pnp","device_id":"port-0","minor":"cancel_remove",
     "bus_completion":{"status":0}},
    {"kind":"pnp","device_id":"port-0","minor":"remove",
     "bus_completion":{"status":0}}
  )"));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_EQ(Parsed->PnpDevices.size(), 1u);
  const auto &Device = Parsed->PnpDevices[0];
  EXPECT_EQ(Device.ID, "port-0");
  EXPECT_EQ(Device.Bus, DriverBusKind::ResourceFree);
  EXPECT_EQ(Device.InitialDevicePower, DevicePowerState::D0);
  EXPECT_EQ(Device.InitialSystemPower, SystemPowerState::Working);
  ASSERT_EQ(Parsed->Requests.size(), 5u);
  const auto &Start = Parsed->Requests[0];
  EXPECT_EQ(Start.Kind, DriverRequestKind::Pnp);
  EXPECT_EQ(Start.DeviceID, "port-0");
  EXPECT_TRUE(Start.Device.empty());
  ASSERT_TRUE(Start.Pnp);
  EXPECT_EQ(Start.Pnp->Minor, DevicePnpRequest::Start);
  EXPECT_EQ(Start.Pnp->BusCompletion.Status, 0u);
  EXPECT_EQ(Start.Pnp->BusCompletion.Delay100ns, 17u);
  EXPECT_FALSE(Parsed->Requests[1].Pnp);
  EXPECT_EQ(Parsed->Requests[1].File, 9u);
  EXPECT_EQ(Parsed->Requests[2].Pnp->BusCompletion.Status, 0xc0000001u);
  EXPECT_EQ(Parsed->Requests[3].Pnp->Minor, DevicePnpRequest::CancelRemove);
  EXPECT_EQ(Parsed->Requests[3].Pnp->BusCompletion.Delay100ns, 0u);
  EXPECT_EQ(Parsed->Requests[4].Pnp->Minor, DevicePnpRequest::Remove);
}

TEST(DriverPnpScenario, MissingHardwareAndPowerFactsAreNeverDefaulted) {
  for (
      const char *Device :
      {"null", "[]", R"({})", R"({"id":"port-0"})",
       R"({"id":"port-0","bus":"resource_free","initial_device_power":"D0"})",
       R"({"id":"port-0","bus":"resource_free","initial_system_power":"working"})",
       R"({"id":"port-0","initial_device_power":"D0","initial_system_power":"working"})",
       R"({"id":"port-0","bus":"pci","initial_device_power":"D0","initial_system_power":"working"})",
       R"({"id":"port-0","bus":"resource_free","initial_device_power":"D3","initial_system_power":"working"})",
       R"({"id":"port-0","bus":"resource_free","initial_device_power":"D0","initial_system_power":"sleeping1"})",
       R"({"id":"port-0","bus":"resource_free","initial_device_power":1,"initial_system_power":"working"})",
       R"({"id":"port-0","bus":"resource_free","initial_device_power":"D0","initial_system_power":"working","resources":[]})"})
    invalidJSON(scenario("", Device));
  invalidJSON(R"({"pnp_devices":null})");
  invalidJSON(R"({"pnp_devices":{}})");
  DriverOptions Options;
  Options.PnpDevices.push_back(DriverPnpDevice{});
  Options.PnpDevices[0].ID = "port-0";
  invalidNative(Options, "explicit initial power");
}

TEST(DriverPnpScenario, IDsAreBoundedCaseSensitiveAndReferToConfiguredDevices) {
  auto CaseSensitive = driverOptionsFromScenarioJSON(
      "{\"pnp_devices\":[" + std::string(DeviceJSON) +
      R"(,{"id":"PORT-0","bus":"resource_free","initial_device_power":"D0","initial_system_power":"working"}]})");
  ASSERT_TRUE(bool(CaseSensitive)) << llvm::toString(CaseSensitive.takeError());
  for (const char *ID : {"", "with space", "/path", ".hidden", "_under",
                         "\\Device\\Name", "\xc3\xa9"}) {
    DriverOptions Options;
    Options.PnpDevices.push_back(pnpDevice(ID));
    invalidNative(Options, "identifier");
  }
  DriverOptions Options;
  Options.PnpDevices.push_back(
      pnpDevice(std::string(DriverScenarioDeviceIDLimit, 'a')));
  auto Error = validateDriverScenario(Options);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Options.PnpDevices[0].ID.push_back('a');
  invalidNative(Options, "identifier");
  Options.PnpDevices = {pnpDevice(), pnpDevice()};
  invalidNative(Options, "duplicate");
  Options.PnpDevices.resize(1);
  Options.Requests.push_back(pnpRequest());
  Options.Requests[0].DeviceID = "PORT-0";
  invalidNative(Options, "configured");
  invalidJSON(scenario(R"({"kind":"create","device_id":"missing"})"));
  invalidJSON(scenario(
      R"({"kind":"create","device_id":"port-0","device":"\\Device\\Named"})"));
  invalidJSON(scenario(R"({"kind":"create","device_id":0})"));
}

TEST(DriverPnpScenario, DeviceCountLimitIsIndependentOfRequestCount) {
  DriverOptions Options;
  for (size_t I = 0; I < DriverScenarioPnpDeviceLimit; ++I)
    Options.PnpDevices.push_back(pnpDevice("dev" + std::to_string(I)));
  auto Error = validateDriverScenario(Options);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Options.PnpDevices.push_back(pnpDevice("overflow"));
  invalidNative(Options, "device count");
}

TEST(DriverPnpScenario, PnpFieldsCannotMixWithFilesOrTransfersEvenWhenZero) {
  for (const char *Field :
       {R"("file":0)", R"("device":"\\Device\\Other")", R"("code":0)",
        R"("input":"")", R"("output_size":0)", R"("direct_input":"")",
        R"("byte_offset":0)", R"("cancel_after_100ns":0)"})
    invalidJSON(scenario(
        std::string(
            R"({"kind":"pnp","device_id":"port-0","minor":"start","bus_completion":{"status":0},)") +
        Field + "}"));
  for (
      const char *Request :
      {R"({"kind":"pnp","minor":"start","bus_completion":{"status":0}})",
       R"({"kind":"pnp","device_id":"port-0","bus_completion":{"status":0}})",
       R"({"kind":"pnp","device_id":"port-0","minor":"start"})",
       R"({"kind":"pnp","device_id":"port-0","minor":"query_resources","bus_completion":{"status":0}})",
       R"({"kind":"pnp","device_id":"port-0","minor":0,"bus_completion":{"status":0}})",
       R"({"kind":"create","minor":"start"})",
       R"({"kind":"create","bus_completion":null})"})
    invalidJSON(scenario(Request));
}

TEST(DriverPnpScenario, FileBusResponsesRequireAnExplicitFinalStatus) {
  auto Parsed = driverOptionsFromScenarioJSON(scenario(R"(
    {"kind":"create","device_id":"port-0",
     "bus_completion":{"status":0}},
    {"kind":"cleanup","device_id":"port-0",
     "bus_completion":{"status":0}},
    {"kind":"close","device_id":"port-0",
     "bus_completion":{"status":0}}
  )"));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_EQ(Parsed->Requests.size(), 3u);
  for (const auto &Request : Parsed->Requests) {
    ASSERT_TRUE(Request.FileBusCompletion);
    EXPECT_EQ(Request.FileBusCompletion->Status, 0u);
    EXPECT_EQ(Request.FileBusCompletion->Delay100ns, 0u);
  }
  auto Delayed = driverOptionsFromScenarioJSON(scenario(R"(
    {"kind":"create","device_id":"port-0",
     "bus_completion":{"status":0,"delay_100ns":17}}
  )"));
  ASSERT_TRUE(bool(Delayed)) << llvm::toString(Delayed.takeError());
  ASSERT_TRUE(Delayed->Requests.front().FileBusCompletion);
  EXPECT_EQ(Delayed->Requests.front().FileBusCompletion->Delay100ns, 17u);
  for (
      llvm::StringRef Request :
      {R"({"kind":"create","bus_completion":{"status":0}})",
       R"({"kind":"create","device_id":"port-0","bus_completion":{"status":259}})",
       R"({"kind":"cleanup","device_id":"port-0","bus_completion":{"status":0,"delay_100ns":1}})",
       R"({"kind":"cleanup","device_id":"port-0","bus_completion":{"delay_100ns":1}})",
       R"({"kind":"read","device_id":"port-0","bus_completion":{"status":0}})"})
    invalidJSON(scenario(Request));
  DriverOptions Native;
  Native.PnpDevices.push_back(pnpDevice());
  DriverRequest Create;
  Create.Kind = DriverRequestKind::Create;
  Create.DeviceID = "port-0";
  Create.FileBusCompletion = DriverBusCompletion{0, UINT64_MAX};
  Native.Requests.push_back(Create);
  invalidNative(Native, "file bus_completion delay exceeds signed time range");
  Native.Requests.front().Kind = DriverRequestKind::Cleanup;
  Native.Requests.front().FileBusCompletion->Delay100ns = 1;
  invalidNative(Native, "delayed file bus_completion requires CREATE");
}

TEST(DriverPnpScenario, BusCompletionHasExplicitFinalStatusAndBoundedDelay) {
  for (const char *Completion :
       {"null", "[]", R"({})", R"({"status":null})", R"({"status":true})",
        R"({"status":-1})", R"({"status":1.0})", R"({"status":4294967296})",
        R"({"status":"0x100000000"})", R"({"status":"103"})",
        R"({"status":"0x103"})", R"({"status":259})",
        R"({"status":0,"delay_100ns":-1})", R"({"status":0,"delay_100ns":1.0})",
        R"({"status":0,"delay_100ns":"0x10"})",
        R"({"status":0,"delay_100ns":9223372036854775808})",
        R"({"status":0,"extra":0})"})
    invalidJSON(scenario(
        std::string(
            R"({"kind":"pnp","device_id":"port-0","minor":"start","bus_completion":)") +
        Completion + "}"));
  for (const char *Minor :
       {"remove", "cancel_remove", "stop", "cancel_stop", "surprise_removal"})
    for (const char *Status : {"0x1", "0x80000005", "0xc0000001"})
      invalidJSON(scenario(
          std::string(R"({"kind":"pnp","device_id":"port-0","minor":")") +
          Minor + R"(","bus_completion":{"status":")" + Status + R"("}})"));
  auto Boundary = driverOptionsFromScenarioJSON(scenario(
      R"({"kind":"pnp","device_id":"port-0","minor":"query_remove","bus_completion":{"status":"0xffffffff","delay_100ns":9223372036854775807}})"));
  ASSERT_TRUE(bool(Boundary)) << llvm::toString(Boundary.takeError());
  EXPECT_EQ(Boundary->Requests[0].Pnp->BusCompletion.Status, UINT32_MAX);
  EXPECT_EQ(Boundary->Requests[0].Pnp->BusCompletion.Delay100ns,
            uint64_t(INT64_MAX));
}

TEST(DriverPnpScenario, DuplicateNestedAndEscapedFieldsAreRejected) {
  invalidJSON(scenario(
      R"({"kind":"pnp","device_id":"port-0","minor":"start","bus_completion":{"status":0,"\u0073tatus":0}})"));
  invalidJSON(scenario(
      "",
      R"({"id":"port-0","\u0069d":"other","bus":"resource_free","initial_device_power":"D0","initial_system_power":"working"})"));
  invalidJSON("{\"pnp_devices\":[" + std::string(DeviceJSON) + ',' +
              DeviceJSON + "]}");
}

TEST(DriverPnpScenario, NativePreflightMatchesJSONBeforeImageLoading) {
  DriverOptions Base;
  Base.PnpDevices.push_back(pnpDevice());
  Base.Requests.push_back(pnpRequest());
  std::vector<DriverOptions> Invalid;
  auto Add = [&](auto Mutate) {
    auto Options = Base;
    Mutate(Options);
    Invalid.push_back(std::move(Options));
  };
  Add([](auto &O) { O.Requests[0].Pnp.reset(); });
  Add([](auto &O) { O.Requests[0].Pnp->BusCompletion.Status.reset(); });
  Add([](auto &O) { O.Requests[0].Pnp->BusCompletion.Status = 0x103; });
  Add([](auto &O) {
    O.Requests[0].Pnp->BusCompletion.Delay100ns = UINT64_MAX;
  });
  Add([](auto &O) {
    O.Requests[0].Pnp->Minor = DevicePnpRequest::QueryStop;
    O.Requests[0].Pnp->BusCompletion.Status = 0x119;
  });
  Add([](auto &O) {
    O.Requests[0].Pnp->Minor = static_cast<DevicePnpRequest>(0xff);
  });
  Add([](auto &O) { O.Requests[0].File = 7; });
  Add([](auto &O) { O.Requests[0].ControlCode = 1; });
  Add([](auto &O) { O.Requests[0].CancelAfter100ns = 0; });
  Add([](auto &O) { O.Requests[0].Kind = DriverRequestKind::Create; });
  Add([](auto &O) { O.Requests[0].Device = "\\Device\\Other"; });
  Add([](auto &O) { O.PnpDevices[0].Bus = static_cast<DriverBusKind>(99); });
  Add([](auto &O) {
    O.PnpDevices[0].InitialDevicePower = DevicePowerState::D3;
  });
  for (const auto &Options : Invalid) {
    auto Result = emulateDriver("missing-pnp-preflight-image.sys", Options);
    ASSERT_FALSE(bool(Result));
    EXPECT_EQ(llvm::toString(Result.takeError()).find("driver scenario:"), 0u);
  }
}

TEST(DriverPnpScenario,
     OmittedInventoryPreservesBaseAndExplicitEmptyReplacesIt) {
  DriverOptions Base;
  Base.PnpDevices.push_back(pnpDevice());
  Base.InstructionLimit = 77;
  auto Preserved = driverOptionsFromScenarioJSON("{}", Base);
  ASSERT_TRUE(bool(Preserved)) << llvm::toString(Preserved.takeError());
  ASSERT_EQ(Preserved->PnpDevices.size(), 1u);
  EXPECT_EQ(Preserved->PnpDevices[0].ID, "port-0");
  EXPECT_EQ(Preserved->InstructionLimit, 77u);
  auto Replaced = driverOptionsFromScenarioJSON(R"({"pnp_devices":[]})", Base);
  ASSERT_TRUE(bool(Replaced)) << llvm::toString(Replaced.takeError());
  EXPECT_TRUE(Replaced->PnpDevices.empty());
  ASSERT_EQ(Base.PnpDevices.size(), 1u);
}

TEST(DriverPnpScenario,
     ReportSeparatesConfiguredPolicyFromObservedBusCompletion) {
  DriverResult Result;
  Result.Stop = DriverStopReason::Returned;
  Result.NTStatus = 0;
  Result.Configuration.PnpDevices.push_back(pnpDevice());
  Result.Configuration.Requests.push_back(pnpRequest());
  Result.PnpDevices.push_back(
      {"port-0", 0x1000, 0, true, DevicePnpState::Started, true});
  DriverRequestResult Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = "port-0";
  Request.IRP = 0x2000;
  Request.Completed = true;
  Request.DispatchStatus = 0x103;
  Request.IOStatus = 0;
  Request.Pnp = DriverPnpRequestResult{DevicePnpRequest::Start,
                                       DevicePnpState::NotStarted,
                                       DevicePnpState::Started,
                                       0,
                                       0,
                                       7};
  Result.Requests.push_back(Request);
  auto Parsed = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  const auto *Root = Parsed->getAsObject();
  ASSERT_NE(Root, nullptr);
  EXPECT_EQ(Root->getBoolean("scenario_success"), true);
  EXPECT_EQ(Root->getInteger("nt_status"), 0);
  const auto *Devices = Root->getArray("pnp_devices");
  ASSERT_NE(Devices, nullptr);
  ASSERT_EQ(Devices->size(), 1u);
  const auto *Device = (*Devices)[0].getAsObject();
  ASSERT_NE(Device, nullptr);
  EXPECT_EQ(Device->getString("id"), "port-0");
  EXPECT_EQ(Device->getString("pdo"), "0x1000");
  EXPECT_EQ(Device->getInteger("add_device_status"), 0);
  EXPECT_EQ(Device->getString("pnp_state"), "started");
  EXPECT_EQ(Device->getBoolean("attached"), true);
  EXPECT_EQ(Device->getBoolean("provider_present"), true);
  const auto *Configuration = Root->getObject("configuration");
  ASSERT_NE(Configuration, nullptr);
  const auto *Configured = Configuration->getArray("pnp_devices");
  ASSERT_NE(Configured, nullptr);
  EXPECT_EQ((*Configured)[0].getAsObject()->getString("bus"), "resource_free");
  EXPECT_EQ((*Configured)[0].getAsObject()->getString("initial_device_power"),
            "D0");
  EXPECT_EQ((*Configured)[0].getAsObject()->getString("initial_system_power"),
            "working");
  const auto *Requests = Root->getArray("requests");
  ASSERT_NE(Requests, nullptr);
  const auto *Item = (*Requests)[0].getAsObject();
  ASSERT_NE(Item, nullptr);
  EXPECT_EQ(Item->getString("kind"), "pnp");
  EXPECT_EQ(Item->getString("device_id"), "port-0");
  ASSERT_NE(Item->get("file"), nullptr);
  EXPECT_EQ(Item->get("file")->kind(), llvm::json::Value::Null);
  const auto *Pnp = Item->getObject("pnp");
  ASSERT_NE(Pnp, nullptr);
  EXPECT_EQ(Pnp->getString("minor"), "start");
  EXPECT_EQ(Pnp->getString("state_before"), "not_started");
  EXPECT_EQ(Pnp->getString("state_after"), "started");
  EXPECT_EQ(Pnp->getInteger("bus_status"), 0);
  EXPECT_EQ(Pnp->getInteger("bus_received_at_100ns"), 0);
  EXPECT_EQ(Pnp->getInteger("bus_completed_at_100ns"), 7);
  Result.Requests[0].Pnp->BusStatus.reset();
  Result.Requests[0].Pnp->BusReceivedAt100ns.reset();
  Result.Requests[0].Pnp->BusCompletedAt100ns.reset();
  Parsed = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  Pnp = (*Parsed->getAsObject()->getArray("requests"))[0]
            .getAsObject()
            ->getObject("pnp");
  ASSERT_NE(Pnp, nullptr);
  for (const char *Name :
       {"bus_status", "bus_received_at_100ns", "bus_completed_at_100ns"}) {
    ASSERT_NE(Pnp->get(Name), nullptr);
    EXPECT_EQ(Pnp->get(Name)->kind(), llvm::json::Value::Null);
  }
}

TEST(DriverPnpScenario,
     AddDeviceFailureOrMissingObservationPreventsScenarioSuccess) {
  DriverResult Result;
  Result.Stop = DriverStopReason::Returned;
  Result.NTStatus = 0;
  Result.Configuration.PnpDevices.push_back(pnpDevice());
  Result.PnpDevices.push_back(
      {"port-0", 0x1000, 0xc0000001, false, DevicePnpState::NotStarted, true});
  for (unsigned Step = 0; Step != 3; ++Step) {
    auto Parsed = llvm::json::parse(driverResultJSON(Result));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    EXPECT_EQ(Parsed->getAsObject()->getInteger("nt_status"), 0);
    EXPECT_EQ(Parsed->getAsObject()->getBoolean("nt_success"), true);
    EXPECT_EQ(Parsed->getAsObject()->getBoolean("scenario_success"), false);
    if (!Step)
      Result.PnpDevices[0].AddDeviceStatus.reset();
    else
      Result.PnpDevices.clear();
  }
}

TEST(DriverPnpScenario,
     LegacyRequestFileAndStatusFieldsKeepTheirTypesAndValues) {
  DriverResult Result;
  DriverRequestResult Request;
  Request.Kind = DriverRequestKind::Read;
  Request.File = 9;
  Request.Device = "\\Device\\Legacy";
  Request.DispatchStatus = 0;
  Result.Requests.push_back(Request);
  auto Parsed = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  const auto *Item =
      (*Parsed->getAsObject()->getArray("requests"))[0].getAsObject();
  ASSERT_NE(Item, nullptr);
  EXPECT_EQ(Item->getInteger("file"), 9);
  EXPECT_EQ(Item->getInteger("dispatch_status"), 0);
  EXPECT_EQ(Item->getString("device"), "\\Device\\Legacy");
  EXPECT_EQ(Item->getString("kind"), "read");
  EXPECT_EQ(Item->get("device_id")->kind(), llvm::json::Value::Null);
  EXPECT_EQ(Item->get("pnp")->kind(), llvm::json::Value::Null);
}

TEST(DriverPnpScenario, EightMinorSpellingsRoundTripThroughReports) {
  const std::pair<const char *, DevicePnpRequest> Cases[] = {
      {"start", DevicePnpRequest::Start},
      {"query_remove", DevicePnpRequest::QueryRemove},
      {"cancel_remove", DevicePnpRequest::CancelRemove},
      {"remove", DevicePnpRequest::Remove},
      {"query_stop", DevicePnpRequest::QueryStop},
      {"stop", DevicePnpRequest::Stop},
      {"cancel_stop", DevicePnpRequest::CancelStop},
      {"surprise_removal", DevicePnpRequest::SurpriseRemoval},
  };
  for (const auto &[Spelling, Minor] : Cases) {
    SCOPED_TRACE(Spelling);
    auto Options = driverOptionsFromScenarioJSON(scenario(
        std::string(R"({"kind":"pnp","device_id":"port-0","minor":")") +
        Spelling + R"(","bus_completion":{"status":0}})"));
    ASSERT_TRUE(bool(Options)) << llvm::toString(Options.takeError());
    ASSERT_EQ(Options->Requests.size(), 1u);
    EXPECT_EQ(Options->Requests[0].Pnp->Minor, Minor);
    DriverResult Result;
    Result.Configuration = *Options;
    DriverRequestResult Request;
    Request.Kind = DriverRequestKind::Pnp;
    Request.DeviceID = "port-0";
    Request.Pnp = DriverPnpRequestResult{};
    Request.Pnp->Minor = Minor;
    Request.Pnp->StateBefore = DevicePnpState::StopPending;
    Request.Pnp->StateAfter = DevicePnpState::Stopped;
    Result.Requests.push_back(Request);
    auto JSON = llvm::json::parse(driverResultJSON(Result));
    ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
    const auto *Pnp = (*JSON->getAsObject()->getArray("requests"))[0]
                          .getAsObject()
                          ->getObject("pnp");
    ASSERT_NE(Pnp, nullptr);
    EXPECT_EQ(Pnp->getString("minor"), Spelling);
    EXPECT_EQ(Pnp->getString("state_before"), "stop_pending");
    EXPECT_EQ(Pnp->getString("state_after"), "stopped");
    EXPECT_EQ(Pnp->get("bus_status")->kind(), llvm::json::Value::Null);
  }
}

TEST(DriverPnpScenario, QueryStopResourceRequeryFailsJSONAndNativePreflight) {
  for (const char *Status : {"281", "\"0x119\""})
    invalidJSON(scenario(
        std::string(
            R"({"kind":"pnp","device_id":"port-0","minor":"query_stop","bus_completion":{"status":)") +
        Status + "}}"));
  DriverOptions Options;
  Options.PnpDevices.push_back(pnpDevice());
  Options.Requests.push_back(pnpRequest(DevicePnpRequest::QueryStop));
  Options.Requests[0].Pnp->BusCompletion.Status = 0x119;
  invalidNative(Options, "resource requery");
  EXPECT_STREQ(devicePnpFinalStatusError(DevicePnpRequest::QueryStop, 0x119),
               "STATUS_RESOURCE_REQUIREMENTS_CHANGED requires unsupported "
               "resource requery");
  EXPECT_EQ(devicePnpFinalStatusError(DevicePnpRequest::QueryStop, 0), nullptr);
  EXPECT_EQ(devicePnpFinalStatusError(DevicePnpRequest::QueryStop, 0xc0000001),
            nullptr);
  EXPECT_EQ(devicePnpFinalStatusError(DevicePnpRequest::Start, 0x119), nullptr);
  EXPECT_EQ(devicePnpFinalStatusError(DevicePnpRequest::QueryRemove, 0x119),
            nullptr);
}

TEST(DriverPnpScenario, FinalStatusPolicyIsSharedWithoutWeakeningExactSuccess) {
  for (DevicePnpRequest Minor :
       {DevicePnpRequest::Stop, DevicePnpRequest::CancelStop,
        DevicePnpRequest::SurpriseRemoval, DevicePnpRequest::Remove,
        DevicePnpRequest::CancelRemove}) {
    EXPECT_EQ(devicePnpFinalStatusError(Minor, 0), nullptr);
    EXPECT_STREQ(devicePnpFinalStatusError(Minor, 1),
                 "this PnP request requires STATUS_SUCCESS");
    EXPECT_STREQ(devicePnpFinalStatusError(Minor, 0xc0000001),
                 "this PnP request must not fail");
    EXPECT_STREQ(devicePnpFinalStatusError(Minor, 0x103),
                 "STATUS_PENDING is not a final PnP completion");
  }
  EXPECT_STREQ(
      devicePnpFinalStatusError(static_cast<DevicePnpRequest>(0xff), 0),
      "unsupported pnp minor");
}
} // namespace
} // namespace neverd::emulation
