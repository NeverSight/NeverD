//===- KernelFrameworkInterruptTests.cpp - Interrupt object power ownership
//===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelFrameworkTestSupport.h"
#include "windows/KernelScheduler.h"

namespace neverd::emulation {
namespace {
using namespace framework_test;
using namespace framework;

class DriverKernelFrameworkInterrupt : public DriverKernelFramework {
protected:
  static constexpr uint64_t PDO = Driver + 0x3000;
  static constexpr uint64_t FDO = Driver + 0x3100;
  static constexpr uint64_t InitSlot = Driver + 0x3200;
  static constexpr uint64_t DeviceSlot = Driver + 0x3208;
  static constexpr uint64_t PnpConfig = Driver + 0x3300;
  static constexpr uint64_t InterruptConfig = Driver + 0x3400;
  static constexpr uint64_t InterruptSlot = Driver + 0x3500;
  static constexpr uint64_t InterruptInfo = Driver + 0x3600;
  static constexpr uint64_t ISR = 0x180003000;
  static constexpr uint64_t Enable = 0x180003100;
  static constexpr uint64_t Disable = 0x180003200;
  static constexpr uint64_t DPC = 0x180003300;
  static constexpr uint64_t Before = 0x180004000;
  static constexpr uint64_t After = 0x180004100;
  static constexpr uint64_t Exit = 0x180004200;
  static constexpr uint64_t Release = 0x180004300;
  uint64_t Device = 0, NextConnection = 0x400000;
  unsigned Assigned = 1;
  std::set<uint64_t> Connected;
  std::map<uint64_t, uint64_t> Deferred;
  std::vector<std::pair<uint64_t, std::vector<uint64_t>>> Calls;

  void SetUp() override {
    DriverKernelFramework::SetUp();
    KernelFramework::DeviceHost Devices;
    Devices.Create =
        [](llvm::StringRef, uint32_t,
           bool) -> llvm::Expected<KernelFramework::DeviceCreation> {
      return failure("this fixture only creates a PnP FDO");
    };
    Devices.CreatePnp =
        [](uint64_t, llvm::StringRef, uint32_t, uint32_t, bool,
           bool) -> llvm::Expected<KernelFramework::DeviceCreation> {
      return KernelFramework::DeviceCreation{0, FDO};
    };
    Devices.Delete = [](uint64_t) { return llvm::Error::success(); };
    Devices.FinishInitializing = [](uint64_t) {
      return llvm::Error::success();
    };
    Model.setDeviceHost(std::move(Devices));
    bind();
    put(Config + DriverConfigAddDevice, Before);
    put(Config + DriverConfigFlags, 0, 4);
    createDriver();
    const auto Add = take(Model.beginPnpAddDevice(PDO));
    put(InitSlot, Add.Init);
    put(PnpConfig, PnpPowerCallbacksSize, 4);
    put(PnpConfig + PnpPowerCallbacksFirstOffset, Before);
    put(PnpConfig + PnpPowerCallbacksFirstOffset + 8, After);
    put(PnpConfig + PnpPowerCallbacksFirstOffset + 16, Exit);
    put(PnpConfig + PnpPowerCallbacksFirstOffset + 40, Release);
    take(invoke(api::WdfDeviceInitSetPnpPowerEventCallbacks,
                {Globals, Add.Init, PnpConfig}));
    EXPECT_EQ(
        take(invoke(api::WdfDeviceCreate, {Globals, InitSlot, 0, DeviceSlot})),
        0u);
    Device = get(DeviceSlot);
    success(Model.finishPnpAddDevice(PDO, Add.Init, 0));
    KernelFramework::InterruptHost Host;
    Host.Describe = [this](const KernelFramework::InterruptSelection &S)
        -> llvm::Expected<std::optional<KernelFramework::InterruptConnection>> {
      if (S.Ordinal >= Assigned)
        return std::optional<KernelFramework::InterruptConnection>{};
      KernelFramework::InterruptConnection Info;
      Info.Vector = 65;
      Info.Affinity = 1;
      Info.IRQL = S.Passive ? 0 : 5;
      return std::optional<KernelFramework::InterruptConnection>{Info};
    };
    Host.Connect = [this, Describe = Host.Describe](const auto &Selection,
                                                    uint64_t, uint64_t) mutable
        -> llvm::Expected<std::optional<KernelFramework::InterruptConnection>> {
      auto Result = Describe(Selection);
      if (!Result)
        return Result.takeError();
      if (*Result) {
        (*Result)->Token = NextConnection++;
        Connected.insert((*Result)->Token);
      }
      return Result;
    };
    Host.Disconnect = [this](uint64_t Object) {
      EXPECT_EQ(Connected.erase(Object), 1u);
      return llvm::Error::success();
    };
    Host.PrepareCall =
        [this](uint64_t Object, uint64_t PC, llvm::ArrayRef<uint64_t> Arguments,
               uint64_t Token) -> llvm::Expected<GuestCallToken> {
      EXPECT_TRUE(Connected.count(Object));
      Calls.emplace_back(PC, Arguments.vec());
      return GuestCallToken{GuestCallOwner::Interrupt, Token};
    };
    Host.QueueDeferred = [this](uint64_t Handle, uint64_t, uint64_t,
                                llvm::ArrayRef<uint64_t>, bool,
                                uint64_t Token) -> llvm::Expected<bool> {
      return Deferred.emplace(Handle, Token).second;
    };
    Host.HasDeferred = [this](uint64_t Handle) {
      return Deferred.count(Handle);
    };
    Model.setInterruptHost(std::move(Host));
    configureInterrupt();
  }

