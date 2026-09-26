//===- KernelPnpCompletionTests.cpp - Provider response continuations -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify explicit bus responses, virtual deadlines and real WDM completion
/// continuations independently from guest instruction execution.
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

class KernelPnpCompletion : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t DispatchPC = 0x180001000;
  static constexpr uint64_t CompletionPC = 0x180001100;
  static constexpr uint32_t Pending = 0x103;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  uint64_t PDO = 0, FDO = 0;

  void success(llvm::Error E) {
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
  template <class T>
  void rejected(llvm::Expected<T> Value, llvm::StringRef Message) {
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find(Message.str()),
              std::string::npos);
  }
  uint64_t get(uint64_t Address, unsigned Width = 8) {
    return take(Memory->readInteger(Address, Width));
  }
  void put(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    success(Model->validateGuestAccess(Address, Width, true));
    success(Memory->writeInteger(Address, Value, Width));
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Arguments) {
    return take(Model->call(Name, Arguments));
  }
  void SetUp() override {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    success(Memory->map(Scratch, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = DispatchPC;
    Image.Size = 0x3000;
    DriverOptions Options;
    DriverPnpDevice Device;
    Device.ID = "device0";
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    Options.PnpDevices.push_back(Device);
    success(Model->initialize(Image, Options));
    const uint64_t Extension = get(Model->driverObject() + DriverExtensionOffset);
    put(Extension + DriverAddDeviceOffset, DispatchPC);
    put(Model->driverObject() + DriverDispatchOffset + 0x1b * 8, DispatchPC);
    success(Model->finishEntry());
    success(Model->preparePnpDevices());
    auto Add = take(Model->beginAddDevice("device0"));
    PDO = Add.Argument1;
    ASSERT_NE(PDO, 0u);
    EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 16, 0,
                                      UnknownDeviceType, 0, 0, Scratch}), 0u);
    FDO = get(Scratch);
    put(FDO + DeviceFlagsOffset, DeviceBufferedIO, 4);
    EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {FDO, PDO}), PDO);
    success(Model->finishAddDevice("device0", 0));
  }
  uint64_t begin(uint64_t Delay, uint32_t Status = 0, bool Handler = true,
                 DevicePnpRequest Minor = DevicePnpRequest::Start) {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Pnp;
    Request.DeviceID = "device0";
    Request.Pnp = DriverPnpOperation{};
    Request.Pnp->Minor = Minor;
    Request.Pnp->BusCompletion.Status = Status;
    Request.Pnp->BusCompletion.Delay100ns = Delay;
    auto Invoke = take(Model->beginRequest(Request));
    EXPECT_EQ(Invoke.Argument0, FDO);
    const uint64_t IRP = Invoke.IRP;
    const uint64_t Top = get(IRP + IRPStackPointerOffset);
    std::vector<uint8_t> Prefix(StackCompletionOffset);
    success(Model->validateGuestAccess(Top, Prefix.size(), false));
    success(Memory->read(Top, Prefix));
    success(Model->validateGuestAccess(Top - StackSize, Prefix.size(), true));
    success(Memory->write(Top - StackSize, Prefix));
    put(Top - StackSize + StackControlOffset,
        Handler ? StackInvokeOnSuccess | StackInvokeOnError : 0, 1);
    if (Handler) {
      put(Top - StackSize + StackCompletionOffset, CompletionPC);
      put(Top - StackSize + StackCompletionContextOffset, Scratch);
    }
    return IRP;
  }
  void send(uint64_t IRP, uint32_t Expected) {
    EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), Expected);
  }
  const DriverRequestResult &observation() { return Result.Requests.back(); }
  KernelScheduler::Invocation delivery() {
    auto Call = take(Model->nextScheduled(true));
    if (!Call)
      Call = take(Model->nextScheduled(false));
    EXPECT_TRUE(Call);
    if (!Call)
      return {};
    EXPECT_EQ(Call->Kind, KernelScheduler::CallbackKind::WDMCompletion);
    EXPECT_EQ(Call->IRQL, 0u);
    EXPECT_EQ(Call->Owner, PDO);
    EXPECT_EQ(Call->PC, CompletionPC);
    return *Call;
  }
  void finish(uint64_t ID, uint32_t Return = 0) {
    EXPECT_FALSE(take(Model->continueScheduled(ID, Return)));
    success(Model->finishScheduled(ID));
  }
  void completePnp(DevicePnpRequest Minor) {
    const uint64_t IRP = begin(0, StatusSuccess, false, Minor);
    send(IRP, StatusSuccess);
    success(Model->recordDispatchReturn(IRP, StatusSuccess));
    success(Model->finalizeRequest(IRP));
  }
  void rejectedUpperStatus(DevicePnpRequest Minor, uint32_t Status,
                            llvm::StringRef Message) {
    completePnp(DevicePnpRequest::Start);
    if (Minor == DevicePnpRequest::Stop ||
        Minor == DevicePnpRequest::CancelStop)
      completePnp(DevicePnpRequest::QueryStop);
    const uint64_t IRP = begin(13, StatusSuccess, true, Minor);
    send(IRP, Pending);
    success(Model->recordDispatchReturn(IRP, Pending));
    const auto Call = delivery();
    const auto State = observation().Pnp->StateAfter;
    const uint64_t Cursor = get(IRP + IRPStackPointerOffset);
    call("IoMarkIrpPending", {IRP});
    put(IRP + IRPStatusOffset, Status, 4);
    rejected(Model->continueScheduled(Call.ID, 0), Message);
    EXPECT_EQ(get(IRP + IRPStackPointerOffset), Cursor);
    EXPECT_EQ(get(IRP + IRPStatusOffset, 4), Status);
    EXPECT_FALSE(observation().Completed);
    EXPECT_TRUE(Model->requestPending(IRP));
    EXPECT_EQ(observation().Pnp->BusStatus, StatusSuccess);
    EXPECT_EQ(observation().Pnp->BusCompletedAt100ns, 13u);
    success(Model->validateGuestAccess(IRP + IRPStatusOffset, 4, false));
    success(Model->snapshot());
    EXPECT_EQ(Result.PnpDevices.front().PnpState, State);
  }
};

