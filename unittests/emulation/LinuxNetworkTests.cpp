//===- LinuxNetworkTests.cpp - Shared offline descriptor contracts -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded offline network contracts and their semantic checks.

#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxNetwork.h"
#include "os/linux/kernel/LinuxServices.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
using linux_model::ServiceKind;
constexpr uint64_t Base = 0x20000000;
class LinuxNetwork : public testing::TestWithParam<GuestArchitecture> {
protected:
  std::optional<BackendSelection> Backend;
  ProcessOptions Options;
  ProcessResult Report{};
  linux_model::MemoryLayout Layout{0x800000000000ULL, 4096};
  std::unique_ptr<linux_model::LinuxServices> Kernel;
  ExecutionBackend &cpu() { return *Backend->CPU; }
  void SetUp() override {
    ExecutionConfiguration C;
    C.Backend = ExecutionBackendKind::Unicorn;
    C.Architecture = GetParam();
    C.Contract = GetParam() == GuestArchitecture::X64
                     ? ExecutionContract::CheckedUserX64
                     : ExecutionContract::CheckedUserAArch64;
    auto P = probeExecutionBackend(C);
    ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
    if (P->Availability != BackendAvailability::Available)
      GTEST_SKIP() << P->Reason;
    Backend.emplace(llvm::cantFail(createExecutionBackend(C, 4 * 1024 * 1024)));
    llvm::cantFail(
        cpu().addressSpace()->map(Base, 8192, Read | Write | UserAccessible));
    Options.LinuxFiles.emplace().DescriptorLimit = 6;
    LinuxStreamConnection C0;
    C0.Peer = {2, 0, 0x23, 0x29, 127, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    C0.ExpectedSend = {1, 2, 3};
    C0.Receive = {7, 8};
    C0.ReceiveEOF = true;
    C0.MaxSend = C0.MaxReceive = 2;
    Options.LinuxNetwork.emplace().Connections = {C0, C0};
    llvm::cantFail(cpu().write(Base, C0.Peer));
    Kernel = std::make_unique<linux_model::LinuxServices>(
        cpu(), Layout, Base + 8192, Options, Report);
  }
  std::optional<uint64_t> call(ServiceKind K, uint64_t A = 0, uint64_t B = 0,
                               uint64_t C = 0, uint64_t D = 0, uint64_t E = 0,
                               uint64_t F = 0) {
    return llvm::cantFail(Kernel->handle(K, {0, 0, {A, B, C, D, E, F}, {}}));
  }
  uint64_t open() { return call(ServiceKind::Socket, 2, 1, 6).value(); }
};
TEST_P(LinuxNetwork, TimeoutOptionsShareAliasesAndPreserveFailedWrites) {
  const auto FD = open();
  const auto Alias = call(ServiceKind::Dup, FD).value();
  uint8_t Value[16]{};
  auto Set = [&](uint32_t Name, int64_t Seconds, int64_t Micros) {
    llvm::support::endian::write64le(Value, Seconds);
    llvm::support::endian::write64le(Value + 8, Micros);
    llvm::cantFail(cpu().write(Base + 64, Value));
    return call(ServiceKind::SetSocketOption, FD, 1, Name, Base + 64, 16);
  };
  EXPECT_EQ(call(ServiceKind::SetSocketOption, 99, 1, 20, 1, 16), uint64_t(-9));
  EXPECT_EQ(call(ServiceKind::SetSocketOption, 1, 1, 20, 1, 16), uint64_t(-88));
  EXPECT_EQ(call(ServiceKind::SetSocketOption, FD, 1, 20, 1, 15),
            uint64_t(-22));
  EXPECT_EQ(call(ServiceKind::SetSocketOption, FD, 1, 20, 1, 16),
            uint64_t(-14));
  EXPECT_EQ(Set(20, 23, 0), 0u);
  EXPECT_EQ(Set(21, 11, 0), 0u);
  EXPECT_EQ(Set(20, 1, 1000000), uint64_t(-33));
  EXPECT_FALSE(Set(20, 1, 1));
  for (uint32_t Name : {20u, 21u})
    for (uint32_t Length : {0u, 7u, 16u, 24u}) {
      uint8_t Size[4];
      llvm::support::endian::write32le(Size, Length);
      llvm::cantFail(cpu().write(Base + 96, Size));
      std::vector<uint8_t> Guard(32, 0xa5);
      llvm::cantFail(cpu().write(Base + 128, Guard));
      EXPECT_EQ(call(ServiceKind::GetSocketOption, Alias, 1, Name, Base + 128,
                     Base + 96),
                0u);
      llvm::cantFail(cpu().read(Base + 96, Size));
      const auto Count = std::min(Length, 16u);
      EXPECT_EQ(llvm::support::endian::read32le(Size), Count);
      llvm::cantFail(cpu().read(Base + 128, Guard));
      uint8_t Expected[16]{};
      llvm::support::endian::write64le(Expected, Name == 20 ? 23 : 11);
      EXPECT_TRUE(std::equal(Expected, Expected + Count, Guard.begin()));
      EXPECT_TRUE(std::all_of(Guard.begin() + Count, Guard.end(),
                              [](uint8_t V) { return V == 0xa5; }));
    }
  EXPECT_EQ(call(ServiceKind::Close, FD), 0u);
  EXPECT_EQ(call(ServiceKind::Connect, Alias, Base, 16), 0u);
}

TEST_P(LinuxNetwork, FailedAdmissionDoesNotConsumeConnectionOrStream) {
  const auto FD = open();
  EXPECT_EQ(call(ServiceKind::Connect, 99, 1, 16), uint64_t(-9));
  EXPECT_EQ(call(ServiceKind::Connect, 1, 1, 16), uint64_t(-88));
  EXPECT_EQ(call(ServiceKind::Connect, FD, 1, 129), uint64_t(-22));
  EXPECT_EQ(call(ServiceKind::Connect, FD, 1, 16), uint64_t(-14));
  EXPECT_EQ(call(ServiceKind::Connect, FD, Base, 16), 0u);
  EXPECT_EQ(call(ServiceKind::Connect, FD, Base, 16), uint64_t(-106));
  const uint8_t Wrong[] = {0xff, 2, 3};
  llvm::cantFail(cpu().write(Base + 64, Wrong));
  EXPECT_FALSE(call(ServiceKind::SendTo, FD, Base + 64, 3));
  EXPECT_EQ(Report.Diagnostic,
            "outgoing TCP bytes differ from the offline contract at stream "
            "offset 0 (buffer offset 0): expected 0x01, actual 0xff "
            "(connection index 0)");
  const uint8_t Right[] = {1, 2, 3};
  llvm::cantFail(cpu().write(Base + 64, Right));
  EXPECT_EQ(call(ServiceKind::Write, FD, Base + 64, 3), 2u);
  EXPECT_EQ(call(ServiceKind::SendTo, FD, Base + 66, 1), 1u);
  EXPECT_FALSE(call(ServiceKind::SendTo, FD, Base + 64, 1));
  EXPECT_EQ(call(ServiceKind::SendTo, FD, UINT64_MAX, 0), 0u);
  EXPECT_FALSE(call(ServiceKind::ReceiveFrom, FD, 1, 2));
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 128, 4), 2u);
  uint8_t Read[2];
  llvm::cantFail(cpu().read(Base + 128, Read));
  EXPECT_EQ(Read[0], 7);
  EXPECT_EQ(Read[1], 8);
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, FD, 1, 2), 0u);
}
TEST_P(LinuxNetwork, MismatchOffsetsUseSharedCursorWithoutConsumingPrefix) {
  for (uint64_t Prefix : {1u, 2u}) {
    const auto FD = open();
    ASSERT_EQ(call(ServiceKind::Connect, FD, Base, 16), 0u);
    const auto Alias = call(ServiceKind::Dup, FD).value();
    const uint8_t Wrong[] = {1, 2, 0xff};
    llvm::cantFail(cpu().write(Base + 64, Wrong));
    // The second case also leaves a mismatching byte beyond MaxSend.
    ASSERT_EQ(call(ServiceKind::Write, FD, Base + 64, Prefix == 2 ? 3 : 1),
              Prefix);
    EXPECT_FALSE(
        call(ServiceKind::SendTo, Alias, Base + 64 + Prefix, 3 - Prefix));
    EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Report.Diagnostic,
              Prefix == 1
                  ? "outgoing TCP bytes differ from the offline contract at "
                    "stream offset 2 (buffer offset 1): expected 0x03, actual "
                    "0xff (connection index 0)"
                  : "outgoing TCP bytes differ from the offline contract at "
                    "stream offset 2 (buffer offset 0): expected 0x03, actual "
                    "0xff (connection index 1)");
    const uint8_t Right[] = {1, 2, 3};
    llvm::cantFail(cpu().write(Base + 64, Right));
    EXPECT_EQ(call(ServiceKind::Write, FD, Base + 64 + Prefix, 3 - Prefix),
              3 - Prefix);
    EXPECT_EQ(call(ServiceKind::Close, FD), 0u);
    EXPECT_EQ(call(ServiceKind::Close, Alias), 0u);
  }
}
TEST_P(LinuxNetwork, MismatchConnectionIdentitySurvivesAliasesAndFDReuse) {
  auto Failure = Options.LinuxNetwork->Connections.front();
  Failure.ConnectErrno = 111;
  Failure.ExpectedSend.clear();
  Failure.Receive.clear();
  Failure.ReceiveEOF = false;
  Options.LinuxNetwork->Connections.insert(
      Options.LinuxNetwork->Connections.begin(), Failure);
  const auto Failed = open();
  ASSERT_EQ(call(ServiceKind::Connect, Failed, Base, 16), uint64_t(-111));
  ASSERT_EQ(call(ServiceKind::Close, Failed), 0u);
  const auto First = open();
  ASSERT_EQ(First, Failed);
  auto WrongPeer = Failure.Peer;
  ++WrongPeer[3];
  llvm::cantFail(cpu().write(Base, WrongPeer));
  EXPECT_FALSE(call(ServiceKind::Connect, First, Base, 16));
  llvm::cantFail(cpu().write(Base, Failure.Peer));
  ASSERT_EQ(call(ServiceKind::Connect, First, Base, 16), 0u);
  EXPECT_EQ(call(ServiceKind::Connect, First, Base, 16), uint64_t(-106));
  const auto Alias = call(ServiceKind::Dup, First).value();
  ASSERT_EQ(call(ServiceKind::Close, First), 0u);
  const auto Reused = open();
  ASSERT_EQ(Reused, First);
  ASSERT_EQ(call(ServiceKind::Connect, Reused, Base, 16), 0u);
  const uint8_t Wrong[] = {1, 0xff, 3};
  llvm::cantFail(cpu().write(Base + 64, Wrong));
  EXPECT_FALSE(call(ServiceKind::Write, Alias, Base + 64, 3));
  EXPECT_EQ(Report.Diagnostic,
            "outgoing TCP bytes differ from the offline contract at stream "
            "offset 1 (buffer offset 1): expected 0x02, actual 0xff "
            "(connection index 1)");
  EXPECT_FALSE(call(ServiceKind::SendTo, Reused, Base + 64, 3));
  EXPECT_EQ(Report.Diagnostic,
            "outgoing TCP bytes differ from the offline contract at stream "
            "offset 1 (buffer offset 1): expected 0x02, actual 0xff "
            "(connection index 2)");
  ASSERT_EQ(call(ServiceKind::Dup3, Alias, Reused, 0), Reused);
  ASSERT_EQ(call(ServiceKind::Write, Reused, Base + 64, 1), 1u);
  EXPECT_FALSE(call(ServiceKind::SendTo, Alias, Base + 65, 2));
  EXPECT_EQ(Report.Diagnostic,
            "outgoing TCP bytes differ from the offline contract at stream "
            "offset 1 (buffer offset 0): expected 0x02, actual 0xff "
            "(connection index 1)");
  const uint8_t Right[] = {2, 3};
  llvm::cantFail(cpu().write(Base + 65, Right));
  EXPECT_EQ(call(ServiceKind::Write, Reused, Base + 65, 2), 2u);
}
TEST_P(LinuxNetwork, SharedCursorsReplacementAndFDReuseKeepOneOwner) {
  const auto FD = open(), Other = open();
  ASSERT_EQ(FD, 3u);
  ASSERT_EQ(Other, 4u);
  ASSERT_EQ(call(ServiceKind::Connect, FD, Base, 16), 0u);
  ASSERT_EQ(call(ServiceKind::Dup, FD), 5u);
  EXPECT_EQ(call(ServiceKind::Socket, 2, 1, 0), uint64_t(-24));
  EXPECT_EQ(call(ServiceKind::Dup3, FD, Other, 0x80000), Other);
  EXPECT_EQ(call(ServiceKind::Close, FD), 0u);
  EXPECT_EQ(call(ServiceKind::Close, 5), 0u);
  EXPECT_EQ(call(ServiceKind::Read, Other, Base + 128, 1), 1u);
  EXPECT_EQ(call(ServiceKind::Read, Other, Base + 129, 1), 1u);
  EXPECT_EQ(call(ServiceKind::Lseek, Other, 0, 0), uint64_t(-29));
  const auto Reused = open();
  EXPECT_EQ(Reused, FD);
  EXPECT_EQ(call(ServiceKind::Connect, Reused, Base, 16), 0u);
  EXPECT_EQ(call(ServiceKind::Read, Reused, Base + 130, 2), 2u);
  EXPECT_EQ(call(ServiceKind::Read, Other, Base + 132, 2), 0u);
}
TEST_P(LinuxNetwork,
       MissingInputsNonblockingAndUnknownReadinessRemainUnsupported) {
  Options.LinuxNetwork->Connections.front().ReceiveEOF = false;
  const auto FD = open();
  EXPECT_FALSE(call(ServiceKind::Write, FD, Base, 1));
  EXPECT_EQ(call(ServiceKind::Connect, FD, Base, 16), 0u);
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 128, 2), 2u);
  EXPECT_FALSE(call(ServiceKind::Read, FD, Base + 128, 2));
  const auto Nonblocking = call(ServiceKind::Socket, 2, 1 | 0x800, 0).value();
  EXPECT_FALSE(call(ServiceKind::Connect, Nonblocking, Base, 16));
  Options.LinuxNetwork.reset();
  EXPECT_FALSE(call(ServiceKind::Socket, 2, 1, 0));
}
TEST_P(LinuxNetwork, DatagramBoundariesPeekTruncationAndAliasesAreShared) {
  const auto Peer = Options.LinuxNetwork->Connections[0].Peer;
  LinuxDatagramConnection D;
  D.Peer = Peer;
  D.ExpectedSend = {{1, 2, 3}, {}};
  D.ReceiveEvents = {{{7, 8, 9}, 0, 1}, {{}, 0, 2}, {{10}, 0, 2}};
  Options.LinuxNetwork->DatagramConnections = {D, D};
  const auto FD = call(ServiceKind::Socket, 2, 2, 17).value();
  ASSERT_EQ(call(ServiceKind::Connect, FD, Base, 16), 0u);
  EXPECT_FALSE(call(ServiceKind::ReceiveFrom, FD, Base + 128, 3));
  const uint8_t Wrong[] = {1, 2, 4}, Right[] = {1, 2, 3};
  llvm::cantFail(cpu().write(Base + 64, Wrong));
  EXPECT_FALSE(call(ServiceKind::SendTo, FD, Base + 64, 3));
  EXPECT_FALSE(call(ServiceKind::SendTo, FD, Base + 64, 2));
  llvm::cantFail(cpu().write(Base + 64, Right));
  EXPECT_EQ(call(ServiceKind::Write, FD, Base + 64, 3), 3u);
  const auto Alias = call(ServiceKind::Dup, FD).value();
  EXPECT_EQ(call(ServiceKind::Close, FD), 0u);
  uint8_t Length[4];
  llvm::support::endian::write32le(Length, 3);
  llvm::cantFail(cpu().write(Base + 96, Length));
  std::vector<uint8_t> Guard(32, 0xa5);
  llvm::cantFail(cpu().write(Base + 160, Guard));
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, Alias, Base + 128, 2, 2 | 0x20,
                 Base + 160, Base + 96),
            3u);
  llvm::cantFail(cpu().read(Base + 96, Length));
  EXPECT_EQ(llvm::support::endian::read32le(Length), 16u);
  llvm::cantFail(cpu().read(Base + 160, Guard));
  EXPECT_TRUE(std::equal(Peer.begin(), Peer.begin() + 3, Guard.begin()));
  EXPECT_TRUE(std::all_of(Guard.begin() + 3, Guard.end(),
                          [](uint8_t V) { return V == 0xa5; }));
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, Alias, Base + 128, 1), 1u);
  EXPECT_FALSE(call(ServiceKind::ReceiveFrom, Alias, Base + 128, 3));
  EXPECT_EQ(call(ServiceKind::SendTo, Alias, UINT64_MAX, 0), 0u);
  // read(0) leaves the empty datagram queued; recv(0) consumes it.
  EXPECT_EQ(call(ServiceKind::Read, Alias, 1, 0), 0u);
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, Alias, 1, 0, 2), 0u);
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, Alias, 1, 0), 0u);
  EXPECT_EQ(call(ServiceKind::Read, Alias, Base + 128, 3), 1u);
  uint8_t Last;
  llvm::cantFail(cpu().read(Base + 128, llvm::MutableArrayRef(&Last, 1)));
  EXPECT_EQ(Last, 10);
  EXPECT_FALSE(call(ServiceKind::Read, Alias, Base + 128, 3));
  EXPECT_EQ(call(ServiceKind::Socket, 2, 2, 0), FD);
  EXPECT_EQ(call(ServiceKind::Connect, FD, Base, 16), 0u);
  EXPECT_FALSE(call(ServiceKind::Read, FD, Base + 128, 3));
}

