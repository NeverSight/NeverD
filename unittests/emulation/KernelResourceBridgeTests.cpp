//===- KernelResourceBridgeTests.cpp - Physical resource transactions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check resource packet and epoch ownership through public KernelModel calls
/// and the real Unicorn memory backend, independently of the genuine fixture.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

#include <array>

namespace neverd::emulation {
namespace {
using namespace windows;

class KernelResourceBridge : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t Entry = 0x180001000;
  static constexpr uint64_t Completion = 0x180001100;
  static constexpr uint32_t Failed = 0xc0000001;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  std::vector<uint64_t> PDO, FDO;

  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  template <class T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  void reject(llvm::Error E, llvm::StringRef Text = {}) {
    ASSERT_TRUE(bool(E));
    EXPECT_NE(llvm::toString(std::move(E)).find(Text.str()), std::string::npos);
  }
  template <class T>
  void reject(llvm::Expected<T> Value, llvm::StringRef Text = {}) {
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find(Text.str()),
              std::string::npos);
  }
  uint64_t get(uint64_t Address, unsigned Width = 8) {
    return take(Memory->readInteger(Address, Width));
  }
  void put(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    ok(Model->validateGuestAccess(Address, Width, true));
    ok(Memory->writeInteger(Address, Value, Width));
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Args) {
    return take(Model->call(Name, Args));
  }
  uint64_t physical(unsigned Index = 0) {
    return 0x400000000ULL + Index * 0x10000;
  }
  void initialize(unsigned Count = 1) {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(Scratch, 0x10000, Read | Write));
    DriverOptions Options;
    for (unsigned I = 0; I < Count; ++I) {
      DriverPnpDevice Device;
      Device.ID = "bank" + std::to_string(I);
      Device.Bus = DriverBusKind::RegisterBank;
      Device.InitialDevicePower = DevicePowerState::D0;
      Device.InitialSystemPower = SystemPowerState::Working;
      Device.InitialReportedDevicePower = DevicePowerState::D0;
      DriverMemoryResource Resource;
      Resource.ID = "range";
      Resource.RawStart = 0x300000000ULL + I * 0x10000;
      Resource.TranslatedStart = physical(I);
      Resource.Length = 0x1000;
      Resource.Registers = {
          {0, 4, DriverRegisterAccess::ReadWrite, 0x12340000 + I},
          {0xffc, 4, DriverRegisterAccess::ReadOnly, 0x99}};
      Device.Resources.push_back(Resource);
      Options.PnpDevices.push_back(Device);
    }
    Result.Configuration = Options;
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Entry;
    Image.Size = 0x3000;
    ok(Model->initialize(Image, Options));
    const uint64_t Extension =
        get(Model->driverObject() + DriverExtensionOffset);
    put(Extension + DriverAddDeviceOffset, Entry);
    for (unsigned Major : {0x16u, 0x1bu})
      put(Model->driverObject() + DriverDispatchOffset + Major * 8, Entry);
    ok(Model->finishEntry());
    ok(Model->preparePnpDevices());
    for (unsigned I = 0; I < Count; ++I) {
      const std::string ID = "bank" + std::to_string(I);
      PDO.push_back(take(Model->beginAddDevice(ID)).Argument1);
      EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 16, 0,
                                        UnknownDeviceType, 0, 0, Scratch}),
                0u);
      FDO.push_back(get(Scratch));
      put(FDO.back() + DeviceFlagsOffset,
          DeviceBufferedIO | DevicePowerPageable, 4);
      EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {FDO.back(), PDO.back()}),
                PDO.back());
      ok(Model->finishAddDevice(ID, 0));
    }
  }
  uint64_t prepare(DriverRequest Input, bool Handler = true) {
    const uint64_t IRP = take(Model->beginRequest(Input)).IRP;
    const uint64_t Stack = get(IRP + IRPStackPointerOffset);
    std::array<uint8_t, StackCompletionOffset> Prefix;
    ok(Memory->read(Stack, Prefix));
    ok(Memory->write(Stack - StackSize, Prefix));
    put(Stack - StackSize + StackControlOffset,
        Handler ? StackInvokeOnSuccess | StackInvokeOnError : 0, 1);
    if (Handler)
      put(Stack - StackSize + StackCompletionOffset, Completion);
    return IRP;
  }
  uint64_t begin(DevicePnpRequest Minor, uint32_t Status = 0,
                 uint64_t Delay = 0, unsigned Index = 0, bool Handler = true) {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Pnp;
    Request.DeviceID = "bank" + std::to_string(Index);
    Request.Pnp = DriverPnpOperation{Minor, {Status, Delay}};
    return prepare(Request, Handler);
  }
  void hold(uint64_t IRP, uint32_t Status = 0, unsigned Index = 0,
            bool Power = false) {
    EXPECT_EQ(call(Power ? "PoCallDriver" : "IofCallDriver", {PDO[Index], IRP}),
              Status);
    auto Guest = Model->takeGuestCall();
    ASSERT_TRUE(Guest);
    EXPECT_EQ(Guest->PC, Completion);
    EXPECT_EQ(take(Model->finishGuestCall(Guest->Token,
                                          StatusMoreProcessingRequired)),
              std::optional<uint64_t>(Status));
  }
  void finish(uint64_t IRP, uint32_t Status = 0) {
    put(IRP + IRPStatusOffset, Status, 4);
    put(IRP + IRPInformationOffset, 0);
    call("IofCompleteRequest", {IRP, 0});
    ok(Model->recordDispatchReturn(IRP, Status));
    ok(Model->finalizeRequest(IRP));
  }
  void transaction(DevicePnpRequest Minor, unsigned Index = 0) {
    const uint64_t IRP = begin(Minor, 0, 0, Index);
    hold(IRP, 0, Index);
    finish(IRP);
  }
  uint64_t map(unsigned Index = 0) {
    return call("MmMapIoSpace", {physical(Index), 0x1000, 0});
  }
  uint64_t alias(unsigned Index = 0) {
    return call("MmMapIoSpaceEx", {physical(Index), 4, 0x204});
  }
  void unmap(uint64_t Address, uint64_t Length = 0x1000) {
    call("MmUnmapIoSpace", {Address, Length});
  }
  uint64_t power(DevicePowerState State) {
    DriverRequest Input;
    Input.Kind = DriverRequestKind::Power;
    Input.DeviceID = "bank0";
    Input.Power = DriverPowerOperation{};
    Input.Power->Type = DriverPowerType::Device;
    Input.Power->Minor = DevicePowerRequest::Set;
    Input.Power->State = uint32_t(State);
    Input.Power->BusCompletion.Status = 0;
    return prepare(Input);
  }
};

