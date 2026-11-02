//===- AndroidNetworkTests.cpp - Offline resolver and TCP callers --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded offline network contracts and their semantic checks.

#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <cstring>
#include <tuple>
#include <utility>
namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
const std::vector<uint8_t> IPv4{2, 0, 0x23, 0x29, 127, 0, 0, 1,
                                0, 0, 0,    0,    0,   0, 0, 0};
const std::vector<uint8_t> IPv6{10, 0, 0x23, 0x29, 0, 0, 0, 0, 0, 0,
                                0,  0, 0,    0,    0, 0, 0, 0, 0, 0,
                                0,  0, 0,    1,    0, 0, 0, 0};
class AndroidNetwork : public testing::TestWithParam<
                           std::tuple<const char *, ExecutionBackendKind>> {
protected:
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Android cross compiler unavailable";
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
    Options.Android->Memory.push_back(
        {Buffer, 4096, std::vector<uint8_t>(4096, 0xa5), false});
    Options.Android->ReadMemory.push_back({Buffer, 4096});
    AndroidResolverQuery Q;
    Q.Node = "node.invalid";
    Q.Service = "9001";
    Q.Flags = 2;
    Q.SocketType = 1;
    Q.Addresses = {{1, 6, IPv4, "canonical.invalid"}, {1, 6, IPv6, {}}};
    Options.Android->ResolverQueries = std::vector{Q};
    Options.LinuxFiles.emplace();
    LinuxStreamConnection Stream;
    Stream.Peer = IPv4;
    Stream.ExpectedSend = {0, 0xff, 0x42, 0x13, 0x7f};
    Stream.Receive = {0x31, 0, 0xfe, 0x7f};
    Stream.ReceiveEOF = true;
    Stream.MaxSend = Stream.MaxReceive = 2;
    Options.LinuxNetwork.emplace().Connections.push_back(Stream);
    Options.Limits.Instructions = 100000;
#endif
  }
  ProcessResult run(const char *Entry, uint64_t Mode = 0) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = {Buffer, Mode};
    auto Result =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    // Preserve loader diagnostics in Release too: cantFail's failure path is
    // unreachable in an assertion-disabled build.
    if (!Result)
      llvm::report_fatal_error(Result.takeError());
    return std::move(*Result);
  }
  void returned(const ProcessResult &R) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
  uint64_t word(const ProcessResult &R, unsigned I) {
    return llvm::support::endian::read64le(
        R.MemorySnapshots.at(0).Bytes.data() + 8 * I);
  }
};
TEST_P(AndroidNetwork,
       AddressParsingUsesAPI28DecimalAndRejectsMalformedInputs) {
  struct Case {
    unsigned Family;
    const char *Text;
    const char *Hex;
  };
  for (auto C : {Case{2, "127.0.0.1", "7f000001"},
                 {2, "001.002.003.004", "01020304"},
                 {2, "256.0.0.1", ""},
                 {2, "127.1", ""},
                 {2, "127.0.0.1 ", ""},
                 {2, "1.2.3.4.5", ""},
                 {10, "::", "00000000000000000000000000000000"},
                 {10, "::1", "00000000000000000000000000000001"},
                 {10, "2001:DB8::1", "20010db8000000000000000000000001"},
                 {10, "::ffff:127.0.0.1", "00000000000000000000ffff7f000001"},
                 {10, "1:2:3:4:5:6:7:8", "00010002000300040005000600070008"},
                 {10, "1:2:3:4:5:6:7::8", ""},
                 {10, "1::2::3", ""},
                 {10, ":::1", ""},
                 {10, "::1%2", ""},
                 {10, "12345::1", ""},
                 {10, "127.0.0.1::", ""}}) {
    std::copy(C.Text, C.Text + std::strlen(C.Text) + 1,
              Options.Android->Memory[0].Bytes.begin() + 512);
    auto R = run("network_pton", C.Family);
    returned(R);
    const auto HexBytes = llvm::fromHex(C.Hex);
    const std::vector<uint8_t> Expected(HexBytes.begin(), HexBytes.end());
    EXPECT_EQ(word(R, 0), Expected.empty() ? 0u : 1u) << C.Text;
    EXPECT_EQ(word(R, 1), 73u);
    const auto &Bytes = R.MemorySnapshots[0].Bytes;
    EXPECT_TRUE(
        std::equal(Expected.begin(), Expected.end(), Bytes.begin() + 64));
    EXPECT_EQ(Bytes[64 + Expected.size()], 0xa5);
  }
  auto R = run("network_pton", 999);
  returned(R);
  EXPECT_EQ(word(R, 0), UINT64_MAX);
  EXPECT_EQ(word(R, 1), 97u);
}

