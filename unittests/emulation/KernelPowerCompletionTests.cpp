//===- KernelPowerCompletionTests.cpp - Child power IRP continuations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify real child request identity, terminal callbacks and snapshot lifetime
/// independently from instruction execution and the scenario foreground loop.
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

class KernelPowerCompletion : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t DispatchPC = 0x180001000;
  static constexpr uint64_t CompletionPC = 0x180001100;
  static constexpr uint64_t PowerPC = 0x180001200;
  static constexpr uint64_t WorkerPC = 0x180001300;
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
  void denied(uint64_t Address, uint32_t Size) {
    auto E = Model->validateGuestAccess(Address, Size, false);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
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
  DriverPowerOperation
  operation(DevicePowerState State, uint64_t Delay = 0,
            DevicePowerRequest Minor = DevicePowerRequest::Set,
            uint32_t Status = StatusSuccess) {
    DriverPowerOperation Power;
    Power.Minor = Minor;
    Power.State = uint32_t(State);
    Power.BusCompletion.Status = Status;
    Power.BusCompletion.Delay100ns = Delay;
    return Power;
  }
  void copyDown(uint64_t IRP, uint64_t Callback = 0) {
    const uint64_t Stack = get(IRP + IRPStackPointerOffset);
    std::vector<uint8_t> Prefix(StackCompletionOffset);
    success(Model->validateGuestAccess(Stack, Prefix.size(), false));
    success(Memory->read(Stack, Prefix));
    success(Model->validateGuestAccess(Stack - StackSize, Prefix.size(), true));
    success(Memory->write(Stack - StackSize, Prefix));
    put(Stack - StackSize + StackControlOffset,
        Callback ? StackInvokeOnSuccess | StackInvokeOnError : 0, 1);
    if (Callback) {
      put(Stack - StackSize + StackCompletionOffset, Callback);
      put(Stack - StackSize + StackCompletionContextOffset, Scratch + 0x200);
    }
  }
  void initialize(std::vector<DriverPowerOperation> Responses) {
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
    Device.ID = "power0";
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    Device.InitialReportedDevicePower = DevicePowerState::D0;
    Device.RequestedDevicePower = std::move(Responses);
    Options.PnpDevices.push_back(std::move(Device));
    success(Model->initialize(Image, Options));
    const uint64_t Extension =
        get(Model->driverObject() + DriverExtensionOffset);
    put(Extension + DriverAddDeviceOffset, DispatchPC);
    put(Model->driverObject() + DriverDispatchOffset + 0x1b * 8, DispatchPC);
    put(Model->driverObject() + DriverDispatchOffset + 0x16 * 8, DispatchPC);
    success(Model->finishEntry());
    success(Model->preparePnpDevices());
    const auto Add = take(Model->beginAddDevice("power0"));
    PDO = Add.Argument1;
    ASSERT_NE(PDO, 0u);
    EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 16, 0,
                                      UnknownDeviceType, 0, 0, Scratch}),
              0u);
    FDO = get(Scratch);
    put(FDO + DeviceFlagsOffset, DeviceBufferedIO | DevicePowerPageable, 4);
    EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {FDO, PDO}), PDO);
    success(Model->finishAddDevice("power0", 0));
    DriverRequest Start;
    Start.Kind = DriverRequestKind::Pnp;
    Start.DeviceID = "power0";
    Start.Pnp = DriverPnpOperation{};
    Start.Pnp->BusCompletion.Status = 0;
    const uint64_t IRP = take(Model->beginRequest(Start)).IRP;
    copyDown(IRP);
    EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), 0u);
    success(Model->recordDispatchReturn(IRP, 0));
    success(Model->finalizeRequest(IRP));
  }
  KernelGuestCall request(DevicePowerState State, uint64_t Callback = PowerPC,
                          uint64_t Device = 0,
                          DevicePowerRequest Minor = DevicePowerRequest::Set) {
    EXPECT_EQ(call("PoRequestPowerIrp",
                   {Device ? Device : FDO, uint8_t(Minor), uint32_t(State),
                    Callback, Scratch + 0x100, 0}),
              StatusPending);
    auto Call = Model->takeGuestCall();
    EXPECT_TRUE(Call);
    if (!Call)
      return {};
    EXPECT_EQ(Call->Token.Owner, GuestCallOwner::WDM);
    EXPECT_EQ(Call->PC, DispatchPC);
    EXPECT_EQ(Call->Arguments.front(), FDO);
    return *Call;
  }
  KernelGuestCall pendingCall(uint64_t ExpectedPC) {
    auto Call = Model->takeGuestCall();
    EXPECT_TRUE(Call);
    if (!Call)
      return {};
    EXPECT_EQ(Call->PC, ExpectedPC);
    return *Call;
  }
  void finishCall(const KernelGuestCall &Call, uint64_t ReturnValue,
                  uint64_t Expected) {
    auto Finished = take(Model->finishGuestCall(Call.Token, ReturnValue));
    ASSERT_TRUE(Finished);
    EXPECT_EQ(*Finished, Expected);
  }
  KernelScheduler::Invocation scheduled(uint64_t PC) {
    auto Next = take(Model->nextScheduled(true));
    if (!Next)
      Next = take(Model->nextScheduled(false));
    EXPECT_TRUE(Next);
    if (!Next)
      return {};
    EXPECT_EQ(Next->Kind, KernelScheduler::CallbackKind::WDMCompletion);
    EXPECT_EQ(Next->PC, PC);
    EXPECT_EQ(Next->IRQL, scheduler::PassiveLevel);
    return *Next;
  }
};