TEST_P(LinuxNetwork,
       DatagramFaultAdmissionAndExplicitErrorsDoNotInventPackets) {
  LinuxDatagramConnection D;
  D.Peer = Options.LinuxNetwork->Connections[0].Peer;
  D.ReceiveEvents = {{{}, 4, 0}, {{}, 11, 0}, {{1, 2}, 0, 0}};
  Options.LinuxNetwork->DatagramConnections = {D};
  const auto FD = call(ServiceKind::Socket, 2, 2, 0).value();
  EXPECT_EQ(call(ServiceKind::Connect, FD, 1, 16), uint64_t(-14));
  EXPECT_EQ(call(ServiceKind::Connect, FD, Base, 16), 0u);
  EXPECT_FALSE(call(ServiceKind::Connect, FD, Base, 16));
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, FD, 1, 2), uint64_t(-4));
  EXPECT_FALSE(call(ServiceKind::ReceiveFrom, FD, 1, 2));
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, FD, 1, 2, 0x40), uint64_t(-11));
  EXPECT_FALSE(call(ServiceKind::ReceiveFrom, FD, 1, 2));
  EXPECT_FALSE(
      call(ServiceKind::ReceiveFrom, FD, Base + 128, 2, 0, Base + 160, 1));
  uint8_t Length[4];
  llvm::support::endian::write32le(Length, 16);
  llvm::cantFail(cpu().write(Base + 128, Length));
  EXPECT_FALSE(call(ServiceKind::ReceiveFrom, FD, Base + 128, 2, 0, Base + 160,
                    Base + 128));
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, FD, Base + 128, 1), 1u);
  // The suffix of a truncated datagram is gone, not a second stream read.
  EXPECT_FALSE(call(ServiceKind::ReceiveFrom, FD, Base + 128, 2));
}

