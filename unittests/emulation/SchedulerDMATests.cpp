//===- SchedulerDMATests.cpp - Reserved DMA callback execution ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource waiting, atomic admission and nested callback ownership share the
/// scheduler's real capacity and dispatch limits without emulating KDPCs.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "windows/KernelScheduler.h"

#include <string>
#include <utility>

namespace neverd::emulation {
namespace {
using Scheduler = KernelScheduler;

Scheduler::Callback callback(uint64_t Object, uint64_t Owner = 100) {
  return {Object, Owner, 200, 0x180001000, {Owner, 0, 0x6000, 0x7000}};
}

Scheduler::DpcCallback dpc(uint64_t Object) {
  Scheduler::DpcCallback Result;
  static_cast<Scheduler::Callback &>(Result) = callback(Object);
  return Result;
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

void fails(llvm::Error E, llvm::StringRef Part) {
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find(Part.str()), std::string::npos);
}

template <class T> void fails(llvm::Expected<T> Result, llvm::StringRef Part) {
  ASSERT_FALSE(bool(Result));
  fails(Result.takeError(), Part);
}

TEST(DriverSchedulerDMA, ResourceWaitDoesNotInventADeadlineOrReadyProducer) {
  Scheduler S;
  const auto ID = take(S.reserveDMAListControl(callback(1)));
  EXPECT_TRUE(S.hasPending());
  EXPECT_TRUE(S.hasOutstanding(100));
  EXPECT_FALSE(S.hasQueuedDMAListControl());
  EXPECT_EQ(S.queuedCallbackCount(), 0u);
  EXPECT_FALSE(S.nextEventTime100ns());
  EXPECT_FALSE(take(S.next(true)));
  EXPECT_EQ(S.now100ns(), 0u);
  EXPECT_EQ(S.dispatchCount(), 0u);
  // An explicit unrelated timeout may advance time, but cannot grant DMA.
  EXPECT_FALSE(take(S.next(true, 19)));
  EXPECT_EQ(S.now100ns(), 19u);
  EXPECT_FALSE(S.hasQueuedDMAListControl());
  success(S.readyDMAListControls({ID}));
  const auto Call = take(S.next(false));
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->ID, ID);
  EXPECT_EQ(Call->DueTime100ns, 19u);
  EXPECT_EQ(Call->Kind, Scheduler::CallbackKind::DMAListControl);
  EXPECT_EQ(Call->IRQL, 2);
  EXPECT_EQ(Call->Arguments, callback(1).Arguments);
  success(S.finish(ID));
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverSchedulerDMA, PromotionUsesItsReservedCapacityAndIdentity) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 3;
  Scheduler S(Limits);
  const auto Waiting = take(S.reserveDMAListControl(callback(1)));
  const auto Worker = take(S.enqueueWorkItem(callback(2)));
  EXPECT_TRUE(take(S.queueDPC(dpc(3))));
  fails(S.reserveDMAListControl(callback(4)), "pending callback limit");
  success(S.canReadyDMAListControls({Waiting}));
  success(S.readyDMAListControls({Waiting}));
  EXPECT_EQ(S.queuedCallbackCount(), 3u);
  const auto DPC = take(S.next(false));
  ASSERT_TRUE(DPC);
  EXPECT_EQ(DPC->Kind, Scheduler::CallbackKind::DPC);
  success(S.finish(DPC->ID));
  const auto DMA = take(S.next(false));
  ASSERT_TRUE(DMA);
  EXPECT_EQ(DMA->ID, Waiting);
  success(S.finish(DMA->ID));
  const auto Last = take(S.next(false));
  ASSERT_TRUE(Last);
  EXPECT_EQ(Last->ID, Worker);
  success(S.finish(Last->ID));
  EXPECT_EQ(take(S.reserveDMAListControl(callback(4))), 4u);
}