TEST_F(KernelResourceBridge,
       PackedListsAreReadonlyAndExpireAtFinalIrpCompletion) {
  initialize();
  const uint64_t IRP = begin(DevicePnpRequest::Start);
  const uint64_t Stack = get(IRP + IRPStackPointerOffset);
  const uint64_t Raw = get(Stack + StackStartResourcesOffset);
  const uint64_t Translated = get(Stack + StackStartTranslatedResourcesOffset);
  ASSERT_NE(Raw, 0u);
  ASSERT_NE(Translated, Raw);
  EXPECT_EQ(get(Raw, 4), 1u);
  EXPECT_EQ(get(Raw + 12, 2), 1u);
  EXPECT_EQ(get(Raw + 14, 2), 1u);
  EXPECT_EQ(get(Raw + 16, 4), 1u);
  EXPECT_EQ(get(Raw + 20, 1), 3u);
  EXPECT_EQ(get(Raw + 21, 1), 1u);
  EXPECT_EQ(get(Raw + 24), 0x300000000ULL);
  EXPECT_EQ(get(Translated + 24), physical());
  EXPECT_EQ(get(Translated + 32, 4), 0x1000u);
  reject(Model->validateGuestAccess(Raw, 1, true));
  reject(Model->validateGuestAccess(Translated + 24, 8, true));
  reject(Model->call("MmMapIoSpace", {physical(), 4, 0}), "not assigned");
  hold(IRP);
  ok(Model->validateGuestAccess(Raw, 40, false));
  const auto Mapping = map();
  EXPECT_EQ(get(Mapping, 4), 0x12340000u);
  finish(IRP);
  reject(Model->validateGuestAccess(Raw, 1, false));
  reject(Model->validateGuestAccess(Translated + 39, 1, false));
  EXPECT_EQ(get(Mapping, 4), 0x12340000u);
  unmap(Mapping);
}

