//===- KernelSchedulerTests.cpp - Guest callback and time contracts -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify deterministic callback ownership, virtual time and resource bounds.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "os/windows/kernel/KernelScheduler.h"

#include <utility>

namespace neverd::emulation {
namespace {

using Scheduler = KernelScheduler;

Scheduler::Callback work(uint64_t Object, uint64_t Owner = 100) {
  return {Object, Owner, 200, 0x180001000, {Object + 10, Object + 20}};
}

Scheduler::DpcCallback
dpc(uint64_t Object,
    Scheduler::DpcImportance Importance = Scheduler::DpcImportance::Medium) {
  Scheduler::DpcCallback DPC;
  static_cast<Scheduler::Callback &>(DPC) = work(Object);
  DPC.Importance = Importance;
  return DPC;
}

TEST(KernelSchedulerThreadReservation, HoldsCapacityAndOwnerUntilMadeRunnable) {
  Scheduler::Limits L;
  L.MaxPendingCallbacks = 1;
  Scheduler S(L);
  auto ID = S.reserveSystemThread(work(1));
  ASSERT_TRUE(bool(ID));
  EXPECT_TRUE(S.hasPending());
  EXPECT_TRUE(S.hasOutstanding(100));
  EXPECT_TRUE(S.hasForegroundBlockingCallbacks());
  EXPECT_EQ(S.queuedCallbackCount(), 0u);
  EXPECT_FALSE(S.nextPassiveThread());
  auto Next = S.next(false);
  ASSERT_TRUE(bool(Next));
  EXPECT_FALSE(*Next);
  auto Extra = S.enqueueSystemThread(work(2));
  ASSERT_FALSE(bool(Extra));
  llvm::consumeError(Extra.takeError());
  auto Bad = S.readySystemThread(*ID + 1);
  ASSERT_TRUE(bool(Bad));
  llvm::consumeError(std::move(Bad));
  llvm::cantFail(S.readySystemThread(*ID));
  Next = S.next(false);
  ASSERT_TRUE(bool(Next));
  ASSERT_TRUE(*Next);
  EXPECT_EQ((**Next).ID, *ID);
  EXPECT_EQ((**Next).Kind, Scheduler::CallbackKind::SystemThread);
  llvm::cantFail(S.finish(*ID));
  EXPECT_FALSE(S.hasPending());
  EXPECT_FALSE(S.hasOutstanding(100));
}

void success(llvm::Error E) {
  if (E)
    ADD_FAILURE() << llvm::toString(std::move(E));
}

template <class T> T take(llvm::Expected<T> Result) {
  if (!Result) {
    ADD_FAILURE() << llvm::toString(Result.takeError());
    return {};
  }
  return std::move(*Result);
}

template <class T>
void expectError(llvm::Expected<T> Result, llvm::StringRef Text) {
  ASSERT_FALSE(bool(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find(Text.str()),
            std::string::npos);
}

void expectError(llvm::Error E, llvm::StringRef Text) {
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find(Text.str()), std::string::npos);
}

Scheduler::Callback dispatch(uint64_t IRP, uint64_t Device = 100) {
  auto Call = work(IRP, Device);
  Call.Arguments = {Device, IRP};
  return Call;
}

Scheduler::Callback usbIdle(uint64_t Token, uint64_t Context = 0) {
  auto Call = work(Token);
  Call.Arguments = {Context};
  return Call;
}

Scheduler::UsbIdleCallback nativeUsbIdle(uint64_t IRP, uint64_t Token = 500) {
  return {{IRP, 100, 200, 0, {100, IRP, Token}},
          Scheduler::CallbackKind::FrameworkUsbIdle};
}

TEST(DriverKernelScheduler,
     ForegroundBoundaryDistinguishesParkedCallbackKinds) {
  Scheduler S;
  EXPECT_FALSE(S.hasForegroundBlockingCallbacks());
  const auto Thread = take(S.enqueueSystemThread(work(1)));
  EXPECT_TRUE(S.hasForegroundBlockingCallbacks());
  ASSERT_TRUE(take(S.next()));
  EXPECT_TRUE(S.hasForegroundBlockingCallbacks());
  EXPECT_FALSE(S.isSuspendedSystemThread(Thread));
  success(S.suspend(Thread));
  EXPECT_FALSE(S.hasForegroundBlockingCallbacks());
  EXPECT_TRUE(S.isSuspendedSystemThread(Thread));
  EXPECT_FALSE(S.isSuspendedSystemThread(Thread + 1));
  const auto Worker = take(S.enqueueWorkItem(work(2)));
  EXPECT_TRUE(S.hasForegroundBlockingCallbacks());
  ASSERT_TRUE(take(S.next()));
  success(S.suspend(Worker));
  EXPECT_TRUE(S.hasForegroundBlockingCallbacks());
  EXPECT_FALSE(S.isSuspendedSystemThread(Worker));
  success(S.resume(Worker));
  success(S.finish(Worker));
  EXPECT_FALSE(S.hasForegroundBlockingCallbacks());
  success(S.resume(Thread));
  EXPECT_TRUE(S.hasForegroundBlockingCallbacks());
  success(S.finish(Thread));
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverKernelScheduler, MixedUsbIdleBatchAdmissionIsAtomic) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 2;
  Scheduler S(Limits);
  const std::vector<Scheduler::UsbIdleCallback> Batch = {{usbIdle(1)},
                                                         nativeUsbIdle(2)};
  success(S.canEnqueueUsbIdleBatch(Batch));
  success(S.canEnqueueUsbIdleBatch(Batch));
  EXPECT_FALSE(S.hasPending());
  EXPECT_EQ(take(S.enqueueWorkItem(work(3))), 1u);
  expectError(S.canEnqueueUsbIdleBatch(Batch), "pending callback limit");
  EXPECT_EQ(S.queuedCallbackCount(), 1u);
  EXPECT_TRUE(S.cancelWorkItem(3));
  success(S.canEnqueueUsbIdleBatch(Batch));
  EXPECT_EQ(take(S.enqueueUsbIdleCallback(usbIdle(1))), 2u);
  EXPECT_EQ(take(S.enqueueFrameworkUsbIdleCallback(2, 100, 200, 500)), 3u);
  EXPECT_EQ(S.queuedCallbackCount(), 2u);
}

TEST(DriverKernelScheduler, MixedUsbIdleBatchValidatesExactNativePayload) {
  Scheduler S;
  const auto Native = nativeUsbIdle(2);
  for (unsigned InvalidField = 0; InvalidField != 9; ++InvalidField) {
    auto Bad = Native;
    switch (InvalidField) {
    case 0:
      Bad.Call.Object = 0;
      break;
    case 1:
      Bad.Call.Owner = 0;
      break;
    case 2:
      Bad.Call.Thread = 0;
      break;
    case 3:
      Bad.Call.PC = 0x180001000;
      break;
    case 4:
      Bad.Call.SynchronizationObject = 1;
      break;
    case 5:
      Bad.Call.Arguments.pop_back();
      break;
    case 6:
      ++Bad.Call.Arguments[0];
      break;
    case 7:
      ++Bad.Call.Arguments[1];
      break;
    case 8:
      Bad.Call.Arguments[2] = 0;
      break;
    }
    expectError(S.canEnqueueUsbIdleBatch({{usbIdle(1)}, Bad}),
                "typed provider");
    EXPECT_FALSE(S.hasPending());
  }
  auto Foreign = Native;
  Foreign.Kind = Scheduler::CallbackKind::WDMProviderDispatch;
  expectError(S.canEnqueueUsbIdleBatch({{usbIdle(1)}, Foreign}), "foreign");
  expectError(S.canEnqueueUsbIdleBatch({{usbIdle(2)}, Native}), "outstanding");
  EXPECT_EQ(take(S.enqueueFrameworkUsbIdleCallback(2, 100, 200, 500)), 1u);
}

TEST(DriverKernelScheduler, NativeUsbIdleUsesPassiveFifoAndExactWithdrawal) {
  Scheduler S;
  const uint64_t First = take(S.enqueueWorkItem(work(1)));
  const uint64_t Removed =
      take(S.enqueueFrameworkUsbIdleCallback(2, 100, 200, 500));
  const uint64_t Native =
      take(S.enqueueFrameworkUsbIdleCallback(3, 100, 200, 501));
  const uint64_t Last = take(S.enqueueUsbIdleCallback(usbIdle(4)));
  expectError(S.withdrawUsbIdleCallback(First), "exact queued");
  EXPECT_FALSE(S.cancelWorkItem(2));
  success(S.canWithdrawUsbIdleCallback(Removed));
  success(S.withdrawUsbIdleCallback(Removed));
  expectError(S.withdrawUsbIdleCallback(Removed), "exact queued");
  for (uint64_t ID : {First, Native, Last}) {
    auto Call = take(S.next());
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->ID, ID);
    EXPECT_EQ(Call->IRQL, scheduler::PassiveLevel);
    if (ID == Native) {
      EXPECT_EQ(Call->Kind, Scheduler::CallbackKind::FrameworkUsbIdle);
      EXPECT_EQ(Call->PC, 0u);
      EXPECT_EQ(Call->Arguments, (std::vector<uint64_t>{100, 3, 501}));
      expectError(S.withdrawUsbIdleCallback(ID), "exact queued");
    }
    success(S.finish(ID));
  }
}

TEST(DriverKernelScheduler,
     NativeUsbIdleTransferPreservesSlotAndDispatchBudget) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 1;
  Limits.MaxDispatches = 1;
  Limits.InitialTime100ns = 25;
  Scheduler S(Limits);
  const uint64_t ID = take(S.enqueueFrameworkUsbIdleCallback(1, 100, 200, 500));
  auto Selected = take(S.next());
  ASSERT_TRUE(Selected);
  auto Guest = work(1);
  Guest.SynchronizationObject = 300;
  Guest.Arguments = {700, 800, 900};
  auto Changed = take(S.beginFrameworkUsbIdleCallback(ID, Guest));
  EXPECT_EQ(Changed.ID, ID);
  EXPECT_EQ(Changed.Kind, Scheduler::CallbackKind::FrameworkDeferred);
  EXPECT_TRUE(Changed.FrameworkUsbIdleContinuation);
  EXPECT_EQ(Changed.PC, Guest.PC);
  EXPECT_EQ(Changed.Object, Selected->Object);
  EXPECT_EQ(Changed.Owner, Selected->Owner);
  EXPECT_EQ(Changed.Thread, Selected->Thread);
  EXPECT_EQ(Changed.IRQL, Selected->IRQL);
  EXPECT_EQ(Changed.DueTime100ns, Selected->DueTime100ns);
  EXPECT_EQ(Changed.SynchronizationObject, Guest.SynchronizationObject);
  EXPECT_EQ(Changed.Arguments, Guest.Arguments);
  EXPECT_EQ(S.dispatchCount(), 1u);
  EXPECT_EQ(S.now100ns(), 25u);
  expectError(S.enqueueWorkItem(work(2)), "pending callback limit");
  expectError(S.beginFrameworkUsbIdleCallback(ID, Guest), "active task");
  success(S.suspend(ID));
  EXPECT_TRUE(S.suspended(ID)->FrameworkUsbIdleContinuation);
  expectError(S.enqueueWorkItem(work(2)), "pending callback limit");
  success(S.resume(ID));
  success(S.finish(ID));
  EXPECT_EQ(take(S.enqueueWorkItem(work(2))), ID + 1);
}