TEST_F(KernelPowerCompletion, SynchronousChildHasItsOwnResultAndVoidCallback) {
  initialize({operation(DevicePowerState::D3)});
  const auto Dispatch = request(DevicePowerState::D3);
  const uint64_t IRP = Dispatch.Arguments[1];
  ASSERT_EQ(Result.Requests.size(), 2u);
  EXPECT_NE(IRP, Result.Requests.front().IRP);
  EXPECT_EQ(Result.Requests.back().Origin,
            DriverRequestOrigin::PoRequestPowerIrp);
  EXPECT_EQ(Result.Requests.back().ResponseIndex, 0u);
  copyDown(IRP);
  EXPECT_EQ(call("PoCallDriver", {PDO, IRP}), 0u);
  const auto Completion = pendingCall(PowerPC);
  ASSERT_EQ(Completion.Arguments.size(), 5u);
  EXPECT_EQ(Completion.Arguments[0], FDO);
  EXPECT_EQ(Completion.Arguments[1], uint8_t(DevicePowerRequest::Set));
  EXPECT_EQ(Completion.Arguments[2], uint32_t(DevicePowerState::D3));
  EXPECT_EQ(Completion.Arguments[3], Scratch + 0x100);
  const uint64_t Snapshot = Completion.Arguments[4];
  EXPECT_NE(Snapshot, IRP + IRPStatusOffset);
  success(Model->validateGuestAccess(Snapshot, PowerStatusBlockSize, false));
  EXPECT_EQ(get(Snapshot, 4), 0u);
  EXPECT_EQ(get(Snapshot + PowerStatusBlockInformationOffset), 0u);
  denied(IRP + IRPStatusOffset, 4);
  EXPECT_TRUE(Result.Requests.back().Completed);
  EXPECT_FALSE(Result.Requests.back().DispatchStatus);
  finishCall(Completion, StatusMoreProcessingRequired, 0);
  denied(Snapshot, PowerStatusBlockSize);
  finishCall(Dispatch, 0, StatusPending);
  success(Model->finalizeRequest(IRP));
  EXPECT_EQ(Result.Requests.back().DispatchStatus, 0u);
  EXPECT_EQ(Result.Requests.back().IOStatus, 0u);
  EXPECT_EQ(Result.Requests.back().Power->DeviceStateAfter,
            DevicePowerState::D3);
}

