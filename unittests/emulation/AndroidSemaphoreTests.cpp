//===- AndroidSemaphoreTests.cpp - Independent semaphore callers --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks guest semaphore semantics with independent native callers.

#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
class AndroidSemaphore : public testing::TestWithParam<
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
    Options.Android->TraceLimit = 4096;
    Options.Android->Memory.push_back({Buffer, 8192, {}, false});
    Options.Android->ReadMemory.push_back({Buffer, 128});
    Options.Limits.Instructions = 1000000;
#endif
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Args) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Args);
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return {ProcessProfile::AndroidNativeAArch64,
              GuestArchitecture::AArch64,
              Options.Backend,
              {}};
    }
    return std::move(*R);
  }
  void returned(const ProcessResult &R, uint64_t Value = 0) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, Value);
  }
};

TEST_P(AndroidSemaphore, CountsErrorsAndPaddingFollowTheLP64Contract) {
  for (uint64_t Shared : {0ULL, 1ULL, 0xffffffffULL, 0x100000000ULL}) {
    auto R = run("semaphore_sequence", {Buffer, Shared});
    returned(R);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    std::vector<uint8_t> Expected(128);
    std::fill_n(Expected.begin(), 16, 0xa5);
    llvm::support::endian::write32le(Expected.data(),
                                     0x7ffffffe + (uint32_t(Shared) != 0));
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
}

TEST_P(AndroidSemaphore, MultipleWaitersRecheckTokensAfterEveryWake) {
  Options.Android->ThreadLimit = 3;
  for (uint64_t Quantum : {7, 4096, 100000}) {
    Options.InstructionQuantum = Quantum;
    for (uint64_t Mode = 0; Mode != 4; ++Mode) {
      SCOPED_TRACE(testing::Message() << Quantum << ":" << Mode);
      auto R = run("semaphore_workers", {Buffer, Mode});
      returned(R);
      ASSERT_EQ(R.NativeThreads.size(), 3u);
      for (const auto &T : R.NativeThreads) {
        EXPECT_TRUE(T.Finished);
        EXPECT_FALSE(T.Waiting);
      }
      unsigned Waits = 0;
      for (const auto &C : R.NativeCalls) {
        EXPECT_TRUE(C.Result.has_value()) << C.Name;
        if (C.Name == "sem_wait") {
          ++Waits;
          EXPECT_EQ(C.Result, 0);
        }
      }
      EXPECT_EQ(Waits, 4u);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      std::vector<uint8_t> Expected(128);
      llvm::support::endian::write32le(Expected.data(), Mode & 1);
      for (unsigned I = 0; I != 4; ++I)
        llvm::support::endian::write32le(Expected.data() + 32 + I * 4, 1);
      for (unsigned I = 0; I != 2; ++I) {
        llvm::support::endian::write32le(Expected.data() + 48 + I * 4, 80 + I);
        llvm::support::endian::write32le(Expected.data() + 56 + I * 4, 101 + I);
      }
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
  }
}

TEST_P(AndroidSemaphore, DisabledWaitAndUnsupportedTimedWaitDoNotSucceed) {
  for (unsigned Op : {0u, 6u}) {
    auto R = run("semaphore_call", {Buffer, Op, 0});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_FALSE(R.NativeCalls.back().Result);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(128));
  }
}

TEST_P(AndroidSemaphore, PendingWaitsKeepFailureAndDeadlockVisible) {
  Options.Android->ThreadLimit = 2;
  Options.InstructionQuantum = 100000;
  for (uint64_t Mode : {0, 1, 2, 3}) {
    auto R = run("semaphore_wait_failure", {Buffer, Mode});
    EXPECT_EQ(R.Stop, Mode ? ProcessStopReason::RuntimeFailure
                           : ProcessStopReason::UnsupportedService)
        << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_EQ(R.NativeThreads.size(), 2u);
    EXPECT_TRUE(R.NativeThreads[1].Waiting);
    unsigned Pending = 0;
    for (const auto &C : R.NativeCalls)
      if (C.Name == "sem_wait") {
        ++Pending;
        EXPECT_FALSE(C.Result);
      }
    EXPECT_EQ(Pending, 1u);
  }
}

TEST_P(AndroidSemaphore, InvalidAndUnalignedWordsFailWithoutForgingReturns) {
  for (uint32_t Word : {0x80000000u, 0xfffffffcu}) {
    auto &Bytes = Options.Android->Memory[0].Bytes;
    Bytes.assign(128, 0xa5);
    llvm::support::endian::write32le(Bytes.data(), Word);
    for (unsigned Op : {0, 1, 2, 3}) {
      auto R = run("semaphore_call", {Buffer, Op, Buffer + 64});
      EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
      EXPECT_FALSE(R.ReturnValue);
    }
  }
  for (uint64_t Pointer : {Buffer + 1, uint64_t(1), uint64_t(0)})
    EXPECT_EQ(run("semaphore_call", {Pointer, 1, 0}).Stop,
              ProcessStopReason::RuntimeFailure);
  returned(run("semaphore_call", {1, 5, 0}));
  returned(run("semaphore_call", {1, 4, 0x40000000}), 0xffffffff);
}

TEST_P(AndroidSemaphore, OnlyTheCountWordNeedsWritePermission) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0xa5);
  Options.Android->ReadMemory = {{Buffer + 4092, 16}};
  auto R = run("semaphore_readonly_tail", {Buffer + 4092});
  returned(R);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  std::vector<uint8_t> Expected(16, 0xa5);
  llvm::support::endian::write32le(Expected.data(), 1);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
}

TEST_P(AndroidSemaphore, DynamicCallsRetainLibraryLifetime) {
  Options.Android->Libraries["libsemaphore-model.so"] = {"sem_post"};
  returned(run("semaphore_dynamic", {Buffer, 0}));
  auto R = run("semaphore_dynamic", {Buffer, 1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(
    OptimizationPackingAndBackend, AndroidSemaphore,
    testing::Combine(
        testing::Values("semaphore-O0-none", "semaphore-O0-android",
                        "semaphore-O0-relr", "semaphore-O2-none",
                        "semaphore-O2-android", "semaphore-O2-relr"),
        testing::Values(ExecutionBackendKind::Unicorn,
                        ExecutionBackendKind::KVM, ExecutionBackendKind::WHP,
                        ExecutionBackendKind::HVF)),
    [](const testing::TestParamInfo<AndroidSemaphore::ParamType> &Info) {
      std::string Name = std::get<0>(Info.param);
      std::replace(Name.begin(), Name.end(), '-', '_');
      return Name + "_" + executionBackendName(std::get<1>(Info.param));
    });
} // namespace
} // namespace neverd::emulation
