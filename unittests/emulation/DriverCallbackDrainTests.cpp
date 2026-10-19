//===- DriverCallbackDrainTests.cpp - Resident callback boundaries --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

///
/// \file
/// Resident callback boundaries.
///

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {
DriverOptions residentOptions() {
  DriverOptions O;
  O.Unload = true;
  O.CallbackDrainPolicy = DriverCallbackDrainPolicy::ForegroundReady;
  O.InstructionLimit = 20000;
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
auto runResident(const DriverOptions &O,
                 const char *Name = "driver_resident_worker") {
  return emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                           (std::string(Name) + ".sys"),
                       O);
}

TEST(DriverCallbackDrain, PolicyIsExplicitInheritedValidatedAndReported) {
  auto Default = driverOptionsFromScenarioJSON("{}");
  ASSERT_TRUE(bool(Default));
  EXPECT_FALSE(Default->CallbackDrainPolicy);
  auto Parsed = driverOptionsFromScenarioJSON(
      R"({"callback_drain_policy":"foreground-ready"})");
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->CallbackDrainPolicy,
            DriverCallbackDrainPolicy::ForegroundReady);
  auto Inherited = driverOptionsFromScenarioJSON("{}", *Parsed);
  ASSERT_TRUE(bool(Inherited));
  EXPECT_EQ(Inherited->CallbackDrainPolicy, Parsed->CallbackDrainPolicy);
  auto Reset = driverOptionsFromScenarioJSON(
      R"({"callback_drain_policy":"complete"})", *Parsed);
  ASSERT_TRUE(bool(Reset));
  EXPECT_EQ(Reset->CallbackDrainPolicy, DriverCallbackDrainPolicy::Complete);
  DriverResult Result;
  Result.Configuration = *Parsed;
  auto JSON = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(JSON));
  EXPECT_EQ(JSON->getAsObject()
                ->getObject("configuration")
                ->getString("callback_drain_policy"),
            "foreground-ready");
  for (const char *Bad : {R"({"callback_drain_policy":true})",
                          R"({"callback_drain_policy":null})",
                          R"({"callback_drain_policy":"ready"})"}) {
    auto Rejected = driverOptionsFromScenarioJSON(Bad);
    ASSERT_FALSE(bool(Rejected));
    EXPECT_NE(
        llvm::toString(Rejected.takeError()).find("callback_drain_policy"),
        std::string::npos);
  }
  Parsed->CallbackDrainPolicy = static_cast<DriverCallbackDrainPolicy>(42);
  auto Rejected = driverOptionsFromScenarioJSON("{}", *Parsed);
  ASSERT_FALSE(bool(Rejected));
  llvm::consumeError(Rejected.takeError());
}

TEST(DriverCallbackDrain,
     ResidentWorkerServicesBusinessRequestsAndJoinsOnUnload) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      auto O = residentOptions();
      O.Contract = Contract;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = runResident(O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u);
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
      ASSERT_EQ(R->Requests.size(), 5u);
      for (const auto &Request : R->Requests) {
        EXPECT_TRUE(Request.Completed);
        EXPECT_EQ(Request.IOStatus, 0u);
      }
      EXPECT_EQ(R->Requests[1].Output,
                (std::vector<uint8_t>{0x5a, 0x4b, 0x78, 0x69}));
      EXPECT_EQ(R->Requests[2].DispatchStatus, 0x103u);
      EXPECT_EQ(R->Requests[2].Output,
                (std::vector<uint8_t>{0xa0, 0xa1, 0xa2, 0xa3}));
    }
}

TEST(DriverCallbackDrain, CompletePolicyStillDrainsBeforeAdmittingRequests) {
  auto O = residentOptions();
  O.CallbackDrainPolicy.reset();
  auto R = runResident(O);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, DriverStopReason::InstructionLimit) << R->Diagnostic;
  EXPECT_TRUE(R->Requests.empty());
  EXPECT_FALSE(R->UnloadCompleted);
}

