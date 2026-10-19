//===- KernelCallbackTests.cpp - Executive callback contracts ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Executive callback contracts.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
class KernelCallback : public ::testing::Test {
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
    auto M = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(M));
    Memory = std::move(*M);
    ok(Memory->map(S, 0x10000, Read | Write));
    ok(Memory->map(PC, 0x1000, Read | Execute));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = PC;
    I.Size = 0x4000;
    ok(Model->initialize(I, DriverOptions{}));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  void put(uint64_t A, uint64_t V, unsigned W = 8) {
    ok(Memory->writeInteger(A, V, W));
  }
  uint64_t get(uint64_t A, unsigned W = 8) {
    auto V = Memory->readInteger(A, W);
    if (!V) {
      ADD_FAILURE() << llvm::toString(V.takeError());
      return 0;
    }
    return *V;
  }
  uint64_t call(llvm::StringRef Name, std::initializer_list<uint64_t> A) {
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
  void name(llvm::StringRef Text, uint32_t Flags = 0x40) {
    for (size_t I = 0; I <= Text.size(); ++I)
      put(S + 0x200 + I * 2, I == Text.size() ? 0 : uint8_t(Text[I]), 2);
    put(S + 0x100, Text.size() * 2, 2);
    put(S + 0x102, (Text.size() + 1) * 2, 2);
    put(S + 0x108, S + 0x200);
    put(S + 0x300, 48, 4);
    put(S + 0x308, 0);
    put(S + 0x310, S + 0x100);
    put(S + 0x318, Flags, 4);
    put(S + 0x320, 0);
    put(S + 0x328, 0);
  }
  uint64_t create(llvm::StringRef Text = "\\Callback\\NeverD",
                  bool Multiple = true, uint32_t Flags = 0x40) {
    name(Text, Flags);
    EXPECT_EQ(call("ExCreateCallback", {S, S + 0x300, 1, Multiple}), 0u);
    return get(S);
  }
  uint64_t reg(uint64_t O, uint64_t Code = PC, uint64_t Context = 0) {
    return call("ExRegisterCallback", {O, Code, Context});
  }
  KernelGuestCall next() {
    auto C = Model->takeGuestCall();
    if (!C) {
      ADD_FAILURE() << "missing callback";
      return {};
    }
    ok(Model->beginGuestCall(C->Token));
    Model->enterExecution(profile::StackBase, profile::StackBase, C->Token);
    return *C;
  }
  bool finish(const KernelGuestCall &C, uint64_t R = 0) {
    auto Done = Model->finishGuestCall(C.Token, R);
    if (!Done) {
      ADD_FAILURE() << llvm::toString(Done.takeError());
      return false;
    }
    if (*Done)
      EXPECT_EQ(**Done, 0u);
    return bool(*Done);
  }
  void drop(uint64_t O) { EXPECT_EQ(call("ObfDereferenceObject", {O}), 0u); }
  void unreg(uint64_t R) { EXPECT_EQ(call("ExUnregisterCallback", {R}), 0u); }
};

TEST_F(KernelCallback, SystemObjectsExistAndRegistrationDoesNotNotify) {
  name("\\Callback\\PowerState");
  ASSERT_EQ(call("ExCreateCallback", {S, S + 0x300, 0, 0}), 0u);
  auto O = get(S);
  EXPECT_EQ(create("\\callback\\powerstate", false), O);
  auto A = reg(O), B = reg(O, PC + 1);
  ASSERT_NE(A, 0u);
  ASSERT_NE(B, 0u);
  EXPECT_FALSE(Model->takeGuestCall());
  error("ExNotifyCallback", {O, 1, 2}, "system-defined");
  error("ObMakeTemporaryObject", {O}, "provider-owned");
  unreg(A);
  unreg(B);
  drop(O);
  drop(O);
  EXPECT_NE(create("\\Callback\\PowerState"), 0u);
  drop(get(S));
}

