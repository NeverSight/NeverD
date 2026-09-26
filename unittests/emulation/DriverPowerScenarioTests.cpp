//===- DriverPowerScenarioTests.cpp - Explicit power scenarios ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit power packet facts, per-PDO response queues and independent
/// reports.
///
//===----------------------------------------------------------------------===//

#include "DriverScenario.h"
#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <utility>

namespace neverd::emulation {
namespace {

constexpr char DeviceJSON[] = R"({"id":"power0","bus":"resource_free",
  "initial_device_power":"D0","initial_system_power":"working"})";
constexpr char OperationJSON[] = R"({"minor":"set","power_type":"device",
  "power_state":"D3","power_action":"sleep","system_context":"0xffffffff",
  "bus_completion":{"status":0,"delay_100ns":7}})";
constexpr char RequestJSON[] = R"({"kind":"power","device_id":"power0",
  "minor":"set","power_type":"device","power_state":"D3",
  "power_action":"sleep","system_context":"0xffffffff",
  "bus_completion":{"status":0,"delay_100ns":7}})";

std::string jsonText(llvm::json::Value Value) {
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  Stream << Value;
  return Text;
}

std::string scenario(llvm::StringRef Requests = RequestJSON,
                     llvm::StringRef Device = DeviceJSON) {
  return "{\"pnp_devices\":[" + Device.str() + "],\"requests\":[" +
         Requests.str() + "]}";
}

DriverPowerOperation operation() {
  return {DevicePowerRequest::Set,
          DriverPowerType::Device,
          static_cast<uint32_t>(DevicePowerState::D3),
          UINT32_MAX,
          DriverPowerAction::Sleep,
          {0, 7}};
}

DriverOptions options() {
  DriverOptions Result;
  Result.PnpDevices.push_back({"power0", DriverBusKind::ResourceFree,
                               DevicePowerState::D0,
                               SystemPowerState::Working});
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Power;
  Request.DeviceID = "power0";
  Request.Power = operation();
  Result.Requests.push_back(Request);
  return Result;
}

void invalidJSON(llvm::StringRef Text) {
  SCOPED_TRACE(Text.str());
  auto Syntax = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Syntax)) << llvm::toString(Syntax.takeError());
  auto Result = driverOptionsFromScenarioJSON(Text);
  ASSERT_FALSE(bool(Result));
  EXPECT_EQ(llvm::toString(Result.takeError()).find("driver scenario:"), 0u);
}

void invalidNative(const DriverOptions &Options, llvm::StringRef Diagnostic) {
  auto Error = validateDriverScenario(Options);
  ASSERT_TRUE(bool(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find(Diagnostic.str()),
            std::string::npos);
}

TEST(DriverPowerScenario, ExplicitPacketFactsPreserveOpaqueContext) {
  auto Result = driverOptionsFromScenarioJSON(scenario());
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Requests.size(), 1u);
  const auto &Request = Result->Requests.front();
  EXPECT_EQ(Request.Kind, DriverRequestKind::Power);
  EXPECT_EQ(Request.DeviceID, "power0");
  EXPECT_FALSE(Request.Pnp);
  ASSERT_TRUE(Request.Power);
  EXPECT_EQ(Request.Power->Minor, DevicePowerRequest::Set);
  EXPECT_EQ(Request.Power->Type, DriverPowerType::Device);
  EXPECT_EQ(Request.Power->State, static_cast<uint32_t>(DevicePowerState::D3));
  EXPECT_EQ(Request.Power->Action, DriverPowerAction::Sleep);
  EXPECT_EQ(Request.Power->SystemContext, UINT32_MAX);
  EXPECT_EQ(Request.Power->BusCompletion.Status, 0u);
  EXPECT_EQ(Request.Power->BusCompletion.Delay100ns, 7u);
  EXPECT_FALSE(Result->PnpDevices.front().InitialReportedDevicePower);
  EXPECT_TRUE(Result->PnpDevices.front().RequestedDevicePower.empty());
}

TEST(DriverPowerScenario, EveryPacketFactAndDeviceIdentityIsRequired) {
  for (const char *Field :
       {"device_id", "minor", "power_type", "power_state", "power_action",
        "system_context", "bus_completion"}) {
    auto Request = llvm::json::parse(RequestJSON);
    ASSERT_TRUE(bool(Request)) << llvm::toString(Request.takeError());
    Request->getAsObject()->erase(Field);
    invalidJSON(scenario(jsonText(std::move(*Request))));
  }
  for (const char *Field :
       {"device", "file", "code", "input", "output_size", "direct_input",
        "byte_offset", "cancel_after_100ns", "origin", "response_index"}) {
    auto Request = llvm::json::parse(RequestJSON);
    ASSERT_TRUE(bool(Request)) << llvm::toString(Request.takeError());
    (*Request->getAsObject())[Field] = 0;
    invalidJSON(scenario(jsonText(std::move(*Request))));
  }
}