TEST(DriverKernelScheduler, NativeUsbIdleTransferFailuresPreserveModelTask) {
  Scheduler S;
  const uint64_t ID = take(S.enqueueFrameworkUsbIdleCallback(1, 100, 200, 500));
  ASSERT_TRUE(take(S.next()));
  auto Guest = work(1);
  auto Bad = Guest;
  Bad.PC = 0;
  expectError(S.beginFrameworkUsbIdleCallback(ID, Bad), "nonzero");
  Bad = Guest;
  ++Bad.Object;
  expectError(S.beginFrameworkUsbIdleCallback(ID, Bad), "identity");
  Bad = Guest;
  ++Bad.Owner;
  expectError(S.beginFrameworkUsbIdleCallback(ID, Bad), "identity");
  Bad = Guest;
  ++Bad.Thread;
  expectError(S.beginFrameworkUsbIdleCallback(ID, Bad), "identity");
  EXPECT_EQ(S.active()->Kind, Scheduler::CallbackKind::FrameworkUsbIdle);
  EXPECT_EQ(S.active()->Arguments, (std::vector<uint64_t>{100, 1, 500}));
  EXPECT_EQ(S.active()->PC, 0u);
  take(S.beginFrameworkUsbIdleCallback(ID, Guest));
  EXPECT_EQ(S.active()->PC, Guest.PC);
}

TEST(DriverKernelScheduler, MixedUsbIdleIdentitySurvivesTransferAndSuspension) {
  Scheduler S;
  const uint64_t ID = take(S.enqueueFrameworkUsbIdleCallback(1, 100, 200, 500));
  const auto RejectDuplicate = [&] {
    expectError(S.enqueueUsbIdleCallback(usbIdle(1)), "outstanding");
    expectError(S.enqueueFrameworkUsbIdleCallback(1, 100, 200, 501),
                "outstanding");
  };
  RejectDuplicate();
  ASSERT_TRUE(take(S.next()));
  RejectDuplicate();
  success(S.suspend(ID));
  RejectDuplicate();
  success(S.resume(ID));
  take(S.beginFrameworkUsbIdleCallback(ID, work(1)));
  RejectDuplicate();
  success(S.suspend(ID));
  RejectDuplicate();
  success(S.resume(ID));
  success(S.finish(ID));
  const uint64_t Raw = take(S.enqueueUsbIdleCallback(usbIdle(1)));
  expectError(S.enqueueFrameworkUsbIdleCallback(1, 100, 200, 502),
              "outstanding");
  success(S.withdrawUsbIdleCallback(Raw));
  EXPECT_EQ(take(S.enqueueFrameworkUsbIdleCallback(1, 100, 200, 502)), Raw + 1);
}

TEST(DriverKernelScheduler, UsbIdleBatchAdmissionPreservesCapacityAndIdentity) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 2;
  Scheduler S(Limits);
  success(S.canEnqueueUsbIdleCallbacks({usbIdle(1), usbIdle(2)}));
  EXPECT_FALSE(S.hasPending());
  auto First = take(S.enqueueWorkItem(work(1)));
  EXPECT_EQ(First, 1u);
  expectError(S.canEnqueueUsbIdleCallbacks({usbIdle(1), usbIdle(2)}),
              "pending callback limit");
  EXPECT_EQ(S.queuedCallbackCount(), 1u);
  EXPECT_EQ(take(S.enqueueUsbIdleCallback(usbIdle(1))), 2u);
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->ID, First);
  success(S.suspend(First));
  expectError(S.canEnqueueUsbIdleCallbacks({usbIdle(2)}),
              "pending callback limit");
  success(S.resume(First));
  success(S.finish(First));
  success(S.canEnqueueUsbIdleCallbacks({usbIdle(2)}));
}

TEST(DriverKernelScheduler,
     UsbIdleCallbackUsesPassiveWorkerOrderAndNullContext) {
  Scheduler S;
  auto First = take(S.enqueueWorkItem(work(1)));
  auto Idle = take(S.enqueueUsbIdleCallback(usbIdle(2)));
  auto Last = take(S.enqueuePoFx(work(3)));
  EXPECT_FALSE(S.cancelWorkItem(2));
  for (uint64_t ID : {First, Idle, Last}) {
    auto Call = take(S.next());
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->ID, ID);
    EXPECT_EQ(Call->IRQL, scheduler::PassiveLevel);
    if (ID == Idle) {
      EXPECT_EQ(Call->Kind, Scheduler::CallbackKind::UsbIdle);
      EXPECT_EQ(Call->Arguments, std::vector<uint64_t>{0});
    }
    success(S.finish(ID));
  }
}

TEST(DriverKernelScheduler, UsbIdleBatchRejectsInvalidPayloadWithoutMutation) {
  Scheduler S;
  auto Bad = usbIdle(2);
  Bad.PC = 0;
  expectError(S.canEnqueueUsbIdleCallbacks({usbIdle(1), Bad}), "nonzero");
  Bad = usbIdle(2);
  Bad.Arguments = {};
  expectError(S.canEnqueueUsbIdleCallbacks({usbIdle(1), Bad}), "one context");
  Bad.Arguments = {0, 1};
  expectError(S.enqueueUsbIdleCallback(Bad), "one context");
  Bad = usbIdle(2);
  Bad.SynchronizationObject = 100;
  expectError(S.canEnqueueUsbIdleCallbacks({usbIdle(1), Bad}),
              "synchronization");
  expectError(S.canEnqueueUsbIdleCallbacks({usbIdle(1), usbIdle(1)}),
              "outstanding");
  EXPECT_FALSE(S.hasPending());
  EXPECT_EQ(take(S.enqueueUsbIdleCallback(usbIdle(1))), 1u);
}

TEST(DriverKernelScheduler, UsbIdleIdentityRemainsOwnedWhileActiveOrSuspended) {
  Scheduler S;
  const uint64_t ID = take(S.enqueueUsbIdleCallback(usbIdle(1)));
  expectError(S.enqueueUsbIdleCallback(usbIdle(1)), "outstanding");
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  expectError(S.enqueueUsbIdleCallback(usbIdle(1)), "outstanding");
  expectError(S.withdrawUsbIdleCallback(ID), "exact queued");
  success(S.suspend(ID));
  expectError(S.enqueueUsbIdleCallback(usbIdle(1)), "outstanding");
  expectError(S.withdrawUsbIdleCallback(ID), "exact queued");
  EXPECT_TRUE(S.hasOutstanding(100));
  success(S.resume(ID));
  success(S.finish(ID));
  EXPECT_FALSE(S.hasOutstanding(100));
  EXPECT_EQ(take(S.enqueueUsbIdleCallback(usbIdle(1))), ID + 1);
}

TEST(DriverKernelScheduler, UsbIdleWithdrawalUsesExactQueuedKindAndIdentity) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 3;
  Scheduler S(Limits);
  const uint64_t Work = take(S.enqueueWorkItem(work(1)));
  const uint64_t First = take(S.enqueueUsbIdleCallback(usbIdle(1)));
  const uint64_t Second = take(S.enqueueUsbIdleCallback(usbIdle(2)));
  expectError(S.withdrawUsbIdleCallback(Work), "exact queued");
  expectError(S.withdrawUsbIdleCallback(Second + 1), "exact queued");
  success(S.canWithdrawUsbIdleCallback(First));
  EXPECT_EQ(S.queuedCallbackCount(), 3u);
  success(S.withdrawUsbIdleCallback(First));
  expectError(S.withdrawUsbIdleCallback(First), "exact queued");
  EXPECT_EQ(S.queuedCallbackCount(), 2u);
  EXPECT_EQ(take(S.enqueueUsbIdleCallback(usbIdle(3))), Second + 1);
  for (uint64_t ID : {Work, Second, Second + 1}) {
    auto Call = take(S.next());
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->ID, ID);
    success(S.finish(ID));
  }
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverKernelScheduler,
     WDMDispatchesSharePassiveWorkerOrderWithoutWorkItemCancellation) {
  Scheduler S;
  const auto First = take(S.enqueueWorkItem(work(1)));
  const auto Guest = take(S.enqueueWDMDispatch(dispatch(2)));
  const auto Provider = take(S.enqueueWDMProviderDispatch(3, 101, 200));
  const auto Last = take(S.enqueueWorkItem(work(4)));
  EXPECT_FALSE(S.cancelWorkItem(2));
  EXPECT_FALSE(S.cancelWorkItem(3));
  EXPECT_FALSE(S.isWorkItemQueued(2));
  EXPECT_TRUE(take(S.queueDPC(dpc(5))));
  auto DPC = take(S.next());
  ASSERT_TRUE(DPC);
  EXPECT_EQ(DPC->Kind, Scheduler::CallbackKind::DPC);
  success(S.finish(DPC->ID));
  for (const auto &[ID, Kind] :
       std::vector<std::pair<uint64_t, Scheduler::CallbackKind>>{
           {First, Scheduler::CallbackKind::WorkItem},
           {Guest, Scheduler::CallbackKind::WDMDispatch},
           {Provider, Scheduler::CallbackKind::WDMProviderDispatch},
           {Last, Scheduler::CallbackKind::WorkItem}}) {
    auto Call = take(S.next());
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->ID, ID);
    EXPECT_EQ(Call->Kind, Kind);
    EXPECT_EQ(Call->IRQL, scheduler::PassiveLevel);
    if (ID == Guest) {
      EXPECT_EQ(Call->PC, dispatch(2).PC);
      EXPECT_EQ(Call->Arguments, (std::vector<uint64_t>{100, 2}));
    }
    if (ID == Provider) {
      EXPECT_EQ(Call->PC, 0u);
      EXPECT_EQ(Call->Arguments, (std::vector<uint64_t>{101, 3}));
      EXPECT_TRUE(S.hasOutstanding(101));
    }
    success(S.finish(ID));
  }
  EXPECT_FALSE(S.hasPending());
  EXPECT_EQ(S.dispatchCount(), 5u);
}

TEST(DriverKernelScheduler,
     WDMDispatchAdmissionCountsActiveDPCAndDoesNotReserveIdentity) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 1;
  Scheduler S(Limits);
  success(S.canEnqueueWDMDispatch());
  success(S.canEnqueueWDMDispatch());
  EXPECT_FALSE(S.hasPending());
  EXPECT_TRUE(take(S.queueDPC(dpc(1))));
  auto DPC = take(S.next());
  ASSERT_TRUE(DPC);
  EXPECT_EQ(DPC->ID, 1u);
  expectError(S.canEnqueueWDMDispatch(), "pending callback limit");
  expectError(S.enqueueWDMDispatch(dispatch(2)), "pending callback limit");
  expectError(S.enqueueWDMProviderDispatch(2, 100, 200),
              "pending callback limit");
  EXPECT_EQ(S.queuedCallbackCount(), 0u);
  success(S.finish(DPC->ID));
  success(S.canEnqueueWDMDispatch());
  EXPECT_EQ(take(S.enqueueWDMProviderDispatch(2, 100, 200)), 2u);
  auto Provider = take(S.next());
  ASSERT_TRUE(Provider);
  success(S.suspend(Provider->ID));
  expectError(S.canEnqueueWDMDispatch(), "pending callback limit");
  success(S.resume(Provider->ID));
  success(S.finish(Provider->ID));
  success(S.canEnqueueWDMDispatch());
  EXPECT_EQ(take(S.enqueueWDMDispatch(dispatch(3))), 3u);
}

