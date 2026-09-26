//===- KernelPowerRequestTests.cpp - Power packet and notification ABI ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify the common power request factory, per-object notification state and
/// explicit profile rejections without executing a synthetic callback body.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

#include <algorithm>
#include <array>

namespace neverd::emulation {
namespace {
using namespace windows;

class KernelPowerRequest : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t Entry = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  uint64_t PDO = 0, FDO = 0, Upper = 0;

  void ok(llvm::Error Error) {
    if (Error)
      ADD_FAILURE() << llvm::toString(std::move(Error));
  }
  template <typename T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  template <typename T>
  void reject(llvm::Expected<T> Value, llvm::StringRef Text) {
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
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Arguments) {
    return take(Model->call(Name, Arguments));
  }
  uint64_t create() {
    EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 16, 0,
                                      UnknownDeviceType, 0, 0, Scratch}),
              0u);
    const uint64_t Device = get(Scratch);
    put(Device + DeviceFlagsOffset, DeviceBufferedIO | DevicePowerPageable, 4);
    return Device;
  }
  void initialize(std::optional<DevicePowerState> Seed = DevicePowerState::D3,
                  bool Start = true, bool TwoObjects = false) {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(Scratch, 0x10000, Read | Write));
    DriverOptions Options;
    DriverPnpDevice Device;
    Device.ID = "power0";
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    Device.InitialReportedDevicePower = Seed;
    Options.PnpDevices.push_back(Device);
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Entry;
    Image.Size = 0x3000;
    ok(Model->initialize(Image, Options));
    for (unsigned Major : {0x16u, 0x1bu})
      put(Model->driverObject() + DriverDispatchOffset + Major * 8, Entry);
    const uint64_t Extension =
        get(Model->driverObject() + DriverExtensionOffset);
    put(Extension + DriverAddDeviceOffset, Entry);
    ok(Model->finishEntry());
    ok(Model->preparePnpDevices());
    PDO = take(Model->beginAddDevice("power0")).Argument1;
    ASSERT_NE(PDO, 0u);
    FDO = create();
    EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {FDO, PDO}), PDO);
    if (TwoObjects) {
      Upper = create();
      EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Upper, PDO}), FDO);
    }
    ok(Model->finishAddDevice("power0", 0));
    if (Start) {
      ASSERT_FALSE(TwoObjects);
      DriverRequest Request;
      Request.Kind = DriverRequestKind::Pnp;
      Request.DeviceID = "power0";
      Request.Pnp = DriverPnpOperation{DevicePnpRequest::Start, {0, 0}};
      const auto IRP = take(Model->beginRequest(Request)).IRP;
      forwardAndFinish(IRP);
    }
  }
  DriverRequest input(DriverPowerType Type = DriverPowerType::Device) {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Power;
    Request.DeviceID = "power0";
    DriverPowerOperation Operation;
    Operation.Minor = DevicePowerRequest::Query;
    Operation.Type = Type;
    Operation.State = 4;
    Operation.SystemContext = 0xfedcba98;
    Operation.Action = Type == DriverPowerType::System
                           ? DriverPowerAction::Sleep
                           : DriverPowerAction::None;
    Operation.BusCompletion.Status = 0;
    Request.Power = Operation;
    return Request;
  }
  void forwardAndFinish(uint64_t IRP) {
    const uint64_t Stack = get(IRP + IRPStackPointerOffset);
    std::array<uint8_t, StackCompletionOffset> Prefix;
    ok(Model->validateGuestAccess(Stack, Prefix.size(), false));
    ok(Memory->read(Stack, Prefix));
    ok(Model->validateGuestAccess(Stack - StackSize, Prefix.size(), true));
    ok(Memory->write(Stack - StackSize, Prefix));
    put(Stack - StackSize + StackControlOffset, 0, 1);
    EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), 0u);
    ok(Model->recordDispatchReturn(IRP, 0));
    ok(Model->finalizeRequest(IRP));
  }
  std::optional<DevicePowerState> reported(uint64_t Device) {
    ok(Model->snapshot());
    const auto Found = std::find_if(
        Result.Devices.begin(), Result.Devices.end(),
        [Device](const auto &Record) { return Record.Address == Device; });
    EXPECT_NE(Found, Result.Devices.end());
    return Found == Result.Devices.end() ? std::nullopt
                                         : Found->ReportedDevicePower;
  }
};

