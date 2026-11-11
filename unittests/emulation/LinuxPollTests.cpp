//===- LinuxPollTests.cpp - Poll and epoll ownership and ABI ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent bitmap and stream-state checks for the shared LP64 kernel.

#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxServices.h"

#include "neverd/emulation/ExecutionConfiguration.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
using linux_model::ServiceKind;
constexpr uint64_t Base = 0x20000000;
template <class T> T require(llvm::Expected<T> Value) {
  if (!Value)
    llvm::report_fatal_error(Value.takeError());
  return std::move(*Value);
}
void require(llvm::Error E) {
  if (E)
    llvm::report_fatal_error(std::move(E));
}
class LinuxPoll : public testing::TestWithParam<GuestArchitecture> {
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
    auto P = require(probeExecutionBackend(C));
    if (P.Availability != BackendAvailability::Available)
      GTEST_SKIP() << P.Reason;
    Backend.emplace(require(createExecutionBackend(C, 4 * 1024 * 1024)));
    require(
        cpu().addressSpace()->map(Base, 8192, Read | Write | UserAccessible));
    Options.LinuxFiles.emplace().DescriptorLimit = 16;
    Options.LinuxFiles->Files["/input"] = {1, 2};
    Options.LinuxTime.emplace().AdvanceOnIdle = true;
    Options.LinuxTime->Clocks[1] = {100, 0};
    LinuxStreamConnection C0;
    C0.Peer = {2, 0, 0x23, 0x29, 127, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    C0.ExpectedSend = {1, 2, 3};
    C0.Receive = {7, 8};
    C0.ReceiveEOF = true;
    Options.LinuxNetwork.emplace().Connections = {C0};
    require(cpu().write(Base, C0.Peer));
    Kernel = std::make_unique<linux_model::LinuxServices>(
        cpu(), Layout, Base + 8192, Options, Report);
  }
  void word(uint64_t Address, uint64_t Value) {
    uint8_t Bytes[8];
    llvm::support::endian::write64le(Bytes, Value);
    require(cpu().write(Address, Bytes));
  }
  uint64_t word(uint64_t Address) {
    uint8_t Bytes[8];
    require(cpu().read(Address, Bytes));
    return llvm::support::endian::read64le(Bytes);
  }
  std::optional<uint64_t> call(ServiceKind K, uint64_t A = 0, uint64_t B = 0,
                               uint64_t C = 0, uint64_t D = 0, uint64_t E = 0,
                               uint64_t F = 0) {
    return require(Kernel->handle(K, {0, 0, {A, B, C, D, E, F}, {}}));
  }
  std::optional<uint64_t> poll(uint64_t Address, uint64_t Count,
                               uint64_t Timeout = Base + 96,
                               linux_model::ThreadContext *Thread = nullptr) {
    const uint64_t Number = GetParam() == GuestArchitecture::X64 ? 271 : 73;
    return require(Kernel->handle(
        {0, Number, {Address, Count, Timeout, 0, 0, 0}, {}}, Thread));
  }
  void entry(unsigned Index, int32_t FD, uint16_t Events,
             uint16_t Returned = 0xa5a5) {
    word(Base + 256 + Index * 8,
         uint32_t(FD) | (uint64_t(Events) << 32) | (uint64_t(Returned) << 48));
  }
  uint16_t events(unsigned Index) { return word(Base + 256 + Index * 8) >> 48; }
  unsigned eventSize() {
    return GetParam() == GuestArchitecture::X64 ? 12 : 16;
  }
  void registration(uint32_t Events, uint64_t Data) {
    uint8_t Bytes[16]{};
    llvm::support::endian::write32le(Bytes, Events);
    llvm::support::endian::write64le(Bytes + eventSize() - 8, Data);
    require(
        cpu().write(Base + 128, llvm::ArrayRef(Bytes).take_front(eventSize())));
  }
  std::optional<uint64_t> epoll(uint64_t FD, uint64_t Output = Base + 512,
                                uint64_t Maximum = 1, uint64_t MS = 0,
                                linux_model::ThreadContext *Thread = nullptr) {
    const uint64_t Number = GetParam() == GuestArchitecture::X64 ? 281 : 22;
    return require(Kernel->handle(
        {0, Number, {FD, Output, Maximum, MS, 0, 0}, {}}, Thread));
  }
  uint64_t socket() {
    auto FD = call(ServiceKind::Socket, 2, 1, 6).value();
    EXPECT_EQ(call(ServiceKind::Connect, FD, Base, 16), 0u);
    return FD;
  }
  uint64_t file() {
    const uint8_t Name[] = {'/', 'i', 'n', 'p', 'u', 't', 0};
    require(cpu().write(Base + 32, Name));
    return call(ServiceKind::OpenAt, uint32_t(-100), Base + 32, 0).value();
  }
};

