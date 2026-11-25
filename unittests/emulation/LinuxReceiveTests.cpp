//===- LinuxReceiveTests.cpp - Blocking socket lifetime and deadlines -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent byte, time and retained-description oracles for direct receives.

#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxServices.h"

#include "neverd/emulation/ExecutionConfiguration.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
using linux_model::ServiceKind;
constexpr uint64_t Base = 0x20000000, Second = 1000000000;
class LinuxReceive : public testing::TestWithParam<GuestArchitecture> {
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
    auto P = llvm::cantFail(probeExecutionBackend(C));
    if (P.Availability != BackendAvailability::Available)
      GTEST_SKIP() << P.Reason;
    Backend.emplace(llvm::cantFail(createExecutionBackend(C, 4 * 1024 * 1024)));
    llvm::cantFail(
        cpu().addressSpace()->map(Base, 8192, Read | Write | UserAccessible));
    Options.LinuxFiles.emplace();
    Options.LinuxTime.emplace().AdvanceOnIdle = true;
    Options.LinuxTime->Clocks[1] = {100, 0};
    LinuxStreamConnection S;
    S.Peer = {2, 0, 0x23, 0x29, 127, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    S.ReceiveEvents = {{{7, 8}, 0, 0, 7}, {{9}, 0, 0, 11}};
    S.ReceiveEOF = true;
    S.ReceiveEOFAtNS = 13;
    Options.LinuxNetwork.emplace().Connections = {S, S};
    Options.LinuxNetwork->Connections[1].ReceiveEvents[0].Bytes = {0xb6};
    LinuxDatagramConnection D;
    D.Peer = S.Peer;
    D.ReceiveEvents = {{{}, 0, 0, 7}, {{5, 6, 7}, 0, 0, 11}};
    Options.LinuxNetwork->DatagramConnections = {D};
    llvm::cantFail(cpu().write(Base, S.Peer));
    Kernel = std::make_unique<linux_model::LinuxServices>(
        cpu(), Layout, Base + 8192, Options, Report);
  }
  std::optional<uint64_t> call(ServiceKind K, uint64_t A = 0, uint64_t B = 0,
                               uint64_t C = 0, uint64_t D = 0, uint64_t E = 0,
                               uint64_t F = 0) {
    return llvm::cantFail(Kernel->handle(K, {0, 0, {A, B, C, D, E, F}, {}}));
  }
  void word(uint64_t Address, uint64_t Value) {
    uint8_t Bytes[8];
    llvm::support::endian::write64le(Bytes, Value);
    llvm::cantFail(cpu().write(Address, Bytes));
  }
  uint64_t word(uint64_t Address) {
    uint8_t Bytes[8];
    llvm::cantFail(cpu().read(Address, Bytes));
    return llvm::support::endian::read64le(Bytes);
  }
  uint64_t open(uint32_t Type = 1) {
    auto FD = call(ServiceKind::Socket, 2, Type, 0).value();
    EXPECT_EQ(call(ServiceKind::Connect, FD, Base, 16), 0u);
    return FD;
  }
  void timeout(uint64_t FD, int64_t Seconds) {
    word(Base + 64, Seconds);
    word(Base + 72, 0);
    ASSERT_EQ(call(ServiceKind::SetSocketOption, FD, 1, 20, Base + 64, 16), 0u);
  }
  std::unique_ptr<linux_model::KernelWait>
  pending(uint64_t FD, uint64_t Address = Base + 128, uint64_t Size = 1,
          uint32_t Flags = 0, uint64_t Peer = 0, uint64_t Length = 0) {
    linux_model::ThreadContext T;
    EXPECT_FALSE(llvm::cantFail(Kernel->handle(
        ServiceKind::ReceiveFrom,
        {0, 0, {FD, Address, Size, Flags, Peer, Length}, {}}, &T)));
    return std::move(T.Pending);
  }
};

TEST_P(LinuxReceive, ArrivalPartialReadsAndDelayedEOFShareOneClock) {
  const auto FD = open();
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 128, 1), 1u);
  EXPECT_EQ(Kernel->clock().elapsed(), 7u);
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, FD, Base + 129, 8), 1u);
  EXPECT_EQ(Kernel->clock().elapsed(), 7u);
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 130, 8), 1u);
  EXPECT_EQ(Kernel->clock().elapsed(), 11u);
  EXPECT_EQ(word(Base + 128), 0x090807u);
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, FD, 1, 1), 0u);
  EXPECT_EQ(Kernel->clock().elapsed(), 13u);
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 128, 1), 0u);
}