TEST_P(LinuxNetwork, DatagramRouteErrorsAndInterruptRetryHaveNoHandshake) {
  LinuxDatagramConnection D;
  D.Peer = Options.LinuxNetwork->Connections[0].Peer;
  D.ConnectErrno = 4;
  auto Success = D;
  Success.ConnectErrno = 0;
  auto Failure = D;
  Failure.ConnectErrno = 101;
  Options.LinuxNetwork->DatagramConnections = {D, Success, Failure};
  const auto FD = call(ServiceKind::Socket, 2, 2 | 0x800, 17).value();
  EXPECT_EQ(call(ServiceKind::Connect, FD, Base, 16), uint64_t(-4));
  EXPECT_EQ(call(ServiceKind::Connect, FD, Base, 16), 0u);
  EXPECT_FALSE(call(ServiceKind::ReceiveFrom, FD, Base, 2, 0x40));
  EXPECT_EQ(call(ServiceKind::Close, FD), 0u);
  EXPECT_EQ(call(ServiceKind::Socket, 2, 2, 17), FD);
  EXPECT_EQ(call(ServiceKind::Connect, FD, Base, 16), uint64_t(-101));
  EXPECT_FALSE(call(ServiceKind::Connect, FD, Base, 16));
}

TEST_P(LinuxNetwork, StreamEventsKeepOrderingPartialReadsErrorsAndExplicitEOF) {
  auto &C = Options.LinuxNetwork->Connections.front();
  C.Receive.clear();
  C.ReceiveEvents = {{{}, 4, 0}, {{7, 8, 9}, 0, 3}, {{}, 11, 3}, {{10}, 0, 3}};
  const auto FD = open();
  EXPECT_EQ(call(ServiceKind::Connect, FD, Base, 16), 0u);
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 128, 3), uint64_t(-4));
  EXPECT_FALSE(call(ServiceKind::Read, FD, Base + 128, 3));
  const uint8_t Send[] = {1, 2, 3};
  llvm::cantFail(cpu().write(Base + 64, Send));
  EXPECT_EQ(call(ServiceKind::Write, FD, Base + 64, 3), 2u);
  EXPECT_FALSE(call(ServiceKind::Read, FD, Base + 128, 3));
  EXPECT_EQ(call(ServiceKind::Write, FD, Base + 66, 1), 1u);
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 128, 3), 2u);
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 130, 3), 1u);
  EXPECT_FALSE(call(ServiceKind::Read, FD, Base + 131, 1));
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, FD, Base + 131, 1, 0x40),
            uint64_t(-11));
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 131, 2), 1u);
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 132, 1), 0u);
  uint8_t Read[4];
  llvm::cantFail(cpu().read(Base + 128, Read));
  EXPECT_EQ(std::vector(Read, Read + 4), (std::vector<uint8_t>{7, 8, 9, 10}));
}

