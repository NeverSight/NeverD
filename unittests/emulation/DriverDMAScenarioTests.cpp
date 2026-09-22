//===- DriverDMAScenarioTests.cpp - DMA input and observation contracts ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit DMA domains, external transfer preflight and independent reports.
///
//===----------------------------------------------------------------------===//

#include "DriverScenario.h"
#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {

constexpr char ConfigJSON[] =
    R"({"address_bits":64,"maximum_length":1048576,"map_registers":256,
        "alignment":1,"logical_base":"0xfedcba9876540000",
        "logical_length":4096,"scatter_gather":true})";
constexpr char EventJSON[] =
    R"({"after_100ns":7,"device_id":"dma-0","logical_address":"0x123456789abcdef0",
        "direction":"write_memory","length":4,"data_hex":"01aB0080"})";

std::string scenario(llvm::StringRef Config = ConfigJSON,
                     llvm::StringRef Event = EventJSON) {
  return R"({"pnp_devices":[{"id":"dma-0","bus":"register_bank",
    "initial_device_power":"D0","initial_system_power":"working",
    "resources":[{"id":"bank","raw_start":0,"translated_start":4096,
                  "length":4096,"registers":[]}],"dma":)" +
         Config.str() +
         R"(}],"requests":[{"kind":"ioctl","code":"0x222000","dma_events":[)" +
         Event.str() + "]}]}";
}

DriverOptions options() {
  DriverOptions Result;
  DriverPnpDevice Device;
  Device.ID = "dma-0";
  Device.Bus = DriverBusKind::RegisterBank;
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  Device.Resources.push_back({"bank", 0, 4096, 4096, {}});
  Device.Dma =
      DriverDmaConfig{64, 1048576, 256, 1, 0xfedcba9876540000, 4096, true};
  Result.PnpDevices.push_back(std::move(Device));
  DriverRequest Request;
  Request.ControlCode = 0x222000;
  Request.DmaEvents.push_back({7,
                               "dma-0",
                               0x123456789abcdef0,
                               DriverDmaDirection::WriteMemory,
                               4,
                               {1, 0xab, 0, 0x80}});
  Result.Requests.push_back(std::move(Request));
  return Result;
}

std::string changed(llvm::StringRef JSON, llvm::StringRef Field,
                    llvm::StringRef Replacement) {
  auto Value = llvm::cantFail(llvm::json::parse(JSON));
  // Serializing a parsed 1.0 can emit 1. Preserve the original numeric token
  // so the parser tests actually exercise floating-point input rejection.
  (*Value.getAsObject())[Field] = "neverd-test-literal";
  std::string Text = llvm::formatv("{0}", Value).str();
  constexpr llvm::StringLiteral Marker = "\"neverd-test-literal\"";
  Text.replace(Text.find(Marker.str()), Marker.size(), Replacement.str());
  return Text;
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
  auto Syntax = llvm::json::parse(JSON);
  ASSERT_TRUE(bool(Syntax)) << llvm::toString(Syntax.takeError());
  auto Parsed = driverOptionsFromScenarioJSON(JSON);
  ASSERT_FALSE(bool(Parsed));
  EXPECT_NE(llvm::toString(Parsed.takeError()).find(Text.str()),
            std::string::npos)
      << JSON.str();
}

TEST(DriverDMAScenario,
     PreservesExactAddressesAndIndependentDeviceTransactions) {
  auto Parsed = driverOptionsFromScenarioJSON(scenario());
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_TRUE(Parsed->PnpDevices[0].Dma);
  const auto &Dma = *Parsed->PnpDevices[0].Dma;
  EXPECT_EQ(Dma.LogicalBase, 0xfedcba9876540000u);
  EXPECT_EQ(Dma.LogicalLength, 4096u);
  EXPECT_TRUE(Dma.ScatterGather);
  const auto &Event = Parsed->Requests[0].DmaEvents[0];
  EXPECT_EQ(Event.LogicalAddress, 0x123456789abcdef0u);
  EXPECT_EQ(Event.Direction, DriverDmaDirection::WriteMemory);
  EXPECT_EQ(Event.Data, (std::vector<uint8_t>{1, 0xab, 0, 0x80}));
  // Configuration validates the declaration, not whether this future address
  // will belong to a live DMA mapping when its external transaction is due.
  EXPECT_LT(Event.LogicalAddress, Dma.LogicalBase);
  validNative(*Parsed);
  auto Decimal = driverOptionsFromScenarioJSON(
      scenario(changed(ConfigJSON, "logical_base", "18364758544493051904")));
  ASSERT_TRUE(bool(Decimal)) << llvm::toString(Decimal.takeError());
  EXPECT_EQ(Decimal->PnpDevices[0].Dma->LogicalBase, Dma.LogicalBase);
}