TEST_F(KernelPowerCompletion,
       DelayedProviderSchedulesTerminalCallbackWithoutIoCompletion) {
  initialize({operation(DevicePowerState::D3, 17)});
  const auto Dispatch = request(DevicePowerState::D3, PowerPC, PDO);
  const uint64_t IRP = Dispatch.Arguments[1];
  copyDown(IRP);
  EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), StatusPending);
  finishCall(Dispatch, StatusPending, StatusPending);
  EXPECT_FALSE(Result.Requests.back().Completed);
  const auto Callback = scheduled(PowerPC);
  ASSERT_EQ(Callback.Arguments.size(), 5u);
  EXPECT_EQ(Callback.Arguments[0], PDO);
  EXPECT_EQ(Callback.DueTime100ns, 17u);
  EXPECT_EQ(Result.Requests.back().Power->BusReceivedAt100ns, 0u);
  EXPECT_EQ(Result.Requests.back().Power->BusCompletedAt100ns, 17u);
  denied(IRP, 1);
  const uint64_t Snapshot = Callback.Arguments[4];
  success(Model->validateGuestAccess(Snapshot, 16, false));
  EXPECT_FALSE(take(Model->continueScheduled(Callback.ID, UINT64_MAX)));
  success(Model->finishScheduled(Callback.ID));
  denied(Snapshot, 16);
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPowerCompletion,
       DelayedChildWithoutCallbackFinalizesAutomatically) {
  initialize({operation(DevicePowerState::D3, 9)});
  const auto Dispatch = request(DevicePowerState::D3, 0);
  const uint64_t IRP = Dispatch.Arguments[1];
  copyDown(IRP);
  EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), StatusPending);
  finishCall(Dispatch, StatusPending, StatusPending);
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  EXPECT_FALSE(Model->requestPending());
  EXPECT_TRUE(Result.Requests.back().Completed);
  EXPECT_EQ(Result.Requests.back().Power->BusCompletedAt100ns, 9u);
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPowerCompletion,
       DirectProviderChildHasNoSyntheticDispatchCallback) {
  initialize({operation(DevicePowerState::D3)});
  call("IoDetachDevice", {PDO});
  call("IoDeleteDevice", {FDO});
  EXPECT_EQ(call("PoRequestPowerIrp", {PDO, 2, 4, PowerPC, Scratch, 0}),
            StatusPending);
  const auto Completion = pendingCall(PowerPC);
  ASSERT_EQ(Completion.Arguments.size(), 5u);
  EXPECT_EQ(Completion.Arguments[0], PDO);
  EXPECT_EQ(Result.Requests.back().DispatchStatus, 0u);
  EXPECT_TRUE(Result.Requests.back().Completed);
  const uint64_t IRP = Result.Requests.back().IRP;
  finishCall(Completion, 0, StatusPending);
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPowerCompletion,
       ChildCallbackCanCompleteItsIndependentSystemParent) {
  auto ChildResponse = operation(DevicePowerState::D3, 5);
  ChildResponse.Action = DriverPowerAction::Sleep;
  initialize({ChildResponse});
  DriverRequest Input;
  Input.Kind = DriverRequestKind::Power;
  Input.DeviceID = "power0";
  Input.Power = DriverPowerOperation{};
  Input.Power->Type = DriverPowerType::System;
  Input.Power->State = uint32_t(SystemPowerState::Sleeping3);
  Input.Power->Action = DriverPowerAction::Sleep;
  Input.Power->BusCompletion.Status = 0;
  const uint64_t Parent = take(Model->beginRequest(Input)).IRP;
  copyDown(Parent, CompletionPC);
  EXPECT_EQ(call("IofCallDriver", {PDO, Parent}), 0u);
  const auto ParentCompletion = pendingCall(CompletionPC);
  call("IoMarkIrpPending", {Parent});
  EXPECT_EQ(call("PoRequestPowerIrp", {FDO, 2, 4, PowerPC, Parent, 0}),
            StatusPending);
  const auto ChildDispatch = pendingCall(DispatchPC);
  const uint64_t Child = ChildDispatch.Arguments[1];
  ASSERT_NE(Child, Parent);
  copyDown(Child);
  EXPECT_EQ(call("IofCallDriver", {PDO, Child}), StatusPending);
  finishCall(ChildDispatch, StatusPending, StatusPending);
  finishCall(ParentCompletion, StatusMoreProcessingRequired, 0);
  success(Model->recordDispatchReturn(Parent, StatusPending));
  const auto Completion = scheduled(PowerPC);
  EXPECT_EQ(Completion.Arguments[3], Parent);
  call("IofCompleteRequest", {Parent, 0});
  EXPECT_FALSE(Model->takeGuestCall());
  success(Model->finalizeRequest(Parent));
  success(Model->validateGuestAccess(Completion.Arguments[4], 16, false));
  EXPECT_FALSE(take(Model->continueScheduled(Completion.ID, 0)));
  success(Model->finishScheduled(Completion.ID));
  ASSERT_EQ(Result.Requests.size(), 3u);
  EXPECT_EQ(Result.Requests[1].Origin, DriverRequestOrigin::Scenario);
  EXPECT_EQ(Result.Requests[2].Origin, DriverRequestOrigin::PoRequestPowerIrp);
  EXPECT_EQ(Result.Requests[1].Power->SystemStateAfter,
            SystemPowerState::Sleeping3);
  EXPECT_EQ(Result.Requests[2].Power->DeviceStateAfter, DevicePowerState::D3);
  EXPECT_EQ(Result.Requests[1].IOStatus, 0u);
  EXPECT_EQ(Result.Requests[2].IOStatus, 0u);
  EXPECT_FALSE(Model->requestPending());
}