TEST(DriverPowerScenario, TypesTargetsActionsAndContextHaveStrictBoundaries) {
  const std::pair<const char *, const char *> Changes[] = {
      {"minor", "stop"},
      {"power_type", "bus"},
      {"power_state", "D1"},
      {"power_state", "D2"},
      {"power_state", "working"},
      {"power_action", "hibernate"},
      {"power_action", "shutdown"},
      {"system_context", "0x100000000"},
      {"system_context", "not-hex"},
  };
  for (const auto &[Field, Value] : Changes) {
    auto Request = llvm::json::parse(RequestJSON);
    ASSERT_TRUE(bool(Request)) << llvm::toString(Request.takeError());
    (*Request->getAsObject())[Field] = Value;
    invalidJSON(scenario(jsonText(std::move(*Request))));
  }
  for (const char *Context : {"-1", "0.0", "4294967296"}) {
    std::string Request = RequestJSON;
    const std::string Old = "\"0xffffffff\"";
    Request.replace(Request.find(Old), Old.size(), Context);
    invalidJSON(scenario(Request));
  }
  invalidJSON(scenario(
      R"({"kind":"power","device_id":"power0","minor":"query",
         "power_type":"system","power_state":"working","power_action":"none",
         "system_context":0,"bus_completion":{"status":0}})"));
  auto System = driverOptionsFromScenarioJSON(scenario(
      R"({"kind":"power","device_id":"power0","minor":"query",
         "power_type":"system","power_state":"sleeping3","power_action":"sleep",
         "system_context":4294967295,"bus_completion":{"status":"0xc0000001"}})"));
  ASSERT_TRUE(bool(System)) << llvm::toString(System.takeError());
  EXPECT_EQ(System->Requests[0].Power->Type, DriverPowerType::System);
  EXPECT_EQ(System->Requests[0].Power->SystemContext, UINT32_MAX);
  EXPECT_EQ(System->Requests[0].Power->State,
            static_cast<uint32_t>(SystemPowerState::Sleeping3));
}

TEST(DriverPowerScenario, ReportedStateAndResponseQueueAreIndependentFacts) {
  auto Device = llvm::json::parse(DeviceJSON);
  auto Response = llvm::json::parse(OperationJSON);
  ASSERT_TRUE(bool(Device)) << llvm::toString(Device.takeError());
  ASSERT_TRUE(bool(Response)) << llvm::toString(Response.takeError());
  (*Device->getAsObject())["initial_reported_device_power"] = "D3";
  llvm::json::Array Queue;
  Queue.push_back(std::move(*Response));
  (*Device->getAsObject())["requested_device_power"] = std::move(Queue);
  auto Result = driverOptionsFromScenarioJSON(scenario("", jsonText(*Device)));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_TRUE(Result->Requests.empty());
  const auto &Configured = Result->PnpDevices.front();
  EXPECT_EQ(Configured.InitialDevicePower, DevicePowerState::D0);
  EXPECT_EQ(Configured.InitialReportedDevicePower, DevicePowerState::D3);
  ASSERT_EQ(Configured.RequestedDevicePower.size(), 1u);
  EXPECT_EQ(Configured.RequestedDevicePower[0].State,
            static_cast<uint32_t>(DevicePowerState::D3));
  for (const char *Bad : {"D1", "working", ""}) {
    (*Device->getAsObject())["initial_reported_device_power"] = Bad;
    invalidJSON(scenario("", jsonText(*Device)));
  }
  (*Device->getAsObject())["initial_reported_device_power"] = nullptr;
  invalidJSON(scenario("", jsonText(*Device)));
}