TEST_F(KernelCallback, MissingAndMalformedNamesPreserveOutput) {
  name("\\Callback\\Missing");
  put(S, 0xaabbccdd);
  EXPECT_EQ(call("ExCreateCallback", {S, S + 0x300, 0, 1}), 0xc0000034u);
  EXPECT_EQ(get(S), 0xaabbccddu);
  put(S + 0x310, 0);
  EXPECT_EQ(call("ExCreateCallback", {S, S + 0x300, 0, 1}), 0xc0000001u);
  EXPECT_EQ(get(S), 0xaabbccddu);
  name("\\Callback\\Missing");
  put(S + 0x300, 47, 4);
  EXPECT_EQ(call("ExCreateCallback", {S, S + 0x300, 1, 1}), 0xc000000du);
  name("\\Callback\\Missing");
  put(S + 0x320, S + 0x500);
  error("ExCreateCallback", {S, S + 0x300, 1, 1}, "security");
}

TEST_F(KernelCallback, InvalidOutputDoesNotPublishNamedObject) {
  name("\\Callback\\NeverD");
  error("ExCreateCallback", {0, S + 0x300, 1, 1}, "buffer");
  EXPECT_EQ(call("ExCreateCallback", {S, S + 0x300, 0, 1}), 0xc0000034u);
}

TEST_F(KernelCallback,
       SingleRegistrationPolicyAndIndependentPointerReferences) {
  const auto O = create("\\Callback\\Single", false, 0x50);
  EXPECT_EQ(create("\\Callback\\Single", true), O);
  auto R = reg(O);
  ASSERT_NE(R, 0u);
  EXPECT_EQ(reg(O, PC + 1), 0u);
  drop(O);
  drop(O);
  name("\\Callback\\Single");
  ASSERT_EQ(call("ExCreateCallback", {S, S + 0x300, 0, 0}), 0u);
  EXPECT_EQ(get(S), O);
  unreg(R);
  const auto New = reg(O, PC + 2);
  EXPECT_NE(New, 0u);
  unreg(New);
  EXPECT_EQ(call("ObMakeTemporaryObject", {O}), 0u);
  drop(O);
  auto E = Model->validateGuestAccess(O, 1, false);
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("freed"), std::string::npos);
  name("\\Callback\\Single");
  EXPECT_EQ(call("ExCreateCallback", {S, S + 0x300, 0, 1}), 0xc0000034u);
  EXPECT_NE(create("\\Callback\\Single"), O);
  drop(get(S));
}

TEST_F(KernelCallback,
       TemporaryNameIsGoneWhileRegistrationStillKeepsItsObject) {
  auto Old = create();
  auto R = reg(Old);
  drop(Old);
  name("\\Callback\\NeverD");
  put(S, 0x77777777);
  EXPECT_EQ(call("ExCreateCallback", {S, S + 0x300, 0, 1}), 0xc0000034u);
  EXPECT_EQ(get(S), 0x77777777u);
  auto New = create("\\Callback\\NeverD", true, 0x50);
  EXPECT_NE(New, Old);
  unreg(R); // Destroying the old unlinked object must preserve the new binding.
  ASSERT_EQ(call("ExCreateCallback", {S, S + 0x300, 0, 1}), 0u);
  EXPECT_EQ(get(S), New);
  drop(New);
  EXPECT_EQ(call("ObMakeTemporaryObject", {New}), 0u);
  drop(New);
}

TEST_F(KernelCallback, PermanentObjectRequiresExplicitTemporaryConversion) {
  auto O = create("\\Callback\\Permanent", true, 0x50);
  drop(O);
  name("\\Callback\\Permanent");
  ASSERT_EQ(call("ExCreateCallback", {S, S + 0x300, 0, 0}), 0u);
  EXPECT_EQ(get(S), O);
  EXPECT_EQ(call("ObMakeTemporaryObject", {O}), 0u);
  EXPECT_EQ(call("ExCreateCallback", {S, S + 0x300, 0, 1}), 0xc0000034u);
  drop(O);
  EXPECT_EQ(call("ExCreateCallback", {S, S + 0x300, 0, 1}), 0xc0000034u);
}