  void configureInterrupt(bool PowerCallbacks = true) {
    success(Memory.write(InterruptConfig,
                         std::vector<uint8_t>(InterruptConfigSize)));
    put(InterruptConfig, InterruptConfigSize, 4);
    put(InterruptConfig + InterruptShareVector, InterruptTriDefault, 4);
    put(InterruptConfig + InterruptReportInactive, InterruptTriDefault, 4);
    put(InterruptConfig + InterruptISR, ISR);
    put(InterruptConfig + InterruptDPC, DPC);
    if (PowerCallbacks) {
      put(InterruptConfig + InterruptEnable, Enable);
      put(InterruptConfig + InterruptDisable, Disable);
    }
  }
  llvm::Expected<uint64_t> create() {
    return invoke(api::WdfInterruptCreate,
                  {Globals, Device, InterruptConfig, 0, InterruptSlot});
  }
  uint64_t interrupt() {
    EXPECT_EQ(take(create()), 0u);
    return get(InterruptSlot);
  }
  void start() {
    EXPECT_TRUE(take(Model.beginPnpPowerTransition(
        PDO, InitSlot, DevicePnpRequest::Start, 0, 0, 0)));
  }
  void expectCallback(uint64_t PC, uint64_t Status = 0) {
    const auto Call = callback();
    EXPECT_EQ(Call.PC, PC);
    take(Model.finishGuestCall(Call.Token, Status));
  }
};

TEST_F(DriverKernelFrameworkInterrupt,
       AutomaticPowerCallbacksBracketD0AndResources) {
  const auto Handle = interrupt();
  start();
  expectCallback(Before);
  const auto Call = callback();
  EXPECT_EQ(Call.PC, Enable);
  EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Handle, Device}));
  ASSERT_TRUE(Call.ExecutionToken);
  EXPECT_EQ(Call.ExecutionToken->Owner, GuestCallOwner::Interrupt);
  take(Model.finishGuestCall(Call.Token, 0));
  expectCallback(After);
  ASSERT_TRUE(Model.takePnpCompletion());
  ASSERT_EQ(Connected.size(), 1u);
  EXPECT_TRUE(take(Model.beginPnpPowerTransition(
      PDO, InitSlot, DevicePnpRequest::Stop, 0, 0, 0)));
  expectCallback(Disable);
  EXPECT_TRUE(Connected.empty());
  expectCallback(Exit);
  expectCallback(Release);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(take(invoke(api::WdfInterruptWdmGetInterrupt, {Globals, Handle})),
            0u);
}

