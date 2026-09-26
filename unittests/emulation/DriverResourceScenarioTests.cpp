//===- DriverResourceScenarioTests.cpp - Explicit MMIO scenario facts ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Register-bank schema and native preflight without a guest or host device.
///
//===----------------------------------------------------------------------===//

#include "DriverScenario.h"
#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {

constexpr char ResourceJSON[] = R"({"id":"regs-0","raw_start":"0x1230",
  "translated_start":"0x80002000","length":64,"registers":[
    {"offset":8,"width":4,"access":"read_only","value":"0x10203040"},
    {"offset":0,"width":1,"access":"read_write","value":"0xff"},
    {"offset":2,"width":2,"access":"read_write","value":"0xffff"},
    {"offset":4,"width":4,"access":"read_write","value":"0xffffffff"}]})";

std::string scenario(llvm::StringRef Resources = ResourceJSON) {
  return R"({"pnp_devices":[{"id":"unit-0","bus":"register_bank",
    "initial_device_power":"D0","initial_system_power":"working",
    "resources":[)" +
         Resources.str() + "]}]}";
}

DriverMemoryResource resource(std::string ID = "regs-0", uint64_t Base = 0) {
  return {std::move(ID),
          Base,
          0x80000000 + Base,
          1024,
          {{0, 1, DriverRegisterAccess::ReadWrite, 0x12},
           {2, 2, DriverRegisterAccess::ReadWrite, 0x3456},
           {4, 4, DriverRegisterAccess::ReadOnly, 0x789abcde}}};
}

DriverPnpDevice device(std::string ID = "unit-0", uint64_t Base = 0) {
  DriverPnpDevice Result;
  Result.ID = std::move(ID);
  Result.Bus = DriverBusKind::RegisterBank;
  Result.InitialDevicePower = DevicePowerState::D0;
  Result.InitialSystemPower = SystemPowerState::Working;
  Result.Resources.push_back(resource("regs-0", Base));
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

TEST(DriverResourceScenario, ParsesExplicitPhysicalFactsAndPreservesOrder) {
  auto Parsed = driverOptionsFromScenarioJSON(scenario());
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_EQ(Parsed->PnpDevices.size(), 1u);
  const auto &Device = Parsed->PnpDevices[0];
  EXPECT_EQ(Device.Bus, DriverBusKind::RegisterBank);
  ASSERT_EQ(Device.Resources.size(), 1u);
  const auto &Resource = Device.Resources[0];
  EXPECT_EQ(Resource.ID, "regs-0");
  EXPECT_EQ(Resource.RawStart, 0x1230u);
  EXPECT_EQ(Resource.TranslatedStart, 0x80002000u);
  EXPECT_EQ(Resource.Length, 64u);
  ASSERT_EQ(Resource.Registers.size(), 4u);
  EXPECT_EQ(Resource.Registers[0].Offset, 8u);
  EXPECT_EQ(Resource.Registers[0].Access, DriverRegisterAccess::ReadOnly);
  EXPECT_EQ(Resource.Registers[1].Offset, 0u);
  EXPECT_EQ(Resource.Registers[1].Width, 1u);
  EXPECT_EQ(Resource.Registers[1].Value, 0xffu);
  EXPECT_EQ(Resource.Registers[2].Value, 0xffffu);
  EXPECT_EQ(Resource.Registers[3].Value, UINT32_MAX);
  validNative(*Parsed);
}

TEST(DriverResourceScenario, RequiresEveryResourceAndRegisterFactExplicitly) {
  for (const char *Field :
       {"id", "raw_start", "translated_start", "length", "registers"}) {
    auto Value = llvm::json::parse(ResourceJSON);
    ASSERT_TRUE(bool(Value));
    Value->getAsObject()->erase(Field);
    invalidJSON(scenario(llvm::formatv("{0}", *Value).str()), "explicit");
  }
  for (const char *Field : {"offset", "width", "access", "value"}) {
    auto Value = llvm::json::parse(ResourceJSON);
    ASSERT_TRUE(bool(Value));
    (*Value->getAsObject()->getArray("registers"))[0].getAsObject()->erase(
        Field);
    invalidJSON(scenario(llvm::formatv("{0}", *Value).str()), "explicit");
  }
  invalidJSON(scenario("null"));
  invalidJSON(scenario("[]"));
}

TEST(DriverResourceScenario, RejectsUnknownDuplicateAndWronglyTypedFacts) {
  for (const char *Field : {"raw_start", "translated_start", "length"}) {
    for (const char *Bad : {"-1", "true", "null", "[]", "{}", "1.5", "\"42\"",
                            "\"0x10000000000000000\""}) {
      auto Value = llvm::json::parse(ResourceJSON);
      auto Replacement = llvm::json::parse(Bad);
      ASSERT_TRUE(bool(Value));
      ASSERT_TRUE(bool(Replacement));
      (*Value->getAsObject())[Field] = std::move(*Replacement);
      invalidJSON(scenario(llvm::formatv("{0}", *Value).str()));
    }
  }
  for (const char *Field : {"offset", "width", "value"}) {
    auto Value = llvm::json::parse(ResourceJSON);
    ASSERT_TRUE(bool(Value));
    (*(*Value->getAsObject()->getArray("registers"))[0].getAsObject())[Field] =
        "0x100000000";
    invalidJSON(scenario(llvm::formatv("{0}", *Value).str()), "32 bits");
  }
  invalidJSON(scenario(R"({"id":"r","raw_start":0,"translated_start":0,
    "length":4,"registers":[],"unknown":0})"),
              "unknown");
  invalidJSON(scenario(R"({"id":"r","raw_start":0,"translated_start":0,
    "length":4,"registers":[{"offset":0,"width":4,"access":"read_write",
    "value":0,"side_effect":"clear"}]})"),
              "unknown");
  invalidJSON(scenario(R"({"id":"r","raw_start":0,"translated_start":0,
    "length":4,"registers":[{"offset":0,"width":4,"access":"read_write",
    "value":0,"\u0076alue":1}]})"),
              "duplicate");
}