TEST_F(KernelCallback, NotifyCallsEveryRegistrationInOrderAtCallerIRQL) {
  auto O = create();
  auto A = reg(O, PC, 0x111), B = reg(O, PC + 1, 0x222);
  EXPECT_EQ(call("KfRaiseIrql", {2}), 0u);
  EXPECT_EQ(call("ExNotifyCallback", {O, 0x333, 0x444}), 0u);
  auto First = next();
  EXPECT_EQ(First.PC, PC);
  EXPECT_EQ(First.Arguments, (std::vector<uint64_t>{0x111, 0x333, 0x444}));
  EXPECT_EQ(call("KeGetCurrentIrql", {}), 2u);
  EXPECT_FALSE(finish(First, 0xffffffff));
  auto Second = next();
  EXPECT_EQ(Second.PC, PC + 1);
  EXPECT_EQ(Second.Arguments.front(), 0x222u);
  EXPECT_TRUE(finish(Second, 12345));
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_EQ(call("KeLowerIrql", {0}), 0u);
  unreg(A);
  unreg(B);
  drop(O);
}

TEST_F(KernelCallback,
       NotifyTraversesLiveRegistrationListAndSkipsRemovedEntries) {
  auto O = create();
  auto A = reg(O), B = reg(O, PC + 1);
  EXPECT_EQ(call("ExNotifyCallback", {O, 1, 2}), 0u);
  auto First = next();
  unreg(B);
  auto C = reg(O, PC + 2);
  EXPECT_FALSE(finish(First));
  auto Last = next();
  EXPECT_EQ(Last.PC, PC + 2);
  EXPECT_TRUE(finish(Last));
  unreg(A);
  unreg(C);
  drop(O);
}

TEST_F(KernelCallback, NestedNotifyHasIndependentCursorAndBusyState) {
  auto O = create();
  auto A = reg(O), B = reg(O, PC + 1);
  EXPECT_EQ(call("ExNotifyCallback", {O, 1, 2}), 0u);
  auto Outer = next();
  EXPECT_EQ(call("ExNotifyCallback", {O, 3, 4}), 0u);
  auto InnerA = next();
  EXPECT_NE(InnerA.Token.ID, Outer.Token.ID);
  EXPECT_FALSE(finish(InnerA));
  auto InnerB = next();
  EXPECT_EQ(InnerB.PC, PC + 1);
  EXPECT_TRUE(finish(InnerB));
  Model->enterExecution(profile::StackBase, profile::StackBase, Outer.Token);
  EXPECT_FALSE(finish(Outer));
  auto OuterB = next();
  EXPECT_EQ(OuterB.Arguments.at(1), 1u);
  EXPECT_TRUE(finish(OuterB));
  unreg(A);
  unreg(B);
  drop(O);
}

TEST_F(KernelCallback, SelfUnregisterFailsClearlyWithoutDroppingRegistration) {
  auto O = create();
  auto R = reg(O);
  EXPECT_EQ(call("ExNotifyCallback", {O, 0, 0}), 0u);
  auto C = next();
  error("ExUnregisterCallback", {R}, "own callback");
  EXPECT_TRUE(finish(C));
  unreg(R);
  drop(O);
}

TEST_F(KernelCallback, ForeignUnregisterWaitsUntilActiveCallbackReturns) {
  auto O = create();
  auto R = reg(O);
  EXPECT_EQ(call("ExNotifyCallback", {O, 0, 0}), 0u);
  auto C = next();
  const auto Saved = Model->captureExecutionContext();
  Model->enterExecution(S + 0x8000, S + 0x8000);
  EXPECT_EQ(call("ExUnregisterCallback", {R}), 0u);
  auto W = Model->takeWait();
  ASSERT_TRUE(W);
  auto Ready = Model->pollWait(*W);
  ASSERT_TRUE(bool(Ready));
  EXPECT_FALSE(*Ready);
  ok(Model->restoreExecutionContext(Saved));
  EXPECT_TRUE(finish(C));
  Model->enterExecution(S + 0x8000, S + 0x8000);
  Ready = Model->pollWait(*W);
  ASSERT_TRUE(bool(Ready));
  ASSERT_TRUE(*Ready);
  EXPECT_EQ(**Ready, 0u);
  ok(Model->restoreExecutionContext(Saved));
  drop(O);
}