TEST(DriverKernelScheduler,
     OnlyTypedProviderDispatchCanCarryNoGuestPCAndInvalidPayloadIsAtomic) {
  Scheduler S;
  auto Bad = dispatch(1);
  Bad.PC = 0;
  expectError(S.enqueueWDMDispatch(Bad), "nonzero");
  expectError(S.enqueueWorkItem(Bad), "nonzero");
  expectError(S.enqueueWDMCompletion(Bad), "nonzero");
  Bad = dispatch(1);
  Bad.Arguments = {1, 100};
  expectError(S.enqueueWDMDispatch(Bad), "device and IRP");
  Bad = dispatch(1);
  Bad.SynchronizationObject = 77;
  expectError(S.enqueueWDMDispatch(Bad), "device and IRP");
  expectError(S.enqueueWDMProviderDispatch(0, 100, 200), "nonzero");
  expectError(S.enqueueWDMProviderDispatch(1, 0, 200), "nonzero");
  expectError(S.enqueueWDMProviderDispatch(1, 100, 0), "nonzero");
  EXPECT_FALSE(S.hasPending());
  EXPECT_EQ(take(S.enqueueWDMProviderDispatch(1, 100, 200)), 1u);
}

TEST(DriverKernelScheduler,
     WDMDispatchPacketCannotBeQueuedTwiceWhileQueuedActiveOrSuspended) {
  Scheduler S;
  const auto ID = take(S.enqueueWDMDispatch(dispatch(1)));
  expectError(S.enqueueWDMProviderDispatch(1, 100, 200), "outstanding");
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  expectError(S.enqueueWDMDispatch(dispatch(1)), "outstanding");
  success(S.suspend(ID));
  expectError(S.enqueueWDMProviderDispatch(1, 101, 201), "outstanding");
  EXPECT_TRUE(S.hasOutstanding(100));
  EXPECT_FALSE(S.hasOutstanding(101));
  success(S.resume(ID));
  success(S.finish(ID));
  EXPECT_FALSE(S.hasOutstanding(100));
  EXPECT_EQ(take(S.enqueueWDMProviderDispatch(2, 101, 201)), ID + 1);
}

TEST(DriverKernelScheduler,
     ProviderCompletionReusesFullSlotIdentityTimeAndDispatchBudget) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 1;
  Limits.MaxDispatches = 1;
  Limits.InitialTime100ns = 123;
  Scheduler S(Limits);
  const auto ID = take(S.enqueueWDMProviderDispatch(1, 100, 200));
  auto Provider = take(S.next());
  ASSERT_TRUE(Provider);
  auto Completion = work(1);
  Completion.Arguments = {100, 1, 42};
  expectError(S.enqueueWDMCompletion(Completion), "pending callback limit");
  const auto Continued = take(S.beginWDMProviderCompletion(ID, Completion));
  EXPECT_EQ(Continued.ID, ID);
  EXPECT_EQ(Continued.Kind, Scheduler::CallbackKind::WDMCompletion);
  EXPECT_EQ(Continued.IRQL, scheduler::PassiveLevel);
  EXPECT_EQ(Continued.DueTime100ns, Provider->DueTime100ns);
  EXPECT_EQ(Continued.DueTime100ns, 123u);
  EXPECT_EQ(Continued.Object, Provider->Object);
  EXPECT_EQ(Continued.Owner, Provider->Owner);
  EXPECT_EQ(Continued.Thread, Provider->Thread);
  EXPECT_EQ(Continued.PC, Completion.PC);
  EXPECT_EQ(Continued.Arguments, Completion.Arguments);
  EXPECT_EQ(S.active()->Kind, Scheduler::CallbackKind::WDMCompletion);
  EXPECT_EQ(S.queuedCallbackCount(), 0u);
  EXPECT_EQ(S.dispatchCount(), 1u);
  success(S.suspend(ID));
  EXPECT_TRUE(S.hasOutstanding(100));
  success(S.resume(ID));
  EXPECT_EQ(S.active()->Arguments, Completion.Arguments);
  success(S.finish(ID));
  EXPECT_EQ(take(S.enqueueWorkItem(work(2))), ID + 1);
  expectError(S.next(), "dispatch limit");
}

TEST(DriverKernelScheduler,
     InvalidProviderCompletionTransferPreservesItsOriginalTask) {
  Scheduler S;
  const auto ID = take(S.enqueueWDMProviderDispatch(1, 100, 200));
  expectError(S.beginWDMProviderCompletion(ID, work(1)), "identity mismatch");
  auto Provider = take(S.next());
  ASSERT_TRUE(Provider);
  expectError(S.beginWDMProviderCompletion(ID + 1, work(1)),
              "identity mismatch");
  for (unsigned Field = 0; Field != 6; ++Field) {
    auto Bad = work(1);
    switch (Field) {
    case 0:
      ++Bad.Object;
      break;
    case 1:
      ++Bad.Owner;
      break;
    case 2:
      ++Bad.Thread;
      break;
    case 3:
      Bad.PC = 0;
      break;
    case 4:
      Bad.Arguments.resize(scheduler::MaxArguments + 1);
      break;
    case 5:
      Bad.SynchronizationObject = 5;
      break;
    }
    auto Attempt = S.beginWDMProviderCompletion(ID, Bad);
    ASSERT_FALSE(bool(Attempt));
    llvm::consumeError(Attempt.takeError());
    ASSERT_TRUE(S.active());
    EXPECT_EQ(S.active()->ID, ID);
    EXPECT_EQ(S.active()->Kind, Scheduler::CallbackKind::WDMProviderDispatch);
    EXPECT_EQ(S.active()->PC, 0u);
    EXPECT_EQ(S.active()->Arguments, Provider->Arguments);
    EXPECT_EQ(S.dispatchCount(), 1u);
  }
  take(S.beginWDMProviderCompletion(ID, work(1)));
  expectError(S.beginWDMProviderCompletion(ID, work(1)), "active provider");
  success(S.finish(ID));
  const auto Guest = take(S.enqueueWDMDispatch(dispatch(2)));
  ASSERT_TRUE(take(S.next()));
  expectError(S.beginWDMProviderCompletion(Guest, work(2)), "active provider");
  EXPECT_EQ(S.active()->Kind, Scheduler::CallbackKind::WDMDispatch);
}

TEST(DriverKernelScheduler,
     FrameworkInterruptCallbacksCoalesceWithoutAliasingKernelObjects) {
  Scheduler S;
  const auto Deferred = take(S.queueFrameworkInterrupt(work(1), false));
  ASSERT_TRUE(Deferred);
  EXPECT_FALSE(take(S.queueFrameworkInterrupt(work(1), false)));
  EXPECT_FALSE(S.removeDPC(1));
  EXPECT_TRUE(take(S.queueDPC(dpc(1))));
  const auto Worker = take(S.queueFrameworkInterrupt(work(1), true));
  ASSERT_TRUE(Worker);
  EXPECT_FALSE(take(S.queueFrameworkInterrupt(work(1), true)));
  EXPECT_FALSE(S.cancelWorkItem(1));
  EXPECT_TRUE(S.hasFrameworkInterrupt(1));
  auto First = take(S.next());
  ASSERT_TRUE(First);
  EXPECT_EQ(First->Kind, Scheduler::CallbackKind::FrameworkInterruptDPC);
  EXPECT_EQ(First->IRQL, scheduler::DispatchLevel);
  EXPECT_EQ(First->ID, *Deferred);
  EXPECT_EQ(First->Arguments, work(1).Arguments);
  // Dequeue permits another delivery while the active callback retains owner.
  EXPECT_TRUE(take(S.queueFrameworkInterrupt(work(1), false)));
  success(S.finish(First->ID));
  auto Kernel = take(S.next());
  ASSERT_TRUE(Kernel);
  EXPECT_EQ(Kernel->Kind, Scheduler::CallbackKind::DPC);
  success(S.finish(Kernel->ID));
  auto Requeued = take(S.next());
  ASSERT_TRUE(Requeued);
  EXPECT_EQ(Requeued->Kind, Scheduler::CallbackKind::FrameworkInterruptDPC);
  success(S.finish(Requeued->ID));
  auto Passive = take(S.next());
  ASSERT_TRUE(Passive);
  EXPECT_EQ(Passive->Kind, Scheduler::CallbackKind::FrameworkInterruptWorkItem);
  EXPECT_EQ(Passive->IRQL, scheduler::PassiveLevel);
  EXPECT_EQ(Passive->ID, *Worker);
  success(S.suspend(Passive->ID));
  EXPECT_TRUE(S.hasFrameworkInterrupt(1));
  EXPECT_TRUE(S.hasOutstanding(100));
  success(S.resume(Passive->ID));
  success(S.finish(Passive->ID));
  EXPECT_FALSE(S.hasFrameworkInterrupt(1));
  EXPECT_FALSE(S.hasOutstanding(100));
}

TEST(DriverKernelScheduler,
     FrameworkInterruptCoalescingDoesNotConsumeCapacityOrIdentities) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 1;
  Scheduler S(Limits);
  auto First = take(S.queueFrameworkInterrupt(work(1), false));
  ASSERT_TRUE(First);
  EXPECT_FALSE(take(S.queueFrameworkInterrupt(work(1), false)));
  expectError(S.queueFrameworkInterrupt(work(2), false), "limit");
  auto Invocation = take(S.next());
  ASSERT_TRUE(Invocation);
  expectError(S.queueFrameworkInterrupt(work(1), false), "limit");
  success(S.finish(Invocation->ID));
  auto Next = take(S.queueFrameworkInterrupt(work(1), false));
  ASSERT_TRUE(Next);
  EXPECT_EQ(*Next, *First + 1);
}

TEST(DriverKernelScheduler,
     FrameworkPassiveContinuationsRetainIdentityAndWorkerOrdering) {
  Scheduler S;
  const auto First = take(S.enqueueWorkItem(work(1)));
  const auto Deferred =
      take(S.enqueueFrameworkDeferred(work(1), scheduler::PassiveLevel));
  EXPECT_TRUE(S.cancelWorkItem(1));
  EXPECT_FALSE(S.cancelWorkItem(1));
  EXPECT_FALSE(S.isWorkItemQueued(1));
  expectError(S.enqueueFrameworkDeferred(work(1), scheduler::PassiveLevel),
              "outstanding");
  auto Invocation = take(S.next());
  ASSERT_TRUE(Invocation);
  EXPECT_EQ(Invocation->Kind, Scheduler::CallbackKind::FrameworkDeferred);
  EXPECT_EQ(Invocation->ID, Deferred);
  EXPECT_GT(Deferred, First);
  EXPECT_EQ(Invocation->IRQL, scheduler::PassiveLevel);
  EXPECT_EQ(Invocation->Arguments, work(1).Arguments);
  expectError(S.enqueueFrameworkDeferred(work(1), scheduler::PassiveLevel),
              "outstanding");
  success(S.suspend(Deferred));
  EXPECT_TRUE(S.hasOutstanding(100));
  expectError(S.enqueueFrameworkDeferred(work(1), scheduler::PassiveLevel),
              "outstanding");
  const auto Second =
      take(S.enqueueFrameworkDeferred(work(2), scheduler::PassiveLevel));
  auto Next = take(S.next());
  ASSERT_TRUE(Next);
  EXPECT_EQ(Next->ID, Second);
  success(S.finish(Second));
  success(S.resume(Deferred));
  success(S.finish(Deferred));
  EXPECT_FALSE(S.hasOutstanding(100));
}