TEST(DriverSchedulerDMA, InterruptDpcDmaAndPassiveOrderIsExplicit) {
  Scheduler S;
  take(S.enqueueWorkItem(callback(1)));
  take(S.enqueueWDMCompletion(callback(2)));
  take(S.enqueueFrameworkCancel(callback(3)));
  const auto DMA = take(S.reserveDMAListControl(callback(4)));
  success(S.readyDMAListControls({DMA}));
  EXPECT_TRUE(take(S.queueDPC(dpc(5))));
  Scheduler::InterruptCallback IRQ;
  static_cast<Scheduler::Callback &>(IRQ) = callback(6);
  IRQ.IRQL = IRQ.Priority = 5;
  take(S.enqueueInterrupt(IRQ));
  for (uint64_t Object : {6, 5, 4, 3, 2, 1}) {
    const auto Call = take(S.next(false));
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->Object, Object);
    EXPECT_EQ(Call->IRQL, Object == 6 ? 5 : Object >= 4 ? 2 : 0);
    success(S.finish(Call->ID));
  }
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverSchedulerDMA, RepeatedObjectsNeverCoalesceOrAliasOtherQueues) {
  Scheduler S;
  const auto First = take(S.reserveDMAListControl(callback(1)));
  const auto Second = take(S.reserveDMAListControl(callback(1)));
  EXPECT_NE(First, Second);
  take(S.enqueueWorkItem(callback(1)));
  EXPECT_TRUE(take(S.queueDPC(dpc(1))));
  EXPECT_TRUE(S.cancelWorkItem(1));
  EXPECT_TRUE(S.removeDPC(1));
  EXPECT_FALSE(S.cancelWorkItem(1));
  EXPECT_FALSE(S.removeDPC(1));
  success(S.readyDMAListControls({Second, First}));
  for (uint64_t ID : {Second, First}) {
    const auto Call = take(S.next(false));
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->ID, ID); // FIFO follows actual readiness insertion.
    EXPECT_EQ(Call->Object, 1u);
    success(S.finish(ID));
  }
}

TEST(DriverSchedulerDMA, AdmissionBatchFailureDoesNotPartiallyPromote) {
  Scheduler S;
  const auto First = take(S.reserveDMAListControl(callback(1, 101)));
  const auto Second = take(S.reserveDMAListControl(callback(2, 102)));
  fails(S.readyDMAListControls({First, UINT64_MAX}), "not waiting");
  fails(S.readyDMAListControls({First, Second, First}), "duplicate");
  EXPECT_FALSE(S.hasQueuedDMAListControl());
  EXPECT_EQ(S.queuedCallbackCount(), 0u);
  EXPECT_EQ(S.dispatchCount(), 0u);
  EXPECT_EQ(S.now100ns(), 0u);
  EXPECT_TRUE(S.hasOutstanding(101));
  EXPECT_TRUE(S.hasOutstanding(102));
  success(S.canReadyDMAListControls({First, Second}));
  EXPECT_FALSE(S.hasQueuedDMAListControl());
  success(S.readyDMAListControls({First, Second}));
  fails(S.readyDMAListControls({First}), "not waiting");
  EXPECT_EQ(S.queuedCallbackCount(), 2u);
  EXPECT_EQ(take(S.reserveDMAListControl(callback(3))), Second + 1);
}

TEST(DriverSchedulerDMA, NonwaitingIdentitiesCannotBePromotedOrStartedAgain) {
  Scheduler S;
  const auto Ready = take(S.reserveDMAListControl(callback(1)));
  const auto Inline = take(S.reserveDMAListControl(callback(2)));
  const auto Held = take(S.reserveDMAListControl(callback(3)));
  success(S.readyDMAListControls({Ready}));
  const auto Call = take(S.next(false));
  ASSERT_TRUE(Call);
  success(S.beginInlineDMAListControl(Inline));
  for (uint64_t ID : {Ready, Inline, uint64_t(0), UINT64_MAX}) {
    fails(S.readyDMAListControls({Held, ID}), "not waiting");
    fails(S.beginInlineDMAListControl(ID), "not waiting");
  }
  success(S.canReadyDMAListControls({Held}));
  EXPECT_EQ(S.dispatchCount(), 2u);
  success(S.finishInlineDMAListControl(Inline));
  success(S.suspend(Ready));
  fails(S.readyDMAListControls({Ready}), "not waiting");
  success(S.resume(Ready));
  success(S.finish(Ready));
  fails(S.beginInlineDMAListControl(Ready), "not waiting");
}