TEST_F(KernelPnpCompletion, DPCForwardCannotAdvanceThePnPCursor) {
  const uint64_t IRP = begin(0);
  const uint64_t Cursor = get(IRP + IRPStackPointerOffset);
  const uint64_t Dpc = Scratch + 0x200;
  call("KeInitializeDpc", {Dpc, DispatchPC, 0});
  EXPECT_EQ(call("KeInsertQueueDpc", {Dpc, 0, 0}), 1u);
  const auto Active = take(Model->nextScheduled(false));
  ASSERT_TRUE(Active);
  EXPECT_EQ(Active->Kind, KernelScheduler::CallbackKind::DPC);
  EXPECT_EQ(Active->IRQL, scheduler::DispatchLevel);
  rejected(Model->call("IofCallDriver", {PDO, IRP}),
           "IRQL below DISPATCH_LEVEL");
  EXPECT_EQ(get(IRP + IRPStackPointerOffset), Cursor);
  EXPECT_FALSE(observation().Pnp->BusReceivedAt100ns);
  EXPECT_FALSE(observation().Pnp->BusCompletedAt100ns);
  EXPECT_FALSE(Model->takeGuestCall());
  success(Model->finishScheduled(Active->ID));
  send(IRP, 0);
  const auto Complete = Model->takeGuestCall();
  ASSERT_TRUE(Complete);
  EXPECT_EQ(take(Model->finishGuestCall(Complete->Token, 0)),
            std::optional<uint64_t>(0));
  success(Model->recordDispatchReturn(IRP, 0));
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPnpCompletion, SynchronousBusStatusSurvivesChangedGuestIoStatus) {
  const uint32_t Failure = 0xc0000001;
  const uint64_t IRP = begin(0, Failure);
  send(IRP, Failure);
  auto Complete = Model->takeGuestCall();
  ASSERT_TRUE(Complete);
  EXPECT_EQ(Complete->Token.Owner, GuestCallOwner::WDM);
  EXPECT_EQ(Complete->Arguments, (std::vector<uint64_t>{FDO, IRP, Scratch}));
  EXPECT_EQ(observation().Pnp->BusStatus, Failure);
  EXPECT_EQ(observation().Pnp->BusReceivedAt100ns, 0u);
  EXPECT_EQ(observation().Pnp->BusCompletedAt100ns, 0u);
  put(IRP + IRPStatusOffset, 0, 4);
  EXPECT_EQ(take(Model->finishGuestCall(Complete->Token, 0)),
            std::optional<uint64_t>(Failure));
  EXPECT_EQ(observation().IOStatus, 0u);
  success(Model->recordDispatchReturn(IRP, Failure));
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPnpCompletion, DeadlineBeginsAtProviderReceiptAndPreservesPending) {
  EXPECT_FALSE(take(Model->nextScheduled(true, 37)));
  const uint64_t IRP = begin(11);
  send(IRP, Pending);
  EXPECT_EQ(observation().Pnp->BusReceivedAt100ns, 37u);
  EXPECT_FALSE(observation().Pnp->BusStatus);
  EXPECT_EQ(Model->nextEventTime(), 48u);
  EXPECT_FALSE(Model->takeGuestCall());
  success(Model->recordDispatchReturn(IRP, Pending));
  rejected(Model->call("IofCompleteRequest", {IRP, 0}), "provider still owns");
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  auto Call = delivery();
  EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{FDO, IRP, Scratch}));
  EXPECT_EQ(observation().Pnp->BusCompletedAt100ns, 48u);
  EXPECT_EQ(observation().Pnp->BusStatus, 0u);
  EXPECT_FALSE(observation().Completed);
  call("IoMarkIrpPending", {IRP});
  finish(Call.ID);
  EXPECT_TRUE(observation().Completed);
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPnpCompletion, DeadlineWithoutGuestCallbackCompletesWithoutDispatch) {
  const uint64_t IRP = begin(5, 0, false);
  send(IRP, Pending);
  success(Model->recordDispatchReturn(IRP, Pending));
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_TRUE(observation().Completed);
  EXPECT_EQ(observation().Pnp->BusCompletedAt100ns, 5u);
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPnpCompletion, ScheduledMPRKeepsThePacketUntilLaterCompletion) {
  const uint64_t IRP = begin(10);
  send(IRP, Pending);
  success(Model->recordDispatchReturn(IRP, Pending));
  auto Call = delivery();
  call("IoMarkIrpPending", {IRP});
  finish(Call.ID, StatusMoreProcessingRequired);
  EXPECT_FALSE(observation().Completed);
  EXPECT_TRUE(Model->requestPending(IRP));
  EXPECT_FALSE(Model->nextEventTime());
  success(Model->validateGuestAccess(IRP + IRPStatusOffset, 4, false));
  call("IofCompleteRequest", {IRP, 0});
  EXPECT_TRUE(observation().Completed);
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPnpCompletion, ScheduledNestedCompletionThenMPRDoesNotReadRetiredIRP) {
  const uint64_t IRP = begin(10);
  send(IRP, Pending);
  success(Model->recordDispatchReturn(IRP, Pending));
  auto Call = delivery();
  call("IoMarkIrpPending", {IRP});
  call("IofCompleteRequest", {IRP, 0});
  EXPECT_TRUE(observation().Completed);
  success(Memory->write(IRP, std::vector<uint8_t>(IRPSize + 2 * StackSize, 0xee)));
  finish(Call.ID, StatusMoreProcessingRequired);
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPnpCompletion, WaitingCompletionResumesAfterWorkerWithTheSameIdentity) {
  const uint64_t IRP = begin(10);
  send(IRP, Pending);
  success(Model->recordDispatchReturn(IRP, Pending));
  auto Completion = delivery();
  const uint64_t Item = call("IoAllocateWorkItem", {FDO});
  call("IoQueueWorkItem", {Item, DispatchPC, profile::DelayedWorkQueue, Scratch});
  put(Scratch + 0x100, uint64_t(-5));
  call("KeDelayExecutionThread", {0, 0, Scratch + 0x100});
  auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  success(Model->suspendScheduled(Completion.ID));
  auto Worker = take(Model->nextScheduled(false));
  ASSERT_TRUE(Worker);
  EXPECT_EQ(Worker->Kind, KernelScheduler::CallbackKind::WorkItem);
  call("IoFreeWorkItem", {Item});
  finish(Worker->ID);
  EXPECT_FALSE(take(Model->nextScheduled(true, Wait->Deadline)));
  EXPECT_EQ(take(Model->pollWait(*Wait)), std::optional<uint32_t>(0));
  success(Model->resumeScheduled(Completion.ID));
  EXPECT_EQ(Model->currentIRQL(), 0u);
  call("IoMarkIrpPending", {IRP});
  finish(Completion.ID);
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPnpCompletion, FullCallbackCapacityCannotPublishBusCompletionEffects) {
  const uint64_t IRP = begin(10);
  send(IRP, Pending);
  success(Model->recordDispatchReturn(IRP, Pending));
  for (unsigned I = 0; I < scheduler::DefaultMaxPendingCallbacks; ++I) {
    const uint64_t Item = call("IoAllocateWorkItem", {FDO});
    call("IoQueueWorkItem", {Item, DispatchPC, profile::DelayedWorkQueue, Scratch});
    auto Worker = take(Model->nextScheduled(false));
    ASSERT_TRUE(Worker);
    success(Model->suspendScheduled(Worker->ID));
  }
  const uint64_t Pointer = get(IRP + IRPStackPointerOffset);
  const uint64_t Status = get(IRP + IRPStatusOffset, 4);
  rejected(Model->nextScheduled(true), "pending callback limit");
  EXPECT_EQ(get(IRP + IRPStackPointerOffset), Pointer);
  EXPECT_EQ(get(IRP + IRPStatusOffset, 4), Status);
  EXPECT_FALSE(observation().Pnp->BusStatus);
  EXPECT_FALSE(observation().Pnp->BusCompletedAt100ns);
  EXPECT_EQ(Model->nextEventTime(), 10u);
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_FALSE(observation().Completed);
}

