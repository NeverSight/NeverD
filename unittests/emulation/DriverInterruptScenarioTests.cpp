//===- DriverInterruptScenarioTests.cpp - Explicit interrupt scenarios ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Interrupt input and observations without WDK dependencies or a guest CPU.
///
//===----------------------------------------------------------------------===//

#include "DriverScenario.h"
#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {

constexpr char InterruptJSON[] = R"({"id":"irq-0","raw_vector":"0xffffffff",
  "raw_level":37,"raw_affinity":"0x1","translated_vector":81,
  "translated_level":7,"translated_affinity":1,"mode":"latched",
  "share":"device_exclusive"})";
constexpr char EventJSON[] =
    R"({"after_100ns":"0x7","device_id":"unit-0","interrupt_id":"irq-0"})";

std::string scenario(llvm::StringRef Interrupt = InterruptJSON,
                     llvm::StringRef Event = EventJSON) {
  return R"({"pnp_devices":[{"id":"unit-0","bus":"register_bank",
    "initial_device_power":"D0","initial_system_power":"working",
    "interrupts":[)" +
         Interrupt.str() +
         R"(]}],"requests":[{"kind":"ioctl","code":"0x222000",
    "interrupt_events":[)" +
         Event.str() + "]}]}";
}

DriverPnpDevice device(std::string ID = "unit-0", uint32_t Vector = 81) {
  DriverPnpDevice Result;
  Result.ID = std::move(ID);
  Result.Bus = DriverBusKind::RegisterBank;
  Result.InitialDevicePower = DevicePowerState::D0;
  Result.InitialSystemPower = SystemPowerState::Working;
  Result.Interrupts.push_back({"irq-0", UINT32_MAX, 37, 1, Vector, 7, 1});
  return Result;
}

DriverOptions options() {
  DriverOptions Result;
  Result.PnpDevices.push_back(device());
  DriverRequest Request;
  Request.ControlCode = 0x222000;
  Request.InterruptEvents.push_back({7, "unit-0", "irq-0"});
  Result.Requests.push_back(std::move(Request));
  return Result;
}

void validNative(const DriverOptions &Options) {
  auto E = validateDriverScenario(Options);
  EXPECT_FALSE(bool(E)) << llvm::toString(std::move(E));
}

void invalidNative(const DriverOptions &Options, llvm::StringRef Text) {
  auto E = validateDriverScenario(Options);
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find(Text.str()), std::string::npos);
}

void invalidJSON(llvm::StringRef JSON,
                 llvm::StringRef Text = "driver scenario:") {
  SCOPED_TRACE(JSON.str());
  auto Syntax = llvm::json::parse(JSON);
  ASSERT_TRUE(bool(Syntax)) << llvm::toString(Syntax.takeError());
  auto Parsed = driverOptionsFromScenarioJSON(JSON);
  ASSERT_FALSE(bool(Parsed));
  EXPECT_NE(llvm::toString(Parsed.takeError()).find(Text.str()),
            std::string::npos);
}

TEST(DriverInterruptScenario, ParsesIndependentRawTranslatedAndPulseFacts) {
  auto Parsed = driverOptionsFromScenarioJSON(scenario());
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_EQ(Parsed->PnpDevices.size(), 1u);
  const auto &Device = Parsed->PnpDevices[0];
  EXPECT_TRUE(Device.Resources.empty());
  ASSERT_EQ(Device.Interrupts.size(), 1u);
  const auto &IRQ = Device.Interrupts[0];
  EXPECT_EQ(IRQ.ID, "irq-0");
  EXPECT_EQ(IRQ.RawVector, UINT32_MAX);
  EXPECT_EQ(IRQ.RawLevel, 37u);
  EXPECT_EQ(IRQ.RawAffinity, 1u);
  EXPECT_EQ(IRQ.TranslatedVector, 81u);
  EXPECT_EQ(IRQ.TranslatedLevel, 7u);
  EXPECT_EQ(IRQ.TranslatedAffinity, 1u);
  EXPECT_EQ(IRQ.Mode, DriverInterruptMode::Latched);
  EXPECT_EQ(IRQ.Share, DriverInterruptShare::DeviceExclusive);
  ASSERT_EQ(Parsed->Requests.size(), 1u);
  ASSERT_EQ(Parsed->Requests[0].InterruptEvents.size(), 1u);
  const auto &Event = Parsed->Requests[0].InterruptEvents[0];
  EXPECT_EQ(Event.After100ns, 7u);
  EXPECT_EQ(Event.DeviceID, "unit-0");
  EXPECT_EQ(Event.InterruptID, "irq-0");
  validNative(*Parsed);
}

