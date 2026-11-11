//===- AndroidPollTests.cpp - Scheduled readiness across guest threads
//===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original SVC/libc callers, copied wait state and deterministic virtual time.

#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
class AndroidPoll : public testing::TestWithParam<
                        std::tuple<const char *, ExecutionBackendKind>> {
protected:
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Android fixtures unavailable";
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
    Options.Android->ThreadLimit = 3;
    Options.Android->TraceLimit = 50000;
    Options.Android->Memory.push_back(
        {Buffer, 8192, std::vector<uint8_t>(8192), false});
    Options.Android->ReadMemory.push_back({Buffer, 8192});
    Options.InstructionQuantum = 7;
    Options.Limits.Instructions = 50000;
    Options.LinuxTime.emplace().AdvanceOnIdle = true;
    Options.LinuxTime->Clocks[1] = {100, 0};
    Options.LinuxFiles.emplace();
    LinuxStreamConnection C0;
    C0.Peer = {2, 0, 0x23, 0x29, 127, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    C0.ReceiveEvents = {{{0x5a}, 0, 0, 7}};
    Options.LinuxNetwork.emplace().Connections = {C0};
#endif
  }
  ProcessResult run(uint64_t Route, uint64_t Capacity) {
    Options.Android->EntrySymbol = "poll_workers";
    Options.Android->Arguments = {Buffer, Route, Capacity};
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
  uint64_t word(const ProcessResult &R, unsigned I) {
    return llvm::support::endian::read64le(
        R.MemorySnapshots.at(0).Bytes.data() + 8 * I);
  }
  void completed(const ProcessResult &R) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    EXPECT_EQ(word(R, 10), 42u);
    EXPECT_EQ(word(R, 11), 43u);
    EXPECT_EQ(word(R, 12), 100u);
    EXPECT_EQ(word(R, 13), 3u);
    EXPECT_EQ(word(R, 14), 456u);
    EXPECT_FALSE(R.TraceTruncated);
    for (const auto &C : R.NativeCalls)
      EXPECT_TRUE(C.Result) << C.Name;
  }
};

TEST_P(AndroidPoll, OriginalImportsAndRawWrappersWaitForSharedReadiness) {
  const char *Names[] = {"poll",        "ppoll",   "epoll_wait",
                         "epoll_pwait", "syscall", "syscall"};
  for (uint64_t Route = 0; Route != 6; ++Route) {
    auto R = run(Route, 0);
    completed(R);
    EXPECT_EQ(word(R, 0), 1u);
    EXPECT_EQ(word(R, 1), 73u);
    EXPECT_EQ(word(R, 2), 100u);
    EXPECT_EQ(word(R, 3), 7u);
    EXPECT_EQ(word(R, 4), 1u);
    EXPECT_EQ(word(R, 5), Route == 2 || Route == 3 || Route == 5
                              ? 0x1234567887654321ULL
                              : 0u);
    EXPECT_EQ(word(R, 7), Route == 4 ? 999993u : 1000000u);
    unsigned Calls = 0;
    for (const auto &C : R.NativeCalls)
      if (C.ThreadID == 1001 && C.Name == Names[Route]) {
        ++Calls;
        EXPECT_EQ(C.Result, 1u);
      }
    EXPECT_EQ(Calls, 1u);
  }
}
TEST_P(AndroidPoll, TimeoutsPreserveLibcTimespecAndUseVirtualMilliseconds) {
  auto &C = Options.LinuxNetwork->Connections[0];
  C.ReceiveEvents.clear();
  C.ReceiveQuietUntilNS = 2000000;
  for (uint64_t Route = 0; Route != 6; ++Route) {
    auto R = run(Route, 0);
    completed(R);
    EXPECT_EQ(word(R, 0), 0u);
    EXPECT_EQ(word(R, 1), 73u);
    EXPECT_EQ(word(R, 3), 1000000u);
    EXPECT_EQ(word(R, 7), Route == 4 ? 0u : 1000000u);
  }
}
TEST_P(AndroidPoll, EpollFailuresConvertErrnoInTheCallingThread) {
  for (uint64_t Route : {2u, 3u, 5u}) {
    auto R = run(Route, 1);
    completed(R);
    EXPECT_EQ(word(R, 0), UINT64_MAX);
    EXPECT_EQ(word(R, 1), 9u);
  }
}
TEST_P(AndroidPoll, ConnectRelativeSilenceExpiresAtTheGuestDeadline) {
  auto &C = Options.LinuxNetwork->Connections[0];
  C.ReceiveEvents.clear();
  C.ReceiveQuietAfterConnectNS = 1000000;
  for (uint64_t Route : {0u, 4u}) {
    auto R = run(Route, 0);
    completed(R);
    EXPECT_EQ(word(R, 0), 0u);
    EXPECT_EQ(word(R, 3), 1000000u);
    EXPECT_EQ(word(R, 4), 0u);
    EXPECT_EQ(word(R, 7), Route == 4 ? 0u : 1000000u);
  }
  Options.LinuxTime->AdvanceOnIdle = false;
  auto MissingClock =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_FALSE(bool(MissingClock));
  EXPECT_NE(llvm::toString(MissingClock.takeError())
                .find("linux_time.advance_on_idle"),
            std::string::npos);
}
INSTANTIATE_TEST_SUITE_P(
    Backends, AndroidPoll,
    testing::Combine(testing::Values("poll-O0-none", "poll-O0-android",
                                     "poll-O0-relr", "poll-O2-none",
                                     "poll-O2-android", "poll-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
