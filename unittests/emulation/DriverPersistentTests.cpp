//===- DriverPersistentTests.cpp - Retained driver orchestration ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Retained driver orchestration.
///

#include "gtest/gtest.h"
#include "os/windows/driver/DriverExecutionRun.h"
#include "os/windows/driver/DriverTask.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace neverd::emulation;
namespace {
struct Lifetime {
  unsigned &Destroyed;
  ~Lifetime() { ++Destroyed; }
};
DriverTask childTask(unsigned &Destroyed, bool Throws) {
  Lifetime Child{Destroyed};
  co_yield EnvironmentAction{
      EnvironmentActionKind::StageBoundary, 0, "test", {}};
  if (Throws)
    throw std::runtime_error("injected child failure");
  co_return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                    "injected explicit failure");
}
DriverTask parentTask(unsigned &Destroyed, bool Throws) {
  Lifetime Parent{Destroyed};
  co_return co_await childTask(Destroyed, Throws);
}
TEST(DriverTaskOwnership, CancellationReleasesEverySuspendedParentAndChild) {
  unsigned Destroyed = 0;
  DriverTask::Control Control;
  {
    auto Task = parentTask(Destroyed, false);
    Task.bind(Control);
    Control.Leaf.resume();
    ASSERT_TRUE(Control.Action);
    EXPECT_EQ(Destroyed, 0u);
  }
  EXPECT_EQ(Destroyed, 2u);
}
TEST(DriverTaskOwnership, ChildExceptionsPropagateAsOwnedRootErrors) {
  unsigned Destroyed = 0;
  DriverTask::Control Control;
  auto Task = parentTask(Destroyed, true);
  Task.bind(Control);
  Control.Leaf.resume();
  Control.Action.reset();
  Control.Leaf.resume();
  ASSERT_TRUE(Task.done());
  EXPECT_FALSE(Control.Leaf);
  EXPECT_EQ(Destroyed, 2u);
  EXPECT_NE(llvm::toString(Task.takeResult()).find("injected child failure"),
            std::string::npos);
}
TEST(DriverTaskOwnership, ExplicitChildErrorsSurviveNestedCompletion) {
  unsigned Destroyed = 0;
  DriverTask::Control Control;
  auto Task = parentTask(Destroyed, false);
  Task.bind(Control);
  Control.Leaf.resume();
  Control.Action.reset();
  Control.Leaf.resume();
  ASSERT_TRUE(Task.done());
  EXPECT_EQ(Destroyed, 2u);
  EXPECT_EQ(llvm::toString(Task.takeResult()), "injected explicit failure");
}
std::filesystem::path fixture(const char *Name) {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         (std::string(Name) + ".sys");
}
DriverOptions resident() {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.InstructionLimit = 20000;
  O.TimeoutMilliseconds = 5000;
  O.CallbackDrainPolicy = DriverCallbackDrainPolicy::ForegroundReady;
  O.Unload = true;
  DriverRequest Create;
  Create.Kind = DriverRequestKind::Create;
  Create.Device = "\\Device\\NeverDIO";
  O.Requests.push_back(Create);
  DriverRequest Echo;
  Echo.ControlCode = 0x222000;
  Echo.Input = {0, 0x11, 0x22, 0x33};
  Echo.OutputSize = 4;
  O.Requests.push_back(Echo);
  DriverRequest Deferred;
  Deferred.ControlCode = 0x222020;
  Deferred.OutputSize = 4;
  O.Requests.push_back(Deferred);
  DriverRequest Cleanup;
  Cleanup.Kind = DriverRequestKind::Cleanup;
  O.Requests.push_back(Cleanup);
  DriverRequest Close;
  Close.Kind = DriverRequestKind::Close;
  O.Requests.push_back(Close);
  return O;
}
ExecutionRunOptions control(const DriverOptions &O, uint64_t Quantum = 7) {
  ExecutionRunOptions R;
  R.Limits = {O.InstructionLimit, O.EventLimit, O.TimeoutMilliseconds * 1000};
  R.InstructionQuantum = Quantum;
  R.ActionsPerRun = 1;
  R.PauseStages = {"after-entry"};
  R.EnableResourcePauses = false;
  return R;
}
llvm::Expected<ExecutionRunEvent> advance(ExecutionRun &Run) {
  for (unsigned N = 0; N < 100000; ++N) {
    auto E = Run.run();
    if (!E)
      return E.takeError();
    if (E->State == ExecutionRunState::Paused &&
        E->Reason == ExecutionRunReason::Quantum) {
      if (auto Error = Run.resume())
        return std::move(Error);
      continue;
    }
    return E;
  }
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "driver test exceeded action bound");
}
class PersistentDriver : public testing::TestWithParam<ExecutionContract> {
protected:
  void SetUp() override {
    auto B = queryExecutionBackendBuild(ExecutionBackendKind::Unicorn,
                                        GuestArchitecture::X64);
    ASSERT_TRUE(bool(B));
    if (B->Availability != BackendAvailability::Available)
      GTEST_SKIP() << B->Reason;
  }
  DriverOptions options() {
    DriverOptions O;
    O.Contract = GetParam();
    O.Backend = ExecutionBackendKind::Unicorn;
    O.TimeoutMilliseconds = 5000;
    return O;
  }
};
TEST_P(PersistentDriver, CreationRetainsOneCpuAndBudgetWithoutExecutingEntry) {
  auto O = options();
  auto P = createDriverExecutionRun(fixture("success"), O, control(O));
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  EXPECT_EQ(P->Run->state(), ExecutionRunState::Ready);
  EXPECT_FALSE(P->State->UnattachedCPU);
  EXPECT_TRUE(P->State->CPU);
  EXPECT_EQ(P->State->Budget->timePolicy(), ExecutionTimePolicy::ActiveWork);
  EXPECT_EQ(P->Run->inspect()->Instructions, 0u);
  EXPECT_EQ(P->State->Result.Instructions, 0u);
  EXPECT_FALSE(P->State->Result.NTStatus);
  EXPECT_TRUE(P->State->Result.Calls.empty());
}
TEST_P(PersistentDriver, ClockAndDisabledCETReadsSurviveActionBoundaries) {
  auto O = options();
  auto P = createDriverExecutionRun(fixture("success"), O, control(O, 1));
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  // RDSSPD must preserve the full RAX when CET is disabled. Cooperative TSC
  // reads return the scheduler's zero clock; RDTSCP also writes processor 0.
  const uint8_t Code[] = {
      0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xf3,
      0x0f, 0x1e, 0xc8, 0x48, 0xba, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33,
      0x22, 0x11, 0x48, 0x31, 0xd0, 0x75, 0x1d, 0xb8, 1,    0,    0,
      0,    0xba, 1,    0,    0,    0,    0x0f, 0x31, 0x0b, 0xc2, 0x75,
      13,   0xb9, 1,    0,    0,    0,    0x0f, 1,    0xf9, 0x0b, 0xc2,
      0x0b, 0xc1, 0xc3, 0xb8, 1,    0,    0,    0,    0xc3};
  ASSERT_EQ(
      llvm::toString(P->State->CPU->writeBacking(P->State->Image->Entry, Code)),
      "");
  auto E = advance(*P->Run);
  ASSERT_TRUE(bool(E)) << llvm::toString(E.takeError());
  EXPECT_EQ(E->Reason, ExecutionRunReason::Stage) << E->Diagnostic;
  EXPECT_EQ(E->Stage, "after-entry");
  EXPECT_EQ(P->State->Result.NTStatus, 0u);
}
TEST_P(PersistentDriver, ExactMicrosecondTimeoutUsesTheSameCumulativeBudget) {
  auto O = options();
  O.TimeoutMilliseconds = 5001;
  auto C = control(O);
  C.Limits.TimeoutMicroseconds = 5000123;
  auto P = createDriverExecutionRun(fixture("success"), O, C);
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  EXPECT_EQ(P->State->Budget->limits().TimeoutMicroseconds, 5000123u);
  EXPECT_EQ(P->Run->inspect()->Instructions, 0u);
  auto E = advance(*P->Run);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->Reason, ExecutionRunReason::Stage) << E->Diagnostic;
  EXPECT_EQ(P->State->Budget->limits().TimeoutMicroseconds, 5000123u);
  EXPECT_GT(P->State->Budget->instructions(), 0u);
}
TEST_P(PersistentDriver,
       AfterEntryPrecedesTheFirstDrainAndPreservesRamAcrossRequests) {
  auto O = resident();
  O.Contract = GetParam();
  O.Backend = ExecutionBackendKind::Unicorn;
  auto P = createDriverExecutionRun(fixture("driver_resident_worker"), O,
                                    control(O, 1));
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  const auto *CPU = P->State->CPU;
  const auto *Kernel = P->State->Kernel.get();
  const auto *Budget = P->State->Budget.get();
  auto Stage = advance(*P->Run);
  ASSERT_TRUE(bool(Stage)) << llvm::toString(Stage.takeError());
  ASSERT_EQ(Stage->Reason, ExecutionRunReason::Stage) << Stage->Diagnostic;
  EXPECT_EQ(Stage->Stage, "after-entry");
  EXPECT_EQ(P->State->Result.NTStatus, 0u);
  EXPECT_FALSE(P->State->Result.UnloadCompleted);
  EXPECT_TRUE(P->State->Result.Requests.empty());
  EXPECT_TRUE(std::any_of(
      P->State->Result.Calls.begin(), P->State->Result.Calls.end(),
      [](const auto &E) { return E.Name == "PsCreateSystemThread"; }));
  EXPECT_FALSE(std::any_of(
      P->State->Result.Calls.begin(), P->State->Result.Calls.end(),
      [](const auto &E) { return E.Name == "KeDelayExecutionThread"; }));
  const auto Before = P->Run->inspect()->Instructions;
  const uint8_t Marker[] = {1, 0x80, 0xff, 4};
  ASSERT_EQ(llvm::toString(P->Run->writeMemory(profile::StackBase, Marker)),
            "");
  ASSERT_EQ(llvm::toString(P->Run->resume()), "");
  auto End = advance(*P->Run);
  ASSERT_TRUE(bool(End)) << llvm::toString(End.takeError());
  ASSERT_EQ(End->State, ExecutionRunState::Completed) << End->Diagnostic;
  EXPECT_EQ(P->State->CPU, CPU);
  EXPECT_EQ(P->State->Kernel.get(), Kernel);
  EXPECT_EQ(P->State->Budget.get(), Budget);
  EXPECT_GT(P->Run->inspect()->Instructions, Before);
  EXPECT_EQ(P->State->Result.Instructions, P->Run->inspect()->Instructions);
  EXPECT_TRUE(P->State->Result.UnloadCompleted);
  ASSERT_EQ(P->State->Result.Requests.size(), 5u);
  EXPECT_EQ(P->State->Result.Requests[2].Output,
            (std::vector<uint8_t>{0xa0, 0xa1, 0xa2, 0xa3}));
  EXPECT_EQ(*P->Run->readMemory(profile::StackBase, 4),
            (std::vector<uint8_t>{1, 0x80, 0xff, 4}));
}
TEST_P(PersistentDriver,
       QuantaPreserveTheCompleteScheduledAndCooperativeDriverReport) {
  for (bool Scheduling : {false, true}) {
    auto O = resident();
    O.Contract = GetParam();
    O.Backend = ExecutionBackendKind::Unicorn;
    if (Scheduling)
      O.Scheduling = DriverScheduling{23, 1};
    auto Baseline = emulateDriver(fixture("driver_resident_worker"), O);
    ASSERT_TRUE(bool(Baseline)) << llvm::toString(Baseline.takeError());
    ASSERT_EQ(Baseline->Stop, DriverStopReason::Returned)
        << Baseline->Diagnostic;
    auto P = createDriverExecutionRun(fixture("driver_resident_worker"), O,
                                      control(O, 1));
    ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
    auto Stage = advance(*P->Run);
    ASSERT_TRUE(bool(Stage));
    ASSERT_EQ(Stage->Reason, ExecutionRunReason::Stage) << Stage->Diagnostic;
    ASSERT_EQ(llvm::toString(P->Run->resume()), "");
    auto End = advance(*P->Run);
    ASSERT_TRUE(bool(End));
    ASSERT_EQ(End->State, ExecutionRunState::Completed) << End->Diagnostic;
    EXPECT_EQ(driverResultJSON(P->State->Result), driverResultJSON(*Baseline));
  }
}
TEST_P(PersistentDriver, NestedCallbackAndUnloadFramesSurviveEveryActionYield) {
  auto O = options();
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.Unload = true;
  auto Baseline = emulateDriver(fixture("driver_callbacks"), O);
  ASSERT_TRUE(bool(Baseline)) << llvm::toString(Baseline.takeError());
  ASSERT_EQ(Baseline->NTStatus, 0u) << Baseline->Diagnostic;
  auto P =
      createDriverExecutionRun(fixture("driver_callbacks"), O, control(O, 1));
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  auto Stage = advance(*P->Run);
  ASSERT_TRUE(bool(Stage));
  ASSERT_EQ(Stage->Reason, ExecutionRunReason::Stage) << Stage->Diagnostic;
  ASSERT_EQ(llvm::toString(P->Run->resume()), "");
  auto End = advance(*P->Run);
  ASSERT_TRUE(bool(End));
  ASSERT_EQ(End->State, ExecutionRunState::Completed) << End->Diagnostic;
  EXPECT_EQ(driverResultJSON(P->State->Result), driverResultJSON(*Baseline));
}
TEST_P(PersistentDriver, FailedEntryCompletesWithoutPublishingASuccessStage) {
  auto O = options();
  auto P = createDriverExecutionRun(fixture("failure"), O, control(O, 1));
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  auto End = advance(*P->Run);
  ASSERT_TRUE(bool(End));
  EXPECT_EQ(End->State, ExecutionRunState::Completed) << End->Diagnostic;
  EXPECT_EQ(P->State->Result.NTStatus, 0xc0000001u);
  EXPECT_EQ(P->State->Result.Stop, DriverStopReason::Returned);
}
TEST_P(PersistentDriver,
       UnsupportedModelOperationRemainsATerminalDriverFailure) {
  auto O = options();
  auto P = createDriverExecutionRun(fixture("unknown"), O, control(O, 1));
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  auto End = advance(*P->Run);
  ASSERT_TRUE(bool(End));
  EXPECT_EQ(End->State, ExecutionRunState::Failed);
  EXPECT_EQ(P->State->Result.Stop, DriverStopReason::UnsupportedAPI);
  EXPECT_NE(End->Diagnostic.find("NeverDUnsupportedFixtureAPI"),
            std::string::npos);
  EXPECT_FALSE(P->State->Result.NTStatus);
}
TEST_P(PersistentDriver,
       DestructionCancelsSuspendedNestedFramesAndReleasesTheCpu) {
  auto O = options();
  auto C = control(O, 1);
  C.PauseStages.clear();
  auto P = createDriverExecutionRun(fixture("driver_callbacks"), O, C);
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  bool Nested = false;
  for (unsigned N = 0; N < 20000; ++N) {
    auto E = P->Run->run();
    ASSERT_TRUE(bool(E));
    ASSERT_EQ(E->State, ExecutionRunState::Paused) << E->Diagnostic;
    Nested = std::any_of(P->State->StackInUse.begin(),
                         P->State->StackInUse.end(), [](bool V) { return V; });
    if (Nested)
      break;
    ASSERT_EQ(llvm::toString(P->Run->resume()), "");
  }
  ASSERT_TRUE(Nested);
  std::weak_ptr<PhysicalMemory> Physical =
      P->State->CPU->addressSpace()->physicalMemory();
  P->Run.reset();
  EXPECT_TRUE(Physical.expired());
}
TEST_P(PersistentDriver, IndependentPausedDriversKeepSeparateObjectsAndMemory) {
  auto O = options();
  auto A = createDriverExecutionRun(fixture("success"), O, control(O));
  auto B = createDriverExecutionRun(fixture("success"), O, control(O));
  ASSERT_TRUE(bool(A) && bool(B));
  ASSERT_EQ(advance(*A->Run)->Reason, ExecutionRunReason::Stage);
  ASSERT_EQ(advance(*B->Run)->Reason, ExecutionRunReason::Stage);
  const auto Count = B->Run->inspect()->Instructions;
  EXPECT_NE(A->State->CPU->addressSpace(), B->State->CPU->addressSpace());
  EXPECT_NE(A->State->Kernel.get(), B->State->Kernel.get());
  const uint8_t Marker[] = {0x77};
  ASSERT_EQ(llvm::toString(A->Run->writeMemory(profile::StackBase, Marker)),
            "");
  EXPECT_EQ(*A->Run->readMemory(profile::StackBase, 1),
            (std::vector<uint8_t>{0x77}));
  EXPECT_EQ(*B->Run->readMemory(profile::StackBase, 1),
            (std::vector<uint8_t>{0}));
  ASSERT_EQ(llvm::toString(A->Run->resume()), "");
  EXPECT_EQ(advance(*A->Run)->State, ExecutionRunState::Completed);
  EXPECT_EQ(B->Run->state(), ExecutionRunState::Paused);
  EXPECT_EQ(B->Run->inspect()->Instructions, Count);
}
TEST_P(PersistentDriver,
       UnsupportedDriverInvocationAbiFailsBeforeAnyCallEffects) {
  auto O = options();
  auto P = createDriverExecutionRun(fixture("success"), O, control(O));
  ASSERT_TRUE(bool(P));
  ASSERT_EQ(advance(*P->Run)->Reason, ExecutionRunReason::Stage);
  const auto Count = P->Run->inspect()->Instructions;
  const auto SP = *P->State->CPU->reg(X64Register::SP);
  GuestInvocationRequest Request;
  Request.Entry = P->State->Image->Entry;
  Request.ABI = IntegerCallingConvention::SysVAMD64;
  EXPECT_NE(llvm::toString(P->Run->invoke(Request)), "");
  EXPECT_EQ(P->Run->state(), ExecutionRunState::Paused);
  EXPECT_EQ(P->Run->inspect()->Instructions, Count);
  EXPECT_EQ(*P->State->CPU->reg(X64Register::SP), SP);
}
TEST_P(PersistentDriver, ConflictingBudgetsFailBeforeOpeningAnInput) {
  auto O = options();
  auto C = control(O);
  ++C.Limits.Instructions;
  auto P = createDriverExecutionRun("unavailable-driver.sys", O, C);
  EXPECT_FALSE(bool(P));
  if (!P)
    EXPECT_NE(llvm::toString(P.takeError()).find("limits must match"),
              std::string::npos);
}
TEST_P(PersistentDriver, CrossThreadPauseRetainsTheLiveWorkerContinuation) {
  auto O = options();
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.InstructionLimit = 100000000;
  O.TimeoutMilliseconds = 30000;
  auto C = control(O, 100000000);
  C.ActionsPerRun = 4096;
  C.PauseStages.clear();
  // The counter-loop fixture exhausts its memory-write event quota before a
  // fast legacy CPU can be paused. Reuse the existing PAUSE-only worker so
  // this check observes cancellation rather than a different finite limit.
  auto P = createDriverExecutionRun(fixture("driver_resident_worker_runnable"),
                                    O, C);
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  std::optional<llvm::Expected<ExecutionRunEvent>> Event;
  std::thread Worker([&] { Event.emplace(P->Run->run()); });
  const auto Bound = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (P->Run->state() == ExecutionRunState::Ready &&
         std::chrono::steady_clock::now() < Bound)
    std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  auto Error = llvm::toString(P->Run->requestPause());
  Worker.join();
  ASSERT_EQ(Error, "") << (Event && bool(*Event) ? (*Event)->Diagnostic
                                                 : "no driver event")
                       << "; driver=" << P->State->Result.Diagnostic;
  ASSERT_TRUE(Event && bool(*Event));
  EXPECT_EQ((*Event)->Reason, ExecutionRunReason::UserPause)
      << (*Event)->Diagnostic;
  EXPECT_EQ(P->Run->state(), ExecutionRunState::Paused);
  const auto Count = P->Run->inspect()->Instructions;
  EXPECT_GT(Count, 0u);
  EXPECT_EQ(P->State->Result.Instructions, Count);
  EXPECT_EQ(P->State->Result.NTStatus, 0u);
  EXPECT_TRUE(std::any_of(P->State->StackInUse.begin(),
                          P->State->StackInUse.end(),
                          [](bool Used) { return Used; }));
  const auto PC = *P->State->CPU->reg(X64Register::PC);
  ASSERT_EQ(llvm::toString(P->Run->resume()), "");
  ASSERT_EQ(llvm::toString(P->Run->requestPause()), "");
  auto Again = P->Run->run();
  ASSERT_TRUE(bool(Again));
  EXPECT_EQ(Again->Reason, ExecutionRunReason::UserPause);
  EXPECT_EQ(P->Run->inspect()->Instructions, Count);
  EXPECT_EQ(*P->State->CPU->reg(X64Register::PC), PC);
}
TEST_P(PersistentDriver, ModelQuotaStopsRetainTheirTypedRuntimeReasons) {
  for (bool Events : {false, true}) {
    auto O = options();
    if (Events)
      O.EventLimit = 1;
    else
      O.InstructionLimit = 31;
    auto P = createDriverExecutionRun(fixture(Events ? "devices" : "loop"), O,
                                      control(O));
    ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
    auto End = advance(*P->Run);
    ASSERT_TRUE(bool(End));
    EXPECT_EQ(End->State, ExecutionRunState::Failed);
    EXPECT_EQ(End->Reason, Events ? ExecutionRunReason::EventLimit
                                  : ExecutionRunReason::InstructionLimit)
        << End->Diagnostic;
    EXPECT_EQ(P->State->Result.Stop, Events
                                         ? DriverStopReason::EventLimit
                                         : DriverStopReason::InstructionLimit);
  }
}
INSTANTIATE_TEST_SUITE_P(
    Contracts, PersistentDriver,
    testing::Values(ExecutionContract::Legacy, ExecutionContract::CheckedX64),
    [](const testing::TestParamInfo<ExecutionContract> &P) {
      return P.param == ExecutionContract::Legacy ? "Legacy" : "Checked";
    });
} // namespace