TEST(DriverResourceScenario, BusInventoriesAreExplicitAndLegacyRemainsEmpty) {
  DriverOptions Options;
  Options.PnpDevices.push_back(device());
  Options.PnpDevices[0].Resources.clear();
  invalidNative(Options, "require resources");
  invalidJSON(scenario(""), "require resources");
  auto Value = llvm::json::parse(scenario());
  ASSERT_TRUE(bool(Value));
  (*Value->getAsObject()->getArray("pnp_devices"))[0].getAsObject()->erase(
      "resources");
  invalidJSON(llvm::formatv("{0}", *Value).str(), "explicit resources");
  Options.PnpDevices[0].Bus = DriverBusKind::ResourceFree;
  validNative(Options);
  Options.PnpDevices[0].Resources.push_back(resource());
  invalidNative(Options, "resource_free");
  Options.PnpDevices[0].Bus = static_cast<DriverBusKind>(255);
  invalidNative(Options, "unsupported PnP bus");
}

TEST(DriverResourceScenario, IDsAreCaseSensitiveAndLocalToEachPdo) {
  DriverOptions Options;
  Options.PnpDevices = {device(), device("unit-1", 4096)};
  validNative(Options);
  auto &Resources = Options.PnpDevices[0].Resources;
  Resources.push_back(resource("REGS-0", 2048));
  validNative(Options);
  Resources[1].ID = Resources[0].ID;
  invalidNative(Options, "duplicate resource id");
  Resources.pop_back();
  for (const char *ID :
       {"", ".hidden", "_under", "with space", "/path", "\xc3\xa9"}) {
    Resources[0].ID = ID;
    invalidNative(Options, "ASCII identifier");
  }
  Resources[0].ID.assign(DriverScenarioResourceIDLimit, 'r');
  validNative(Options);
  Resources[0].ID.push_back('r');
  invalidNative(Options, "ASCII identifier");
}