TEST_P(LinuxNetwork, TCPConnectFailuresPreserveAdmissionAndBoundedOwnership) {
  Options.LinuxNetwork->Connections.resize(1);
  Options.LinuxNetwork->TCPConnectFailures = LinuxTCPConnectFailures{101, 2};
  auto Other = Options.LinuxNetwork->Connections[0].Peer;
  ++Other[3];
  llvm::cantFail(cpu().write(Base + 32, Other));
  auto FD = open();
  // A present exact attempt cannot be bypassed by the failure-only tail.
  EXPECT_FALSE(call(ServiceKind::Connect, FD, Base + 32, 16));
  EXPECT_EQ(call(ServiceKind::Connect, FD, Base, 16), 0u);
  EXPECT_EQ(call(ServiceKind::Close, FD), 0u);
  FD = open();
  EXPECT_EQ(call(ServiceKind::Connect, 99, 1, 16), uint64_t(-9));
  EXPECT_EQ(call(ServiceKind::Connect, 1, 1, 16), uint64_t(-88));
  EXPECT_EQ(call(ServiceKind::Connect, FD, 1, 129), uint64_t(-22));
  EXPECT_EQ(call(ServiceKind::Connect, FD, 1, 16), uint64_t(-14));
  EXPECT_EQ(call(ServiceKind::Connect, FD, Base + 8191, 16), uint64_t(-14));
  Other[0] = 10;
  llvm::cantFail(cpu().write(Base + 32, Other));
  EXPECT_FALSE(call(ServiceKind::Connect, FD, Base + 32, 16));
  Other[0] = 2;
  llvm::cantFail(cpu().write(Base + 32, Other));
  auto Nonblocking = call(ServiceKind::Socket, 2, 1 | 0x800, 0).value();
  EXPECT_FALSE(call(ServiceKind::Connect, Nonblocking, Base, 16));
  EXPECT_EQ(call(ServiceKind::Close, Nonblocking), 0u);
  auto UDP = call(ServiceKind::Socket, 2, 2, 17).value();
  EXPECT_FALSE(call(ServiceKind::Connect, UDP, Base, 16));
  EXPECT_EQ(call(ServiceKind::Close, UDP), 0u);
  EXPECT_EQ(call(ServiceKind::Connect, FD, Base + 32, 16), uint64_t(-101));
  const auto Alias = call(ServiceKind::Dup, FD).value();
  EXPECT_EQ(call(ServiceKind::Close, FD), 0u);
  EXPECT_FALSE(call(ServiceKind::Connect, Alias, Base, 16));
  EXPECT_FALSE(call(ServiceKind::Read, Alias, Base + 128, 1));
  EXPECT_EQ(call(ServiceKind::Close, Alias), 0u);
  std::vector<uint8_t> IPv6(28, 0);
  IPv6[0] = 10;
  IPv6[23] = 1;
  llvm::cantFail(cpu().write(Base + 64, IPv6));
  FD = call(ServiceKind::Socket, 10, 1, 6).value();
  EXPECT_EQ(call(ServiceKind::Connect, FD, Base + 64, 28), uint64_t(-101));
  EXPECT_EQ(call(ServiceKind::Close, FD), 0u);
  FD = open();
  EXPECT_FALSE(call(ServiceKind::Connect, FD, Base, 16));
  EXPECT_NE(Report.Diagnostic.find("failure contract exhausted"),
            std::string::npos);
}

