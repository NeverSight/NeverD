//===- LinuxSelectTests.cpp - Offline pselect6 ownership and ABI ----------===//
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
class LinuxSelect : public testing::TestWithParam<GuestArchitecture> {
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
    Options.LinuxFiles.emplace().DescriptorLimit = 6;
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
  std::optional<uint64_t> select(uint64_t N, uint64_t R, uint64_t W = 0,
                                 uint64_t X = 0, uint64_t T = Base + 96,
                                 uint64_t S = 0) {
    const uint64_t Number = GetParam() == GuestArchitecture::X64 ? 270 : 72;
    return require(Kernel->handle({0, Number, {N, R, W, X, T, S}, {}}));
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

TEST_P(LinuxSelect, ReadyBitsCountAcrossSetsAndDoNotConsumeStreams) {
  ASSERT_EQ(socket(), 3u);
  ASSERT_EQ(call(ServiceKind::Dup, 3), 4u);
  word(Base + 64, 0x8000000000000018);
  word(Base + 72, 8);
  word(Base + 80, 0);
  word(Base + 88, 0xa5a5a5a5a5a5a5a5);
  EXPECT_EQ(select(5, Base + 64, Base + 72, Base + 80), 3u);
  EXPECT_EQ(word(Base + 64), 24u);
  EXPECT_EQ(word(Base + 72), 8u);
  EXPECT_EQ(word(Base + 80), 0u);
  EXPECT_EQ(word(Base + 88), 0xa5a5a5a5a5a5a5a5u);
  EXPECT_EQ(select(5, Base + 64, Base + 72), 3u);
  EXPECT_EQ(call(ServiceKind::Read, 4, Base + 128, 8), 2u);
  EXPECT_EQ(word(Base + 128), 0x0807u);
  // EOF stays readable without another payload or cursor mutation.
  EXPECT_EQ(select(5, Base + 64), 2u);
  EXPECT_EQ(call(ServiceKind::Read, 3, Base + 128, 1), 0u);
}

TEST_P(LinuxSelect, AllDescriptorsAreValidatedBeforeReadinessAndWrites) {
  ASSERT_EQ(socket(), 3u);
  word(Base + 64, 0x28);
  EXPECT_EQ(select(6, Base + 64), uint64_t(-9));
  EXPECT_EQ(word(Base + 64), 0x28u);
  EXPECT_EQ(select(6, Base + 64, 1), uint64_t(-14));
  word(Base + 64, uint64_t(1) << 63);
  EXPECT_EQ(select(64, Base + 64), uint64_t(-9));
  EXPECT_EQ(select(4, Base + 64), 0u); // Ignore bits beyond nfds.
  EXPECT_EQ(word(Base + 64), 0u);
  EXPECT_FALSE(select(65, Base + 64));
  EXPECT_NE(Report.Diagnostic.find("initial descriptor word"),
            std::string::npos);
}

TEST_P(LinuxSelect, WrapperTimeoutMaskAndCountErrorPriority) {
  word(Base + 96, UINT64_MAX);
  EXPECT_EQ(select(UINT64_MAX, 1, 0, 0, Base + 96, 1), uint64_t(-14));
  word(Base + 112, 1);
  word(Base + 120, 1);
  EXPECT_EQ(select(UINT64_MAX, 1, 0, 0, Base + 96, Base + 112), uint64_t(-22));
  word(Base + 96, 0);
  EXPECT_EQ(select(0, 0, 0, 0, Base + 96, Base + 112), uint64_t(-22));
  word(Base + 120, 8);
  EXPECT_EQ(select(0, 0, 0, 0, Base + 96, Base + 112), uint64_t(-14));
  word(Base + 112, Base + 128);
  EXPECT_FALSE(select(0, 0, 0, 0, Base + 96, Base + 112));
  EXPECT_NE(Report.Diagnostic.find("signal masks"), std::string::npos);
  word(Base + 112, 0);
  word(Base + 120, UINT64_MAX); // Null mask ignores sigsetsize.
  EXPECT_EQ(select(0, 0, 0, 0, Base + 96, Base + 112), 0u);
  EXPECT_EQ(select(UINT64_MAX, 1), uint64_t(-22));
  EXPECT_EQ(select(0x1234567800000000, 0), 0u);
}

TEST_P(LinuxSelect, OrderedBitmapCopiesRetainEarlierOutputOnFault) {
  ASSERT_EQ(socket(), 3u);
  word(Base + 64, 0x8000000000000008);
  word(Base + 4096, 8);
  require(
      cpu().addressSpace()->protect(Base + 4096, 4096, Read | UserAccessible));
  EXPECT_EQ(select(4, Base + 64, Base + 4096), uint64_t(-14));
  EXPECT_EQ(word(Base + 64), 8u);
  EXPECT_EQ(word(Base + 4096), 8u);
}

TEST_P(LinuxSelect, AliasedSetsAndTimeoutRetainCopyOrder) {
  ASSERT_EQ(file(), 3u);
  word(Base + 64, 8);
  EXPECT_EQ(select(4, Base + 64, Base + 64, Base + 64), 2u);
  EXPECT_EQ(word(Base + 64), 0u); // Exception set is stored last.
  word(Base + 64, 8);
  word(Base + 72, 0);
  EXPECT_EQ(select(4, Base + 64, Base + 64, Base + 64, Base + 64), 2u);
  EXPECT_EQ(word(Base + 64), 8u); // Positive timeout is stored after sets.
  EXPECT_EQ(word(Base + 72), 0u);
  EXPECT_EQ(Kernel->clock().elapsed(), 0u);
}

TEST_P(LinuxSelect, PartialOutputsAreExplicitAndReadonlyTimeoutIsIgnored) {
  ASSERT_EQ(socket(), 3u);
  word(Base + 64, 8);
  word(Base + 4092, 0x0000000300000008);
  word(Base + 4096, 3);
  require(
      cpu().addressSpace()->protect(Base + 4096, 4096, Read | UserAccessible));
  EXPECT_EQ(select(4, Base + 64, 0, 0, Base + 4096), 1u);
  EXPECT_EQ(word(Base + 4096), 3u);
  EXPECT_FALSE(select(4, Base + 4092));
  EXPECT_NE(Report.Diagnostic.find("partial select bitmap"), std::string::npos);
}

TEST_P(LinuxSelect, MissingAndGatedReceiveInputsDoNotInventReadiness) {
  auto &C = Options.LinuxNetwork->Connections[0];
  C.Receive.clear();
  C.ReceiveEOF = false;
  ASSERT_EQ(socket(), 3u);
  word(Base + 64, 8);
  EXPECT_FALSE(select(4, Base + 64));
  EXPECT_EQ(word(Base + 64), 8u);
  C.ReceiveEvents = {{{9}, 0, 3}};
  EXPECT_FALSE(select(4, Base + 64));
  const uint8_t Bytes[] = {1, 2, 3};
  require(cpu().write(Base + 128, Bytes));
  EXPECT_EQ(call(ServiceKind::SendTo, 3, Base + 128, 3), 3u);
  EXPECT_EQ(select(4, Base + 64), 1u);
  EXPECT_EQ(call(ServiceKind::Read, 3, Base + 128, 1), 1u);
  EXPECT_EQ(word(Base + 128) & 0xff, 9u);
  EXPECT_FALSE(select(4, Base + 64));
  EXPECT_FALSE(select(4, 0, Base + 64)); // Acceptance budget is not readiness.
}

TEST_P(LinuxSelect, CompletionErrorsAndExceptionalDataRemainUnknown) {
  auto &C = Options.LinuxNetwork->Connections[0];
  C.Receive.clear();
  C.ReceiveEvents = {{{}, 4, 0}};
  ASSERT_EQ(socket(), 3u);
  word(Base + 64, 8);
  EXPECT_FALSE(select(4, Base + 64));
  EXPECT_EQ(call(ServiceKind::Read, 3, Base + 128, 1), uint64_t(-4));
  EXPECT_FALSE(select(4, 0, 0, Base + 64));
}

TEST_P(LinuxSelect, ZeroLengthDatagramsAreReadableWithoutConsumption) {
  LinuxDatagramConnection C;
  C.Peer = Options.LinuxNetwork->Connections[0].Peer;
  C.ExpectedSend = {{}};
  C.ReceiveEvents = {{{}, 0, 0}};
  Options.LinuxNetwork->DatagramConnections = {C};
  ASSERT_EQ(call(ServiceKind::Socket, 2, 2, 17), 3u);
  ASSERT_EQ(call(ServiceKind::Connect, 3, Base, 16), 0u);
  word(Base + 64, 8);
  word(Base + 72, 8);
  EXPECT_EQ(select(4, Base + 64, Base + 72), 2u);
  EXPECT_EQ(select(4, Base + 64), 1u);
  EXPECT_EQ(call(ServiceKind::ReceiveFrom, 3, Base + 128, 1), 0u);
  EXPECT_FALSE(select(4, Base + 64));
}

TEST_P(LinuxSelect, BlockingWaitsNeverBecomeUnobservedTimeoutSuccess) {
  EXPECT_EQ(select(0, 0), 0u);
  EXPECT_FALSE(select(0, 0, 0, 0, 0));
  word(Base + 96, 5);
  EXPECT_FALSE(select(0, 0));
  EXPECT_NE(Report.Diagnostic.find("readiness schedule"), std::string::npos);
  EXPECT_EQ(word(Base + 96), 5u);
  EXPECT_EQ(Kernel->clock().elapsed(), 0u);
}

TEST_P(LinuxSelect, LiveProcessDescriptorsRetainTheirOwnedReadiness) {
  Options.LinuxKernel.emplace().GKI = AndroidGKIKernel::Android12_5_10;
  ASSERT_EQ(call(ServiceKind::PidFDOpen, linux_model::ProcessID), 3u);
  word(Base + 64, 8);
  EXPECT_EQ(select(4, Base + 64), 0u);
  EXPECT_EQ(word(Base + 64), 0u);
  EXPECT_EQ(call(ServiceKind::Close, 3), 0u);
  word(Base + 64, 8);
  EXPECT_EQ(select(4, Base + 64), uint64_t(-9));
  ASSERT_EQ(call(ServiceKind::Socket, 2, 1, 6), 3u);
  EXPECT_FALSE(select(4, Base + 64)); // Not connected, not implicitly ready.
  EXPECT_EQ(word(Base + 64), 8u);
}

INSTANTIATE_TEST_SUITE_P(Architectures, LinuxSelect,
                         testing::Values(GuestArchitecture::X64,
                                         GuestArchitecture::AArch64));
} // namespace
} // namespace neverd::emulation
