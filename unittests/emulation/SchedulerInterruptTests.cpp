//===- SchedulerInterruptTests.cpp - Interrupt scheduling boundaries -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise real-priority metadata and atomic shared deadline admission.
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
  return {Object, Owner, 200, 0x180001000, {0x5000, 0x6000}};
}

Scheduler::InterruptCallback interrupt(uint64_t Event, uint8_t Priority = 5,
                                       uint8_t IRQL = 5) {
  Scheduler::InterruptCallback Result;
  static_cast<Scheduler::Callback &>(Result) = callback(Event);
  Result.Priority = Priority;
  Result.IRQL = IRQL;
  return Result;
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

TEST(DriverKernelScheduler, InterruptPriorityIsDistinctFromExecutionIRQL) {
  Scheduler S;
  take(S.enqueueWorkItem(callback(1)));
  take(S.enqueueWDMCompletion(callback(2)));
  take(S.enqueueFrameworkCancel(callback(3)));
  EXPECT_TRUE(take(S.queueDPC(dpc(4))));
  take(S.enqueueInterrupt(interrupt(5, 4, 12)));
  take(S.enqueueInterrupt(interrupt(6, 9, 9)));
  take(S.enqueueInterrupt(interrupt(7, 9, 10)));
  for (uint64_t Object : {6, 7, 5, 4, 3, 2, 1}) {
    auto Call = take(S.next(false));
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->Object, Object);
    if (Object >= 5) {
      EXPECT_EQ(Call->Kind, Scheduler::CallbackKind::Interrupt);
      EXPECT_EQ(Call->IRQL, Object == 5 ? 12 : Object == 6 ? 9 : 10);
      EXPECT_EQ(Call->InterruptPriority, Object == 5 ? 4 : 9);
    }
    success(S.finish(Call->ID));
  }
  EXPECT_EQ(S.now100ns(), 0u);
}

TEST(DriverKernelScheduler, DistinctPulsesForOneConnectionAreNotCoalesced) {
  Scheduler S;
  const auto First = take(S.enqueueInterrupt(interrupt(1)));
  const auto Second = take(S.enqueueInterrupt(interrupt(2)));
  EXPECT_NE(First, Second);
  EXPECT_EQ(S.queuedCallbackCount(), 2u);
  for (uint64_t ID : {First, Second}) {
    auto Call = take(S.next(false));
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->ID, ID);
    EXPECT_EQ(Call->Arguments, (std::vector<uint64_t>{0x5000, 0x6000}));
    success(S.finish(ID));
  }
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverKernelScheduler, InterruptIdentityDoesNotAliasOtherCallbackKinds) {
  Scheduler S;
  take(S.enqueueWorkItem(callback(1)));
  EXPECT_TRUE(take(S.queueDPC(dpc(1))));
  take(S.enqueueFrameworkCancel(callback(1)));
  take(S.enqueueWDMCompletion(callback(1)));
  take(S.enqueueInterrupt(interrupt(1)));
  EXPECT_TRUE(S.cancelWorkItem(1));
  EXPECT_TRUE(S.removeDPC(1));
  EXPECT_FALSE(S.isWorkItemQueued(1));
  EXPECT_FALSE(S.isDPCQueued(1));
  EXPECT_TRUE(S.hasQueuedInterrupt());
  EXPECT_EQ(S.queuedCallbackCount(), 3u);
  auto Call = take(S.next(false));
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->Kind, Scheduler::CallbackKind::Interrupt);
  success(S.finish(Call->ID));
  EXPECT_TRUE(S.hasQueuedFrameworkCancel());
  EXPECT_TRUE(S.hasQueuedWDMCompletion());
}

TEST(DriverKernelScheduler, InterruptBatchFailureDoesNotConsumeIdentities) {
  Scheduler S;
  const auto Original = take(S.enqueueInterrupt(interrupt(1)));
  fails(S.canEnqueueInterrupts({interrupt(2), interrupt(2)}),
        "already queued");
  fails(S.canEnqueueInterrupts({interrupt(2), interrupt(1)}),
        "already queued");
  EXPECT_EQ(S.queuedCallbackCount(), 1u);
  EXPECT_EQ(take(S.enqueueInterrupt(interrupt(2))), Original + 1);
  EXPECT_EQ(S.dispatchCount(), 0u);
}

