//===- DriverDispatcherTests.cpp - Compiled dispatcher contracts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify callback ABI, virtual timer behavior and preserved waiting stacks.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
struct DispatcherCase {
  uint32_t Code;
  bool Pending;
  uint8_t Dpcs, Workers, Waits, Proof;
};
struct DispatcherFailure {
  uint32_t Code;
  const char *Diagnostic;
};
#define NEVERD_DISPATCHER_CASE(Name, Code, Pending, Dpcs, Workers, Waits,      \
                               Proof)                                          \
  constexpr DispatcherCase Name{Code, Pending, Dpcs, Workers, Waits, Proof};
#define NEVERD_DISPATCHER_FAILURE(Name, Code, Diagnostic)                      \
  constexpr DispatcherFailure Name{Code, Diagnostic};
#include "fixtures/DriverDispatcherCases.def"
#undef NEVERD_DISPATCHER_FAILURE
#undef NEVERD_DISPATCHER_CASE

DriverOptions dispatcherScenario(uint32_t Code) {
  DriverOptions Options;
  DriverRequest Create;
  Create.Kind = DriverRequestKind::Create;
  Options.Requests.push_back(Create);
  DriverRequest IO;
  IO.Kind = DriverRequestKind::DeviceControl;
  IO.ControlCode = Code;
  IO.Input = {0, 1, 0x5a, 0xff};
  IO.OutputSize = Code == CompleteIrpWithQueuedDpcStorage.Code ? 64 : 8;
  Options.Requests.push_back(IO);
  DriverRequest Cleanup;
  Cleanup.Kind = DriverRequestKind::Cleanup;
  Options.Requests.push_back(Cleanup);
  DriverRequest Close;
  Close.Kind = DriverRequestKind::Close;
  Options.Requests.push_back(Close);
  Options.Unload = true;
  return Options;
}

void checkDispatcherSuccess(uint32_t Code, bool Pending, uint8_t Dpcs,
                            uint8_t Workers, uint8_t Waits, uint8_t Proof) {
  auto Result = emulateDriver(NEVERD_DRIVER_FIXTURES "/driver_dispatcher.sys",
                              dispatcherScenario(Code));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_EQ(Result->Requests.size(), 4u);
  EXPECT_EQ(Result->Requests[1].DispatchStatus, Pending ? 0x103u : 0u);
  EXPECT_EQ(Result->Requests[1].IOStatus, 0u);
  EXPECT_TRUE(Result->Requests[1].Completed);
  EXPECT_EQ(
      Result->Requests[1].Output,
      (std::vector<uint8_t>{Dpcs, Workers, Waits, Proof, 'W', 'D', 'M', '!'}));
  EXPECT_TRUE(Result->UnloadCompleted);
  EXPECT_TRUE(Result->Devices.empty());
  if (Dpcs || Workers)
    EXPECT_TRUE(std::any_of(Result->Calls.begin(), Result->Calls.end(),
                            [](const auto &Call) {
                              return Call.Name == "KeGetCurrentIrql" &&
                                     Call.Phase.starts_with("callback:");
                            }));
}

void checkDispatcherFailure(uint32_t Code, const char *Diagnostic,
                            const char *FailedAPI = nullptr) {
  auto Result = emulateDriver(NEVERD_DRIVER_FIXTURES "/driver_dispatcher.sys",
                              dispatcherScenario(Code));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
  EXPECT_NE(Result->Diagnostic.find(Diagnostic), std::string::npos)
      << Result->Diagnostic;
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_FALSE(Result->Requests[1].Completed);
  EXPECT_FALSE(Result->UnloadCompleted);
  if (FailedAPI) {
    ASSERT_FALSE(Result->Calls.empty());
    EXPECT_EQ(Result->Calls.back().Name, FailedAPI);
    EXPECT_FALSE(Result->Calls.back().Result);
  }
}

TEST(DriverDispatcher, TimerRelative) {
  checkDispatcherSuccess(TimerRelative.Code, TimerRelative.Pending,
                         TimerRelative.Dpcs, TimerRelative.Workers,
                         TimerRelative.Waits, TimerRelative.Proof);
}

TEST(DriverDispatcher, TimerAbsolute) {
  checkDispatcherSuccess(TimerAbsolute.Code, TimerAbsolute.Pending,
                         TimerAbsolute.Dpcs, TimerAbsolute.Workers,
                         TimerAbsolute.Waits, TimerAbsolute.Proof);
}

TEST(DriverDispatcher, PeriodicCancel) {
  checkDispatcherSuccess(PeriodicCancel.Code, PeriodicCancel.Pending,
                         PeriodicCancel.Dpcs, PeriodicCancel.Workers,
                         PeriodicCancel.Waits, PeriodicCancel.Proof);
}

TEST(DriverDispatcher, TimerRearmCancel) {
  checkDispatcherSuccess(TimerRearmCancel.Code, TimerRearmCancel.Pending,
                         TimerRearmCancel.Dpcs, TimerRearmCancel.Workers,
                         TimerRearmCancel.Waits, TimerRearmCancel.Proof);
}

TEST(DriverDispatcher, DpcArguments) {
  checkDispatcherSuccess(DpcArguments.Code, DpcArguments.Pending,
                         DpcArguments.Dpcs, DpcArguments.Workers,
                         DpcArguments.Waits, DpcArguments.Proof);
}