TEST(DriverDMAScenario, RequiresCompleteExplicitConfigurationAndEventFacts) {
  for (const char *Field :
       {"address_bits", "maximum_length", "map_registers", "alignment",
        "logical_base", "logical_length", "scatter_gather"}) {
    auto Value = llvm::cantFail(llvm::json::parse(ConfigJSON));
    Value.getAsObject()->erase(Field);
    invalidJSON(scenario(llvm::formatv("{0}", Value).str()), "explicit");
  }
  for (const char *Field : {"after_100ns", "device_id", "logical_address",
                            "direction", "length", "data_hex"}) {
    auto Value = llvm::cantFail(llvm::json::parse(EventJSON));
    Value.getAsObject()->erase(Field);
    invalidJSON(scenario(ConfigJSON, llvm::formatv("{0}", Value).str()));
  }
  for (const char *Bad : {"null", "[]", "true", "42"}) {
    invalidJSON(scenario(Bad));
    invalidJSON(scenario(ConfigJSON, Bad));
  }
  invalidJSON(scenario(changed(ConfigJSON, "coherent", "true")), "unknown");
  invalidJSON(scenario(ConfigJSON, changed(EventJSON, "interrupt", "true")),
              "unknown");
  invalidJSON(scenario(R"({"address_bits":32,"\u0061ddress_bits":64})"),
              "duplicate");
  invalidJSON(scenario(ConfigJSON, R"({"length":4,"\u006cength":8})"),
              "duplicate");
}

TEST(DriverDMAScenario, RejectsCoercionsAndLossyIntegerInputs) {
  for (const char *Field : {"address_bits", "maximum_length", "map_registers",
                            "alignment", "logical_base", "logical_length"})
    for (const char *Bad : {"-1", "true", "null", "[]", "{}", "1.0", "1.5",
                            "\"4096\"", "\"0x10000000000000000\""})
      invalidJSON(scenario(changed(ConfigJSON, Field, Bad)));
  for (const char *Bad : {"0", "1", "null", "[]", "\"true\""})
    invalidJSON(scenario(changed(ConfigJSON, "scatter_gather", Bad)));
  for (const char *Field : {"after_100ns", "logical_address", "length"})
    for (const char *Bad : {"-1", "false", "null", "{}", "1.0", "1.25", "\"7\"",
                            "\"0x10000000000000000\""})
      invalidJSON(scenario(ConfigJSON, changed(EventJSON, Field, Bad)));
  auto Parsed = driverOptionsFromScenarioJSON(
      scenario(changed(ConfigJSON, "scatter_gather", "false")));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_FALSE(Parsed->PnpDevices[0].Dma->ScatterGather);
}

TEST(DriverDMAScenario, ChecksCapabilitiesAndLogicalApertureBoundaries) {
  auto Options = options();
  auto &Dma = *Options.PnpDevices[0].Dma;
  for (uint32_t Value : {0u, 31u, 33u, 48u, 65u}) {
    Dma.AddressBits = Value;
    invalidNative(Options, "address_bits");
  }
  Dma.AddressBits = 32;
  Dma.LogicalBase = 0xfffff000;
  validNative(Options); // The last byte may be the maximum 32-bit address.
  Dma.LogicalLength = 8192;
  invalidNative(Options, "address_bits");
  Dma.AddressBits = 64;
  Dma.LogicalBase = UINT64_MAX - 4095;
  Dma.LogicalLength = 4096;
  invalidNative(Options, "overflows");
  Dma.LogicalBase = 4096;
  for (uint64_t Value :
       {uint64_t(0), uint64_t(1), uint64_t(4095), uint64_t(1073741825)}) {
    Dma.LogicalLength = Value;
    invalidNative(Options, "logical_length");
  }
  Dma.LogicalLength = 1073741824;
  validNative(Options);
  Dma.LogicalLength = 4096;
  for (uint64_t Value : {uint64_t(0), uint64_t(1), uint64_t(4095)}) {
    Dma.LogicalBase = Value;
    invalidNative(Options, "logical_base");
  }
  Dma.LogicalBase = 4096;
  for (uint32_t Value : {0u, 3u, 8192u}) {
    Dma.Alignment = Value;
    invalidNative(Options, "alignment");
  }
  Dma.Alignment = 4096;
  validNative(Options);
  for (uint32_t Value : {0u, 1048577u}) {
    Dma.MaximumLength = Value;
    invalidNative(Options, "maximum_length");
  }
  Dma.MaximumLength = 1;
  for (uint32_t Value : {0u, 257u}) {
    Dma.MapRegisters = Value;
    invalidNative(Options, "map_registers");
  }
  Dma.MapRegisters = 1;
  validNative(Options);
  invalidJSON(scenario(changed(ConfigJSON, "map_registers", "257")),
              "map_registers");
  invalidJSON(scenario(changed(ConfigJSON, "logical_length", "4097")),
              "logical_length");
}

