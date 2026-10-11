//===- DriverSchedulingTests.cpp - Preemptible driver execution ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "fixtures/driver_kmdf_power_policy_test.h"
#include "gtest/gtest.h"
#include "os/windows/kernel/KernelScheduler.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverReportFields.h"
#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#define NEVERD_PREEMPT_CASE(Name, Value) constexpr uint32_t Name = Value;
#define NEVERD_PREEMPT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_PREEMPT_TEXT(Name, Value) constexpr char Name[] = Value;
#include "fixtures/DriverPreemptiveCases.def"
#undef NEVERD_PREEMPT_TEXT
#undef NEVERD_PREEMPT_VALUE
#undef NEVERD_PREEMPT_CASE
struct Parameter {
  ExecutionBackendKind Backend;
  ExecutionContract Contract;
  uint32_t Code;
  std::string Name;
};
std::vector<Parameter> parameters() {
  std::vector<Parameter> Result;
  for (auto Backend : {ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                       ExecutionBackendKind::WHP})
    for (auto Contract :
         {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
      const std::string Prefix =
          std::string(executionBackendName(Backend)) +
          (Contract == ExecutionContract::Legacy ? LegacySuffix
                                                 : CheckedSuffix);
#define NEVERD_PREEMPT_CASE(Name, Value)                                       \
  Result.push_back({Backend, Contract, Value, Prefix + #Name});
#include "fixtures/DriverPreemptiveCases.def"
#undef NEVERD_PREEMPT_CASE
    }
  return Result;
}
DriverOptions scenario(const Parameter &P) {
  DriverOptions Options;
  Options.Backend = P.Backend;
  Options.Contract = P.Contract;
  Options.Scheduling = DriverScheduling{Quantum, InstructionTime};
  Options.InstructionLimit = InstructionBudget;
  // This oracle checks modeled time and instruction ordering. Allow each
  // bounded replay enough host time on contended native CI runners.
  Options.TimeoutMilliseconds = HostTimeoutMilliseconds;
  if (P.Code == TimerWakeThenReset || P.Code == TimeoutBeforeSignal ||
      P.Code == TimeoutBeforeTimer)
    Options.Scheduling->QuantumInstructions = WaiterQuantum;
  if (P.Code == TimeoutBeforeTimer)
    Options.Scheduling->InstructionTime100ns = CoarseInstructionTime;
  DriverRequest Create;
  Create.Kind = DriverRequestKind::Create;
  Options.Requests.push_back(Create);
  DriverRequest IO;
  IO.Kind = DriverRequestKind::DeviceControl;
  IO.ControlCode = P.Code;
  IO.OutputSize = OutputLength;
  if (P.Code == CancelBeforeDispatchReturn || P.Code == CancelUnderSpinLock ||
      P.Code == CancelPeerContext)
    IO.CancelAfter100ns = CancelDelay;
  Options.Requests.push_back(IO);
  for (auto Kind : {DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
    DriverRequest Request;
    Request.Kind = Kind;
    Options.Requests.push_back(Request);
  }
  Options.Unload = true;
  return Options;
}
class DriverScheduling : public testing::TestWithParam<Parameter> {};
TEST_P(DriverScheduling, BusyGuestMakesProgressWithoutCooperativeYield) {
  const auto &P = GetParam();
  auto Probe = createExecutionBackend(P.Backend, P.Contract, ProbeMemory);
  if (!Probe) {
    auto Error = Probe.takeError();
    const bool Unavailable = Error.isA<BackendUnavailableError>();
    auto Reason = llvm::toString(std::move(Error));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  auto Options = scenario(P);
  auto Run = [&]() {
    return emulateDriver(std::string(NEVERD_DRIVER_FIXTURES) + Image, Options);
  };
  auto Result = Run();
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_EQ(Result->Requests.size(), 4u);
  ASSERT_EQ(Result->Requests[1].Output.size(), OutputLength);
  EXPECT_EQ(Result->Requests[1].Output[0], SuccessMarker);
  if (P.Code == CancelBeforeDispatchReturn || P.Code == CancelUnderSpinLock ||
      P.Code == CancelPeerContext) {
    EXPECT_EQ(Result->Requests[1].DispatchStatus, PendingStatus);
    ASSERT_TRUE(Result->Requests[1].CancelRequestedAt100ns);
  }
  // Every backend must reproduce the same instruction-driven observation.
  auto Again = Run();
  ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
  ASSERT_EQ(Again->Stop, DriverStopReason::Returned) << Again->Diagnostic;
  EXPECT_EQ(Again->Instructions, Result->Instructions);
  EXPECT_EQ(Again->Requests[1].Output, Result->Requests[1].Output);
  ASSERT_EQ(Again->Calls.size(), Result->Calls.size());
  for (size_t I = 0; I < Result->Calls.size(); ++I) {
    EXPECT_EQ(Again->Calls[I].Name, Result->Calls[I].Name);
    EXPECT_EQ(Again->Calls[I].Phase, Result->Calls[I].Phase);
  }
  if (P.Code == BusyThread) {
    Options.Scheduling.reset();
    Options.InstructionLimit = CooperativeBudget;
    auto Cooperative = Run();
    ASSERT_TRUE(bool(Cooperative)) << llvm::toString(Cooperative.takeError());
    EXPECT_EQ(Cooperative->Stop, DriverStopReason::InstructionLimit);
    EXPECT_EQ(Cooperative->Instructions, CooperativeBudget);
    for (uint64_t Slice : {uint64_t(1), Quantum + 1}) {
      Options.Scheduling =
          neverd::emulation::DriverScheduling{Slice, InstructionTime};
      Options.InstructionLimit = InstructionBudget;
      auto Tiny = Run();
      ASSERT_TRUE(bool(Tiny)) << llvm::toString(Tiny.takeError());
      EXPECT_EQ(Tiny->Stop, DriverStopReason::Returned) << Tiny->Diagnostic;
    }
    Options.InstructionLimit = 1;
    auto Limited = Run();
    ASSERT_TRUE(bool(Limited)) << llvm::toString(Limited.takeError());
    EXPECT_EQ(Limited->Stop, DriverStopReason::InstructionLimit);
    EXPECT_EQ(Limited->Instructions, 1u);
  }
}
INSTANTIATE_TEST_SUITE_P(Preemptive, DriverScheduling,
                         testing::ValuesIn(parameters()),
                         [](const testing::TestParamInfo<Parameter> &P) {
                           return P.param.Name;
                         });
namespace pofx_case {
#define NEVERD_PREEMPT_POFX(Name, Value) constexpr uint8_t Name = Value;
#include "fixtures/DriverPreemptiveCases.def"
#undef NEVERD_PREEMPT_POFX
} // namespace pofx_case
std::vector<Parameter> poFxParameters() {
  std::vector<Parameter> Result;
  for (auto Backend : {ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                       ExecutionBackendKind::WHP})
    for (auto Contract :
         {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
      const std::string Prefix =
          std::string(executionBackendName(Backend)) +
          (Contract == ExecutionContract::Legacy ? LegacySuffix
                                                 : CheckedSuffix);
#define NEVERD_PREEMPT_POFX(Name, Value)                                       \
  Result.push_back({Backend, Contract, Value, Prefix + #Name});
#include "fixtures/DriverPreemptiveCases.def"
#undef NEVERD_PREEMPT_POFX
    }
  return Result;
}
class DriverSchedulingPoFx : public testing::TestWithParam<Parameter> {};
TEST_P(DriverSchedulingPoFx, BlockingCallbacksRetainTheirOriginalThread) {
#ifdef NEVERD_WDM_POFX_FIXTURE
  const auto &P = GetParam();
  auto Probe = createExecutionBackend(P.Backend, P.Contract, ProbeMemory);
  if (!Probe) {
    auto Error = Probe.takeError();
    const bool Unavailable = Error.isA<BackendUnavailableError>();
    auto Reason = llvm::toString(std::move(Error));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  DriverOptions Options;
  Options.Backend = P.Backend;
  Options.Contract = P.Contract;
  Options.Scheduling =
      neverd::emulation::DriverScheduling{Quantum, InstructionTime};
  DriverPnpDevice Device;
  Device.ID = PoFxDevice;
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  Device.InitialReportedDevicePower = DevicePowerState::D0;
  Options.PnpDevices.push_back(Device);
  auto Pnp = [&](DevicePnpRequest Minor) {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Pnp;
    Request.DeviceID = PoFxDevice;
    Request.Pnp = DriverPnpOperation{Minor, DriverBusCompletion{0, 0}};
    return Request;
  };
  auto File = [&](DriverRequestKind Kind) {
    DriverRequest Request;
    Request.Kind = Kind;
    Request.DeviceID = PoFxDevice;
    Request.File = 1;
    return Request;
  };
  auto IO = File(DriverRequestKind::DeviceControl);
  IO.ControlCode = PoFxControlCode;
  IO.Input = {uint8_t(P.Code)};
  if (P.Code == pofx_case::ExplicitState || P.Code == pofx_case::DeferredState)
    IO.PowerPolicyEvents.push_back(
        {1, PoFxDevice, DriverPowerPolicyAction::ComponentIdleState, 0, 1});
  if (P.Code == pofx_case::Power)
    IO.PowerPolicyEvents.push_back(
        {1, PoFxDevice, DriverPowerPolicyAction::PowerNotRequired});
  Options.Requests = {Pnp(DevicePnpRequest::Start),
                      File(DriverRequestKind::Create),
                      IO,
                      File(DriverRequestKind::Cleanup),
                      File(DriverRequestKind::Close),
                      Pnp(DevicePnpRequest::QueryRemove),
                      Pnp(DevicePnpRequest::Remove)};
  Options.Unload = true;
  auto Result = emulateDriver(NEVERD_WDM_POFX_FIXTURE, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_TRUE(Result->UnloadCompleted);
  for (const auto &Request : Result->Requests) {
    EXPECT_TRUE(Request.Completed);
    EXPECT_EQ(Request.IOStatus, 0u);
  }
  EXPECT_FALSE(std::any_of(Result->Messages.begin(), Result->Messages.end(),
                           [](const auto &Text) {
                             return Text.find(PoFxFailure) != std::string::npos;
                           }));
#else
  GTEST_SKIP();
#endif
}
INSTANTIATE_TEST_SUITE_P(Preemptive, DriverSchedulingPoFx,
                         testing::ValuesIn(poFxParameters()),
                         [](const testing::TestParamInfo<Parameter> &P) {
                           return P.param.Name;
                         });
namespace power_case {
#define NEVERD_POWER_PREEMPT_TEXT(Name, Value) constexpr char Name[] = Value;
#include "fixtures/DriverPowerPreemptiveCases.def"
#undef NEVERD_POWER_PREEMPT_TEXT
} // namespace power_case
std::vector<Parameter> powerParameters() {
  std::vector<Parameter> Result;
  for (auto Backend : {ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                       ExecutionBackendKind::WHP})
    for (auto Contract :
         {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
      const std::string Prefix =
          std::string(executionBackendName(Backend)) +
          (Contract == ExecutionContract::Legacy ? LegacySuffix
                                                 : CheckedSuffix);
#define NEVERD_POWER_PREEMPT_CASE(Name, Code)                                  \
  Result.push_back({Backend, Contract, Code, Prefix + #Name});
#include "fixtures/DriverPowerPreemptiveCases.def"
#undef NEVERD_POWER_PREEMPT_CASE
    }
  return Result;
}
class DriverSchedulingPower : public testing::TestWithParam<Parameter> {};
TEST_P(DriverSchedulingPower,
       BusyDispatchAllowsIndependentPassivePowerCallbacks) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  const auto &P = GetParam();
  auto Probe = createExecutionBackend(P.Backend, P.Contract, ProbeMemory);
  if (!Probe) {
    auto Error = Probe.takeError();
    const bool Unavailable = Error.isA<BackendUnavailableError>();
    auto Reason = llvm::toString(std::move(Error));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  DriverOptions Options;
  Options.Backend = P.Backend;
  Options.Contract = P.Contract;
  Options.Scheduling =
      neverd::emulation::DriverScheduling{Quantum, InstructionTime};
  Options.ServiceName = power_case::Service;
  DriverPnpDevice Device;
  Device.ID = power_case::Device;
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  Device.WakeCapabilities = DriverWakeCapabilities{true, true};
  for (auto State : {DevicePowerState::D3, DevicePowerState::D0}) {
    DriverPowerOperation Power;
    Power.Minor = DevicePowerRequest::Set;
    Power.State = uint32_t(State);
    Power.BusCompletion = {0, TimerDelay};
    Device.RequestedDevicePower.push_back(Power);
  }
  Options.PnpDevices.push_back(Device);
  auto Pnp = [&](DevicePnpRequest Minor) {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Pnp;
    Request.DeviceID = power_case::Device;
    Request.Pnp = DriverPnpOperation{Minor, DriverBusCompletion{0, 0}};
    return Request;
  };
  auto File = [&](DriverRequestKind Kind) {
    DriverRequest Request;
    Request.Kind = Kind;
    Request.DeviceID = power_case::Device;
    Request.File = 1;
    return Request;
  };
  auto IO = File(DriverRequestKind::DeviceControl);
  IO.ControlCode = PoFxControlCode;
  IO.Input = {uint8_t(P.Code)};
  IO.OutputSize = KmdfPowerSnapshotWords * sizeof(uint32_t);
  IO.PowerPolicyEvents = {
      {0, power_case::Device, DriverPowerPolicyAction::Idle},
      {KmdfPowerWakeDelay100ns, power_case::Device,
       DriverPowerPolicyAction::Wake}};
  Options.Requests = {Pnp(DevicePnpRequest::Start),
                      File(DriverRequestKind::Create),
                      IO,
                      File(DriverRequestKind::Cleanup),
                      File(DriverRequestKind::Close),
                      Pnp(DevicePnpRequest::QueryRemove),
                      Pnp(DevicePnpRequest::Remove)};
  Options.Unload = true;
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_TRUE(Result->UnloadCompleted);
  ASSERT_EQ(Result->PowerPolicyEvents.size(), 2u);
  for (const auto &Event : Result->PowerPolicyEvents)
    EXPECT_TRUE(Event.OccurredAt100ns);
  for (const auto &Request : Result->Requests) {
    EXPECT_TRUE(Request.Completed);
    EXPECT_EQ(Request.IOStatus, 0u);
  }
#else
  GTEST_SKIP();
#endif
}
INSTANTIATE_TEST_SUITE_P(Preemptive, DriverSchedulingPower,
                         testing::ValuesIn(powerParameters()),
                         [](const testing::TestParamInfo<Parameter> &P) {
                           return P.param.Name;
                         });

TEST(DriverSchedulingPolicy, PassiveQueuesAndResumptionsShareReadyOrder) {
  KernelScheduler S(KernelScheduler::Limits{}, true);
  const KernelScheduler::Callback First{1, 1, 1, CallbackPC, {}};
  auto Second = First;
  Second.Object = 2;
  const auto Thread = llvm::cantFail(S.enqueueSystemThread(First));
  const auto Worker = llvm::cantFail(S.enqueueWorkItem(Second));
  const auto ResumedOrder = llvm::cantFail(S.issueReadyOrder());
  auto Next = llvm::cantFail(S.next(false));
  ASSERT_TRUE(Next);
  EXPECT_EQ(Next->ID, Thread);
  EXPECT_LT(Next->ReadyOrder, *S.nextPassiveReadyOrder());
  EXPECT_LT(*S.nextPassiveReadyOrder(), ResumedOrder);
  llvm::cantFail(S.finish(Next->ID));
  Next = llvm::cantFail(S.next(false));
  ASSERT_TRUE(Next);
  EXPECT_EQ(Next->ID, Worker);
  llvm::cantFail(S.finish(Next->ID));
}
TEST(DriverSchedulingPolicy, HighestPriorityPrecedesOlderPassiveWork) {
  KernelScheduler S(KernelScheduler::Limits{}, true);
  KernelScheduler::Callback Call{1, 1, 1, CallbackPC, {}};
  const auto First = llvm::cantFail(S.enqueueWorkItem(Call));
  Call.Object++;
  const auto Second = llvm::cantFail(S.enqueueSystemThread(Call));
  Call.Object++;
  const auto Third = llvm::cantFail(S.enqueueWorkItem(Call));
  llvm::cantFail(S.setThreadPriority(Second, thread_priority::Maximum));
  llvm::cantFail(S.setThreadPriority(Third, thread_priority::Maximum));
  for (auto ID : {Second, Third, First}) {
    auto Next = llvm::cantFail(S.next(false));
    ASSERT_TRUE(Next);
    EXPECT_EQ(Next->ID, ID);
    llvm::cantFail(S.finish(ID));
  }
}
TEST(DriverSchedulingPolicy,
     PriorityChangesReevaluateQueuedAndSuspendedThreads) {
  KernelScheduler S(KernelScheduler::Limits{}, true);
  KernelScheduler::Callback Call{1, 1, 1, CallbackPC, {}};
  const auto First = llvm::cantFail(S.enqueueSystemThread(Call));
  Call.Object++;
  const auto Second = llvm::cantFail(S.enqueueSystemThread(Call));
  const auto SecondOrder = S.nextPassiveThread()->ReadyOrder + 1;
  llvm::cantFail(S.setThreadPriority(Second, thread_priority::Maximum));
  EXPECT_EQ(S.nextPassiveThread()->ReadyOrder, SecondOrder);
  llvm::cantFail(S.setThreadPriority(Second, thread_priority::Minimum));
  auto Next = llvm::cantFail(S.next(false));
  ASSERT_TRUE(Next);
  ASSERT_EQ(Next->ID, First);
  llvm::cantFail(S.suspend(First));
  llvm::cantFail(S.setThreadPriority(First, thread_priority::Maximum));
  llvm::cantFail(S.resume(First));
  EXPECT_EQ(S.threadPriority(First), thread_priority::Maximum);
  EXPECT_TRUE(
      S.readyThread(First, Next->ReadyOrder).precedes(*S.nextPassiveThread()));
  llvm::cantFail(S.finish(First));
  Next = llvm::cantFail(S.next(false));
  ASSERT_TRUE(Next);
  EXPECT_EQ(Next->ID, Second);
  llvm::cantFail(S.finish(Second));
}
TEST(DriverSchedulingPolicy, InvalidPriorityPreservesCanonicalState) {
  KernelScheduler S;
  constexpr uint64_t Key = CallbackPC;
  EXPECT_EQ(llvm::cantFail(S.setThreadPriority(Key, thread_priority::Maximum)),
            thread_priority::Default);
  for (int32_t Priority : {-1, 0, thread_priority::Maximum + 1, INT32_MAX}) {
    auto Refused = S.setThreadPriority(Key, Priority);
    ASSERT_FALSE(bool(Refused));
    EXPECT_EQ(llvm::toString(Refused.takeError()),
              thread_priority::InvalidPriority);
    EXPECT_EQ(S.threadPriority(Key), thread_priority::Maximum);
  }
  S.forgetThreadPriority(Key);
  EXPECT_EQ(S.threadPriority(Key), thread_priority::Default);
}
TEST(DriverSchedulingPolicy, CooperativeDefaultRetainsWorkerFIFO) {
  KernelScheduler S;
  KernelScheduler::Callback Call{1, 1, 1, CallbackPC, {}};
  const auto Thread = llvm::cantFail(S.enqueueSystemThread(Call));
  Call.Object++;
  const auto Worker = llvm::cantFail(S.enqueueWorkItem(Call));
  llvm::cantFail(S.setThreadPriority(Thread, thread_priority::Maximum));
  auto Next = llvm::cantFail(S.next(false));
  ASSERT_TRUE(Next);
  EXPECT_EQ(Next->ID, Worker);
  llvm::cantFail(S.finish(Worker));
  Next = llvm::cantFail(S.next(false));
  ASSERT_TRUE(Next);
  EXPECT_EQ(Next->ID, Thread);
  llvm::cantFail(S.finish(Thread));
}
TEST(DriverSchedulingPolicy, RunningClockKeepsEveryPeriodicBoundary) {
  KernelScheduler S;
  const KernelScheduler::Callback Work{1, 1, 1, CallbackPC, {}};
  llvm::cantFail(S.enqueueWorkItem(Work));
  ASSERT_TRUE(llvm::cantFail(S.next(false)));
  EXPECT_FALSE(
      llvm::cantFail(S.setTimer(1, 1, -int64_t(TimerDelay), TimerPeriod)));
  auto Refused = S.advanceTo100ns(TimerDelay);
  ASSERT_TRUE(bool(Refused));
  llvm::consumeError(std::move(Refused));
  Refused = S.advanceExecutionTo100ns(TimerDelay + 1);
  ASSERT_TRUE(bool(Refused));
  llvm::consumeError(std::move(Refused));
  EXPECT_EQ(S.now100ns(), 0u);
  for (uint64_t I = 0; I < TimerCount; ++I) {
    llvm::cantFail(S.advanceExecutionTo100ns(
        TimerDelay + I * scheduler::TicksPerMillisecond));
    EXPECT_EQ(S.timerExpirationCount(), I + 1);
    EXPECT_TRUE(llvm::cantFail(S.consumeTimerSignal(1)));
    EXPECT_TRUE(S.active());
  }
}
TEST(DriverSchedulingPolicy,
     RunningClockCapacityFailurePreservesTimeAndExpiry) {
  KernelScheduler::Limits Limits;
  Limits.MaxPendingCallbacks = 1;
  KernelScheduler S(Limits);
  const KernelScheduler::Callback Work{1, 1, 1, CallbackPC, {}};
  llvm::cantFail(S.enqueueWorkItem(Work));
  ASSERT_TRUE(llvm::cantFail(S.next(false)));
  KernelScheduler::DpcCallback Dpc;
  static_cast<KernelScheduler::Callback &>(Dpc) = Work;
  Dpc.Object = 2;
  llvm::cantFail(S.setTimer(1, 1, -int64_t(TimerDelay), 0, Dpc));
  auto Error = S.advanceExecutionTo100ns(TimerDelay);
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  EXPECT_EQ(S.now100ns(), 0u);
  EXPECT_EQ(S.timerExpirationCount(), 0u);
  EXPECT_TRUE(S.isTimerArmed(1));
  EXPECT_EQ(S.active()->Object, 1u);
}
TEST(DriverSchedulingPolicy, RejectsMalformedAndOverflowingScenarios) {
#define NEVERD_PREEMPT_JSON(Name, Text)                                        \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    auto Parsed = driverOptionsFromScenarioJSON(Text);                         \
    EXPECT_FALSE(bool(Parsed));                                                \
    if (!Parsed)                                                               \
      llvm::consumeError(Parsed.takeError());                                  \
  }
#include "fixtures/DriverPreemptiveCases.def"
#undef NEVERD_PREEMPT_JSON
}
TEST(DriverSchedulingPolicy, ReportsExplicitPolicyAndPreservesDefaultShape) {
  DriverResult Result;
  auto Default = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
  EXPECT_EQ(Default.getAsObject()
                ->getObject(field::Configuration)
                ->get(driver_scheduling::Scheduling),
            nullptr);
  Result.Configuration.Scheduling =
      neverd::emulation::DriverScheduling{Quantum, InstructionTime};
  auto JSON = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
  auto *Policy = JSON.getAsObject()
                     ->getObject(field::Configuration)
                     ->getObject(driver_scheduling::Scheduling);
  ASSERT_NE(Policy, nullptr);
  EXPECT_EQ(Policy->getInteger(driver_scheduling::QuantumInstructions),
            Quantum);
  EXPECT_EQ(Policy->getInteger(driver_scheduling::InstructionTime100ns),
            InstructionTime);
}
} // namespace
} // namespace neverd::emulation