TEST(DriverPowerScenario, ResponseTemplatesCannotChooseAnotherDeviceOrOrigin) {
  for (const char *Field :
       {"device_id", "device", "kind", "origin", "response_index", "file"}) {
    auto Device = llvm::json::parse(DeviceJSON);
    auto Response = llvm::json::parse(OperationJSON);
    ASSERT_TRUE(bool(Device)) << llvm::toString(Device.takeError());
    ASSERT_TRUE(bool(Response)) << llvm::toString(Response.takeError());
    (*Response->getAsObject())[Field] = "other";
    llvm::json::Array Queue;
    Queue.push_back(std::move(*Response));
    (*Device->getAsObject())["requested_device_power"] = std::move(Queue);
    invalidJSON(scenario("", jsonText(std::move(*Device))));
  }
  auto Options = options();
  auto System = operation();
  System.Type = DriverPowerType::System;
  System.State = static_cast<uint32_t>(SystemPowerState::Sleeping3);
  Options.PnpDevices[0].RequestedDevicePower.push_back(System);
  invalidNative(Options, "requires device power type");
}

TEST(DriverPowerScenario,
     ResponseLimitCountsAllDevicesIndependentlyOfRequests) {
  auto Options = options();
  Options.PnpDevices.push_back({"power1", DriverBusKind::ResourceFree,
                                DevicePowerState::D0,
                                SystemPowerState::Working});
  Options.PnpDevices[0].RequestedDevicePower.assign(
      DriverScenarioPowerResponseLimit - 1, operation());
  Options.PnpDevices[1].RequestedDevicePower.push_back(operation());
  auto Error = validateDriverScenario(Options);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Options.PnpDevices[1].RequestedDevicePower.push_back(operation());
  invalidNative(Options, "combined response limit");
}

TEST(DriverPowerScenario, NativePreflightRejectsForgedOrMismatchedOperations) {
  const auto Base = options();
  std::vector<DriverOptions> Invalid;
  auto Add = [&](auto Change) {
    auto Current = Base;
    Change(Current);
    Invalid.push_back(std::move(Current));
  };
  Add([](auto &O) { O.Requests[0].Power.reset(); });
  Add([](auto &O) { O.Requests[0].DeviceID.clear(); });
  Add([](auto &O) { O.Requests[0].Pnp = DriverPnpOperation{}; });
  Add([](auto &O) { O.Requests[0].Kind = DriverRequestKind::Create; });
  Add([](auto &O) { O.Requests[0].Kind = DriverRequestKind::Pnp; });
  Add([](auto &O) {
    O.Requests[0].Power->Type = static_cast<DriverPowerType>(99);
  });
  Add([](auto &O) {
    O.Requests[0].Power->Action = static_cast<DriverPowerAction>(99);
  });
  Add([](auto &O) {
    O.Requests[0].Power->Minor = static_cast<DevicePowerRequest>(0);
  });
  Add([](auto &O) { O.Requests[0].Power->State = 0; });
  Add([](auto &O) { O.Requests[0].Power->BusCompletion.Status.reset(); });
  Add([](auto &O) { O.Requests[0].Power->BusCompletion.Status = 0x103; });
  Add([](auto &O) {
    O.Requests[0].Power->BusCompletion.Delay100ns = UINT64_MAX;
  });
  Add([](auto &O) { O.Requests[0].File = 1; });
  Add([](auto &O) { O.Requests[0].CancelAfter100ns = 0; });
  Add([](auto &O) {
    O.PnpDevices[0].InitialReportedDevicePower = DevicePowerState::D1;
  });
  for (const auto &Options : Invalid) {
    auto Result = emulateDriver("missing-power-preflight-image.sys", Options);
    ASSERT_FALSE(bool(Result));
    EXPECT_EQ(llvm::toString(Result.takeError()).find("driver scenario:"), 0u);
  }
}

