//===- AndroidSnapshotTests.cpp - Explicit final mapping observations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/Limits.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Initial = 0x22000000, Dynamic = 0x24000000;

class AndroidSnapshots : public testing::TestWithParam<
                             std::tuple<const char *, ExecutionBackendKind>> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(std::get<0>(GetParam())) + ".so");
    Options.Backend = std::get<1>(GetParam());
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->EntrySymbol = "snapshot_lifetime";
    Options.Android->Memory.push_back(
        {Initial, 4096, std::vector<uint8_t>(64, 0xa5), false});
    Options.Android->ReadMemory = {{Initial, 64}, {Dynamic, 64, false}};
    Options.InstructionQuantum = 7;
#endif
  }
  llvm::Expected<ProcessResult> run(unsigned Mode = 0) {
    Options.Android->Arguments = {Dynamic, Mode};
    return emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  }
  static void snapshots(const ProcessResult &R) {
    ASSERT_EQ(R.MemorySnapshots.size(), 2u);
    EXPECT_EQ(R.MemorySnapshots[0].Address, Initial);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(64, 0xa5));
    EXPECT_EQ(R.MemorySnapshots[1].Address, Dynamic);
    std::vector<uint8_t> Expected(64);
    for (unsigned I = 0; I != 32; ++I)
      Expected[13 + I] = I * 7 + 3;
    EXPECT_EQ(R.MemorySnapshots[1].Bytes, Expected);
  }
};

TEST_P(AndroidSnapshots, InitialMappingRequirementRemainsTheDefault) {
  Options.Android->ReadMemory.back() = {Dynamic, 64};
  for (bool Observe : {false, true}) {
    Options.Android->ObserveMemoryAtStop = Observe;
    auto R = run();
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("not readable at entry"),
              std::string::npos);
  }
}

TEST_P(AndroidSnapshots,
       AtStopRAMKeepsWorkloadOutcomeAndContinuesAfterMissingRanges) {
  const std::vector<NativeMemoryRead> Reads{
      {Initial, 64}, {Dynamic, 64, false}, {Initial, 64}};
  for (unsigned Mode = 0; Mode != 6; ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Limits.Instructions = 1000;
    Options.Android->ReadMemory.clear();
    Options.Android->ObserveMemoryAtStop = false;
    const auto Before = llvm::cantFail(run(Mode));
    Options.Android->ReadMemory = Reads;
    Options.Android->ObserveMemoryAtStop = true;
    auto After = llvm::cantFail(run(Mode));
    ASSERT_EQ(After.MemorySnapshots.size(), Reads.size());
    EXPECT_EQ(After.MemorySnapshots.front().Bytes,
              std::vector<uint8_t>(64, 0xa5));
    EXPECT_EQ(After.MemorySnapshots.back().Bytes,
              std::vector<uint8_t>(64, 0xa5));
    const auto &DynamicSnapshot = After.MemorySnapshots[1];
    if (Mode == 1) {
      EXPECT_TRUE(DynamicSnapshot.Bytes.empty());
      EXPECT_FALSE(DynamicSnapshot.Diagnostic.empty());
      EXPECT_LE(
          DynamicSnapshot.Diagnostic.size(),
          ::neverd::limits::emulation::kAndroidFailureMemoryDiagnosticBytes);
    } else {
      std::vector<uint8_t> Expected(64);
      for (unsigned I = 0; I != 32; ++I)
        Expected[13 + I] = I * 7 + 3;
      EXPECT_EQ(DynamicSnapshot.Bytes, Expected);
      EXPECT_TRUE(DynamicSnapshot.Diagnostic.empty());
    }
    After.MemorySnapshots.clear();
    EXPECT_EQ(processResultJSON(After), processResultJSON(Before));
  }
}

TEST_P(AndroidSnapshots, AtStopRAMReservesDiagnosticBudgetOncePerRange) {
  Options.Android->ReadMemory = {{Dynamic, 64, false}};
  Options.Android->ObserveMemoryAtStop = true;
  Options.Android->ObserveMemoryOnFailure = true;
  Options.OutputLimit =
      64 + ::neverd::limits::emulation::kAndroidFailureMemoryDiagnosticBytes -
      1;
  auto Rejected = run(1);
  ASSERT_FALSE(bool(Rejected));
  llvm::consumeError(Rejected.takeError());
  ++Options.OutputLimit;
  const auto Result = llvm::cantFail(run(1));
  EXPECT_EQ(Result.Stop, ProcessStopReason::Returned);
  ASSERT_EQ(Result.MemorySnapshots.size(), 1u);
  EXPECT_TRUE(Result.MemorySnapshots[0].Bytes.empty());
  EXPECT_FALSE(Result.MemorySnapshots[0].Diagnostic.empty());
}

TEST_P(AndroidSnapshots, DeferredReadObservesGuestCreatedBytes) {
  auto R = run();
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, ProcessStopReason::Returned) << R->Diagnostic;
  EXPECT_EQ(R->ReturnValue, Dynamic);
  snapshots(*R);
}

TEST_P(AndroidSnapshots, UnmappingAndPermissionsStillFailAtTheFinalStop) {
  for (unsigned Mode : {1, 2}) {
    auto R = run(Mode);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::RuntimeFailure);
    EXPECT_EQ(R->Diagnostic,
              "Android native: requested memory became unreadable");
    ASSERT_EQ(R->MemorySnapshots.size(), 1u);
    EXPECT_EQ(R->MemorySnapshots[0].Bytes, std::vector<uint8_t>(64, 0xa5));
  }
}

TEST_P(AndroidSnapshots, UnsupportedServicesKeepObservableFinalMemory) {
  auto R = run(3);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
  ASSERT_FALSE(R->NativeCalls.empty());
  EXPECT_EQ(R->NativeCalls.back().Name, "unsupported_snapshot_service");
  snapshots(*R);
}

TEST_P(AndroidSnapshots, TerminalFaultsDoNotAttemptFinalReads) {
  auto R = run(4);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, ProcessStopReason::CPUFailure);
  EXPECT_TRUE(R->MemorySnapshots.empty());
}

TEST_P(AndroidSnapshots, InstructionLimitsRetainTheFinalMappingObservation) {
  Options.Limits.Instructions = 1000;
  auto R = run(5);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, ProcessStopReason::InstructionLimit);
  EXPECT_EQ(R->Instructions, 1000u);
  snapshots(*R);
}

TEST_P(AndroidSnapshots, DeferredReadsRetainSizeAndOutputBudgetChecks) {
  for (NativeMemoryRead Invalid :
       {NativeMemoryRead{Dynamic, 0, false},
        NativeMemoryRead{UINT64_MAX - 31, 64, false},
        NativeMemoryRead{Dynamic, Options.OutputLimit, false}}) {
    Options.Android->ReadMemory.back() = Invalid;
    auto R = run();
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("snapshot"),
              std::string::npos);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Compiled, AndroidSnapshots,
    testing::Combine(
        testing::Values("snapshots-O0-none", "snapshots-O0-android",
                        "snapshots-O0-relr", "snapshots-O2-none",
                        "snapshots-O2-android", "snapshots-O2-relr"),
        testing::Values(ExecutionBackendKind::Unicorn,
                        ExecutionBackendKind::HVF, ExecutionBackendKind::KVM,
                        ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