TEST_P(LinuxPoll, PerEntryResultsDuplicatesNegativeAndInvalidDescriptors) {
  ASSERT_EQ(socket(), 3u);
  entry(0, 3, 1 | 0x40);
  entry(1, 3, 4 | 0x100);
  entry(2, -1, 0xffff);
  entry(3, 99, 0);
  entry(4, 99, 0xffff);
  EXPECT_EQ(poll(Base + 256, 5), 4u);
  EXPECT_EQ(events(0), 0x41u);
  EXPECT_EQ(events(1), 0x104u);
  EXPECT_EQ(events(2), 0u);
  EXPECT_EQ(events(3), 0x20u);
  EXPECT_EQ(events(4), 0x20u);
  EXPECT_EQ(call(ServiceKind::Read, 3, Base + 128, 8), 2u);
  entry(0, 3, 1);
  EXPECT_EQ(poll(Base + 256, 1),
            1u); // EOF stays readable, without HUP invention.
  EXPECT_EQ(events(0), 1u);
}
TEST_P(LinuxPoll, RegularFilesIgnoreCursorAndUnsupportedEventBits) {
  ASSERT_EQ(file(), 3u);
  entry(0, 3, 0xffff);
  EXPECT_EQ(poll(Base + 256, 1), 1u);
  EXPECT_EQ(events(0), 0x145u);
  EXPECT_EQ(call(ServiceKind::Read, 3, Base + 128, 8), 2u);
  EXPECT_EQ(poll(Base + 256, 1), 1u);
  EXPECT_EQ(events(0), 0x145u);
}
TEST_P(LinuxPoll, ArrivalTimeoutAndRawRemainingTimespecShareVirtualClock) {
  auto &C = Options.LinuxNetwork->Connections[0];
  C.Receive.clear();
  C.ReceiveEOF = false;
  C.ReceiveEvents = {{{0x5a}, 0, 0, 7}};
  ASSERT_EQ(socket(), 3u);
  entry(0, 3, 1);
  word(Base + 104, 11);
  EXPECT_EQ(poll(Base + 256, 1), 1u);
  EXPECT_EQ(Kernel->clock().elapsed(), 7u);
  EXPECT_EQ(word(Base + 104), 4u);
  EXPECT_EQ(call(ServiceKind::Read, 3, Base + 128, 1), 1u);
  C.ReceiveQuietUntilNS = 100;
  entry(0, 3, 1);
  EXPECT_EQ(poll(Base + 256, 1), 0u);
  EXPECT_EQ(events(0), 0u);
  EXPECT_EQ(Kernel->clock().elapsed(), 11u);
  EXPECT_EQ(word(Base + 104), 0u);
}
TEST_P(LinuxPoll, QuietDurationStartsAtConnectAndIsSharedByAliases) {
  auto &Connections = Options.LinuxNetwork->Connections;
  Connections[0].Receive.clear();
  Connections[0].ReceiveEOF = false;
  Connections[0].ReceiveQuietAfterConnectNS = 2000000;
  Connections.resize(2, Connections.front());
  ASSERT_TRUE(Kernel->clock().advanceTo(71, Report));
  ASSERT_EQ(socket(), 3u);
  ASSERT_EQ(call(ServiceKind::Dup, 3), 4u);
  ASSERT_EQ(call(ServiceKind::Close, 3), 0u);
  entry(0, 4, 1);
  EXPECT_EQ(call(ServiceKind::Poll, Base + 256, 1, 1), 0u);
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, 4, Base + 128, 1, 0x40),
            uint64_t(-11)); // MSG_DONTWAIT observes the same quiet interval.
  EXPECT_EQ(call(ServiceKind::Poll, Base + 256, 1, 1), 0u);
  EXPECT_EQ(Kernel->clock().elapsed(), 2000071u);
  EXPECT_EQ(events(0), 0u);
  entry(0, 4, 1);
  EXPECT_FALSE(call(ServiceKind::Poll, Base + 256, 1, 1));
  EXPECT_EQ(Kernel->clock().elapsed(), 2000072u);
  EXPECT_EQ(events(0), 0xa5a5u);
  EXPECT_FALSE(call(ServiceKind::ReceiveFrom, 4, Base + 128, 1, 0x40));
  ASSERT_EQ(call(ServiceKind::Close, 4), 0u);
  ASSERT_EQ(socket(), 3u);
  entry(0, 3, 1);
  EXPECT_EQ(call(ServiceKind::Poll, Base + 256, 1, 2), 0u);
  EXPECT_EQ(Kernel->clock().elapsed(), 4000072u);
}
TEST_P(LinuxPoll, UnrepresentableQuietEndDoesNotConsumeTheConnection) {
  auto &Connections = Options.LinuxNetwork->Connections;
  Connections[0].Receive.clear();
  Connections[0].ReceiveEOF = false;
  Connections[0].ReceiveEvents = {{{0x5a}, 0, 0, 100}};
  Connections[0].ReceiveQuietAfterConnectNS = 20;
  Connections.resize(2, Connections.front());
  Connections[1].ReceiveQuietAfterConnectNS = INT64_MAX - 1;
  ASSERT_EQ(call(ServiceKind::Socket, 2, 1, 6), 3u);
  EXPECT_FALSE(call(ServiceKind::Connect, 3, Base, 16));
  EXPECT_EQ(Report.Diagnostic, "invalid offline receive end time");
  ASSERT_TRUE(Kernel->clock().advanceTo(80, Report));
  ASSERT_EQ(call(ServiceKind::Connect, 3, Base, 16), 0u);
  entry(0, 3, 1);
  EXPECT_EQ(call(ServiceKind::Poll, Base + 256, 1, 1), 1u);
  EXPECT_EQ(Kernel->clock().elapsed(), 100u);
  EXPECT_EQ(call(ServiceKind::Read, 3, Base + 128, 1), 1u);
  EXPECT_EQ(call(ServiceKind::Poll, Base + 256, 1, 0), 0u);
  ASSERT_EQ(call(ServiceKind::Socket, 2, 1, 6), 4u);
  for (unsigned Attempt = 0; Attempt != 2; ++Attempt) {
    EXPECT_FALSE(call(ServiceKind::Connect, 4, Base, 16));
    EXPECT_EQ(Report.Diagnostic,
              "offline receive quiet interval has no finite end");
  }
}
TEST_P(LinuxPoll, SuspendedInputsAreCopiedButOutputPermissionsAreRechecked) {
  auto &C = Options.LinuxNetwork->Connections[0];
  C.Receive.clear();
  C.ReceiveEOF = false;
  C.ReceiveEvents = {{{0x5a}, 0, 0, 7}};
  ASSERT_EQ(socket(), 3u);
  entry(0, 3, 1);
  word(Base + 104, 20);
  linux_model::ThreadContext Thread;
  EXPECT_FALSE(poll(Base + 256, 1, Base + 96, &Thread));
  ASSERT_TRUE(Thread.Pending);
  EXPECT_FALSE(Thread.Pending->ready());
  EXPECT_EQ(Thread.Pending->nextWake(), 7u);
  entry(0, -1,
        4); // Only the entry's output field is overwritten on completion.
  word(Base + 104, 999);
  EXPECT_TRUE(Kernel->clock().advanceTo(7, Report));
  EXPECT_EQ(require(Thread.Pending->resume(Report)), 1u);
  EXPECT_EQ(word(Base + 256), 0x00010004ffffffffULL);
  EXPECT_EQ(word(Base + 104), 13u);
  Thread.Pending.reset();
  C.ReceiveEvents[0].ReadyAtNS = 30;
  entry(0, 3, 1);
  EXPECT_FALSE(poll(Base + 256, 1, Base + 96, &Thread));
  ASSERT_TRUE(Thread.Pending);
  require(cpu().addressSpace()->protect(Base, 4096, Read | UserAccessible));
  EXPECT_TRUE(Kernel->clock().advanceTo(20, Report));
  EXPECT_EQ(require(Thread.Pending->resume(Report)), uint64_t(-14));
}
TEST_P(LinuxPoll, ZeroCountDeadlinesAndMillisAdapterDoNotNeedGKI) {
  word(Base + 104, 7);
  EXPECT_EQ(poll(0, 0), 0u);
  EXPECT_EQ(Kernel->clock().elapsed(), 7u);
  EXPECT_EQ(call(ServiceKind::Poll, 0, 0, 2), 0u);
  EXPECT_EQ(Kernel->clock().elapsed(), 2000007u);
  EXPECT_FALSE(call(ServiceKind::Poll, 0, 0, uint32_t(-1)));
  EXPECT_NE(Report.Diagnostic.find("readiness schedule"), std::string::npos);
}
TEST_P(LinuxPoll, UnknownReadinessAndClosedDescriptorsNeverBecomeTimeouts) {
  auto &C = Options.LinuxNetwork->Connections[0];
  C.Receive.clear();
  C.ReceiveEOF = false;
  ASSERT_EQ(socket(), 3u);
  entry(0, 3, 1);
  EXPECT_FALSE(poll(Base + 256, 1));
  EXPECT_EQ(events(0), 0xa5a5u);
  C.ReceiveEvents = {{{0x5a}, 0, 0, 7}};
  word(Base + 104, 20);
  linux_model::ThreadContext Thread;
  EXPECT_FALSE(poll(Base + 256, 1, Base + 96, &Thread));
  ASSERT_TRUE(Thread.Pending);
  EXPECT_EQ(call(ServiceKind::Close, 3), 0u);
  ASSERT_EQ(file(), 3u);
  EXPECT_TRUE(Thread.Pending->ready());
  EXPECT_FALSE(require(Thread.Pending->resume(Report)));
  EXPECT_EQ(Report.Diagnostic, "poll descriptor changed while waiting");
  EXPECT_EQ(events(0), 0xa5a5u);
}
TEST_P(LinuxPoll, PollAdmissionAndOrderedCopiesPreserveEarlierFields) {
  EXPECT_EQ(poll(Base + 256, 17), uint64_t(-22));
  EXPECT_EQ(poll(1, 1), uint64_t(-14));
  EXPECT_EQ(poll(Layout.UserLimit + 1, 0), uint64_t(-14));
  word(Base + 96, UINT64_MAX);
  EXPECT_EQ(poll(1, 17), uint64_t(-22));
  word(Base + 96, 0);
  word(Base + 4088, 99);
  word(Base + 4096, 99);
  require(
      cpu().addressSpace()->protect(Base + 4096, 4096, Read | UserAccessible));
  EXPECT_EQ(poll(Base + 4088, 2), uint64_t(-14));
  EXPECT_EQ(word(Base + 4088), (uint64_t(0x20) << 48) | 99);
  EXPECT_EQ(word(Base + 4096), 99u);
}
TEST_P(LinuxPoll, EpollLevelOneShotModifyAndOpaqueDataRoundTrip) {
  ASSERT_EQ(socket(), 3u);
  EXPECT_EQ(call(ServiceKind::EpollCreate, 0), uint64_t(-22));
  EXPECT_EQ(call(ServiceKind::EpollCreate1, 1), uint64_t(-22));
  ASSERT_EQ(call(ServiceKind::EpollCreate1, 0), 4u);
  registration(1 | (1u << 30), 0x1234567887654321ULL);
  EXPECT_EQ(call(ServiceKind::EpollCtl, 4, 1, 3, Base + 128), 0u);
  EXPECT_EQ(call(ServiceKind::EpollCtl, 4, 1, 3, Base + 128), uint64_t(-17));
  word(Base + 512, UINT64_MAX);
  EXPECT_EQ(epoll(4), 1u);
  EXPECT_EQ(uint32_t(word(Base + 512)), 1u);
  EXPECT_EQ(word(Base + 512 + eventSize() - 8), 0x1234567887654321ULL);
  if (eventSize() == 16)
    EXPECT_EQ(word(Base + 512) >> 32, UINT32_MAX);
  EXPECT_EQ(epoll(4), 0u);
  registration(1, 77);
  EXPECT_EQ(call(ServiceKind::EpollCtl, 4, 3, 3, Base + 128), 0u);
  EXPECT_EQ(epoll(4), 1u);
  EXPECT_EQ(epoll(4), 1u);
  EXPECT_EQ(word(Base + 512 + eventSize() - 8), 77u);
  EXPECT_EQ(call(ServiceKind::EpollCtl, 4, 2, 3, 1), 0u);
  EXPECT_EQ(call(ServiceKind::EpollCtl, 4, 2, 3, 1), uint64_t(-2));
  EXPECT_EQ(epoll(4), 0u);
}
TEST_P(LinuxPoll, EpollDupCloseReuseAndRegistrationIdentity) {
  ASSERT_EQ(socket(), 3u);
  ASSERT_EQ(call(ServiceKind::EpollCreate1), 4u);
  ASSERT_EQ(call(ServiceKind::Dup, 3), 5u);
  ASSERT_EQ(call(ServiceKind::Dup, 4), 6u);
  registration(1, 13);
  EXPECT_EQ(call(ServiceKind::EpollCtl, 4, 1, 3, Base + 128), 0u);
  registration(1, 15);
  EXPECT_EQ(call(ServiceKind::EpollCtl, 4, 1, 5, Base + 128), 0u);
  EXPECT_EQ(epoll(6), 1u);
  EXPECT_EQ(word(Base + 512 + eventSize() - 8), 13u);
  EXPECT_EQ(epoll(6), 1u);
  EXPECT_EQ(word(Base + 512 + eventSize() - 8), 15u);
  EXPECT_EQ(call(ServiceKind::Close, 4), 0u);
  EXPECT_EQ(call(ServiceKind::Close, 3), 0u);
  ASSERT_EQ(file(), 3u);
  EXPECT_EQ(epoll(6, Base + 512, 4),
            2u); // Closed registration remains via dup.
  EXPECT_EQ(call(ServiceKind::Close, 5), 0u);
  EXPECT_EQ(epoll(6),
            0u); // Last open description release removes both watches.
  EXPECT_EQ(call(ServiceKind::Read, 6, Base + 128, 8), uint64_t(-22));
  EXPECT_EQ(call(ServiceKind::Write, 6, Base + 128, 8), uint64_t(-22));
  EXPECT_EQ(call(ServiceKind::Lseek, 6, 0, 0), uint64_t(-29));
}
TEST_P(LinuxPoll, EpollWaitOwnsSetAcrossCloseAndRechecksLiveModifications) {
  auto &C = Options.LinuxNetwork->Connections[0];
  C.Receive.clear();
  C.ReceiveEOF = false;
  C.ReceiveEvents = {{{9}, 0, 0, 7}};
  ASSERT_EQ(socket(), 3u);
  ASSERT_EQ(call(ServiceKind::EpollCreate1), 4u);
  ASSERT_EQ(call(ServiceKind::Dup, 4), 5u);
  registration(1, 13);
  EXPECT_EQ(call(ServiceKind::EpollCtl, 4, 1, 3, Base + 128), 0u);
  linux_model::ThreadContext Thread;
  EXPECT_FALSE(epoll(4, Base + 512, 1, 1, &Thread));
  ASSERT_TRUE(Thread.Pending);
  EXPECT_EQ(call(ServiceKind::Close, 4), 0u);
  registration(1, 99);
  EXPECT_EQ(call(ServiceKind::EpollCtl, 5, 3, 3, Base + 128), 0u);
  EXPECT_TRUE(Kernel->clock().advanceTo(7, Report));
  EXPECT_EQ(require(Thread.Pending->resume(Report)), 1u);
  EXPECT_EQ(word(Base + 512 + eventSize() - 8), 99u);
}
TEST_P(LinuxPoll, EpollErrorPriorityUnsupportedModesAndOutputFaults) {
  EXPECT_EQ(epoll(99, 0, 0), uint64_t(-22));
  EXPECT_EQ(epoll(99, Layout.UserLimit, 1), uint64_t(-14));
  EXPECT_EQ(epoll(99), uint64_t(-9));
  ASSERT_EQ(socket(), 3u);
  ASSERT_EQ(call(ServiceKind::EpollCreate1), 4u);
  ASSERT_EQ(file(), 5u);
  EXPECT_EQ(call(ServiceKind::EpollCtl, 99, 1, 99, 1), uint64_t(-14));
  registration(1, 12);
  EXPECT_EQ(call(ServiceKind::EpollCtl, 4, 1, 5, Base + 128), uint64_t(-1));
  EXPECT_EQ(call(ServiceKind::EpollCtl, 4, 1, 4, Base + 128), uint64_t(-22));
  registration(1 | (1u << 31), 12);
  EXPECT_FALSE(call(ServiceKind::EpollCtl, 4, 1, 3, Base + 128));
  registration(1 | (1u << 30), 12);
  EXPECT_EQ(call(ServiceKind::EpollCtl, 4, 1, 3, Base + 128), 0u);
  require(
      cpu().addressSpace()->protect(Base + 4096, 4096, Read | UserAccessible));
  const auto At = Base + 4096 - (eventSize() - 8);
  EXPECT_EQ(epoll(4, At), uint64_t(-14));
  EXPECT_EQ(uint32_t(word(At)), 1u); // Event mask survives failed data field.
  EXPECT_EQ(epoll(4), 1u);           // Failed output did not disable ONESHOT.
}
INSTANTIATE_TEST_SUITE_P(Architectures, LinuxPoll,
                         testing::Values(GuestArchitecture::X64,
                                         GuestArchitecture::AArch64));
} // namespace
} // namespace neverd::emulation
