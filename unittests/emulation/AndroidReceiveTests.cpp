//===- AndroidReceiveTests.cpp - Original blocking receive callers --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Raw traps, libc errno and socket lifetime across cooperative guest threads.

#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000, Second = 1000000000;
class AndroidReceive : public testing::TestWithParam<
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
    auto Probe = llvm::cantFail(probeExecutionBackend(C));
    if (Probe.Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe.Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->ThreadLimit = 3;
    Options.Android->TraceLimit = 50000;
    Options.Android->Memory.push_back(
        {Buffer, 8192, std::vector<uint8_t>(8192), false});
    Options.Android->ReadMemory.push_back({Buffer, 8192});
    Options.InstructionQuantum = 31;
    Options.Limits.Instructions = 50000;
    Options.LinuxTime.emplace().AdvanceOnIdle = true;
    Options.LinuxTime->Clocks[1] = {100, 0};
    Options.LinuxFiles.emplace();
    LinuxStreamConnection S;
    S.Peer = {2, 0, 0x23, 0x29, 127, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    S.ReceiveEvents = {{{0x5a}, 0, 0, 7}, {{0x6b}, 0, 0, 11}};
    Options.LinuxNetwork.emplace().Connections = {S, S};
    Options.LinuxNetwork->Connections[1].ReceiveEvents = {{{0xb6}, 0, 0, 0}};
    LinuxDatagramConnection D;
    D.Peer = S.Peer;
    D.ReceiveEvents = {{{}, 0, 0, 7}};
    Options.LinuxNetwork->DatagramConnections = {D};
#endif
  }
  ProcessResult run(uint64_t Route, uint64_t Action = 0, uint64_t Type = 1) {
    Options.Android->EntrySymbol = "receive_workers";
    Options.Android->Arguments = {Buffer, Route, Action, Type};
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
  uint64_t word(const ProcessResult &R, unsigned I) {
    return llvm::support::endian::read64le(
        R.MemorySnapshots.at(0).Bytes.data() + 8 * I);
  }
  void callResult(const ProcessResult &R, uint64_t Route,
                  std::optional<uint64_t> Value) {
    unsigned Calls = 0;
    if (Route == 0 || Route == 2) {
      for (const auto &C : R.Services)
        if (C.ThreadID == 1001 && C.Number == (Route == 0 ? 207 : 63)) {
          ++Calls;
          EXPECT_EQ(C.Result, Value);
        }
    } else {
      const char *Name = Route == 1   ? "recv"
                         : Route == 3 ? "read"
                         : Route == 4 ? "syscall"
                         : Route == 6 ? "__read_chk"
                                      : "recvfrom";
      for (const auto &C : R.NativeCalls)
        if (C.ThreadID == 1001 && C.Name == Name) {
          ++Calls;
          EXPECT_EQ(C.Result, Value);
        }
    }
    EXPECT_EQ(Calls, 1u); // One original call, not a replayed trap or import.
  }
  void completed(const ProcessResult &R, uint64_t Route, uint64_t Value,
                 uint64_t Error = 73) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    EXPECT_FALSE(R.TraceTruncated);
    EXPECT_EQ(R.Trace.size(), R.Instructions);
    callResult(R, Route, Value);
    EXPECT_EQ(word(R, 0), Value);
    EXPECT_EQ(word(R, 1), Error);
    EXPECT_EQ(word(R, 4), 123u);
    EXPECT_EQ(word(R, 6), 456u);
    EXPECT_EQ(word(R, 8), 100u);
    EXPECT_EQ(word(R, 9), 3u);
    EXPECT_EQ(word(R, 11), 91u);
    EXPECT_EQ(word(R, 12), 42u);
    EXPECT_EQ(word(R, 13), 43u);
    for (const auto &T : R.NativeThreads) {
      EXPECT_TRUE(T.Finished);
      EXPECT_FALSE(T.Waiting);
    }
  }
};

