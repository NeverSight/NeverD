//===- KernelClockTests.cpp - Explicit guest clock facts ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit guest clock facts.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelScheduler.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/Support/JSON.h"

#include <tuple>

namespace neverd::emulation {
namespace {
TEST(KernelClock, UnspecifiedAndInvalidBootFactsNeverBecomeDefaults) {
  KernelScheduler Empty;
  auto R = Empty.queryTimeIncrement();
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError())
                .find("requires system_clock_increment_100ns"),
            std::string::npos);
  KernelScheduler::Limits L;
  L.SystemClockIncrement100ns = 0;
  KernelScheduler Invalid(L);
  R = Invalid.queryTimeIncrement();
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("positive uint32"),
            std::string::npos);
}

TEST(KernelClock, BootIncrementIsCopiedAndIndependentOfElapsedTime) {
  for (uint32_t Increment : {1u, 156250u, UINT32_MAX}) {
    KernelScheduler::Limits L;
    L.InitialTime100ns = 19;
    L.SystemClockIncrement100ns = Increment;
    KernelScheduler S(L);
    L.SystemClockIncrement100ns = 7; // Caller changes do not alter this boot.
    const uint64_t Later = uint64_t(Increment) * 3 + 100;
    auto E = S.advanceTo100ns(Later);
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
    for (unsigned I = 0; I != 8; ++I) {
      auto R = S.queryTimeIncrement();
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(*R, uint64_t(Increment));
      EXPECT_EQ(S.now100ns(), Later);
    }
  }
}

TEST(DriverClockScenario, PositiveUint32RoundTripsAndOmissionPreservesBase) {
  for (uint32_t Increment : {1u, 156250u, UINT32_MAX}) {
    auto Parsed = driverOptionsFromScenarioJSON(
        "{\"system_clock_increment_100ns\":" + std::to_string(Increment) + "}");
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    EXPECT_EQ(Parsed->SystemClockIncrement100ns, Increment);
    auto Preserved = driverOptionsFromScenarioJSON("{}", *Parsed);
    ASSERT_TRUE(bool(Preserved));
    EXPECT_EQ(Preserved->SystemClockIncrement100ns, Increment);
    DriverResult Result;
    Result.Configuration = *Preserved;
    auto Report = llvm::json::parse(driverResultJSON(Result));
    ASSERT_TRUE(bool(Report));
    EXPECT_EQ(Report->getAsObject()
                  ->getObject("configuration")
                  ->getInteger("system_clock_increment_100ns"),
              int64_t(Increment));
  }
  auto Empty = driverOptionsFromScenarioJSON("{}");
  ASSERT_TRUE(bool(Empty));
  EXPECT_FALSE(Empty->SystemClockIncrement100ns);
}

TEST(DriverClockScenario, MalformedAndZeroInputsFailBeforeExecution) {
  for (const char *Value :
       {"null", "true", "1.5", "\"156250\"", "0", "-1", "4294967296"}) {
    auto Parsed = driverOptionsFromScenarioJSON(
        std::string("{\"system_clock_increment_100ns\":") + Value + "}");
    ASSERT_FALSE(bool(Parsed)) << Value;
    EXPECT_NE(
        llvm::toString(Parsed.takeError()).find("system_clock_increment_100ns"),
        std::string::npos);
  }
  DriverOptions Base;
  Base.SystemClockIncrement100ns = 0;
  auto Parsed = driverOptionsFromScenarioJSON("{}", Base);
  ASSERT_FALSE(bool(Parsed));
  llvm::consumeError(Parsed.takeError());
}

class KernelClockModel : public testing::Test {
protected:
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  llvm::Error initialize(std::optional<uint32_t> Increment,
                         bool Shared = false) {
    auto B = UnicornBackend::create(4 * 1024 * 1024);
    if (!B)
      return B.takeError();
    Memory = std::move(*B);
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 4096;
    I.Size = 0x3000;
    DriverOptions O;
    O.SystemClockIncrement100ns = Increment;
    if (Shared)
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    auto E = Model->initialize(I, O);
    if (E)
      return E;
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
    return llvm::Error::success();
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Args) {
    auto R = Model->call(Name, Args);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
};

TEST_F(KernelClockModel, QueryHasZeroArgumentsAndIsStableAtEveryIRQL) {
  auto E = initialize(UINT32_MAX);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  for (uint8_t Level : {0, 1, 2, 15}) {
    EXPECT_EQ(call("KfRaiseIrql", {Level}), 0u);
    EXPECT_EQ(call("KeQueryTimeIncrement", {}), uint64_t(UINT32_MAX));
    EXPECT_EQ(Model->currentIRQL(), Level);
    EXPECT_EQ(call("KeLowerIrql", {0}), 0u);
  }
}

TEST_F(KernelClockModel, MissingInputFailsAtTheZeroArgumentQuery) {
  auto E = initialize(std::nullopt);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  auto R = Model->call("KeQueryTimeIncrement", {});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError())
                .find("requires system_clock_increment_100ns"),
            std::string::npos);
}

