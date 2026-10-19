//===- KernelProcessNotifyTests.cpp - Legacy process notifications --------===//
// NeverD Decompiler
///
/// \file
/// Legacy process notifications.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
namespace neverd::emulation {
namespace {
class KernelProcessNotify : public ::testing::Test {
protected:
  static constexpr uint64_t S = 0x22000000, PC = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  void reset(bool Native = false, bool ForceIntegrity = false) {
    Model.reset();
    auto M = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(M));
    Memory = std::move(*M);
    ok(Memory->map(S, 0x10000, Read | Write));
    ok(Memory->map(PC, 0x1000, Read | Execute));
    Result = DriverResult{};
    DriverOptions O;
    if (Native)
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    Result.Configuration = O;
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = PC;
    I.Size = 0x4000;
    I.DLLCharacteristics = ForceIntegrity ? 0x80 : 0;
    ok(Model->initialize(I, O));
    foreground();
  }
  void SetUp() override { reset(); }
  void foreground() {
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  uint64_t call(llvm::StringRef Name, std::initializer_list<uint64_t> A = {}) {
    auto V = Model->call(Name.str(), A);
    if (!V) {
      ADD_FAILURE() << llvm::toString(V.takeError());
      return UINT64_MAX;
    }
    return *V;
  }
  void error(llvm::StringRef Name, std::initializer_list<uint64_t> A,
             llvm::StringRef Text) {
    auto V = Model->call(Name.str(), A);
    ASSERT_FALSE(bool(V));
    EXPECT_NE(llvm::toString(V.takeError()).find(Text.str()),
              std::string::npos);
  }
  uint64_t reg(uint64_t Routine = PC, uint64_t Remove = 0) {
    return call("PsSetCreateProcessNotifyRoutine", {Routine, Remove});
  }
  KernelGuestCall
  event(DriverProcessEvent::Kind Kind = DriverProcessEvent::Kind::Create,
        uint32_t PID = 100, uint32_t Parent = 4, uint32_t Creator = 4) {
    auto C = Model->beginProcessEvent({Kind, PID, Parent, Creator, 0},
                                      Result.ProcessEvents.size());
    if (!C) {
      ADD_FAILURE() << llvm::toString(C.takeError());
      return {};
    }
    if (!*C)
      return {};
    ok(Model->beginGuestCall((**C).Token));
    Model->enterExecution(profile::StackBase, profile::StackBase, (**C).Token);
    return **C;
  }
  bool finish(KernelGuestCall C) {
    ok(Model->validateExecutionReturn(profile::StackBase, 0));
    auto R = Model->finishGuestCall(C.Token, 0xdeadbeef);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return false;
    }
    if (*R)
      EXPECT_EQ(**R, 0u);
    return bool(*R);
  }
  KernelGuestCall next() {
    auto C = Model->takeGuestCall();
    if (!C) {
      ADD_FAILURE() << "missing process callback";
      return {};
    }
    ok(Model->beginGuestCall(C->Token));
    Model->enterExecution(profile::StackBase, profile::StackBase, C->Token);
    return *C;
  }
};
TEST_F(KernelProcessNotify,
       RegistrationDoesNotGenerateProcessEventsAndBooleanUsesLowByte) {
  EXPECT_EQ(Model->argumentCount("PsSetCreateProcessNotifyRoutine"), 2u);
  EXPECT_EQ(reg(PC, 0x100), 0u);
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_TRUE(Result.ProcessEvents.empty());
  EXPECT_EQ(reg(PC, 0x101), 0u);
  EXPECT_EQ(reg(PC, 1), 0xc000007au);
  EXPECT_EQ(reg(0), 0xc0000022u);
  EXPECT_EQ(reg(S), 0xc0000022u);
  EXPECT_EQ(Model->argumentCount("PsSetCreateProcessNotifyRoutineEx"), 2u);
}
TEST_F(KernelProcessNotify,
       DocumentedDuplicateAndCapacityStatusesAndSlotReuse) {
  EXPECT_EQ(reg(), 0u);
  EXPECT_EQ(reg(), 0xc000000du);
  for (unsigned I = 1; I < 64; ++I)
    EXPECT_EQ(reg(PC + I), 0u);
  EXPECT_EQ(reg(PC + 64), 0xc000000du);
  EXPECT_EQ(reg(PC + 17, 1), 0u);
  EXPECT_EQ(reg(PC + 64), 0u);
}
TEST_F(KernelProcessNotify, ExtendedAdmissionUsesOriginalImageIntegrity) {
  reset(true, false);
  EXPECT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 0}), 0xc0000022u);
  auto Module = Memory->readInteger(
      Model->driverObject() + windows::DriverSectionOffset, 8);
  ASSERT_TRUE(bool(Module));
  ok(Memory->writeInteger(*Module + 0x68, 0x20, 4));
  EXPECT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 0}), 0xc0000022u);
  reset(false, true);
  EXPECT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 0x100}), 0u);
  EXPECT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 0}), 0xc000000du);
  EXPECT_EQ(reg(), 0u);
  EXPECT_EQ(reg(PC, 1), 0u);
  EXPECT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 0x101}), 0u);
  EXPECT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 1}), 0xc000007au);
}