TEST_F(KernelPnpCompletion, DelayedQueryStopFailureRollsBackAtUpperCompletion) {
  completePnp(DevicePnpRequest::Start);
  constexpr uint32_t Failure = 0xc0000001;
  const uint64_t IRP = begin(17, Failure, true, DevicePnpRequest::QueryStop);
  EXPECT_EQ(observation().Pnp->StateAfter, DevicePnpState::StopPending);
  send(IRP, Pending);
  success(Model->recordDispatchReturn(IRP, Pending));
  const auto Call = delivery();
  EXPECT_EQ(observation().Pnp->BusStatus, Failure);
  EXPECT_EQ(observation().Pnp->BusCompletedAt100ns, 17u);
  EXPECT_EQ(observation().Pnp->StateAfter, DevicePnpState::StopPending);
  EXPECT_FALSE(observation().Completed);
  call("IoMarkIrpPending", {IRP});
  finish(Call.ID);
  EXPECT_EQ(observation().IOStatus, Failure);
  EXPECT_EQ(observation().DispatchStatus, Pending);
  EXPECT_EQ(observation().Pnp->StateAfter, DevicePnpState::Started);
  success(Model->finalizeRequest(IRP));
  EXPECT_EQ(Result.PnpDevices.front().PnpState, DevicePnpState::Started);
}