TEST_F(KernelClockModel, ProcessorStallAdvancesSharedTimeAtEveryIRQL) {
  auto E = initialize(1, true);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  EXPECT_EQ(Model->argumentCount("KeStallExecutionProcessor"), 1u);
  constexpr uint64_t Tick =
      windows::SharedClockDataBase + windows::SharedTickCountOffset;
  for (uint8_t Level : {0, 1, 2, 15}) {
    call("KfRaiseIrql", {Level});
    const auto Before = Model->now100ns();
    EXPECT_EQ(call("KeStallExecutionProcessor", {UINT64_C(0xabcdef0000000019)}),
              0u);
    EXPECT_EQ(Model->now100ns(), Before + 250);
    EXPECT_EQ(Model->currentIRQL(), Level);
    EXPECT_EQ(llvm::cantFail(Memory->readInteger(Tick, 8)), Before + 250);
    call("KeLowerIrql", {0});
  }
  const auto Before = Model->now100ns();
  EXPECT_EQ(call("KeStallExecutionProcessor", {0}), 0u);
  EXPECT_EQ(Model->now100ns(), Before);
}

TEST_F(KernelClockModel, HALImportsKeepTheirOwnProviderAndSharedClock) {
  auto E = initialize(std::nullopt);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  KernelExportRegistry::Export Export{"KeStallExecutionProcessor", "hal.dll"};
  EXPECT_EQ(Model->argumentCount(Export), 1u);
  const uint64_t Arguments[] = {7};
  auto R =
      Model->call(Export, Arguments, [](unsigned) -> llvm::Expected<uint64_t> {
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                       "unexpected argument read");
      });
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(Model->now100ns(), 70u);
  Export.Module = "ndis.sys";
  EXPECT_FALSE(Model->argumentCount(Export));
  Export.Module = "hal.dll";
  Export.Name = "ExAllocatePoolWithTag";
  EXPECT_FALSE(Model->argumentCount(Export));
}

TEST_F(KernelClockModel,
       ProcessorStallVisitsEachTimerWithoutBlockingTheThread) {
  auto E = initialize(std::nullopt);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  const auto Pool = call("ExAllocatePoolWithTag", {0, 256, 0x5354414c});
  call("KeInitializeTimer", {Pool});
  call("KeInitializeTimer", {Pool + 128});
  call("KeSetTimer", {Pool, uint64_t(-10LL), 0});
  call("KeSetTimer", {Pool + 128, uint64_t(-30LL), 0});
  call("KfRaiseIrql", {2});
  EXPECT_EQ(call("KeStallExecutionProcessor", {5}), 0u);
  EXPECT_EQ(Model->now100ns(), 50u);
  EXPECT_EQ(Model->currentIRQL(), 2u);
  EXPECT_EQ(call("KeReadStateTimer", {Pool}), 1u);
  EXPECT_EQ(call("KeReadStateTimer", {Pool + 128}), 1u);
  call("KeLowerIrql", {0});
  call("ExFreePoolWithTag", {Pool, 0x5354414c});
}

TEST_F(KernelClockModel, ProcessorStallRejectsOverflowWithoutChangingTime) {
  auto E = initialize(std::nullopt);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  E = Model->advanceExecutionTo100ns(UINT64_MAX - 5);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  auto R = Model->call("KeStallExecutionProcessor", {1});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("overflow"), std::string::npos);
  EXPECT_EQ(Model->now100ns(), UINT64_MAX - 5);
  EXPECT_EQ(call("KeStallExecutionProcessor", {UINT64_C(0x100000000)}), 0u);
  EXPECT_EQ(Model->now100ns(), UINT64_MAX - 5);
}

TEST_F(KernelClockModel, InvalidTypedInputDoesNotPublishKernelObjects) {
  auto E = initialize(0);
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("positive uint32"),
            std::string::npos);
  EXPECT_EQ(Model->driverObject(), 0u);
  auto Mapped = Memory->canAccess(profile::KernelArenaBase, 1, Read);
  ASSERT_TRUE(bool(Mapped));
  EXPECT_FALSE(*Mapped);
}