TEST(DriverDMAScenario, DomainsMayOverlapButReservedPhysicalRamNeverDoes) {
  auto Options = options();
  auto Other = Options.PnpDevices[0];
  Other.ID = "dma-1";
  Other.Resources[0].TranslatedStart = 8192;
  Options.PnpDevices.push_back(Other);
  validNative(Options); // Same logical aperture, separate PDO translation.
  Options.PnpDevices[1].Dma.reset();
  auto &Resource = Options.PnpDevices[1].Resources[0];
  Resource.TranslatedStart = DriverDmaPhysicalBase;
  invalidNative(Options, "reserved DMA physical RAM");
  Resource.TranslatedStart = DriverDmaPhysicalBase - Resource.Length + 1;
  invalidNative(Options, "reserved DMA physical RAM");
  Resource.TranslatedStart = DriverDmaPhysicalBase + DriverDmaPhysicalSize - 1;
  invalidNative(Options, "reserved DMA physical RAM");
  Resource.TranslatedStart = DriverDmaPhysicalBase - Resource.Length;
  validNative(Options);
  Resource.TranslatedStart = DriverDmaPhysicalBase + DriverDmaPhysicalSize;
  validNative(Options);
  Resource.TranslatedStart = DriverDmaPhysicalBase;
  Options.PnpDevices[0].Dma.reset();
  Options.Requests.clear();
  invalidNative(Options, "reserved DMA physical RAM");
}

TEST(DriverDMAScenario,
     MdlPhysicalReservationAlsoAppliesWithoutDmaConfiguration) {
  auto Options = options();
  Options.PnpDevices[0].Dma.reset();
  Options.Requests.clear();
  auto &Resource = Options.PnpDevices[0].Resources[0];
  Resource.RawStart = DriverDmaPhysicalBase;
  validNative(Options); // Only translated physical memory competes with RAM.
  Resource.TranslatedStart = DriverDmaPhysicalBase;
  invalidNative(Options, "reserved DMA physical RAM");
  Resource.TranslatedStart = DriverDmaPhysicalBase + DriverDmaPhysicalSize;
  validNative(Options);

  auto Input = llvm::cantFail(llvm::json::parse(scenario()));
  Input.getAsObject()->erase("requests");
  auto *Device =
      (*Input.getAsObject()->getArray("pnp_devices"))[0].getAsObject();
  Device->erase("dma");
  auto *Memory = (*Device->getArray("resources"))[0].getAsObject();
  (*Memory)["translated_start"] = DriverDmaPhysicalBase;
  invalidJSON(llvm::formatv("{0}", Input).str(), "reserved DMA physical RAM");
  (*Memory)["translated_start"] = DriverDmaPhysicalBase - 4096;
  auto Parsed =
      driverOptionsFromScenarioJSON(llvm::formatv("{0}", Input).str());
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  validNative(*Parsed);
}

TEST(DriverDMAScenario, DmaCannotReplaceHardwareResourcesOrUseResourceFreeBus) {
  auto Options = options();
  Options.PnpDevices[0].Resources.clear();
  invalidNative(Options, "resources or interrupts");
  Options.PnpDevices[0].Interrupts.push_back({"irq", 1, 1, 1, 81, 3, 1});
  validNative(Options);
  Options.PnpDevices[0].Interrupts.clear();
  Options.PnpDevices[0].Bus = DriverBusKind::ResourceFree;
  invalidNative(Options, "register_bank");
  for (const char *Bad : {"{}", "null", "[]"})
    invalidJSON(
        std::string(R"({"pnp_devices":[{"id":"dma-0","bus":"resource_free",
      "initial_device_power":"D0","initial_system_power":"working","dma":)") +
            Bad + "}]}",
        "cannot specify dma");
}

