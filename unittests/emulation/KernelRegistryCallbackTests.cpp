//===- KernelRegistryCallbackTests.cpp - Registry filter continuations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Registry filter continuations.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
class KernelRegistryCallback : public testing::Test {
protected:
  static constexpr uint64_t S = 0x22000000, PC = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  void SetUp() override {
    auto Backend = UnicornBackend::create(8 * 1024 * 1024);
    ASSERT_TRUE(bool(Backend));
    Memory = std::move(*Backend);
    ok(Memory->map(S, 0x10000, Read | Write));
    ok(Memory->map(PC, 4096, Read | Execute));
    DriverOptions O;
    O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    O.Registry = std::vector<DriverRegistryKey>{
        {"\\Registry\\Machine\\Software\\NeverD", {{"Mode", 4, {1, 0, 0, 0}}}}};
    O.RegistryHives =
        std::vector<DriverRegistryHive>{{"\\Registry\\Machine\\Software"}};
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = PC;
    I.Size = 0x4000;
    Model = std::make_unique<KernelModel>(*Memory, Result);
    ok(Model->initialize(I, O));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
    unicode(S + 0x100, "\\Registry\\Machine\\Software\\NeverD");
    unicode(S + 0x200, "Mode");
    put(S + 0x300, 48, 4);
    put(S + 0x310, S + 0x100);
    put(S + 0x318, 0x240, 4);
  }
  void put(uint64_t A, uint64_t V, unsigned W = 8) {
    ok(Memory->writeInteger(A, V, W));
  }
  uint64_t get(uint64_t A, unsigned W = 8) {
    return llvm::cantFail(Memory->readInteger(A, W));
  }
  void unicode(uint64_t A, llvm::StringRef Text) {
    put(A, Text.size() * 2, 2);
    put(A + 2, Text.size() * 2, 2);
    put(A + 8, A + 16);
    for (size_t I = 0; I < Text.size(); ++I)
      put(A + 16 + I * 2, uint8_t(Text[I]), 2);
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> A) {
    auto R = Model->call(Name, A);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
  void error(const char *Name, std::initializer_list<uint64_t> A,
             llvm::StringRef Text) {
    auto R = Model->call(Name, A);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find(Text.str()),
              std::string::npos);
  }
  uint64_t reg(uint64_t Context = 0) {
    EXPECT_EQ(call("CmRegisterCallback", {PC, Context, S}), 0u);
    return get(S);
  }
  KernelGuestCall next(uint64_t Class) {
    auto C = Model->takeGuestCall();
    if (!C) {
      ADD_FAILURE() << "no callback";
      return {};
    }
    EXPECT_EQ(C->Token.Owner, GuestCallOwner::RegistryCallback);
    EXPECT_EQ(C->Arguments[1], Class);
    ok(Model->beginGuestCall(C->Token));
    Model->enterExecution(profile::StackBase, profile::StackBase, C->Token);
    return *C;
  }
  std::optional<uint64_t> finish(const KernelGuestCall &C,
                                 uint32_t Status = 0) {
    auto R = Model->finishGuestCall(C.Token, Status);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return {};
    }
    Model->enterExecution(profile::StackBase);
    return *R;
  }
  uint64_t open() {
    EXPECT_EQ(call("ZwOpenKey", {S + 8, 0xf003f, S + 0x300}), 0u);
    if (Model->takeGuestCall()) {
      ADD_FAILURE() << "open helper must precede registration";
      return 0;
    }
    return get(S + 8);
  }
  uint64_t mode() {
    ok(Model->snapshot());
    for (const auto &Key : *Result.Registry)
      for (const auto &V : Key.Values)
        if (V.Name == "Mode")
          return V.Data.at(0);
    ADD_FAILURE() << "missing mode";
    return UINT64_MAX;
  }
};