TEST_F(KernelPnpCompletion, DelayedQueryStopUpperFailureOverridesBusSuccess) {
  completePnp(DevicePnpRequest::Start);
  constexpr uint32_t Failure = 0xc0000001;
  const uint64_t IRP = begin(11, StatusSuccess, true, DevicePnpRequest::QueryStop);
  send(IRP, Pending);
  success(Model->recordDispatchReturn(IRP, Pending));
  const auto Call = delivery();
  put(IRP + IRPStatusOffset, Failure, 4);
  call("IoMarkIrpPending", {IRP});
  finish(Call.ID);
  EXPECT_EQ(observation().Pnp->BusStatus, StatusSuccess);
  EXPECT_EQ(observation().IOStatus, Failure);
  EXPECT_EQ(observation().Pnp->StateAfter, DevicePnpState::Started);
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPnpCompletion, DelayedStopRejectsNonzeroUpperSuccessBeforePop) {
  rejectedUpperStatus(DevicePnpRequest::Stop, 1, "STATUS_SUCCESS");
}

TEST_F(KernelPnpCompletion, DelayedCancelStopRejectsNonzeroUpperSuccessBeforePop) {
  rejectedUpperStatus(DevicePnpRequest::CancelStop, 1, "STATUS_SUCCESS");
}