TEST(DriverInterruptScenario, RequiresAllDescriptorAndEventFacts) {
  for (const char *Field :
       {"id", "raw_vector", "raw_level", "raw_affinity", "translated_vector",
        "translated_level", "translated_affinity", "mode", "share"}) {
    auto Value = llvm::json::parse(InterruptJSON);
    ASSERT_TRUE(bool(Value));
    Value->getAsObject()->erase(Field);
    invalidJSON(scenario(llvm::formatv("{0}", *Value).str()), "explicit");
  }
  for (const char *Field : {"after_100ns", "device_id", "interrupt_id"}) {
    auto Value = llvm::json::parse(EventJSON);
    ASSERT_TRUE(bool(Value));
    Value->getAsObject()->erase(Field);
    invalidJSON(scenario(InterruptJSON, llvm::formatv("{0}", *Value).str()),
                "explicit");
  }
  invalidJSON(scenario("null"));
  invalidJSON(scenario(InterruptJSON, "null"));
}

TEST(DriverInterruptScenario, SharedVectorsRequireMatchingTranslatedLineFacts) {
  auto Options = options();
  Options.PnpDevices.push_back(device("unit-1"));
  for (auto &Device : Options.PnpDevices)
    Device.Interrupts.front().Share = DriverInterruptShare::Shared;
  validNative(Options);
  auto &Peer = Options.PnpDevices.back().Interrupts.front();
  Peer.TranslatedLevel += 1;
  invalidNative(Options, "same shared line");
  Peer.TranslatedLevel -= 1;
  Peer.Share = DriverInterruptShare::DeviceExclusive;
  invalidNative(Options, "same shared line");
  auto Value = llvm::json::parse(InterruptJSON);
  ASSERT_TRUE(bool(Value));
  (*Value->getAsObject())["share"] = "shared";
  auto Parsed = driverOptionsFromScenarioJSON(
      scenario(llvm::formatv("{0}", *Value).str()));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->PnpDevices.front().Interrupts.front().Share,
            DriverInterruptShare::Shared);
}