TEST_F(KernelRegistryCallback, FlushPublishesWholeHiveBetweenTypedPreAndPost) {
  const auto H = open(), Cookie = reg();
  EXPECT_EQ(call("ZwFlushKey", {H}), 0u);
  auto Pre = next(30);
  const auto Info = Pre.Arguments[2];
  const auto Object = get(Info);
  EXPECT_NE(Object, 0u);
  ok(Model->snapshot());
  EXPECT_EQ(Result.RegistryHives->at(0).FlushCount, 0u);
  put(Info + 8, 0x12345678);
  EXPECT_FALSE(finish(Pre));
  ok(Model->snapshot());
  EXPECT_EQ(Result.RegistryHives->at(0).FlushCount, 1u);
  auto Post = next(31);
  const auto After = Post.Arguments[2];
  EXPECT_EQ(get(After), Object);
  EXPECT_EQ(get(After + 8, 4), 0u);
  EXPECT_EQ(get(After + 16), Info);
  EXPECT_EQ(get(After + 32), 0x12345678u);
  EXPECT_EQ(finish(Post), 0u);
  call("CmUnRegisterCallback", {Cookie});
  call("ZwClose", {H});
}
TEST_F(KernelRegistryCallback, FlushPostOverrideKeepsAlreadyCommittedState) {
  const auto H = open(), Cookie = reg();
  call("ZwFlushKey", {H});
  auto Pre = next(30);
  EXPECT_FALSE(finish(Pre));
  auto Post = next(31);
  put(Post.Arguments[2] + 24, 0xc0000022, 4);
  EXPECT_EQ(finish(Post, 0xc0000503), 0xc0000022u);
  ok(Model->snapshot());
  EXPECT_EQ(Result.RegistryHives->at(0).FlushCount, 1u);
  call("CmUnRegisterCallback", {Cookie});
  call("ZwClose", {H});
}
TEST_F(KernelRegistryCallback, FlushPreBlockAndBypassDoNotCommitStorage) {
  const auto H = open(), Cookie = reg();
  for (uint32_t Status : {0xc0000022u, 0xc0000503u}) {
    call("ZwFlushKey", {H});
    auto Pre = next(30);
    const auto Returned = finish(Pre, Status);
    EXPECT_EQ(Returned, Status == 0xc0000503u ? 0u : Status);
    EXPECT_FALSE(Model->takeGuestCall());
    ok(Model->snapshot());
    EXPECT_EQ(Result.RegistryHives->at(0).FlushCount, 0u);
  }
  call("CmUnRegisterCallback", {Cookie});
  call("ZwClose", {H});
}
TEST_F(KernelRegistryCallback,
       InformationalStatusDoesNotBypassRegistryEffects) {
  const auto H = open(), Cookie = reg();
  put(S + 0x500, 7, 4);
  call("ZwSetValueKey", {H, S + 0x200, 0, 4, S + 0x500, 4});
  const auto Pre = next(1);
  EXPECT_FALSE(finish(Pre, 0x106));
  EXPECT_EQ(mode(), 7u);
  const auto Post = next(16);
  EXPECT_EQ(finish(Post), 0u);
  call("CmUnRegisterCallback", {Cookie});
  call("ZwClose", {H});
}
TEST_F(KernelRegistryCallback, RegistrationIsAtomicTypedAndDoesNotNotify) {
  put(S, 0xdeadbeef);
  error("CmRegisterCallback", {PC, 0, S + 0xffff}, "permissions");
  EXPECT_EQ(get(S), 0xdeadbeefu);
  const auto Cookie = reg();
  EXPECT_NE(Cookie, 0u);
  EXPECT_FALSE(Model->takeGuestCall());
  error("CmRegisterCallback", {PC, 0, S + 16}, "multiple legacy");
  EXPECT_EQ(call("CmUnRegisterCallback", {Cookie + 1}), 0xc000000du);
  EXPECT_EQ(call("CmUnRegisterCallback", {Cookie}), 0u);
  EXPECT_EQ(call("CmUnRegisterCallback", {Cookie}), 0xc000000du);
  const auto New = reg();
  EXPECT_NE(New, Cookie);
  EXPECT_EQ(call("CmUnRegisterCallback", {New}), 0u);
}
TEST_F(KernelRegistryCallback, OpenUsesCapturedRootAndOpaqueReservedFields) {
  const auto Cookie = reg();
  EXPECT_EQ(call("ZwOpenKey", {S + 8, 0xf003f, S + 0x300}), 0u);
  auto Pre = next(28);
  const auto P = Pre.Arguments[2];
  EXPECT_EQ(get(P), S + 0x100);
  EXPECT_NE(get(P + 8), 0u);
  EXPECT_EQ(get(P + 104), 1u);
  const auto Remaining = get(P + 112);
  EXPECT_EQ(get(Remaining, 2), get(S + 0x100, 2) - 20);
  for (auto [Address, Size, Write] :
       {std::tuple<uint64_t, uint32_t, bool>{S + 0x308, 8, true},
        {P + 16, 8, false},
        {P + 60, 4, false},
        {get(P + 72), 8, false}}) {
    auto E = Model->validateGuestAccess(Address, Size, Write);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  EXPECT_FALSE(finish(Pre));
  const auto H = get(S + 8);
  ASSERT_NE(H, 0u);
  auto Post = next(29);
  EXPECT_EQ(get(Post.Arguments[2]), get(get(P + 72)));
  EXPECT_TRUE(finish(Post));
  EXPECT_EQ(call("CmUnRegisterCallback", {Cookie}), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(KernelRegistryCallback, BlockingPreventsMutationAndPostDelivery) {
  const auto H = open(), Cookie = reg(S + 0x400);
  put(S + 0x500, 7, 4);
  EXPECT_EQ(call("ZwSetValueKey", {H, S + 0x200, 0, 4, S + 0x500, 4}), 0u);
  auto Pre = next(1);
  EXPECT_EQ(Pre.Arguments[0], S + 0x400);
  const auto Info = Pre.Arguments[2], Object = get(Info);
  EXPECT_NE(Object, 0u);
  EXPECT_NE(Object, H);
  EXPECT_EQ(get(Info + 8), S + 0x200);
  EXPECT_EQ(get(Info + 24), S + 0x500);
  auto Denied = Model->validateGuestAccess(Object, 1, false);
  ASSERT_TRUE(bool(Denied));
  llvm::consumeError(std::move(Denied));
  const auto Done = finish(Pre, 0xc0000022);
  ASSERT_TRUE(Done);
  EXPECT_EQ(*Done, 0xc0000022u);
  EXPECT_EQ(mode(), 1u);
  EXPECT_FALSE(Model->takeGuestCall());
  auto Expired = Model->validateGuestAccess(Info, 1, false);
  ASSERT_TRUE(bool(Expired));
  llvm::consumeError(std::move(Expired));
  EXPECT_EQ(call("CmUnRegisterCallback", {Cookie}), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}
TEST_F(KernelRegistryCallback, PostContextAndOverrideRetainCommittedValue) {
  const auto H = open(), Cookie = reg();
  put(S + 0x500, 7, 4);
  EXPECT_EQ(call("ZwSetValueKey", {H, S + 0x200, 0, 4, S + 0x500, 4}), 0u);
  auto Pre = next(1);
  const auto P = Pre.Arguments[2];
  ok(Model->validateGuestAccess(P + 40, 8, true));
  put(P + 40, S + 0x600);
  EXPECT_FALSE(finish(Pre));
  EXPECT_EQ(mode(), 7u);
  auto Post = next(16);
  const auto Q = Post.Arguments[2];
  EXPECT_EQ(get(Q + 16), P);
  EXPECT_EQ(get(Q + 32), S + 0x600);
  EXPECT_EQ(get(Q + 8, 4), 0u);
  ok(Model->validateGuestAccess(Q + 24, 4, true));
  put(Q + 24, 0xc0000022, 4);
  const auto Done = finish(Post, 0xc0000503);
  ASSERT_TRUE(Done);
  EXPECT_EQ(*Done, 0xc0000022u);
  EXPECT_EQ(mode(), 7u);
  EXPECT_EQ(call("CmUnRegisterCallback", {Cookie}), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}
TEST_F(KernelRegistryCallback,
       QueryBypassUsesCallbackOutputsWithoutRegistryEffects) {
  const auto H = open(), Cookie = reg();
  put(S + 0x500, 0xdeadbeef, 4);
  EXPECT_EQ(
      call("ZwQueryValueKey", {H, S + 0x200, 2, S + 0x500, 16, S + 0x520}), 0u);
  auto Pre = next(8);
  const auto P = Pre.Arguments[2];
  EXPECT_EQ(get(P + 24), S + 0x500);
  EXPECT_EQ(get(P + 40), S + 0x520);
  put(S + 0x500, 0x11223344, 4);
  put(S + 0x520, 4, 4);
  auto Done = finish(Pre, 0xc0000503);
  ASSERT_TRUE(Done);
  EXPECT_EQ(*Done, 0u);
  EXPECT_EQ(get(S + 0x500, 4), 0x11223344u);
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_EQ(mode(), 1u);
  EXPECT_EQ(call("CmUnRegisterCallback", {Cookie}), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}
TEST_F(KernelRegistryCallback,
       NotificationFieldsAndRecursiveOperationsFailClearly) {
  const auto H = open(), Cookie = reg();
  EXPECT_EQ(call("ZwDeleteKey", {H}), 0u);
  auto Pre = next(0);
  const auto P = Pre.Arguments[2];
  for (const auto &[A, Write] : {std::pair{P, true}, {P + 24, false}}) {
    auto E = Model->validateGuestAccess(A, 1, Write);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  error("CmUnRegisterCallback", {Cookie}, "prohibited");
  error("ZwDeleteKey", {H}, "recursive");
  auto Done = finish(Pre, 0xc0000022);
  ASSERT_TRUE(Done);
  EXPECT_EQ(call("CmUnRegisterCallback", {Cookie}), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}
TEST_F(KernelRegistryCallback,
       RemovalWaitsAcrossPreAndPostAndPreservesBorrowedThread) {
  const auto H = open(), Cookie = reg();
  EXPECT_EQ(call("ZwDeleteValueKey", {H, S + 0x200}), 0u);
  auto Pre = next(2);
  const auto Saved = Model->captureExecutionContext();
  Model->enterExecution(S + 0x8000, S + 0x8000);
  auto Borrowed = Model->validateGuestAccess(Pre.Arguments[2], 1, false);
  ASSERT_TRUE(bool(Borrowed));
  llvm::consumeError(std::move(Borrowed));
  error("ZwClose", {H}, "borrowed by an in-flight");
  EXPECT_EQ(call("CmUnRegisterCallback", {Cookie}), 0u);
  auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  auto Ready = Model->pollWait(*Wait);
  ASSERT_TRUE(bool(Ready));
  EXPECT_FALSE(*Ready);
  ok(Model->restoreExecutionContext(Saved));
  EXPECT_FALSE(finish(Pre));
  auto Post = next(17);
  EXPECT_TRUE(finish(Post));
  Model->enterExecution(S + 0x8000, S + 0x8000);
  Ready = Model->pollWait(*Wait);
  ASSERT_TRUE(bool(Ready));
  ASSERT_TRUE(*Ready);
  EXPECT_EQ(**Ready, 0u);
  EXPECT_EQ(call("CmUnRegisterCallback", {Cookie}), 0xc000000du);
  ok(Model->restoreExecutionContext(Saved));
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(KernelRegistryCallback, PrivateAPCRegionPreservesCallerNesting) {
  const auto H = open(), Cookie = reg();
  for (bool CallerCritical : {false, true}) {
    if (CallerCritical)
      EXPECT_EQ(call("KeEnterCriticalRegion", {}), 0u);
    EXPECT_EQ(call("KeAreApcsDisabled", {}), CallerCritical);
    EXPECT_EQ(call("ZwDeleteValueKey", {H, S + 0x200}), 0u);
    auto Pre = next(2);
    EXPECT_EQ(call("KeAreApcsDisabled", {}), 1u);
    EXPECT_EQ(call("KeAreAllApcsDisabled", {}), 0u);
    EXPECT_EQ(call("KeEnterCriticalRegion", {}), 0u);
    EXPECT_EQ(call("KeLeaveCriticalRegion", {}), 0u);
    auto Done = finish(Pre, 0xc0000022);
    ASSERT_TRUE(Done);
    EXPECT_EQ(*Done, 0xc0000022u);
    EXPECT_EQ(call("KeAreApcsDisabled", {}), CallerCritical);
    if (CallerCritical)
      EXPECT_EQ(call("KeLeaveCriticalRegion", {}), 0u);
  }
  EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
  EXPECT_EQ(call("CmUnRegisterCallback", {Cookie}), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(KernelRegistryCallback, UnbalancedAPCStateFailsAtCallbackReturn) {
  const auto H = open();
  reg();
  EXPECT_EQ(call("ZwDeleteValueKey", {H, S + 0x200}), 0u);
  auto Pre = next(2);
  EXPECT_EQ(call("KeEnterGuardedRegion", {}), 0u);
  auto Done = Model->finishGuestCall(Pre.Token, 0);
  ASSERT_FALSE(bool(Done));
  EXPECT_NE(llvm::toString(Done.takeError()).find("unbalanced APC"),
            std::string::npos);
}

TEST_F(KernelRegistryCallback, ZwCallbackKeepsActorAndRestoresPreviousMode) {
  const auto H = open(), Cookie = reg();
  ok(Model->setUserRequestContext(true, 0x3456));
  EXPECT_EQ(call("PsGetCurrentProcessId", {}), 0x3456u);
  EXPECT_EQ(call("ExGetPreviousMode", {}), 1u);
  EXPECT_EQ(call("ZwDeleteValueKey", {H, S + 0x200}), 0u);
  auto Prepared = Model->takeGuestCall();
  ASSERT_TRUE(Prepared);
  ok(Model->beginGuestCall(Prepared->Token));
  constexpr uint64_t Child = S + 0x8000;
  ok(Model->inheritExecutionContext(Child, profile::StackBase));
  Model->enterExecution(Child, profile::StackBase, Prepared->Token);
  EXPECT_EQ(call("PsGetCurrentProcessId", {}), 0x3456u);
  EXPECT_EQ(call("ExGetPreviousMode", {}), 0u);
  EXPECT_EQ(call("KeAreApcsDisabled", {}), 1u);
  auto Done = finish(*Prepared, 0xc0000022);
  ASSERT_TRUE(Done);
  EXPECT_EQ(call("PsGetCurrentProcessId", {}), 0x3456u);
  EXPECT_EQ(call("ExGetPreviousMode", {}), 1u);
  EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
  ok(Model->retireStack(Child, 4096));
  EXPECT_EQ(call("CmUnRegisterCallback", {Cookie}), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(KernelRegistryCallback,
       OperationKeepsUserAuthorityAfterChildStackRetires) {
  const auto H = open();
  EXPECT_EQ(call("IoCreateDevice",
                 {Model->driverObject(), 0, 0, 0x22, 0, 0, S + 0x900}),
            0u);
  for (unsigned Major : {0u, 14u}) {
    const auto Slot =
        Model->driverObject() + windows::DriverDispatchOffset + Major * 8;
    ok(Model->validateGuestAccess(Slot, 8, true));
    put(Slot, PC);
  }
  DriverRequest Create;
  Create.Kind = DriverRequestKind::Create;
  Create.RequestorProcessID = 0x3456;
  auto Invocation = Model->beginRequest(Create);
  ASSERT_TRUE(bool(Invocation)) << llvm::toString(Invocation.takeError());
  const auto CreateIRP = Invocation->Argument1;
  ok(Model->validateGuestAccess(CreateIRP + windows::IRPStatusOffset, 16,
                                true));
  put(CreateIRP + windows::IRPStatusOffset, 0, 4);
  put(CreateIRP + windows::IRPInformationOffset, 0);
  EXPECT_EQ(call("IofCompleteRequest", {CreateIRP, 0}), 0u);
  ok(Model->recordDispatchReturn(CreateIRP, 0));
  ok(Model->finalizeRequest(CreateIRP));
  DriverRequest IO;
  IO.ControlCode = 0x222003;
  IO.OutputSize = 16;
  IO.RequestorProcessID = 0x3456;
  Invocation = Model->beginRequest(IO);
  ASSERT_TRUE(bool(Invocation)) << llvm::toString(Invocation.takeError());
  const auto User = get(Invocation->Argument1 + windows::IRPUserBufferOffset);
  ASSERT_NE(User, 0u);
  ok(Model->setUserRequestContext(true, 0x3456));
  const auto Cookie = reg();
  EXPECT_EQ(call("ZwQueryValueKey", {H, S + 0x200, 2, User, 16, S + 0x520}),
            0u);
  auto Prepared = Model->takeGuestCall();
  ASSERT_TRUE(Prepared);
  ok(Model->beginGuestCall(Prepared->Token));
  constexpr uint64_t Child = S + 0x8000;
  ok(Model->inheritExecutionContext(Child, profile::StackBase));
  Model->enterExecution(Child, profile::StackBase, Prepared->Token);
  // This is DriverSession's actual return order: the physical callback stack
  // expires before the model resumes the original registry operation.
  ok(Model->retireStack(Child, 4096));
  auto Done = Model->finishGuestCall(Prepared->Token, 0);
  ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
  EXPECT_FALSE(*Done);
  EXPECT_EQ(get(User + 12, 4), 1u);
  Model->enterExecution(profile::StackBase);
  auto Post = next(23);
  EXPECT_TRUE(finish(Post));
  EXPECT_EQ(call("CmUnRegisterCallback", {Cookie}), 0u);
}

TEST_F(KernelRegistryCallback, LiveContextAndUnloadNeedExplicitRetirement) {
  const auto Context = call("ExAllocatePoolWithTag", {0, 32, 0x1234});
  const auto Cookie = reg(Context);
  error("ExFreePoolWithTag", {Context, 0x1234},
        "registered routine or context");
  put(Model->driverObject() + windows::DriverUnloadOffset, PC);
  ok(Model->finishEntry());
  auto U = Model->beginUnload();
  ASSERT_TRUE(bool(U));
  auto E = Model->finishUnload();
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("registry callbacks"),
            std::string::npos);
  EXPECT_EQ(call("CmUnRegisterCallback", {Cookie}), 0u);
  EXPECT_EQ(call("ExFreePoolWithTag", {Context, 0x1234}), 0u);
  ok(Model->finishUnload());
}
TEST(KernelRegistryCallbackDriver,
     OriginalLifecycleDeliversPrePostOnTheCallerThread) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Contract = Contract;
      O.Unload = true;
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
      O.Registry =
          std::vector<DriverRegistryKey>{{"\\Registry\\Machine\\System\\Current"
                                          "ControlSet\\Services\\NeverDDriver",
                                          {{"Mode", 4, {0, 0, 0, 0}}}}};
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_registry_callbacks.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
    }
}
TEST(KernelRegistryCallbackDriver,
     CompiledCallbackStatusReachesTheOriginalCaller) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Contract = Contract;
      O.Unload = true;
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
      O.Registry =
          std::vector<DriverRegistryKey>{{"\\Registry\\Machine\\System\\Current"
                                          "ControlSet\\Services\\NeverDDriver",
                                          {{"Mode", 4, {0, 0, 0, 0}}}}};
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_registry_callback_status.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
      unsigned Sets = 0;
      for (const auto &Call : R->Calls)
        if (Call.Name == "ZwSetValueKey") {
          EXPECT_EQ(Call.Result, 0xc0000022u);
          ++Sets;
        }
      EXPECT_EQ(Sets, 2u);
      ASSERT_TRUE(R->Registry);
      bool Found = false;
      for (const auto &Key : *R->Registry)
        for (const auto &V : Key.Values)
          if (V.Name == "Mode") {
            EXPECT_EQ(V.Data, (std::vector<uint8_t>{5, 0, 0, 0}));
            Found = true;
          }
      EXPECT_TRUE(Found);
    }
}
} // namespace
} // namespace neverd::emulation