TEST_F(KernelPnpCompletion, DelayedSurpriseRejectsNonzeroUpperSuccessBeforePop) {
  rejectedUpperStatus(DevicePnpRequest::SurpriseRemoval, 1, "STATUS_SUCCESS");
}

TEST_F(KernelPnpCompletion, UpperQueryStopResourceRequeryCannotCommitOrPop) {
  rejectedUpperStatus(DevicePnpRequest::QueryStop,
                      StatusResourceRequirementsChanged,
                      "unsupported resource requery");
}

TEST_F(KernelPnpCompletion, DelayedCancelStopMPRRetainsStopPendingUntilRecomplete) {
  completePnp(DevicePnpRequest::Start);
  completePnp(DevicePnpRequest::QueryStop);
  const uint64_t IRP = begin(19, StatusSuccess, true, DevicePnpRequest::CancelStop);
  send(IRP, Pending);
  success(Model->recordDispatchReturn(IRP, Pending));
  const auto Call = delivery();
  call("IoMarkIrpPending", {IRP});
  finish(Call.ID, StatusMoreProcessingRequired);
  EXPECT_EQ(observation().Pnp->BusStatus, StatusSuccess);
  EXPECT_EQ(observation().Pnp->BusCompletedAt100ns, 19u);
  EXPECT_EQ(observation().Pnp->StateAfter, DevicePnpState::StopPending);
  EXPECT_TRUE(Model->requestPending(IRP));
  EXPECT_FALSE(Model->nextEventTime());
  success(Model->snapshot());
  EXPECT_EQ(Result.PnpDevices.front().PnpState, DevicePnpState::StopPending);
  call("IofCompleteRequest", {IRP, 0});
  EXPECT_EQ(observation().Pnp->StateAfter, DevicePnpState::Started);
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPnpCompletion, DelayedSurprisePreservesDevicesUntilLaterRemove) {
  completePnp(DevicePnpRequest::Start);
  const uint64_t IRP =
      begin(11, StatusSuccess, true, DevicePnpRequest::SurpriseRemoval);
  send(IRP, Pending);
  success(Model->recordDispatchReturn(IRP, Pending));
  const auto Call = delivery();
  call("IoMarkIrpPending", {IRP});
  finish(Call.ID);
  success(Model->finalizeRequest(IRP));
  EXPECT_EQ(Result.PnpDevices.front().PnpState, DevicePnpState::SurpriseRemoved);
  EXPECT_TRUE(Result.PnpDevices.front().Attached);
  EXPECT_TRUE(Result.PnpDevices.front().ProviderPresent);
  success(Model->validateGuestAccess(FDO + DeviceExtensionOffset, 8, false));
  const uint64_t Remove = begin(7, StatusSuccess, false, DevicePnpRequest::Remove);
  send(Remove, Pending);
  call("IoDetachDevice", {PDO});
  call("IoDeleteDevice", {FDO});
  success(Model->recordDispatchReturn(Remove, Pending));
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  EXPECT_TRUE(observation().Completed);
  EXPECT_EQ(observation().Pnp->BusCompletedAt100ns, 18u);
  success(Model->finalizeRequest(Remove));
  EXPECT_EQ(Result.PnpDevices.front().PnpState, DevicePnpState::Removed);
  EXPECT_FALSE(Result.PnpDevices.front().ProviderPresent);
  EXPECT_TRUE(Result.Devices.empty());
}

TEST_F(KernelPnpCompletion, MismatchedRawMinorCannotConsumeConfiguredResponse) {
  const uint64_t IRP = begin(0);
  const uint64_t Next = get(IRP + IRPStackPointerOffset) - StackSize;
  put(Next + StackMinorOffset, uint8_t(DevicePnpRequest::Remove), 1);
  rejected(Model->call("IofCallDriver", {PDO, IRP}), "raw PnP minor");
  EXPECT_FALSE(observation().Pnp->BusReceivedAt100ns);
  EXPECT_FALSE(Model->takeGuestCall());
}

} // namespace
} // namespace neverd::emulation