TEST(DriverKernelScheduler,
     FrameworkDispatchPrecedesWorkersAndCanHandOffItsPassiveEpilogue) {
  Scheduler S;
  const auto Worker = take(S.enqueueWorkItem(work(1)));
  const auto Passive =
      take(S.enqueueFrameworkDeferred(work(2), scheduler::PassiveLevel));
  expectError(S.enqueueFrameworkDeferred(work(3), 1), "IRQL");
  const auto Dispatch =
      take(S.enqueueFrameworkDeferred(work(3), scheduler::DispatchLevel));
  EXPECT_TRUE(S.hasQueuedDPC());
  EXPECT_FALSE(S.isDPCQueued(3));
  EXPECT_FALSE(S.removeDPC(3));
  expectError(S.enqueueFrameworkDeferred(work(3), scheduler::PassiveLevel),
              "outstanding");
  auto First = take(S.next());
  ASSERT_TRUE(First);
  EXPECT_EQ(First->ID, Dispatch);
  EXPECT_EQ(First->IRQL, scheduler::DispatchLevel);
  const auto Epilogue =
      take(S.enqueueFrameworkDeferred(work(3), scheduler::PassiveLevel));
  expectError(S.enqueueFrameworkDeferred(work(3), scheduler::DispatchLevel),
              "outstanding");
  success(S.finish(Dispatch));
  for (uint64_t ID : {Worker, Passive, Epilogue}) {
    auto Next = take(S.next());
    ASSERT_TRUE(Next);
    EXPECT_EQ(Next->ID, ID);
    EXPECT_EQ(Next->IRQL, scheduler::PassiveLevel);
    success(S.finish(ID));
  }
}

TEST(DriverKernelScheduler, WorkersPreserveFIFOAndExactGuestMetadata) {
  Scheduler S;
  const uint64_t FirstID = take(S.enqueueWorkItem(work(1)));
  const uint64_t SecondID = take(S.enqueueWorkItem(work(2, 101)));
  EXPECT_NE(FirstID, SecondID);
  auto First = take(S.next());
  ASSERT_TRUE(First);
  EXPECT_EQ(First->ID, FirstID);
  EXPECT_EQ(First->Object, 1u);
  EXPECT_EQ(First->Owner, 100u);
  EXPECT_EQ(First->Thread, 200u);
  EXPECT_EQ(First->PC, 0x180001000u);
  EXPECT_EQ(First->Arguments, (std::vector<uint64_t>{11, 21}));
  EXPECT_EQ(First->IRQL, 0u);
  EXPECT_EQ(First->Kind, Scheduler::CallbackKind::WorkItem);
  EXPECT_FALSE(S.isWorkItemQueued(1));
  EXPECT_TRUE(S.hasOutstanding(100));
  success(S.finish(FirstID));
  EXPECT_FALSE(S.hasOutstanding(100));
  auto Second = take(S.next());
  ASSERT_TRUE(Second);
  EXPECT_EQ(Second->ID, SecondID);
  success(S.finish(SecondID));
  EXPECT_FALSE(take(S.next()));
  EXPECT_FALSE(S.hasPending());
  EXPECT_EQ(S.dispatchCount(), 2u);
}

TEST(DriverKernelScheduler, QueuedWorkItemDuplicatesFailButCallbackMayRequeue) {
  Scheduler S;
  take(S.enqueueWorkItem(work(1)));
  expectError(S.enqueueWorkItem(work(1)), "already queued");
  auto First = take(S.next());
  ASSERT_TRUE(First);
  const uint64_t RequeuedID = take(S.enqueueWorkItem(work(1)));
  EXPECT_NE(First->ID, RequeuedID);
  EXPECT_EQ(S.queuedCallbackCount(), 1u);
  success(S.finish(First->ID));
  EXPECT_TRUE(S.hasOutstanding(100));
  auto Second = take(S.next());
  ASSERT_TRUE(Second);
  EXPECT_EQ(Second->ID, RequeuedID);
  success(S.finish(Second->ID));
  EXPECT_FALSE(S.hasOutstanding(100));
}

TEST(DriverKernelScheduler, CancellationCannotEraseRunningOwnership) {
  Scheduler S;
  take(S.enqueueWorkItem(work(1)));
  take(S.enqueueWorkItem(work(2)));
  EXPECT_TRUE(S.cancelWorkItem(1));
  EXPECT_FALSE(S.cancelWorkItem(1));
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->Object, 2u);
  EXPECT_FALSE(S.cancelWorkItem(2));
  EXPECT_TRUE(S.hasOutstanding(100));
  expectError(S.finish(Call->ID + 1), "identity mismatch");
  EXPECT_TRUE(S.active());
  success(S.finish(Call->ID));
  expectError(S.finish(Call->ID), "identity mismatch");
}

TEST(DriverKernelScheduler, DPCsPrecedeWorkersAndHighImportanceInsertsAtHead) {
  Scheduler S;
  take(S.enqueueWorkItem(work(1)));
  EXPECT_TRUE(take(S.queueDPC(dpc(2, Scheduler::DpcImportance::Low))));
  EXPECT_TRUE(take(S.queueDPC(dpc(3))));
  EXPECT_TRUE(take(S.queueDPC(dpc(4, Scheduler::DpcImportance::High))));
  EXPECT_TRUE(take(S.queueDPC(dpc(5, Scheduler::DpcImportance::MediumHigh))));
  EXPECT_TRUE(take(S.queueDPC(dpc(6, Scheduler::DpcImportance::High))));
  for (uint64_t Object : {6, 4, 2, 3, 5, 1}) {
    auto Call = take(S.next());
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->Object, Object);
    EXPECT_EQ(Call->IRQL, Object == 1 ? 0u : 2u);
    EXPECT_EQ(Call->Kind, Object == 1 ? Scheduler::CallbackKind::WorkItem
                                      : Scheduler::CallbackKind::DPC);
    success(S.finish(Call->ID));
  }
}

TEST(DriverKernelScheduler,
     DuplicateDPCKeepsOriginalArgumentsAndQueuePosition) {
  Scheduler S;
  EXPECT_TRUE(take(S.queueDPC(dpc(1))));
  EXPECT_TRUE(take(S.queueDPC(dpc(2))));
  auto Replacement = dpc(2, Scheduler::DpcImportance::High);
  Replacement.Arguments = {0xffff};
  EXPECT_FALSE(take(S.queueDPC(Replacement)));
  auto First = take(S.next());
  ASSERT_TRUE(First);
  EXPECT_EQ(First->Object, 1u);
  EXPECT_TRUE(take(S.queueDPC(dpc(1))));
  success(S.finish(First->ID));
  auto Second = take(S.next());
  ASSERT_TRUE(Second);
  EXPECT_EQ(Second->Object, 2u);
  EXPECT_EQ(Second->Arguments, (std::vector<uint64_t>{12, 22}));
  success(S.finish(Second->ID));
  EXPECT_TRUE(S.removeDPC(1));
  EXPECT_FALSE(S.removeDPC(1));
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverKernelScheduler, CallbackCannotAdvanceTimeOrDispatchRecursively) {
  Scheduler S;
  take(S.enqueueWorkItem(work(1)));
  EXPECT_FALSE(take(S.setTimer(9, 100, -100, 0, dpc(2))));
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  expectError(S.next(), "unfinished callback");
  EXPECT_EQ(S.now100ns(), 0u);
  EXPECT_TRUE(S.isTimerArmed(9));
  success(S.finish(Call->ID));
  auto Timer = take(S.next());
  ASSERT_TRUE(Timer);
  EXPECT_EQ(Timer->Object, 2u);
  EXPECT_EQ(Timer->SourceTimer, 9u);
  EXPECT_EQ(S.now100ns(), 100u);
  success(S.finish(Timer->ID));
}

TEST(DriverKernelScheduler, FutureTimerDoesNotOvertakeReadyWork) {
  Scheduler S;
  EXPECT_FALSE(take(S.setTimer(9, 100, -50, 0, dpc(2))));
  take(S.enqueueWorkItem(work(1)));
  auto Worker = take(S.next());
  ASSERT_TRUE(Worker);
  EXPECT_EQ(Worker->Object, 1u);
  EXPECT_EQ(S.now100ns(), 0u);
  success(S.finish(Worker->ID));
  EXPECT_FALSE(take(S.next(false)));
  EXPECT_TRUE(S.hasPending());
  auto Timer = take(S.next());
  ASSERT_TRUE(Timer);
  EXPECT_EQ(Timer->Object, 2u);
  EXPECT_EQ(S.now100ns(), 50u);
  success(S.finish(Timer->ID));
}

TEST(DriverKernelScheduler, RelativeAbsoluteAndPastDeadlinesUseSameClock) {
  Scheduler::Limits Limits;
  Limits.InitialTime100ns = 100;
  Scheduler S(Limits);
  EXPECT_FALSE(take(S.setTimer(10, 100, 150, 0, dpc(1))));
  EXPECT_FALSE(take(S.setTimer(20, 100, -25, 0, dpc(2))));
  EXPECT_FALSE(take(S.setTimer(30, 100, 50, 0, dpc(3))));
  const uint64_t Times[] = {100, 125, 150};
  const uint64_t Objects[] = {3, 2, 1};
  for (unsigned I = 0; I != 3; ++I) {
    auto Call = take(S.next());
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->Object, Objects[I]);
    EXPECT_EQ(S.now100ns(), Times[I]);
    success(S.finish(Call->ID));
  }
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverKernelScheduler, EqualTimerDeadlinesFollowArmingOrder) {
  Scheduler S;
  EXPECT_FALSE(take(S.setTimer(90, 100, -10, 0, dpc(9))));
  EXPECT_FALSE(take(S.setTimer(10, 100, -10, 0, dpc(1))));
  EXPECT_TRUE(take(S.setTimer(90, 100, -10, 0, dpc(9))));
  for (uint64_t Object : {1, 9}) {
    auto Call = take(S.next());
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->Object, Object);
    EXPECT_EQ(S.now100ns(), 10u);
    success(S.finish(Call->ID));
  }
}

TEST(DriverKernelScheduler, ResetAndCancelReplaceOnlyFutureTimerInsertion) {
  Scheduler S;
  EXPECT_FALSE(take(S.setTimer(10, 100, -5, 0, dpc(1))));
  EXPECT_TRUE(take(S.setTimer(10, 100, -10, 0, dpc(2))));
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->Object, 2u);
  EXPECT_EQ(S.now100ns(), 10u);
  EXPECT_FALSE(S.cancelTimer(10));
  EXPECT_TRUE(take(S.timerSignaled(10)));
  EXPECT_TRUE(S.hasOutstanding(100));
  success(S.finish(Call->ID));
  EXPECT_FALSE(take(S.setTimer(10, 100, -20, 0, dpc(1))));
  EXPECT_FALSE(take(S.timerSignaled(10)));
  EXPECT_TRUE(S.cancelTimer(10));
  EXPECT_FALSE(S.cancelTimer(10));
  EXPECT_FALSE(take(S.next()));
  EXPECT_EQ(S.now100ns(), 10u);
}