TEST(DriverKernelScheduler,
     SuspendedInterruptPreservesExactContextAndCapacity) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 1;
  Scheduler S(Limits);
  auto Event = interrupt(8, 7, 11);
  Event.Owner = 900;
  const auto ID = take(S.enqueueInterrupt(Event));
  auto Call = take(S.next(false));
  ASSERT_TRUE(Call);
  success(S.suspend(ID));
  ASSERT_NE(S.suspended(ID), nullptr);
  EXPECT_EQ(S.suspended(ID)->Kind, Scheduler::CallbackKind::Interrupt);
  EXPECT_EQ(S.suspended(ID)->IRQL, 11);
  EXPECT_EQ(S.suspended(ID)->InterruptPriority, 7);
  EXPECT_TRUE(S.hasOutstanding(900));
  fails(S.enqueueInterrupt(interrupt(9)), "limit");
  success(S.resume(ID));
  ASSERT_TRUE(S.active());
  EXPECT_EQ(S.active()->Arguments, Event.Arguments);
  EXPECT_EQ(S.active()->Owner, 900u);
  success(S.finish(ID));
  EXPECT_FALSE(S.hasOutstanding(900));
  EXPECT_FALSE(S.hasPending());
}

TEST(DriverKernelScheduler, SharedBoundaryAdmitsISRBeforeSelectingTimerDPC) {
  Scheduler S;
  take(S.setTimer(10, 100, -50, 0, dpc(20)));
  success(S.canAdvanceTo100ns(50, 1));
  success(S.advanceTo100ns(50));
  EXPECT_FALSE(S.active());
  EXPECT_EQ(S.dispatchCount(), 0u);
  EXPECT_TRUE(take(S.timerSignaled(10)));
  take(S.enqueueInterrupt(interrupt(30)));
  auto ISR = take(S.next(false));
  ASSERT_TRUE(ISR);
  EXPECT_EQ(ISR->Object, 30u);
  EXPECT_EQ(ISR->DueTime100ns, 50u);
  // The ISR may queue post-processing, but dispatch cannot nest through next.
  EXPECT_TRUE(take(S.queueDPC(dpc(40))));
  fails(S.next(false), "unfinished");
  success(S.finish(ISR->ID));
  for (uint64_t Object : {20, 40}) {
    auto DPC = take(S.next(false));
    ASSERT_TRUE(DPC);
    EXPECT_EQ(DPC->Object, Object);
    EXPECT_EQ(DPC->IRQL, 2);
    success(S.finish(DPC->ID));
  }
}

TEST(DriverKernelScheduler,
     WholeBoundaryCapacityFailurePreservesTimerAndTime) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 2;
  Scheduler S(Limits);
  const auto Worker = take(S.enqueueWorkItem(callback(1)));
  ASSERT_TRUE(take(S.next(false)));
  success(S.suspend(Worker));
  take(S.setTimer(10, 100, -20, 0, dpc(20)));
  fails(S.canAdvanceTo100ns(20, 1), "limit");
  EXPECT_EQ(S.now100ns(), 0u);
  EXPECT_EQ(S.timerExpirationCount(), 0u);
  EXPECT_EQ(S.queuedCallbackCount(), 0u);
  EXPECT_TRUE(S.isTimerArmed(10));
  EXPECT_FALSE(take(S.timerSignaled(10)));
  success(S.resume(Worker));
  success(S.finish(Worker));
  success(S.canAdvanceTo100ns(20, 1));
  success(S.advanceTo100ns(20));
  const auto IRQ = take(S.enqueueInterrupt(interrupt(30)));
  EXPECT_EQ(IRQ, Worker + 2); // Only the committed timer consumed an ID.
  EXPECT_EQ(S.queuedCallbackCount(), 2u);
}