TEST(DriverInterruptScenario,
     LevelSourcesRequireExplicitPeriodsAndStateActions) {
  auto Options = options();
  auto &Resource = Options.PnpDevices.front().Interrupts.front();
  auto &Event = Options.Requests.front().InterruptEvents.front();
  Resource.Mode = DriverInterruptMode::LevelSensitive;
  Event.Action = DriverInterruptAction::Assert;
  invalidNative(Options, "retrigger_after_100ns");
  Resource.RetriggerAfter100ns = 0;
  invalidNative(Options, "retrigger_after_100ns");
  Resource.RetriggerAfter100ns = uint64_t(INT64_MAX) + 1;
  invalidNative(Options, "retrigger_after_100ns");
  Resource.RetriggerAfter100ns = 3;
  validNative(Options);
  Event.Action = DriverInterruptAction::Deassert;
  validNative(Options);
  Event.Action = DriverInterruptAction::Pulse;
  invalidNative(Options, "assert/deassert");
  Event.Action = static_cast<DriverInterruptAction>(0xff);
  invalidNative(Options, "action");
  Event.Action = DriverInterruptAction::Assert;
  Resource.Mode = DriverInterruptMode::Latched;
  invalidNative(Options, "cannot specify retrigger_after_100ns");
  Resource.RetriggerAfter100ns.reset();
  invalidNative(Options, "pulse");

  auto IRQ = llvm::json::parse(InterruptJSON);
  auto Input = llvm::json::parse(EventJSON);
  ASSERT_TRUE(bool(IRQ));
  ASSERT_TRUE(bool(Input));
  (*IRQ->getAsObject())["mode"] = "level_sensitive";
  (*IRQ->getAsObject())["retrigger_after_100ns"] = "0x3";
  (*Input->getAsObject())["action"] = "assert";
  auto Parse = [&] {
    return scenario(llvm::formatv("{0}", *IRQ).str(),
                    llvm::formatv("{0}", *Input).str());
  };
  auto Parsed = driverOptionsFromScenarioJSON(Parse());
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->PnpDevices.front().Interrupts.front().RetriggerAfter100ns,
            3u);
  EXPECT_EQ(Parsed->Requests.front().InterruptEvents.front().Action,
            DriverInterruptAction::Assert);
  for (const char *Bad : {"null", "true", "-1", "0", "[]", "1.5", "\"3\""}) {
    auto Value = llvm::json::parse(Bad);
    ASSERT_TRUE(bool(Value));
    (*IRQ->getAsObject())["retrigger_after_100ns"] = std::move(*Value);
    invalidJSON(Parse());
  }
  (*IRQ->getAsObject())["retrigger_after_100ns"] = 3;
  for (const char *Bad : {"null", "true", "0", "[]", "\"ack\""}) {
    auto Value = llvm::json::parse(Bad);
    ASSERT_TRUE(bool(Value));
    (*Input->getAsObject())["action"] = std::move(*Value);
    invalidJSON(Parse(), "action");
  }
}

TEST(DriverInterruptScenario, SharedLevelSourcesRequireOneSamplingPeriod) {
  auto Options = options();
  Options.PnpDevices.push_back(device("unit-1"));
  for (auto &Device : Options.PnpDevices) {
    auto &Resource = Device.Interrupts.front();
    Resource.Mode = DriverInterruptMode::LevelSensitive;
    Resource.Share = DriverInterruptShare::Shared;
    Resource.RetriggerAfter100ns = 3;
  }
  Options.Requests.front().InterruptEvents.front().Action =
      DriverInterruptAction::Assert;
  validNative(Options);
  Options.PnpDevices.back().Interrupts.front().RetriggerAfter100ns = 4;
  invalidNative(Options, "same shared line");
}

TEST(DriverInterruptScenario, RejectsWrongTypesUnknownFactsAndDuplicates) {
  for (const char *Field :
       {"raw_vector", "raw_level", "raw_affinity", "translated_vector",
        "translated_level", "translated_affinity"}) {
    for (const char *Bad : {"-1", "true", "null", "[]", "{}", "1.5", "\"42\"",
                            "\"0x10000000000000000\""}) {
      auto Value = llvm::json::parse(InterruptJSON);
      auto Replacement = llvm::json::parse(Bad);
      ASSERT_TRUE(bool(Value));
      ASSERT_TRUE(bool(Replacement));
      (*Value->getAsObject())[Field] = std::move(*Replacement);
      invalidJSON(scenario(llvm::formatv("{0}", *Value).str()));
    }
  }
  for (const char *Field :
       {"raw_vector", "raw_level", "translated_vector", "translated_level"}) {
    auto Value = llvm::json::parse(InterruptJSON);
    ASSERT_TRUE(bool(Value));
    (*Value->getAsObject())[Field] = "0x100000000";
    invalidJSON(scenario(llvm::formatv("{0}", *Value).str()), "32 bits");
  }
  for (const char *Field : {"enabled", "group", "level", "acknowledge"}) {
    auto Value = llvm::json::parse(InterruptJSON);
    ASSERT_TRUE(bool(Value));
    (*Value->getAsObject())[Field] = 0;
    invalidJSON(scenario(llvm::formatv("{0}", *Value).str()), "unknown");
  }
  invalidJSON(scenario(InterruptJSON,
                       R"({"after_100ns":0,"device_id":"unit-0",
                      "interrupt_id":"irq-0","at_100ns":0})"),
              "unknown");
  invalidJSON(scenario(InterruptJSON,
                       R"({"after_100ns":0,"device_id":"unit-0",
                      "interrupt_id":"irq-0","\u0061fter_100ns":1})"),
              "duplicate");
}