TEST_F(KernelPowerCompletion, TerminalCallbackCanIssueAnotherIndependentChild) {
  initialize(
      {operation(DevicePowerState::D3), operation(DevicePowerState::D0)});
  const auto First = request(DevicePowerState::D3);
  copyDown(First.Arguments[1]);
  EXPECT_EQ(call("IofCallDriver", {PDO, First.Arguments[1]}), 0u);
  const auto Completion = pendingCall(PowerPC);
  const uint64_t Snapshot = Completion.Arguments[4];
  const auto Second = request(DevicePowerState::D0, 0);
  EXPECT_NE(First.Arguments[1], Second.Arguments[1]);
  copyDown(Second.Arguments[1]);
  EXPECT_EQ(call("IofCallDriver", {PDO, Second.Arguments[1]}), 0u);
  finishCall(Second, 0, StatusPending);
  success(Model->validateGuestAccess(Snapshot, 16, false));
  finishCall(Completion, 0, 0);
  finishCall(First, 0, StatusPending);
  ASSERT_EQ(Result.Requests.size(), 3u);
  EXPECT_EQ(Result.Requests[1].ResponseIndex, 0u);
  EXPECT_EQ(Result.Requests[2].ResponseIndex, 1u);
  EXPECT_EQ(Result.Requests[1].Power->DeviceStateAfter, DevicePowerState::D3);
  EXPECT_EQ(Result.Requests[2].Power->DeviceStateBefore, DevicePowerState::D3);
  EXPECT_EQ(Result.Requests[2].Power->DeviceStateAfter, DevicePowerState::D0);
}

