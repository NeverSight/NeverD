//===- KernelPnpDeviceTests.cpp - PDO and AddDevice ownership ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise independent provider/guest device inventories, AddDevice callback
/// identities and ordinary failure without replacing guest cleanup.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;

class KernelPnpDevice : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t AddPC = 0x180001000;
  static constexpr uint32_t Failed = 0xc0000001;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;

  void SetUp() override {
    auto Backend = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
    Memory = std::move(*Backend);
    success(Memory->map(Scratch, 0x1000, Read | Write));
  }

  void success(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  template <class T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return T{};
    }
    return std::move(*Value);
  }
  void rejected(llvm::Error E, llvm::StringRef Text) {
    ASSERT_TRUE(bool(E));
    const auto Message = llvm::toString(std::move(E));
    EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
  }
  template <class T>
  void rejected(llvm::Expected<T> Value, llvm::StringRef Text) {
    ASSERT_FALSE(bool(Value));
    rejected(Value.takeError(), Text);
  }
  uint64_t get(uint64_t Address, unsigned Width = 8) {
    return take(Memory->readInteger(Address, Width));
  }
  void write(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    success(Model->validateGuestAccess(Address, Width, true));
    success(Memory->writeInteger(Address, Value, Width));
  }
  void initialize(bool RegisterAdd = true) {
    DriverOptions Options;
    for (const auto *ID : {"first", "second"}) {
      DriverPnpDevice Device;
      Device.ID = ID;
      Device.InitialDevicePower = DevicePowerState::D0;
      Device.InitialSystemPower = SystemPowerState::Working;
      Options.PnpDevices.push_back(Device);
    }
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = AddPC;
    Image.Size = 0x3000;
    success(Model->initialize(Image, Options));
    if (RegisterAdd)
      write(get(Model->driverObject() + DriverExtensionOffset) +
                DriverAddDeviceOffset,
            AddPC);
  }
  void prepare() {
    initialize();
    success(Model->finishEntry());
    success(Model->preparePnpDevices());
  }
  uint64_t createFdo() {
    EXPECT_EQ(take(Model->call("IoCreateDevice",
                               {Model->driverObject(), 16, 0, UnknownDeviceType,
                                SecureOpen, 0, Scratch})),
              StatusSuccess);
    const auto Device = get(Scratch);
    write(Device + DeviceFlagsOffset, DeviceBufferedIO, 4);
    return Device;
  }
};

TEST_F(KernelPnpDevice,
       PreparationUsesInitializedOptionsAfterEntryExactlyOnce) {
  initialize();
  rejected(Model->preparePnpDevices(), "completed entry");
  success(Model->finishEntry());
  success(Model->preparePnpDevices());
  ASSERT_EQ(Result.PnpDevices.size(), 2u);
  EXPECT_TRUE(Result.Configuration.PnpDevices.empty());
  EXPECT_EQ(Result.PnpDevices[0].ID, "first");
  EXPECT_EQ(Result.PnpDevices[1].ID, "second");
  EXPECT_TRUE(Result.PnpDevices[0].ProviderPresent);
  EXPECT_FALSE(Result.PnpDevices[0].AddDeviceStatus);
  rejected(Model->preparePnpDevices(), "completed entry");
}

TEST_F(KernelPnpDevice, ConfiguredPdoRequiresAnActualAddDeviceCallback) {
  initialize(false);
  success(Model->finishEntry());
  rejected(Model->preparePnpDevices(), "registered guest AddDevice");
  EXPECT_TRUE(Result.PnpDevices.empty());
  EXPECT_TRUE(Result.Devices.empty());
}