TEST_F(KernelPowerRequest, DevicePacketUsesExactFileFreeKernelModeAbi) {
  initialize();
  const auto Request = take(Model->beginRequest(input()));
  const uint64_t IRP = Request.IRP;
  ASSERT_NE(IRP, 0u);
  EXPECT_EQ(Request.PC, Entry);
  EXPECT_EQ(Request.Argument0, FDO);
  EXPECT_EQ(Request.Argument1, IRP);
  EXPECT_EQ(get(IRP, 2), 6u);
  EXPECT_EQ(get(IRP + 2, 2), IRPSize + 2 * StackSize);
  EXPECT_EQ(get(IRP + IRPStackCountOffset, 1), 2u);
  EXPECT_EQ(get(IRP + IRPLocationOffset, 1), 2u);
  EXPECT_EQ(get(IRP + IRPRequestorModeOffset, 1), KernelMode);
  EXPECT_EQ(get(IRP + IRPFlagsOffset, 4), 0u);
  for (uint64_t Offset : {IRPMdlOffset, IRPSystemBufferOffset,
                          IRPUserBufferOffset, IRPOriginalFileOffset})
    EXPECT_EQ(get(IRP + Offset), 0u);
  EXPECT_EQ(get(IRP + IRPStatusOffset, 4), StatusNotSupported);
  EXPECT_EQ(get(IRP + IRPInformationOffset), 0u);
  const uint64_t Stack = get(IRP + IRPStackPointerOffset);
  EXPECT_EQ(Stack, IRP + 0xd0 + 0x48);
  EXPECT_EQ(get(Stack, 1), 0x16u);
  EXPECT_EQ(get(Stack + 1, 1), 3u);
  EXPECT_EQ(get(Stack + 8, 4), 0xfedcba98u);
  EXPECT_EQ(get(Stack + 16, 4), 1u);
  EXPECT_EQ(get(Stack + 24, 4), 4u);
  EXPECT_EQ(get(Stack + 32, 4), 0u);
  EXPECT_EQ(get(Stack + StackDeviceOffset), FDO);
  EXPECT_EQ(get(Stack + StackFileOffset), 0u);
  forwardAndFinish(IRP);
  ASSERT_TRUE(Result.Requests.back().Power);
  EXPECT_EQ(Result.Requests.back().Power->SystemContext, 0xfedcba98u);
  EXPECT_EQ(Result.Requests.back().Origin, DriverRequestOrigin::Scenario);
  EXPECT_FALSE(Result.Requests.back().ResponseIndex);
}

TEST_F(KernelPowerRequest, SystemPacketPreservesExplicitContextAndSleepAction) {
  initialize();
  auto Input = input(DriverPowerType::System);
  Input.Power->SystemContext = 0xab014400;
  const auto Request = take(Model->beginRequest(Input));
  const auto Stack = get(Request.IRP + IRPStackPointerOffset);
  EXPECT_EQ(get(Stack + StackPowerTypeOffset, 4), 0u);
  EXPECT_EQ(get(Stack + StackPowerStateOffset, 4), 4u);
  EXPECT_EQ(get(Stack + StackPowerActionOffset, 4), 2u);
  EXPECT_EQ(get(Stack + StackPowerSystemContextOffset, 4), 0xab014400u);
  // This normal full-prefix copy must preserve opaque bits, not reinterpret
  // them.
  forwardAndFinish(Request.IRP);
  const auto &Power = Result.Requests.back().Power;
  ASSERT_TRUE(Power);
  EXPECT_EQ(Power->SystemContext, 0xab014400u);
  EXPECT_EQ(Power->Action, DriverPowerAction::Sleep);
  EXPECT_EQ(Power->SystemStateAfter, SystemPowerState::Working);
  EXPECT_EQ(Power->DeviceStateAfter, DevicePowerState::D0);
}

TEST_F(KernelPowerRequest,
       NotificationHistoryIsIndependentForEachAssociatedDo) {
  initialize(DevicePowerState::D3, false, true);
  EXPECT_EQ(reported(FDO), DevicePowerState::D3);
  EXPECT_EQ(reported(Upper), DevicePowerState::D3);
  EXPECT_EQ(call("PoSetPowerState", {FDO, 1, 1}), 4u);
  EXPECT_EQ(call("PoSetPowerState", {Upper, 1, 4}), 4u);
  EXPECT_EQ(call("PoSetPowerState", {FDO, 1, 4}), 1u);
  EXPECT_EQ(call("PoSetPowerState", {Upper, 1, 1}), 4u);
  EXPECT_EQ(reported(FDO), DevicePowerState::D3);
  EXPECT_EQ(reported(Upper), DevicePowerState::D0);
  ASSERT_EQ(Result.PnpDevices.size(), 1u);
  EXPECT_EQ(Result.PnpDevices[0].DevicePower, DevicePowerState::D0);
  EXPECT_EQ(Result.PnpDevices[0].SystemPower, SystemPowerState::Working);
  EXPECT_EQ(Result.PnpDevices[0].PnpState, DevicePnpState::NotStarted);
  EXPECT_TRUE(Result.Requests.empty());
}

TEST_F(KernelPowerRequest,
       MissingNotificationSeedIsNeverInferredFromLifecycle) {
  initialize(std::nullopt);
  EXPECT_EQ(Result.PnpDevices[0].DevicePower, DevicePowerState::D0);
  EXPECT_FALSE(reported(FDO));
  reject(Model->call("PoSetPowerState", {FDO, 1, 1}),
         "initial_reported_device_power");
  EXPECT_FALSE(reported(FDO));
  EXPECT_EQ(Result.PnpDevices[0].DevicePower, DevicePowerState::D0);
}