TEST_F(KernelPowerCompletion, MPRPreservesChildBeforeTerminalCallback) {
  initialize({operation(DevicePowerState::D3)});
  const auto Dispatch = request(DevicePowerState::D3);
  const uint64_t IRP = Dispatch.Arguments[1];
  copyDown(IRP, CompletionPC);
  EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), 0u);
  const auto IoCompletion = pendingCall(CompletionPC);
  call("IoMarkIrpPending", {IRP});
  finishCall(IoCompletion, StatusMoreProcessingRequired, 0);
  EXPECT_FALSE(Model->takeGuestCall());
  success(Model->validateGuestAccess(IRP + IRPStatusOffset, 4, false));
  EXPECT_FALSE(Result.Requests.back().Completed);
  finishCall(Dispatch, StatusPending, StatusPending);
  call("IofCompleteRequest", {IRP, 0});
  const auto Completion = pendingCall(PowerPC);
  EXPECT_TRUE(Result.Requests.back().Completed);
  finishCall(Completion, 0, 0);
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPowerCompletion, SnapshotRemainsLiveAcrossWaitAndWorker) {
  initialize({operation(DevicePowerState::D3, 5)});
  const auto Dispatch = request(DevicePowerState::D3);
  copyDown(Dispatch.Arguments[1]);
  EXPECT_EQ(call("IofCallDriver", {PDO, Dispatch.Arguments[1]}), StatusPending);
  finishCall(Dispatch, StatusPending, StatusPending);
  const auto Completion = scheduled(PowerPC);
  const uint64_t Snapshot = Completion.Arguments[4];
  const uint64_t Event = Scratch + 0x400;
  call("KeInitializeEvent", {Event, 0, 0});
  call("KeWaitForSingleObject", {Event, 0, 0, 0, 0});
  const auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  success(Model->suspendScheduled(Completion.ID));
  const uint64_t Item = call("IoAllocateWorkItem", {FDO});
  call("IoQueueWorkItem", {Item, WorkerPC, profile::DelayedWorkQueue, Event});
  const auto Next = take(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  EXPECT_EQ(Next->PC, WorkerPC);
  success(Model->validateGuestAccess(Snapshot, 16, false));
  call("KeSetEvent", {Event, 0, 0});
  const auto Ready = take(Model->pollWait(*Wait));
  ASSERT_TRUE(Ready);
  EXPECT_EQ(*Ready, StatusSuccess);
  call("IoFreeWorkItem", {Item});
  success(Model->finishScheduled(Next->ID));
  success(Model->resumeScheduled(Completion.ID));
  EXPECT_FALSE(take(Model->continueScheduled(Completion.ID, 0)));
  success(Model->finishScheduled(Completion.ID));
  denied(Snapshot, 16);
}

TEST_F(KernelPowerCompletion, SnapshotWritesDoNotRewriteCompletedPacketStatus) {
  initialize({operation(DevicePowerState::D3)});
  const auto Dispatch = request(DevicePowerState::D3);
  copyDown(Dispatch.Arguments[1]);
  call("IofCallDriver", {PDO, Dispatch.Arguments[1]});
  const auto Completion = pendingCall(PowerPC);
  put(Completion.Arguments[4], 0xc0000001, 4);
  put(Completion.Arguments[4] + 8, 123);
  EXPECT_EQ(Result.Requests.back().IOStatus, 0u);
  EXPECT_EQ(Result.Requests.back().Information, 0u);
  finishCall(Completion, 0, 0);
  finishCall(Dispatch, 0, StatusPending);
  EXPECT_EQ(Result.Requests.back().IOStatus, 0u);
  EXPECT_EQ(Result.Requests.back().Information, 0u);
}

TEST_F(KernelPowerCompletion,
       FailedLocalQueryStillCompletesCallbackAndPreservesBusNull) {
  initialize({operation(DevicePowerState::D3, 0, DevicePowerRequest::Query)});
  const auto Dispatch =
      request(DevicePowerState::D3, PowerPC, 0, DevicePowerRequest::Query);
  const uint64_t IRP = Dispatch.Arguments[1];
  put(IRP + IRPStatusOffset, 0xc0000001, 4);
  put(IRP + IRPInformationOffset, 0);
  call("IofCompleteRequest", {IRP, 0});
  const auto Completion = pendingCall(PowerPC);
  EXPECT_EQ(get(Completion.Arguments[4], 4), 0xc0000001u);
  EXPECT_FALSE(Result.Requests.back().Power->BusReceivedAt100ns);
  EXPECT_FALSE(Result.Requests.back().Power->BusCompletedAt100ns);
  finishCall(Completion, 0, 0);
  finishCall(Dispatch, 0xc0000001, StatusPending);
  EXPECT_EQ(Result.Requests.back().Power->DeviceStateAfter,
            DevicePowerState::D0);
}

TEST_F(KernelPowerCompletion,
       RejectedRequestsDoNotConsumeResponseOrCreateObservation) {
  initialize({operation(DevicePowerState::D3)});
  const size_t Count = Result.Requests.size();
  rejected(Model->call("PoRequestPowerIrp", {FDO, 2, 1, PowerPC, 0, 0}),
           "does not match");
  rejected(Model->call("PoRequestPowerIrp", {FDO, 2, 4, PowerPC, 0, Scratch}),
           "null output");
  EXPECT_EQ(call("PoRequestPowerIrp", {FDO, 255, 4, PowerPC, 0, 0}),
            StatusInvalidParameter2);
  EXPECT_EQ(Result.Requests.size(), Count);
  EXPECT_FALSE(Model->takeGuestCall());
  const auto Dispatch = request(DevicePowerState::D3, 0);
  copyDown(Dispatch.Arguments[1]);
  call("IofCallDriver", {PDO, Dispatch.Arguments[1]});
  finishCall(Dispatch, 0, StatusPending);
  EXPECT_EQ(Result.Requests.back().ResponseIndex, 0u);
  rejected(Model->call("PoRequestPowerIrp", {FDO, 2, 4, 0, 0, 0}), "exhausted");
}

TEST_F(KernelPowerCompletion,
       DPCRequestIsExplicitlyUnsupportedBeforeAnySideEffects) {
  initialize({operation(DevicePowerState::D3)});
  const uint64_t Dpc = Scratch + 0x600;
  call("KeInitializeDpc", {Dpc, WorkerPC, 0});
  EXPECT_EQ(call("KeInsertQueueDpc", {Dpc, 0, 0}), 1u);
  const auto Next = take(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  ASSERT_EQ(Next->IRQL, scheduler::DispatchLevel);
  rejected(Model->call("PoRequestPowerIrp", {FDO, 2, 4, PowerPC, 0, 0}),
           "PASSIVE_LEVEL");
  EXPECT_EQ(Result.Requests.size(), 1u);
  EXPECT_FALSE(Model->takeGuestCall());
  success(Model->finishScheduled(Next->ID));
  const auto Dispatch = request(DevicePowerState::D3, 0);
  copyDown(Dispatch.Arguments[1]);
  call("IofCallDriver", {PDO, Dispatch.Arguments[1]});
  finishCall(Dispatch, 0, StatusPending);
  EXPECT_EQ(Result.Requests.back().ResponseIndex, 0u);
}

TEST_F(KernelPowerCompletion,
       FullCapacityPreservesDelayedTerminalCallbackAndConsumedResponse) {
  initialize(
      {operation(DevicePowerState::D3, 11), operation(DevicePowerState::D0)});
  const auto Dispatch = request(DevicePowerState::D3);
  const uint64_t IRP = Dispatch.Arguments[1];
  copyDown(IRP);
  EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), StatusPending);
  finishCall(Dispatch, StatusPending, StatusPending);
  uint64_t LastWorker = 0, LastItem = 0;
  for (unsigned I = 0; I < scheduler::DefaultMaxPendingCallbacks; ++I) {
    LastItem = call("IoAllocateWorkItem", {FDO});
    call("IoQueueWorkItem", {LastItem, WorkerPC, profile::DelayedWorkQueue, 0});
    const auto Worker = take(Model->nextScheduled(false));
    ASSERT_TRUE(Worker);
    LastWorker = Worker->ID;
    success(Model->suspendScheduled(LastWorker));
  }
  std::vector<uint8_t> Before(IRPSize + 2 * StackSize);
  success(Memory->read(IRP, Before));
  rejected(Model->nextScheduled(true), "pending callback limit");
  std::vector<uint8_t> After(Before.size());
  success(Memory->read(IRP, After));
  EXPECT_EQ(After, Before);
  ASSERT_EQ(Result.Requests.size(), 2u);
  EXPECT_EQ(Result.Requests.back().ResponseIndex, 0u);
  EXPECT_FALSE(Result.Requests.back().Completed);
  EXPECT_FALSE(Result.Requests.back().Power->BusStatus);
  EXPECT_FALSE(Result.Requests.back().Power->BusCompletedAt100ns);
  EXPECT_EQ(Model->nextEventTime(), 11u);
  EXPECT_FALSE(Model->takeGuestCall());
  success(Model->resumeScheduled(LastWorker));
  call("IoFreeWorkItem", {LastItem});
  success(Model->finishScheduled(LastWorker));
  const auto Completion = scheduled(PowerPC);
  EXPECT_EQ(Result.Requests.back().Power->BusCompletedAt100ns, 11u);
  EXPECT_FALSE(take(Model->continueScheduled(Completion.ID, 0)));
  success(Model->finishScheduled(Completion.ID));
  // Retrying the due batch must neither re-consume response zero nor consume
  // the next response before its actual PoRequestPowerIrp call.
  const auto Next = request(DevicePowerState::D0, 0);
  copyDown(Next.Arguments[1]);
  call("IofCallDriver", {PDO, Next.Arguments[1]});
  finishCall(Next, 0, StatusPending);
  ASSERT_EQ(Result.Requests.size(), 3u);
  EXPECT_EQ(Result.Requests.back().ResponseIndex, 1u);
  EXPECT_EQ(Result.Requests.back().Power->DeviceStateAfter,
            DevicePowerState::D0);
}
} // namespace
} // namespace neverd::emulation