TEST_F(KernelResourceBridge, DelayedLowerStartDoesNotAssignResourcesAtReceipt) {
  initialize();
  const uint64_t IRP = begin(DevicePnpRequest::Start, 0, 17, 0, false);
  EXPECT_EQ(call("IofCallDriver", {PDO[0], IRP}), 0x103u);
  EXPECT_EQ(Result.Requests.back().Pnp->BusReceivedAt100ns, 0u);
  reject(Model->call("MmMapIoSpace", {physical(), 4, 0}), "not assigned");
  ok(Model->recordDispatchReturn(IRP, 0x103));
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  EXPECT_EQ(Result.Requests.back().Pnp->BusCompletedAt100ns, 17u);
  EXPECT_TRUE(Result.Requests.back().Completed);
  ok(Model->finalizeRequest(IRP));
  const auto Mapping = map();
  EXPECT_EQ(get(Mapping, 4), 0x12340000u);
  unmap(Mapping);
}

TEST_F(KernelResourceBridge,
       FailedLowerStartLeavesFreshRetryAndNoPhysicalAssignment) {
  initialize();
  const uint64_t IRP = begin(DevicePnpRequest::Start, Failed);
  const auto OldRaw =
      get(get(IRP + IRPStackPointerOffset) + StackStartResourcesOffset);
  hold(IRP, Failed);
  reject(Model->call("MmMapIoSpace", {physical(), 4, 0}), "not assigned");
  finish(IRP, Failed);
  EXPECT_EQ(Result.PnpDevices[0].PnpState, DevicePnpState::NotStarted);
  const uint64_t Retry = begin(DevicePnpRequest::Start);
  EXPECT_NE(get(get(Retry + IRPStackPointerOffset) + StackStartResourcesOffset),
            OldRaw);
  hold(Retry);
  finish(Retry);
  const auto Mapping = map();
  EXPECT_EQ(get(Mapping, 4), 0x12340000u);
  unmap(Mapping);
}

TEST_F(KernelResourceBridge, UpperStartFailureRequiresUnmapBeforeAtomicRetry) {
  initialize();
  const uint64_t IRP = begin(DevicePnpRequest::Start);
  hold(IRP);
  const uint64_t Mapping = map();
  ok(Memory->writeInteger(Mapping, 0x76543210, 4));
  put(IRP + IRPStatusOffset, Failed, 4);
  put(IRP + IRPInformationOffset, 0);
  const auto Cursor = get(IRP + IRPStackPointerOffset);
  reject(Model->call("IofCompleteRequest", {IRP, 0}), "mapping");
  EXPECT_EQ(get(IRP + IRPStackPointerOffset), Cursor);
  EXPECT_FALSE(Result.Requests.back().Completed);
  EXPECT_EQ(get(Mapping, 4), 0x76543210u);
  unmap(Mapping);
  finish(IRP, Failed);
  reject(Model->call("MmMapIoSpace", {physical(), 4, 0}), "not assigned");
  transaction(DevicePnpRequest::Start);
  const auto Remapped = map();
  EXPECT_NE(Remapped, Mapping);
  EXPECT_EQ(get(Remapped, 4), 0x76543210u);
  unmap(Remapped);
}

TEST_F(KernelResourceBridge,
       StopCompletionWithMappingFailsBeforeProviderAndCanRetry) {
  initialize();
  transaction(DevicePnpRequest::Start);
  const auto Mapping = map();
  const auto Alias = alias();
  ok(Memory->writeInteger(Alias, 0xabcd9876, 4));
  transaction(DevicePnpRequest::QueryStop);
  const uint64_t IRP = begin(DevicePnpRequest::Stop, 0, 0, 0, false);
  const auto Cursor = get(IRP + IRPStackPointerOffset);
  reject(Model->call("IofCallDriver", {PDO[0], IRP}), "mapping");
  EXPECT_EQ(get(IRP + IRPStackPointerOffset), Cursor);
  EXPECT_FALSE(Result.Requests.back().Pnp->BusReceivedAt100ns);
  EXPECT_FALSE(Result.Requests.back().Completed);
  EXPECT_EQ(get(Mapping, 4), 0xabcd9876u);
  unmap(Mapping);
  EXPECT_EQ(get(Alias, 4), 0xabcd9876u);
  unmap(Alias, 4);
  EXPECT_EQ(call("IofCallDriver", {PDO[0], IRP}), 0u);
  ok(Model->recordDispatchReturn(IRP, 0));
  ok(Model->finalizeRequest(IRP));
  EXPECT_EQ(Result.PnpDevices[0].PnpState, DevicePnpState::Stopped);
  reject(Model->call("MmMapIoSpace", {physical(), 4, 0}), "not assigned");
  transaction(DevicePnpRequest::Start);
  const auto Remapped = map();
  EXPECT_EQ(get(Remapped, 4), 0xabcd9876u);
  unmap(Remapped);
}