TEST_P(LinuxNetwork, TCPConnectFailuresReachConfiguredBoundThenStop) {
  Options.LinuxNetwork->Connections.clear();
  // A counted failure retains no connection inventory. Exercise more failures
  // than the stored TCP/UDP entry limit while repeatedly reusing one FD.
  constexpr uint32_t Count = 4097;
  Options.LinuxNetwork->TCPConnectFailures =
      LinuxTCPConnectFailures{101, Count};
  ASSERT_EQ(llvm::toString(
                linux_model::validateNetworkOptions(*Options.LinuxNetwork)),
            "");
  for (unsigned I = 0; I != Count; ++I) {
    SCOPED_TRACE(I);
    const auto FD = open();
    ASSERT_EQ(call(ServiceKind::Connect, FD, Base, 16), uint64_t(-101));
    ASSERT_EQ(call(ServiceKind::Close, FD), 0u);
  }
  const auto FD = open();
  EXPECT_FALSE(call(ServiceKind::Connect, FD, Base, 16));
  EXPECT_NE(Report.Diagnostic.find("failure contract exhausted"),
            std::string::npos);
}

TEST(LinuxNetworkJSON,
     TCPConnectFailuresRequireFiniteExplicitNegativeOutcomes) {
  auto Check = [](llvm::StringRef Fields, bool Valid) {
    auto P = processOptionsFromJSON(
        "{\"linux_network\":{\"tcp_connect_failures\":" + Fields.str() + "}}");
    EXPECT_EQ(bool(P), Valid) << Fields.str();
    if (P && Valid) {
      ASSERT_TRUE(P->LinuxNetwork->TCPConnectFailures);
      EXPECT_GT(P->LinuxNetwork->TCPConnectFailures->Count, 0u);
    }
    if (!P)
      llvm::consumeError(P.takeError());
  };
  for (unsigned Error : {4, 111, 110, 101, 113, 13})
    Check("{\"errno\":" + std::to_string(Error) +
              ",\"count\":" + std::to_string(4096) + "}",
          true);
  Check(R"({"errno":101,"count":4097})", true);
  Check(R"({"errno":101,"count":4294967295})", true);
  Check(R"({"errno":101,"count":"4294967295"})", true);
  // A long offline worker must be able to pass the former 256-attempt bound.
  Check(R"({"errno":101,"count":257})", true);
  for (const char *Fields : {"null",
                             "[]",
                             "{}",
                             R"({"errno":101})",
                             R"({"count":1})",
                             R"({"errno":0,"count":1})",
                             R"({"errno":115,"count":1})",
                             R"({"errno":101,"count":0})",
                             R"({"errno":101,"count":-1})",
                             R"({"errno":101,"count":1.5})",
                             R"({"errno":101,"count":null})",
                             R"({"errno":101,"count":true})",
                             R"({"errno":101,"count":"1x"})",
                             R"({"errno":101,"count":"-1"})",
                             R"({"errno":101,"count":"0"})",
                             R"({"errno":101,"count":"4294967296"})",
                             R"({"errno":101,"count":4294967296})",
                             R"({"errno":101,"count":1,"peer_hex":""})",
                             R"({"errno":101,"count":1,"receive_hex":"01"})",
                             R"({"errno":-1,"count":1})",
                             R"({"errno":true,"count":1})"})
    Check(Fields, false);
  // Observation integers also accept lossless decimal strings.
  Check(R"({"errno":101,"count":"1"})", true);
  Check("{\"errno\":\"101\",\"count\":\"" + std::to_string(4096) + "\"}", true);
  LinuxNetworkOptions N;
  LinuxStreamConnection S;
  S.Peer = {2, 0, 0x23, 0x29, 127, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  S.ConnectErrno = 111;
  N.Connections.resize(4096 - 2, S);
  LinuxDatagramConnection D;
  D.Peer = S.Peer;
  N.DatagramConnections = {D};
  for (auto F : {LinuxTCPConnectFailures{101, 1},
                 {101, 2},
                 {0, 1},
                 {115, 1},
                 {101, 0},
                 {101, UINT32_MAX}}) {
    N.TCPConnectFailures = F;
    auto E = linux_model::validateNetworkOptions(N);
    EXPECT_EQ(bool(E), F.Error != 101 || F.Count == 0);
    llvm::consumeError(std::move(E));
  }
  // The stored entry limit remains shared by TCP and UDP, independently of
  // the finite failure counter.
  N.TCPConnectFailures = LinuxTCPConnectFailures{101, UINT32_MAX};
  N.Connections.push_back(S);
  EXPECT_EQ(llvm::toString(linux_model::validateNetworkOptions(N)), "");
  N.DatagramConnections.push_back(D);
  auto TooMany = linux_model::validateNetworkOptions(N);
  EXPECT_TRUE(bool(TooMany));
  llvm::consumeError(std::move(TooMany));
  const std::string Peer =
      R"({"peer_hex":"020023297f0000010000000000000000","connect_errno":111})";
  // A small explicit list keeps this count-independence test below the
  // independently enforced process-options JSON metadata budget.
  const std::string Entries = Peer;
  for (uint32_t Count : {4095u, 4096u, 4097u, UINT32_MAX}) {
    auto P = processOptionsFromJSON(
        "{\"linux_network\":{\"connections\":[" + Entries +
        "],\"tcp_connect_failures\":{\"errno\":101,\"count\":" +
        std::to_string(Count) + "}}}");
    EXPECT_TRUE(bool(P));
    if (!P)
      llvm::consumeError(P.takeError());
  }
}

INSTANTIATE_TEST_SUITE_P(Raw, LinuxNetwork,
                         testing::Values(GuestArchitecture::X64,
                                         GuestArchitecture::AArch64));

TEST(LinuxNetworkJSON, StrictWireAndSemanticAdmission) {
  auto Check = [](llvm::StringRef Text, bool Valid) {
    auto P = processOptionsFromJSON(Text);
    EXPECT_EQ(bool(P), Valid);
    if (!P)
      llvm::consumeError(P.takeError());
  };
  Check(R"({"linux_network":{"connections":[]}})", true);
  Check(R"({"linux_network":{"datagram_connections":[]}})", true);
  Check(R"({"linux_network":{"datagram_connections":null}})", false);
  Check(R"({"linux_network":{"connections":[],"unknown":0}})", false);
  Check(
      R"({"linux_network":{"connections":[{"peer_hex":"020023297f0000010000000000000000","connect_errno":111}]}})",
      true);
  Check(
      R"({"linux_network":{"connections":[{"peer_hex":"020023297f0000010000000000000000","connect_errno":115}]}})",
      false);
  Check(
      R"({"linux_network":{"connections":[{"peer_hex":"020023297f0000010000000000000000","max_send":0}]}})",
      false);
  Check(R"({"linux_network":{"connections":[{"peer_hex":"0z"}]}})", false);
  const std::string Peer = "\"peer_hex\":\"020023297f0000010000000000000000\"";
  auto Event = [&](llvm::StringRef Fields, bool Datagram, bool Valid) {
    Check("{\"linux_network\":{\"" +
              std::string(Datagram ? "datagram_connections" : "connections") +
              "\":[{" + Peer + "," + Fields.str() + "}]}}",
          Valid);
  };
  Event(R"("receive_events":[{"receive_hex":""}])", true, true);
  Event(R"("receive_events":[{"receive_hex":""}])", false, false);
  Event(R"("receive_events":[{"errno":4}])", false, true);
  Event(R"("receive_events":[{"errno":0}])", true, false);
  Event(R"("receive_events":[{"errno":4,"receive_hex":""}])", true, false);
  Event(R"("receive_events":[{}])", true, false);
  Event(R"("receive_events":[{"errno":5}])", true, false);
  Event(R"("receive_events":[{"errno":104}])", false, false);
  Event(R"("receive_events":[{"errno":4,"after_send":-1}])", true, false);
  Event(R"("receive_events":[{"errno":4,"after_send":1}])", true, false);
  Event(
      R"("expected_datagrams":[""],"receive_events":[{"receive_hex":"01","after_send":1}])",
      true, true);
  Event(
      R"("expected_datagrams":["00"],"receive_events":[{"errno":4,"after_send":1},{"errno":4}])",
      true, false);
  Event(R"("connect_errno":101,"expected_datagrams":[""])", true, false);
  Event(R"("receive_eof":true)", true, false);
  Event(
      R"("expected_send_hex":"00","receive_events":[{"receive_hex":"01","after_send":1}],"receive_eof":true)",
      false, true);
  Event(R"("receive_hex":"","receive_events":[])", false, false);
  Event(R"("receive_events":[{"errno":4,"extra":0}])", false, false);

  Event(R"("receive_events":[{"receive_hex":"01","ready_at_ns":7}])", false,
        true);
  Event(R"("receive_events":[{"receive_hex":"","ready_at_ns":0}])", true, true);
  Event(R"("receive_events":[{"receive_hex":"01","ready_at_ns":-1}])", false,
        false);
  Event(R"("receive_events":[{"receive_hex":"01","ready_at_ns":null}])", false,
        false);
  Event(R"("receive_events":[{"errno":4,"ready_at_ns":7}])", false, false);
  Event(
      R"("receive_events":[{"receive_hex":"01","ready_at_ns":8},{"receive_hex":"02","ready_at_ns":7}])",
      false, false);
  Event(R"("receive_eof_at_ns":7)", false, false);
  Event(R"("receive_eof":true,"receive_eof_at_ns":7)", false, true);
  Event(R"("receive_eof":true,"receive_quiet_until_ns":7)", false, false);
  Event(
      R"("receive_events":[{"receive_hex":"01","ready_at_ns":8}],"receive_quiet_until_ns":7)",
      false, false);
  Event(
      R"("receive_events":[{"receive_hex":"01","ready_at_ns":8}],"receive_quiet_until_ns":10)",
      false, true);
  Event(R"("receive_quiet_until_ns":9223372036854775807)", true, false);
  Event(R"("receive_quiet_until_ns":18446744073709551615)", false, false);
  Event(R"("connect_errno":101,"receive_quiet_until_ns":7)", true, false);
  // C++ admission shares byte and event limits with JSON callers.
  LinuxNetworkOptions N;
  LinuxDatagramConnection D;
  D.Peer = {2, 0, 0x23, 0x29, 127, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  D.ExpectedSend = {std::vector<uint8_t>(65508)};
  N.DatagramConnections = {D};
  auto E = linux_model::validateNetworkOptions(N);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  Check(
      R"({"android":{"resolver_queries":[{"node":null,"service":"9001","error":8}]}})",
      true);
  Check(
      R"({"android":{"resolver_queries":[{"node":"a","service":"1","error":11}]}})",
      false);
  Check(
      R"({"android":{"resolver_queries":[{"node":"a","service":"1","error":8,"extra":0}]}})",
      false);
  Check(
      R"({"android":{"resolver_queries":[{"node":"a","service":"1","family":10,"addresses":[{"sockaddr_hex":"020023297f0000010000000000000000"}]}]}})",
      false);
  Check(
      R"({"android":{"resolver_queries":[{"node":"a","service":"1","error":8},{"node":"a","service":"1","error":8}]}})",
      false);
}
} // namespace
} // namespace neverd::emulation