TEST_F(DriverKernelFrameworkInterrupt,
       EnableFailureUnwindsPriorInterruptsAndPreservesStatus) {
  Assigned = 2;
  interrupt();
  interrupt();
  start();
  expectCallback(Before);
  expectCallback(Enable);
  expectCallback(Enable, windows::StatusUnsuccessful);
  expectCallback(Disable);
  EXPECT_TRUE(Connected.empty());
  expectCallback(Exit);
  expectCallback(Release);
  const auto Done = Model.takePnpCompletion();
  ASSERT_TRUE(Done);
  EXPECT_EQ(Done->Status, windows::StatusUnsuccessful);
  EXPECT_FALSE(Model.hasPendingGuestCall());
}

TEST_F(DriverKernelFrameworkInterrupt,
       InterruptWithoutPowerCallbacksStillConnectsAndDisconnects) {
  configureInterrupt(false);
  interrupt();
  start();
  expectCallback(Before);
  EXPECT_EQ(Connected.size(), 1u);
  expectCallback(After);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_TRUE(take(Model.beginPnpPowerTransition(
      PDO, InitSlot, DevicePnpRequest::Stop, 0, 0, 0)));
  EXPECT_TRUE(Connected.empty());
  expectCallback(Exit);
  expectCallback(Release);
  ASSERT_TRUE(Model.takePnpCompletion());
}

TEST_F(DriverKernelFrameworkInterrupt, ExcessObjectsRemainUnassigned) {
  interrupt();
  const auto Extra = interrupt();
  start();
  expectCallback(Before);
  expectCallback(Enable);
  expectCallback(After);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(Connected.size(), 1u);
  EXPECT_EQ(take(invoke(api::WdfInterruptWdmGetInterrupt, {Globals, Extra})),
            0u);
}

TEST_F(DriverKernelFrameworkInterrupt,
       DeferredCallbackCoalescesAndDrainsBeforeD0Exit) {
  const auto Handle = interrupt();
  start();
  drain();
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(take(invoke(api::WdfInterruptQueueDpcForIsr, {Globals, Handle}, 5)),
            1u);
  EXPECT_EQ(take(invoke(api::WdfInterruptQueueDpcForIsr, {Globals, Handle}, 5)),
            0u);
  const auto Token = Deferred.at(Handle);
  EXPECT_TRUE(take(Model.beginPnpPowerTransition(
      PDO, InitSlot, DevicePnpRequest::Stop, 0, 0, 0)));
  expectCallback(Disable);
  EXPECT_FALSE(Model.hasPendingGuestCall());
  EXPECT_FALSE(Model.takePnpCompletion());
  // The framework retains its reference until return; the scheduler then
  // retires its independent active callback before resuming the power IRP.
  take(Model.finishGuestCall(Token, Sentinel));
  EXPECT_FALSE(Model.hasPendingGuestCall());
  Deferred.erase(Handle);
  success(Model.resumeInterruptDrain());
  expectCallback(Exit);
  expectCallback(Release);
  ASSERT_TRUE(Model.takePnpCompletion());
}

TEST_F(DriverKernelFrameworkInterrupt,
       InvalidConfigurationDoesNotPublishAnObject) {
  put(InterruptConfig + InterruptISR, 0);
  put(InterruptSlot, Sentinel);
  const auto Live = liveAllocations();
  EXPECT_EQ(take(create()), InvalidParameter);
  EXPECT_EQ(get(InterruptSlot), Sentinel);
  EXPECT_EQ(liveAllocations(), Live);
  configureInterrupt();
  put(InterruptConfig + InterruptAutomaticSerialization, 1, 1);
  EXPECT_EQ(take(create()), IncompatibleExecutionLevel);
  EXPECT_EQ(liveAllocations(), Live);
}

