//===- AndroidMessageQueueTests.cpp - JNI queue lifetime checks -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original JNI calls across optimization, relocation and CPU backends.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace neverd::emulation {
namespace {

class AndroidMessageQueue
    : public testing::TestWithParam<
          std::tuple<const char *, ExecutionBackendKind>> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  static constexpr uint64_t Buffer = 0x20000000;

  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Android fixture compiler unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(std::get<0>(GetParam())) + ".so");
    Options.Backend = std::get<1>(GetParam());
    ExecutionConfiguration Configuration;
    Configuration.Backend = Options.Backend;
    Configuration.Architecture = GuestArchitecture::AArch64;
    Configuration.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Configuration);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->JNI = true;
    Options.Android->EntrySymbol = "message_queue";
    Options.Android->ObserveJavaState = true;
    Options.Android->Memory.push_back(
        {Buffer, 4096, std::vector<uint8_t>(4096, 0xa5)});
    Options.Android->ReadMemory = {{Buffer, 4096}};
    auto &Input = Options.Android->Application.emplace();
    Input.main_looper_prepared = true;
    Input.initial_message_pool_empty = true;
    Input.initial_main_queue
        .emplace(
            mobile::dalvik::AndroidApplicationSnapshot::EmptyMessageQueue{})
        .native_callbacks_empty = true;
    Options.LinuxTime.emplace();
    Options.LinuxTime->Clocks = {{1, {10, 123456789}}};
    Options.LinuxTime->AdvanceOnIdle = true;
    Options.InstructionQuantum = 7;
    Options.Limits.Instructions = 50000;
    Options.Limits.Events = 5000;
    Options.Limits.TimeoutMicroseconds = 30000000;
#endif
  }

  ProcessResult run(unsigned Mode = 0) {
    Options.Android->Arguments = {Buffer, Mode};
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }

  void output(const ProcessResult &Result, bool Completed) {
    ASSERT_EQ(Result.MemorySnapshots.size(), 1u);
    std::vector<uint8_t> Expected(4096, 0xa5);
    if (Completed) {
      for (unsigned I = 0; I != 8; ++I)
        llvm::support::endian::write64le(Expected.data() + I * 8,
                                         I == 7 ? 73 : 1);
    }
    EXPECT_EQ(Result.MemorySnapshots[0].Bytes, Expected);
  }
};

TEST_P(AndroidMessageQueue, RetrievesLiveMessagesAcrossJNIReferenceLifetimes) {
  auto Result = run();
  ASSERT_EQ(Result.Stop, ProcessStopReason::Returned) << Result.Diagnostic;
  EXPECT_EQ(Result.ReturnValue, 0u);
  ASSERT_TRUE(Result.JavaState);
  ASSERT_TRUE(Result.JavaState->framework.main_message_queue);
  const auto &Queue = *Result.JavaState->framework.main_message_queue;
  EXPECT_TRUE(Queue.complete);
  EXPECT_TRUE(Queue.messages.empty());
  EXPECT_FALSE(Queue.blocked);
  output(Result, true);
}

TEST_P(AndroidMessageQueue, MissingOrNonemptyNativeCallbacksPreserveMessages) {
  for (const auto Empty : {std::optional<bool>{}, std::optional<bool>{false}}) {
    Options.Android->Application->initial_main_queue->native_callbacks_empty =
        Empty;
    auto Result = run();
    ASSERT_EQ(Result.Stop, ProcessStopReason::UnsupportedService)
        << Result.Diagnostic;
    EXPECT_EQ(
        Result.Diagnostic,
        "Android MessageQueue native callback state is not empty or known");
    ASSERT_TRUE(Result.JavaState);
    ASSERT_TRUE(Result.JavaState->framework.main_message_queue);
    EXPECT_EQ(Result.JavaState->framework.main_message_queue->messages.size(),
              2u);
    ASSERT_FALSE(Result.NativeCalls.empty());
    EXPECT_FALSE(Result.NativeCalls.back().Result);
    output(Result, false);
  }
}

TEST_P(AndroidMessageQueue,
       EmptyAndForeignQueuesRetainTheirExplicitBoundaries) {
  for (unsigned Mode : {1u, 2u}) {
    auto Result = run(Mode);
    ASSERT_EQ(Result.Stop, ProcessStopReason::UnsupportedService)
        << Result.Diagnostic;
    EXPECT_EQ(Result.Diagnostic,
              Mode == 1 ? "Android MessageQueue blocking wait is unmodeled"
                        : "Android MessageQueue state is not supplied");
    ASSERT_TRUE(Result.JavaState);
    ASSERT_TRUE(Result.JavaState->framework.main_message_queue);
    EXPECT_EQ(Result.JavaState->framework.main_message_queue->messages.size(),
              Mode == 1 ? 0u : 2u);
    ASSERT_FALSE(Result.NativeCalls.empty());
    EXPECT_FALSE(Result.NativeCalls.back().Result);
    output(Result, Mode == 1);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Backends, AndroidMessageQueue,
    testing::Combine(
        testing::Values("message_queue-O0-none", "message_queue-O2-none",
                        "message_queue-O0-android", "message_queue-O2-android",
                        "message_queue-O0-relr", "message_queue-O2-relr"),
        testing::Values(ExecutionBackendKind::Unicorn,
                        ExecutionBackendKind::HVF, ExecutionBackendKind::KVM,
                        ExecutionBackendKind::WHP)));

} // namespace
} // namespace neverd::emulation