TEST(DriverInterruptScenario,
     PreservesMemoryOnlyAndAllowsMixedAndInterruptOnly) {
  auto Options = options();
  validNative(Options);
  Options.PnpDevices[0].Resources.push_back({"bank", 0, 4096, 4, {}});
  validNative(Options);
  Options.Requests.clear();
  Options.PnpDevices[0].Interrupts.clear();
  validNative(Options);
  Options.PnpDevices[0].Resources.clear();
  invalidNative(Options, "require resources or interrupts");
  auto Value = llvm::json::parse(scenario());
  ASSERT_TRUE(bool(Value));
  auto *Device =
      (*Value->getAsObject()->getArray("pnp_devices"))[0].getAsObject();
  (*Device)["resources"] = llvm::json::Array{};
  auto Parsed =
      driverOptionsFromScenarioJSON(llvm::formatv("{0}", *Value).str());
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  (*Device)["interrupts"] = llvm::json::Array{};
  invalidJSON(llvm::formatv("{0}", *Value).str(),
              "require resources or interrupts");
}

TEST(DriverInterruptScenario, ResourceFreeRejectsEvenExplicitEmptyInventory) {
  auto Options = options();
  Options.PnpDevices[0].Bus = DriverBusKind::ResourceFree;
  invalidNative(Options, "resource_free");
  Options.PnpDevices[0].Interrupts.clear();
  Options.Requests.clear();
  validNative(Options);
  invalidJSON(R"({"pnp_devices":[{"id":"unit-0","bus":"resource_free",
    "initial_device_power":"D0","initial_system_power":"working",
    "interrupts":[]}]})",
              "resource_free");
  auto Parsed = driverOptionsFromScenarioJSON(R"({"pnp_devices":[{
    "id":"unit-0","bus":"resource_free","initial_device_power":"D0",
    "initial_system_power":"working"}]})");
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
}

TEST(DriverInterruptScenario,
     EnforcesCpuGroupModeAndDeviceIrqlWithoutVectorGuess) {
  auto Options = options();
  auto &IRQ = Options.PnpDevices[0].Interrupts[0];
  for (uint32_t Vector : {0u, UINT32_MAX}) {
    IRQ.RawVector = Vector;
    IRQ.TranslatedVector = Vector;
    for (size_t Level :
         {DriverInterruptMinimumLevel, DriverInterruptMaximumLevel}) {
      IRQ.TranslatedLevel = Level;
      IRQ.RawLevel = DriverInterruptRawLevelLimit;
      validNative(Options);
    }
  }
  IRQ.RawLevel = DriverInterruptRawLevelLimit + 1;
  invalidNative(Options, "group-zero");
  IRQ.RawLevel = 0;
  for (uint32_t Level : {0u, 2u, 13u, UINT32_MAX}) {
    IRQ.TranslatedLevel = Level;
    invalidNative(Options, "DIRQL");
  }
  IRQ.TranslatedLevel = DriverInterruptMinimumLevel;
  for (uint64_t Affinity :
       {uint64_t(0), uint64_t(2), uint64_t(3), UINT64_MAX}) {
    IRQ.RawAffinity = Affinity;
    invalidNative(Options, "CPU zero");
    IRQ.RawAffinity = 1;
    IRQ.TranslatedAffinity = Affinity;
    invalidNative(Options, "CPU zero");
    IRQ.TranslatedAffinity = 1;
  }
  IRQ.Mode = static_cast<DriverInterruptMode>(2);
  invalidNative(Options, "mode");
  IRQ.Mode = DriverInterruptMode::Latched;
  IRQ.Share = static_cast<DriverInterruptShare>(4);
  invalidNative(Options, "share");
  for (const char *Field : {"mode", "share"}) {
    auto Value = llvm::json::parse(InterruptJSON);
    ASSERT_TRUE(bool(Value));
    (*Value->getAsObject())[Field] = "level_shared";
    invalidJSON(scenario(llvm::formatv("{0}", *Value).str()), "unsupported");
  }
}