TEST_F(DriverKernelFrameworkInterrupt,
       CreationAfterStartIsRejectedBeforeMutation) {
  start();
  drain();
  ASSERT_TRUE(Model.takePnpCompletion());
  const auto Live = liveAllocations();
  EXPECT_EQ(take(create()), InvalidDeviceState);
  EXPECT_EQ(liveAllocations(), Live);
}

TEST_F(DriverKernelFrameworkInterrupt, GetInfoEnforcesIRQLAndResourceLifetime) {
  const auto Handle = interrupt();
  put(InterruptInfo, InterruptInfoSize, 4);
  expectError(
      invoke(api::WdfInterruptGetInfo, {Globals, Handle, InterruptInfo}),
      "assigned hardware");
  start();
  drain();
  ASSERT_TRUE(Model.takePnpCompletion());
  take(invoke(api::WdfInterruptGetInfo, {Globals, Handle, InterruptInfo}, 2));
  EXPECT_EQ(get(InterruptInfo + InterruptInfoVector, 4), 65u);
  EXPECT_EQ(get(InterruptInfo + InterruptInfoIRQL, 1), 5u);
  expectError(
      invoke(api::WdfInterruptGetInfo, {Globals, Handle, InterruptInfo}, 5),
      "IRQL");
  EXPECT_EQ(take(invoke(api::WdfInterruptGetDevice, {Globals, Handle}, 5)),
            Device);
}

TEST_F(DriverKernelFrameworkInterrupt,
       SynchronizeReturnsOnlyActualBooleanByte) {
  const auto Handle = interrupt();
  start();
  drain();
  ASSERT_TRUE(Model.takePnpCompletion());
  take(invoke(api::WdfInterruptSynchronize, {Globals, Handle, ISR, Sentinel}));
  const auto Call = callback();
  EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Handle, Sentinel}));
  const auto Result = take(Model.finishGuestCall(Call.Token, 0x123456ab));
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, 0xabu);
}

TEST_F(DriverKernelFrameworkInterrupt, ConnectedObjectCannotBeDeleted) {
  const auto Handle = interrupt();
  start();
  drain();
  ASSERT_TRUE(Model.takePnpCompletion());
  expectError(invoke(api::WdfObjectDelete, {Globals, Handle}),
              "disconnected and drained");
  EXPECT_EQ(take(invoke(api::WdfInterruptGetDevice, {Globals, Handle})),
            Device);
}
TEST_F(DriverKernelFrameworkInterrupt,
       DispatchDereferenceDefersPassiveDestroyAndRetainsItsContext) {
  type();
  attributes(Device, Type, 0, ChildDestroy);
  const auto Child = object(Attrs);
  const auto Context =
      take(invoke(api::WdfObjectGetTypedContextWorker, {Globals, Child, Type}));
  put(Context, Sentinel);
  take(invoke(api::WdfObjectReferenceActual, {Globals, Child, 0, 0, 0}));
  take(invoke(api::WdfObjectDelete, {Globals, Child}));
  EXPECT_FALSE(Model.hasPendingGuestCall());
  std::optional<KernelFramework::GuestCall> DeferredCall;
  KernelFramework::DeviceHost Host;
  Host.DeferCall = [&](const KernelFramework::GuestCall &Call,
                       uint64_t WdmDevice) {
    EXPECT_EQ(WdmDevice, FDO);
    DeferredCall = Call;
    return llvm::Error::success();
  };
  Model.setDeviceHost(std::move(Host));
  take(invoke(api::WdfObjectDereferenceActual, {Globals, Child, 0, 0, 0},
              scheduler::DispatchLevel));
  ASSERT_TRUE(DeferredCall);
  EXPECT_EQ(DeferredCall->PC, ChildDestroy);
  EXPECT_FALSE(Model.hasPendingGuestCall());
  EXPECT_EQ(get(Context), Sentinel);
  success(Model.validateGuestAccess(Context, 8, false));
  take(Model.finishGuestCall(DeferredCall->Token, 0));
  expectError(Model.validateGuestAccess(Context, 8, false), "freed");
}

} // namespace
} // namespace neverd::emulation