TEST(DriverKernelScheduler, ExpiredDPCSurvivesTimerCancellationAndReset) {
  Scheduler S;
  EXPECT_FALSE(take(S.setTimer(10, 100, -10, 1, dpc(1))));
  EXPECT_FALSE(take(S.setTimer(20, 100, -10, 0, dpc(2))));
  auto First = take(S.next());
  ASSERT_TRUE(First);
  EXPECT_EQ(First->Object, 1u);
  EXPECT_FALSE(S.cancelTimer(20));
  EXPECT_TRUE(S.isDPCQueued(2));
  EXPECT_FALSE(take(S.setTimer(20, 100, -100, 0, dpc(3))));
  EXPECT_TRUE(S.cancelTimer(10));
  EXPECT_TRUE(S.hasOutstanding(100));
  success(S.finish(First->ID));
  auto Queued = take(S.next());
  ASSERT_TRUE(Queued);
  EXPECT_EQ(Queued->Object, 2u);
  EXPECT_EQ(S.now100ns(), 10u);
  success(S.finish(Queued->ID));
  auto Reset = take(S.next());
  ASSERT_TRUE(Reset);
  EXPECT_EQ(Reset->Object, 3u);
  EXPECT_EQ(S.now100ns(), 110u);
  success(S.finish(Reset->ID));
}

TEST(DriverKernelScheduler, RemovingQueuedDPCDoesNotDisarmPeriodicTimer) {
  Scheduler S;
  EXPECT_FALSE(take(S.setTimer(10, 100, -10, 1, dpc(1))));
  EXPECT_FALSE(take(S.setTimer(20, 100, -10, 1, dpc(2))));
  auto First = take(S.next());
  ASSERT_TRUE(First);
  EXPECT_TRUE(S.removeDPC(2));
  EXPECT_TRUE(S.isTimerArmed(20));
  EXPECT_TRUE(S.cancelTimer(10));
  success(S.finish(First->ID));
  auto Periodic = take(S.next());
  ASSERT_TRUE(Periodic);
  EXPECT_EQ(Periodic->Object, 2u);
  EXPECT_EQ(S.now100ns(), 10010u);
  EXPECT_TRUE(S.isTimerArmed(20));
  EXPECT_TRUE(S.cancelTimer(20));
  success(S.finish(Periodic->ID));
}

TEST(DriverKernelScheduler, SharedDPCQueuesOnceAndBothTimersSignal) {
  Scheduler S;
  EXPECT_FALSE(take(S.setTimer(10, 100, -10, 0, dpc(1))));
  EXPECT_FALSE(take(S.setTimer(20, 100, -10, 0, dpc(1))));
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->SourceTimer, 10u);
  EXPECT_TRUE(take(S.timerSignaled(10)));
  EXPECT_TRUE(take(S.timerSignaled(20)));
  EXPECT_EQ(S.queuedCallbackCount(), 0u);
  EXPECT_EQ(S.timerExpirationCount(), 2u);
  success(S.finish(Call->ID));
  EXPECT_FALSE(take(S.next()));
}

TEST(DriverKernelScheduler, SignalOnlyTimerDoesNotHideLaterCallback) {
  Scheduler S;
  EXPECT_FALSE(take(S.setTimer(10, 100, -5, 0)));
  EXPECT_FALSE(take(S.setTimer(20, 100, -10, 0, dpc(2))));
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->Object, 2u);
  EXPECT_TRUE(take(S.timerSignaled(10)));
  EXPECT_TRUE(take(S.consumeTimerSignal(10)));
  EXPECT_FALSE(take(S.consumeTimerSignal(10)));
  EXPECT_FALSE(take(S.timerSignaled(10)));
  success(S.finish(Call->ID));
  success(S.forgetTimer(10));
  expectError(S.timerSignaled(10), "unknown");
}

TEST(DriverKernelScheduler, TimerOwnershipOutlivesQueueRemovalAndCancellation) {
  Scheduler S;
  EXPECT_FALSE(take(S.setTimer(10, 100, -1, 1, dpc(1))));
  EXPECT_TRUE(S.hasOutstanding(100));
  expectError(S.forgetTimer(10), "outstanding");
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  EXPECT_TRUE(S.cancelTimer(10));
  EXPECT_TRUE(S.hasOutstanding(100));
  expectError(S.forgetTimer(10), "outstanding");
  success(S.finish(Call->ID));
  EXPECT_FALSE(S.hasOutstanding(100));
  success(S.forgetTimer(10));
}

TEST(DriverKernelScheduler, NonperiodicTimerCanBeFreedByItsCallback) {
  Scheduler S;
  EXPECT_FALSE(take(S.setTimer(10, 100, -1, 0, dpc(1))));
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  success(S.forgetTimer(10));
  EXPECT_TRUE(S.hasOutstanding(100));
  success(S.finish(Call->ID));
  EXPECT_FALSE(S.hasOutstanding(100));
}

TEST(DriverKernelScheduler,
     PendingBudgetIncludesRunningCallbackWithoutDroppingIt) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 1;
  Scheduler S(Limits);
  take(S.enqueueWorkItem(work(1)));
  expectError(S.queueDPC(dpc(2)), "pending callback limit");
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  expectError(S.enqueueWorkItem(work(2)), "pending callback limit");
  EXPECT_TRUE(S.hasOutstanding(100));
  success(S.finish(Call->ID));
  EXPECT_TRUE(take(S.queueDPC(dpc(2))));
  EXPECT_FALSE(take(S.queueDPC(dpc(2))));
}

TEST(DriverKernelScheduler, DispatchBudgetPreservesUndispatchedCallbacks) {
  Scheduler::Limits Limits;
  Limits.MaxDispatches = 1;
  Scheduler S(Limits);
  take(S.enqueueWorkItem(work(1)));
  take(S.enqueueWorkItem(work(2)));
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  success(S.finish(Call->ID));
  expectError(S.next(), "dispatch limit");
  EXPECT_TRUE(S.isWorkItemQueued(2));
  EXPECT_TRUE(S.hasOutstanding(100));
  EXPECT_EQ(S.dispatchCount(), 1u);
}

TEST(DriverKernelScheduler, TimerBatchCapacityFailureIsAtomicAndRecoverable) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 1;
  Scheduler S(Limits);
  EXPECT_FALSE(take(S.setTimer(10, 100, -5, 0, dpc(1))));
  EXPECT_FALSE(take(S.setTimer(20, 100, -5, 0, dpc(2))));
  expectError(S.next(), "pending callback limit");
  EXPECT_EQ(S.now100ns(), 0u);
  EXPECT_EQ(S.timerExpirationCount(), 0u);
  EXPECT_FALSE(take(S.timerSignaled(10)));
  EXPECT_FALSE(take(S.timerSignaled(20)));
  EXPECT_TRUE(S.isTimerArmed(10));
  EXPECT_TRUE(S.isTimerArmed(20));
  EXPECT_TRUE(S.cancelTimer(20));
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->Object, 1u);
  EXPECT_EQ(S.now100ns(), 5u);
  success(S.finish(Call->ID));
}

TEST(DriverKernelScheduler, PeriodicTimerWithoutDPCStillConsumesFiniteBudget) {
  Scheduler::Limits Limits;
  Limits.MaxTimerExpirations = 2;
  Scheduler S(Limits);
  EXPECT_FALSE(take(S.setTimer(10, 100, -1, 1)));
  expectError(S.next(), "timer expiration limit");
  EXPECT_EQ(S.timerExpirationCount(), 2u);
  EXPECT_EQ(S.now100ns(), 10001u);
  EXPECT_TRUE(S.isTimerArmed(10));
  EXPECT_TRUE(S.hasOutstanding(100));
  EXPECT_TRUE(S.cancelTimer(10));
  EXPECT_FALSE(take(S.next()));
}

TEST(DriverKernelScheduler, TimerOverflowDoesNotResetAnExistingDeadline) {
  Scheduler::Limits Limits;
  Limits.InitialTime100ns = UINT64_MAX - 20;
  Scheduler S(Limits);
  EXPECT_FALSE(take(S.setTimer(10, 100, -10, 0, dpc(1))));
  expectError(S.setTimer(10, 100, -21, 0, dpc(2)), "overflows");
  expectError(S.setTimer(20, 100, INT64_MIN, 0), "overflows");
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->Object, 1u);
  EXPECT_EQ(S.now100ns(), UINT64_MAX - 10);
  success(S.finish(Call->ID));
}

TEST(DriverKernelScheduler, MostNegativeRelativeIntervalIsComputedWithoutUB) {
  Scheduler S;
  EXPECT_FALSE(take(S.setTimer(10, 100, INT64_MIN, 0, dpc(1))));
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  EXPECT_EQ(S.now100ns(), uint64_t(1) << 63);
  success(S.finish(Call->ID));
}

TEST(DriverKernelScheduler, PeriodicOverflowIsAnAtomicTimeFailure) {
  Scheduler::Limits Limits;
  Limits.InitialTime100ns = UINT64_MAX - 20;
  Scheduler S(Limits);
  EXPECT_FALSE(take(S.setTimer(10, 100, -10, 1, dpc(1))));
  expectError(S.next(), "overflows");
  EXPECT_EQ(S.now100ns(), Limits.InitialTime100ns);
  EXPECT_TRUE(S.isTimerArmed(10));
  EXPECT_FALSE(take(S.timerSignaled(10)));
  EXPECT_EQ(S.queuedCallbackCount(), 0u);
}

TEST(DriverKernelScheduler, TimeAndObjectLimitsRejectBeforeMutation) {
  Scheduler::Limits Limits;
  Limits.InitialTime100ns = 10;
  Limits.MaxTime100ns = 20;
  Limits.MaxTimers = 1;
  Scheduler S(Limits);
  expectError(S.setTimer(10, 100, -11, 0), "virtual time limit");
  EXPECT_FALSE(S.hasPending());
  EXPECT_FALSE(take(S.setTimer(10, 100, -10, 0)));
  expectError(S.setTimer(20, 100, -10, 0), "timer object limit");
  expectError(S.setTimer(10, 101, -1, 0), "owner cannot change");
  expectError(S.setTimer(10, 100, -1, UINT32_MAX), "Windows LONG");
  EXPECT_FALSE(take(S.next()));
  EXPECT_EQ(S.now100ns(), 20u);
  success(S.forgetTimer(10));
  EXPECT_FALSE(take(S.setTimer(20, 100, 20, 0)));
}

TEST(DriverKernelScheduler, InvalidCallbackAndTimeConfigurationFailClearly) {
  Scheduler S;
  auto Bad = work(1);
  Bad.Thread = 0;
  expectError(S.enqueueWorkItem(Bad), "nonzero");
  Bad = work(1);
  Bad.Arguments.resize(scheduler::MaxArguments + 1);
  expectError(S.enqueueWorkItem(Bad), "argument count");
  expectError(S.queueDPC(dpc(1, static_cast<Scheduler::DpcImportance>(99))),
              "importance");
  expectError(S.setTimer(10, 101, -1, 0, dpc(1)), "same owner");
  Scheduler::Limits Limits;
  Limits.InitialTime100ns = 2;
  Limits.MaxTime100ns = 1;
  Scheduler Invalid(Limits);
  expectError(Invalid.next(), "initial time");
  expectError(Invalid.enqueueWorkItem(work(1)), "initial time");
  expectError(Invalid.setTimer(1, 100, -1, 0), "initial time");
}