TEST(DriverResourceScenario, RejectsInvalidRegisterExtentsWidthsAndValues) {
  DriverOptions Options;
  Options.PnpDevices.push_back(device());
  auto &Resource = Options.PnpDevices[0].Resources[0];
  Resource.Registers = {{0, 4, DriverRegisterAccess::ReadWrite, UINT32_MAX}};
  validNative(Options);
  for (uint8_t Width : {0, 3, 8, 255}) {
    Resource.Registers[0].Width = Width;
    invalidNative(Options, "width");
  }
  Resource.Registers[0].Width = 4;
  Resource.Registers[0].Access = static_cast<DriverRegisterAccess>(255);
  invalidNative(Options, "register access");
  Resource.Registers[0].Access = DriverRegisterAccess::ReadWrite;
  for (uint32_t Offset : {1022u, 1024u, UINT32_MAX}) {
    Resource.Registers[0].Offset = Offset;
    invalidNative(Options, "beyond");
  }
  Resource.Registers[0].Offset = 2;
  invalidNative(Options, "naturally aligned");
  Resource.Registers[0].Offset = 0;
  ++Resource.TranslatedStart;
  invalidNative(Options, "naturally aligned");
  --Resource.TranslatedStart;
  for (uint8_t Width : {1, 2}) {
    Resource.Registers[0].Width = Width;
    Resource.Registers[0].Value = uint32_t{1} << (Width * 8);
    invalidNative(Options, "value exceeds");
    --Resource.Registers[0].Value;
    validNative(Options);
  }
  Resource.Registers = {{4, 4, DriverRegisterAccess::ReadWrite, 0},
                        {6, 2, DriverRegisterAccess::ReadOnly, 0}};
  invalidNative(Options, "register intervals overlap");
}

TEST(DriverResourceScenario, HalfOpenIntervalsHaveExactPdoAndGlobalOwnership) {
  DriverOptions Options;
  Options.PnpDevices = {device(), device("unit-1", 2048)};
  auto &First = Options.PnpDevices[0].Resources;
  First.push_back(resource("regs-1", 1024));
  validNative(Options);
  // Raw addresses belong to the device's bus; translated addresses are global.
  Options.PnpDevices[1].Resources[0].RawStart = 0;
  validNative(Options);
  --First[1].RawStart;
  invalidNative(Options, "raw resource intervals overlap");
  ++First[1].RawStart;
  First[1].TranslatedStart = First[0].TranslatedStart;
  invalidNative(Options, "translated resource intervals overlap");
  First[1].TranslatedStart = 0x80000400;
  Options.PnpDevices[1].Resources[0].TranslatedStart = 0x80000400;
  invalidNative(Options, "translated resource intervals overlap");
}

TEST(DriverResourceScenario,
     PhysicalBoundsPreserveZeroHighAndUnalignedAddresses) {
  DriverOptions Options;
  Options.PnpDevices.push_back(device());
  auto &Resource = Options.PnpDevices[0].Resources[0];
  Resource.RawStart = 0;
  Resource.TranslatedStart = 0;
  Resource.Registers.clear();
  validNative(Options);
  Resource.Length = 1;
  Resource.RawStart = UINT64_MAX - 1;
  Resource.TranslatedStart = UINT64_MAX - 1;
  validNative(Options);
  ++Resource.RawStart;
  invalidNative(Options, "overflows");
  --Resource.RawStart;
  ++Resource.TranslatedStart;
  invalidNative(Options, "overflows");
  Resource.RawStart = 3;
  Resource.TranslatedStart = 5;
  Resource.Registers = {{0, 1, DriverRegisterAccess::ReadOnly, 0x12}};
  validNative(Options);
  Resource.Length = 0;
  invalidNative(Options, "length");
  Resource.Length = DriverScenarioResourceLengthLimit;
  validNative(Options);
  ++Resource.Length;
  invalidNative(Options, "length");
  auto Parsed = driverOptionsFromScenarioJSON(scenario(R"({"id":"high",
    "raw_start":"0xfffffffffffffffe","translated_start":18446744073709551614,
    "length":"0x1","registers":[]})"));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->PnpDevices[0].Resources[0].RawStart, UINT64_MAX - 1);
  EXPECT_EQ(Parsed->PnpDevices[0].Resources[0].TranslatedStart, UINT64_MAX - 1);
}