TEST(DriverSchedulerDMA, NestedInlineCallbacksRetainTheirScheduledParent) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 3;
  Scheduler S(Limits);
  const auto Parent = take(S.enqueueWorkItem(callback(1, 101)));
  const auto First = take(S.reserveDMAListControl(callback(2, 102)));
  const auto Second = take(S.reserveDMAListControl(callback(3, 103)));
  ASSERT_TRUE(take(S.next(false)));
  success(S.canBeginInlineDMAListControl(First));
  EXPECT_EQ(S.dispatchCount(), 1u);
  success(S.beginInlineDMAListControl(First));
  success(S.beginInlineDMAListControl(Second));
  ASSERT_TRUE(S.active());
  EXPECT_EQ(S.active()->ID, Parent);
  EXPECT_EQ(S.active()->Owner, 101u);
  EXPECT_EQ(S.active()->Kind, Scheduler::CallbackKind::WorkItem);
  EXPECT_EQ(S.dispatchCount(), 3u);
  for (uint64_t Owner : {101, 102, 103})
    EXPECT_TRUE(S.hasOutstanding(Owner));
  fails(S.reserveDMAListControl(callback(4)), "pending callback limit");
  fails(S.finishInlineDMAListControl(First), "identity mismatch");
  fails(S.finish(Parent), "inline DMA");
  fails(S.suspend(Parent), "inline DMA");
  fails(S.next(false), "unfinished");
  fails(S.advanceTo100ns(1), "runnable");
  success(S.finishInlineDMAListControl(Second));
  EXPECT_FALSE(S.hasOutstanding(103));
  EXPECT_TRUE(S.hasOutstanding(102));
  success(S.finishInlineDMAListControl(First));
  EXPECT_FALSE(S.hasOutstanding(102));
  EXPECT_TRUE(S.hasOutstanding(101));
  EXPECT_EQ(S.dispatchCount(), 3u);
  success(S.finish(Parent));
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverSchedulerDMA, ForegroundInlineExecutionCannotResumeAnotherParent) {
  Scheduler S;
  const auto Other = take(S.enqueueWorkItem(callback(1, 101)));
  ASSERT_TRUE(take(S.next(false)));
  success(S.suspend(Other));
  const auto Inline = take(S.reserveDMAListControl(callback(2, 102)));
  success(S.beginInlineDMAListControl(Inline));
  EXPECT_FALSE(S.active());
  EXPECT_TRUE(S.hasOutstanding(102));
  fails(S.resume(Other), "unfinished");
  fails(S.next(true), "unfinished");
  fails(S.advanceTo100ns(1), "runnable");
  fails(S.finishInlineDMAListControl(Other), "identity mismatch");
  EXPECT_EQ(S.now100ns(), 0u);
  success(S.finishInlineDMAListControl(Inline));
  EXPECT_FALSE(S.hasOutstanding(102));
  success(S.resume(Other));
  success(S.finish(Other));
  EXPECT_EQ(S.dispatchCount(), 2u);
}

TEST(DriverSchedulerDMA, DispatchLimitFailureKeepsReservationAndParent) {
  Scheduler::Limits Limits;
  Limits.MaxDispatches = 1;
  Scheduler S(Limits);
  const auto Parent = take(S.enqueueWorkItem(callback(1)));
  const auto Child = take(S.reserveDMAListControl(callback(2, 102)));
  ASSERT_TRUE(take(S.next(false)));
  fails(S.canBeginInlineDMAListControl(Child), "dispatch limit");
  fails(S.beginInlineDMAListControl(Child), "dispatch limit");
  ASSERT_TRUE(S.active());
  EXPECT_EQ(S.active()->ID, Parent);
  EXPECT_TRUE(S.hasOutstanding(102));
  EXPECT_EQ(S.dispatchCount(), 1u);
  success(S.readyDMAListControls({Child}));
  success(S.finish(Parent));
  fails(S.next(false), "dispatch limit");
  EXPECT_TRUE(S.hasQueuedDMAListControl());
  EXPECT_EQ(S.queuedCallbackCount(), 1u);
  EXPECT_FALSE(S.active());
  EXPECT_EQ(take(S.reserveDMAListControl(callback(3))), Child + 1);
}