TEST(DriverInterruptScenario,
     IDsAreLocalButTranslatedVectorsAreGloballyExclusive) {
  auto Options = options();
  Options.PnpDevices.push_back(device("unit-1", 82));
  validNative(Options);
  Options.PnpDevices[1].Interrupts[0].TranslatedVector = 81;
  invalidNative(Options, "globally exclusive");
  Options.PnpDevices[1].Interrupts[0].TranslatedVector = 82;
  auto Second = Options.PnpDevices[0].Interrupts[0];
  Second.ID = "IRQ-0";
  Second.TranslatedVector = 83;
  Options.PnpDevices[0].Interrupts.push_back(Second);
  validNative(Options);
  Options.PnpDevices[0].Interrupts[1].ID = "irq-0";
  invalidNative(Options, "duplicate interrupt id");
  Options.PnpDevices[0].Interrupts.pop_back();
  for (const char *ID :
       {"", ".hidden", "_under", "with space", "/path", "\xc3\xa9"}) {
    Options.PnpDevices[1].Interrupts[0].ID = ID;
    invalidNative(Options, "ASCII identifier");
  }
  Options.PnpDevices[1].Interrupts[0].ID.assign(DriverScenarioInterruptIDLimit,
                                                'i');
  validNative(Options);
  Options.PnpDevices[1].Interrupts[0].ID.push_back('i');
  invalidNative(Options, "ASCII identifier");
}

TEST(DriverInterruptScenario, BoundsPerDeviceAndCombinedInterruptCounts) {
  auto Options = options();
  Options.PnpDevices.clear();
  for (size_t D = 0; D < DriverScenarioInterruptLimit /
                             DriverScenarioInterruptsPerDeviceLimit;
       ++D) {
    auto Device = device("unit-" + std::to_string(D));
    Device.Interrupts.clear();
    for (size_t I = 0; I < DriverScenarioInterruptsPerDeviceLimit; ++I)
      Device.Interrupts.push_back(
          {"irq-" + std::to_string(I), 0, 0, 1,
           static_cast<uint32_t>(D * DriverScenarioInterruptsPerDeviceLimit +
                                 I),
           3, 1});
    Options.PnpDevices.push_back(std::move(Device));
  }
  validNative(Options);
  Options.PnpDevices[0].Interrupts.push_back({"extra", 0, 0, 1, 1000, 3, 1});
  invalidNative(Options, "per-device count");
  Options.PnpDevices[0].Interrupts.pop_back();
  Options.PnpDevices.push_back(device("extra", 1000));
  invalidNative(Options, "combined count");
}

TEST(DriverInterruptScenario,
     EventsNameExplicitTargetsAndPreserveRepeatedPulseOrder) {
  auto Options = options();
  Options.PnpDevices.push_back(device("unit-1", 82));
  auto &Request = Options.Requests[0];
  Request.DeviceID = "unit-1";
  Request.InterruptEvents = {
      {7, "unit-0", "irq-0"}, {0, "unit-1", "irq-0"}, {7, "unit-0", "irq-0"}};
  validNative(Options);
  Request.InterruptEvents[1].After100ns = INT64_MAX;
  validNative(Options);
  ++Request.InterruptEvents[1].After100ns;
  invalidNative(Options, "signed 64-bit");
  Request.InterruptEvents[1].After100ns = 0;
  Request.InterruptEvents[1].DeviceID = "missing";
  invalidNative(Options, "configured pnp device");
  Request.InterruptEvents[1].DeviceID = "unit-1";
  Request.InterruptEvents[1].InterruptID = "missing";
  invalidNative(Options, "interrupt on the event device");
  Request.InterruptEvents[1].InterruptID = "bad id";
  invalidNative(Options, "ASCII identifiers");
  auto Parsed = driverOptionsFromScenarioJSON(
      scenario(InterruptJSON, std::string(EventJSON) + "," + EventJSON));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_EQ(Parsed->Requests[0].InterruptEvents.size(), 2u);
  EXPECT_EQ(Parsed->Requests[0].InterruptEvents[1].After100ns, 7u);
}