TEST(DriverKernelScheduler, BoundaryCountsCoalescedTimerDPCOnce) {
  Scheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 2;
  Scheduler S(Limits);
  take(S.setTimer(10, 100, -20, 0, dpc(30)));
  take(S.setTimer(11, 100, -20, 0, dpc(30)));
  success(S.canAdvanceTo100ns(20, 1));
  success(S.advanceTo100ns(20));
  take(S.enqueueInterrupt(interrupt(40)));
  EXPECT_EQ(S.timerExpirationCount(), 2u);
  EXPECT_TRUE(take(S.timerSignaled(10)));
  EXPECT_TRUE(take(S.timerSignaled(11)));
  EXPECT_EQ(S.queuedCallbackCount(), 2u);
}

TEST(DriverKernelScheduler, AdvanceCannotSkipReadyWorkOrEarlierTimer) {
  Scheduler S;
  take(S.setTimer(10, 100, -20, 0));
  fails(S.advanceTo100ns(21), "earlier timer");
  const auto Work = take(S.enqueueWorkItem(callback(1)));
  fails(S.advanceTo100ns(20), "runnable");
  ASSERT_TRUE(take(S.next(false)));
  fails(S.advanceTo100ns(20), "runnable");
  EXPECT_EQ(S.now100ns(), 0u);
  success(S.finish(Work));
  success(S.advanceTo100ns(20));
  EXPECT_TRUE(take(S.timerSignaled(10)));
  EXPECT_FALSE(S.active());
  fails(S.advanceTo100ns(19), "time range");
  EXPECT_EQ(S.now100ns(), 20u);
}

TEST(DriverKernelScheduler, BoundaryFailuresPreservePeriodicExpiry) {
  Scheduler::Limits Limits;
  Limits.MaxTime100ns = 20000;
  Limits.MaxTimerExpirations = 0;
  Scheduler S(Limits);
  take(S.setTimer(10, 100, -10000, 1, dpc(20)));
  fails(S.canAdvanceTo100ns(10000, 1), "expiration limit");
  EXPECT_EQ(S.now100ns(), 0u);
  EXPECT_TRUE(S.isTimerArmed(10));
  EXPECT_FALSE(take(S.timerSignaled(10)));
  EXPECT_EQ(S.nextEventTime100ns(), 10000u);
  fails(S.canAdvanceTo100ns(30000), "earlier timer");
}

TEST(DriverKernelScheduler, InvalidInterruptMetadataDoesNotEnterQueues) {
  Scheduler S;
  for (const auto &Event : {interrupt(1, 2, 5), interrupt(1, 13, 13),
                            interrupt(1, 8, 7), interrupt(1, 5, 15)})
    fails(S.enqueueInterrupt(Event), "priority");
  auto NullPC = interrupt(1);
  NullPC.PC = 0;
  fails(S.enqueueInterrupt(NullPC), "guest PC");
  EXPECT_FALSE(S.hasPending());
  EXPECT_EQ(take(S.enqueueInterrupt(interrupt(1))), 1u);
}

TEST(DriverKernelScheduler, DispatchLimitDoesNotDiscardQueuedInterrupt) {
  Scheduler::Limits Limits;
  Limits.MaxDispatches = 0;
  Scheduler S(Limits);
  take(S.enqueueInterrupt(interrupt(1)));
  fails(S.next(false), "dispatch limit");
  EXPECT_FALSE(S.active());
  EXPECT_TRUE(S.hasQueuedInterrupt());
  EXPECT_TRUE(S.hasOutstanding(100));
  EXPECT_EQ(S.queuedCallbackCount(), 1u);
}

TEST(DriverKernelScheduler, ExternalCallbackOverflowLeavesClockUnchanged) {
  Scheduler S;
  take(S.setTimer(10, 100, -20, 0, dpc(20)));
  fails(S.canAdvanceTo100ns(20, UINT64_MAX), "count overflow");
  EXPECT_EQ(S.now100ns(), 0u);
  EXPECT_EQ(S.timerExpirationCount(), 0u);
  EXPECT_FALSE(take(S.timerSignaled(10)));
  EXPECT_FALSE(S.hasQueuedDPC());
}

} // namespace
} // namespace neverd::emulation