TEST(DriverSchedulerDMA, InlineAndReadyShareOneDispatchBudget) {
  Scheduler::Limits Limits;
  Limits.MaxDispatches = 2;
  Scheduler S(Limits);
  const auto Inline = take(S.reserveDMAListControl(callback(1)));
  const auto Ready = take(S.reserveDMAListControl(callback(2)));
  success(S.beginInlineDMAListControl(Inline));
  success(S.finishInlineDMAListControl(Inline));
  fails(S.finishInlineDMAListControl(Inline), "identity mismatch");
  success(S.readyDMAListControls({Ready}));
  ASSERT_TRUE(take(S.next(false)));
  success(S.suspend(Ready));
  success(S.resume(Ready));
  success(S.finish(Ready));
  EXPECT_EQ(S.dispatchCount(), 2u);
  const auto Last = take(S.reserveDMAListControl(callback(3)));
  fails(S.beginInlineDMAListControl(Last), "dispatch limit");
  EXPECT_TRUE(S.hasOutstanding(100));
  EXPECT_FALSE(S.hasQueuedDMAListControl());
}

TEST(DriverSchedulerDMA, SuspendedDeliveryKeepsKindArgumentsAndCapacity) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 1;
  Scheduler S(Limits);
  const auto ID = take(S.reserveDMAListControl(callback(1)));
  success(S.readyDMAListControls({ID}));
  ASSERT_TRUE(take(S.next(false)));
  success(S.suspend(ID));
  ASSERT_NE(S.suspended(ID), nullptr);
  EXPECT_EQ(S.suspended(ID)->Kind, Scheduler::CallbackKind::DMAListControl);
  EXPECT_EQ(S.suspended(ID)->IRQL, 2);
  EXPECT_EQ(S.suspended(ID)->Arguments, callback(1).Arguments);
  fails(S.enqueueWorkItem(callback(2)), "pending callback limit");
  success(S.resume(ID));
  success(S.finish(ID));
  EXPECT_EQ(take(S.reserveDMAListControl(callback(2))), ID + 1);
}

TEST(DriverSchedulerDMA, ReservationsParticipateInAtomicTimerBoundaryCapacity) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 2;
  Scheduler S(Limits);
  const auto Held = take(S.reserveDMAListControl(callback(1)));
  take(S.setTimer(20, 100, -10, 0, dpc(30)));
  fails(S.canAdvanceTo100ns(10, 1), "pending callback limit");
  EXPECT_EQ(S.now100ns(), 0u);
  EXPECT_EQ(S.timerExpirationCount(), 0u);
  EXPECT_FALSE(take(S.timerSignaled(20)));
  EXPECT_FALSE(S.hasQueuedDPC());
  success(S.canAdvanceTo100ns(10));
  success(S.advanceTo100ns(10));
  // Both slots are occupied, yet resource availability can still promote DMA.
  success(S.readyDMAListControls({Held}));
  EXPECT_EQ(S.queuedCallbackCount(), 2u);
  const auto TimerDPC = take(S.next(false));
  ASSERT_TRUE(TimerDPC);
  EXPECT_EQ(TimerDPC->Kind, Scheduler::CallbackKind::DPC);
  success(S.finish(TimerDPC->ID));
  const auto DMA = take(S.next(false));
  ASSERT_TRUE(DMA);
  EXPECT_EQ(DMA->ID, Held);
  success(S.finish(DMA->ID));
}

TEST(DriverSchedulerDMA, InvalidReservationConsumesNeitherCapacityNorID) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 1;
  Scheduler S(Limits);
  for (unsigned Field = 0; Field < 4; ++Field) {
    auto Invalid = callback(1);
    if (Field == 0)
      Invalid.Object = 0;
    else if (Field == 1)
      Invalid.Owner = 0;
    else if (Field == 2)
      Invalid.Thread = 0;
    else
      Invalid.PC = 0;
    fails(S.canReserveDMAListControl(Invalid), "nonzero");
    fails(S.reserveDMAListControl(Invalid), "nonzero");
  }
  auto TooMany = callback(1);
  TooMany.Arguments.resize(scheduler::MaxArguments + 1);
  fails(S.reserveDMAListControl(TooMany), "argument count");
  EXPECT_FALSE(S.hasPending());
  success(S.canReserveDMAListControl(callback(1)));
  success(S.canReserveDMAListControl(callback(1)));
  EXPECT_FALSE(S.hasOutstanding(100));
  EXPECT_EQ(take(S.reserveDMAListControl(callback(1))), 1u);
  fails(S.canReserveDMAListControl(callback(2)), "pending callback limit");
}

