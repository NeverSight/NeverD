//===- KernelFrameworkFileSendTests.cpp - KMDF file send ownership --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep a delayed synchronous CREATE owned by its lower target until wakeup.
//===----------------------------------------------------------------------===//

#include "KernelFrameworkTestSupport.h"

namespace neverd::emulation {
namespace {
using namespace framework_test;
namespace api = framework::api;

class DriverKernelFrameworkFileSend : public DriverKernelFramework {
protected:
  static constexpr uint64_t PDO = Driver + 0x3000;
  static constexpr uint64_t FDO = Driver + 0x3100;
  static constexpr uint64_t File = Driver + 0x3200;
  static constexpr uint64_t IRP = Driver + 0x3300;
  static constexpr uint64_t InitSlot = Driver + 0x3400;
  static constexpr uint64_t DeviceSlot = Driver + 0x3408;
  static constexpr uint64_t FileConfig = Driver + 0x3500;
  static constexpr uint64_t SendOptions = Driver + 0x3600;
  static constexpr uint64_t AddDevicePC = 0x180002000;
  static constexpr uint64_t FileCreatePC = 0x180003000;
  static constexpr uint64_t CompletionPC = 0x180004000;
  uint64_t Device = 0, Request = 0, Target = 0;
  uint64_t Information = 0;
  unsigned Sends = 0, Completed = 0;