TEST_F(KernelProcessNotify, ExtendedExitUsesProcessObjectAndNullCreateInfo) {
  reset(false, true);
  EXPECT_EQ(event().Token.ID, 0u);
  ok(Model->completeProcessEvent());
  ASSERT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 0}), 0u);
  ASSERT_EQ(reg(PC + 1), 0u);
  auto C = event(DriverProcessEvent::Kind::Exit);
  ASSERT_EQ(C.Arguments.size(), 3u);
  EXPECT_EQ(C.Arguments[1], 100u);
  EXPECT_EQ(C.Arguments[2], 0u);
  EXPECT_EQ(call("PsGetProcessId", {C.Arguments[0]}), 100u);
  EXPECT_EQ(call("PsGetCurrentProcessId"), 100u);
  EXPECT_EQ(call("KeAreApcsDisabled"), 1u);
  EXPECT_EQ(call("KeAreAllApcsDisabled"), 0u);
  error("PsSetCreateProcessNotifyRoutineEx", {PC, 1}, "prohibited");
  EXPECT_FALSE(finish(C));
  auto D = next();
  EXPECT_EQ(D.Arguments, (std::vector<uint64_t>{4, 100, 0}));
  EXPECT_TRUE(finish(D));
  ok(Model->completeProcessEvent());
  EXPECT_EQ(call("PsLookupProcessByProcessId", {100, S}), 0xc000000bu);
  EXPECT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 1}), 0u);
  EXPECT_EQ(reg(PC + 1, 1), 0u);
}

TEST_F(KernelProcessNotify,
       ExtendedCreationMissingMetadataStopsBeforeLifecyclePublication) {
  reset(false, true);
  ASSERT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 0}), 0u);
  auto C = Model->beginProcessEvent(
      {DriverProcessEvent::Kind::Create, 100, 4, 4, 0}, 0);
  ASSERT_FALSE(bool(C));
  EXPECT_NE(llvm::toString(C.takeError()).find("PS_CREATE_NOTIFY_INFO"),
            std::string::npos);
  EXPECT_TRUE(Result.ProcessEvents.empty());
  EXPECT_EQ(call("PsLookupProcessByProcessId", {100, S}), 0xc000000bu);
  EXPECT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 1}), 0u);
  EXPECT_EQ(event().Token.ID, 0u);
  ok(Model->completeProcessEvent());
}

TEST_F(KernelProcessNotify, NativeExtendedDuplicatesRetainTypedSlotIdentity) {
  reset(true, true);
  EXPECT_EQ(event().Token.ID, 0u);
  ok(Model->completeProcessEvent());
  EXPECT_EQ(reg(), 0u);
  EXPECT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 0}), 0u);
  EXPECT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 0}), 0u);
  EXPECT_EQ(reg(PC, 1), 0u);
  auto C = event(DriverProcessEvent::Kind::Exit);
  EXPECT_EQ(C.Arguments[2], 0u);
  EXPECT_FALSE(finish(C));
  auto D = next();
  EXPECT_EQ(D.Arguments, C.Arguments);
  EXPECT_TRUE(finish(D));
  ok(Model->completeProcessEvent());
  EXPECT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 1}), 0u);
  EXPECT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 1}), 0u);
  EXPECT_EQ(reg(PC, 1), 0xc000007au);
}