TEST(DriverSchedulerDMA, InvalidInitialTimeRejectsAllAdmissionPreflights) {
  Scheduler::Limits Limits;
  Limits.InitialTime100ns = 2;
  Limits.MaxTime100ns = 1;
  Scheduler S(Limits);
  fails(S.canReserveDMAListControl(callback(1)), "initial time");
  fails(S.reserveDMAListControl(callback(1)), "initial time");
  fails(S.canReadyDMAListControls({}), "initial time");
  fails(S.readyDMAListControls({}), "initial time");
  fails(S.canBeginInlineDMAListControl(1), "initial time");
  fails(S.beginInlineDMAListControl(1), "initial time");
  EXPECT_FALSE(S.hasPending());
  EXPECT_EQ(S.dispatchCount(), 0u);
}

TEST(DriverSchedulerDMA, InlineBudgetCanBeCheckedBeforeAnyReservation) {
  Scheduler::Limits Limits;
  Limits.MaxDispatches = 1;
  Scheduler S(Limits);
  success(S.canDispatchInlineDMAListControl());
  EXPECT_FALSE(S.hasPending());
  EXPECT_EQ(S.dispatchCount(), 0u);
  const auto ID = take(S.reserveDMAListControl(callback(1)));
  EXPECT_EQ(ID, 1u);
  success(S.beginInlineDMAListControl(ID));
  success(S.finishInlineDMAListControl(ID));
  fails(S.canDispatchInlineDMAListControl(), "dispatch limit");
  EXPECT_FALSE(S.hasPending());
  EXPECT_EQ(S.dispatchCount(), 1u);
  EXPECT_EQ(take(S.reserveDMAListControl(callback(2))), 2u);
}

TEST(DriverSchedulerDMA, InlineBudgetPreflightRejectsInvalidInitialTime) {
  Scheduler::Limits Limits;
  Limits.MaxTime100ns = 0;
  Limits.InitialTime100ns = 1;
  Scheduler S(Limits);
  fails(S.canDispatchInlineDMAListControl(), "initial time");
  EXPECT_FALSE(S.hasPending());
  EXPECT_EQ(S.dispatchCount(), 0u);
  EXPECT_EQ(S.now100ns(), 1u);
}

TEST(DriverSchedulerDMA, InlineReturnPreflightPreservesNestedOwners) {
  Scheduler S;
  const auto Parent = take(S.reserveDMAListControl(callback(1, 101)));
  const auto Child = take(S.reserveDMAListControl(callback(2, 102)));
  success(S.beginInlineDMAListControl(Parent));
  success(S.beginInlineDMAListControl(Child));
  fails(S.canFinishInlineDMAListControl(Parent), "identity mismatch");
  success(S.canFinishInlineDMAListControl(Child));
  success(S.canFinishInlineDMAListControl(Child));
  EXPECT_TRUE(S.hasOutstanding(101));
  EXPECT_TRUE(S.hasOutstanding(102));
  EXPECT_EQ(S.dispatchCount(), 2u);
  success(S.finishInlineDMAListControl(Child));
  success(S.canFinishInlineDMAListControl(Parent));
  EXPECT_TRUE(S.hasOutstanding(101));
  success(S.finishInlineDMAListControl(Parent));
  fails(S.canFinishInlineDMAListControl(Parent), "identity mismatch");
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverSchedulerDMA, OtherKindsCannotConsumeDMAIdentityOrCapacity) {
  Scheduler S;
  using Kind = Scheduler::CallbackKind;
  for (Kind Other :
       {Kind::WorkItem, Kind::DPC, Kind::FrameworkCancel, Kind::WDMCompletion,
        Kind::Interrupt, static_cast<Kind>(255)}) {
    EXPECT_FALSE(Scheduler::isDMACallbackKind(Other));
    fails(S.canReserveDMACallback(callback(1), Other), "not a DMA callback");
    fails(S.reserveDMACallback(callback(1), Other), "not a DMA callback");
    fails(S.canBeginInlineDMACallback(1, Other), "not a DMA callback");
    fails(S.beginInlineDMACallback(1, Other), "not a DMA callback");
    fails(S.canFinishInlineDMACallback(1, Other), "not a DMA callback");
    fails(S.finishInlineDMACallback(1, Other), "not a DMA callback");
  }
  EXPECT_FALSE(S.hasPending());
  EXPECT_EQ(S.dispatchCount(), 0u);
  EXPECT_EQ(S.now100ns(), 0u);
  EXPECT_TRUE(Scheduler::isDMACallbackKind(Kind::DMAAdapterControl));
  EXPECT_TRUE(Scheduler::isDMACallbackKind(Kind::DMAListControl));
  EXPECT_EQ(take(S.reserveDMACallback(callback(1), Kind::DMAAdapterControl)),
            1u);
}

