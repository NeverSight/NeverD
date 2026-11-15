//===- AndroidStreamWaitTests.cpp - Resumable buffered input -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Buffered libc callers retain progress across scheduled kernel reads.

#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
class AndroidStreamWait : public testing::TestWithParam<
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
    C0.ReceiveEvents = {{{'a', 'b'}, 0, 0, 1}, {{'c', '\n', 'Z'}, 0, 0, 7}};
    C0.ReceiveEOF = true;
    C0.ReceiveEOFAtNS = 11;
    Options.LinuxNetwork.emplace().Connections = {C0};
#endif
  }
  ProcessResult run(uint64_t Route, uint64_t Capacity) {
    Options.Android->EntrySymbol = "stream_waits";
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

TEST_P(AndroidStreamWait, PartialItemsAndLinesRetainProgressAcrossRefills) {
  for (uint64_t Capacity : {2u, 8u}) {
    for (uint64_t Route : {0u, 1u, 3u, 4u, 6u, 7u}) {
      auto R = run(Route, Capacity);
      completed(R);
      const bool Block = Route == 0 || Route == 3 || Route == 6;
      EXPECT_EQ(word(R, 0), Block ? 2u : Buffer + 256);
      EXPECT_EQ(word(R, 1), 73u);
      EXPECT_EQ(word(R, 2), 100u);
      EXPECT_EQ(word(R, 3), Block ? 11u : 7u);
      EXPECT_EQ(word(R, 4), Block ? 1u : 0u);
      EXPECT_EQ(word(R, 5), 0u);
      const auto *Bytes = R.MemorySnapshots.at(0).Bytes.data() + 256;
      EXPECT_EQ(std::string(reinterpret_cast<const char *>(Bytes), 5),
                Block ? std::string("abc\nZ") : std::string("abc\n\0", 5));
      if (!Block) {
        EXPECT_EQ(word(R, 6), 'Z');
        EXPECT_EQ(word(R, 7), UINT32_MAX);
        EXPECT_EQ(word(R, 8), 1u);
      }
    }
  }
}
TEST_P(AndroidStreamWait, GetcCompletesTheOriginalCharacterImport) {
  for (uint64_t Route : {2u, 5u}) {
    auto R = run(Route, 2);
    completed(R);
    EXPECT_EQ(word(R, 0), 'a');
    EXPECT_EQ(word(R, 1), 73u);
    EXPECT_EQ(word(R, 3), 1u);
    EXPECT_EQ(word(R, 4), 0u);
    EXPECT_EQ(word(R, 5), 0u);
  }
}
TEST_P(AndroidStreamWait, PartialElementTimeoutKeepsBytesAndThreadLocalErrno) {
  auto &C = Options.LinuxNetwork->Connections[0];
  C.ReceiveEvents = {{{'a', 'b', 'c'}, 0, 0, 1}};
  C.ReceiveEOF = false;
  C.ReceiveEOFAtNS.reset();
  C.ReceiveQuietUntilNS = 3000000000;
  for (uint64_t Capacity : {2u, 8u}) {
    auto R = run(0, Capacity);
    completed(R);
    EXPECT_EQ(word(R, 0), 1u);
    EXPECT_EQ(word(R, 1), 11u);
    EXPECT_EQ(word(R, 2), 102u);
    EXPECT_EQ(word(R, 3), 1u);
    EXPECT_EQ(word(R, 4), 0u);
    EXPECT_EQ(word(R, 5), 1u);
    EXPECT_EQ(word(R, 32) & 0xffffff, 0x636261u);
  }
}
TEST_P(AndroidStreamWait, UnknownRefillLeavesOriginalImportIncomplete) {
  auto &C = Options.LinuxNetwork->Connections[0];
  C.ReceiveEvents = {{{'a', 'b', 'c'}, 0, 0, 1}};
  C.ReceiveEOF = false;
  C.ReceiveEOFAtNS.reset();
  auto R = run(0, 8);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R.Diagnostic, "socket readiness has no offline observation "
                          "(TCP connection index 0, read readiness)");
  EXPECT_EQ(word(R, 32) & 0xffffff, 0x636261u);
  unsigned Calls = 0;
  for (const auto &C : R.NativeCalls)
    if (C.Name == "fread") {
      ++Calls;
      EXPECT_FALSE(C.Result);
    }
  EXPECT_EQ(Calls, 1u);
}
TEST_P(AndroidStreamWait,
       FortifiedBoundsFailBeforeInvalidStreamOrMemoryAccess) {
  for (uint64_t Route : {0u, 1u, 2u}) {
    Options.Android->EntrySymbol = "stream_checked_bounds";
    Options.Android->Arguments = {Buffer, Route};
    auto R = llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("async-safe text log write contract"),
              std::string::npos);
    EXPECT_TRUE(llvm::StringRef(R.StandardError).starts_with("FORTIFY: f"));
    ASSERT_EQ(R.NativeCalls.size(), 1u);
    EXPECT_FALSE(R.NativeCalls[0].Result);
  }
}
INSTANTIATE_TEST_SUITE_P(
    Backends, AndroidStreamWait,
    testing::Combine(
        testing::Values("stream_waits-O0-none", "stream_waits-O0-android",
                        "stream_waits-O0-relr", "stream_waits-O2-none",
                        "stream_waits-O2-android", "stream_waits-O2-relr"),
        testing::Values(ExecutionBackendKind::Unicorn,
                        ExecutionBackendKind::HVF, ExecutionBackendKind::KVM,
                        ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
