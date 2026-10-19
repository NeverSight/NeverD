//===- KernelDmaGuardTests.cpp - Explicit class-202 policy observations
//-----===//
///
/// \file
/// Explicit class-202 policy observations.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {
TEST(DriverDmaGuardScenario, FalseTrueAndOmittedStatesRemainDistinct) {
  for (bool Enabled : {false, true}) {
    auto O = driverOptionsFromScenarioJSON(
        Enabled ? R"({"dma_guard_policy_enabled":true})"
                : R"({"dma_guard_policy_enabled":false})");
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    EXPECT_EQ(O->DmaGuardPolicyEnabled, Enabled);
    auto P = driverOptionsFromScenarioJSON("{}", *O);
    ASSERT_TRUE(bool(P));
    EXPECT_EQ(P->DmaGuardPolicyEnabled, Enabled);
    DriverResult R;
    R.Configuration = *P;
    auto JSON = llvm::json::parse(driverResultJSON(R));
    ASSERT_TRUE(bool(JSON));
    EXPECT_EQ(JSON->getAsObject()
                  ->getObject("configuration")
                  ->getBoolean("dma_guard_policy_enabled"),
              Enabled);
  }
  auto O = driverOptionsFromScenarioJSON("{}");
  ASSERT_TRUE(bool(O));
  EXPECT_FALSE(O->DmaGuardPolicyEnabled.has_value());
  DriverResult R;
  R.Configuration = *O;
  auto JSON = llvm::json::parse(driverResultJSON(R));
  ASSERT_TRUE(bool(JSON));
  EXPECT_EQ(JSON->getAsObject()
                ->getObject("configuration")
                ->get("dma_guard_policy_enabled"),
            nullptr);
  for (const char *Bad : {"null", "0", "1", "1.5", "\"false\"", "{}", "[]"}) {
    auto P = driverOptionsFromScenarioJSON(
        std::string("{\"dma_guard_policy_enabled\":") + Bad + "}");
    ASSERT_FALSE(bool(P)) << Bad;
    EXPECT_NE(llvm::toString(P.takeError()).find("dma_guard_policy_enabled"),
              std::string::npos);
  }
}
class KernelDmaGuard : public testing::Test {
protected:
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  const uint64_t Buffer = 0x60000000, LengthOut = 0x60001000;
  void ok(llvm::Error E) {
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  }
  void initialize(std::optional<bool> Enabled, unsigned Version = 19041) {
    auto B = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(B));
    Memory = std::move(*B);
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverOptions O;
    if (Version)
      O.Version = DriverOptions::WindowsVersion{10, 0, Version};
    O.DmaGuardPolicyEnabled = Enabled;
    O.SecurePciEnabled = !Enabled.value_or(false);
    O.CPUID.push_back({1, 0, {0, 0, uint32_t(1) << 31, 0}});
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 4096;
    I.Size = 0x3000;
    ok(Model->initialize(I, O));
    O.DmaGuardPolicyEnabled = !Enabled.value_or(false);
    ok(Memory->map(Buffer, 0x2000, Read | Write));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  void reset() {
    ok(Memory->writeInteger(Buffer, 0xa5a5a5a5a5a5a5a5ULL, 8));
    ok(Memory->writeInteger(LengthOut, 0xfeedbeef, 4));
  }
  uint64_t get(uint64_t At, unsigned Width = 4) {
    auto R = Memory->readInteger(At, Width);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
};
TEST_F(KernelDmaGuard, ExactOneByteOutputIsStableCopiedAndIndependent) {
  for (bool Enabled : {false, true}) {
    initialize(Enabled);
    for (const char *Name :
         {"ZwQuerySystemInformation", "NtQuerySystemInformation"})
      for (unsigned I = 0; I != 3; ++I) {
        reset();
        auto R = Model->call(Name, {202, Buffer, 1, LengthOut});
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(*R, 0u);
        EXPECT_EQ(get(Buffer, 8), 0xa5a5a5a5a5a5a500ULL | uint64_t(Enabled));
        EXPECT_EQ(get(LengthOut), 1u);
      }
  }
}
TEST_F(KernelDmaGuard, MismatchRequiresExactlyOneByteAndDoesNotNeedPolicy) {
  initialize(std::nullopt);
  for (uint64_t Length : {0ULL, 2ULL, 4ULL, 4096ULL, 0xffffffffULL}) {
    reset();
    for (uint64_t Out : {Buffer, uint64_t(0), UINT64_MAX}) {
      auto R = Model->call("ZwQuerySystemInformation",
                           {202, Out, Length, LengthOut});
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(*R, 0xc0000004u);
      EXPECT_EQ(get(LengthOut), 1u);
      EXPECT_EQ(get(Buffer, 8), 0xa5a5a5a5a5a5a5a5ULL);
    }
  }
  auto R = Model->call("ZwQuerySystemInformation", {202, 0, 0, 0});
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(*R, 0xc0000004u);
}
TEST_F(KernelDmaGuard, MissingPolicyAndUnsupportedVersionDoNotTouchOutputs) {
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    initialize(Mode == 0 ? std::nullopt : std::optional<bool>(false),
               Mode == 1   ? 0
               : Mode == 2 ? 22621
                           : 19041);
    reset();
    auto R =
        Model->call("ZwQuerySystemInformation", {202, Buffer, 1, LengthOut});
    ASSERT_FALSE(bool(R));
    EXPECT_NE(
        llvm::toString(R.takeError())
            .find(Mode == 0 ? "requires dma_guard_policy_enabled" : "19041"),
        std::string::npos);
    EXPECT_EQ(get(Buffer), 0xa5a5a5a5u);
    EXPECT_EQ(get(LengthOut), 0xfeedbeefu);
  }
}
TEST_F(KernelDmaGuard, BadOutputOrLengthStorageNeverProducesPartialWrites) {
  initialize(true);
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    reset();
    if (Mode < 2)
      ok(Memory->protect(Mode == 0 ? Buffer : LengthOut, 4096, Read));
    auto R = Model->call("ZwQuerySystemInformation",
                         {202, Mode == 2 ? UINT64_MAX : Buffer, 1, LengthOut});
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(get(Buffer), 0xa5a5a5a5u);
    EXPECT_EQ(get(LengthOut), 0xfeedbeefu);
    ok(Memory->protect(Buffer, 0x2000, Read | Write));
  }
  reset();
  ok(Memory->protect(LengthOut, 4096, Read));
  auto R = Model->call("ZwQuerySystemInformation", {202, 0, 2, LengthOut});
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_EQ(get(Buffer), 0xa5a5a5a5u);
}
TEST_F(KernelDmaGuard, NullAndOverlappingReturnLengthRetainNativeStoreOrder) {
  initialize(false);
  reset();
  auto R = Model->call("ZwQuerySystemInformation", {202, Buffer, 1, Buffer});
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(*R, 0u);
  EXPECT_EQ(get(Buffer), 1u); // ReturnLength overwrites the earlier false byte.
  EXPECT_EQ(get(Buffer + 4), 0xa5a5a5a5u);
  reset();
  R = Model->call("ZwQuerySystemInformation", {202, Buffer, 0x100000001ULL, 0});
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(*R, 0u); // ULONG input consumes only the low 32 bits.
  EXPECT_EQ(get(Buffer), 0xa5a5a500u);
  EXPECT_EQ(get(LengthOut), 0xfeedbeefu);
}
TEST_F(KernelDmaGuard, HigherIRQLCannotQueryOrWriteOutputs) {
  initialize(false);
  reset();
  auto R = Model->call("KfRaiseIrql", {2});
  ASSERT_TRUE(bool(R));
  R = Model->call("ZwQuerySystemInformation", {202, Buffer, 1, LengthOut});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("PASSIVE_LEVEL"),
            std::string::npos);
  EXPECT_EQ(get(Buffer), 0xa5a5a5a5u);
  EXPECT_EQ(get(LengthOut), 0xfeedbeefu);
}
TEST_F(KernelDmaGuard, NtUserModeFailsExplicitlyWhileZwKeepsCallerContext) {
  initialize(false);
  reset();
  ok(Model->setUserRequestContext(true, 0x3456));
  auto R = Model->call("NtQuerySystemInformation", {202, Buffer, 1, LengthOut});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("UserMode"), std::string::npos);
  EXPECT_EQ(get(Buffer), 0xa5a5a5a5u);
  EXPECT_EQ(get(LengthOut), 0xfeedbeefu);
  R = Model->call("ZwQuerySystemInformation", {202, Buffer, 1, LengthOut});
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(*R, 0u);
  EXPECT_EQ(get(Buffer), 0xa5a5a500u);
  EXPECT_EQ(get(LengthOut), 1u);
  R = Model->call("ExGetPreviousMode", {});
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(*R, 1u);
  R = Model->call("PsGetCurrentProcessId", {});
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(*R, 0x3456u);
}
TEST(KernelDmaGuardCaller, RealQueriesPreserveTailAndConsumeBothPolicyStates) {
  for (bool Enabled : {false, true})
    for (auto Contract :
         {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
      for (bool Preemptive : {false, true}) {
        DriverOptions O;
        O.Contract = Contract;
        O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
        O.DmaGuardPolicyEnabled = Enabled;
        O.Unload = true;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                   "driver_dma_guard.sys",
                               O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
        EXPECT_EQ(R->NTStatus, 0u);
        EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
        ASSERT_EQ(R->Messages.size(), 1u);
        EXPECT_EQ(R->Messages[0],
                  std::string("DMA guard enabled=") + (Enabled ? "1" : "0"));
      }
}
} // namespace
} // namespace neverd::emulation