TEST_P(LinuxReceive, TimeoutDoesNotConsumeLaterArrivalAndExactTieReceives) {
  for (unsigned I = 0; I != 2; ++I) {
    Options.LinuxNetwork->Connections[I].ReceiveEvents[0].ReadyAtNS =
        2 * Second + I;
    Options.LinuxNetwork->Connections[I].ReceiveEvents.resize(1);
    Options.LinuxNetwork->Connections[I].ReceiveEOF = false;
    Options.LinuxNetwork->Connections[I].ReceiveEOFAtNS.reset();
  }
  const auto First = open(), Later = open();
  timeout(First, 2);
  timeout(Later, 2);
  auto A = pending(First), B = pending(Later, Base + 136);
  ASSERT_TRUE(A);
  ASSERT_TRUE(B);
  EXPECT_EQ(A->nextWake(), 2 * Second);
  EXPECT_EQ(B->nextWake(), 2 * Second);
  ASSERT_TRUE(Kernel->clock().advanceTo(2 * Second, Report));
  EXPECT_EQ(llvm::cantFail(A->resume(Report)), 1u);
  EXPECT_EQ(llvm::cantFail(B->resume(Report)), uint64_t(-11));
  EXPECT_EQ(word(Base + 128), 7u);
  EXPECT_EQ(word(Base + 136), 0u);
  EXPECT_EQ(call(ServiceKind::Read, Later, Base + 136, 1), 1u);
  EXPECT_EQ(Kernel->clock().elapsed(), 2 * Second + 1);
  EXPECT_EQ(word(Base + 136), 0xb6u);
}

TEST_P(LinuxReceive, QuietContractMustCoverTheTimeoutInclusive) {
  for (auto &C : Options.LinuxNetwork->Connections) {
    C.ReceiveEvents.clear();
    C.ReceiveEOF = false;
    C.ReceiveEOFAtNS.reset();
    C.ReceiveQuietUntilNS = 2 * Second;
  }
  const auto FD = open();
  timeout(FD, 2);
  EXPECT_EQ(call(ServiceKind::Read, FD, 1, 1), uint64_t(-11));
  EXPECT_EQ(Kernel->clock().elapsed(), 2 * Second);
  // Next call cannot turn unknown future input into another timeout.
  EXPECT_FALSE(call(ServiceKind::Read, FD, Base + 128, 1));
  EXPECT_EQ(Kernel->clock().elapsed(), 2 * Second + 1);
  EXPECT_EQ(word(Base + 128), 0u);
  EXPECT_EQ(Report.Diagnostic, "socket readiness has no offline observation "
                               "(TCP connection index 0, read readiness), "
                               "receive quiet through 2000000000 ns elapsed");

  auto &Datagram = Options.LinuxNetwork->DatagramConnections.front();
  Datagram.ReceiveEvents.clear();
  Datagram.ReceiveQuietUntilNS = 0;
  const auto UDP = open(2);
  EXPECT_FALSE(call(ServiceKind::ReceiveFrom, UDP, Base + 128, 1, 0x40));
  EXPECT_EQ(Report.Diagnostic, "socket readiness has no offline observation, "
                               "receive quiet through 0 ns elapsed");
}

TEST_P(LinuxReceive, NonblockingAndImmediateTimeoutNeverAdvanceOrConsume) {
  const auto FD = open();
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, FD, Base + 128, 1, 0x40),
            uint64_t(-11));
  timeout(FD, -1);
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 128, 1), uint64_t(-11));
  EXPECT_EQ(call(ServiceKind::Read, FD, 1, 0), 0u);
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, FD, 1, 0), 0u);
  const auto UDP = open(2 | 0x800);
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, UDP, Base + 128, 0), uint64_t(-11));
  EXPECT_EQ(Kernel->clock().elapsed(), 0u);
  ASSERT_TRUE(Kernel->clock().advanceTo(7, Report));
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 128, 1), 1u);
  EXPECT_EQ(word(Base + 128), 7u);
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, UDP, 1, 0), 0u);
}