TEST_F(KernelResourceBridge, PhysicalD3TakesEffectBeforeUpperCompletion) {
  initialize();
  transaction(DevicePnpRequest::Start);
  const auto Mapping = map();
  const uint64_t IRP = power(DevicePowerState::D3);
  hold(IRP, 0, 0, true);
  EXPECT_EQ(Result.PnpDevices[0].DevicePower, DevicePowerState::D0);
  EXPECT_FALSE(Result.Requests.back().Completed);
  // A checked MMIO access failure is terminal for this backend; it is last.
  reject(Memory->readInteger(Mapping, 4), "physical device power D0");
}

TEST_F(KernelResourceBridge,
       D3D0RetainsAliasValuesAndPoSetDoesNotChangePhysicalPower) {
  initialize();
  transaction(DevicePnpRequest::Start);
  const auto Mapping = map();
  const auto Alias = alias();
  ok(Memory->writeInteger(Alias, 0x24681357, 4));
  EXPECT_EQ(call("PoSetPowerState", {FDO[0], 1, 4}), 1u);
  EXPECT_EQ(get(Mapping, 4), 0x24681357u);
  const uint64_t Down = power(DevicePowerState::D3);
  hold(Down, 0, 0, true);
  finish(Down);
  EXPECT_EQ(Result.PnpDevices[0].DevicePower, DevicePowerState::D3);
  const uint64_t Up = power(DevicePowerState::D0);
  hold(Up, 0, 0, true);
  EXPECT_EQ(Result.PnpDevices[0].DevicePower, DevicePowerState::D3);
  EXPECT_EQ(get(Alias, 4), 0x24681357u);
  finish(Up);
  EXPECT_EQ(get(Mapping, 4), 0x24681357u);
  unmap(Alias, 4);
  unmap(Mapping);
}

TEST_F(KernelResourceBridge, SurpriseArrivalDisablesHardwareBeforeForwarding) {
  initialize();
  transaction(DevicePnpRequest::Start);
  const auto Mapping = map();
  begin(DevicePnpRequest::SurpriseRemoval);
  EXPECT_FALSE(Result.Requests.back().Pnp->BusReceivedAt100ns);
  reject(Model->call("MmMapIoSpace", {physical(), 4, 0}), "absent");
  // Access cannot wait until lower or upper surprise completion to be denied.
  reject(Memory->readInteger(Mapping, 4),
         "unavailable physical resource epoch");
}

TEST_F(KernelResourceBridge, TwoProvidersHaveSeparateEpochsAndPhysicalBanks) {
  initialize(2);
  transaction(DevicePnpRequest::Start);
  const auto First = map();
  ok(Memory->writeInteger(First, 0x55556666, 4));
  reject(Model->call("MmMapIoSpace", {physical(1), 4, 0}), "not assigned");
  transaction(DevicePnpRequest::Start, 1);
  const auto Second = map(1);
  EXPECT_EQ(get(Second, 4), 0x12340001u);
  transaction(DevicePnpRequest::QueryStop);
  unmap(First);
  transaction(DevicePnpRequest::Stop);
  EXPECT_EQ(get(Second, 4), 0x12340001u);
  transaction(DevicePnpRequest::Start);
  const auto NewFirst = map();
  EXPECT_EQ(get(NewFirst, 4), 0x55556666u);
  EXPECT_EQ(get(Second, 4), 0x12340001u);
  unmap(NewFirst);
  unmap(Second);
}

} // namespace
} // namespace neverd::emulation