TEST(DriverDispatcher, DpcRemoval) {
  checkDispatcherSuccess(DpcRemoval.Code, DpcRemoval.Pending, DpcRemoval.Dpcs,
                         DpcRemoval.Workers, DpcRemoval.Waits,
                         DpcRemoval.Proof);
}

TEST(DriverDispatcher, DpcToWorker) {
  checkDispatcherSuccess(DpcToWorker.Code, DpcToWorker.Pending,
                         DpcToWorker.Dpcs, DpcToWorker.Workers,
                         DpcToWorker.Waits, DpcToWorker.Proof);
}

TEST(DriverDispatcher, DispatchEventWait) {
  checkDispatcherSuccess(DispatchEventWait.Code, DispatchEventWait.Pending,
                         DispatchEventWait.Dpcs, DispatchEventWait.Workers,
                         DispatchEventWait.Waits, DispatchEventWait.Proof);
}

TEST(DriverDispatcher, WorkerEventWait) {
  checkDispatcherSuccess(WorkerEventWait.Code, WorkerEventWait.Pending,
                         WorkerEventWait.Dpcs, WorkerEventWait.Workers,
                         WorkerEventWait.Waits, WorkerEventWait.Proof);
}

TEST(DriverDispatcher, WorkerTimerWait) {
  checkDispatcherSuccess(WorkerTimerWait.Code, WorkerTimerWait.Pending,
                         WorkerTimerWait.Dpcs, WorkerTimerWait.Workers,
                         WorkerTimerWait.Waits, WorkerTimerWait.Proof);
}

TEST(DriverDispatcher, FiniteTimeoutAndDelay) {
  checkDispatcherSuccess(
      FiniteTimeoutAndDelay.Code, FiniteTimeoutAndDelay.Pending,
      FiniteTimeoutAndDelay.Dpcs, FiniteTimeoutAndDelay.Workers,
      FiniteTimeoutAndDelay.Waits, FiniteTimeoutAndDelay.Proof);
}

TEST(DriverDispatcher, NotificationEvent) {
  checkDispatcherSuccess(NotificationEvent.Code, NotificationEvent.Pending,
                         NotificationEvent.Dpcs, NotificationEvent.Workers,
                         NotificationEvent.Waits, NotificationEvent.Proof);
}

TEST(DriverDispatcher, SynchronizationEvent) {
  checkDispatcherSuccess(
      SynchronizationEvent.Code, SynchronizationEvent.Pending,
      SynchronizationEvent.Dpcs, SynchronizationEvent.Workers,
      SynchronizationEvent.Waits, SynchronizationEvent.Proof);
}

TEST(DriverDispatcher, NotificationTimer) {
  checkDispatcherSuccess(NotificationTimer.Code, NotificationTimer.Pending,
                         NotificationTimer.Dpcs, NotificationTimer.Workers,
                         NotificationTimer.Waits, NotificationTimer.Proof);
}

TEST(DriverDispatcher, SynchronizationTimer) {
  checkDispatcherSuccess(
      SynchronizationTimer.Code, SynchronizationTimer.Pending,
      SynchronizationTimer.Dpcs, SynchronizationTimer.Workers,
      SynchronizationTimer.Waits, SynchronizationTimer.Proof);
}

TEST(DriverDispatcher, InvalidDpcWait) {
  checkDispatcherFailure(InvalidDpcWait.Code, InvalidDpcWait.Diagnostic);
}

TEST(DriverDispatcher, OpaqueDpc) {
  checkDispatcherFailure(OpaqueDpc.Code, OpaqueDpc.Diagnostic);
}

TEST(DriverDispatcher, FreePendingTimer) {
  checkDispatcherFailure(FreePendingTimer.Code, FreePendingTimer.Diagnostic);
}

TEST(DriverDispatcher, TimerImmediateWait) {
  checkDispatcherSuccess(TimerImmediateWait.Code, TimerImmediateWait.Pending,
                         TimerImmediateWait.Dpcs, TimerImmediateWait.Workers,
                         TimerImmediateWait.Waits, TimerImmediateWait.Proof);
}

TEST(DriverDispatcher, WorkerEventSetReset) {
  checkDispatcherSuccess(WorkerEventSetReset.Code, WorkerEventSetReset.Pending,
                         WorkerEventSetReset.Dpcs, WorkerEventSetReset.Workers,
                         WorkerEventSetReset.Waits, WorkerEventSetReset.Proof);
}

TEST(DriverDispatcher, TimerExpiryLatchesBeforeDpcRearm) {
  const auto &Case = TimerExpiryLatchesBeforeDpcRearm;
  checkDispatcherSuccess(Case.Code, Case.Pending, Case.Dpcs, Case.Workers,
                         Case.Waits, Case.Proof);
}

TEST(DriverDispatcher, CompleteIrpWithQueuedDpcStorage) {
  const auto &Case = CompleteIrpWithQueuedDpcStorage;
  checkDispatcherFailure(Case.Code, Case.Diagnostic, "IofCompleteRequest");
}

TEST(DriverDispatcher, DpcBeforeResumedWorker) {
  const auto &Case = DpcBeforeResumedWorker;
  checkDispatcherSuccess(Case.Code, Case.Pending, Case.Dpcs, Case.Workers,
                         Case.Waits, Case.Proof);
}

} // namespace
} // namespace neverd::emulation