TEST(DriverInterruptScenario, EventsAreOnlyAcceptedOnTransferRequests) {
  auto Options = options();
  for (auto Kind : {DriverRequestKind::Read, DriverRequestKind::Write,
                    DriverRequestKind::DeviceControl}) {
    Options.Requests[0].Kind = Kind;
    Options.Requests[0].ControlCode = 0;
    validNative(Options);
  }
  for (auto Kind : {DriverRequestKind::Create, DriverRequestKind::Cleanup,
                    DriverRequestKind::Close, DriverRequestKind::Pnp,
                    DriverRequestKind::Power}) {
    Options.Requests[0].Kind = Kind;
    invalidNative(Options, "only for read, write and ioctl");
  }
  for (const char *Kind : {"create", "cleanup", "close", "pnp", "power"})
    invalidJSON(std::string(R"({"requests":[{"kind":")") + Kind +
                    R"(","interrupt_events":[]}]})",
                "only for read, write and ioctl");
  for (const char *Bad : {"-1", "true", "null", "[]", "1.5", "\"7\""}) {
    auto Value = llvm::json::parse(EventJSON);
    auto Replacement = llvm::json::parse(Bad);
    ASSERT_TRUE(bool(Value));
    ASSERT_TRUE(bool(Replacement));
    (*Value->getAsObject())["after_100ns"] = std::move(*Replacement);
    invalidJSON(scenario(InterruptJSON, llvm::formatv("{0}", *Value).str()));
  }
}

TEST(DriverInterruptScenario, BoundsEventsPerRequestAndAcrossScenario) {
  auto Options = options();
  Options.Requests[0].InterruptEvents.resize(
      DriverScenarioInterruptEventsPerRequestLimit, {0, "unit-0", "irq-0"});
  validNative(Options);
  Options.Requests[0].InterruptEvents.push_back({0, "unit-0", "irq-0"});
  invalidNative(Options, "per-request count");
  Options.Requests[0].InterruptEvents.pop_back();
  auto Request = Options.Requests[0];
  Options.Requests.resize(DriverScenarioInterruptEventLimit /
                              DriverScenarioInterruptEventsPerRequestLimit,
                          Request);
  validNative(Options);
  Options.Requests.push_back(Request);
  invalidNative(Options, "combined count");
}

TEST(DriverInterruptScenario,
     ReportsConfiguredFactsSeparatelyFromObservations) {
  DriverResult Result;
  Result.Configuration = options();
  auto Report = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  const auto *Root = Report->getAsObject();
  EXPECT_TRUE(Root->getArray("interrupts")->empty());
  EXPECT_TRUE(Root->getArray("requests")->empty());
  const auto *Configuration = Root->getObject("configuration");
  const auto *Device =
      (*Configuration->getArray("pnp_devices"))[0].getAsObject();
  EXPECT_TRUE(Device->getArray("resources")->empty());
  const auto *IRQ = (*Device->getArray("interrupts"))[0].getAsObject();
  EXPECT_EQ(IRQ->getInteger("raw_vector"), UINT32_MAX);
  EXPECT_EQ(IRQ->getInteger("raw_level"), 37);
  EXPECT_EQ(IRQ->getInteger("translated_vector"), 81);
  EXPECT_EQ(IRQ->getInteger("translated_level"), 7);
  EXPECT_EQ(IRQ->getString("mode"), "latched");
  EXPECT_EQ(IRQ->getString("share"), "device_exclusive");
  const auto *Event =
      (*Configuration->getArray("interrupt_events"))[0].getAsObject();
  EXPECT_EQ(Event->getInteger("source_request_index"), 0);
  EXPECT_EQ(Event->getInteger("event_index"), 0);
  EXPECT_EQ(Event->getInteger("after_100ns"), 7);
  EXPECT_EQ(Event->getString("device_id"), "unit-0");
  EXPECT_EQ(Event->getString("interrupt_id"), "irq-0");
}