TEST(DriverPowerScenario,
     ObservedChildrenHaveIndependentRowsAndResponseIdentity) {
  DriverResult Result;
  Result.Stop = DriverStopReason::Returned;
  Result.NTStatus = 0;
  Result.Configuration = options();
  Result.Configuration.PnpDevices[0].InitialReportedDevicePower =
      DevicePowerState::D3;
  Result.Configuration.PnpDevices[0].RequestedDevicePower.assign(2,
                                                                 operation());
  Result.PnpDevices.push_back(
      {"power0", 0x1000, 0, true, DevicePnpState::Started, true,
       DevicePowerState::D3, SystemPowerState::Sleeping3});
  Result.Devices.push_back({0x1000, 0, 0, "", DevicePowerState::D0});
  Result.Devices.push_back({0x2000, 0, 0, "", DevicePowerState::D3});
  DriverRequestResult Request;
  Request.Kind = DriverRequestKind::Power;
  Request.DeviceID = "power0";
  Request.IRP = 0x3000;
  Request.Completed = true;
  Request.DispatchStatus = 0x103;
  Request.IOStatus = 0;
  Request.Power = DriverPowerRequestResult{};
  Request.Power->State = static_cast<uint32_t>(DevicePowerState::D3);
  Request.Power->DeviceStateAfter = DevicePowerState::D3;
  Request.Power->SystemContext = UINT32_MAX;
  Request.Power->Action = DriverPowerAction::Sleep;
  Result.Requests.push_back(Request);
  Request.Origin = DriverRequestOrigin::PoRequestPowerIrp;
  Request.ResponseIndex = 0;
  Request.IRP = 0x4000;
  Request.Power->RequestedDeviceObject = 0x2000;
  Request.Power->BusStatus = 0;
  Request.Power->BusReceivedAt100ns = 0;
  Request.Power->BusCompletedAt100ns = 7;
  Result.Requests.push_back(Request);
  auto JSON = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
  const auto &Root = *JSON->getAsObject();
  EXPECT_EQ(Root.getBoolean("scenario_success"), true);
  const auto *Rows = Root.getArray("requests");
  ASSERT_NE(Rows, nullptr);
  ASSERT_EQ(Rows->size(), 2u);
  const auto &Scenario = *(*Rows)[0].getAsObject();
  EXPECT_EQ(Scenario.getString("origin"), "scenario");
  EXPECT_EQ(Scenario.get("response_index")->kind(), llvm::json::Value::Null);
  EXPECT_EQ(Scenario.get("file")->kind(), llvm::json::Value::Null);
  EXPECT_EQ(Scenario.getObject("power")->get("bus_status")->kind(),
            llvm::json::Value::Null);
  const auto &Child = *(*Rows)[1].getAsObject();
  EXPECT_EQ(Child.getString("origin"), "PoRequestPowerIrp");
  EXPECT_EQ(Child.getInteger("response_index"), 0);
  EXPECT_EQ(Child.getString("irp"), "0x4000");
  EXPECT_EQ(Child.getString("device_id"), "power0");
  const auto *Power = Child.getObject("power");
  ASSERT_NE(Power, nullptr);
  EXPECT_EQ(Power->getString("requested_device_object"), "0x2000");
  EXPECT_EQ(Power->getString("power_state"), "D3");
  EXPECT_EQ(Power->getString("power_action"), "sleep");
  EXPECT_EQ(Power->getInteger("system_context"), UINT32_MAX);
  EXPECT_EQ(Power->getInteger("bus_received_at_100ns"), 0);
  EXPECT_EQ(Power->getInteger("bus_completed_at_100ns"), 7);
  EXPECT_EQ(Root.getArray("devices")->front().getAsObject()->getString(
                "reported_device_power"),
            "D0");
  EXPECT_EQ((*Root.getArray("devices"))[1].getAsObject()->getString(
                "reported_device_power"),
            "D3");
  EXPECT_EQ(Root.getArray("pnp_devices")
                ->front()
                .getAsObject()
                ->getString("device_power"),
            "D3");
  const auto *Configured = Root.getObject("configuration")
                               ->getArray("pnp_devices")
                               ->front()
                               .getAsObject();
  EXPECT_EQ(Configured->getString("initial_reported_device_power"), "D3");
  EXPECT_EQ(Configured->getArray("requested_device_power")->size(), 2u);
  Result.Requests[1].IOStatus = 0xc0000001;
  JSON = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
  EXPECT_EQ(JSON->getAsObject()->getBoolean("scenario_success"), false);
  Result.Requests[1].IOStatus = 0;
  Result.Requests[1].Completed = false;
  JSON = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
  EXPECT_EQ(JSON->getAsObject()->getBoolean("scenario_success"), false);
}

TEST(DriverPowerScenario, UnconsumedResponsesDoNotInventChildObservations) {
  DriverResult Result;
  Result.Stop = DriverStopReason::Returned;
  Result.NTStatus = 0;
  Result.Configuration = options();
  Result.Configuration.Requests.clear();
  Result.Configuration.PnpDevices[0].RequestedDevicePower.assign(2,
                                                                 operation());
  Result.PnpDevices.push_back(
      {"power0", 0x1000, 0, false, DevicePnpState::NotStarted, true});
  auto JSON = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
  EXPECT_EQ(JSON->getAsObject()->getBoolean("scenario_success"), true);
  EXPECT_TRUE(JSON->getAsObject()->getArray("requests")->empty());
}

} // namespace
} // namespace neverd::emulation