TEST_F(KernelPowerRequest, UnsupportedRouteFlagsRejectBeforePacketPublication) {
  initialize();
  for (uint32_t Flags :
       {uint32_t(DeviceBufferedIO),
        uint32_t(DeviceBufferedIO | DevicePowerPageable | DevicePowerInrush)}) {
    put(FDO + DeviceFlagsOffset, Flags, 4);
    reject(Model->beginRequest(input()), "DO_POWER_PAGABLE");
    EXPECT_EQ(Result.Requests.back().IRP, 0u);
    EXPECT_FALSE(Result.Requests.back().Power);
    EXPECT_EQ(Result.PnpDevices[0].DevicePower, DevicePowerState::D0);
    EXPECT_EQ(reported(FDO), DevicePowerState::D3);
  }
  put(FDO + DeviceFlagsOffset, DeviceBufferedIO | DevicePowerPageable, 4);
  const auto Valid = take(Model->beginRequest(input()));
  forwardAndFinish(Valid.IRP);
  EXPECT_EQ(Result.Requests.back().IOStatus, 0u);
}

TEST_F(KernelPowerRequest, InvalidPacketAndNotificationInputsPreserveState) {
  initialize();
  auto WrongType = input();
  WrongType.Power->Type = static_cast<DriverPowerType>(9);
  reject(Model->beginRequest(WrongType), "power type");
  EXPECT_EQ(Result.Requests.back().IRP, 0u);
  auto WrongState = input();
  WrongState.Power->State = 2;
  reject(Model->beginRequest(WrongState), "D0 or D3");
  EXPECT_EQ(Result.Requests.back().IRP, 0u);
  auto WithFile = input();
  WithFile.File = 9;
  reject(Model->beginRequest(WithFile), "without file");
  EXPECT_EQ(Result.Requests.back().IRP, 0u);
  reject(Model->call("PoSetPowerState", {FDO, 0, 1}), "DevicePowerState");
  reject(Model->call("PoSetPowerState", {FDO, 1, 2}), "D0 and D3");
  reject(Model->call("PoSetPowerState", {PDO, 1, 1}),
         "driver's live PnP device");
  const auto Control = create();
  reject(Model->call("PoSetPowerState", {Control, 1, 1}),
         "driver's live PnP device");
  EXPECT_EQ(reported(FDO), DevicePowerState::D3);
  EXPECT_EQ(call("PoSetPowerState", {FDO, 1, 1}), 4u);
  const auto Valid = take(Model->beginRequest(input()));
  forwardAndFinish(Valid.IRP);
  EXPECT_EQ(Result.PnpDevices[0].DevicePower, DevicePowerState::D0);
}

TEST_F(KernelPowerRequest, VistaStartNextHasNoHandshakeOrCompletionSideEffect) {
  initialize();
  const auto Request = take(Model->beginRequest(input()));
  const auto IRP = Request.IRP;
  const auto Stack = get(IRP + IRPStackPointerOffset);
  for (unsigned I = 0; I != 3; ++I)
    EXPECT_EQ(call("PoStartNextPowerIrp", {IRP}), 0u);
  EXPECT_EQ(get(IRP + IRPStackPointerOffset), Stack);
  EXPECT_EQ(get(IRP + IRPStatusOffset, 4), StatusNotSupported);
  EXPECT_FALSE(Result.Requests.back().Completed);
  EXPECT_FALSE(Result.Requests.back().Power->BusReceivedAt100ns);
  EXPECT_FALSE(Model->takeGuestCall());
  forwardAndFinish(IRP);
  reject(Model->call("PoStartNextPowerIrp", {IRP}), "owned live power IRP");
  const auto Next = take(Model->beginRequest(input()));
  // No prior or next PoStartNext call is required by the Vista+ contract.
  forwardAndFinish(Next.IRP);
}

TEST_F(KernelPowerRequest, DpcMayNotifyD0ButCannotNotifyD3) {
  initialize();
  const uint64_t Dpc = Scratch + 0x200;
  call("KeInitializeDpc", {Dpc, Entry, 0});
  EXPECT_EQ(call("KeInsertQueueDpc", {Dpc, 0, 0}), 1u);
  const auto Scheduled = take(Model->nextScheduled(false));
  ASSERT_TRUE(Scheduled);
  ASSERT_EQ(Scheduled->IRQL, scheduler::DispatchLevel);
  EXPECT_EQ(call("PoSetPowerState", {FDO, 1, 1}), 4u);
  reject(Model->call("PoSetPowerState", {FDO, 1, 4}), "APC_LEVEL");
  EXPECT_EQ(reported(FDO), DevicePowerState::D0);
  EXPECT_EQ(Result.PnpDevices[0].DevicePower, DevicePowerState::D0);
  ok(Model->finishScheduled(Scheduled->ID));
  EXPECT_EQ(Model->currentIRQL(), scheduler::PassiveLevel);
  EXPECT_EQ(call("PoSetPowerState", {FDO, 1, 4}), 1u);
  EXPECT_EQ(reported(FDO), DevicePowerState::D3);
  EXPECT_EQ(Result.PnpDevices[0].DevicePower, DevicePowerState::D0);
}
} // namespace
} // namespace neverd::emulation