TEST(DriverSchedulerDMA, MixedFIFOHasOnePriorityTierWithoutObjectCoalescing) {
  Scheduler S;
  using Kind = Scheduler::CallbackKind;
  const auto Channel =
      take(S.reserveDMACallback(callback(7), Kind::DMAAdapterControl));
  const auto List = take(S.reserveDMAListControl(callback(7)));
  const auto OtherChannel =
      take(S.reserveDMACallback(callback(7), Kind::DMAAdapterControl));
  // Admission order, rather than reservation age or callback kind, owns FIFO.
  success(S.readyDMACallbacks({List, Channel}));
  success(S.readyDMACallbacks({OtherChannel}));
  const auto Worker = take(S.enqueueWorkItem(callback(7)));
  const auto Completion = take(S.enqueueWDMCompletion(callback(7)));
  const auto Cancel = take(S.enqueueFrameworkCancel(callback(7)));
  EXPECT_TRUE(take(S.queueDPC(dpc(7))));
  Scheduler::InterruptCallback IRQ;
  static_cast<Scheduler::Callback &>(IRQ) = callback(7);
  IRQ.IRQL = IRQ.Priority = 5;
  const auto Interrupt = take(S.enqueueInterrupt(IRQ));
  EXPECT_EQ(S.queuedCallbackCount(), 8u);
  auto Call = take(S.next(false));
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->ID, Interrupt);
  success(S.finish(Call->ID));
  Call = take(S.next(false));
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->Kind, Kind::DPC);
  success(S.finish(Call->ID));
  for (uint64_t ID : {List, Channel, OtherChannel}) {
    EXPECT_TRUE(S.hasQueuedDMACallback());
    EXPECT_EQ(S.hasQueuedDMAListControl(), ID == List);
    Call = take(S.next(false));
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->ID, ID);
    EXPECT_EQ(Call->Kind,
              ID == List ? Kind::DMAListControl : Kind::DMAAdapterControl);
    EXPECT_EQ(Call->IRQL, 2);
    EXPECT_EQ(Call->Arguments, callback(7).Arguments);
    success(S.finish(ID));
  }
  EXPECT_FALSE(S.hasQueuedDMACallback());
  for (uint64_t ID : {Cancel, Completion, Worker}) {
    Call = take(S.next(false));
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->ID, ID);
    EXPECT_EQ(Call->IRQL, 0);
    success(S.finish(ID));
  }
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverSchedulerDMA, MixedBatchRejectsWrongKindWithoutPartialPromotion) {
  Scheduler S;
  using Kind = Scheduler::CallbackKind;
  const auto List = take(S.reserveDMAListControl(callback(1, 101)));
  const auto Channel =
      take(S.reserveDMACallback(callback(2, 102), Kind::DMAAdapterControl));
  fails(S.canReadyDMAListControls({List, Channel}), "kind mismatch");
  fails(S.readyDMAListControls({List, Channel}), "kind mismatch");
  fails(S.canReadyDMACallbacks({List, Channel, Channel}), "duplicate");
  fails(S.readyDMACallbacks({List, Channel, UINT64_MAX}), "not waiting");
  EXPECT_FALSE(S.hasQueuedDMACallback());
  EXPECT_FALSE(take(S.next(true)));
  EXPECT_EQ(S.now100ns(), 0u);
  EXPECT_TRUE(S.hasOutstanding(101));
  EXPECT_TRUE(S.hasOutstanding(102));
  success(S.canReadyDMACallbacks({Channel, List}));
  EXPECT_EQ(S.queuedCallbackCount(), 0u);
  success(S.readyDMACallbacks({Channel, List}));
  for (uint64_t ID : {Channel, List}) {
    const auto Call = take(S.next(false));
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->ID, ID);
    success(S.finish(ID));
  }
  EXPECT_EQ(take(S.reserveDMAListControl(callback(3))), 3u);
}