TEST_P(AndroidReceive, RawAndLibcReadAndRecvSuspendBeforeDataArrival) {
  for (uint64_t Route : {0u, 1u, 2u, 3u}) {
    auto R = run(Route);
    completed(R, Route, 1);
    EXPECT_EQ(word(R, 2), 100u);
    EXPECT_EQ(word(R, 3), 7u);
    EXPECT_EQ(word(R, 5), 0x5au);
  }
}
TEST_P(AndroidReceive,
       ReadChkResumesTheOriginalReadAcrossDescriptorAndMemoryChanges) {
  for (uint64_t Action : {0u, 1u}) {
    SCOPED_TRACE(Action);
    auto R = run(6, Action);
    completed(R, 6, 1);
    EXPECT_EQ(word(R, 3), 7u);
    EXPECT_EQ(word(R, 5), 0x5au);
    if (Action == 1)
      EXPECT_EQ(word(R, 16), 0xb6u);
  }
  // The shared atomic socket contract refuses a lost destination mapping.
  // It leaves the original call pending instead of claiming an errno result.
  auto Refused = run(6, 2);
  EXPECT_EQ(Refused.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Refused.Diagnostic.find("completely accessible"),
            std::string::npos);
  callResult(Refused, 6, std::nullopt);
  EXPECT_EQ(word(Refused, 4), 0u);
  EXPECT_EQ(word(Refused, 512), 0u);
  EXPECT_EQ(word(Refused, 9), 3u);
  EXPECT_EQ(word(Refused, 6), 456u);
  Options.LinuxNetwork->Connections[0].ReceiveEvents = {
      {{0x5a}, 0, 0, 3 * Second}};
  auto R = run(6, 3);
  completed(R, 6, UINT64_MAX, 11);
  EXPECT_EQ(word(R, 2), 102u);
  EXPECT_EQ(word(R, 3), 0u);
}
TEST_P(AndroidReceive, TimeoutSnapshotSurvivesOptionChangeAndArrivalWinsTie) {
  for (uint64_t Arrival : {2 * Second, 3 * Second}) {
    Options.LinuxNetwork->Connections[0].ReceiveEvents = {
        {{0x5a}, 0, 0, Arrival}};
    for (uint64_t Route : {0u, 1u, 4u}) {
      auto R = run(Route, 3);
      const bool Expired = Arrival > 2 * Second;
      completed(R, Route, Expired ? (Route ? UINT64_MAX : uint64_t(-11)) : 1,
                Expired && Route ? 11 : 73);
      EXPECT_EQ(word(R, 2), 102u);
      EXPECT_EQ(word(R, 3), 0u);
      EXPECT_EQ(word(R, 5), Expired ? 0u : 0x5au);
    }
  }
}
TEST_P(AndroidReceive, ClosedAndReusedDescriptorCannotStealSuspendedReceive) {
  for (uint64_t Route : {0u, 1u, 2u, 3u}) {
    auto R = run(Route, 1);
    completed(R, Route, 1);
    EXPECT_EQ(word(R, 3), 7u);
    EXPECT_EQ(word(R, 5), 0x5au);
    EXPECT_EQ(word(R, 10), 1u);
    EXPECT_EQ(word(R, 16), 0xb6u);
  }
}
TEST_P(AndroidReceive, TwoConsumersShareArrivalCursorAndRearmTheLoser) {
  for (uint64_t Route : {0u, 1u}) {
    auto R = run(Route, 4);
    completed(R, Route, 1);
    EXPECT_EQ(word(R, 10), 1u);
    const auto First = word(R, 5), Other = word(R, 16);
    EXPECT_TRUE((First == 0x5a && Other == 0x6b) ||
                (First == 0x6b && Other == 0x5a));
    EXPECT_EQ(word(R, 3), First == 0x5a ? 7u : 11u);
    EXPECT_EQ(word(R, 15), Other == 0x5a ? 7u : 11u);
  }
}
TEST_P(AndroidReceive, BufferPermissionsChangedWhileBlockedRemainUnsupported) {
  for (uint64_t Route : {0u, 1u, 2u, 3u}) {
    auto R = run(Route, 2);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("completely accessible"), std::string::npos);
    callResult(R, Route, std::nullopt);
    EXPECT_EQ(word(R, 4), 0u);
    EXPECT_EQ(word(R, 512), 0u);
    EXPECT_EQ(word(R, 9), 3u);
    EXPECT_EQ(word(R, 6), 456u);
  }
}
TEST_P(AndroidReceive, DelayedEOFCompletesTheOriginalReceiveWithZero) {
  auto &C = Options.LinuxNetwork->Connections[0];
  C.ReceiveEvents.clear();
  C.ReceiveEOF = true;
  C.ReceiveEOFAtNS = 7;
  for (uint64_t Route : {0u, 1u}) {
    auto R = run(Route);
    completed(R, Route, 0);
    EXPECT_EQ(word(R, 3), 7u);
    EXPECT_EQ(word(R, 5), 0u);
  }
}
TEST_P(AndroidReceive, EmptyDatagramWakesSyscallAndRecvfromWithoutPayload) {
  for (uint64_t Route : {4u, 5u}) {
    auto R = run(Route, 0, 2);
    completed(R, Route, 0);
    EXPECT_EQ(word(R, 3), 7u);
    EXPECT_EQ(word(R, 5), 0u);
    if (Route == 5) {
      EXPECT_EQ(word(R, 20), 16u);
      EXPECT_EQ(word(R, 32), 0x0100007f29230002u);
    }
  }
}
TEST_P(AndroidReceive, BufferedImportsNeverResumeAsTheirNestedRead) {
  for (uint64_t Arrival : {0u, 7u}) {
    Options.LinuxNetwork->Connections[0].ReceiveEvents[0].ReadyAtNS = Arrival;
    Options.Android->EntrySymbol = "receive_buffered";
    Options.Android->Arguments = {Buffer};
    auto R = llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
    EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 1u);
    EXPECT_EQ(word(R, 0), 1u);
    EXPECT_EQ(word(R, 1), 0x5au);
    unsigned Completed = 0;
    for (const auto &C : R.NativeCalls)
      if (C.Name == "fread") {
        ++Completed;
        EXPECT_EQ(C.Result, 1u);
      }
    EXPECT_EQ(Completed, 1u);
  }
}
INSTANTIATE_TEST_SUITE_P(
    Backends, AndroidReceive,
    testing::Combine(testing::Values("receive-O0-none", "receive-O0-android",
                                     "receive-O0-relr", "receive-O2-none",
                                     "receive-O2-android", "receive-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
