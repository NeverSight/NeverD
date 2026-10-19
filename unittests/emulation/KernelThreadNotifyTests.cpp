//===- KernelThreadNotifyTests.cpp - Legacy thread notifications ----------===//
// NeverD Decompiler
///
/// \file
/// Legacy thread notifications.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include <cstdlib>
#include <filesystem>
namespace neverd::emulation {
namespace {
class KernelThreadNotify : public ::testing::Test {
protected:
  static constexpr uint64_t S = 0x60000000, PC = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  void SetUp() override {
    Memory = llvm::cantFail(UnicornBackend::create(8 * 1024 * 1024));
    ok(Memory->map(S, 0x10000, Read | Write));
    ok(Memory->map(PC, 4096, Read | Execute));
    ok(Memory->map(profile::StackBase, profile::StackSize, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverOptions O;
    O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    DriverImage I;
    I.Base = PC - 4096;
    I.Entry = PC;
    I.Size = 0x4000;
    ok(Model->initialize(I, O));
    foreground();
  }
  void foreground() {
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> A = {}) {
    auto V = Model->call(Name, A);
    if (!V) {
      ADD_FAILURE() << Name << ": " << llvm::toString(V.takeError());
      return UINT64_MAX;
    }
    return *V;
  }
  void error(const char *Name, std::initializer_list<uint64_t> A,
             llvm::StringRef Text) {
    auto V = Model->call(Name, A);
    ASSERT_FALSE(bool(V));
    EXPECT_NE(llvm::toString(V.takeError()).find(Text.str()),
              std::string::npos);
  }
  uint64_t get(uint64_t At, unsigned Size = 8) {
    return llvm::cantFail(Memory->readInteger(At, Size));
  }
  void put(uint64_t At, uint64_t V) { ok(Memory->writeInteger(At, V, 8)); }
  uint64_t reg(uint64_t P = PC) {
    return call("PsSetCreateThreadNotifyRoutine", {P});
  }
  uint64_t remove(uint64_t P = PC) {
    return call("PsRemoveCreateThreadNotifyRoutine", {P});
  }
  void create() {
    EXPECT_EQ(
        call("PsCreateSystemThread", {S, 0x40, 0, 0, S + 0x100, PC + 0x100, 0}),
        0u);
  }
  KernelGuestCall pending() {
    auto C = Model->takeGuestCall();
    if (!C) {
      ADD_FAILURE() << "missing thread notification";
      return {};
    }
    EXPECT_EQ(C->Token.Owner, GuestCallOwner::ThreadNotify);
    return *C;
  }
  bool finish(const KernelGuestCall &C) {
    ok(Model->beginGuestCall(C.Token));
    auto V = Model->finishGuestCall(C.Token, 0xdeadbeef);
    if (!V) {
      ADD_FAILURE() << llvm::toString(V.takeError());
      return false;
    }
    if (*V)
      EXPECT_EQ(**V, 0u);
    return bool(*V);
  }
  KernelScheduler::Invocation start() {
    auto Next = Model->nextScheduled(false);
    if (!Next) {
      ADD_FAILURE() << llvm::toString(Next.takeError());
      return {};
    }
    if (!*Next) {
      ADD_FAILURE() << "missing runnable thread";
      return {};
    }
    Model->enterExecution(profile::CallbackStackBase, (**Next).ID);
    return **Next;
  }
  void stop(const KernelScheduler::Invocation &T) {
    EXPECT_EQ(call("PsTerminateSystemThread", {0}), 0u);
    while (auto C = Model->takeGuestCall())
      EXPECT_TRUE(finish(*C));
    auto Status = Model->takeThreadTermination();
    ASSERT_TRUE(Status);
    EXPECT_EQ(*Status, 0u);
    ok(Model->finishScheduled(T.ID));
    foreground();
  }
};
TEST_F(KernelThreadNotify, RegistrationSlotsDuplicatesAndIRQL) {
  EXPECT_TRUE(Result.ThreadNotifications.empty());
  for (unsigned I = 0; I < 64; ++I)
    EXPECT_EQ(reg(), 0u);
  EXPECT_EQ(reg(PC + 1), 0xc000009au);
  EXPECT_EQ(remove(PC + 1), 0xc000007au);
  call("KfRaiseIrql", {1});
  error("PsSetCreateThreadNotifyRoutine", {PC}, "PASSIVE_LEVEL");
  EXPECT_EQ(remove(), 0u);
  call("KfRaiseIrql", {2});
  error("PsRemoveCreateThreadNotifyRoutine", {PC}, "IRQL");
  call("KeLowerIrql", {1});
  call("KeLowerIrql", {0});
  for (unsigned I = 0; I < 63; ++I)
    EXPECT_EQ(remove(), 0u);
  EXPECT_EQ(remove(), 0xc000007au);
  error("PsSetCreateThreadNotifyRoutine", {0}, "executable owned guest");
  error("PsSetCreateThreadNotifyRoutine", {S}, "executable owned guest");
}
TEST_F(KernelThreadNotify,
       CreationRetainsActorAndHoldsReadinessAndOutputPublication) {
  const auto Actor = call("PsGetCurrentThreadId");
  EXPECT_EQ(reg(), 0u);
  put(S, 0x1122334455667788);
  put(S + 0x100, 0x8877665544332211);
  create();
  EXPECT_EQ(get(S), 0x1122334455667788u);
  EXPECT_EQ(get(S + 0x100), 0x8877665544332211u);
  auto Next = Model->nextScheduled(false);
  ASSERT_TRUE(bool(Next));
  EXPECT_FALSE(*Next);
  auto C = pending();
  ASSERT_EQ(C.Arguments.size(), 3u);
  EXPECT_EQ(C.Arguments[0], 4u);
  EXPECT_EQ(C.Arguments[2], 1u);
  EXPECT_NE(C.Arguments[1], Actor);
  ok(Model->beginGuestCall(C.Token));
  EXPECT_EQ(call("PsGetCurrentThreadId"), Actor);
  EXPECT_EQ(call("KeAreApcsDisabled"), 1u);
  error("KeLeaveCriticalRegion", {}, "implicit critical region");
  call("KeEnterCriticalRegion");
  call("KeLeaveCriticalRegion");
  error("PsRemoveCreateThreadNotifyRoutine", {PC}, "callback is prohibited");
  auto Done = Model->finishGuestCall(C.Token, UINT64_MAX);
  ASSERT_TRUE(bool(Done));
  ASSERT_TRUE(*Done);
  EXPECT_EQ(**Done, 0u);
  EXPECT_NE(get(S), 0x1122334455667788u);
  EXPECT_EQ(get(S + 0x100), 4u);
  EXPECT_EQ(get(S + 0x108), C.Arguments[1]);
  ASSERT_EQ(Result.ThreadNotifications.size(), 1u);
  EXPECT_TRUE(Result.ThreadNotifications[0].Completed);
  EXPECT_EQ(Result.ThreadNotifications[0].DeliveredRoutines,
            (std::vector<uint64_t>{PC}));
  const auto H = get(S);
  EXPECT_EQ(remove(), 0u);
  auto T = start();
  stop(T);
  call("ZwClose", {H});
}
TEST_F(KernelThreadNotify,
       ExitCallbacksPrecedeSignalingAndKeepTerminatingIdentity) {
  reg();
  create();
  EXPECT_TRUE(finish(pending()));
  const auto H = get(S), Target = get(S + 0x108);
  auto T = start();
  EXPECT_EQ(call("PsGetCurrentThreadId"), Target);
  EXPECT_EQ(call("PsTerminateSystemThread", {0xc0000005}), 0u);
  EXPECT_FALSE(Model->takeThreadTermination());
  auto Early = Model->finishScheduled(T.ID);
  ASSERT_TRUE(bool(Early));
  llvm::consumeError(std::move(Early));
  EXPECT_EQ(call("PsIsThreadTerminating", {T.Object}), 1u);
  auto C = pending();
  EXPECT_EQ(C.Arguments, (std::vector<uint64_t>{4, Target, 0}));
  EXPECT_TRUE(Model->hasDeferredThreadTermination(C.Token));
  EXPECT_FALSE(Model->hasDeferredThreadTermination(
      {GuestCallOwner::ExecutiveCallback, C.Token.ID}));
  ok(Model->beginGuestCall(C.Token));
  EXPECT_EQ(call("PsGetCurrentThread"), T.Object);
  put(S + 0x200, 0);
  EXPECT_EQ(call("PsLookupThreadByThreadId", {Target, S + 0x208}), 0u);
  Model->enterExecution(S, S);
  EXPECT_EQ(call("KeWaitForSingleObject", {T.Object, 0, 0, 0, S + 0x200}),
            0x102u);
  Model->enterExecution(profile::CallbackStackBase, T.ID);
  auto Done = Model->finishGuestCall(C.Token, 1);
  ASSERT_TRUE(bool(Done));
  ASSERT_TRUE(*Done);
  EXPECT_EQ(Model->takeThreadTermination(),
            std::optional<uint32_t>{0xc0000005});
  ok(Model->finishScheduled(T.ID));
  foreground();
  EXPECT_EQ(call("KeWaitForSingleObject", {T.Object, 0, 0, 0, S + 0x200}), 0u);
  call("ObfDereferenceObject", {T.Object});
  EXPECT_EQ(remove(), 0u);
  call("ZwClose", {H});
  ASSERT_EQ(Result.ThreadNotifications.size(), 2u);
  EXPECT_FALSE(Result.ThreadNotifications[1].Create);
  EXPECT_TRUE(Result.ThreadNotifications[1].Completed);
}
TEST_F(KernelThreadNotify, RemovalRundownRetainsGenerationAfterSlotReuse) {
  reg();
  create();
  auto C = pending();
  ok(Model->beginGuestCall(C.Token));
  Model->enterExecution(S, S);
  EXPECT_EQ(remove(), 0u);
  auto W = Model->takeWait();
  ASSERT_TRUE(W);
  EXPECT_EQ(reg(), 0u);
  auto Wait = Model->pollWait(*W);
  ASSERT_TRUE(bool(Wait));
  EXPECT_FALSE(*Wait);
  foreground();
  auto Done = Model->finishGuestCall(C.Token, 0);
  ASSERT_TRUE(bool(Done));
  ASSERT_TRUE(*Done);
  Model->enterExecution(S, S);
  Wait = Model->pollWait(*W);
  ASSERT_TRUE(bool(Wait));
  ASSERT_TRUE(*Wait);
  EXPECT_EQ(**Wait, 0u);
  EXPECT_EQ(remove(), 0u);
  foreground();
  const auto H = get(S);
  auto T = start();
  stop(T);
  call("ZwClose", {H});
}
TEST_F(KernelThreadNotify,
       LaterRegistrationHasNoRetroactiveCreateButReceivesExit) {
  create();
  const auto H = get(S);
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_EQ(reg(), 0u);
  EXPECT_FALSE(Model->takeGuestCall());
  auto T = start();
  stop(T);
  EXPECT_EQ(remove(), 0u);
  call("ZwClose", {H});
  ASSERT_EQ(Result.ThreadNotifications.size(), 2u);
  EXPECT_TRUE(Result.ThreadNotifications[0].DeliveredRoutines.empty());
  EXPECT_EQ(Result.ThreadNotifications[1].DeliveredRoutines,
            (std::vector<uint64_t>{PC}));
}
TEST_F(KernelThreadNotify, LiveSlotTraversalDeliversDuplicatesInOrder) {
  reg();
  reg();
  create();
  auto C = pending();
  EXPECT_FALSE(finish(C));
  auto D = pending();
  EXPECT_EQ(C.Arguments, D.Arguments);
  EXPECT_TRUE(finish(D));
  const auto H = get(S);
  remove();
  remove();
  auto T = start();
  stop(T);
  call("ZwClose", {H});
  EXPECT_EQ(Result.ThreadNotifications[0].DeliveredRoutines,
            (std::vector<uint64_t>{PC, PC}));
}
TEST_F(KernelThreadNotify,
       NestedCreationKeepsIndependentTraversalsAndCriticalRegions) {
  reg();
  create();
  auto Outer = pending();
  ok(Model->beginGuestCall(Outer.Token));
  EXPECT_EQ(call("PsCreateSystemThread",
                 {S + 0x200, 0x40, 0, 0, S + 0x300, PC + 0x100, 0}),
            0u);
  auto Inner = pending();
  EXPECT_NE(Inner.Token.ID, Outer.Token.ID);
  EXPECT_NE(Inner.Arguments[1], Outer.Arguments[1]);
  EXPECT_TRUE(finish(Inner));
  EXPECT_EQ(call("KeAreApcsDisabled"), 1u);
  auto Done = Model->finishGuestCall(Outer.Token, 0);
  ASSERT_TRUE(bool(Done));
  ASSERT_TRUE(*Done);
  EXPECT_EQ(call("KeAreApcsDisabled"), 0u);
  const auto HOuter = get(S), HInner = get(S + 0x200);
  remove();
  auto T = start();
  EXPECT_EQ(call("PsGetCurrentThreadId"), Inner.Arguments[1]);
  stop(T);
  T = start();
  EXPECT_EQ(call("PsGetCurrentThreadId"), Outer.Arguments[1]);
  stop(T);
  call("ZwClose", {HInner});
  call("ZwClose", {HOuter});
}
TEST_F(KernelThreadNotify, UnbalancedCallbackDoesNotCommitCreation) {
  reg();
  put(S, 0x1122334455667788);
  create();
  auto C = pending();
  ok(Model->beginGuestCall(C.Token));
  call("KeEnterCriticalRegion");
  auto Bad = Model->finishGuestCall(C.Token, 0);
  ASSERT_FALSE(bool(Bad));
  llvm::consumeError(Bad.takeError());
  EXPECT_EQ(get(S), 0x1122334455667788u);
  EXPECT_FALSE(Result.ThreadNotifications[0].Completed);
  call("KeLeaveCriticalRegion");
  auto Done = Model->finishGuestCall(C.Token, 0);
  ASSERT_TRUE(bool(Done));
  ASSERT_TRUE(*Done);
  const auto H = get(S);
  remove();
  auto T = start();
  stop(T);
  call("ZwClose", {H});
}
TEST_F(KernelThreadNotify, CreatorProcessCanDifferFromTheNewSystemThread) {
  ok(Model->setUserRequestContext(true, 100));
  const auto Creator = call("PsGetCurrentThreadId");
  reg();
  put(S + 0x400, 48);
  put(S + 0x418, 0x200);
  EXPECT_EQ(call("PsCreateSystemThread",
                 {S, 0x40, S + 0x400, 0, S + 0x100, PC + 0x100, 0}),
            0u);
  auto C = pending();
  EXPECT_EQ(C.Arguments[0], 4u);
  ok(Model->beginGuestCall(C.Token));
  EXPECT_EQ(call("PsGetCurrentProcessId"), 100u);
  EXPECT_EQ(call("PsGetCurrentThreadId"), Creator);
  auto Done = Model->finishGuestCall(C.Token, 0);
  ASSERT_TRUE(bool(Done));
  ASSERT_TRUE(*Done);
  EXPECT_EQ(Result.ThreadNotifications.back().ActorProcessID, 100u);
  EXPECT_EQ(Result.ThreadNotifications.back().ProcessID, 4u);
  const auto H = get(S);
  remove();
  ok(Model->setUserRequestContext(false));
  auto T = start();
  stop(T);
  call("ZwClose", {H});
}
TEST_F(KernelThreadNotify, ExecutablePoolCallbackRemainsPinnedUntilRemoval) {
  const auto P = call("ExAllocatePoolWithTag", {0, 32, 0x54455354});
  ASSERT_NE(P, 0u);
  // Observe executable backing independently of the pool allocation policy.
  ok(Memory->protect(P & ~uint64_t(4095), 4096, Read | Write | Execute));
  EXPECT_EQ(reg(P), 0u);
  error("ExFreePoolWithTag", {P, 0x54455354},
        "registered or retiring thread callback");
  EXPECT_EQ(remove(P), 0u);
  EXPECT_EQ(call("ExFreePoolWithTag", {P, 0x54455354}), 0u);
}
TEST(DriverThreadNotify, LeakedRegistrationRejectsUnload) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    DriverOptions O;
    O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    O.Contract = Contract;
    O.Unload = true;
    auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_thread_notify_leak.sys",
                           O);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->NTStatus, 0u);
    EXPECT_EQ(R->Stop, DriverStopReason::ModelError);
    EXPECT_FALSE(R->UnloadCompleted);
    EXPECT_NE(R->Diagnostic.find("thread callbacks"), std::string::npos);
  }
}
TEST(DriverThreadNotify, CompiledLifecycleClients) {
  const char *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE"),
             *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (unsigned Reference : {0u, 1u, 2u}) {
        if (Reference && (!Image || !PDB))
          continue;
        SCOPED_TRACE(::testing::Message()
                     << "reference=" << Reference << " preemptive="
                     << Preemptive << " contract=" << unsigned(Contract));
        DriverOptions O;
        O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
        O.Contract = Contract;
        O.Unload = true;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        if (Reference)
          O.KernelImage = Image;
        if (Reference == 2)
          O.KernelPDB = PDB;
        auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                   "driver_thread_notify.sys",
                               O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
        EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
        EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
        ASSERT_EQ(R->ThreadNotifications.size(), 2u);
        for (const auto &E : R->ThreadNotifications) {
          EXPECT_TRUE(E.Completed);
          EXPECT_EQ(E.DeliveredRoutines.size(), 2u);
        }
      }
}
} // namespace
} // namespace neverd::emulation