TEST(DriverSchedulerDMA, MixedPromotionUsesCapacityReservedBeforeRelease) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 3;
  Scheduler S(Limits);
  using Kind = Scheduler::CallbackKind;
  const auto Parent = take(S.enqueueWorkItem(callback(1, 101)));
  const auto Channel =
      take(S.reserveDMACallback(callback(2, 102), Kind::DMAAdapterControl));
  const auto List = take(S.reserveDMAListControl(callback(3, 103)));
  ASSERT_TRUE(take(S.next(false)));
  fails(S.reserveDMACallback(callback(4), Kind::DMAAdapterControl),
        "pending callback limit");
  success(S.readyDMACallbacks({Channel, List}));
  // Promotion during a release only queues; the scheduled releaser survives.
  ASSERT_TRUE(S.active());
  EXPECT_EQ(S.active()->ID, Parent);
  EXPECT_EQ(S.dispatchCount(), 1u);
  fails(S.next(false), "unfinished callback");
  success(S.finish(Parent));
  for (uint64_t ID : {Channel, List}) {
    const auto Call = take(S.next(false));
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->ID, ID);
    EXPECT_TRUE(S.hasOutstanding(Call->Owner));
    success(S.finish(ID));
    EXPECT_FALSE(S.hasOutstanding(Call->Owner));
  }
  EXPECT_EQ(take(S.reserveDMACallback(callback(4), Kind::DMAAdapterControl)),
            4u);
}

TEST(DriverSchedulerDMA, MixedInlineKindAndLifoFailuresPreserveParent) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 3;
  Scheduler S(Limits);
  using Kind = Scheduler::CallbackKind;
  EXPECT_TRUE(take(S.queueDPC(dpc(1))));
  const auto Parent = take(S.next(false));
  ASSERT_TRUE(Parent);
  const auto Channel =
      take(S.reserveDMACallback(callback(2, 102), Kind::DMAAdapterControl));
  const auto List = take(S.reserveDMAListControl(callback(3, 103)));
  fails(S.canBeginInlineDMAListControl(Channel), "kind mismatch");
  fails(S.beginInlineDMAListControl(Channel), "kind mismatch");
  fails(S.beginInlineDMACallback(List, Kind::DMAAdapterControl),
        "kind mismatch");
  EXPECT_EQ(S.dispatchCount(), 1u);
  success(S.canBeginInlineDMACallback(Channel, Kind::DMAAdapterControl));
  success(S.beginInlineDMACallback(Channel, Kind::DMAAdapterControl));
  success(S.beginInlineDMAListControl(List));
  EXPECT_EQ(S.dispatchCount(), 3u);
  fails(S.canFinishInlineDMACallback(Channel, Kind::DMAAdapterControl),
        "identity mismatch");
  fails(S.finishInlineDMACallback(Channel, Kind::DMAAdapterControl),
        "identity mismatch");
  fails(S.finishInlineDMACallback(List, Kind::DMAAdapterControl),
        "kind mismatch");
  fails(S.suspend(Parent->ID), "inline DMA");
  fails(S.canFinish(Parent->ID), "inline DMA");
  ASSERT_TRUE(S.active());
  EXPECT_EQ(S.active()->ID, Parent->ID);
  EXPECT_EQ(S.active()->Kind, Kind::DPC);
  EXPECT_TRUE(S.hasOutstanding(102));
  EXPECT_TRUE(S.hasOutstanding(103));
  success(S.finishInlineDMAListControl(List));
  fails(S.canFinishInlineDMAListControl(Channel), "kind mismatch");
  fails(S.finishInlineDMAListControl(Channel), "kind mismatch");
  EXPECT_TRUE(S.hasOutstanding(102));
  EXPECT_FALSE(S.hasOutstanding(103));
  success(S.canFinishInlineDMACallback(Channel, Kind::DMAAdapterControl));
  EXPECT_TRUE(S.hasOutstanding(102));
  success(S.finishInlineDMACallback(Channel, Kind::DMAAdapterControl));
  EXPECT_FALSE(S.hasOutstanding(102));
  success(S.canFinish(Parent->ID));
  success(S.canFinish(Parent->ID));
  ASSERT_TRUE(S.active());
  EXPECT_EQ(S.active()->ID, Parent->ID);
  success(S.finish(Parent->ID));
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverSchedulerDMA, ChannelWaitingAndSuspensionRetainOriginalArguments) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 1;
  Scheduler S(Limits);
  using Kind = Scheduler::CallbackKind;
  auto Metadata = callback(1, 101);
  Metadata.Arguments = {101, 0x70001000, 0x80002000, 0xDEADBEEF};
  const auto ID = take(S.reserveDMACallback(Metadata, Kind::DMAAdapterControl));
  Metadata.Arguments[1] = 0; // The registering caller can change its storage.
  EXPECT_FALSE(S.hasQueuedDMACallback());
  EXPECT_FALSE(S.nextEventTime100ns());
  EXPECT_FALSE(take(S.next(true)));
  EXPECT_EQ(S.now100ns(), 0u);
  EXPECT_TRUE(S.hasPending());
  EXPECT_TRUE(S.hasOutstanding(101));
  success(S.readyDMACallbacks({ID}));
  const auto Call = take(S.next(false));
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->Arguments[1], 0x70001000u);
  EXPECT_EQ(Call->IRQL, 2);
  // Suspension is a scheduler primitive; API-level waits still enforce IRQL.
  success(S.suspend(ID));
  ASSERT_NE(S.suspended(ID), nullptr);
  EXPECT_EQ(S.suspended(ID)->Kind, Kind::DMAAdapterControl);
  EXPECT_EQ(S.suspended(ID)->Arguments, Call->Arguments);
  fails(S.reserveDMAListControl(callback(2)), "pending callback limit");
  success(S.resume(ID));
  EXPECT_EQ(S.active()->Arguments, Call->Arguments);
  EXPECT_EQ(S.dispatchCount(), 1u);
  success(S.finish(ID));
  EXPECT_FALSE(S.hasOutstanding(101));
  EXPECT_EQ(take(S.reserveDMAListControl(callback(2))), 2u);
}