  void SetUp() override {
    DriverKernelFramework::SetUp();
    KernelFramework::DeviceHost Devices;
    Devices.Create =
        [](llvm::StringRef, uint32_t,
           bool) -> llvm::Expected<KernelFramework::DeviceCreation> {
      return failure("control device is not part of this scenario");
    };
    Devices.CreatePnp =
        [](uint64_t Physical, llvm::StringRef, uint32_t, uint32_t, bool,
           bool) -> llvm::Expected<KernelFramework::DeviceCreation> {
      EXPECT_EQ(Physical, PDO);
      return KernelFramework::DeviceCreation{windows::StatusSuccess, FDO};
    };
    Devices.Delete = [](uint64_t) { return llvm::Error::success(); };
    Devices.FinishInitializing = [](uint64_t Address) {
      EXPECT_EQ(Address, FDO);
      return llvm::Error::success();
    };
    Model.setDeviceHost(std::move(Devices));
    bind();
    put(Config + framework::DriverConfigAddDevice, AddDevicePC);
    put(Config + framework::DriverConfigFlags, 0, sizeof(uint32_t));
    createDriver();
    const auto Add = take(Model.beginPnpAddDevice(PDO));
    EXPECT_EQ(Add.Callback, AddDevicePC);
    put(InitSlot, Add.Init);
    put(FileConfig, framework::FileConfigSize, sizeof(uint32_t));
    put(FileConfig + framework::FileCreateCallbackOffset, FileCreatePC);
    put(FileConfig + framework::FileAutoForwardOffset,
        framework::FileAutoForwardTrue, sizeof(uint32_t));
    put(FileConfig + framework::FileClassOffset,
        framework::FileObjectCannotUseFsContexts, sizeof(uint32_t));
    take(invoke(api::WdfDeviceInitSetFileObjectConfig,
                {Globals, Add.Init, FileConfig, 0}));
    attributes();
    put(Attrs + framework::AttributesExecution, framework::ExecutionPassive,
        sizeof(uint32_t));
    put(Attrs + framework::AttributesSynchronization,
        framework::SynchronizationNone, sizeof(uint32_t));
    EXPECT_EQ(take(invoke(api::WdfDeviceCreate,
                          {Globals, InitSlot, Attrs, DeviceSlot})),
              windows::StatusSuccess);
    Device = get(DeviceSlot);
    success(Model.finishPnpAddDevice(PDO, Add.Init, windows::StatusSuccess));
    KernelFramework::RequestHost Host;
    Host.View =
        [](uint64_t Packet) -> llvm::Expected<KernelFramework::RequestView> {
      EXPECT_EQ(Packet, IRP);
      return KernelFramework::RequestView{
          IRP, 0, framework::RequestMajorCreate, 0, 0, 0, false, 0, 0, File};
    };
    Host.MarkPending = [](uint64_t) { return llvm::Error::success(); };
    Host.ValidateFileForward = [](uint64_t Packet) {
      EXPECT_EQ(Packet, IRP);
      return llvm::Error::success();
    };
    Host.SendFileSynchronously =
        [this](uint64_t Packet,
               std::optional<int64_t> Timeout) -> llvm::Expected<uint32_t> {
      EXPECT_EQ(Packet, IRP);
      EXPECT_EQ(Timeout, -int64_t(100));
      ++Sends;
      return windows::StatusPending;
    };
    Host.Information = [this](uint64_t) -> llvm::Expected<uint64_t> {
      return Information;
    };
    Host.SetInformation = [this](uint64_t, uint64_t Value) {
      Information = Value;
      return llvm::Error::success();
    };
    Host.ValidateCompletion = [](uint64_t, uint32_t, uint64_t) {
      return llvm::Error::success();
    };
    Host.Complete = [this](uint64_t Packet, uint32_t Status, uint64_t Info) {
      EXPECT_EQ(Packet, IRP);
      EXPECT_EQ(Status, windows::StatusSuccess);
      EXPECT_EQ(Info, 0u);
      ++Completed;
      return llvm::Error::success();
    };
    Model.setRequestHost(std::move(Host));
    const auto Create = take(Model.routeRequest(FDO, IRP));
    ASSERT_TRUE(Create);
    ASSERT_EQ(Create->PC, FileCreatePC);
    ASSERT_EQ(Create->Arguments.size(), 3u);
    Request = Create->Arguments[1];
    Target = take(invoke(api::WdfDeviceGetIoTarget, {Globals, Device}));
    put(SendOptions, framework::RequestSendOptionsSize, sizeof(uint32_t));
    put(SendOptions + framework::RequestSendFlagsOffset,
        framework::RequestSendSynchronous | framework::RequestSendTimeout,
        sizeof(uint32_t));
    put(SendOptions + framework::RequestSendTimeoutOffset,
        uint64_t(-int64_t(100)));
  }
};

TEST_F(DriverKernelFrameworkFileSend,
       PendingSynchronousSendRetainsOwnershipUntilLowerCompletion) {
  EXPECT_EQ(take(invoke(api::WdfRequestSend,
                        {Globals, Request, Target, SendOptions})),
            1u);
  EXPECT_EQ(Model.synchronousFileSendPending(Request), true);
  EXPECT_EQ(Sends, 1u);
  const auto LiveBefore = liveAllocations();
  expectError(invoke(api::WdfRequestComplete, {Globals, Request, 0}),
              "synchronous");
  expectError(
      invoke(api::WdfRequestSend, {Globals, Request, Target, SendOptions}),
      "synchronous");
  expectError(
      invoke(api::WdfRequestFormatRequestUsingCurrentType, {Globals, Request}),
      "synchronous");
  expectError(invoke(api::WdfRequestSetCompletionRoutine,
                     {Globals, Request, CompletionPC, 0}),
              "synchronous");
  expectError(invoke(api::WdfRequestSetInformation, {Globals, Request, 7}),
              "synchronous");
  EXPECT_EQ(Completed, 0u);
  EXPECT_EQ(Sends, 1u);
  EXPECT_EQ(Information, 0u);
  EXPECT_EQ(liveAllocations(), LiveBefore);
  EXPECT_EQ(Model.synchronousFileSendPending(Request), true);
  EXPECT_FALSE(Model.takeGuestCall());
  success(Model.completeSynchronousFileSend(IRP, windows::StatusSuccess));
  EXPECT_EQ(Model.synchronousFileSendPending(Request), false);
  EXPECT_EQ(take(invoke(api::WdfRequestGetStatus, {Globals, Request})),
            windows::StatusSuccess);
  take(invoke(api::WdfRequestComplete,
              {Globals, Request, windows::StatusSuccess}));
  EXPECT_EQ(Completed, 1u);
  EXPECT_EQ(Model.synchronousFileSendPending(Request), std::nullopt);
}
} // namespace
} // namespace neverd::emulation