DriverResult completedScenario() {
  DriverResult Result;
  Result.Configuration = options();
  Result.Stop = DriverStopReason::Returned;
  Result.NTStatus = 0;
  DriverPnpDeviceResult Device;
  Device.ID = "unit-0";
  Device.AddDeviceStatus = 0;
  Result.PnpDevices.push_back(Device);
  DriverRequestResult Request;
  Request.Completed = true;
  Request.DispatchStatus = 0;
  Request.IOStatus = 0;
  Result.Requests.push_back(Request);
  DriverInterruptResult Interrupt;
  Interrupt.DeviceID = "unit-0";
  Interrupt.InterruptID = "irq-0";
  Interrupt.Epoch = 2;
  Interrupt.DueAt100ns = 7;
  Interrupt.OccurredAt100ns = 7;
  Interrupt.DeliveredAt100ns = 9;
  Interrupt.ReturnedAt100ns = 9;
  Interrupt.InterruptObject = 0x12340000;
  Interrupt.ReturnValue = 0;
  Result.Interrupts.push_back(Interrupt);
  return Result;
}

TEST(DriverInterruptScenario, SharedHandlerReportPreservesIndividualBooleans) {
  auto Result = completedScenario();
  auto &Event = Result.Interrupts.front();
  Event.ReturnValue = 0x80;
  Event.Handlers = {{0x12340000, 9, 9, 0x80}, {0x12340010, 9, 9, 0}};
  auto Report = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  const auto *Item =
      Report->getAsObject()->getArray("interrupts")->front().getAsObject();
  ASSERT_TRUE(Item);
  EXPECT_EQ(Item->getBoolean("claimed"), true);
  const auto *Handlers = Item->getArray("handlers");
  ASSERT_TRUE(Handlers);
  ASSERT_EQ(Handlers->size(), 2u);
  EXPECT_EQ((*Handlers)[0].getAsObject()->getInteger("delivery_index"), 0);
  EXPECT_EQ((*Handlers)[0].getAsObject()->getInteger("return_value"), 0x80);
  EXPECT_EQ((*Handlers)[0].getAsObject()->getBoolean("claimed"), true);
  EXPECT_EQ((*Handlers)[1].getAsObject()->getInteger("return_value"), 0);
  EXPECT_EQ((*Handlers)[1].getAsObject()->getBoolean("claimed"), false);
}

TEST(DriverInterruptScenario,
     AppliedLevelStateReportsSuccessWithoutInventingAnIsr) {
  auto Result = completedScenario();
  auto &Resource = Result.Configuration.PnpDevices.front().Interrupts.front();
  Resource.Mode = DriverInterruptMode::LevelSensitive;
  Resource.RetriggerAfter100ns = 3;
  auto &Configured =
      Result.Configuration.Requests.front().InterruptEvents.front();
  auto &Event = Result.Interrupts.front();
  Configured.Action = Event.Action = DriverInterruptAction::Deassert;
  Event.DeliveredAt100ns.reset();
  Event.ReturnedAt100ns.reset();
  Event.InterruptObject.reset();
  Event.ReturnValue.reset();
  auto Report = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  const auto *Root = Report->getAsObject();
  EXPECT_EQ(Root->getBoolean("scenario_success"), true);
  const auto *Configuration = Root->getObject("configuration");
  const auto *Device =
      Configuration->getArray("pnp_devices")->front().getAsObject();
  EXPECT_EQ(Device->getArray("interrupts")
                ->front()
                .getAsObject()
                ->getInteger("retrigger_after_100ns"),
            3);
  EXPECT_EQ(Configuration->getArray("interrupt_events")
                ->front()
                .getAsObject()
                ->getString("action"),
            "deassert");
  const auto *Observed = Root->getArray("interrupts")->front().getAsObject();
  EXPECT_EQ(Observed->getString("action"), "deassert");
  EXPECT_TRUE(Observed->get("delivered_at_100ns")->getAsNull());
  EXPECT_TRUE(Observed->get("claimed")->getAsNull());
  EXPECT_TRUE(Observed->getArray("handlers")->empty());
  Event.OccurredAt100ns.reset();
  Report = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(Report));
  EXPECT_EQ(Report->getAsObject()->getBoolean("scenario_success"), false);
}