TEST(DriverSchedulerDMA, ChannelAndListShareInlineAndScheduledDispatchBudget) {
  Scheduler::Limits Limits;
  Limits.MaxDispatches = 1;
  Scheduler S(Limits);
  using Kind = Scheduler::CallbackKind;
  success(S.canDispatchInlineDMACallback());
  const auto Channel =
      take(S.reserveDMACallback(callback(1, 101), Kind::DMAAdapterControl));
  const auto List = take(S.reserveDMAListControl(callback(2, 102)));
  success(S.beginInlineDMACallback(Channel, Kind::DMAAdapterControl));
  success(S.finishInlineDMACallback(Channel, Kind::DMAAdapterControl));
  fails(S.canDispatchInlineDMACallback(), "dispatch limit");
  fails(S.beginInlineDMAListControl(List), "dispatch limit");
  success(S.canReadyDMACallbacks({List}));
  success(S.readyDMACallbacks({List}));
  fails(S.next(false), "dispatch limit");
  EXPECT_FALSE(S.active());
  EXPECT_EQ(S.queuedCallbackCount(), 1u);
  EXPECT_TRUE(S.hasOutstanding(102));
  EXPECT_FALSE(S.hasOutstanding(101));
  EXPECT_EQ(S.dispatchCount(), 1u);
  EXPECT_EQ(take(S.reserveDMACallback(callback(3), Kind::DMAAdapterControl)),
            3u);
}

TEST(DriverSchedulerDMA, MixedReservationsPreflightWholeTimerBoundary) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 3;
  Scheduler S(Limits);
  using Kind = Scheduler::CallbackKind;
  const auto Channel =
      take(S.reserveDMACallback(callback(1), Kind::DMAAdapterControl));
  const auto List = take(S.reserveDMAListControl(callback(2)));
  EXPECT_FALSE(take(S.setTimer(20, 100, -10, 0, dpc(30))));
  fails(S.canAdvanceTo100ns(10, 1), "pending callback limit");
  EXPECT_EQ(S.now100ns(), 0u);
  EXPECT_FALSE(take(S.timerSignaled(20)));
  EXPECT_EQ(S.timerExpirationCount(), 0u);
  success(S.canAdvanceTo100ns(10));
  success(S.advanceTo100ns(10));
  success(S.readyDMACallbacks({Channel, List}));
  EXPECT_EQ(S.queuedCallbackCount(), 3u);
  const auto First = take(S.next(false));
  ASSERT_TRUE(First);
  EXPECT_EQ(First->Kind, Kind::DPC);
  success(S.finish(First->ID));
  for (uint64_t ID : {Channel, List}) {
    const auto Call = take(S.next(false));
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->ID, ID);
    EXPECT_EQ(Call->DueTime100ns, 10u);
    success(S.finish(ID));
  }
  EXPECT_EQ(S.dispatchCount(), 3u);
}

} // namespace
} // namespace neverd::emulation
