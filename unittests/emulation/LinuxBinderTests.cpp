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

#include <utility>

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
  linux_model::ThreadContext *Task = nullptr;
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
    return llvm::cantFail(
        Kernel->handle(K, {0, 0, {A, B, C, 0, 0, 0}, {}}, Task));
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
  std::optional<uint64_t> send(uint64_t FD, uint32_t Command,
                               llvm::ArrayRef<uint8_t> Body = {}) {
    std::vector<uint8_t> Bytes(4 + Body.size());
    llvm::support::endian::write32le(Bytes.data(), Command);
    std::copy(Body.begin(), Body.end(), Bytes.begin() + 4);
    llvm::cantFail(cpu().write(Base + 512, Bytes));
    record(Bytes.size(), 0, Base + 512, 0, 0);
    return call(ServiceKind::Ioctl, FD, 0xc0306201, Base + 256);
  }
  std::optional<uint64_t> ref(uint64_t FD, uint32_t Command, uint64_t Value,
                              std::optional<uint64_t> Cookie = {}) {
    uint8_t Bytes[16];
    llvm::support::endian::write64le(Bytes, Value);
    llvm::support::endian::write64le(Bytes + 8, Cookie.value_or(0));
    return send(FD, Command,
                llvm::ArrayRef(Bytes).take_front(Cookie                  ? 16
                                                 : Command == 0x40086303 ? 8
                                                                         : 4));
  }
  std::optional<uint64_t> tx(uint64_t FD, bool Reply, uint32_t Handle,
                             llvm::ArrayRef<uint8_t> Payload = {},
                             llvm::ArrayRef<uint64_t> Objects = {}) {
    uint8_t Bytes[64]{};
    llvm::support::endian::write32le(Bytes, Handle);
    llvm::support::endian::write32le(Bytes + 16, 19);
    llvm::support::endian::write64le(Bytes + 32, Payload.size());
    llvm::support::endian::write64le(Bytes + 40, Objects.size() * 8);
    llvm::support::endian::write64le(Bytes + 48, Base + 2048);
    llvm::support::endian::write64le(Bytes + 56, Base + 3072);
    llvm::cantFail(cpu().write(Base + 2048, Payload));
    std::vector<uint8_t> Offsets(Objects.size() * 8);
    for (size_t I = 0; I != Objects.size(); ++I)
      llvm::support::endian::write64le(Offsets.data() + I * 8, Objects[I]);
    llvm::cantFail(cpu().write(Base + 3072, Offsets));
    return send(FD, Reply ? 0x40406301 : 0x40406300, Bytes);
  }
  std::vector<uint8_t> receive(uint64_t FD) {
    record(0, 0, 0, 512, Base + 4096);
    EXPECT_EQ(call(ServiceKind::Ioctl, FD, 0xc0306201, Base + 256), 0u)
        << Report.Diagnostic;
    return read(Base + 4096,
                llvm::support::endian::read64le(read(Base + 288, 8).data()));
  }
  static std::vector<uint8_t> object(uint32_t Type, uint64_t Pointer,
                                     uint64_t Cookie = 0) {
    std::vector<uint8_t> Bytes(24);
    llvm::support::endian::write32le(Bytes.data(), Type);
    llvm::support::endian::write32le(Bytes.data() + 4, 0x17f);
    llvm::support::endian::write64le(Bytes.data() + 8, Pointer);
    llvm::support::endian::write64le(Bytes.data() + 16, Cookie);
    return Bytes;
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
  for (uint32_t Flags : {1u, 8u, 0x80000010u}) {
    SCOPED_TRACE(Flags);
    auto Invalid = Tx;
    llvm::support::endian::write32le(Invalid.data() + 20, Flags);
    llvm::cantFail(cpu().write(Base + 1024, Invalid));
    ASSERT_FALSE(Send(Client, 0x40406300, Base + 1024, 64));
    EXPECT_NE(Report.Diagnostic.find("synchronous profile"), std::string::npos)
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
  EXPECT_EQ(std::vector<uint8_t>(View->Bytes.begin(), View->Bytes.end()),
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
TEST_P(LinuxBinder, StrongObjectsPreserveIdentityAcrossRepliesAndReturnHome) {
  const auto Server = open(), Client = open();
  const auto SM = map(Server, 4096).value(), CM = map(Client, 4096).value();
  auto S = Kernel->binderConnection(Server),
       C = Kernel->binderConnection(Client);
  ASSERT_EQ(call(ServiceKind::Ioctl, Server, 0x40046207), 0u);
  ASSERT_EQ(send(Server, 0x630c), 0u);
  ASSERT_EQ(tx(Client, false, 0), 0u);
  ASSERT_EQ(receive(Server).size(), 72u);
  auto Payload = object(0x73622a85, 0x10203040, 0x50607080);
  auto Twice = Payload;
  Twice.insert(Twice.end(), Payload.begin(), Payload.end());
  ASSERT_EQ(tx(Server, true, 0, Twice, {0, 24}), 0u);
  auto Controls = receive(Server);
  ASSERT_EQ(Controls.size(), 48u);
  EXPECT_EQ(llvm::support::endian::read32le(Controls.data() + 4), 0x80107207u);
  EXPECT_EQ(llvm::support::endian::read32le(Controls.data() + 24), 0x80107208u);
  EXPECT_EQ(llvm::support::endian::read64le(Controls.data() + 8), 0x10203040u);
  EXPECT_EQ(llvm::support::endian::read64le(Controls.data() + 16), 0x50607080u);
  ASSERT_EQ(ref(Server, 0x40106308, 0x10203040, 0x50607080), 0u);
  ASSERT_EQ(ref(Server, 0x40106309, 0x10203040, 0x50607080), 0u);
  ASSERT_EQ(receive(Client).size(), 76u);
  auto View = Kernel->binderBuffer(*C, CM, 48);
  ASSERT_TRUE(View);
  EXPECT_EQ(View->Lease->Objects, (std::vector<uint64_t>{0, 24}));
  EXPECT_EQ(read(CM, 24), object(0x73682a85, 1));
  EXPECT_EQ(read(CM + 24, 24), object(0x73682a85, 1));
  EXPECT_EQ(read(CM + 48, 16), (std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 0, 24,
                                                     0, 0, 0, 0, 0, 0, 0}));
  ASSERT_EQ(C->BinderHandles.size(), 1u);
  EXPECT_EQ(C->BinderHandles.at(1).Strong, 2u);
  ASSERT_EQ(ref(Client, 0x40046304, 1), 0u);
  ASSERT_EQ(ref(Client, 0x40046305, 1), 0u);
  ASSERT_EQ(ref(Client, 0x40086303, CM), 0u);
  EXPECT_EQ(C->BinderHandles.at(1).Strong, 1u);
  ASSERT_EQ(ref(Server, 0x40086303, SM), 0u);
  ASSERT_EQ(tx(Client, false, 1, object(0x73682a85, 1), {0}), 0u);
  auto Delivered = receive(Server);
  ASSERT_EQ(Delivered.size(), 72u);
  EXPECT_EQ(llvm::support::endian::read64le(Delivered.data() + 8), 0x10203040u);
  EXPECT_EQ(llvm::support::endian::read64le(Delivered.data() + 16),
            0x50607080u);
  EXPECT_EQ(read(SM, 24), Payload);
  EXPECT_EQ(S->BinderNodes.at(0x10203040)->LocalStrong, 2u);
  ASSERT_EQ(tx(Server, true, 0), 0u);
  ASSERT_EQ(ref(Server, 0x40086303, SM), 0u);
  ASSERT_EQ(receive(Server).size(), 8u);
  ASSERT_EQ(receive(Client).size(), 76u);
  auto NewView = Kernel->binderBuffer(*C, CM, 0);
  ASSERT_TRUE(NewView);
  EXPECT_NE(NewView->Lease, View->Lease);
  ASSERT_EQ(ref(Client, 0x40086303, CM), 0u);
  ASSERT_EQ(ref(Client, 0x40046306, 1), 0u);
  EXPECT_FALSE(ref(Client, 0x40046305,
                   1)); // A weak-only ref cannot recreate a strong node.
  ASSERT_EQ(ref(Client, 0x40046307, 1), 0u);
  EXPECT_TRUE(C->BinderHandles.empty());
  Controls = receive(Server);
  ASSERT_EQ(Controls.size(), 44u);
  EXPECT_EQ(llvm::support::endian::read32le(Controls.data() + 4), 0x80107209u);
  EXPECT_EQ(llvm::support::endian::read32le(Controls.data() + 24), 0x8010720au);
  EXPECT_FALSE(S->BinderNodes.contains(0x10203040));
}

TEST_P(LinuxBinder, LooperReadinessMatchesActualProcessAndPrivateWork) {
  const auto Server = open(), Client = open();
  ASSERT_TRUE(map(Server, 4096));
  ASSERT_TRUE(map(Client, 4096));
  ASSERT_EQ(call(ServiceKind::Ioctl, Server, 0x40046207), 0u);
  auto S = Kernel->binderConnection(Server);
  auto C = Kernel->binderConnection(Client);
  linux_model::ThreadContext Sender, OtherSender, Receiver, OtherReceiver;
  Sender.ID = 101;
  OtherSender.ID = 102;
  Receiver.ID = 201;
  OtherReceiver.ID = 202;
  Task = &Sender;
  ASSERT_EQ(tx(Client, false, 0), 0u);
  EXPECT_FALSE(C->hasReadyBinderThreadWork(Sender.ID));
  EXPECT_FALSE(S->hasReadyBinderThreadWork(Receiver.ID));
  Task = &Receiver;
  ASSERT_EQ(send(Server, 0x630c), 0u);
  EXPECT_TRUE(S->hasReadyBinderThreadWork(Receiver.ID));
  auto First = receive(Server);
  ASSERT_EQ(First.size(), 72u);
  EXPECT_EQ(llvm::support::endian::read32le(First.data() + 4), 0x80407202u);
  Task = &OtherSender;
  ASSERT_EQ(tx(Client, false, 0), 0u);
  // The busy receiver cannot take the second request. Another entered looper
  // can, without a private BR_REPLY to wake it.
  EXPECT_FALSE(S->hasReadyBinderThreadWork(Receiver.ID));
  EXPECT_FALSE(S->hasReadyBinderThreadWork(OtherReceiver.ID));
  Task = &OtherReceiver;
  ASSERT_EQ(send(Server, 0x630c), 0u);
  EXPECT_TRUE(S->hasReadyBinderThreadWork(OtherReceiver.ID));
  ASSERT_EQ(receive(Server).size(), 72u);
  EXPECT_FALSE(S->hasReadyBinderThreadWork(OtherReceiver.ID));
  ASSERT_EQ(tx(Server, true, 0), 0u);
  EXPECT_TRUE(S->hasReadyBinderThreadWork(OtherReceiver.ID));
  EXPECT_TRUE(C->hasReadyBinderThreadWork(OtherSender.ID));
  EXPECT_FALSE(C->hasReadyBinderThreadWork(Sender.ID));
  auto Acknowledged = receive(Server);
  ASSERT_EQ(Acknowledged.size(), 8u);
  EXPECT_EQ(llvm::support::endian::read32le(Acknowledged.data() + 4), 0x7206u);
  Task = &OtherSender;
  ASSERT_EQ(receive(Client).size(), 76u);
  Task = &Receiver;
  ASSERT_EQ(tx(Server, true, 0), 0u);
  Task = &Sender;
  ASSERT_EQ(receive(Client).size(), 76u);
}

TEST_P(LinuxBinder, RepeatedExportKeepsPendingNodeWorkOnItsFirstOwnerThread) {
  const auto Server = open(), Client = open();
  ASSERT_TRUE(map(Server, 4096));
  ASSERT_TRUE(map(Client, 4096));
  ASSERT_EQ(call(ServiceKind::Ioctl, Server, 0x40046207), 0u);
  linux_model::ThreadContext First, Second;
  First.ID = 101;
  Second.ID = 102;
  const auto Payload = object(0x73622a85, 0x1110, 0x2220);
  for (auto *Owner : {&First, &Second}) {
    Task = nullptr;
    ASSERT_EQ(tx(Client, false, 0), 0u);
    Task = Owner;
    ASSERT_EQ(send(Server, 0x630c), 0u);
    ASSERT_EQ(receive(Server).size(), 72u);
    ASSERT_EQ(tx(Server, true, 0, Payload, {0}), 0u);
    Task = nullptr;
    ASSERT_EQ(receive(Client).size(), 76u);
  }
  // The second export increments an existing strong reference. It does not
  // acquire the node again or move the first thread's pending notifications.
  Task = &First;
  auto Controls = receive(Server);
  ASSERT_EQ(Controls.size(), 48u);
  EXPECT_EQ(llvm::support::endian::read32le(Controls.data() + 4), 0x80107207u);
  EXPECT_EQ(llvm::support::endian::read32le(Controls.data() + 24), 0x80107208u);
  EXPECT_EQ(llvm::support::endian::read32le(Controls.data() + 44), 0x7206u);
  ASSERT_EQ(ref(Server, 0x40106308, 0x1110, 0x2220), 0u);
  ASSERT_EQ(ref(Server, 0x40106309, 0x1110, 0x2220), 0u);
  Task = &Second;
  Controls = receive(Server);
  ASSERT_EQ(Controls.size(), 8u);
  EXPECT_EQ(llvm::support::endian::read32le(Controls.data() + 4), 0x7206u);
  Task = nullptr;
  auto C = Kernel->binderConnection(Client);
  ASSERT_EQ(C->BinderHandles.size(), 1u);
  EXPECT_EQ(C->BinderHandles.at(1).Strong, 2u);
  EXPECT_EQ(C->BinderHandles.at(1).Node->InternalStrong, 1u);
}

TEST_P(LinuxBinder, ForwardedHandlesAreRenumberedAndDeadOwnersAreNotRetained) {
  const auto Manager = open(), Owner = open(), Client = open();
  const auto MM = map(Manager, 4096).value(), OM = map(Owner, 4096).value(),
             CM = map(Client, 4096).value();
  auto M = Kernel->binderConnection(Manager);
  auto O = Kernel->binderConnection(Owner);
  auto C = Kernel->binderConnection(Client);
  ASSERT_EQ(call(ServiceKind::Ioctl, Manager, 0x40046207), 0u);
  ASSERT_EQ(send(Manager, 0x630c), 0u);
  ASSERT_EQ(send(Owner, 0x630c), 0u);
  ASSERT_EQ(tx(Owner, false, 0, object(0x73622a85, 0x1110, 0x2220), {0}), 0u);
  ASSERT_EQ(receive(Manager).size(), 72u);
  record(0, 0, 0, 512, Base + 4096);
  EXPECT_EQ(call(ServiceKind::Ioctl, Owner, 0xc0306201, Base + 256),
            uint64_t(-11));
  ASSERT_EQ(ref(Manager, 0x40046305, 1), 0u);
  ASSERT_EQ(ref(Manager, 0x40086303, MM), 0u);
  ASSERT_EQ(tx(Manager, true, 0), 0u);
  ASSERT_EQ(receive(Manager).size(), 8u);
  ASSERT_EQ(receive(Owner).size(), 116u);
  ASSERT_EQ(ref(Owner, 0x40106308, 0x1110, 0x2220), 0u);
  ASSERT_EQ(ref(Owner, 0x40106309, 0x1110, 0x2220), 0u);
  ASSERT_EQ(ref(Owner, 0x40086303, OM), 0u);
  ASSERT_EQ(tx(Client, false, 0), 0u);
  ASSERT_EQ(receive(Manager).size(), 72u);
  auto Payload = object(0x73622a85, 0x3330, 0x4440);
  const auto Forwarded = object(0x73682a85, 1);
  Payload.insert(Payload.end(), Forwarded.begin(), Forwarded.end());
  ASSERT_EQ(tx(Manager, true, 0, Payload, {0, 24}), 0u);
  ASSERT_EQ(receive(Manager).size(), 48u);
  ASSERT_EQ(ref(Manager, 0x40106308, 0x3330, 0x4440), 0u);
  ASSERT_EQ(ref(Manager, 0x40106309, 0x3330, 0x4440), 0u);
  ASSERT_EQ(ref(Manager, 0x40086303, MM), 0u);
  ASSERT_EQ(receive(Client).size(), 76u);
  EXPECT_EQ(read(CM, 24), object(0x73682a85, 1));
  EXPECT_EQ(read(CM + 24, 24), object(0x73682a85, 2));
  EXPECT_EQ(C->BinderHandles.at(2).Node, M->BinderHandles.at(1).Node);
  ASSERT_EQ(ref(Client, 0x40046305, 2), 0u);
  ASSERT_EQ(ref(Client, 0x40086303, CM), 0u);
  ASSERT_EQ(tx(Client, false, 2), 0u);
  auto Request = receive(Owner);
  ASSERT_EQ(Request.size(), 72u);
  EXPECT_EQ(llvm::support::endian::read64le(Request.data() + 8), 0x1110u);
  EXPECT_EQ(llvm::support::endian::read64le(Request.data() + 16), 0x2220u);
  ASSERT_EQ(tx(Owner, true, 0), 0u);
  ASSERT_EQ(ref(Owner, 0x40086303, OM), 0u);
  ASSERT_EQ(receive(Owner).size(), 8u);
  ASSERT_EQ(receive(Client).size(), 76u);
  ASSERT_EQ(ref(Client, 0x40086303, CM), 0u);
  std::weak_ptr<linux_model::LinuxDeviceState> Weak = O;
  O.reset();
  ASSERT_EQ(call(ServiceKind::Close, Owner), 0u);
  ASSERT_EQ(call(ServiceKind::Munmap, OM, 4096), 0u);
  EXPECT_TRUE(Weak.expired());
  ASSERT_EQ(tx(Client, false, 2), 0u);
  auto Dead = receive(Client);
  ASSERT_EQ(Dead.size(), 8u);
  EXPECT_EQ(llvm::support::endian::read32le(Dead.data() + 4), 0x7205u);
}

TEST_P(LinuxBinder,
       FinalOpenRetirementReleasesReferencesAndFreeRejectsOverRelease) {
  const auto Server = open(), Client = open();
  const auto SM = map(Server, 4096).value(), CM = map(Client, 4096).value();
  auto S = Kernel->binderConnection(Server),
       C = Kernel->binderConnection(Client);
  ASSERT_EQ(call(ServiceKind::Ioctl, Server, 0x40046207), 0u);
  ASSERT_EQ(send(Server, 0x630c), 0u);
  ASSERT_EQ(tx(Client, false, 0), 0u);
  ASSERT_EQ(receive(Server).size(), 72u);
  ASSERT_EQ(tx(Server, true, 0, object(0x73622a85, 0x1110, 0x2220), {0}), 0u);
  ASSERT_EQ(receive(Server).size(), 48u);
  EXPECT_FALSE(ref(Server, 0x40106308, 0x1110, 0x9990));
  EXPECT_TRUE(S->BinderNodes.at(0x1110)->PendingWeak);
  ASSERT_EQ(ref(Server, 0x40106308, 0x1110, 0x2220), 0u);
  ASSERT_EQ(ref(Server, 0x40106309, 0x1110, 0x2220), 0u);
  ASSERT_EQ(ref(Server, 0x40086303, SM), 0u);
  ASSERT_EQ(receive(Client).size(), 76u);
  // A raw caller can consume a reference owned by a buffer. Later free must
  // diagnose that protocol misuse instead of dereferencing a vanished handle.
  ASSERT_EQ(ref(Client, 0x40046306, 1), 0u);
  EXPECT_FALSE(ref(Client, 0x40086303, CM));
  EXPECT_EQ(C->BinderBuffers.size(), 1u);
  ASSERT_EQ(receive(Server).size(), 44u);
  ASSERT_EQ(tx(Client, false, 0), 0u);
  ASSERT_EQ(receive(Server).size(), 72u);
  ASSERT_EQ(tx(Server, true, 0, object(0x73622a85, 0x1110, 0x2220), {0}), 0u);
  ASSERT_EQ(receive(Server).size(), 48u);
  ASSERT_EQ(ref(Server, 0x40106308, 0x1110, 0x2220), 0u);
  ASSERT_EQ(ref(Server, 0x40106309, 0x1110, 0x2220), 0u);
  ASSERT_EQ(ref(Server, 0x40086303, SM), 0u);
  ASSERT_EQ(receive(Client).size(), 76u);
  C.reset();
  ASSERT_EQ(call(ServiceKind::Close, Client), 0u);
  ASSERT_EQ(call(ServiceKind::Munmap, CM, 4096), 0u);
  auto Controls = receive(Server);
  ASSERT_EQ(Controls.size(), 44u);
  EXPECT_EQ(llvm::support::endian::read32le(Controls.data() + 4), 0x80107209u);
  EXPECT_EQ(llvm::support::endian::read32le(Controls.data() + 24), 0x8010720au);
  EXPECT_FALSE(S->BinderNodes.contains(0x1110));
}

TEST_P(LinuxBinder, InvalidObjectsReturnFailedReplyAndReleaseTranslatedPrefix) {
  const auto Server = open(), Client = open();
  const auto SM = map(Server, 4096).value();
  auto S = Kernel->binderConnection(Server),
       C = Kernel->binderConnection(Client);
  ASSERT_EQ(call(ServiceKind::Ioctl, Server, 0x40046207), 0u);
  const auto Local = object(0x73622a85, 0x1234, 0x5678);
  auto Twice = Local;
  Twice.insert(Twice.end(), Local.begin(), Local.end());
  auto BadCookie = Twice;
  llvm::support::endian::write64le(BadCookie.data() + 40, 0x9999);
  auto BadTag = Twice;
  llvm::support::endian::write32le(BadTag.data() + 24, 0xffffffff);
  struct Invalid {
    std::vector<uint8_t> Payload;
    std::vector<uint64_t> Offsets;
    std::optional<uint64_t> Translated;
  };
  std::vector<Invalid> Cases = {{{}, {0}, {}},
                                {Local, {1}, {}},
                                {Local, {UINT64_MAX}, {}},
                                {{0x85, 0x2a, 0x62}, {0}, {}},
                                {object(0xffffffff, 0), {0}, {}},
                                {object(0x73682a85, 77), {0}, {}},
                                {Local, {0, 0}, 0},
                                {Twice, {0, 4}, 0},
                                {Twice, {24, 0}, 24},
                                {BadCookie, {0, 24}, 0},
                                {BadTag, {0, 24}, 0}};
  // Known but incomplete objects are malformed, even when their valid forms
  // require an unsupported translation profile.
  for (const auto &[Type, Size] : {std::pair{0x73622a85u, 24u},
                                   {0x77622a85u, 24u},
                                   {0x77682a85u, 24u},
                                   {0x66642a85u, 24u},
                                   {0x70742a85u, 40u},
                                   {0x66646185u, 32u}}) {
    auto Short = object(Type, 0);
    Short.resize(Size - 1);
    Cases.push_back({std::move(Short), {0}, {}});
  }
  for (size_t I = 0; I != Cases.size(); ++I) {
    SCOPED_TRACE(I);
    const auto &[Payload, Offsets, Translated] = Cases[I];
    ASSERT_EQ(tx(Client, false, 0, Payload, Offsets), 0u) << Report.Diagnostic;
    EXPECT_EQ(llvm::support::endian::read64le(read(Base + 264, 8).data()), 68u);
    EXPECT_EQ(receive(Client),
              (std::vector<uint8_t>{0x0c, 0x72, 0, 0, 0x11, 0x72, 0, 0}));
    EXPECT_TRUE(Report.Diagnostic.empty());
    EXPECT_TRUE(C->BinderNodes.empty());
    EXPECT_TRUE(S->BinderHandles.empty());
    EXPECT_TRUE(C->BinderOutgoing.empty());
    EXPECT_TRUE(S->BinderProcessWork.empty());
    EXPECT_TRUE(S->BinderBuffers.empty());
    // SET_CONTEXT_MGR owns one permanent local strong reference.
    EXPECT_EQ(S->BinderNodes.at(0)->LocalStrong, 1u);
    auto Expected = Payload;
    if (Translated) {
      const auto Handle = object(0x73682a85, 1);
      std::copy(Handle.begin(), Handle.end(), Expected.begin() + *Translated);
    }
    EXPECT_EQ(read(SM, Expected.size()), Expected);
    const auto Table =
        read(SM + ((Payload.size() + 7) & ~uint64_t(7)), Offsets.size() * 8);
    for (size_t N = 0; N != Offsets.size(); ++N)
      EXPECT_EQ(llvm::support::endian::read64le(Table.data() + N * 8),
                Offsets[N]);
  }
  // A failed buffer never becomes a lease and its first arena slot is reusable.
  ASSERT_EQ(send(Server, 0x630c), 0u);
  ASSERT_EQ(tx(Client, false, 0, Local, {0}), 0u);
  const auto Request = receive(Server);
  ASSERT_EQ(Request.size(), 72u);
  EXPECT_EQ(llvm::support::endian::read64le(Request.data() + 56), SM);
  EXPECT_EQ(read(SM, 24), object(0x73682a85, 1));
  EXPECT_TRUE(Kernel->binderBuffer(*S, SM, 24));
}

TEST_P(LinuxBinder, UnmodeledObjectKindsStopBeforeTranslationEffects) {
  const auto Server = open(), Client = open();
  const auto SM = map(Server, 4096).value();
  auto S = Kernel->binderConnection(Server),
       C = Kernel->binderConnection(Client);
  ASSERT_EQ(call(ServiceKind::Ioctl, Server, 0x40046207), 0u);
  for (const auto &[Type, Size] : {std::pair{0x77622a85u, 24u},
                                   {0x77682a85u, 24u},
                                   {0x66642a85u, 24u},
                                   {0x70742a85u, 40u},
                                   {0x66646185u, 32u}}) {
    SCOPED_TRACE(Type);
    auto Payload = object(0x73622a85, 0x1234, 0x5678);
    auto Other = object(Type, 0);
    Other.resize(Size);
    Payload.insert(Payload.end(), Other.begin(), Other.end());
    EXPECT_FALSE(tx(Client, false, 0, Payload, {0, 24}));
    EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(Report.Diagnostic.find("translation is not modeled"),
              std::string::npos);
    EXPECT_TRUE(C->BinderNodes.empty());
    EXPECT_TRUE(S->BinderHandles.empty());
    EXPECT_TRUE(C->BinderOutgoing.empty());
    EXPECT_TRUE(S->BinderProcessWork.empty());
    EXPECT_EQ(read(SM, 4096), std::vector<uint8_t>(4096));
    EXPECT_EQ(llvm::support::endian::read64le(read(Base + 264, 8).data()), 0u);
  }
}

TEST_P(LinuxBinder,
       UnalignedObjectTablesFailAfterCopyWithoutObjectTranslation) {
  const auto Server = open(), Client = open();
  const auto SM = map(Server, 4096).value();
  auto S = Kernel->binderConnection(Server),
       C = Kernel->binderConnection(Client);
  ASSERT_EQ(call(ServiceKind::Ioctl, Server, 0x40046207), 0u);
  const auto Payload = object(0x73622a85, 0x1234, 0x5678);
  llvm::cantFail(cpu().write(Base + 2048, Payload));
  const uint8_t Offsets[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0xa5};
  llvm::cantFail(cpu().write(Base + 3072, Offsets));
  uint8_t Tx[64]{};
  llvm::support::endian::write64le(Tx + 32, 24);
  llvm::support::endian::write64le(Tx + 48, Base + 2048);
  llvm::support::endian::write64le(Tx + 56, Base + 3072);
  for (uint64_t Size : {1u, 7u, 9u}) {
    SCOPED_TRACE(Size);
    llvm::support::endian::write64le(Tx + 40, Size);
    ASSERT_EQ(send(Client, 0x40406300, Tx), 0u) << Report.Diagnostic;
    EXPECT_EQ(receive(Client),
              (std::vector<uint8_t>{0x0c, 0x72, 0, 0, 0x11, 0x72, 0, 0}));
    EXPECT_EQ(read(SM, 24), Payload);
    EXPECT_EQ(read(SM + 24, Size),
              std::vector<uint8_t>(Offsets, Offsets + Size));
    EXPECT_TRUE(S->BinderBuffers.empty());
    EXPECT_TRUE(S->BinderHandles.empty());
    EXPECT_TRUE(C->BinderNodes.empty());
  }
  llvm::support::endian::write64le(Tx + 40, UINT64_MAX);
  EXPECT_FALSE(send(Client, 0x40406300, Tx));
  EXPECT_NE(Report.Diagnostic.find("object table budget"), std::string::npos);
}

TEST_P(LinuxBinder, InvalidReplyRetiresCallAndSeparatesBothErrorSlots) {
  const auto Server = open(), Client = open();
  const auto SM = map(Server, 4096).value(), CM = map(Client, 4096).value();
  auto S = Kernel->binderConnection(Server),
       C = Kernel->binderConnection(Client);
  ASSERT_EQ(call(ServiceKind::Ioctl, Server, 0x40046207), 0u);
  linux_model::ThreadContext Caller, Callee;
  Caller.ID = 101;
  Callee.ID = 201;
  Task = &Caller;
  ASSERT_EQ(tx(Client, false, 0), 0u);
  Task = &Callee;
  ASSERT_EQ(send(Server, 0x630c), 0u);
  ASSERT_EQ(receive(Server).size(), 72u);
  const auto Bad = object(0xffffffff, 0);
  llvm::cantFail(cpu().write(Base + 2048, Bad));
  llvm::cantFail(cpu().write(Base + 3072, std::vector<uint8_t>(8)));
  uint8_t Commands[80]{};
  llvm::support::endian::write32le(Commands, 0x40406301);
  llvm::support::endian::write64le(Commands + 36, 24);
  llvm::support::endian::write64le(Commands + 44, 8);
  llvm::support::endian::write64le(Commands + 52, Base + 2048);
  llvm::support::endian::write64le(Commands + 60, Base + 3072);
  llvm::support::endian::write32le(Commands + 68, 0x40086303);
  llvm::support::endian::write64le(Commands + 72, SM);
  llvm::cantFail(cpu().write(Base + 512, Commands));
  record(80, 0, Base + 512, 0, 0);
  ASSERT_EQ(call(ServiceKind::Ioctl, Server, 0xc0306201, Base + 256), 0u)
      << Report.Diagnostic;
  EXPECT_EQ(llvm::support::endian::read64le(read(Base + 264, 8).data()), 68u);
  EXPECT_TRUE(S->BinderIncoming.empty());
  EXPECT_TRUE(C->BinderOutgoing.empty());
  EXPECT_TRUE(C->BinderBuffers.empty());
  EXPECT_TRUE(Kernel->binderBuffer(*S, SM, 0));
  EXPECT_EQ(read(CM, 24), Bad);
  EXPECT_TRUE(S->hasReadyBinderThreadWork(Callee.ID));
  EXPECT_TRUE(C->hasReadyBinderThreadWork(Caller.ID));
  EXPECT_FALSE(S->hasReadyBinderThreadWork(Caller.ID));
  EXPECT_FALSE(C->hasReadyBinderThreadWork(Callee.ID));
  // return_error holds COMPLETE on the server. It prevents even touching the
  // next write's invalid address, and a short read cannot consume that slot.
  record(4, 0, 1, 71, Base + 4096);
  ASSERT_EQ(call(ServiceKind::Ioctl, Server, 0xc0306201, Base + 256), 0u);
  EXPECT_EQ(llvm::support::endian::read64le(read(Base + 264, 8).data()), 0u);
  EXPECT_EQ(llvm::support::endian::read64le(read(Base + 288, 8).data()), 4u);
  record(80, 68, Base + 512, 72, Base + 4096);
  ASSERT_EQ(call(ServiceKind::Ioctl, Server, 0xc0306201, Base + 256), 0u);
  EXPECT_EQ(llvm::support::endian::read64le(read(Base + 264, 8).data()), 68u);
  EXPECT_EQ(read(Base + 4096, 8),
            (std::vector<uint8_t>{0x0c, 0x72, 0, 0, 6, 0x72, 0, 0}));
  record(80, 68, Base + 512, 0, 0);
  ASSERT_EQ(call(ServiceKind::Ioctl, Server, 0xc0306201, Base + 256), 0u);
  EXPECT_EQ(llvm::support::endian::read64le(read(Base + 264, 8).data()), 80u);
  EXPECT_TRUE(S->BinderBuffers.empty());

  Task = &Caller;
  record(0, 0, 0, 72, Base + 4096);
  ASSERT_EQ(call(ServiceKind::Ioctl, Client, 0xc0306201, Base + 256), 0u);
  EXPECT_EQ(read(Base + 4096, 8),
            (std::vector<uint8_t>{0x0c, 0x72, 0, 0, 6, 0x72, 0, 0}));
  // The caller's unread reply_error must allow writes. Reading that old error
  // must not pop the new transaction that the caller has just started.
  ASSERT_EQ(tx(Client, false, 0), 0u);
  EXPECT_EQ(llvm::support::endian::read64le(read(Base + 264, 8).data()), 68u);
  record(0, 0, 0, 72, Base + 4096);
  ASSERT_EQ(call(ServiceKind::Ioctl, Client, 0xc0306201, Base + 256), 0u);
  EXPECT_EQ(read(Base + 4096, 8),
            (std::vector<uint8_t>{0x0c, 0x72, 0, 0, 0x11, 0x72, 0, 0}));
  EXPECT_TRUE(C->BinderOutgoing.contains(Caller.ID));
  Task = &Callee;
  ASSERT_EQ(receive(Server).size(), 72u);
  ASSERT_EQ(tx(Server, true, 0), 0u);
  // Ordinary successful COMPLETE does not block the following free command.
  ASSERT_EQ(ref(Server, 0x40086303, SM), 0u);
  EXPECT_TRUE(S->BinderBuffers.empty());
  ASSERT_EQ(receive(Server).size(), 8u);
  Task = &Caller;
  const auto Reply = receive(Client);
  ASSERT_EQ(Reply.size(), 76u);
  EXPECT_EQ(llvm::support::endian::read64le(Reply.data() + 60), CM);
  EXPECT_TRUE(C->BinderOutgoing.empty());
  ASSERT_EQ(ref(Client, 0x40086303, CM), 0u);
}

TEST_P(LinuxBinder, InvalidRepliesShareOneUnreadCallerErrorSlot) {
  const auto Server = open(), Client = open();
  const auto SM = map(Server, 4096).value();
  ASSERT_TRUE(map(Client, 4096));
  auto S = Kernel->binderConnection(Server),
       C = Kernel->binderConnection(Client);
  ASSERT_EQ(call(ServiceKind::Ioctl, Server, 0x40046207), 0u);
  ASSERT_EQ(send(Server, 0x630c), 0u);
  for (unsigned I = 0; I != 2; ++I) {
    SCOPED_TRACE(I);
    ASSERT_EQ(tx(Client, false, 0), 0u);
    ASSERT_EQ(receive(Server).size(), 72u);
    ASSERT_EQ(tx(Server, true, 0, object(0xffffffff, 0), {0}), 0u);
    EXPECT_EQ(receive(Server),
              (std::vector<uint8_t>{0x0c, 0x72, 0, 0, 6, 0x72, 0, 0}));
    ASSERT_EQ(ref(Server, 0x40086303, SM), 0u);
    EXPECT_TRUE(S->BinderIncoming.empty());
    EXPECT_TRUE(C->BinderOutgoing.empty());
  }
  EXPECT_EQ(receive(Client),
            (std::vector<uint8_t>{0x0c, 0x72, 0, 0, 6, 0x72, 0, 0, 0x11, 0x72,
                                  0, 0, 6, 0x72, 0, 0}));
  EXPECT_TRUE(C->BinderBuffers.empty());
}

TEST_P(LinuxBinder, InvalidObjectFailureStopsBeforeDecodingLaterCommands) {
  const auto Server = open(), Client = open();
  ASSERT_TRUE(map(Server, 4096));
  ASSERT_EQ(call(ServiceKind::Ioctl, Server, 0x40046207), 0u);
  ASSERT_EQ(tx(Client, false, 0, object(0xffffffff, 0), {0}), 0u);
  auto Commands = read(Base + 512, 68);
  ASSERT_EQ(receive(Client).size(), 8u);
  Commands.resize(72);
  for (uint32_t Tail : {0xffffffffu, 0x40406300u}) {
    SCOPED_TRACE(Tail);
    llvm::support::endian::write32le(Commands.data() + 68, Tail);
    llvm::cantFail(cpu().write(Base + 512, Commands));
    record(72, 0, Base + 512, 0, 0);
    ASSERT_EQ(call(ServiceKind::Ioctl, Client, 0xc0306201, Base + 256), 0u)
        << Report.Diagnostic;
    EXPECT_EQ(llvm::support::endian::read64le(read(Base + 264, 8).data()), 68u);
    EXPECT_EQ(receive(Client),
              (std::vector<uint8_t>{0x0c, 0x72, 0, 0, 0x11, 0x72, 0, 0}));
    // Only after consuming return_error does the unknown/incomplete next
    // command reach the existing unsupported protocol boundary.
    record(72, 68, Base + 512, 0, 0);
    EXPECT_FALSE(call(ServiceKind::Ioctl, Client, 0xc0306201, Base + 256));
    EXPECT_NE(
        Report.Diagnostic.find(Tail == 0xffffffffu ? "protocol" : "incomplete"),
        std::string::npos);
  }
}

TEST_P(LinuxBinder, InvalidObjectsPreservePreexistingLocalAndRemoteReferences) {
  const auto Server = open(), Client = open();
  const auto SM = map(Server, 4096).value(), CM = map(Client, 4096).value();
  auto S = Kernel->binderConnection(Server),
       C = Kernel->binderConnection(Client);
  ASSERT_EQ(call(ServiceKind::Ioctl, Server, 0x40046207), 0u);
  ASSERT_EQ(send(Server, 0x630c), 0u);
  ASSERT_EQ(tx(Client, false, 0), 0u);
  ASSERT_EQ(receive(Server).size(), 72u);
  const auto Local = object(0x73622a85, 0x1110, 0x2220);
  ASSERT_EQ(tx(Server, true, 0, Local, {0}), 0u);
  ASSERT_EQ(receive(Server).size(), 48u);
  ASSERT_EQ(ref(Server, 0x40106308, 0x1110, 0x2220), 0u);
  ASSERT_EQ(ref(Server, 0x40106309, 0x1110, 0x2220), 0u);
  ASSERT_EQ(ref(Server, 0x40086303, SM), 0u);
  ASSERT_EQ(receive(Client).size(), 76u);
  ASSERT_EQ(ref(Client, 0x40046305, 1), 0u);
  ASSERT_EQ(ref(Client, 0x40086303, CM), 0u);
  const auto Node = S->BinderNodes.at(0x1110);
  const auto Bad = object(0xffffffff, 0);
  auto Payload = object(0x73682a85, 1);
  Payload.insert(Payload.end(), Bad.begin(), Bad.end());
  ASSERT_EQ(tx(Client, false, 0, Payload, {0, 24}), 0u);
  EXPECT_EQ(receive(Client),
            (std::vector<uint8_t>{0x0c, 0x72, 0, 0, 0x11, 0x72, 0, 0}));
  EXPECT_EQ(read(SM, 24), Local); // Returned home before the later bad tag.
  EXPECT_EQ(Node->LocalStrong, 0u);
  EXPECT_EQ(Node->InternalStrong, 1u);
  EXPECT_EQ(C->BinderHandles.at(1).Strong, 1u);
  EXPECT_TRUE(S->BinderBuffers.empty());

  ASSERT_EQ(tx(Client, false, 1), 0u);
  ASSERT_EQ(receive(Server).size(), 72u);
  EXPECT_EQ(Node->LocalStrong, 1u); // Original request's target lease.
  Payload = Local;
  Payload.insert(Payload.end(), Bad.begin(), Bad.end());
  ASSERT_EQ(tx(Server, true, 0, Payload, {0, 24}), 0u);
  EXPECT_EQ(read(CM, 24), object(0x73682a85, 1));
  EXPECT_EQ(Node->LocalStrong, 1u);
  EXPECT_EQ(Node->InternalStrong, 1u);
  EXPECT_EQ(C->BinderHandles.at(1).Strong, 1u);
  EXPECT_TRUE(C->BinderBuffers.empty());
  EXPECT_TRUE(Kernel->binderBuffer(*S, SM, 0));
  EXPECT_EQ(receive(Server),
            (std::vector<uint8_t>{0x0c, 0x72, 0, 0, 6, 0x72, 0, 0}));
  EXPECT_EQ(receive(Client), (std::vector<uint8_t>{0x0c, 0x72, 0, 0, 6, 0x72, 0,
                                                   0, 0x11, 0x72, 0, 0}));
  ASSERT_EQ(ref(Server, 0x40086303, SM), 0u);
  EXPECT_EQ(Node->LocalStrong, 0u);
  ASSERT_EQ(ref(Client, 0x40046306, 1), 0u);
  EXPECT_EQ(receive(Server).size(), 44u);
  EXPECT_FALSE(S->BinderNodes.contains(0x1110));
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