TEST_P(AndroidNetwork, TCPConnectFailuresShareRawNamedDynamicAndVariadicCalls) {
  Options.Android->Libraries["libc.so"] = {"connect"};
  using Case = std::pair<uint64_t, uint32_t>;
  for (const auto &[Mode, Count] :
       {Case{0, 4}, Case{0, 4097}, Case{0, UINT32_MAX}, Case{1, 4},
        Case{2, 4}}) {
    SCOPED_TRACE(Mode);
    SCOPED_TRACE(Count);
    Options.LinuxNetwork->TCPConnectFailures =
        LinuxTCPConnectFailures{101, Count};
    const auto R = run("network_connect_failures", Mode);
    if (!Mode)
      returned(R);
    else {
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
      EXPECT_FALSE(R.ReturnValue);
      EXPECT_NE(R.Diagnostic.find(Mode == 1 ? "failure contract exhausted"
                                            : "closed before reuse"),
                std::string::npos);
      ASSERT_FALSE(R.NativeCalls.empty());
      EXPECT_EQ(R.NativeCalls.back().Name, "connect");
      EXPECT_FALSE(R.NativeCalls.back().Result);
      EXPECT_EQ(word(R, 10), 0xa5a5a5a5a5a5a5a5ULL);
    }
    EXPECT_EQ(word(R, 0), 0u);
    for (unsigned I = 0; I != (Mode == 2 ? 1u : 4u); ++I) {
      EXPECT_EQ(word(R, 1 + 2 * I), I == 3 ? uint64_t(-101) : UINT64_MAX);
      EXPECT_EQ(word(R, 2 + 2 * I), I == 3 ? 73u : 101u);
    }
    EXPECT_TRUE(std::all_of(R.MemorySnapshots[0].Bytes.begin() + 88,
                            R.MemorySnapshots[0].Bytes.end(),
                            [](uint8_t V) { return V == 0xa5; }));
  }
}

TEST_P(AndroidNetwork, DatagramFallbackRetainsPacketsSourcesAndExplicitErrors) {
  Options.Android->Libraries["libc.so"] = {"recv"};
  for (uint64_t Mode : {0, 1, 2, 3}) {
    const auto &Peer = Mode & 2 ? IPv6 : IPv4;
    LinuxStreamConnection Failure;
    Failure.Peer = Peer;
    Failure.ConnectErrno = 111;
    Options.LinuxNetwork->Connections = {Failure};
    LinuxDatagramConnection D;
    D.Peer = Peer;
    D.ExpectedSend = {{0, 0xff, 0x42}, {}};
    D.ReceiveEvents = {
        {{7, 8, 9}, 0, 1}, {{}, 0, 2}, {{}, 11, 2}, {{10, 11}, 0, 2}};
    Options.LinuxNetwork->DatagramConnections = {D};
    const auto R = run("network_datagram", Mode);
    returned(R);
    EXPECT_EQ(word(R, 0), UINT64_MAX);
    EXPECT_EQ(word(R, 1), 111u);
    EXPECT_EQ(word(R, 2), 1u);
    EXPECT_EQ(word(R, 3), 0u);
    EXPECT_EQ(word(R, 4), 3u);
    EXPECT_EQ(word(R, 5), 3u);
    EXPECT_EQ(word(R, 6), Peer.size());
    EXPECT_EQ(word(R, 7), 1u);
    for (unsigned I = 8; I <= 11; ++I)
      EXPECT_EQ(word(R, I), 0u);
    EXPECT_EQ(word(R, 12), UINT64_MAX);
    EXPECT_EQ(word(R, 13), 11u);
    EXPECT_EQ(word(R, 14), 2u);
    EXPECT_EQ(word(R, 15), 11u);
    const auto &Bytes = R.MemorySnapshots[0].Bytes;
    EXPECT_EQ(Bytes[256], 7);
    EXPECT_EQ(Bytes[257], 8);
    EXPECT_EQ(Bytes[258], 0xa5);
    EXPECT_EQ(Bytes[264], 7);
    EXPECT_EQ(Bytes[265], 0xa5);
    EXPECT_EQ(Bytes[272], 10);
    EXPECT_EQ(Bytes[273], 11);
    EXPECT_EQ(Bytes[274], 0xa5);
    EXPECT_TRUE(
        std::equal(Peer.begin(), Peer.begin() + 3, Bytes.begin() + 320));
    EXPECT_EQ(Bytes[323], 0xa5);
  }
}