TEST(DriverDMAScenario, EnforcesDevicePerspectiveAndExactWritePayload) {
  for (const char *Bad : {"null", "[]", "4", "\"\"", "\"010203\"",
                          "\"0102030405\"", "\"0x010203\"", "\"01020z80\""})
    invalidJSON(scenario(ConfigJSON, changed(EventJSON, "data_hex", Bad)),
                "data_hex");
  for (const char *Data : {"null", "[]", "\"\"", "\"01020304\""})
    invalidJSON(scenario(ConfigJSON, changed(changed(EventJSON, "direction",
                                                     "\"read_memory\""),
                                             "data_hex", Data)),
                "cannot specify data_hex");
  auto Options = options();
  auto &Event = Options.Requests[0].DmaEvents[0];
  Event.Direction = DriverDmaDirection::ReadMemory;
  invalidNative(Options, "cannot contain data");
  Event.Data.clear();
  validNative(Options);
  Event.Direction = DriverDmaDirection::WriteMemory;
  invalidNative(Options, "exactly length");
  Event.Direction = static_cast<DriverDmaDirection>(2);
  invalidNative(Options, "unsupported DMA direction");
  invalidJSON(scenario(ConfigJSON, changed(EventJSON, "direction", "\"read\"")),
              "unsupported DMA direction");
}

TEST(DriverDMAScenario,
     EventTargetAndKindAreExplicitWithoutGuessingRequestOwner) {
  auto Options = options();
  auto Other = Options.PnpDevices[0];
  Other.ID = "other";
  Other.Dma.reset();
  Other.Resources[0].TranslatedStart = 8192;
  Options.PnpDevices.push_back(Other);
  auto &Request = Options.Requests[0];
  Request.DeviceID = "other";
  validNative(Options); // Transfer target is independent of the source IRP.
  Request.DmaEvents[0].DeviceID = "other";
  invalidNative(Options, "configured DMA device");
  Request.DmaEvents[0].DeviceID = "missing";
  invalidNative(Options, "configured DMA device");
  Request.DmaEvents[0].DeviceID = "bad id";
  invalidNative(Options, "ASCII identifier");
  Request.DmaEvents[0].DeviceID = "dma-0";
  for (auto Kind : {DriverRequestKind::Read, DriverRequestKind::Write,
                    DriverRequestKind::DeviceControl}) {
    Request.Kind = Kind;
    Request.ControlCode = 0;
    validNative(Options);
  }
  for (auto Kind : {DriverRequestKind::Create, DriverRequestKind::Cleanup,
                    DriverRequestKind::Close, DriverRequestKind::Pnp,
                    DriverRequestKind::Power}) {
    Request.Kind = Kind;
    invalidNative(Options, "only for read, write and ioctl");
  }
  for (const char *Kind : {"create", "cleanup", "close", "pnp", "power"})
    invalidJSON(std::string(R"({"requests":[{"kind":")") + Kind +
                    R"(","dma_events":[]}]})",
                "only for read, write and ioctl");
}

TEST(DriverDMAScenario, BoundsTimeAddressLengthAndTotalScheduledBytes) {
  auto Options = options();
  auto &Event = Options.Requests[0].DmaEvents[0];
  Event.Direction = DriverDmaDirection::ReadMemory;
  Event.Data.clear();
  Event.After100ns = INT64_MAX;
  Event.LogicalAddress = UINT64_MAX - 1;
  Event.Length = 1;
  validNative(Options); // Aperture accessibility is an observed runtime fact.
  ++Event.After100ns;
  invalidNative(Options, "signed 64-bit");
  Event.After100ns = 0;
  Event.LogicalAddress = UINT64_MAX;
  invalidNative(Options, "overflows");
  Event.LogicalAddress = 0;
  for (uint32_t Length : {0u, 1048577u}) {
    Event.Length = Length;
    invalidNative(Options, "length");
  }
  Event.Length = 1048576;
  auto Copy = Event;
  Options.Requests[0].DmaEvents.assign(16, Copy);
  validNative(Options);
  Copy.Length = 1;
  Options.Requests[0].DmaEvents.push_back(Copy);
  invalidNative(Options, "combined byte");
  invalidJSON(scenario(ConfigJSON, changed(EventJSON, "after_100ns",
                                           "9223372036854775808")),
              "signed 64-bit");
}