TEST(DriverKernelScheduler, SuspendedWorkerRetainsOwnershipAcrossNestedDPC) {
  Scheduler S;
  auto Original = work(1, 101);
  const uint64_t WorkerID = take(S.enqueueWorkItem(Original));
  auto Worker = take(S.next());
  ASSERT_TRUE(Worker);
  success(S.suspend(WorkerID));
  EXPECT_FALSE(S.active());
  EXPECT_EQ(S.suspendedCallbackCount(), 1u);
  EXPECT_EQ(S.queuedCallbackCount(), 0u);
  EXPECT_TRUE(S.hasPending());
  EXPECT_TRUE(S.hasOutstanding(101));
  EXPECT_FALSE(S.isWorkItemQueued(1));
  EXPECT_FALSE(S.cancelWorkItem(1));
  EXPECT_TRUE(take(S.queueDPC(dpc(2))));
  auto DPC = take(S.next());
  ASSERT_TRUE(DPC);
  EXPECT_EQ(DPC->Kind, Scheduler::CallbackKind::DPC);
  EXPECT_EQ(DPC->IRQL, scheduler::DispatchLevel);
  EXPECT_TRUE(S.hasOutstanding(101));
  EXPECT_TRUE(S.hasOutstanding(100));
  expectError(S.resume(WorkerID), "unfinished callback");
  success(S.finish(DPC->ID));
  success(S.resume(WorkerID));
  ASSERT_TRUE(S.active());
  EXPECT_EQ(S.active()->ID, WorkerID);
  EXPECT_EQ(S.active()->Object, Original.Object);
  EXPECT_EQ(S.active()->Owner, Original.Owner);
  EXPECT_EQ(S.active()->Thread, Original.Thread);
  EXPECT_EQ(S.active()->PC, Original.PC);
  EXPECT_EQ(S.active()->Arguments, Original.Arguments);
  EXPECT_EQ(S.active()->Kind, Scheduler::CallbackKind::WorkItem);
  EXPECT_EQ(S.active()->IRQL, scheduler::PassiveLevel);
  EXPECT_EQ(S.suspendedCallbackCount(), 0u);
  EXPECT_EQ(S.dispatchCount(), 2u);
  EXPECT_FALSE(S.hasOutstanding(100));
  success(S.finish(WorkerID));
  EXPECT_FALSE(S.hasOutstanding(101));
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverKernelScheduler, WaitPredicateChoosesWhichSuspendedWorkerResumes) {
  Scheduler S;
  const uint64_t FirstID = take(S.enqueueWorkItem(work(1, 101)));
  const uint64_t SecondID = take(S.enqueueWorkItem(work(2, 102)));
  ASSERT_TRUE(take(S.next()));
  success(S.suspend(FirstID));
  ASSERT_TRUE(take(S.next()));
  success(S.suspend(SecondID));
  EXPECT_EQ(S.suspendedCallbackCount(), 2u);
  ASSERT_NE(S.suspended(FirstID), nullptr);
  ASSERT_NE(S.suspended(SecondID), nullptr);
  EXPECT_FALSE(take(S.next(false)));
  // Readiness belongs to the wait model, not suspension order or nesting.
  success(S.resume(FirstID));
  success(S.finish(FirstID));
  EXPECT_EQ(S.suspended(FirstID), nullptr);
  EXPECT_FALSE(S.hasOutstanding(101));
  EXPECT_TRUE(S.hasOutstanding(102));
  success(S.resume(SecondID));
  success(S.finish(SecondID));
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverKernelScheduler, InvalidSuspendResumeAndFinishPreserveOwnership) {
  Scheduler S;
  expectError(S.suspend(99), "identity mismatch");
  expectError(S.resume(99), "identity mismatch");
  auto ID = take(S.enqueueWorkItem(work(1)));
  ASSERT_TRUE(take(S.next()));
  expectError(S.suspend(ID + 1), "identity mismatch");
  ASSERT_TRUE(S.active());
  EXPECT_EQ(S.active()->ID, ID);
  EXPECT_EQ(S.suspendedCallbackCount(), 0u);
  success(S.suspend(ID));
  expectError(S.suspend(ID), "identity mismatch");
  expectError(S.finish(ID), "identity mismatch");
  expectError(S.resume(ID + 1), "identity mismatch");
  EXPECT_FALSE(S.active());
  ASSERT_NE(S.suspended(ID), nullptr);
  EXPECT_TRUE(S.hasOutstanding(100));
  success(S.resume(ID));
  expectError(S.resume(ID), "unfinished callback");
  success(S.finish(ID));
  expectError(S.resume(ID), "identity mismatch");
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverKernelScheduler, SuspendedCallbacksConsumeCapacityUntilFinished) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 2;
  Scheduler S(Limits);
  const auto FirstID = take(S.enqueueWorkItem(work(1)));
  ASSERT_TRUE(take(S.next()));
  success(S.suspend(FirstID));
  // Requeueing the same dequeued work item is a new invocation, not a resume.
  const auto SecondID = take(S.enqueueWorkItem(work(1)));
  ASSERT_NE(FirstID, SecondID);
  ASSERT_TRUE(take(S.next()));
  success(S.suspend(SecondID));
  expectError(S.enqueueWorkItem(work(2)), "pending callback limit");
  expectError(S.queueDPC(dpc(3)), "pending callback limit");
  EXPECT_EQ(S.suspendedCallbackCount(), 2u);
  EXPECT_FALSE(S.cancelWorkItem(1));
  success(S.resume(FirstID));
  expectError(S.enqueueWorkItem(work(2)), "pending callback limit");
  success(S.finish(FirstID));
  EXPECT_TRUE(take(S.queueDPC(dpc(3))));
  EXPECT_TRUE(S.removeDPC(3));
  success(S.resume(SecondID));
  success(S.finish(SecondID));
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverKernelScheduler, ResumeDoesNotRedispatchOrConsumeDispatchBudget) {
  Scheduler::Limits Limits;
  Limits.MaxDispatches = 1;
  Scheduler S(Limits);
  const auto ID = take(S.enqueueWorkItem(work(1)));
  ASSERT_TRUE(take(S.next()));
  for (unsigned I = 0; I < 3; ++I) {
    success(S.suspend(ID));
    success(S.resume(ID));
  }
  EXPECT_EQ(S.dispatchCount(), 1u);
  success(S.finish(ID));
}

TEST(DriverKernelScheduler, SuspendedTimerDPCRetainsReleasePreflightOwnership) {
  Scheduler S;
  EXPECT_FALSE(take(S.setTimer(10, 100, -5, 0, dpc(1))));
  expectError(S.canForgetTimer(10), "outstanding");
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  success(S.canForgetTimer(10));
  EXPECT_TRUE(take(S.timerSignaled(10))); // Preflight does not release.
  success(S.suspend(Call->ID));
  expectError(S.canForgetTimer(10), "outstanding");
  expectError(S.forgetTimer(10), "outstanding");
  EXPECT_FALSE(S.removeDPC(1));
  EXPECT_FALSE(S.cancelTimer(10));
  EXPECT_TRUE(S.hasOutstanding(100));
  auto Saved = S.suspended(Call->ID);
  ASSERT_NE(Saved, nullptr);
  EXPECT_EQ(Saved->SourceTimer, 10u);
  EXPECT_EQ(Saved->DueTime100ns, 5u);
  EXPECT_EQ(Saved->IRQL, scheduler::DispatchLevel);
  success(S.resume(Call->ID));
  success(S.canForgetTimer(10));
  success(S.forgetTimer(10));
  expectError(S.canForgetTimer(10), "unknown");
  EXPECT_TRUE(S.hasOutstanding(100));
  success(S.finish(Call->ID));
  EXPECT_FALSE(S.hasOutstanding(100));
}

TEST(DriverKernelScheduler, DeadlineConversionIsSharedCheckedAndNonmutating) {
  Scheduler::Limits Limits;
  Limits.InitialTime100ns = 10;
  Limits.MaxTime100ns = 100;
  Scheduler S(Limits);
  EXPECT_EQ(take(S.computeDeadline(-5)), 15u);
  EXPECT_EQ(take(S.computeDeadline(50)), 50u);
  EXPECT_EQ(take(S.computeDeadline(0)), 0u);
  expectError(S.computeDeadline(-91), "virtual time limit");
  expectError(S.computeDeadline(101), "virtual time limit");
  EXPECT_EQ(S.now100ns(), 10u);
  EXPECT_FALSE(S.hasPending());
  Scheduler Unbounded;
  EXPECT_EQ(take(Unbounded.computeDeadline(INT64_MIN)), uint64_t(1) << 63);
  Limits.InitialTime100ns = UINT64_MAX - 1;
  Limits.MaxTime100ns = UINT64_MAX;
  Scheduler Overflow(Limits);
  expectError(Overflow.computeDeadline(-2), "overflows");
}

TEST(DriverKernelScheduler, DeadlineCanAdvanceIdleClockWithoutScheduledTimers) {
  Scheduler S;
  EXPECT_FALSE(S.nextEventTime100ns());
  EXPECT_FALSE(take(S.next(true, 100)));
  EXPECT_EQ(S.now100ns(), 100u);
  EXPECT_FALSE(S.hasPending());
  EXPECT_EQ(S.timerExpirationCount(), 0u);
  EXPECT_FALSE(take(S.next(true, 50)));
  EXPECT_EQ(S.now100ns(), 100u);
  EXPECT_FALSE(take(S.next(false, 200)));
  EXPECT_EQ(S.now100ns(), 100u);
}

TEST(DriverKernelScheduler, WaitDeadlinePrecedesFutureTimerWithoutLosingIt) {
  Scheduler S;
  EXPECT_FALSE(take(S.setTimer(10, 100, 100, 0, dpc(1))));
  EXPECT_EQ(S.nextEventTime100ns(), 100u);
  EXPECT_FALSE(take(S.next(true, 40)));
  EXPECT_EQ(S.now100ns(), 40u);
  EXPECT_EQ(S.timerExpirationCount(), 0u);
  EXPECT_TRUE(S.isTimerArmed(10));
  EXPECT_FALSE(take(S.timerSignaled(10)));
  auto Call = take(S.next(true, 100));
  ASSERT_TRUE(Call);
  EXPECT_EQ(S.now100ns(), 100u);
  EXPECT_EQ(Call->SourceTimer, 10u);
  EXPECT_TRUE(take(S.timerSignaled(10)));
  success(S.finish(Call->ID));
  EXPECT_FALSE(S.nextEventTime100ns());
}

TEST(DriverKernelScheduler, SignalOnlyExpiryYieldsBeforeLaterWaitDeadline) {
  Scheduler S;
  EXPECT_FALSE(take(S.setTimer(10, 100, 25, 0)));
  EXPECT_FALSE(take(S.setTimer(20, 100, 50, 0, dpc(1))));
  EXPECT_FALSE(take(S.next(true, 100)));
  EXPECT_EQ(S.now100ns(), 25u);
  EXPECT_TRUE(take(S.consumeTimerSignal(10)));
  EXPECT_FALSE(take(S.timerSignaled(20)));
  EXPECT_EQ(S.nextEventTime100ns(), 50u);
  auto Call = take(S.next(true, 100));
  ASSERT_TRUE(Call);
  EXPECT_EQ(S.now100ns(), 50u);
  success(S.finish(Call->ID));
  EXPECT_FALSE(take(S.next(true, 100)));
  EXPECT_EQ(S.now100ns(), 100u);
}