TEST_F(KernelClockModel, SharedTicksUseTheSameClockAndCoherentHighParts) {
  auto E = initialize(1, true);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  constexpr uint64_t Tick =
      windows::SharedClockDataBase + windows::SharedTickCountOffset;
  auto Before = Memory->readInteger(Tick, 8);
  ASSERT_TRUE(bool(Before));
  EXPECT_EQ(*Before, 0u);
  const uint64_t Later = (uint64_t(1) << 32) + 5;
  E = Model->advanceExecutionTo100ns(Later);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  auto Quad = Memory->readInteger(Tick, 8);
  auto High2 = Memory->readInteger(Tick + 8, 4);
  ASSERT_TRUE(bool(Quad));
  ASSERT_TRUE(bool(High2));
  EXPECT_EQ(*Quad, Later);
  EXPECT_EQ(*High2, 1u);
  EXPECT_EQ(call("KeQueryTimeIncrement", {}), 1u);
  auto Writable = Memory->canAccess(windows::SharedClockDataBase, 4096, Write);
  auto Executable =
      Memory->canAccess(windows::SharedClockDataBase, 4096, Execute);
  ASSERT_TRUE(bool(Writable));
  ASSERT_TRUE(bool(Executable));
  EXPECT_FALSE(*Writable);
  EXPECT_FALSE(*Executable);
}

TEST_F(KernelClockModel,
       OnlyTheTwelveTickBytesAreReadableAndNeverGuestWritable) {
  auto E = initialize(156250, true);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  constexpr uint64_t Tick =
      windows::SharedClockDataBase + windows::SharedTickCountOffset;
  for (unsigned Size : {1, 4, 8, 12}) {
    E = Model->validateGuestAccess(Tick, Size, false);
    EXPECT_FALSE(bool(E)) << llvm::toString(std::move(E));
  }
  for (const auto &[Address, Size, IsWrite] :
       {std::tuple{Tick, 1u, true}, std::tuple{Tick - 1, 8u, false},
        std::tuple{Tick + 8, 5u, false},
        std::tuple{windows::SharedClockDataBase + 0x14, 8u, false}}) {
    E = Model->validateGuestAccess(Address, Size, IsWrite);
    ASSERT_TRUE(bool(E));
    EXPECT_NE(llvm::toString(std::move(E)).find("shared clock data"),
              std::string::npos);
  }
  constexpr uint64_t Scratch = 0x60000000;
  E = Memory->map(Scratch, 4096, Read | Write);
  ASSERT_FALSE(bool(E));
  E = Memory->writeInteger(Scratch, 0x1122334455667788ULL, 8);
  ASSERT_FALSE(bool(E));
  auto Rejected = Model->call("RtlCopyMemory", {Scratch, Tick - 1, 8});
  ASSERT_FALSE(bool(Rejected));
  llvm::consumeError(Rejected.takeError());
  auto Unchanged = Memory->readInteger(Scratch, 8);
  ASSERT_TRUE(bool(Unchanged));
  EXPECT_EQ(*Unchanged, 0x1122334455667788ULL);
}

TEST_F(KernelClockModel, AnAPIClockFactAloneDoesNotInventAnUnspecifiedLayout) {
  auto E = initialize(156250);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  EXPECT_EQ(call("KeQueryTimeIncrement", {}), 156250u);
  auto Mapped = Memory->canAccess(windows::SharedClockDataBase, 1, Read);
  ASSERT_TRUE(bool(Mapped));
  EXPECT_FALSE(*Mapped);
}

TEST(KernelClockDriver, RealCallerObservesTheSameIncrementAfterDelayAndAtDPC) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Contract = Contract;
      O.SystemClockIncrement100ns = 156250;
      O.Unload = true;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_clock_increment.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted);
      unsigned Queries = 0;
      for (const auto &C : R->Calls)
        if (C.Name == "KeQueryTimeIncrement") {
          EXPECT_TRUE(C.Arguments.empty());
          EXPECT_EQ(C.Result, 156250u);
          ++Queries;
        }
      EXPECT_EQ(Queries, 3u);
    }
}

TEST(KernelClockDriver, CompiledSharedTickReadsAdvanceWithActualDelay) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Contract = Contract;
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
      O.SystemClockIncrement100ns = 156250;
      O.Unload = true;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_clock_shared_tick.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
    }
}
} // namespace
} // namespace neverd::emulation