TEST(DriverDMAScenario, BoundsPerRequestAndCombinedEventCounts) {
  auto Options = options();
  DriverDmaEvent Event{0, "dma-0", 0, DriverDmaDirection::ReadMemory, 1, {}};
  Options.Requests[0].DmaEvents.assign(64, Event);
  validNative(Options);
  Options.Requests[0].DmaEvents.push_back(Event);
  invalidNative(Options, "per-request count");
  Options.Requests[0].DmaEvents.pop_back();
  const auto Request = Options.Requests[0];
  Options.Requests.assign(16, Request);
  validNative(Options);
  Options.Requests.push_back(Request);
  invalidNative(Options, "combined count");
  std::string Events = EventJSON;
  for (unsigned I = 1; I < 65; ++I)
    Events += std::string(",") + EventJSON;
  invalidJSON(scenario(ConfigJSON, Events), "bounded array");
  auto JSON = llvm::cantFail(llvm::json::parse(scenario()));
  for (const char *Bad : {"null", "{}", "true"}) {
    (*(*JSON.getAsObject()->getArray("requests"))[0]
          .getAsObject())["dma_events"] =
        llvm::cantFail(llvm::json::parse(Bad));
    invalidJSON(llvm::formatv("{0}", JSON).str(), "bounded array");
  }
}

TEST(DriverDMAScenario, JsonSharesAggregateBudgetsAndPhysicalReservation) {
  auto Input = llvm::cantFail(llvm::json::parse(scenario()));
  auto *Requests = Input.getAsObject()->getArray("requests");
  auto *Events = (*Requests)[0].getAsObject()->getArray("dma_events");
  auto Event = (*Events)[0];
  (*Event.getAsObject())["direction"] = "read_memory";
  Event.getAsObject()->erase("data_hex");
  (*Event.getAsObject())["length"] = 1048576;
  Events->clear();
  for (unsigned I = 0; I < 16; ++I)
    Events->push_back(Event);
  auto Parsed =
      driverOptionsFromScenarioJSON(llvm::formatv("{0}", Input).str());
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  validNative(*Parsed);
  (*Event.getAsObject())["length"] = 1;
  Events->push_back(Event);
  invalidJSON(llvm::formatv("{0}", Input).str(), "combined byte");
  Events->clear();
  for (unsigned I = 0; I < 64; ++I)
    Events->push_back(Event);
  auto Request = (*Requests)[0];
  for (unsigned I = 1; I < 16; ++I)
    Requests->push_back(Request);
  Parsed = driverOptionsFromScenarioJSON(llvm::formatv("{0}", Input).str());
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  validNative(*Parsed);
  Requests->push_back(Request);
  invalidJSON(llvm::formatv("{0}", Input).str(), "combined count");

  Input = llvm::cantFail(llvm::json::parse(scenario()));
  auto *Resource = (*(*Input.getAsObject()->getArray("pnp_devices"))[0]
                         .getAsObject()
                         ->getArray("resources"))[0]
                       .getAsObject();
  (*Resource)["translated_start"] = DriverDmaPhysicalBase;
  invalidJSON(llvm::formatv("{0}", Input).str(), "reserved DMA physical RAM");
  (*Resource)["translated_start"] =
      DriverDmaPhysicalBase + DriverDmaPhysicalSize;
  Parsed = driverOptionsFromScenarioJSON(llvm::formatv("{0}", Input).str());
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  validNative(*Parsed);
}

TEST(DriverDMAScenario, OmittedBaseFactsPersistAndReplacementsAreRevalidated) {
  auto Base = options();
  auto Parsed = driverOptionsFromScenarioJSON("{}", Base);
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->PnpDevices[0].Dma->LogicalBase,
            Base.PnpDevices[0].Dma->LogicalBase);
  EXPECT_EQ(Parsed->Requests[0].DmaEvents[0].Data,
            Base.Requests[0].DmaEvents[0].Data);
  Parsed = driverOptionsFromScenarioJSON(R"({"pnp_devices":[]})", Base);
  ASSERT_FALSE(bool(Parsed));
  EXPECT_NE(llvm::toString(Parsed.takeError()).find("configured DMA device"),
            std::string::npos);
  Base.PnpDevices[0].Dma->Alignment = 3;
  Parsed = driverOptionsFromScenarioJSON("{}", Base);
  ASSERT_FALSE(bool(Parsed));
  EXPECT_NE(llvm::toString(Parsed.takeError()).find("alignment"),
            std::string::npos);
}