TEST(DriverKernelScheduler, AlreadyDueSignalOnlyExpiryDoesNotOvershootWait) {
  Scheduler::Limits Limits;
  Limits.InitialTime100ns = 100;
  Scheduler S(Limits);
  EXPECT_FALSE(take(S.setTimer(10, 100, 50, 0)));
  EXPECT_EQ(S.nextEventTime100ns(), 100u);
  EXPECT_FALSE(take(S.next(true, 200)));
  EXPECT_EQ(S.now100ns(), 100u);
  EXPECT_TRUE(take(S.consumeTimerSignal(10)));
  EXPECT_FALSE(take(S.next(true, 200)));
  EXPECT_EQ(S.now100ns(), 200u);
}

TEST(DriverKernelScheduler, PeriodicSignalOnlyTimerYieldsBetweenEveryExpiry) {
  Scheduler S;
  EXPECT_FALSE(take(S.setTimer(10, 100, -1, 1)));
  for (uint64_t Time : {1, 10001, 20001}) {
    EXPECT_FALSE(take(S.next(true, 25000)));
    EXPECT_EQ(S.now100ns(), Time);
    EXPECT_TRUE(take(S.consumeTimerSignal(10)));
  }
  EXPECT_FALSE(take(S.next(true, 25000)));
  EXPECT_EQ(S.now100ns(), 25000u);
  EXPECT_EQ(S.timerExpirationCount(), 3u);
  EXPECT_EQ(S.nextEventTime100ns(), 30001u);
  EXPECT_FALSE(take(S.timerSignaled(10)));
}

TEST(DriverKernelScheduler, SuspendedWaitRetainsCapacityWhenTimerBatchExpires) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 1;
  Scheduler S(Limits);
  const auto ID = take(S.enqueueWorkItem(work(1)));
  ASSERT_TRUE(take(S.next()));
  success(S.suspend(ID));
  EXPECT_FALSE(take(S.setTimer(10, 100, 5, 0, dpc(2))));
  expectError(S.next(true, 10), "pending callback limit");
  EXPECT_EQ(S.now100ns(), 0u);
  EXPECT_EQ(S.timerExpirationCount(), 0u);
  ASSERT_NE(S.suspended(ID), nullptr);
  EXPECT_FALSE(take(S.timerSignaled(10)));
  success(S.resume(ID));
  success(S.finish(ID));
  auto Call = take(S.next(true, 10));
  ASSERT_TRUE(Call);
  EXPECT_EQ(S.now100ns(), 5u);
  success(S.finish(Call->ID));
}

TEST(DriverKernelScheduler, InvalidWaitDeadlineCannotMutateTimeOrReadyQueue) {
  Scheduler::Limits Limits;
  Limits.MaxTime100ns = 100;
  Scheduler S(Limits);
  const auto ID = take(S.enqueueWorkItem(work(1)));
  EXPECT_FALSE(take(S.setTimer(10, 100, 0, 0)));
  expectError(S.next(true, 101), "virtual time limit");
  EXPECT_EQ(S.now100ns(), 0u);
  EXPECT_EQ(S.dispatchCount(), 0u);
  EXPECT_TRUE(S.isWorkItemQueued(1));
  EXPECT_FALSE(take(S.timerSignaled(10)));
  auto Call = take(S.next(false));
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->ID, ID);
  EXPECT_TRUE(take(S.timerSignaled(10)));
  success(S.finish(ID));
}

TEST(DriverKernelScheduler, DueTimersCanSignalWithoutPreemptingActiveCallback) {
  Scheduler S;
  const auto ID = take(S.enqueueWorkItem(work(1)));
  ASSERT_TRUE(take(S.next()));
  EXPECT_FALSE(take(S.setTimer(10, 100, 0, 0)));
  EXPECT_FALSE(take(S.setTimer(20, 100, 0, 0, dpc(2))));
  EXPECT_FALSE(take(S.setTimer(30, 100, -10, 0, dpc(3))));
  success(S.processDueTimers());
  ASSERT_TRUE(S.active());
  EXPECT_EQ(S.active()->ID, ID);
  EXPECT_EQ(S.now100ns(), 0u);
  EXPECT_TRUE(take(S.timerSignaled(10)));
  EXPECT_TRUE(take(S.timerSignaled(20)));
  EXPECT_FALSE(take(S.timerSignaled(30)));
  EXPECT_TRUE(S.isDPCQueued(2));
  EXPECT_FALSE(S.isDPCQueued(3));
  EXPECT_EQ(S.dispatchCount(), 1u);
  EXPECT_EQ(S.timerExpirationCount(), 2u);
  success(S.processDueTimers());
  EXPECT_EQ(S.timerExpirationCount(), 2u);
  success(S.finish(ID));
  auto Call = take(S.next(false));
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->Object, 2u);
  EXPECT_EQ(S.now100ns(), 0u);
  success(S.finish(Call->ID));
}

TEST(DriverKernelScheduler, DueTimerFailureRetainsActiveCallbackAndTimerBatch) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 1;
  Scheduler S(Limits);
  const auto ID = take(S.enqueueWorkItem(work(1)));
  ASSERT_TRUE(take(S.next()));
  EXPECT_FALSE(take(S.setTimer(10, 100, 0, 0)));
  EXPECT_FALSE(take(S.setTimer(20, 100, 0, 0, dpc(2))));
  expectError(S.processDueTimers(), "pending callback limit");
  ASSERT_TRUE(S.active());
  EXPECT_EQ(S.active()->ID, ID);
  EXPECT_EQ(S.timerExpirationCount(), 0u);
  EXPECT_EQ(S.queuedCallbackCount(), 0u);
  EXPECT_FALSE(take(S.timerSignaled(10)));
  EXPECT_FALSE(take(S.timerSignaled(20)));
  EXPECT_TRUE(S.isTimerArmed(10));
  EXPECT_TRUE(S.isTimerArmed(20));
  success(S.finish(ID));
  success(S.processDueTimers());
  EXPECT_TRUE(take(S.timerSignaled(10)));
  EXPECT_TRUE(take(S.timerSignaled(20)));
  EXPECT_FALSE(S.active());
  EXPECT_TRUE(S.isDPCQueued(2));
}

TEST(DriverKernelScheduler, ProcessingDuePeriodicTimerNeverAdvancesIdleTime) {
  Scheduler S;
  EXPECT_FALSE(take(S.setTimer(10, 100, 0, 1)));
  success(S.processDueTimers());
  EXPECT_TRUE(take(S.consumeTimerSignal(10)));
  EXPECT_EQ(S.nextEventTime100ns(), scheduler::TicksPerMillisecond);
  success(S.processDueTimers());
  EXPECT_FALSE(take(S.timerSignaled(10)));
  EXPECT_EQ(S.now100ns(), 0u);
  EXPECT_EQ(S.timerExpirationCount(), 1u);
}

TEST(DriverKernelScheduler, RearmingTimerCannotEraseActivePeriodicProvenance) {
  Scheduler S;
  EXPECT_FALSE(take(S.setTimer(10, 100, -1, 1, dpc(1))));
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  EXPECT_TRUE(Call->SourceTimerPeriodic);
  EXPECT_TRUE(take(S.setTimer(10, 100, -5, 0, dpc(2))));
  EXPECT_TRUE(S.cancelTimer(10));
  expectError(S.canForgetTimer(10), "outstanding");
  expectError(S.forgetTimer(10), "outstanding");
  ASSERT_TRUE(S.active());
  EXPECT_EQ(S.active()->ID, Call->ID);
  success(S.finish(Call->ID));
  success(S.forgetTimer(10));
}

TEST(DriverKernelScheduler, RearmingTimerCannotInventActivePeriodicProvenance) {
  Scheduler S;
  EXPECT_FALSE(take(S.setTimer(10, 100, -1, 0, dpc(1))));
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  EXPECT_FALSE(Call->SourceTimerPeriodic);
  EXPECT_FALSE(take(S.setTimer(10, 100, -5, 1, dpc(2))));
  EXPECT_TRUE(S.cancelTimer(10));
  success(S.canForgetTimer(10));
  success(S.forgetTimer(10));
  EXPECT_TRUE(S.hasOutstanding(100));
  success(S.finish(Call->ID));
  EXPECT_FALSE(S.hasOutstanding(100));
}

TEST(DriverKernelScheduler,
     FrameworkCancellationHasAnIndependentObjectNamespace) {
  Scheduler S;
  const auto CancelID = take(S.enqueueFrameworkCancel(work(1, 101)));
  EXPECT_TRUE(S.hasQueuedFrameworkCancel());
  EXPECT_FALSE(S.isWorkItemQueued(1));
  EXPECT_FALSE(S.cancelWorkItem(1));
  EXPECT_FALSE(S.isDPCQueued(1));
  EXPECT_FALSE(S.removeDPC(1));
  take(S.enqueueWorkItem(work(1, 102)));
  EXPECT_TRUE(take(S.queueDPC(dpc(1))));
  EXPECT_EQ(S.queuedCallbackCount(), 3u);
  EXPECT_TRUE(S.removeDPC(1));
  EXPECT_TRUE(S.cancelWorkItem(1));
  EXPECT_EQ(S.queuedCallbackCount(), 1u);
  EXPECT_FALSE(S.hasOutstanding(100));
  EXPECT_FALSE(S.hasOutstanding(102));
  EXPECT_TRUE(S.hasOutstanding(101));
  auto Call = take(S.next(false));
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->ID, CancelID);
  EXPECT_EQ(Call->Kind, Scheduler::CallbackKind::FrameworkCancel);
  EXPECT_FALSE(S.hasQueuedFrameworkCancel());
  success(S.finish(Call->ID));
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverKernelScheduler, DPCsPrecedeCancellationFIFOWhichPrecedesWorkers) {
  Scheduler S;
  const auto WorkerID = take(S.enqueueWorkItem(work(1, 101)));
  auto First = work(2, 102);
  First.Thread = 300;
  First.PC = 0x180003000;
  First.Arguments = {0x1234};
  const auto FirstID = take(S.enqueueFrameworkCancel(First));
  const auto SecondID = take(S.enqueueFrameworkCancel(work(3, 103)));
  EXPECT_TRUE(take(S.queueDPC(dpc(4))));
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->Kind, Scheduler::CallbackKind::DPC);
  EXPECT_EQ(Call->IRQL, 2u);
  success(S.finish(Call->ID));
  Call = take(S.next());
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->ID, FirstID);
  EXPECT_EQ(Call->Object, First.Object);
  EXPECT_EQ(Call->Owner, First.Owner);
  EXPECT_EQ(Call->Thread, First.Thread);
  EXPECT_EQ(Call->PC, First.PC);
  EXPECT_EQ(Call->Arguments, First.Arguments);
  EXPECT_EQ(Call->Kind, Scheduler::CallbackKind::FrameworkCancel);
  EXPECT_EQ(Call->IRQL, 0u);
  success(S.finish(Call->ID));
  Call = take(S.next());
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->ID, SecondID);
  success(S.finish(Call->ID));
  Call = take(S.next());
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->ID, WorkerID);
  EXPECT_EQ(Call->Kind, Scheduler::CallbackKind::WorkItem);
  success(S.finish(Call->ID));
  EXPECT_FALSE(S.hasPending());
  EXPECT_EQ(S.dispatchCount(), 4u);
}