TEST_P(LinuxReceive, CloseAndReusedFDDoNotRetargetRetainedReceive) {
  const auto FD = open();
  auto P = pending(FD);
  ASSERT_TRUE(P);
  ASSERT_EQ(call(ServiceKind::Close, FD), 0u);
  const auto Reused = open();
  ASSERT_EQ(Reused, FD);
  ASSERT_TRUE(Kernel->clock().advanceTo(7, Report));
  EXPECT_TRUE(P->ready());
  EXPECT_EQ(llvm::cantFail(P->resume(Report)), 1u);
  EXPECT_EQ(call(ServiceKind::Read, Reused, Base + 136, 1), 1u);
  EXPECT_EQ(word(Base + 128), 7u);
  EXPECT_EQ(word(Base + 136), 0xb6u);
}

TEST_P(LinuxReceive, AliasConsumptionRearmsWithoutExtendingCopiedTimeout) {
  auto &C = Options.LinuxNetwork->Connections[0];
  C.ReceiveEvents = {{{1}, 0, 0, Second}, {{2}, 0, 0, 3 * Second}};
  C.ReceiveEOF = false;
  C.ReceiveEOFAtNS.reset();
  const auto FD = open();
  const auto Alias = call(ServiceKind::Dup, FD).value();
  timeout(FD, 2);
  auto P = pending(FD);
  ASSERT_TRUE(P);
  timeout(Alias, 0); // The in-flight call retains its original deadline.
  ASSERT_TRUE(Kernel->clock().advanceTo(Second, Report));
  EXPECT_TRUE(P->ready());
  EXPECT_EQ(call(ServiceKind::Read, Alias, Base + 136, 1), 1u);
  EXPECT_FALSE(P->ready());
  EXPECT_EQ(P->nextWake(), 2 * Second);
  EXPECT_FALSE(
      llvm::cantFail(P->resume(Report))); // Premature completion refused.
  EXPECT_EQ(word(Base + 128), 0u);
  ASSERT_TRUE(Kernel->clock().advanceTo(2 * Second, Report));
  EXPECT_EQ(llvm::cantFail(P->resume(Report)), uint64_t(-11));
  EXPECT_EQ(call(ServiceKind::Read, Alias, Base + 128, 1), 1u);
  EXPECT_EQ(Kernel->clock().elapsed(), 3 * Second);
  EXPECT_EQ(word(Base + 128), 2u);
}

TEST_P(LinuxReceive, ResumeRechecksBufferAndKeepsUncopiedBytes) {
  const auto FD = open();
  auto P = pending(FD, Base + 4095, 2);
  ASSERT_TRUE(P);
  llvm::cantFail(
      cpu().addressSpace()->protect(Base + 4096, 4096, Read | UserAccessible));
  ASSERT_TRUE(Kernel->clock().advanceTo(7, Report));
  EXPECT_FALSE(llvm::cantFail(P->resume(Report)));
  EXPECT_NE(Report.Diagnostic.find("completely accessible"), std::string::npos);
  EXPECT_EQ(word(Base + 4092), 0u);
  // The model refused partial-copy semantics without publishing a prefix.
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 128, 2), 2u);
  EXPECT_EQ(word(Base + 128), 0x0807u);
}

TEST_P(LinuxReceive, ExplicitCompletionAfterCompetitionUsesCapturedTimeout) {
  auto &C = Options.LinuxNetwork->Connections[0];
  C.ReceiveEvents = {{{1}, 0, 0, 7}, {{}, 11, 0}};
  C.ReceiveEOF = false;
  C.ReceiveEOFAtNS.reset();
  const auto FD = open();
  const auto Alias = call(ServiceKind::Dup, FD).value();
  timeout(FD, 2);
  auto P = pending(FD);
  ASSERT_TRUE(P);
  timeout(Alias, 0);
  ASSERT_TRUE(Kernel->clock().advanceTo(7, Report));
  EXPECT_EQ(call(ServiceKind::Read, Alias, Base + 136, 1), 1u);
  EXPECT_TRUE(P->ready());
  EXPECT_EQ(llvm::cantFail(P->resume(Report)), uint64_t(-11));
  EXPECT_EQ(Kernel->clock().elapsed(), 7u); // Untimed explicit completion.
  EXPECT_EQ(word(Base + 128), 0u);
  EXPECT_FALSE(call(ServiceKind::ReceiveFrom, Alias, Base + 128, 1, 0x40));
}