TEST(DriverInterruptScenario, UnclaimedBooleanIsNotAnNtStatusFailure) {
  auto Result = completedScenario();
  for (uint8_t Value : {0, 1, 0x80, 0xff}) {
    Result.Interrupts[0].ReturnValue = Value;
    auto Report = llvm::json::parse(driverResultJSON(Result));
    ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
    const auto *Root = Report->getAsObject();
    EXPECT_EQ(Root->getBoolean("scenario_success"), true);
    ASSERT_EQ(Root->getArray("requests")->size(), 1u);
    const auto *Event = (*Root->getArray("interrupts"))[0].getAsObject();
    EXPECT_EQ(Event->getInteger("return_value"), Value);
    EXPECT_EQ(Event->getBoolean("claimed"), Value != 0);
    EXPECT_EQ(Event->getInteger("epoch"), 2);
    EXPECT_EQ(Event->getInteger("due_at_100ns"), 7);
    EXPECT_EQ(Event->getInteger("occurred_at_100ns"), 7);
    EXPECT_EQ(Event->getInteger("delivered_at_100ns"), 9);
    EXPECT_EQ(Event->getInteger("returned_at_100ns"), 9);
    EXPECT_EQ(Event->getString("interrupt_object"), "0x12340000");
    EXPECT_TRUE(Event->get("undelivered_reason")->getAsNull());
    EXPECT_FALSE(Event->get("io_status"));
    EXPECT_FALSE(Event->get("irp"));
  }
}

TEST(DriverInterruptScenario, MissingOrUndeliveredPulseCannotReportSuccess) {
  auto Result = completedScenario();
  Result.Interrupts[0].DeliveredAt100ns.reset();
  Result.Interrupts[0].ReturnedAt100ns.reset();
  Result.Interrupts[0].ReturnValue.reset();
  Result.Interrupts[0].UndeliveredReason =
      "captured interrupt connection was disconnected";
  auto Report = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  EXPECT_EQ(Report->getAsObject()->getBoolean("scenario_success"), false);
  const auto *Event =
      (*Report->getAsObject()->getArray("interrupts"))[0].getAsObject();
  EXPECT_EQ(Event->getInteger("occurred_at_100ns"), 7);
  EXPECT_TRUE(Event->get("delivered_at_100ns")->getAsNull());
  EXPECT_TRUE(Event->get("returned_at_100ns")->getAsNull());
  EXPECT_TRUE(Event->get("return_value")->getAsNull());
  EXPECT_TRUE(Event->get("claimed")->getAsNull());
  EXPECT_EQ(Event->getString("undelivered_reason"),
            "captured interrupt connection was disconnected");
  Result.Interrupts.clear();
  Report = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(Report));
  EXPECT_EQ(Report->getAsObject()->getBoolean("scenario_success"), false);
  Result.Configuration.Requests[0].InterruptEvents.clear();
  Report = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(Report));
  EXPECT_EQ(Report->getAsObject()->getBoolean("scenario_success"), true);
}

} // namespace
} // namespace neverd::emulation