TEST(DriverKernelScheduler, WDMCancellationRunsAtDispatchBeforeItsWorker) {
  Scheduler S;
  const auto Worker = take(S.enqueueWorkItem(work(10, 101)));
  const auto Cancel = take(S.enqueueWDMCancellation(work(11, 101)));
  EXPECT_TRUE(S.hasQueuedCancellation());
  EXPECT_FALSE(S.hasQueuedFrameworkCancel());
  expectError(S.canEnqueueWDMCancellations({work(11, 101)}), "already queued");
  auto Call = take(S.next(false));
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->ID, Cancel);
  EXPECT_EQ(Call->Kind, Scheduler::CallbackKind::WDMCancel);
  EXPECT_EQ(Call->IRQL, 2u);
  success(S.finish(Call->ID));
  Call = take(S.next(false));
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->ID, Worker);
  EXPECT_EQ(Call->IRQL, 0u);
  success(S.finish(Call->ID));
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverKernelScheduler,
     DuplicateQueuedCancellationCannotReplaceItsDelivery) {
  Scheduler S;
  const auto ID = take(S.enqueueFrameworkCancel(work(1, 101)));
  auto Replacement = work(1, 102);
  Replacement.Arguments = {0x9999};
  expectError(S.enqueueFrameworkCancel(Replacement), "already queued");
  EXPECT_EQ(S.queuedCallbackCount(), 1u);
  EXPECT_FALSE(S.hasOutstanding(102));
  auto Call = take(S.next(false));
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->ID, ID);
  EXPECT_EQ(Call->Owner, 101u);
  EXPECT_EQ(Call->Arguments, (std::vector<uint64_t>{11, 21}));
  success(S.finish(Call->ID));
}

TEST(DriverKernelScheduler, CancellationAdmissionSharesAllCallbackCapacity) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 1;
  Scheduler S(Limits);
  take(S.enqueueWorkItem(work(1)));
  expectError(S.enqueueFrameworkCancel(work(2, 102)), "pending callback limit");
  EXPECT_FALSE(S.hasOutstanding(102));
  EXPECT_TRUE(S.cancelWorkItem(1));
  const auto ID = take(S.enqueueFrameworkCancel(work(2, 102)));
  expectError(S.enqueueWorkItem(work(3)), "pending callback limit");
  auto Call = take(S.next());
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->ID, ID);
  expectError(S.enqueueFrameworkCancel(work(4)), "pending callback limit");
  success(S.suspend(ID));
  EXPECT_EQ(S.queuedCallbackCount(), 0u);
  EXPECT_EQ(S.suspendedCallbackCount(), 1u);
  EXPECT_TRUE(S.hasPending());
  EXPECT_TRUE(S.hasOutstanding(102));
  expectError(S.queueDPC(dpc(5)), "pending callback limit");
  success(S.resume(ID));
  success(S.finish(ID));
  EXPECT_FALSE(S.hasOutstanding(102));
  EXPECT_TRUE(take(S.enqueueWorkItem(work(6))));
}

TEST(DriverKernelScheduler,
     SuspendedCancellationKeepsKindAcrossInterveningWorker) {
  Scheduler S;
  const auto CancelID = take(S.enqueueFrameworkCancel(work(1, 101)));
  const auto WorkerID = take(S.enqueueWorkItem(work(2, 102)));
  auto Call = take(S.next(false));
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->ID, CancelID);
  success(S.suspend(CancelID));
  EXPECT_FALSE(S.hasQueuedFrameworkCancel());
  ASSERT_NE(S.suspended(CancelID), nullptr);
  EXPECT_EQ(S.suspended(CancelID)->Kind,
            Scheduler::CallbackKind::FrameworkCancel);
  EXPECT_EQ(S.suspended(CancelID)->Arguments, (std::vector<uint64_t>{11, 21}));
  Call = take(S.next(false));
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->ID, WorkerID);
  EXPECT_EQ(Call->Kind, Scheduler::CallbackKind::WorkItem);
  EXPECT_TRUE(S.hasOutstanding(101));
  expectError(S.resume(CancelID), "unfinished callback");
  success(S.finish(WorkerID));
  EXPECT_FALSE(take(S.next(false)));
  EXPECT_TRUE(S.hasPending());
  success(S.resume(CancelID));
  ASSERT_TRUE(S.active());
  EXPECT_EQ(S.active()->ID, CancelID);
  EXPECT_EQ(S.active()->Object, 1u);
  EXPECT_EQ(S.active()->Owner, 101u);
  EXPECT_EQ(S.active()->Thread, 200u);
  EXPECT_EQ(S.active()->Kind, Scheduler::CallbackKind::FrameworkCancel);
  EXPECT_EQ(S.active()->IRQL, 0u);
  EXPECT_FALSE(S.cancelWorkItem(1));
  EXPECT_EQ(S.dispatchCount(), 2u);
  success(S.finish(CancelID));
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverKernelScheduler, ReadyCancellationPreventsAdvancingToFutureTimer) {
  Scheduler S;
  const auto ID = take(S.enqueueFrameworkCancel(work(1)));
  EXPECT_FALSE(take(S.setTimer(10, 100, -50, 0, dpc(2))));
  auto Call = take(S.next(true, 100));
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->ID, ID);
  EXPECT_EQ(Call->DueTime100ns, 0u);
  EXPECT_EQ(Call->SourceTimer, 0u);
  EXPECT_EQ(S.now100ns(), 0u);
  success(S.finish(ID));
  Call = take(S.next());
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->Kind, Scheduler::CallbackKind::DPC);
  EXPECT_EQ(Call->SourceTimer, 10u);
  EXPECT_EQ(S.now100ns(), 50u);
  success(S.finish(Call->ID));
}

TEST(DriverKernelScheduler,
     CancellationDispatchExhaustionKeepsQueuedOwnership) {
  Scheduler::Limits Limits;
  Limits.MaxDispatches = 1;
  Scheduler S(Limits);
  const auto ID = take(S.enqueueFrameworkCancel(work(1, 101)));
  take(S.enqueueFrameworkCancel(work(2, 102)));
  ASSERT_TRUE(take(S.next(false)));
  success(S.finish(ID));
  expectError(S.next(false), "dispatch limit");
  EXPECT_EQ(S.queuedCallbackCount(), 1u);
  EXPECT_TRUE(S.hasOutstanding(102));
  EXPECT_FALSE(S.hasOutstanding(101));
  EXPECT_EQ(S.dispatchCount(), 1u);
  EXPECT_FALSE(S.active());
}

TEST(DriverKernelScheduler, InvalidCancellationMetadataCannotRetainOwnership) {
  Scheduler S;
  auto Invalid = work(1, 101);
  Invalid.Thread = 0;
  expectError(S.enqueueFrameworkCancel(Invalid), "nonzero");
  Invalid = work(1, 101);
  Invalid.Arguments.resize(scheduler::MaxArguments + 1);
  expectError(S.enqueueFrameworkCancel(Invalid), "argument count");
  EXPECT_FALSE(S.hasOutstanding(101));
  EXPECT_FALSE(S.hasPending());
  EXPECT_EQ(S.queuedCallbackCount(), 0u);
}

TEST(DriverKernelScheduler,
     WDMCompletionPriorityAndObjectNamespacesAreIndependent) {
  Scheduler S;
  const auto Worker = take(S.enqueueWorkItem(work(7)));
  const auto First = take(S.enqueueWDMCompletion(work(7, 101)));
  const auto Second = take(S.enqueueWDMCompletion(work(8, 102)));
  const auto Cancel = take(S.enqueueFrameworkCancel(work(7, 103)));
  EXPECT_TRUE(take(S.queueDPC(dpc(7))));
  EXPECT_TRUE(S.hasQueuedCompletion());
  auto Call = take(S.next(false));
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->Kind, Scheduler::CallbackKind::DPC);
  success(S.finish(Call->ID));
  for (auto ID : {Cancel, First, Second, Worker}) {
    Call = take(S.next(false));
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->ID, ID);
    EXPECT_EQ(Call->IRQL, 0u);
    if (ID == First || ID == Second)
      EXPECT_EQ(Call->Kind, Scheduler::CallbackKind::WDMCompletion);
    success(S.finish(ID));
  }
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverKernelScheduler, WDMCompletionBatchAdmissionDoesNotReserveOrMutate) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 2;
  Scheduler S(Limits);
  take(S.enqueueWorkItem(work(1)));
  expectError(S.canEnqueueCompletions({work(2), work(3)}), "pending callback");
  EXPECT_EQ(S.queuedCallbackCount(), 1u);
  EXPECT_FALSE(S.hasQueuedCompletion());
  success(S.canEnqueueCompletions({work(2)}));
  success(S.canEnqueueCompletions({work(2)}));
  EXPECT_EQ(take(S.enqueueWDMCompletion(work(2))), 2u);
  expectError(S.enqueueWDMCompletion(work(3)), "pending callback");
  EXPECT_TRUE(S.cancelWorkItem(1));
  EXPECT_FALSE(S.cancelWorkItem(2));
  EXPECT_FALSE(S.isWorkItemQueued(2));
  EXPECT_TRUE(S.hasQueuedCompletion());
}

TEST(DriverKernelScheduler, WDMCompletionBatchRejectsDuplicatesAndInvalidPC) {
  Scheduler S;
  expectError(S.canEnqueueCompletions({work(1), work(1)}), "already queued");
  auto Invalid = work(1);
  Invalid.PC = 0;
  expectError(S.canEnqueueCompletions({work(2), Invalid}), "nonzero");
  EXPECT_FALSE(S.hasPending());
  const auto ID = take(S.enqueueWDMCompletion(work(1)));
  expectError(S.canEnqueueCompletions({work(1)}), "already queued");
  EXPECT_EQ(ID, 1u);
  EXPECT_EQ(S.queuedCallbackCount(), 1u);
}

TEST(DriverKernelScheduler,
     WDMCompletionSuspensionRetainsKindCapacityAndOwner) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 2;
  Scheduler S(Limits);
  const auto Completion = take(S.enqueueWDMCompletion(work(1, 101)));
  const auto Worker = take(S.enqueueWorkItem(work(2, 102)));
  ASSERT_TRUE(take(S.next(false)));
  success(S.suspend(Completion));
  EXPECT_FALSE(S.hasQueuedCompletion());
  EXPECT_TRUE(S.hasOutstanding(101));
  expectError(S.canEnqueueCompletions({work(3)}), "pending callback");
  auto Call = take(S.next(false));
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->ID, Worker);
  success(S.finish(Worker));
  success(S.resume(Completion));
  ASSERT_TRUE(S.active());
  EXPECT_EQ(S.active()->Kind, Scheduler::CallbackKind::WDMCompletion);
  EXPECT_EQ(S.active()->IRQL, 0u);
  EXPECT_EQ(S.active()->Arguments, (std::vector<uint64_t>{11, 21}));
  success(S.finish(Completion));
  EXPECT_FALSE(S.hasOutstanding(101));
}

TEST(DriverKernelScheduler,
     WDMCompletionDispatchLimitPreservesQueuedOwnership) {
  Scheduler::Limits Limits;
  Limits.MaxDispatches = 0;
  Scheduler S(Limits);
  take(S.enqueueWDMCompletion(work(1, 101)));
  expectError(S.next(false), "dispatch limit");
  EXPECT_TRUE(S.hasQueuedCompletion());
  EXPECT_TRUE(S.hasOutstanding(101));
  EXPECT_FALSE(S.active());
}

} // namespace
} // namespace neverd::emulation