TEST(DriverResourceScenario, BoundsResourcesPerDeviceAndAcrossDevices) {
  DriverOptions Options;
  for (size_t D = 0;
       D < DriverScenarioResourceLimit / DriverScenarioResourcesPerDeviceLimit;
       ++D) {
    auto Device = device("unit-" + std::to_string(D));
    Device.Resources.clear();
    for (size_t R = 0; R < DriverScenarioResourcesPerDeviceLimit; ++R)
      Device.Resources.push_back(
          resource("r-" + std::to_string(R),
                   (D * DriverScenarioResourcesPerDeviceLimit + R) * 4096));
    Options.PnpDevices.push_back(std::move(Device));
  }
  validNative(Options);
  Options.PnpDevices[0].Resources.push_back(resource("extra", 0x100000));
  invalidNative(Options, "per-device count");
  Options.PnpDevices[0].Resources.pop_back();
  Options.PnpDevices.push_back(device("extra", 0x100000));
  invalidNative(Options, "combined count");
}

TEST(DriverResourceScenario, BoundsRegistersPerResourceAndAcrossDevices) {
  DriverOptions Options;
  auto Device = device();
  Device.Resources.clear();
  for (size_t I = 0; I < DriverScenarioRegisterLimit /
                             DriverScenarioRegistersPerResourceLimit;
       ++I) {
    if (Device.Resources.size() == DriverScenarioResourcesPerDeviceLimit) {
      Options.PnpDevices.push_back(std::move(Device));
      Device = device("unit-" + std::to_string(I));
      Device.Resources.clear();
    }
    auto Resource = resource("r-" + std::to_string(I), I * 4096);
    Resource.Registers.clear();
    for (size_t R = 0; R < DriverScenarioRegistersPerResourceLimit; ++R)
      Resource.Registers.push_back({static_cast<uint32_t>(R * 4), 4,
                                    DriverRegisterAccess::ReadWrite, 0});
    Device.Resources.push_back(std::move(Resource));
  }
  Options.PnpDevices.push_back(std::move(Device));
  validNative(Options);
  auto &Registers = Options.PnpDevices[0].Resources[0].Registers;
  Registers.push_back({0, 1, DriverRegisterAccess::ReadOnly, 0});
  invalidNative(Options, "per-resource count");
  Registers.pop_back();
  Options.PnpDevices.push_back(device("extra", 0x100000));
  invalidNative(Options, "combined count");
}

TEST(DriverResourceScenario,
     ReportsConfiguredFactsWithoutCreatingObservedState) {
  auto Options = driverOptionsFromScenarioJSON(scenario());
  ASSERT_TRUE(bool(Options)) << llvm::toString(Options.takeError());
  Options->PnpDevices[0].Resources[0].RawStart = UINT64_MAX - 64;
  DriverResult Result;
  Result.Configuration = *Options;
  DriverPnpDevice Legacy = device("legacy");
  Legacy.Bus = DriverBusKind::ResourceFree;
  Legacy.Resources.clear();
  Result.Configuration.PnpDevices.push_back(std::move(Legacy));
  auto Report = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  const auto *Configured = Report->getAsObject()
                               ->getObject("configuration")
                               ->getArray("pnp_devices");
  ASSERT_NE(Configured, nullptr);
  ASSERT_EQ(Configured->size(), 2u);
  const auto *Bank = (*Configured)[0].getAsObject();
  ASSERT_NE(Bank, nullptr);
  EXPECT_EQ(Bank->getString("bus"), "register_bank");
  const auto *Resources = Bank->getArray("resources");
  ASSERT_NE(Resources, nullptr);
  ASSERT_EQ(Resources->size(), 1u);
  const auto *Resource = (*Resources)[0].getAsObject();
  EXPECT_EQ(Resource->getString("raw_start"), "0xffffffffffffffbf");
  EXPECT_EQ(Resource->getString("translated_start"), "0x80002000");
  EXPECT_EQ(Resource->getInteger("length"), 64);
  const auto *Registers = Resource->getArray("registers");
  ASSERT_NE(Registers, nullptr);
  ASSERT_EQ(Registers->size(), 4u);
  EXPECT_EQ((*Registers)[0].getAsObject()->getInteger("offset"), 8);
  EXPECT_EQ((*Registers)[0].getAsObject()->getString("access"), "read_only");
  EXPECT_EQ((*Registers)[3].getAsObject()->getInteger("value"), UINT32_MAX);
  EXPECT_FALSE((*Configured)[1].getAsObject()->get("resources"));
  EXPECT_TRUE(Report->getAsObject()->getArray("pnp_devices")->empty());
}

} // namespace
} // namespace neverd::emulation