TEST_F(KernelPnpDevice, ProviderAndGuestKeepSeparateDriverDeviceLists) {
  prepare();
  const auto A = Result.PnpDevices[0].PDO;
  const auto B = Result.PnpDevices[1].PDO;
  const auto Owner = get(A + DeviceDriverOffset);
  EXPECT_NE(Owner, Model->driverObject());
  EXPECT_EQ(get(B + DeviceDriverOffset), Owner);
  EXPECT_EQ(get(Owner + DriverDeviceHead), B);
  EXPECT_EQ(get(B + DeviceNext), A);
  EXPECT_EQ(get(A + DeviceNext), 0u);
  EXPECT_EQ(get(A + DeviceFlagsOffset, 4),
            DeviceBusEnumerated | DevicePowerPageable);
  const auto Fdo = createFdo();
  EXPECT_EQ(get(Model->driverObject() + DriverDeviceHead), Fdo);
  EXPECT_EQ(get(Fdo + DeviceNext), 0u);
  EXPECT_EQ(take(Model->call("IoAttachDeviceToDeviceStack", {Fdo, A})), A);
  success(Model->snapshot());
  ASSERT_EQ(Result.Devices.size(), 1u);
  EXPECT_EQ(Result.Devices[0].Address, Fdo);
  EXPECT_TRUE(Result.PnpDevices[0].Attached);
  EXPECT_FALSE(Result.PnpDevices[1].Attached);
}

TEST_F(KernelPnpDevice, GuestCannotDeleteMutateOrImpersonateProviderObjects) {
  prepare();
  const auto PDO = Result.PnpDevices[0].PDO;
  const auto Owner = get(PDO + DeviceDriverOffset);
  rejected(Model->call("IoDeleteDevice", {PDO}), "provider-owned");
  rejected(Model->call("IoCreateDevice",
                       {Owner, 0, 0, UnknownDeviceType, 0, 0, Scratch}),
           "unknown DRIVER_OBJECT");
  rejected(Model->call("IoAttachDeviceToDeviceStack", {PDO, createFdo()}),
           "guest-owned");
  for (uint64_t Offset : {DeviceNext, DeviceFlagsOffset, DeviceStackCountOffset,
                          DeviceAlignmentOffset})
    rejected(Model->validateGuestAccess(PDO + Offset, 1, true), "opaque");
  rejected(Model->validateGuestAccess(Owner + DriverDeviceHead, 8, true),
           "opaque");
  success(Model->snapshot());
  EXPECT_TRUE(Result.PnpDevices[0].ProviderPresent);
}

TEST_F(KernelPnpDevice,
       AddDeviceUsesConcreteArgumentsAndSerialSingleInvocation) {
  prepare();
  const auto PDO = Result.PnpDevices[0].PDO;
  const auto Call = take(Model->beginAddDevice("first"));
  EXPECT_EQ(Call.PC, AddPC);
  EXPECT_EQ(Call.Argument0, Model->driverObject());
  EXPECT_EQ(Call.Argument1, PDO);
  EXPECT_EQ(Call.IRP, 0u);
  rejected(Model->beginAddDevice("second"), "serially");
  const auto Fdo = createFdo();
  EXPECT_EQ(take(Model->call("IoAttachDeviceToDeviceStack", {Fdo, PDO})), PDO);
  success(Model->finishAddDevice("first", StatusSuccess));
  EXPECT_EQ(Result.PnpDevices[0].AddDeviceStatus, StatusSuccess);
  EXPECT_TRUE(Result.PnpDevices[0].Attached);
  rejected(Model->beginAddDevice("first"), "once");
  rejected(Model->finishAddDevice("first", StatusSuccess), "active invocation");
  rejected(Model->beginAddDevice("missing"), "configured PDO");
}

TEST_F(KernelPnpDevice, SuccessfulFilterMayDeclineAttachment) {
  prepare();
  take(Model->beginAddDevice("first"));
  success(Model->finishAddDevice("first", StatusSuccess));
  EXPECT_EQ(Result.PnpDevices[0].AddDeviceStatus, StatusSuccess);
  EXPECT_FALSE(Result.PnpDevices[0].Attached);
  EXPECT_TRUE(Result.PnpDevices[0].ProviderPresent);
  EXPECT_EQ(Result.PnpDevices[0].PnpState, DevicePnpState::NotStarted);
}

