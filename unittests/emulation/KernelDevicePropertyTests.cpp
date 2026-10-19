//===- KernelDevicePropertyTests.cpp - Owned PDO query observations -------===//
///
/// \file
/// Owned PDO query observations.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {
DriverOptions propertyOptions() {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  DriverPnpDevice D;
  D.ID = "port";
  D.InitialDevicePower = DevicePowerState::D0;
  D.InitialSystemPower = SystemPowerState::Working;
  D.InitialReportedDevicePower = DevicePowerState::D0;
  D.DeviceProperties = std::vector<DriverDeviceProperty>{
      {1, std::vector<uint8_t>{'N', 0, 'D', 0, 0, 0, 0, 0}},
      {2, std::nullopt},
      {14, std::vector<uint8_t>{7, 0, 0, 0}},
      {17, std::vector<uint8_t>{42, 0, 0, 0}}};
  O.PnpDevices.push_back(std::move(D));
  return O;
}
TEST(DriverDevicePropertyScenario,
     ValidatesAndRoundTripsUnknownAndMissingValues) {
  auto O = propertyOptions();
  DriverResult R;
  R.Configuration = O;
  auto JSON = llvm::cantFail(llvm::json::parse(driverResultJSON(R)));
  auto *Devices =
      JSON.getAsObject()->getObject("configuration")->get("pnp_devices");
  auto Copy = driverOptionsFromScenarioJSON(
      "{\"pnp_devices\":" + llvm::formatv("{0}", *Devices).str() + "}");
  ASSERT_TRUE(bool(Copy)) << llvm::toString(Copy.takeError());
  ASSERT_TRUE(Copy->PnpDevices[0].DeviceProperties);
  EXPECT_EQ(Copy->PnpDevices[0].DeviceProperties->at(0).Bytes,
            O.PnpDevices[0].DeviceProperties->at(0).Bytes);
  EXPECT_FALSE(Copy->PnpDevices[0].DeviceProperties->at(1).Bytes);
  for (
      const char *Bad :
      {"null", "true", "{}", R"([{"property":1}])",
       R"([{"property":1,"bytes_hex":"00"}])",
       R"([{"property":1,"bytes_hex":"01000000"},{"property":1,"bytes_hex":null}])",
       R"([{"property":14,"bytes_hex":null}])",
       R"([{"property":14,"bytes_hex":"0000"}])",
       R"([{"property":3,"bytes_hex":"00000000"}])",
       R"([{"property":11,"bytes_hex":"0000"}])",
       R"([{"property":4294967296,"bytes_hex":null}])",
       R"([{"property":1,"bytes_hex":"zz"}])",
       R"([{"property":1,"bytes_hex":null,"extra":1}])"}) {
    auto Parsed = driverOptionsFromScenarioJSON(
        std::string(
            R"({"pnp_devices":[{"id":"port","bus":"resource_free","initial_device_power":"D0","initial_system_power":"working","device_properties":)") +
        Bad + "}]}");
    ASSERT_FALSE(bool(Parsed)) << Bad;
    llvm::consumeError(Parsed.takeError());
  }
  O.PnpDevices[0].DeviceProperties =
      std::vector<DriverDeviceProperty>{{1, std::vector<uint8_t>(65538)}};
  auto E = validateDriverDeviceProperties(O.PnpDevices[0].DeviceProperties);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
class KernelDeviceProperty : public testing::Test {
protected:
  static constexpr uint64_t S = 0x60000000;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  uint64_t PDO = 0;
  void ok(llvm::Error E) {
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  }
  void initialize(DriverOptions O = propertyOptions()) {
    Model.reset();
    Result = {};
    Memory = llvm::cantFail(UnicornBackend::create(4 * 1024 * 1024));
    ok(Memory->map(S, 4096, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 4096;
    Image.Size = 0x3000;
    ok(Model->initialize(Image, O));
    const auto Extension = llvm::cantFail(Memory->readInteger(
        Model->driverObject() + windows::DriverExtensionOffset, 8));
    ok(Memory->writeInteger(Extension + windows::DriverAddDeviceOffset,
                            Image.Entry, 8));
    ok(Model->finishEntry());
    ok(Model->preparePnpDevices());
    PDO = Result.PnpDevices[0].PDO;
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  uint64_t query(uint64_t Object, uint32_t Property, uint64_t Size,
                 uint64_t Buffer = S, uint64_t Length = S + 0x300) {
    return llvm::cantFail(Model->call(
        "IoGetDeviceProperty", {Object, Property, Size, Buffer, Length}));
  }
  uint64_t length() {
    return llvm::cantFail(Memory->readInteger(S + 0x300, 4));
  }
  void rejected(llvm::Expected<uint64_t> R) {
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
};
TEST_F(KernelDeviceProperty, NativeRegistryAndNodePropertyMatrix) {
  struct NativeCase {
    uint32_t Property, Capacity;
    bool Absent;
    uint32_t Status, Length;
    const char *Hex;
  };
  static const NativeCase Cases[] = {
#define NEVERD_NATIVE_PROPERTY_CASE(Property, Capacity, Absent, Status,        \
                                    Length, Hex)                               \
  {Property, Capacity, Absent, Status, Length, Hex},
#include "DevicePropertyNativeCases.def"
#undef NEVERD_NATIVE_PROPERTY_CASE
  };
  for (const auto &C : Cases) {
    SCOPED_TRACE(std::to_string(C.Property) + ":" + std::to_string(C.Capacity) +
                 ":" + std::to_string(C.Absent));
    auto O = propertyOptions();
    auto &P = O.PnpDevices[0].DeviceProperties;
    P->clear();
    std::vector<uint8_t> Data;
    if (C.Property == 1)
      Data = {'N', 0, 'D', 0, 0, 0, 0, 0};
    else if (C.Property == 2)
      Data = {'C', 0, 'o', 0, 'm', 0, 'p', 0, 'a', 0, 't', 0, 0, 0, 0, 0};
    else
      Data = {uint8_t(C.Property == 17 ? 42 : 0), 0, 0, 0};
    P->push_back({C.Property, C.Absent && C.Property != 14
                                  ? std::nullopt
                                  : std::optional(Data)});
    initialize(O);
    ok(Memory->write(S, std::vector<uint8_t>(32, 0xa5)));
    EXPECT_EQ(query(PDO, C.Property, C.Capacity), C.Status);
    EXPECT_EQ(length(), C.Length);
    std::vector<uint8_t> Actual(32);
    ok(Memory->read(S, Actual));
    EXPECT_EQ(llvm::toHex(Actual, true), C.Hex);
  }
}
TEST_F(KernelDeviceProperty,
       ShortBuffersNeverTouchPayloadAndLongArgumentsTruncate) {
  initialize();
  EXPECT_EQ(query(PDO, 0xabcd0001, UINT64_C(0x100000001), 0xdead0000),
            0xc0000023u);
  EXPECT_EQ(length(), 8u);
  EXPECT_EQ(query(PDO, 2, 19, 0), 0xc0000034u);
  EXPECT_EQ(length(), 19u);
  EXPECT_EQ(query(PDO, 14, 4), 0u);
  EXPECT_EQ(length(), 4u);
  EXPECT_EQ(llvm::cantFail(Memory->readInteger(S, 4)), 7u);
  rejected(Model->call("IoGetDeviceProperty", {PDO, 1, 8, 0, S + 0x300}));
  rejected(Model->call("IoGetDeviceProperty", {PDO, 1, 8, S, S + 2}));
  EXPECT_EQ(length(), 4u);
}
TEST_F(KernelDeviceProperty,
       MissingFactsAndInvalidLifetimesDoNotBecomeEmptyProperties) {
  auto O = propertyOptions();
  O.PnpDevices[0].DeviceProperties.reset();
  initialize(O);
  rejected(Model->call("IoGetDeviceProperty", {PDO, 1, 0, 0, S + 0x300}));
  initialize();
  rejected(Model->call("IoGetDeviceProperty", {PDO, 8, 0, 0, S + 0x300}));
  rejected(Model->call("IoGetDeviceProperty", {0x1234, 1, 0, 0, S + 0x300}));
  rejected(Model->call("IoGetDeviceProperty", {PDO, 1, 0, 0, 0}));
  EXPECT_EQ(query(0, 1, 0, 0), 0xc0000010u);
  EXPECT_EQ(length(), 0u);
  EXPECT_EQ(query(PDO, 23, 0, 0), 0xc00000f0u);
  EXPECT_EQ(length(), 0u);
  llvm::cantFail(Model->call("KfRaiseIrql", {1}));
  rejected(Model->call("IoGetDeviceProperty", {PDO, 1, 0, 0, S + 0x300}));
  llvm::cantFail(Model->call("KeLowerIrql", {0}));
  auto Invocation = llvm::cantFail(Model->beginAddDevice("port"));
  (void)Invocation;
  ok(Model->finishAddDevice("port", 0xc0000001));
  rejected(Model->call("IoGetDeviceProperty", {PDO, 1, 0, 0, S + 0x300}));
}
TEST(DriverDeviceProperty, CompiledTwoCallQueriesAndProviderRemoval) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (bool Paging : {false, true}) {
        auto O = propertyOptions();
        O.Contract = Contract;
        O.Unload = true;
        O.PageTables = Paging;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        for (auto Minor :
             {DevicePnpRequest::Start, DevicePnpRequest::QueryRemove,
              DevicePnpRequest::Remove}) {
          DriverRequest R;
          R.Kind = DriverRequestKind::Pnp;
          R.DeviceID = "port";
          R.Pnp = DriverPnpOperation{Minor, DriverBusCompletion{0, 3}};
          O.Requests.push_back(R);
        }
        auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                   "driver_device_properties.sys",
                               O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
        EXPECT_EQ(R->NTStatus, 0u);
        EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
        ASSERT_EQ(R->PnpDevices.size(), 1u);
        EXPECT_EQ(R->PnpDevices[0].AddDeviceStatus, 0u);
        EXPECT_FALSE(R->PnpDevices[0].ProviderPresent);
      }
}
} // namespace
} // namespace neverd::emulation