TEST_F(KernelProcessNotify, ExtendedRemovalWaitRetainsOldGeneration) {
  reset(false, true);
  EXPECT_EQ(event().Token.ID, 0u);
  ok(Model->completeProcessEvent());
  ASSERT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 0}), 0u);
  auto C = event(DriverProcessEvent::Kind::Exit);
  Model->enterExecution(S, S);
  EXPECT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 1}), 0u);
  auto W = Model->takeWait();
  ASSERT_TRUE(W);
  EXPECT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 0}), 0u);
  auto Pending = Model->pollWait(*W);
  ASSERT_TRUE(bool(Pending));
  EXPECT_FALSE(*Pending);
  foreground();
  EXPECT_TRUE(finish(C));
  Model->enterExecution(S, S);
  auto Done = Model->pollWait(*W);
  ASSERT_TRUE(bool(Done));
  ASSERT_TRUE(*Done);
  EXPECT_EQ(**Done, 0u);
  foreground();
  ok(Model->completeProcessEvent());
  EXPECT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 1}), 0u);
}

TEST_F(KernelProcessNotify,
       ExtendedCallbacksShareCapacityAndRequirePassiveIRQL) {
  reset(false, true);
  for (unsigned I = 0; I < 32; ++I) {
    EXPECT_EQ(reg(PC + I), 0u);
    EXPECT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC + I, 0}), 0u);
  }
  EXPECT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC + 32, 0}),
            0xc000000du);
  EXPECT_EQ(call("KfRaiseIrql", {1}), 0u);
  error("PsSetCreateProcessNotifyRoutineEx", {PC, 1}, "PASSIVE_LEVEL");
  EXPECT_EQ(call("KeLowerIrql", {0}), 0u);
}
TEST_F(KernelProcessNotify,
       Native19041LegacyDuplicatesHaveIndependentSlotEntries) {
  reset(true);
  EXPECT_EQ(reg(), 0u);
  EXPECT_EQ(reg(), 0u);
  auto C = event();
  EXPECT_FALSE(finish(C));
  auto D = next();
  EXPECT_EQ(D.PC, PC);
  EXPECT_TRUE(finish(D));
  ok(Model->completeProcessEvent());
  EXPECT_EQ(Result.ProcessEvents.back().DeliveredRoutines.size(), 2u);
  EXPECT_EQ(reg(PC, 1), 0u);
  EXPECT_EQ(reg(PC, 1), 0u);
  EXPECT_EQ(reg(PC, 1), 0xc000007au);
}
TEST_F(KernelProcessNotify, PassiveIRQLIsRequired) {
  EXPECT_EQ(call("KfRaiseIrql", {1}), 0u);
  error("PsSetCreateProcessNotifyRoutine", {PC, 0}, "PASSIVE_LEVEL");
  EXPECT_EQ(call("KeLowerIrql", {0}), 0u);
}
TEST_F(KernelProcessNotify, CreateAndExitContextsAPCStateAndPostExitLookup) {
  ASSERT_EQ(reg(), 0u);
  auto C = event();
  EXPECT_EQ(C.Arguments, (std::vector<uint64_t>{4, 100, 1}));
  EXPECT_EQ(call("PsGetCurrentProcessId"), 4u);
  EXPECT_EQ(call("ExGetPreviousMode"), 0u);
  EXPECT_EQ(call("KeAreApcsDisabled"), 1u);
  EXPECT_EQ(call("KeAreAllApcsDisabled"), 0u);
  error("KeLeaveCriticalRegion", {}, "implicit critical region");
  EXPECT_EQ(call("KeEnterCriticalRegion"), 0u);
  EXPECT_EQ(call("KeLeaveCriticalRegion"), 0u);
  EXPECT_TRUE(finish(C));
  ok(Model->completeProcessEvent());
  EXPECT_EQ(call("KeAreApcsDisabled"), 0u);
  C = event(DriverProcessEvent::Kind::Create, 200, 4, 100);
  EXPECT_EQ(call("PsGetCurrentProcessId"), 100u);
  EXPECT_EQ(call("ExGetPreviousMode"), 0u);
  EXPECT_EQ(C.Arguments[0], 4u);
  EXPECT_TRUE(finish(C));
  ok(Model->completeProcessEvent());
  C = event(DriverProcessEvent::Kind::Exit, 200);
  EXPECT_EQ(C.Arguments, (std::vector<uint64_t>{4, 200, 0}));
  EXPECT_EQ(call("PsGetCurrentProcessId"), 200u);
  EXPECT_EQ(call("PsLookupProcessByProcessId", {200, S}), 0u);
  auto Object = Memory->readInteger(S, 8);
  ASSERT_TRUE(bool(Object));
  EXPECT_EQ(call("ObfDereferenceObject", {*Object}), 0u);
  EXPECT_TRUE(finish(C));
  ok(Model->completeProcessEvent());
  EXPECT_EQ(call("PsLookupProcessByProcessId", {200, S}), 0xc000000bu);
  EXPECT_TRUE(Result.ProcessEvents.back().Completed);
}
TEST_F(KernelProcessNotify, CallbackCannotRemoveAnyProcessRoutine) {
  ASSERT_EQ(reg(), 0u);
  ASSERT_EQ(reg(PC + 1), 0u);
  auto C = event();
  error("PsSetCreateProcessNotifyRoutine", {PC + 1, 1}, "prohibited");
  EXPECT_FALSE(finish(C));
  EXPECT_TRUE(finish(next()));
  ok(Model->completeProcessEvent());
}
TEST_F(KernelProcessNotify,
       LiveSlotTraversalSkipsRemovedEntriesAndSeesLaterAdditions) {
  ASSERT_EQ(reg(), 0u);
  ASSERT_EQ(reg(PC + 1), 0u);
  auto C = event();
  Model->enterExecution(S, S);
  EXPECT_EQ(reg(PC + 1, 1), 0u);
  EXPECT_EQ(reg(PC + 2), 0u);
  foreground();
  EXPECT_FALSE(finish(C));
  auto D = next();
  EXPECT_EQ(D.PC, PC + 2);
  EXPECT_TRUE(finish(D));
  ok(Model->completeProcessEvent());
  EXPECT_EQ(Result.ProcessEvents.back().DeliveredRoutines,
            (std::vector<uint64_t>{PC, PC + 2}));
}
TEST_F(KernelProcessNotify, RemovalWaitUsesOldGenerationWhileSlotCanBeReused) {
  ASSERT_EQ(reg(), 0u);
  auto C = event();
  Model->enterExecution(S, S);
  EXPECT_EQ(reg(PC, 1), 0u);
  auto W = Model->takeWait();
  ASSERT_TRUE(W);
  EXPECT_EQ(W->Type, KernelModel::Wait::Kind::ProcessNotifyRemove);
  auto Pending = Model->pollWait(*W);
  ASSERT_TRUE(bool(Pending));
  EXPECT_FALSE(*Pending);
  EXPECT_EQ(reg(PC), 0u);
  foreground();
  EXPECT_TRUE(finish(C));
  Model->enterExecution(S, S);
  auto Done = Model->pollWait(*W);
  ASSERT_TRUE(bool(Done));
  ASSERT_TRUE(*Done);
  EXPECT_EQ(**Done, 0u);
  foreground();
  ok(Model->completeProcessEvent());
  auto D = event(DriverProcessEvent::Kind::Create, 200);
  EXPECT_EQ(D.PC, PC);
  EXPECT_TRUE(finish(D));
  ok(Model->completeProcessEvent());
  EXPECT_EQ(reg(PC, 1), 0u);
}
TEST_F(KernelProcessNotify,
       NoRegistrationStillCommitsExplicitLifecycleWithoutInventingCalls) {
  EXPECT_EQ(event().Token.ID, 0u);
  ok(Model->completeProcessEvent());
  EXPECT_EQ(event(DriverProcessEvent::Kind::Exit).Token.ID, 0u);
  ok(Model->completeProcessEvent());
  EXPECT_EQ(call("PsLookupProcessByProcessId", {100, S}), 0xc000000bu);
  EXPECT_TRUE(Result.ProcessEvents[0].DeliveredRoutines.empty());
  EXPECT_TRUE(Result.ProcessEvents[1].Completed);
}
TEST_F(KernelProcessNotify,
       UnbalancedCallbackIsRejectedAndEventRemainsIncomplete) {
  ASSERT_EQ(reg(), 0u);
  auto C = event();
  EXPECT_EQ(call("KeEnterGuardedRegion"), 0u);
  auto R = Model->finishGuestCall(C.Token, 0);
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_FALSE(Result.ProcessEvents.back().Completed);
}
TEST_F(KernelProcessNotify, UnloadRequiresUnregistration) {
  ASSERT_EQ(reg(), 0u);
  ok(Memory->writeInteger(Model->driverObject() + windows::DriverUnloadOffset,
                          PC, 8));
  ok(Model->finishEntry());
  auto U = Model->beginUnload();
  ASSERT_TRUE(bool(U));
  auto E = Model->finishUnload();
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("process callbacks"),
            std::string::npos);
  EXPECT_EQ(reg(PC, 1), 0u);
  ok(Model->finishUnload());
}
TEST_F(KernelProcessNotify, CallbackCodeRemainsPinnedDuringRemovalRundown) {
  const uint64_t Code = PC;
  ASSERT_EQ(reg(Code), 0u);
  auto E = Model->retireStack(PC, 0x1000);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  auto C = event();
  Model->enterExecution(S, S);
  EXPECT_EQ(reg(Code, 1), 0u);
  auto W = Model->takeWait();
  ASSERT_TRUE(W);
  E = Model->retireStack(PC, 0x1000);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  foreground();
  EXPECT_TRUE(finish(C));
  auto Done = Model->pollWait(*W);
  ASSERT_TRUE(bool(Done));
  ASSERT_TRUE(*Done);
  ok(Model->completeProcessEvent());
  ok(Model->retireStack(PC, 0x1000));
}
TEST_F(KernelProcessNotify, ExecutableMemoryOutsideAnImageIsNotAValidCallback) {
  ok(Memory->protect(S, 0x10000, Read | Write | Execute));
  EXPECT_EQ(reg(S), 0xc0000022u);
  EXPECT_EQ(reg(), 0u);
}
TEST(ProcessNotifyScenario, StrictLifecycleValidationAndJSONRoundTrip) {
  const char *Text = R"({"process_events":[
    {"kind":"create","process_id":100,"parent_id":4},
    {"kind":"create","process_id":200,"parent_id":4,"creator_process_id":100},
    {"kind":"exit","process_id":200,"parent_id":4},
    {"kind":"exit","process_id":100,"parent_id":4}]})";
  auto O = driverOptionsFromScenarioJSON(Text);
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  DriverResult R;
  R.Configuration = *O;
  auto JSON = llvm::json::parse(driverResultJSON(R));
  ASSERT_TRUE(bool(JSON));
  auto *Events =
      JSON->getAsObject()->getObject("configuration")->get("process_events");
  ASSERT_NE(Events, nullptr);
  auto RoundTrip = driverOptionsFromScenarioJSON(
      "{\"process_events\":" + llvm::formatv("{0}", *Events).str() + "}");
  ASSERT_TRUE(bool(RoundTrip)) << llvm::toString(RoundTrip.takeError());
  ASSERT_EQ(RoundTrip->ProcessEvents.size(), 4u);
  EXPECT_EQ(RoundTrip->ProcessEvents[1].CreatorProcessID, 100u);
  for (
      const char *Bad :
      {R"({"process_events":null})",
       R"({"process_events":[{"kind":"create","process_id":4,"parent_id":4}]})",
       R"({"process_events":[{"kind":"exit","process_id":99,"parent_id":4}]})",
       R"({"process_events":[{"kind":"create","process_id":99,"parent_id":4,"creator_process_id":88}]})",
       R"({"process_events":[{"kind":"create","process_id":99,"parent_id":4,"before_request":1}]})",
       R"({"process_events":[{"kind":"create","process_id":99,"parent_id":4,"unknown":1}]})",
       R"({"process_events":[{"kind":"create","process_id":99,"parent_id":4},{"kind":"exit","process_id":99,"parent_id":5}]})",
       R"({"process_events":[{"kind":"create","process_id":99,"parent_id":4},{"kind":"exit","process_id":99,"parent_id":4},{"kind":"create","process_id":99,"parent_id":4}]})"}) {
    auto V = driverOptionsFromScenarioJSON(Bad);
    ASSERT_FALSE(bool(V)) << Bad;
    llvm::consumeError(V.takeError());
  }
  O->ProcessEvents[0].Type = static_cast<DriverProcessEvent::Kind>(42);
  auto E = validateDriverProcessEvents(O->ProcessEvents, {});
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
TEST(ProcessNotifyDriver,
     CompiledCallbacksObserveContextsAPCsLifecycleAndUnload) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      SCOPED_TRACE(executionContractName(Contract));
      SCOPED_TRACE(Preemptive);
      DriverOptions O;
      O.Contract = Contract;
      O.Unload = true;
      if (Preemptive)
        O.Scheduling = DriverScheduling{17, 1};
      O.ProcessEvents = {{DriverProcessEvent::Kind::Create, 100, 4, 4, 0},
                         {DriverProcessEvent::Kind::Create, 200, 4, 100, 0},
                         {DriverProcessEvent::Kind::Exit, 200, 4, 4, 0},
                         {DriverProcessEvent::Kind::Exit, 100, 4, 4, 0}};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_process_notify.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
      ASSERT_EQ(R->ProcessEvents.size(), 4u);
      for (const auto &E : R->ProcessEvents) {
        EXPECT_TRUE(E.Completed);
        EXPECT_EQ(E.DeliveredRoutines.size(), 2u);
      }
      auto J = llvm::json::parse(driverResultJSON(*R));
      ASSERT_TRUE(bool(J));
      EXPECT_EQ(J->getAsObject()->getBoolean("scenario_success"), true);
    }
}
TEST(ProcessNotifyDriver,
     ExtendedExitCallbackUsesActualCreateAndCloseRequests) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    DriverOptions O;
    O.Contract = Contract;
    O.Unload = true;
    O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    for (auto Kind : {DriverRequestKind::Create, DriverRequestKind::Cleanup,
                      DriverRequestKind::Close}) {
      DriverRequest R;
      R.Kind = Kind;
      R.Device = "\\Device\\NeverDProcessEx";
      R.RequestorProcessID = 100;
      O.Requests.push_back(R);
    }
    O.ProcessEvents = {{DriverProcessEvent::Kind::Create, 100, 4, 4, 0},
                       {DriverProcessEvent::Kind::Exit, 100, 4, 4, 3}};
    auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_process_notify_ex.sys",
                           O);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
    EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
    EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
    ASSERT_EQ(R->ProcessEvents.size(), 2u);
    EXPECT_TRUE(R->ProcessEvents[0].DeliveredRoutines.empty());
    EXPECT_EQ(R->ProcessEvents[1].DeliveredRoutines.size(), 1u);
    EXPECT_TRUE(R->ProcessEvents[1].Completed);
    auto J = llvm::json::parse(driverResultJSON(*R));
    ASSERT_TRUE(bool(J));
    EXPECT_EQ(J->getAsObject()->getBoolean("scenario_success"), true);
  }
}

