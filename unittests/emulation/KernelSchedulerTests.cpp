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
#include "windows/KernelScheduler.h"

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

} // namespace
} // namespace neverd::emulation