TEST_F(KernelCallback, OpaqueTokensAndRegisteredContextStorageAreProtected) {
  auto O = create();
  auto Context = call("ExAllocatePoolWithTag", {0, 16, 0x54455354});
  ASSERT_NE(Context, 0u);
  auto R = reg(O, PC, Context);
  for (auto Address : {O, R}) {
    auto E = Model->validateGuestAccess(Address, 1, false);
    ASSERT_TRUE(bool(E));
    EXPECT_NE(llvm::toString(std::move(E)).find("opaque"), std::string::npos);
  }
  error("ExFreePoolWithTag", {Context, 0x54455354}, "registered callback");
  unreg(R);
  EXPECT_EQ(call("ExFreePoolWithTag", {Context, 0x54455354}), 0u);
  drop(O);
}

TEST_F(KernelCallback, BooleanWidthCaseSensitiveLookupAndIrqlAdmission) {
  name("\\Callback\\Case");
  ASSERT_EQ(call("ExCreateCallback", {S, S + 0x300, 0x100, 1}), 0xc0000034u);
  auto O = create("\\Callback\\Case", true, 0x50);
  name("\\Callback\\case", 0);
  EXPECT_EQ(call("ExCreateCallback", {S, S + 0x300, 0, 1}), 0xc0000034u);
  EXPECT_EQ(call("KfRaiseIrql", {2}), 0u);
  error("ExCreateCallback", {S, S + 0x300, 1, 1}, "IRQL");
  error("ExRegisterCallback", {O, PC, 0}, "IRQL");
  EXPECT_EQ(call("ExNotifyCallback", {0, 0, 0}), 0u);
  EXPECT_EQ(call("KeLowerIrql", {0}), 0u);
  EXPECT_EQ(call("ObMakeTemporaryObject", {O}), 0u);
  drop(O);
}

TEST_F(KernelCallback, UnloadRejectsLeakedRegistrationsAndPointerReferences) {
  auto O = create();
  auto R = reg(O);
  put(Model->driverObject() + windows::DriverUnloadOffset, PC);
  ok(Model->finishEntry());
  auto U = Model->beginUnload();
  ASSERT_TRUE(bool(U));
  auto E = Model->finishUnload();
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("callbacks"), std::string::npos);
  unreg(R);
  E = Model->finishUnload();
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("references"), std::string::npos);
  drop(O);
  ok(Model->finishUnload());
  EXPECT_TRUE(Result.UnloadCompleted);
}

TEST_F(KernelCallback, CallbackDirectoryCannotBeReboundAsAVolumeAlias) {
  DriverFiniteFileSystemVolume V{
      "C", "\\Device\\C", {}, true, {{"\\Callback", "\\"}}};
  auto E = validateDriverFiniteFileSystem(
      std::vector<DriverFiniteFileSystemVolume>{V});
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  name("\\Callback");
  // No target name is interpreted before the reserved root binding is rejected.
  error("IoCreateSymbolicLink", {S + 0x100, S + 0x100}, "symbolic-link model");
  const auto O = create("\\Callback\\PowerState");
  drop(O);
}

TEST_F(KernelCallback,
       RegistrationCapacityFailureAndReusePreserveObjectLifetime) {
  auto O = create();
  std::vector<uint64_t> Registrations;
  for (unsigned I = 0; I < 256; ++I) {
    auto R = reg(O);
    ASSERT_NE(R, 0u);
    Registrations.push_back(R);
  }
  EXPECT_EQ(reg(O), 0u);
  unreg(Registrations.back());
  Registrations.pop_back();
  auto R = reg(O);
  EXPECT_NE(R, 0u);
  Registrations.push_back(R);
  for (auto Registration : Registrations)
    unreg(Registration);
  drop(O);
}

TEST(KernelCallbackDriver, OriginalCallbacksExecuteAndUnloadAcrossContracts) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    SCOPED_TRACE(executionContractName(Contract));
    DriverOptions O;
    O.Contract = Contract;
    O.Unload = true;
    auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_callbacks.sys",
                           O);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
    EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
    EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
    EXPECT_TRUE(
        std::any_of(R->Calls.begin(), R->Calls.end(), [](const auto &C) {
          return C.Phase.starts_with("callback:executive");
        }));
  }
}
} // namespace
} // namespace neverd::emulation