TEST_F(KernelPnpDevice, FailedAddRetiresOnlyTheUnreferencedProvider) {
  prepare();
  const auto First = Result.PnpDevices[0].PDO;
  const auto Second = Result.PnpDevices[1].PDO;
  const auto Owner = get(First + DeviceDriverOffset);
  take(Model->beginAddDevice("first"));
  success(Model->finishAddDevice("first", Failed));
  EXPECT_EQ(Result.PnpDevices[0].AddDeviceStatus, Failed);
  EXPECT_FALSE(Result.PnpDevices[0].ProviderPresent);
  EXPECT_EQ(Result.PnpDevices[0].PnpState, DevicePnpState::NotStarted);
  EXPECT_EQ(get(Owner + DriverDeviceHead), Second);
  EXPECT_EQ(get(Second + DeviceNext), 0u);
  rejected(Model->validateGuestAccess(First, 1, false), "freed");
  success(Model->validateGuestAccess(Second, 2, false));
}

TEST_F(KernelPnpDevice,
       FailedAddKeepsGuestLeaksVisibleWithoutSyntheticCleanup) {
  prepare();
  const auto PDO = Result.PnpDevices[0].PDO;
  take(Model->beginAddDevice("first"));
  const auto Fdo = createFdo();
  EXPECT_EQ(take(Model->call("IoAttachDeviceToDeviceStack", {Fdo, PDO})), PDO);
  rejected(Model->finishAddDevice("first", Failed), "live guest device");
  EXPECT_EQ(Result.PnpDevices[0].AddDeviceStatus, Failed);
  EXPECT_TRUE(Result.PnpDevices[0].ProviderPresent);
  EXPECT_TRUE(Result.PnpDevices[0].Attached);
  ASSERT_EQ(Result.Devices.size(), 1u);
  EXPECT_EQ(Result.Devices[0].Address, Fdo);
  success(Model->validateGuestAccess(Fdo, 2, false));
}

TEST_F(KernelPnpDevice, FailedAddCannotHideAnUnattachedGuestDeviceLeak) {
  prepare();
  take(Model->beginAddDevice("first"));
  const auto Fdo = createFdo();
  rejected(Model->finishAddDevice("first", Failed), "live guest device");
  EXPECT_FALSE(Result.PnpDevices[0].Attached);
  EXPECT_TRUE(Result.PnpDevices[0].ProviderPresent);
  ASSERT_EQ(Result.Devices.size(), 1u);
  EXPECT_EQ(Result.Devices[0].Address, Fdo);
}

TEST_F(KernelPnpDevice, PendingIsNotAnAddDeviceCompletionStatus) {
  prepare();
  take(Model->beginAddDevice("first"));
  rejected(Model->finishAddDevice("first", StatusPending),
           "STATUS_SUCCESS or a failing");
  EXPECT_EQ(Result.PnpDevices[0].AddDeviceStatus, StatusPending);
  EXPECT_TRUE(Result.PnpDevices[0].ProviderPresent);
}

TEST_F(KernelPnpDevice, DetachedDeviceCannotMoveToAnotherProviderIdentity) {
  prepare();
  const auto First = Result.PnpDevices[0].PDO;
  const auto Second = Result.PnpDevices[1].PDO;
  take(Model->beginAddDevice("first"));
  const auto Fdo = createFdo();
  EXPECT_EQ(take(Model->call("IoAttachDeviceToDeviceStack", {Fdo, First})),
            First);
  success(Model->finishAddDevice("first", StatusSuccess));
  take(Model->call("IoDetachDevice", {First}));
  rejected(Model->call("IoAttachDeviceToDeviceStack", {Fdo, Second}),
           "another PnP identity");
  success(Model->snapshot());
  EXPECT_FALSE(Result.PnpDevices[0].Attached);
  EXPECT_FALSE(Result.PnpDevices[1].Attached);
  EXPECT_EQ(take(Model->call("IoAttachDeviceToDeviceStack", {Fdo, First})),
            First);
}