TEST(DriverDMAScenario, ReportsDeclarationsAndObservedBytesSeparately) {
  DriverResult Result;
  Result.Configuration = options();
  auto Report = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
  const auto *Root = Report.getAsObject();
  EXPECT_TRUE(Root->getArray("dma_transfers")->empty());
  const auto *Config = Root->getObject("configuration");
  const auto *Dma =
      (*Config->getArray("pnp_devices"))[0].getAsObject()->getObject("dma");
  ASSERT_NE(Dma, nullptr);
  EXPECT_EQ(Dma->getString("logical_base"), "0xFEDCBA9876540000");
  EXPECT_EQ(Dma->getBoolean("scatter_gather"), true);
  const auto *Event = (*Config->getArray("dma_events"))[0].getAsObject();
  EXPECT_EQ(Event->getInteger("source_request_index"), 0);
  EXPECT_EQ(Event->getInteger("event_index"), 0);
  EXPECT_EQ(Event->getString("data_hex"), "01ab0080");
  DriverDmaResult Transfer;
  Transfer.DeviceID = "dma-0";
  Transfer.Epoch = 3;
  Transfer.DueAt100ns = 12;
  Transfer.LogicalAddress = 0x123456789abcdef0;
  Transfer.Length = 4;
  Result.DmaTransfers.push_back(Transfer);
  Report = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
  const auto *Observed =
      (*Report.getAsObject()->getArray("dma_transfers"))[0].getAsObject();
  EXPECT_EQ(Observed->getString("logical_address"), "0x123456789ABCDEF0");
  EXPECT_EQ(Observed->getString("data_hex"), "");
  for (const char *Name : {"occurred_at_100ns", "completed_at_100ns", "mapping",
                           "adapter", "failure_reason"})
    EXPECT_EQ(Observed->get(Name)->kind(), llvm::json::Value::Null);
  Result.DmaTransfers[0].OccurredAt100ns = 12;
  Result.DmaTransfers[0].Mapping = 0x123000;
  Result.DmaTransfers[0].Adapter = 0x456000;
  Result.DmaTransfers[0].FailureReason = "unmapped logical address";
  Report = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
  Observed =
      (*Report.getAsObject()->getArray("dma_transfers"))[0].getAsObject();
  EXPECT_EQ(Observed->getInteger("occurred_at_100ns"), 12);
  EXPECT_EQ(Observed->getString("mapping"), "0x123000");
  EXPECT_EQ(Observed->getString("adapter"), "0x456000");
  EXPECT_EQ(Observed->getString("failure_reason"), "unmapped logical address");
  EXPECT_EQ(Observed->getString("data_hex"), "");
}

TEST(DriverDMAScenario, CompletingSourceIrpCannotSubstituteForDmaCompletion) {
  DriverResult Result;
  Result.Configuration = options();
  Result.Configuration.Requests[0].DmaEvents[0].Direction =
      DriverDmaDirection::ReadMemory;
  Result.Configuration.Requests[0].DmaEvents[0].Data.clear();
  Result.Stop = DriverStopReason::Returned;
  Result.NTStatus = 0;
  DriverPnpDeviceResult Device;
  Device.ID = "dma-0";
  Device.AddDeviceStatus = 0;
  Result.PnpDevices.push_back(Device);
  DriverRequestResult Request;
  Request.Completed = true;
  Request.DispatchStatus = 0;
  Request.IOStatus = 0;
  Result.Requests.push_back(Request);
  auto Succeeded = [&]() {
    auto Report = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
    return Report.getAsObject()->getBoolean("scenario_success").value();
  };
  EXPECT_FALSE(Succeeded());
  DriverDmaResult Transfer;
  Transfer.Length = 4;
  Transfer.OccurredAt100ns = 0;
  Result.DmaTransfers.push_back(Transfer);
  EXPECT_FALSE(Succeeded());
  Result.DmaTransfers[0].CompletedAt100ns = 0;
  EXPECT_FALSE(Succeeded()); // A reported read cannot fabricate omitted bytes.
  Result.DmaTransfers[0].Data = {1, 2, 3, 4};
  EXPECT_TRUE(Succeeded());
  Result.DmaTransfers[0].FailureReason = "device unavailable";
  EXPECT_FALSE(Succeeded());
  Result.DmaTransfers[0].FailureReason.reset();
  Result.DmaTransfers.push_back(Result.DmaTransfers[0]);
  EXPECT_FALSE(Succeeded());
}

} // namespace
} // namespace neverd::emulation