TEST(DriverCallbackDrain, LiveWorkerStillFailsTheFinalUnloadChecks) {
  auto R = runResident(residentOptions(), "driver_resident_worker_leak");
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, DriverStopReason::ModelError) << R->Diagnostic;
  EXPECT_NE(R->Diagnostic.find("live system thread"), std::string::npos);
  EXPECT_FALSE(R->UnloadCompleted);
}

TEST(DriverCallbackDrain, RunnableWorkerCannotBeSkippedToReachForeground) {
  auto R = runResident(residentOptions(), "driver_resident_worker_runnable");
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, DriverStopReason::InstructionLimit) << R->Diagnostic;
  EXPECT_TRUE(R->Requests.empty());
  EXPECT_FALSE(R->UnloadCompleted);
}

TEST(DriverCallbackDrain, ParkedWorkItemStillDrainsBeforeForeground) {
  auto R = runResident(residentOptions(), "driver_resident_worker_workitem");
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, DriverStopReason::InstructionLimit) << R->Diagnostic;
  EXPECT_TRUE(R->Requests.empty());
  EXPECT_FALSE(R->UnloadCompleted);
}
TEST(DriverCallbackDrain,
     ExplicitRequestDelaysRunParkedThreadsAtTheirDeadlines) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      auto O = residentOptions();
      O.Contract = Contract;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      O.Requests[0].DelayBefore100ns = 5000;
      for (size_t I : {1, 2}) {
        O.Requests[I].ControlCode = 0x222024;
        O.Requests[I].Input.clear();
      }
      O.Requests[1].DelayBefore100ns = 5000;
      O.Requests[2].DelayBefore100ns = 20000;
      auto R = runResident(O, "driver_resident_worker_pacing");
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
      ASSERT_EQ(R->RequestDelays.size(), 3u);
      ASSERT_EQ(R->Requests.size(), 5u);
      for (const auto &D : R->RequestDelays) {
        ASSERT_TRUE(D.CompletedAt100ns);
        EXPECT_GE(*D.CompletedAt100ns - D.StartedAt100ns, D.Duration100ns);
      }
      ASSERT_EQ(R->Requests[1].Output.size(), 4u);
      ASSERT_EQ(R->Requests[2].Output.size(), 4u);
      EXPECT_GE(R->Requests[1].Output[0], 2u);
      EXPECT_GT(R->Requests[2].Output[0], R->Requests[1].Output[0]);
      if (!Preemptive) {
        EXPECT_EQ(R->RequestDelays[0].CompletedAt100ns, 5000u);
        EXPECT_EQ(R->RequestDelays[1].CompletedAt100ns, 10000u);
        EXPECT_EQ(R->RequestDelays[2].CompletedAt100ns, 30000u);
        EXPECT_EQ(R->Requests[1].Output[0], 2u);
        EXPECT_EQ(R->Requests[2].Output[0], 4u);
      }
    }
}
TEST(DriverCallbackDrain,
     RequestDelayAdvancesAnIdleClockWithoutBackgroundWork) {
  auto O = residentOptions();
  O.Requests.erase(O.Requests.begin() + 2);
  O.Requests[0].DelayBefore100ns = 1000;
  O.Requests[1].DelayBefore100ns = 2000;
  auto R = runResident(O, "driver_io");
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  ASSERT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
  EXPECT_TRUE(R->UnloadCompleted);
  ASSERT_EQ(R->RequestDelays.size(), 2u);
  EXPECT_EQ(R->RequestDelays[0].CompletedAt100ns, 1000u);
  EXPECT_EQ(R->RequestDelays[1].CompletedAt100ns, 3000u);
}
TEST(DriverCallbackDrain,
     RequestDelaySchemaRejectsMalformedAndOverflowingDurations) {
  for (const auto *Value : {"-1", "true", "1.5", "9223372036854775808"}) {
    auto O = driverOptionsFromScenarioJSON(
        std::string(
            "{\"requests\":[{\"kind\":\"create\",\"delay_before_100ns\":") +
        Value + "}]}");
    ASSERT_FALSE(bool(O));
    llvm::consumeError(O.takeError());
  }
}
} // namespace
} // namespace neverd::emulation
