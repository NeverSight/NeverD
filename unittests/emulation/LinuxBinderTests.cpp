//===- LinuxBinderTests.cpp - Independent LP64 Binder memory contracts ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Raw Linux control requests across both supported LP64 architectures.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxServices.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
using linux_model::ServiceKind;
constexpr uint64_t Base = 0x20000000;
class LinuxBinder : public testing::TestWithParam<GuestArchitecture> {
protected:
  std::optional<BackendSelection> Backend;
  ProcessOptions Options;
  ProcessResult Report{ProcessProfile::LinuxELF64,
                       GuestArchitecture::X64,
                       ExecutionBackendKind::Unicorn,
                       {}};
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
    Backend.emplace(
        llvm::cantFail(createExecutionBackend(C, 16 * 1024 * 1024)));
    llvm::cantFail(
        cpu().addressSpace()->map(Base, 8192, Read | Write | UserAccessible));
    auto &D = Options.LinuxFiles.emplace().Devices["/dev/binder"];
    D.AccessMode = 2;
    D.Binder = true;
    llvm::cantFail(linux_model::validateFileOptions(*Options.LinuxFiles));
    Kernel = std::make_unique<linux_model::LinuxServices>(
        cpu(), Layout, Base + 8192, Options, Report);
  }
  std::optional<uint64_t> call(ServiceKind K, uint64_t A = 0, uint64_t B = 0,
                               uint64_t C = 0) {
    return llvm::cantFail(Kernel->handle(K, {0, 0, {A, B, C, 0, 0, 0}, {}}));
  }
  uint64_t open() {
    const uint8_t Path[] = "/dev/binder";
    llvm::cantFail(cpu().write(Base, Path));
    return call(ServiceKind::OpenAt, uint64_t(-100), Base, 2 | 0x800).value();
  }
  std::optional<uint64_t> map(uint64_t FD, uint64_t Size) {
    return llvm::cantFail(Kernel->handle(
        ServiceKind::Mmap, {0, 0, {0, Size, 1, 2 | 0x4000, FD, 0}, {}}));
  }
  std::vector<uint8_t> read(uint64_t At, unsigned Size) {
    std::vector<uint8_t> Bytes(Size);
    llvm::cantFail(cpu().read(At, Bytes));
    return Bytes;
  }
  void record(uint64_t WriteSize, uint64_t Consumed, uint64_t WriteBuffer,
              uint64_t ReadSize, uint64_t ReadBuffer) {
    uint8_t Bytes[48];
    const uint64_t Values[] = {WriteSize, Consumed, WriteBuffer,
                               ReadSize,  0,        ReadBuffer};
    for (unsigned I = 0; I < 6; ++I)
      llvm::support::endian::write64le(Bytes + I * 8, Values[I]);
    llvm::cantFail(cpu().write(Base + 256, Bytes));
  }
};
TEST_P(LinuxBinder, LargeArenasKeepReadOnlyBoundsAndOpenLifetime) {
  const auto FD = open();
  const auto Alias = call(ServiceKind::Dup, FD).value();
  constexpr uint64_t Size = 4 * 1024 * 1024;
  const auto Address = map(FD, Size - 1);
  ASSERT_TRUE(Address);
  ASSERT_LT(*Address, Layout.UserLimit);
  EXPECT_TRUE(
      llvm::cantFail(cpu().canAccess(*Address, Size, Read | UserAccessible)));
  EXPECT_FALSE(llvm::cantFail(
      cpu().canAccess(*Address + Size, 1, Read | UserAccessible)));
  EXPECT_FALSE(
      llvm::cantFail(cpu().canAccess(*Address, 1, Write | UserAccessible)));
  EXPECT_FALSE(llvm::cantFail(
      cpu().canAccess(*Address + Size - 1, 1, Write | UserAccessible)));
  EXPECT_EQ(call(ServiceKind::Mprotect, *Address + Size - 4096, 4096, 3),
            uint64_t(0) - 13);
  EXPECT_EQ(map(Alias, 4096), uint64_t(0) - 16);
  ASSERT_EQ(call(ServiceKind::Close, FD), 0u);
  EXPECT_TRUE(llvm::cantFail(
      cpu().canAccess(*Address + Size - 4096, 4096, Read | UserAccessible)));
  ASSERT_EQ(call(ServiceKind::Munmap, *Address, Size), 0u);
  EXPECT_FALSE(
      llvm::cantFail(cpu().canAccess(*Address, 1, Read | UserAccessible)));
  EXPECT_EQ(map(Alias, 4096), uint64_t(0) - 16);
  ASSERT_EQ(call(ServiceKind::Close, Alias), 0u);
  const auto Fresh = open();
  const auto NewAddress = map(Fresh, Size);
  ASSERT_TRUE(NewAddress);
  ASSERT_LT(*NewAddress, Layout.UserLimit);
  EXPECT_EQ(call(ServiceKind::Close, Fresh), 0u);
  EXPECT_TRUE(llvm::cantFail(
      cpu().canAccess(*NewAddress, Size, Read | UserAccessible)));
  EXPECT_EQ(call(ServiceKind::Munmap, *NewAddress, Size), 0u);
}
TEST_P(LinuxBinder, OversizedArenaRefusalDoesNotReserveTheOpen) {
  const auto FD = open();
  const auto Before = cpu().addressSpace()->mappedBytes();
  EXPECT_FALSE(map(FD, 4 * 1024 * 1024 + 1));
  EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(cpu().addressSpace()->mappedBytes(), Before);
  const auto Address = map(FD, 1024 * 1024);
  ASSERT_TRUE(Address);
  EXPECT_LT(*Address, Layout.UserLimit);
  EXPECT_EQ(cpu().addressSpace()->mappedBytes(), Before + 1024 * 1024);
}
TEST_P(LinuxBinder, ArenaBudgetFailureAllowsSmallerRetry) {
  Options.MemoryLimit = 2 * 1024 * 1024;
  Kernel = std::make_unique<linux_model::LinuxServices>(
      cpu(), Layout, Base + 8192, Options, Report);
  const auto FD = open();
  const auto Before = cpu().addressSpace()->mappedBytes();
  // Existing mappings consume the same process budget as receive arenas.
  EXPECT_EQ(map(FD, Options.MemoryLimit), uint64_t(0) - 12);
  EXPECT_EQ(cpu().addressSpace()->mappedBytes(), Before);
  const auto Address = map(FD, 1024 * 1024);
  ASSERT_TRUE(Address);
  EXPECT_LT(*Address, Layout.UserLimit);
  EXPECT_EQ(cpu().addressSpace()->mappedBytes(), Before + 1024 * 1024);
}
TEST_P(LinuxBinder, GuardedVersionAndBadPointers) {
  auto FD = open();
  std::vector<uint8_t> Expected(16, 0xa5);
  llvm::cantFail(cpu().write(Base + 256, Expected));
  ASSERT_EQ(call(ServiceKind::Ioctl, FD, 0xc0046209, Base + 260), 0u);
  Expected[4] = 8;
  Expected[5] = Expected[6] = Expected[7] = 0;
  EXPECT_EQ(read(Base + 256, 16), Expected);
  EXPECT_EQ(call(ServiceKind::Ioctl, FD, 0xc0046209, 1), uint64_t(-22));
  EXPECT_EQ(call(ServiceKind::Ioctl, FD, 0x40046205, 1), uint64_t(-22));
  EXPECT_EQ(call(ServiceKind::Ioctl, FD, 0xc0306201, 1), uint64_t(-14));
  EXPECT_EQ(call(ServiceKind::Ioctl, FD, 0x40046208, 1), 0u);
}
TEST_P(LinuxBinder, CompleteRecordsResumeWritesAndPreserveReadGuards) {
  auto FD = open();
  const uint8_t Commands[] = {0xff, 0xff, 0xff, 0xff, 0x0c, 0x63, 0, 0};
  llvm::cantFail(cpu().write(Base + 512, Commands));
  llvm::cantFail(cpu().write(Base + 768, std::vector<uint8_t>(16, 0xa5)));
  record(8, 4, Base + 512, 16, Base + 768);
  auto Expected = read(Base + 256, 48);
  Expected[8] = 8;
  Expected[32] = 4;
  ASSERT_EQ(call(ServiceKind::Ioctl, FD, 0xc0306201, Base + 256), 0u);
  EXPECT_EQ(read(Base + 256, 48), Expected);
  std::vector<uint8_t> Output(16, 0xa5);
  Output[0] = 0x0c;
  Output[1] = 0x72;
  Output[2] = Output[3] = 0;
  EXPECT_EQ(read(Base + 768, 16), Output);
  record(0, 0, UINT64_MAX, 16, Base + 768);
  Expected = read(Base + 256, 48);
  ASSERT_EQ(call(ServiceKind::Ioctl, FD, 0xc0306201, Base + 256),
            uint64_t(-11));
  EXPECT_EQ(read(Base + 256, 48), Expected);
  EXPECT_EQ(read(Base + 768, 16), Output);
}
TEST_P(LinuxBinder, ReadFaultStillReportsConsumedCommands) {
  auto FD = open();
  const uint8_t Command[] = {0x0c, 0x63, 0, 0};
  llvm::cantFail(cpu().write(Base + 512, Command));
  record(4, 0, Base + 512, 4, 1);
  auto Expected = read(Base + 256, 48);
  Expected[8] = 4;
  ASSERT_EQ(call(ServiceKind::Ioctl, FD, 0xc0306201, Base + 256),
            uint64_t(-14));
  EXPECT_EQ(read(Base + 256, 48), Expected);
}
TEST_P(LinuxBinder, OverlappingOutputsKeepKernelCopyOrder) {
  auto FD = open();
  record(0, 0, 1, 4, Base + 256);
  auto Expected = read(Base + 256, 48);
  Expected[32] = 4;
  ASSERT_EQ(call(ServiceKind::Ioctl, FD, 0xc0306201, Base + 256), 0u);
  EXPECT_EQ(read(Base + 256, 48), Expected);
}
TEST_P(LinuxBinder, TransactionBuffersKeepLP64LayoutAndPeerOwnership) {
  const uint64_t Server = open(), Client = open();
  auto Map = [&](uint64_t FD) {
    return llvm::cantFail(Kernel->handle(ServiceKind::Mmap,
                                         {0, 0, {0, 4096, 1, 2, FD, 0}, {}}))
        .value();
  };
  const uint64_t ServerMap = Map(Server), ClientMap = Map(Client);
  ASSERT_NE(ServerMap, ClientMap);
  ASSERT_EQ(call(ServiceKind::Ioctl, Server, 0x40046207, 1), 0u);
  auto Send = [&](uint64_t FD, uint32_t Command, uint64_t Body,
                  unsigned Count) {
    std::vector<uint8_t> Bytes(4 + Count);
    llvm::support::endian::write32le(Bytes.data(), Command);
    if (Count)
      llvm::cantFail(
          cpu().read(Body, llvm::MutableArrayRef(Bytes).drop_front(4)));
    llvm::cantFail(cpu().write(Base + 512, Bytes));
    record(Bytes.size(), 0, Base + 512, 0, 0);
    return call(ServiceKind::Ioctl, FD, 0xc0306201, Base + 256);
  };
  std::vector<uint8_t> Tx(64);
  llvm::support::endian::write32le(Tx.data() + 16, 0x12345678);
  llvm::support::endian::write32le(Tx.data() + 20, 16); // TF_ACCEPT_FDS
  llvm::support::endian::write64le(Tx.data() + 32, 3);
  llvm::support::endian::write64le(Tx.data() + 48, Base + 2048);
  llvm::cantFail(cpu().write(Base + 1024, Tx));
  const uint8_t Payload[] = {0x00, 0xff, 0x61};
  llvm::cantFail(cpu().write(Base + 2048, Payload));
  for (uint32_t Flags : {1u, 8u, 0x80000010u, 16u}) {
    SCOPED_TRACE(Flags);
    auto Invalid = Tx;
    llvm::support::endian::write32le(Invalid.data() + 20, Flags);
    if (Flags == 16)
      llvm::support::endian::write64le(Invalid.data() + 40, 8);
    llvm::cantFail(cpu().write(Base + 1024, Invalid));
    ASSERT_FALSE(Send(Client, 0x40406300, Base + 1024, 64));
    EXPECT_NE(Report.Diagnostic.find("code 0x12345678 flags 0x"),
              std::string::npos)
        << Report.Diagnostic;
    EXPECT_EQ(read(ServerMap, 4096), std::vector<uint8_t>(4096, 0));
    EXPECT_EQ(llvm::support::endian::read64le(read(Base + 264, 8).data()), 0u);
  }
  llvm::cantFail(cpu().write(Base + 1024, Tx));
  ASSERT_EQ(Send(Client, 0x40406300, Base + 1024, 64), 0u);
  ASSERT_EQ(Send(Server, 0x630c, 0, 0), 0u);
  llvm::cantFail(cpu().write(Base + 768, std::vector<uint8_t>(128, 0xa5)));
  record(0, 0, 0, 128, Base + 768);
  ASSERT_EQ(call(ServiceKind::Ioctl, Server, 0xc0306201, Base + 256), 0u);
  auto R = read(Base + 768, 128);
  std::vector<uint8_t> Expected(128, 0xa5);
  std::fill(Expected.begin(), Expected.begin() + 72, 0);
  llvm::support::endian::write32le(Expected.data(), 0x720c);
  llvm::support::endian::write32le(Expected.data() + 4, 0x80407202);
  llvm::support::endian::write32le(Expected.data() + 24, 0x12345678);
  llvm::support::endian::write32le(Expected.data() + 28, 16);
  llvm::support::endian::write32le(Expected.data() + 32,
                                   linux_model::ProcessID);
  llvm::support::endian::write32le(Expected.data() + 36, linux_model::UserID);
  llvm::support::endian::write64le(Expected.data() + 40, 3);
  llvm::support::endian::write64le(Expected.data() + 56, ServerMap);
  llvm::support::endian::write64le(Expected.data() + 64, ServerMap + 8);
  EXPECT_EQ(R, Expected);
  EXPECT_EQ(read(ServerMap, 3), std::vector<uint8_t>({0x00, 0xff, 0x61}));
  // The sender's storage is independent of the receiver's copied payload.
  const uint8_t Answer[] = {0x78, 0x56, 0x34};
  llvm::cantFail(cpu().write(Base + 2048, Answer));
  EXPECT_EQ(read(ServerMap, 3), std::vector<uint8_t>({0x00, 0xff, 0x61}));
  for (uint32_t Flags : {8u, 16u}) {
    auto Invalid = Tx; // A status reply requires exactly four payload bytes.
    llvm::support::endian::write32le(Invalid.data() + 20, Flags);
    llvm::cantFail(cpu().write(Base + 1024, Invalid));
    ASSERT_FALSE(Send(Server, 0x40406301, Base + 1024, 64));
    EXPECT_EQ(read(ClientMap, 4096), std::vector<uint8_t>(4096, 0));
    EXPECT_EQ(llvm::support::endian::read64le(read(Base + 264, 8).data()), 0u);
  }
  // Successful retry proves refusal did not consume the pending call.
  llvm::support::endian::write32le(Tx.data() + 20, 0);
  llvm::cantFail(cpu().write(Base + 1024, Tx));
  ASSERT_EQ(Send(Server, 0x40406301, Base + 1024, 64), 0u);
  record(0, 0, 0, 128, Base + 768);
  ASSERT_EQ(call(ServiceKind::Ioctl, Client, 0xc0306201, Base + 256), 0u);
  R = read(Base + 768, 128);
  EXPECT_EQ(llvm::support::endian::read32le(R.data() + 4), 0x7206u);
  EXPECT_EQ(llvm::support::endian::read32le(R.data() + 8), 0x80407203u);
  EXPECT_EQ(llvm::support::endian::read64le(R.data() + 60), ClientMap);
  EXPECT_EQ(read(ClientMap, 3), std::vector<uint8_t>({0x78, 0x56, 0x34}));
  EXPECT_FALSE(
      llvm::cantFail(cpu().canAccess(ClientMap, 3, Write | UserAccessible)));
  auto Connection = Kernel->binderConnection(Client);
  ASSERT_NE(Connection, nullptr);
  const auto Alias = call(ServiceKind::Dup, Client, 0, 0);
  ASSERT_TRUE(Alias);
  EXPECT_EQ(Kernel->binderConnection(*Alias), Connection);
  auto View = Kernel->binderBuffer(*Connection, ClientMap, 3);
  ASSERT_TRUE(View);
  EXPECT_EQ(std::vector<uint8_t>(View->begin(), View->end()),
            std::vector<uint8_t>({0x78, 0x56, 0x34}));
  EXPECT_FALSE(Kernel->binderBuffer(*Connection, ClientMap, 4));
  std::vector<uint8_t> Free(8);
  llvm::support::endian::write64le(Free.data(), ClientMap);
  llvm::cantFail(cpu().write(Base + 1024, Free));
  ASSERT_EQ(Send(*Alias, 0x40086303, Base + 1024, 8), 0u);
  EXPECT_FALSE(Kernel->binderBuffer(*Connection, ClientMap, 3));
  ASSERT_EQ(call(ServiceKind::Close, Client, 0, 0), 0u);
  EXPECT_FALSE(Kernel->binderConnection(Client));
  EXPECT_EQ(Kernel->binderConnection(*Alias), Connection);
}
TEST_P(LinuxBinder, ModelCannotMixCapturedTransactionsOrMappings) {
  for (unsigned Case = 0; Case < 3; ++Case) {
    auto Input = *Options.LinuxFiles;
    auto &D = Input.Devices.at("/dev/binder");
    if (Case == 0)
      D.Ioctls.push_back({0xc0046209, 0, {}, {}});
    if (Case == 1)
      D.ReadOnlyMappings.push_back({0, std::vector<uint8_t>(4096), {}});
    if (Case == 2)
      D.KGSL = LinuxKGSLContexts{0, 1};
    auto E = linux_model::validateFileOptions(Input);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
}
INSTANTIATE_TEST_SUITE_P(Architectures, LinuxBinder,
                         testing::Values(GuestArchitecture::X64,
                                         GuestArchitecture::AArch64));
} // namespace
} // namespace neverd::emulation