TEST_P(AndroidNetwork, FortifiedMemorySharesCopyMoveAndFillSemantics) {
  Options.Android->Libraries["libc.so"] = {"__memcpy_chk", "__memmove_chk",
                                           "__memset_chk"};
  for (uint64_t Mode : {0, 4, 5}) {
    SCOPED_TRACE(Mode);
    auto R = run("network_checked_memory", Mode);
    returned(R);
    for (const char *Name : {"__memcpy_chk", "__memmove_chk", "__memset_chk"})
      EXPECT_EQ(std::count_if(R.NativeCalls.begin(), R.NativeCalls.end(),
                              [&](const auto &C) { return C.Name == Name; }),
                1);
    for (unsigned I = 0; I != 3; ++I)
      EXPECT_EQ(word(R, I), 1u);
    const auto &Bytes = R.MemorySnapshots[0].Bytes;
    const std::vector<uint8_t> Expected =
        Mode == 4 ? std::vector<uint8_t>(7, 0xa5)
                  : std::vector<uint8_t>{0, 0, 0xff, 0x42, 0xab, 0xab, 0xa5};
    EXPECT_TRUE(
        std::equal(Expected.begin(), Expected.end(), Bytes.begin() + 256));
  }
  for (uint64_t Mode : {1, 2, 3, 6, 7}) {
    SCOPED_TRACE(Mode);
    auto R = run("network_checked_memory", Mode);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    EXPECT_NE(R.Diagnostic.find(Mode == 6   ? "overlapping memcpy"
                                : Mode == 7 ? "invalid guest pointer"
                                            : "FORTIFY"),
              std::string::npos);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
}

TEST_P(AndroidNetwork, StreamReceiveEventsLeaveProtocolDecisionsToGuestCode) {
  for (bool Malformed : {false, true}) {
    auto &C = Options.LinuxNetwork->Connections[0];
    C.ExpectedSend = {0, 0xff, 0x42};
    C.MaxSend = 8;
    C.Receive.clear();
    C.ReceiveEvents = {{{}, 4, 0},
                       {{'O', 'K', 0}, 0, 3},
                       {{}, 11, 3},
                       {{uint8_t(Malformed ? '?' : '!')}, 0, 3}};
    const auto R = run("network_receive_events");
    returned(R);
    EXPECT_EQ(word(R, 0), UINT64_MAX);
    EXPECT_EQ(word(R, 1), 4u);
    EXPECT_EQ(word(R, 2), 3u);
    EXPECT_EQ(word(R, 3), 2u);
    EXPECT_EQ(word(R, 4), 1u);
    EXPECT_EQ(word(R, 5), UINT64_MAX);
    EXPECT_EQ(word(R, 6), 11u);
    EXPECT_EQ(word(R, 7), 1u);
    EXPECT_EQ(word(R, 8), 0u);
    EXPECT_EQ(word(R, 9), !Malformed);
  }
}

TEST_P(AndroidNetwork, AddressFormattingPreservesABIAndBufferBounds) {
  struct Case {
    unsigned Family;
    const char *Hex;
    const char *Text;
  };
  for (auto C : {Case{2, "7f000001", "127.0.0.1"},
                 {10, "00000000000000000000000000000000", "::"},
                 {10, "00000000000000000000000000000001", "::1"},
                 {10, "00000000000000000000ffff7f000001", "::ffff:127.0.0.1"},
                 {10, "000000000000000000000000c0000201", "::192.0.2.1"},
                 {10, "20010000000000010000000000010001", "2001::1:0:0:1:1"},
                 {10, "20010000000100010001000100010001", "2001:0:1:1:1:1:1:1"},
                 {10, "20010db8000000000000000000000000", "2001:db8::"}}) {
    auto Bytes = llvm::fromHex(C.Hex);
    std::copy(Bytes.begin(), Bytes.end(),
              Options.Android->Memory[0].Bytes.begin() + 512);
    const auto Length = std::strlen(C.Text);
    for (auto Size : {Length, Length + 1, size_t(64)}) {
      auto R = run("network_ntop", (uint64_t(Size) << 32) | C.Family);
      returned(R);
      const bool Fits = Size > Length;
      EXPECT_EQ(word(R, 0), Fits);
      EXPECT_EQ(word(R, 1), Fits ? 73u : 28u);
      const auto &Memory = R.MemorySnapshots[0].Bytes;
      if (Fits)
        EXPECT_TRUE(
            std::equal(C.Text, C.Text + Length + 1, Memory.begin() + 64));
      else
        EXPECT_EQ(Memory[64], 0xa5);
      EXPECT_EQ(Memory[64 + Length + 1], 0xa5);
    }
  }
  auto R = run("network_ntop", (uint64_t(64) << 32) | 999);
  returned(R);
  EXPECT_EQ(word(R, 0), 0u);
  EXPECT_EQ(word(R, 1), 97u);
}

TEST_P(AndroidNetwork, TimeoutOptionsUseSharedKernelAndErrno) {
  for (uint64_t Raw : {0, 1}) {
    auto R = run("network_timeout", Raw);
    returned(R);
    EXPECT_EQ(word(R, 0), 0u);
    EXPECT_EQ(word(R, 1), 0u);
    EXPECT_EQ(word(R, 2), 16u);
    EXPECT_EQ(word(R, 3), 73u);
    EXPECT_EQ(word(R, 4), 20u);
    EXPECT_EQ(word(R, 5), 0u);
    EXPECT_EQ(word(R, 6), 0xa5a5a5a5a5a5a5a5ULL);
  }
}

TEST_P(AndroidNetwork, ResolverPreservesOrderLayoutNamesAndErrno) {
  auto R = run("network_resolve");
  returned(R);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(word(R, 0), 0u);
  EXPECT_EQ(word(R, 1), 73u);
  EXPECT_EQ(word(R, 3), 2u);
  EXPECT_EQ(word(R, 68), 73u);
  const auto &Bytes = R.MemorySnapshots[0].Bytes;
  for (unsigned I = 0; I != 2; ++I) {
    const unsigned Start = 4 + I * 16;
    EXPECT_EQ(word(R, Start), 2u);
    EXPECT_EQ(word(R, Start + 1), I ? 10u : 2u);
    EXPECT_EQ(word(R, Start + 2), 1u);
    EXPECT_EQ(word(R, Start + 3), 6u);
    const auto &Address = I ? IPv6 : IPv4;
    EXPECT_EQ(word(R, Start + 4), Address.size());
    EXPECT_TRUE(std::equal(Address.begin(), Address.end(),
                           Bytes.begin() + 8 * (Start + 5)));
    EXPECT_EQ(Bytes[8 * (Start + 5) + Address.size()], 0xa5);
  }
  const char Name[] = "canonical.invalid";
  EXPECT_TRUE(
      std::equal(std::begin(Name), std::end(Name), Bytes.begin() + 13 * 8));
  EXPECT_EQ(Bytes[13 * 8 + sizeof(Name)], 0xa5);
  EXPECT_EQ(word(R, 29), 0xa5a5a5a5a5a5a5a5ULL);
}
TEST_P(AndroidNetwork, ResolverErrorsPreserveNullOrderingAndSystemErrno) {
  for (auto [Mode, Error] :
       {std::pair(1u, 3u), {2u, 5u}, {3u, 12u}, {4u, 8u}}) {
    auto R = run("network_resolve", Mode);
    returned(R);
    EXPECT_EQ(word(R, 0), Error);
    EXPECT_EQ(word(R, 1), 73u);
    EXPECT_EQ(word(R, 2), Mode == 4 ? 0x1234u : 0u);
  }
  auto &Q = Options.Android->ResolverQueries->front();
  Q.Addresses.clear();
  for (unsigned Error : {2u, 8u, 11u}) {
    Q.Error = Error;
    Q.SystemErrno = Error == 11 ? 101 : 0;
    auto R = run("network_resolve");
    returned(R);
    EXPECT_EQ(word(R, 0), Error);
    EXPECT_EQ(word(R, 1), Error == 11 ? 101u : 73u);
    EXPECT_EQ(word(R, 2), 0u);
  }
}
TEST_P(AndroidNetwork, MissingQueryAndRepeatedFreeNeverInventResults) {
  auto R = run("network_resolve", 5);
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
  EXPECT_NE(R.Diagnostic.find("resolver-owned"), std::string::npos);
  Options.Android->ResolverQueries->front().Service = "9002";
  R = run("network_resolve");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("exact offline observation"), std::string::npos);
}
TEST_P(AndroidNetwork, StreamAliasesShortTransfersAndRawWrappersShareState) {
  for (unsigned Raw : {0, 1}) {
    auto R = run("network_stream", Raw);
    returned(R);
    const uint64_t Expected[] = {2, 2, 1, 2, 2, 0, 0, UINT64_MAX, 9};
    for (unsigned I = 0; I < std::size(Expected); ++I)
      EXPECT_EQ(word(R, I + 2), Expected[I]) << I;
    const std::vector<uint8_t> Guarded{0xa5, 0x31, 0,    0xfe,
                                       0x7f, 0xa5, 0xa5, 0xa5};
    const auto &Bytes = R.MemorySnapshots[0].Bytes;
    EXPECT_TRUE(
        std::equal(Guarded.begin(), Guarded.end(), Bytes.begin() + 128));
  }
}
TEST_P(AndroidNetwork, RefusalAndUnknownPeerStaySeparate) {
  auto &C = Options.LinuxNetwork->Connections.front();
  C.ConnectErrno = 111;
  C.ExpectedSend.clear();
  C.Receive.clear();
  C.ReceiveEOF = false;
  auto R = run("network_stream");
  returned(R);
  EXPECT_EQ(word(R, 0), UINT32_MAX);
  EXPECT_EQ(word(R, 1), 111u);
  C.Peer[3]++;
  R = run("network_stream");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("matching offline connection"),
            std::string::npos);
}
TEST_P(AndroidNetwork, OutgoingMismatchReportsStreamAndBufferOffsets) {
  Options.Android->ObserveMemoryOnFailure = true;
  auto &Bytes = Options.LinuxNetwork->Connections.front().ExpectedSend;
  for (unsigned Raw : {0, 1}) {
    for (unsigned Offset : {1, 3, 4}) {
      SCOPED_TRACE(testing::Message() << "raw=" << Raw << " offset=" << Offset);
      const auto Original = Bytes[Offset];
      Bytes[Offset] = 0;
      auto R = run("network_stream", Raw);
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(
          R.Diagnostic,
          Offset == 1
              ? "outgoing TCP bytes differ from the offline contract at "
                "stream offset 1 (buffer offset 1): expected 0x00, actual "
                "0xff (connection index 0)"
          : Offset == 3
              ? "outgoing TCP bytes differ from the offline contract at "
                "stream offset 3 (buffer offset 1): expected 0x00, actual "
                "0x13 (connection index 0)"
              : "outgoing TCP bytes differ from the offline contract at "
                "stream offset 4 (buffer offset 0): expected 0x00, actual "
                "0x7f (connection index 0)");
      if (Offset > 1)
        EXPECT_EQ(word(R, 2), 2u);
      if (Offset > 3)
        EXPECT_EQ(word(R, 3), 2u);
      Bytes[Offset] = Original;
    }
  }
}
TEST_P(AndroidNetwork, MismatchKeepsConnectionIdentityAcrossFDReuse) {
  Options.Android->ObserveMemoryOnFailure = true;
  auto Stream = Options.LinuxNetwork->Connections.front();
  Stream.ExpectedSend = {0, 0};
  auto Failure = Stream;
  Failure.ConnectErrno = 111;
  Failure.ExpectedSend.clear();
  Failure.Receive.clear();
  Failure.ReceiveEOF = false;
  Options.LinuxNetwork->Connections = {Failure, Stream, Stream};
  for (unsigned Mode : {0, 1, 2, 3}) {
    SCOPED_TRACE(Mode);
    auto R = run("network_stream_identity", Mode);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(word(R, 0), 1u);
    EXPECT_EQ(word(R, 1), 1u);
    EXPECT_EQ(R.Diagnostic,
              Mode < 2
                  ? "outgoing TCP bytes differ from the offline contract at "
                    "stream offset 1 (buffer offset 1): expected 0x00, actual "
                    "0xff (connection index 1)"
                  : "outgoing TCP bytes differ from the offline contract at "
                    "stream offset 1 (buffer offset 1): expected 0x00, actual "
                    "0xff (connection index 2)");
  }
}
TEST_P(AndroidNetwork, DynamicResolverUsesTheSameOwner) {
  Options.Android->Libraries["liboffline.so"] = {"getaddrinfo", "freeaddrinfo"};
  auto R = run("network_dynamic");
  returned(R);
  EXPECT_EQ(word(R, 0), 0u);
  EXPECT_EQ(word(R, 1), 2u);
}
INSTANTIATE_TEST_SUITE_P(
    Native, AndroidNetwork,
    testing::Combine(testing::Values("network-O0-none", "network-O0-android",
                                     "network-O0-relr", "network-O2-none",
                                     "network-O2-android", "network-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