TEST_P(LinuxReceive, ZeroDatagramPeekAndLatePeerLengthKeepPacketOwnership) {
  const auto FD = open(2);
  EXPECT_EQ(call(ServiceKind::Read, FD, 1, 0), 0u);
  EXPECT_EQ(Kernel->clock().elapsed(), 0u);
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, FD, 1, 0, 2), 0u);
  EXPECT_EQ(Kernel->clock().elapsed(), 7u);
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, FD, 1, 0), 0u);
  word(Base + 192, 16);
  word(Base + 160, UINT64_MAX);
  auto P = pending(FD, Base + 128, 2, 2 | 0x20, Base + 160, Base + 192);
  ASSERT_TRUE(P);
  word(Base + 192, 2); // addrlen is an output control read at completion.
  ASSERT_TRUE(Kernel->clock().advanceTo(11, Report));
  EXPECT_EQ(llvm::cantFail(P->resume(Report)), 3u);
  EXPECT_EQ(word(Base + 128), 0x0605u);
  EXPECT_EQ(word(Base + 160), 0xffffffffffff0002u);
  EXPECT_EQ(word(Base + 192), 16u);
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 136, 8), 3u);
  EXPECT_EQ(word(Base + 136), 0x070605u);
}

TEST_P(LinuxReceive, InvalidAdmissionAndClockOverflowHaveNoEffects) {
  const auto FD = open();
  EXPECT_FALSE(call(ServiceKind::ReceiveFrom, FD, Base + 128, 1, 0x100));
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 128, UINT64_MAX), uint64_t(-14));
  EXPECT_FALSE(
      call(ServiceKind::ReceiveFrom, FD, Base + 128, 1, 0, Base + 160));
  Options.LinuxTime->Clocks[1] = {INT64_MAX, 999999999};
  EXPECT_FALSE(call(ServiceKind::Read, FD, Base + 128, 1));
  EXPECT_EQ(Kernel->clock().elapsed(), 0u);
  EXPECT_EQ(word(Base + 128), 0u);
  Options.LinuxTime->Clocks[1] = {100, 0};
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 128, 1), 1u);
}

TEST_P(LinuxReceive, CompleteUserExtentIsAdmittedBeforeBlockingOrEOF) {
  const auto FD = open();
  for (auto Kind : {ServiceKind::Read, ServiceKind::ReceiveFrom}) {
    EXPECT_EQ(call(Kind, FD, UINT64_MAX, 0), uint64_t(-14));
    EXPECT_EQ(call(Kind, FD, Layout.UserLimit - 1, 2), uint64_t(-14));
    EXPECT_EQ(call(Kind, FD, UINT64_MAX, 1, 0x40), uint64_t(-14));
  }
  // read looks up the FD first; recvfrom imports the range first.
  EXPECT_EQ(call(ServiceKind::Read, 99, UINT64_MAX, 0), uint64_t(-9));
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, 99, UINT64_MAX, 0), uint64_t(-14));
  EXPECT_EQ(Kernel->clock().elapsed(), 0u);
  EXPECT_EQ(word(Base + 128), 0u);
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 128, 8), 2u);
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 128, 8), 1u);
  ASSERT_TRUE(Kernel->clock().advanceTo(13, Report));
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, FD, UINT64_MAX, 1), uint64_t(-14));
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, FD, 1, 1), 0u);
}

TEST_P(LinuxReceive, UnknownAndSendGatedInputsNeverBecomeTimeouts) {
  auto &C = Options.LinuxNetwork->Connections[0];
  C.ExpectedSend = {0};
  C.ReceiveEvents = {{{1}, 0, 1, 7}};
  const auto FD = open();
  timeout(FD, 2);
  EXPECT_FALSE(call(ServiceKind::Read, FD, Base + 128, 1));
  EXPECT_FALSE(call(ServiceKind::ReceiveFrom, FD, Base + 128, 1, 0x40));
  EXPECT_EQ(Kernel->clock().elapsed(), 0u);
  EXPECT_EQ(call(ServiceKind::Write, FD, Base + 128, 1), 1u);
  EXPECT_EQ(call(ServiceKind::Read, FD, Base + 128, 1), 1u);
  EXPECT_EQ(Kernel->clock().elapsed(), 7u);
}

INSTANTIATE_TEST_SUITE_P(Architectures, LinuxReceive,
                         testing::Values(GuestArchitecture::X64,
                                         GuestArchitecture::AArch64));
} // namespace
} // namespace neverd::emulation