TEST(ProcessNotifyDriver, ExplicitEventsBracketActualFileRequests) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    DriverOptions O;
    O.Contract = Contract;
    O.Unload = true;
    for (auto Kind :
         {DriverRequestKind::Create, DriverRequestKind::DeviceControl,
          DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
      DriverRequest R;
      R.Kind = Kind;
      R.Device = "\\Device\\NeverDIO";
      R.RequestorProcessID = 100;
      if (Kind == DriverRequestKind::DeviceControl) {
        R.ControlCode = 0x222000;
        R.Input = {0, 1, 0x5a, 0xff};
        R.OutputSize = 8;
      }
      O.Requests.push_back(R);
    }
    O.ProcessEvents = {{DriverProcessEvent::Kind::Create, 100, 4, 4, 0},
                       {DriverProcessEvent::Kind::Exit, 100, 4, 4, 4}};
    auto E = validateDriverProcessEvents(O.ProcessEvents, O.Requests);
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
    auto R = emulateDriver(
        std::filesystem::path(NEVERD_DRIVER_FIXTURES) / "driver_io.sys", O);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
    EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
    ASSERT_EQ(R->Requests.size(), 4u);
    EXPECT_EQ(R->Requests[1].Output,
              (std::vector<uint8_t>{0x5a, 0x5b, 0, 0xa5}));
    ASSERT_EQ(R->ProcessEvents.size(), 2u);
    EXPECT_TRUE(R->ProcessEvents[1].Completed);
    EXPECT_TRUE(R->ProcessEvents[1].DeliveredRoutines.empty());
    auto EarlyExit = O.ProcessEvents;
    EarlyExit[1].BeforeRequest = 2;
    E = validateDriverProcessEvents(EarlyExit, O.Requests);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    std::reverse(EarlyExit.begin(), EarlyExit.end());
    E = validateDriverProcessEvents(EarlyExit, O.Requests);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
}
TEST_F(KernelProcessNotify,
       ExplicitCreateInfoProtectsInputsAndVetoRetiresProcess) {
  reset(true, true);
  ASSERT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC, 0}), 0u);
  ASSERT_EQ(call("PsSetCreateProcessNotifyRoutineEx", {PC + 16, 0}), 0u);
  DriverProcessEvent E{DriverProcessEvent::Kind::Create, 100, 4, 4, 0};
  E.CreateInfo =
      DriverProcessCreateInfo{true, "\\??\\C:\\Test.exe", std::nullopt};
  auto C = Model->beginProcessEvent(E, 0);
  ASSERT_TRUE(bool(C)) << llvm::toString(C.takeError());
  ASSERT_TRUE(bool(*C));
  ok(Model->beginGuestCall((**C).Token));
  Model->enterExecution(profile::StackBase, profile::StackBase, (**C).Token);
  const auto Info = (**C).Arguments[2];
  auto Get = [&](uint64_t A, unsigned W = 8) {
    return llvm::cantFail(Memory->readInteger(A, W));
  };
  EXPECT_EQ(Get(Info), 72u);
  EXPECT_EQ(Get(Info + 56), 0u);
  for (const auto &[A, W, Write] :
       {std::tuple{Info + 40, 8u, true}, std::tuple{Info + 12, 4u, false},
        std::tuple{Get(Info + 40), 8u, false},
        std::tuple{Get(Info + 40) + 88, 8u, false}}) {
    auto Bad = Model->validateGuestAccess(A, W, Write);
    EXPECT_TRUE(bool(Bad));
    llvm::consumeError(std::move(Bad));
  }
  ok(Model->validateGuestAccess(Info + 64, 4, true));
  ok(Memory->writeInteger(Info + 64, 0xc0000022, 4));
  EXPECT_TRUE(finish(**C));
  EXPECT_FALSE(Model->takeGuestCall());
  ok(Model->completeProcessEvent());
  foreground();
  EXPECT_EQ(call("PsLookupProcessByProcessId", {100, S}), 0xc000000bu);
  EXPECT_EQ(Result.ProcessEvents[0].CreationStatus, 0xc0000022u);
  EXPECT_EQ(Result.ProcessEvents[0].DeliveredRoutines.size(), 1u);
  auto Dead = Model->validateGuestAccess(Info, 8, false);
  EXPECT_TRUE(bool(Dead));
  llvm::consumeError(std::move(Dead));
}
} // namespace
} // namespace neverd::emulation