TEST_F(KernelPnpDevice, RemoveFindsDetachedFdoThatWasNeverDeleted) {
  prepare();
  const auto PDO = Result.PnpDevices[0].PDO;
  take(Model->beginAddDevice("first"));
  const auto Fdo = createFdo();
  EXPECT_EQ(take(Model->call("IoAttachDeviceToDeviceStack", {Fdo, PDO})), PDO);
  success(Model->finishAddDevice("first", StatusSuccess));
  take(Model->call("IoDetachDevice", {PDO}));
  DriverRequest Remove;
  Remove.Kind = DriverRequestKind::Pnp;
  Remove.DeviceID = "first";
  Remove.Pnp = DriverPnpOperation{DevicePnpRequest::Remove, {StatusSuccess, 0}};
  const auto Request = take(Model->beginRequest(Remove));
  EXPECT_EQ(Request.PC, 0u);
  rejected(Model->finalizeRequest(Request.IRP), "associated guest device");
  success(Model->snapshot());
  EXPECT_TRUE(Result.PnpDevices[0].ProviderPresent);
  EXPECT_FALSE(Result.PnpDevices[0].Attached);
  ASSERT_EQ(Result.Devices.size(), 1u);
  EXPECT_EQ(Result.Devices[0].Address, Fdo);
}

TEST_F(KernelPnpDevice, UnattachedControlDeviceIsNotAssociatedWithPdoRemoval) {
  prepare();
  const auto PDO = Result.PnpDevices[0].PDO;
  take(Model->beginAddDevice("first"));
  const auto Control = createFdo();
  const auto Fdo = createFdo();
  EXPECT_EQ(take(Model->call("IoAttachDeviceToDeviceStack", {Fdo, PDO})), PDO);
  success(Model->finishAddDevice("first", StatusSuccess));
  take(Model->call("IoDetachDevice", {PDO}));
  take(Model->call("IoDeleteDevice", {Fdo}));
  DriverRequest Remove;
  Remove.Kind = DriverRequestKind::Pnp;
  Remove.DeviceID = "first";
  Remove.Pnp = DriverPnpOperation{DevicePnpRequest::Remove, {StatusSuccess, 0}};
  const auto Request = take(Model->beginRequest(Remove));
  success(Model->finalizeRequest(Request.IRP));
  EXPECT_FALSE(Result.PnpDevices[0].ProviderPresent);
  ASSERT_EQ(Result.Devices.size(), 1u);
  EXPECT_EQ(Result.Devices[0].Address, Control);
}
TEST_F(KernelPnpDevice, SameDeadlineProvidersKeepIndependentLifecycleResults) {
  prepare();
  for (const auto *ID : {"first", "second"}) {
    take(Model->beginAddDevice(ID));
    success(Model->finishAddDevice(ID, StatusSuccess));
  }
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = "second";
  Request.Pnp =
      DriverPnpOperation{DevicePnpRequest::Start, {StatusNotSupported, 10}};
  const auto Second = take(Model->beginRequest(Request));
  Request.DeviceID = "first";
  Request.Pnp->BusCompletion.Status = StatusSuccess;
  const auto First = take(Model->beginRequest(Request));
  EXPECT_NE(First.IRP, Second.IRP);
  EXPECT_TRUE(Model->requestPending(First.IRP));
  EXPECT_TRUE(Model->requestPending(Second.IRP));
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  EXPECT_FALSE(Model->requestPending());
  success(Model->finalizeRequest(Second.IRP));
  success(Model->finalizeRequest(First.IRP));
  EXPECT_EQ(Result.PnpDevices[0].PnpState, DevicePnpState::Started);
  EXPECT_EQ(Result.PnpDevices[1].PnpState, DevicePnpState::NotStarted);
  ASSERT_EQ(Result.Requests.size(), 2u);
  EXPECT_EQ(Result.Requests[0].DeviceID, "second");
  EXPECT_EQ(Result.Requests[0].IOStatus, StatusNotSupported);
  EXPECT_EQ(Result.Requests[1].DeviceID, "first");
  EXPECT_EQ(Result.Requests[1].IOStatus, StatusSuccess);
  for (const auto &Observation : Result.Requests)
    EXPECT_EQ(Observation.Pnp->BusCompletedAt100ns, 10u);
}

} // namespace
} // namespace neverd::emulation
