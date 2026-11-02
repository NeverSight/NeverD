//===- LinuxProcTests.cpp - Independent live proc and stat contracts ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Literal observations across both LP64 ABIs, with mutation between reads.
#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxServices.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
using linux_model::ServiceKind;
constexpr uint64_t Base = 0x20000000;
class LinuxProc : public testing::TestWithParam<GuestArchitecture> {
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
    Backend.emplace(llvm::cantFail(createExecutionBackend(C, 4 * 1024 * 1024)));
    llvm::cantFail(
        cpu().addressSpace()->map(Base, 8192, Read | Write | UserAccessible));
    Options.LinuxFiles.emplace();
    auto &F = *Options.LinuxFiles;
    F.Files["/data/value"] = {'a', 'b', 'c'};
    F.WriteLimits["/data/value"] = 16384;
    F.TrackWriteMetadata.insert("/data/value");
    LinuxFileMetadata M;
    M.Device = 0x801;
    M.Inode = 42;
    M.Mode = 0100640;
    M.LinkCount = 1;
    M.UID = 17;
    M.GID = 19;
    M.Size = 3;
    M.BlockSize = 4096;
    M.Blocks = 8;
    M.AccessTime = {7, 11};
    M.ModificationTime = {13, 17};
    M.ChangeTime = {19, 23};
    F.Metadata["/data/value"] = M;
    F.DynamicProc = LinuxProcOptions{"fixture", 12};
    Options.LinuxTime.emplace().Clocks[0] = {100, 900000000};
    Options.LinuxTime->AdvanceOnIdle = true;
    reset();
  }
  void reset() {
    llvm::cantFail(linux_model::validateFileOptions(*Options.LinuxFiles));
    Kernel = std::make_unique<linux_model::LinuxServices>(
        cpu(), Layout, Base + 8192, Options, Report);
    llvm::cantFail(Kernel->initializeProc());
  }
  std::optional<uint64_t> call(ServiceKind K, uint64_t A = 0, uint64_t B = 0,
                               uint64_t C = 0, uint64_t D = 0, uint64_t E = 0,
                               uint64_t F = 0) {
    return llvm::cantFail(Kernel->handle(K, {0, 0, {A, B, C, D, E, F}, {}}));
  }
  void enableVMANames() {
    Options.LinuxKernel.emplace().AnonymousVMANames =
        LinuxAnonymousVMANames::AndroidUserPointer;
    reset();
  }
  std::optional<uint64_t> prctl(uint64_t Address, uint64_t Size, uint64_t Name,
                                uint64_t Suboption = 0,
                                uint64_t Option = 0x53564d41) {
    const uint64_t Number = GetParam() == GuestArchitecture::X64 ? 157 : 167;
    return llvm::cantFail(Kernel->handle(
        {0, Number, {Option, Suboption, Address, Size, Name, 0}, {}}));
  }
  std::string maps() {
    const auto FD = open("/proc/self/maps");
    auto Text = read(FD);
    EXPECT_EQ(call(ServiceKind::Close, FD), 0u);
    return Text;
  }
  void path(llvm::StringRef Name) {
    std::vector<uint8_t> V(Name.begin(), Name.end());
    V.push_back(0);
    llvm::cantFail(cpu().write(Base, V));
  }
  uint64_t open(llvm::StringRef Name, unsigned Flags = 0) {
    path(Name);
    auto FD = call(ServiceKind::OpenAt, uint64_t(-100), Base, Flags);
    EXPECT_TRUE(FD) << Report.Diagnostic;
    return FD.value_or(UINT64_MAX);
  }
  std::vector<uint8_t> bytes(uint64_t At, unsigned Count) {
    std::vector<uint8_t> V(Count);
    llvm::cantFail(cpu().read(At, V));
    return V;
  }
  std::string read(uint64_t FD, unsigned Count = 2048) {
    auto N = call(ServiceKind::Read, FD, Base + 256, Count);
    EXPECT_TRUE(N) << Report.Diagnostic;
    if (!N || *N > Count)
      return "<read error>";
    auto B = bytes(Base + 256, *N);
    return std::string(B.begin(), B.end());
  }
  std::vector<std::string> entries(uint64_t FD, unsigned Count = 2048) {
    auto N = call(ServiceKind::Getdents64, FD, Base + 256, Count);
    EXPECT_TRUE(N) << Report.Diagnostic;
    if (!N || *N > Count)
      return {};
    auto B = bytes(Base + 256, *N);
    std::vector<std::string> Names;
    for (size_t At = 0; At + 19 <= B.size();) {
      Names.emplace_back(reinterpret_cast<const char *>(B.data() + At + 19));
      const auto Length = llvm::support::endian::read16le(B.data() + At + 16);
      if (!Length)
        break;
      At += Length;
    }
    return Names;
  }
  void stat(uint64_t FD, uint64_t Size, uint64_t Blocks, int64_t Seconds,
            int64_t Nanoseconds) {
    const bool X64 = GetParam() == GuestArchitecture::X64;
    const unsigned Length = X64 ? 144 : 128;
    llvm::cantFail(cpu().write(Base + 256, std::vector<uint8_t>(160, 0xa5)));
    ASSERT_EQ(call(ServiceKind::Fstat, FD, Base + 256), 0u);
    std::vector<uint8_t> Expected(160, 0xa5);
    std::fill_n(Expected.begin(), Length, 0);
    auto Put64 = [&](unsigned At, uint64_t V) {
      llvm::support::endian::write64le(Expected.data() + At, V);
    };
    auto Put32 = [&](unsigned At, uint32_t V) {
      llvm::support::endian::write32le(Expected.data() + At, V);
    };
    Put64(0, 0x801);
    Put64(8, 42);
    Put32(X64 ? 24 : 16, 0100640);
    if (X64)
      Put64(16, 1);
    else
      Put32(20, 1);
    Put32(X64 ? 28 : 24, 17);
    Put32(X64 ? 32 : 28, 19);
    Put64(48, Size);
    Put64(56, 4096);
    Put64(64, Blocks);
    Put64(72, 7);
    Put64(80, 11);
    Put64(88, Seconds);
    Put64(96, Nanoseconds);
    Put64(104, Seconds);
    Put64(112, Nanoseconds);
    EXPECT_EQ(bytes(Base + 256, 160), Expected);
    path("/data/value");
    ASSERT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 256), 0u);
    EXPECT_EQ(bytes(Base + 256, 160), Expected);
  }
};

TEST_P(LinuxProc, PrctlRequiresExplicitContractAndDoesNotGuessOtherOperations) {
  EXPECT_FALSE(prctl(Base, 4096, 0));
  EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Report.Diagnostic.find("anonymous_vma_names"), std::string::npos);
  enableVMANames();
  EXPECT_FALSE(prctl(Base, 4096, 0, 0, 15));
  EXPECT_EQ(Report.Diagnostic, "prctl operation is unmodeled");
  EXPECT_EQ(prctl(Base + 1, 0, 0), uint64_t(-22));
  EXPECT_EQ(prctl(Base, UINT64_MAX, 0), uint64_t(-22));
  EXPECT_EQ(prctl(UINT64_MAX - 4095, 4096, 0), uint64_t(-22));
  EXPECT_EQ(prctl(Base, 0, UINT64_MAX, UINT64_MAX), 0u);
  EXPECT_EQ(prctl(Base, 1, 0, uint64_t(1) << 32), uint64_t(-22));
  EXPECT_EQ(prctl(Base, 1, 0, 0, 0x7654321053564d41ULL), 0u);
  EXPECT_EQ(prctl(0x800000000000ULL, 1, 0), uint64_t(-12));
}

TEST_P(LinuxProc, PrctlBorrowsNamesAndMergesByPointerIdentity) {
  enableVMANames();
  const uint64_t Target = 0x30000000, Name = Base + 4096;
  ASSERT_EQ(call(ServiceKind::Mmap, Target, 12288, 3, 34, UINT64_MAX), Target);
  llvm::cantFail(cpu().write(Name, {'a', 'l', 'p', 'h', 'a', 0}));
  llvm::cantFail(cpu().write(Name + 256, {'a', 'l', 'p', 'h', 'a', 0}));
  auto &Space = *cpu().addressSpace();
  const auto Generation = Space.mappingGeneration();
  const auto Allocated = Space.physicalMemory()->allocatedBytes();
  const auto Mapped = Space.mappedBytes();
  ASSERT_EQ(prctl(Target, 12288, Name), 0u);
  EXPECT_NE(maps().find("[anon:alpha]"), std::string::npos);
  llvm::cantFail(cpu().write(Name, {'b', 'e', 't', 'a', 0}));
  auto Text = maps();
  EXPECT_NE(Text.find("[anon:beta]"), std::string::npos);
  EXPECT_NE(Text.find("30000000-30003000 rw-p"), std::string::npos);
  ASSERT_EQ(prctl(Target + 4096, 1, Name + 256), 0u);
  Text = maps();
  EXPECT_NE(Text.find("30000000-30001000 rw-p"), std::string::npos);
  EXPECT_NE(Text.find("30001000-30002000 rw-p"), std::string::npos);
  EXPECT_NE(Text.find("30002000-30003000 rw-p"), std::string::npos);
  EXPECT_NE(Text.find("[anon:alpha]"), std::string::npos);
  // Equal contents at distinct guest addresses must still split the VMA.
  llvm::cantFail(cpu().write(Name + 256, {'b', 'e', 't', 'a', 0}));
  EXPECT_NE(maps().find("30001000-30002000"), std::string::npos);
  ASSERT_EQ(prctl(Target + 4096, 4096, Name), 0u);
  EXPECT_NE(maps().find("30000000-30003000 rw-p"), std::string::npos);
  ASSERT_EQ(prctl(Target + 4096, 4096, 0), 0u);
  EXPECT_NE(maps().find("30001000-30002000 rw-p"), std::string::npos);
  ASSERT_EQ(prctl(Target, 12288, 0), 0u);
  EXPECT_EQ(maps().find("[anon:"), std::string::npos);
  llvm::cantFail(cpu().write(Name, {0}));
  ASSERT_EQ(prctl(Target, 12288, Name), 0u);
  EXPECT_NE(maps().find("[anon:]"), std::string::npos);
  EXPECT_EQ(Space.mappingGeneration(), Generation);
  EXPECT_EQ(Space.mappedBytes(), Mapped);
  EXPECT_EQ(Space.physicalMemory()->allocatedBytes(), Allocated);
  EXPECT_EQ(bytes(Target, 16), std::vector<uint8_t>(16, 0));
}

TEST_P(LinuxProc, PrctlNamesBothSidesOfHolesAndRetiresOnAddressReuse) {
  enableVMANames();
  const uint64_t Target = 0x30000000, Name = Base + 4096;
  llvm::cantFail(cpu().write(Name, {'h', 'o', 'l', 'e', 0}));
  ASSERT_EQ(call(ServiceKind::Mmap, Target, 12288, 3, 34, UINT64_MAX), Target);
  ASSERT_EQ(call(ServiceKind::Munmap, Target + 4096, 4096), 0u);
  ASSERT_EQ(prctl(Target - 4096, 20480, Name), uint64_t(-12));
  auto Text = maps();
  const auto First = Text.find("[anon:hole]");
  ASSERT_NE(First, std::string::npos);
  EXPECT_NE(Text.find("[anon:hole]", First + 1), std::string::npos);
  ASSERT_EQ(call(ServiceKind::Mprotect, Target + 8192, 4096, 1), 0u);
  Text = maps();
  EXPECT_NE(Text.find("30002000-30003000 r--p"), std::string::npos);
  ASSERT_EQ(call(ServiceKind::Munmap, Target, 12288), 0u);
  ASSERT_EQ(call(ServiceKind::Mmap, Target, 12288, 3, 34, UINT64_MAX), Target);
  Text = maps();
  EXPECT_NE(Text.find("30000000-30003000 rw-p"), std::string::npos);
  EXPECT_EQ(Text.find("[anon:hole]"), std::string::npos);
}

TEST_P(LinuxProc, PrctlReadsNamesLazilyAtPageBoundariesWithLegacyNameMax) {
  enableVMANames();
  const uint64_t Target = 0x30000000, Name = Base + 8190;
  ASSERT_EQ(call(ServiceKind::Mmap, Target, 4096, 3, 34, UINT64_MAX), Target);
  llvm::cantFail(cpu().write(Name, {'Z', 0}));
  ASSERT_EQ(prctl(Target, 4096, Name), 0u);
  EXPECT_NE(maps().find("[anon:Z]"), std::string::npos);
  llvm::cantFail(cpu().write(Name, {'Z', 'Q'}));
  EXPECT_NE(maps().find("[anon:ZQ<fault>]"), std::string::npos);
  llvm::cantFail(
      cpu().write(Base + 8192 - 255, std::vector<uint8_t>(255, 'x')));
  ASSERT_EQ(prctl(Target, 4096, Base + 8192 - 255), 0u);
  EXPECT_NE(maps().find("[anon:" + std::string(255, 'x') + "]"),
            std::string::npos);
  ASSERT_EQ(prctl(Target, 4096, UINT64_MAX), 0u);
  EXPECT_NE(maps().find("[anon:<fault>]"), std::string::npos);
  // An invalid pointer at prctl time is neither imported nor pinned.
  ASSERT_EQ(prctl(Target, 4096, Base + 8192), 0u);
  EXPECT_NE(maps().find("[anon:<fault>]"), std::string::npos);
  ASSERT_EQ(call(ServiceKind::Mmap, Base + 8192, 4096, 3, 34, UINT64_MAX),
            Base + 8192);
  llvm::cantFail(cpu().write(Base + 8192, {'n', '\n', '\\', 0}));
  EXPECT_NE(maps().find("[anon:n\n\\]"), std::string::npos);
  llvm::cantFail(cpu().write(Base + 8192, std::vector<uint8_t>(256, 'x')));
  EXPECT_NE(maps().find("[anon:" + std::string(255, 'x') + "]"),
            std::string::npos);
  ASSERT_EQ(call(ServiceKind::Mprotect, Base + 8192, 4096, 0), 0u);
  EXPECT_NE(maps().find("[anon:<fault>]"), std::string::npos);
}

TEST_P(LinuxProc, PrctlNamesRespectProcBufferOwnershipAndRewind) {
  enableVMANames();
  const uint64_t Name = Base + 4096;
  llvm::cantFail(cpu().write(Name, {'o', 'l', 'd', 0}));
  ASSERT_EQ(prctl(Base, 8192, Name), 0u);
  const auto FD = open("/proc/self/maps");
  const auto Alias = *call(ServiceKind::Dup, FD);
  auto Prefix = read(FD, 1);
  llvm::cantFail(cpu().write(Name, {'n', 'e', 'w', 0}));
  const auto Buffered = Prefix + read(Alias);
  EXPECT_NE(Buffered.find("[anon:old]"), std::string::npos);
  EXPECT_EQ(Buffered.find("[anon:new]"), std::string::npos);
  ASSERT_EQ(call(ServiceKind::Lseek, FD, 0), 0u);
  EXPECT_NE(read(Alias).find("[anon:new]"), std::string::npos);
}

TEST_P(LinuxProc, PrctlKeepsHeapLabelsAndRefusesUnmodeledBackingBeforeEffects) {
  enableVMANames();
  const uint64_t Name = Base + 4096;
  llvm::cantFail(cpu().write(Name, {'l', 'a', 'b', 'e', 'l', 0}));
  ASSERT_EQ(call(ServiceKind::Brk, Base + 12288), Base + 12288);
  ASSERT_EQ(prctl(Base + 8192, 4096, Name), 0u);
  auto Text = maps();
  EXPECT_NE(Text.find("[heap]"), std::string::npos);
  EXPECT_EQ(Text.find("[anon:label]"), std::string::npos);
  Options.LinuxFiles->DynamicProc->Mappings.push_back(
      {Base + 4096, 4096, 0, 1, 0, false, "/fixture/backing"});
  reset();
  EXPECT_FALSE(prctl(Base, 8192, Name));
  EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
  Text = maps();
  EXPECT_EQ(Text.find("[anon:"), std::string::npos);
  EXPECT_NE(Text.find("/fixture/backing"), std::string::npos);
}

TEST_P(LinuxProc, PrctlRefusesDeviceVMAsBeforeRenamingAnAnonymousPrefix) {
  auto &D = Options.LinuxFiles->Devices["/dev/example"];
  D.ReadOnlyMappings.push_back({0, std::vector<uint8_t>(4096), {}});
  enableVMANames();
  const auto FD = open("/dev/example");
  const uint64_t Target = 0x30000000, Name = Base + 4096;
  llvm::cantFail(cpu().write(Name, {'a', 0}));
  ASSERT_EQ(call(ServiceKind::Mmap, Target, 4096, 3, 34, UINT64_MAX), Target);
  ASSERT_EQ(call(ServiceKind::Mmap, Target + 4096, 4096, 1, 1, FD, 0),
            Target + 4096);
  EXPECT_FALSE(prctl(Target, 8192, Name));
  EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
  const auto Text = maps();
  EXPECT_EQ(Text.find("[anon:"), std::string::npos);
  EXPECT_NE(Text.find("/dev/example"), std::string::npos);
}

TEST_P(LinuxProc, SparseWritesTruncationAndAliasesMaintainCompleteStat) {
  auto Writer = open("/data/value", 2);
  auto Reader = open("/data/value");
  auto Alias = *call(ServiceKind::Dup, Writer);
  ASSERT_EQ(call(ServiceKind::Lseek, Writer, 8192), 8192u);
  llvm::cantFail(cpu().write(Base + 128, {'X', 'Y'}));
  ASSERT_EQ(call(ServiceKind::Write, Alias, Base + 128, 2), 2u);
  stat(Reader, 8194, 16, 100, 900000000);
  ASSERT_TRUE(Kernel->clock().advanceTo(200000000, Report));
  ASSERT_EQ(call(ServiceKind::Lseek, Alias, 2), 2u);
  ASSERT_EQ(call(ServiceKind::Write, Writer, Base + 128, 1), 1u);
  stat(Reader, 8194, 16, 101, 100000000);
  ASSERT_EQ(call(ServiceKind::Lseek, Reader, 4096), 4096u);
  EXPECT_EQ(read(Reader, 16), std::string(16, '\0'));
  ASSERT_TRUE(Kernel->clock().advanceTo(300000000, Report));
  auto Truncator = open("/data/value", 1 | 512);
  stat(Alias, 0, 0, 101, 200000000);
  EXPECT_EQ(call(ServiceKind::Lseek, Alias, 0, 1), 3u);
  EXPECT_EQ(call(ServiceKind::Write, Truncator, Base + 128, 1), 1u);
  stat(Reader, 1, 8, 101, 200000000);
}

TEST_P(LinuxProc, FailedCopyUpdatesTimeButZeroCountAndVFSFailureDoNot) {
  auto FD = open("/data/value", 2);
  ASSERT_EQ(call(ServiceKind::Write, FD, Base + 16384, 1), uint64_t(-14));
  stat(FD, 3, 8, 100, 900000000);
  ASSERT_TRUE(Kernel->clock().advanceTo(200000000, Report));
  EXPECT_EQ(call(ServiceKind::Write, FD, Base + 16384, 0), 0u);
  EXPECT_EQ(call(ServiceKind::Write, FD, UINT64_MAX, 1), uint64_t(-14));
  EXPECT_EQ(call(ServiceKind::Lseek, FD, INT64_MAX), uint64_t(INT64_MAX));
  EXPECT_EQ(call(ServiceKind::Write, FD, Base + 128, 1), uint64_t(-22));
  stat(FD, 3, 8, 100, 900000000);
}

TEST_P(LinuxProc, MissingRealtimeStopsBeforeDataOrMetadataMutation) {
  Options.LinuxTime.reset();
  auto FD = open("/data/value", 2);
  EXPECT_FALSE(call(ServiceKind::Write, FD, Base + 128, 1));
  EXPECT_NE(Report.Diagnostic.find("clock"), std::string::npos);
  EXPECT_EQ(read(FD, 3), "abc");
  ASSERT_EQ(call(ServiceKind::Fstat, FD, Base + 256), 0u);
  auto B = bytes(Base + 256, 128);
  EXPECT_EQ(llvm::support::endian::read64le(B.data() + 88), 13u);
  EXPECT_EQ(llvm::support::endian::read64le(B.data() + 104), 19u);
}

TEST_P(LinuxProc, PartialWritePublishesOnlyCopiedExtentsAndAllocatedPages) {
  auto FD = open("/data/value", 2);
  ASSERT_EQ(call(ServiceKind::Lseek, FD, 8190), 8190u);
  llvm::cantFail(cpu().write(Base + 4092, {'W', 'X', 'Y', 'Z'}));
  llvm::cantFail(
      cpu().addressSpace()->protect(Base + 4096, 4096, UserAccessible));
  ASSERT_EQ(call(ServiceKind::Write, FD, Base + 4092, 8), 4u);
  // Initial page plus two touched destination pages; the last four source
  // bytes are inaccessible and must not extend the file or its cursor.
  stat(FD, 8194, 24, 100, 900000000);
  EXPECT_EQ(call(ServiceKind::Lseek, FD, 0, 1), 8194u);
  EXPECT_EQ(call(ServiceKind::Lseek, FD, 8190), 8190u);
  EXPECT_EQ(read(FD, 8), "WXYZ");
}

TEST_P(LinuxProc, FDInfoUsesSharedOffsetsAndDescriptorLocalCloseOnExec) {
  auto FD = open("/data/value", 2 | 1024 | 524288);
  auto Alias = *call(ServiceKind::Dup, FD);
  auto Info = open("/proc/self/fdinfo/3");
  const std::string Flags =
      GetParam() == GuestArchitecture::X64 ? "02102002" : "02402002";
  EXPECT_EQ(read(Info),
            "pos:\t0\nflags:\t" + Flags + "\nmnt_id:\t1\nino:\t42\n");
  EXPECT_EQ(call(ServiceKind::Lseek, Alias, 11), 11u);
  EXPECT_EQ(read(Info), "");
  EXPECT_EQ(call(ServiceKind::Lseek, Info, 0), 0u);
  EXPECT_EQ(read(Info, 7), "pos:\t11");
  EXPECT_EQ(call(ServiceKind::Lseek, FD, 19), 19u);
  EXPECT_EQ(read(Info), "\nflags:\t" + Flags + "\nmnt_id:\t1\nino:\t42\n");
  auto AliasInfo = open("/proc/1000/fdinfo/4");
  const std::string AliasFlags =
      GetParam() == GuestArchitecture::X64 ? "0102002" : "0402002";
  EXPECT_EQ(read(AliasInfo),
            "pos:\t19\nflags:\t" + AliasFlags + "\nmnt_id:\t1\nino:\t42\n");
  EXPECT_EQ(call(ServiceKind::Close, FD), 0u);
  EXPECT_EQ(call(ServiceKind::Lseek, Info, 0), 0u);
  EXPECT_EQ(call(ServiceKind::Read, Info, Base + 256, 100), uint64_t(-2));
  EXPECT_EQ(open("/data/value"), 3u);
  EXPECT_NE(read(Info).find("pos:\t0\n"), std::string::npos);
}

TEST_P(LinuxProc, RenameKeepsFDInfoInodeAcrossAReusedOriginalPath) {
  auto &F = *Options.LinuxFiles;
  LinuxFileMetadata Parent;
  Parent.Device = 0x801;
  Parent.Inode = 91;
  Parent.Mode = 0040755;
  Parent.UID = Parent.GID = 1000;
  Parent.LinkCount = 2;
  Parent.Size = 60;
  Parent.BlockSize = 4096;
  F.Directories = {"/data"};
  F.DirectoryCreation.emplace().Parents["/data"] = Parent;
  F.FileRenames.emplace().Parents.insert("/data");
  reset();
  ASSERT_EQ(open("/data/value", 2), 3u);
  ASSERT_EQ(call(ServiceKind::Dup, 3), 4u);
  ASSERT_EQ(open("/proc/self/fdinfo/3"), 5u);
  ASSERT_EQ(open("/proc/self/fdinfo/4"), 6u);
  EXPECT_EQ(read(3, 1), "a");
  const std::string Flags =
      GetParam() == GuestArchitecture::X64 ? "0100002" : "0400002";
  const auto Expected =
      "pos:\t1\nflags:\t" + Flags + "\nmnt_id:\t1\nino:\t42\n";
  for (unsigned phase = 0; phase != 3; ++phase) {
    SCOPED_TRACE(phase);
    if (phase == 1) {
      path("/data/value");
      llvm::cantFail(cpu().write(Base + 128, {'/', 'd', 'a', 't', 'a', '/', 'm',
                                              'o', 'v', 'e', 'd', 0}));
      ASSERT_EQ(call(ServiceKind::Rename, Base, Base + 128), 0u);
    }
    if (phase == 2) {
      path("/data/value");
      ASSERT_EQ(call(ServiceKind::MkdirAt, uint64_t(-100), Base, 0755), 0u);
    }
    for (auto info : {5u, 6u}) {
      ASSERT_EQ(call(ServiceKind::Lseek, info, 0), 0u);
      EXPECT_EQ(read(info), Expected);
    }
  }
  EXPECT_EQ(read(4, 1), "b");
  ASSERT_EQ(call(ServiceKind::Fstat, 3, Base + 256), 0u);
  EXPECT_EQ(llvm::support::endian::read64le(bytes(Base + 256, 16).data() + 8),
            42u);
  path("/data/value");
  ASSERT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 256), 0u);
  EXPECT_NE(llvm::support::endian::read64le(bytes(Base + 256, 16).data() + 8),
            42u);
}

TEST_P(LinuxProc, MapsFollowProtectionUnmapAndRetireBackingIdentity) {
  Options.LinuxFiles->DynamicProc->Mappings.push_back(
      {Base, 8192, 16384, 91, 0x801, false, "/guest/module.so"});
  reset();
  auto FD = open("/proc/self/maps");
  const auto Before = read(FD);
  EXPECT_NE(Before.find("20000000-20002000 rw-p 00004000 08:01 91"),
            std::string::npos);
  ASSERT_EQ(call(ServiceKind::Mprotect, Base + 4096, 4096, 1), 0u);
  EXPECT_EQ(read(FD), "");
  ASSERT_EQ(call(ServiceKind::Lseek, FD, 0), 0u);
  const auto Split = read(FD);
  EXPECT_NE(Split.find("20000000-20001000 rw-p 00004000 08:01 91"),
            std::string::npos);
  EXPECT_NE(Split.find("20001000-20002000 r--p 00005000 08:01 91"),
            std::string::npos);
  ASSERT_EQ(call(ServiceKind::Munmap, Base + 4096, 4096), 0u);
  ASSERT_EQ(call(ServiceKind::Mmap, Base + 4096, 4096, 3, 34, UINT64_MAX),
            Base + 4096);
  ASSERT_EQ(call(ServiceKind::Lseek, FD, 0), 0u);
  const auto Reused = read(FD);
  EXPECT_NE(Reused.find("20001000-20002000 rw-p 00000000 00:00 0"),
            std::string::npos);
  EXPECT_EQ(Reused.find("00005000 08:01 91"), std::string::npos);
}

TEST_P(LinuxProc, FDInfoTraversalObservesTheOldPublicPositionUntilCommit) {
  ASSERT_EQ(open("/data/value"), 3u);
  ASSERT_EQ(open("/proc/self/fdinfo/3"), 4u);
  ASSERT_EQ(open("/proc/self/fdinfo/4"), 5u);
  ASSERT_EQ(call(ServiceKind::Dup2, 5, 4), 4u);
  // Both aliases now describe their own shared open description.
  EXPECT_EQ(read(4, 5), "pos:\t");
  ASSERT_EQ(call(ServiceKind::Lseek, 5, 1), 1u);
  EXPECT_EQ(read(4, 6), "os:\t5\n");
  ASSERT_EQ(call(ServiceKind::Lseek, 4, 0), 0u);
  EXPECT_EQ(read(5, 7), "pos:\t0\n");
}

TEST_P(LinuxProc, InaccessibleGuardMappingsRemainVisibleAndCountTowardsSize) {
  llvm::cantFail(cpu().addressSpace()->protect(Base + 4096, 4096, 0));
  auto FD = open("/proc/self/maps");
  EXPECT_NE(read(FD).find("20001000-20002000 ---p"), std::string::npos);
  auto S = open("/proc/self/status");
  EXPECT_NE(read(S).find("VmSize:\t       8 kB\n"), std::string::npos);
}

TEST_P(LinuxProc, DeviceBackingSurvivesCloseAndTracksPartialUnmap) {
  auto &D = Options.LinuxFiles->Devices["/dev/example"];
  D.ReadOnlyMappings.push_back({8192, std::vector<uint8_t>(8192, 0x5a), {}});
  reset();
  auto FD = open("/dev/example", 524288);
  ASSERT_EQ(call(ServiceKind::Mmap, 0x30000000, 8192, 1, 1, FD, 8192),
            0x30000000u);
  auto Info = open("/proc/self/fdinfo/" + std::to_string(FD));
  EXPECT_NE(read(Info).find(GetParam() == GuestArchitecture::X64
                                ? "flags:\t02100000\n"
                                : "flags:\t02400000\n"),
            std::string::npos);
  ASSERT_EQ(call(ServiceKind::Close, FD), 0u);
  auto Maps = open("/proc/self/maps");
  const auto Initial = read(Maps);
  EXPECT_NE(Initial.find("30000000-30002000 r--s 00002000"), std::string::npos);
  EXPECT_NE(Initial.find("/dev/example\n"), std::string::npos);
  ASSERT_EQ(call(ServiceKind::Munmap, 0x30000000, 4096), 0u);
  ASSERT_EQ(call(ServiceKind::Lseek, Maps, 0), 0u);
  const auto Remaining = read(Maps);
  EXPECT_NE(Remaining.find("30001000-30002000 r--s 00003000"),
            std::string::npos);
  EXPECT_NE(Remaining.find("/dev/example\n"), std::string::npos);
}

TEST_P(LinuxProc, GPUObjectMapsRetainTheirDeviceIdentityUntilUnmap) {
  auto &D = Options.LinuxFiles->Devices["/dev/example"];
  D.AccessMode = 2;
  D.ReadOnlyMappings.push_back({4096, std::vector<uint8_t>(4096), {}});
  D.KGSL = LinuxKGSLContexts{4096, 3, false, true};
  reset();
  auto FD = open("/dev/example", 2);
  const uint64_t Record = Base + 64;
  llvm::cantFail(cpu().write(Record, std::vector<uint8_t>(48)));
  llvm::cantFail(cpu().writeInteger(Record, 8192, 8));
  ASSERT_EQ(call(ServiceKind::Ioctl, FD, 0xc0300945, Record), 0u);
  ASSERT_EQ(llvm::cantFail(cpu().readInteger(Record + 32, 4)), 2u);
  ASSERT_EQ(call(ServiceKind::Mmap, 0x30000000, 8192, 3, 1, FD, 8192),
            0x30000000u);
  auto Maps = open("/proc/self/maps");
  auto Text = read(Maps);
  EXPECT_NE(Text.find("30000000-30002000 rw-s 00002000"), std::string::npos);
  EXPECT_NE(Text.find("/dev/example\n"), std::string::npos);
  ASSERT_EQ(call(ServiceKind::Close, FD), 0u);
  ASSERT_EQ(call(ServiceKind::Mprotect, 0x30000000, 8192, 0), 0u);
  ASSERT_EQ(call(ServiceKind::Lseek, Maps, 0), 0u);
  Text = read(Maps);
  EXPECT_NE(Text.find("30000000-30002000 ---s 00002000"), std::string::npos);
  ASSERT_EQ(call(ServiceKind::Munmap, 0x30000000, 8192), 0u);
  ASSERT_EQ(call(ServiceKind::Mmap, 0x30000000, 8192, 3, 0x22, uint64_t(-1)),
            0x30000000u);
  ASSERT_EQ(call(ServiceKind::Lseek, Maps, 0), 0u);
  Text = read(Maps);
  EXPECT_NE(Text.find("30000000-30002000 rw-p 00000000"), std::string::npos);
  EXPECT_EQ(Text.find("/dev/example"), std::string::npos);
}

TEST_P(LinuxProc, HeapGrowthAndShrinkChangeMapsAndVirtualSizeTogether) {
  ASSERT_EQ(call(ServiceKind::Brk, Base + 8193), Base + 8193);
  auto FD = open("/proc/self/maps");
  const auto Maps = read(FD);
  EXPECT_NE(Maps.find("20002000-20003000 rw-p"), std::string::npos);
  EXPECT_NE(Maps.find("[heap]\n"), std::string::npos);
  auto S = open("/proc/self/status");
  EXPECT_NE(read(S).find("VmSize:\t      12 kB\n"), std::string::npos);
  ASSERT_EQ(call(ServiceKind::Brk, Base + 8192), Base + 8192);
  ASSERT_EQ(call(ServiceKind::Lseek, FD, 0), 0u);
  EXPECT_EQ(read(FD).find("[heap]"), std::string::npos);
  ASSERT_EQ(call(ServiceKind::Lseek, S, 0), 0u);
  EXPECT_NE(read(S).find("VmSize:\t       8 kB\n"), std::string::npos);
}

TEST_P(LinuxProc, StatusAndTaskDirectoryObserveLiveSchedulerOwners) {
  std::vector<linux_model::ProcTask> Tasks{{1000, 'R'}, {1001, 'S'}};
  Kernel->setTaskProvider([&] { return Tasks; });
  auto D = open("/proc/self/task");
  EXPECT_EQ(entries(D), (std::vector<std::string>{".", "..", "1000", "1001"}));
  auto S = open("/proc/self/task/1001/status");
  const auto Text = read(S);
  EXPECT_NE(Text.find("State:\tS (sleeping)\nTgid:\t1000\nPid:\t1001\n"),
            std::string::npos);
  EXPECT_NE(Text.find("PPid:\t12\nTracerPid:\t0\n"), std::string::npos);
  EXPECT_NE(Text.find("VmSize:\t       8 kB\nThreads:\t2\n"),
            std::string::npos);
  Tasks.pop_back();
  EXPECT_EQ(open("/proc/self/task/1001/status"), uint64_t(-2));
  EXPECT_EQ(call(ServiceKind::Lseek, S, 0), 0u);
  EXPECT_EQ(call(ServiceKind::Read, S, Base + 256, 1000), uint64_t(-3));
  EXPECT_EQ(call(ServiceKind::Lseek, D, 0), 0u);
  EXPECT_EQ(entries(D), (std::vector<std::string>{".", "..", "1000"}));
  S = open("/proc/1000/status");
  EXPECT_NE(read(S).find("Threads:\t1\n"), std::string::npos);
}

TEST_P(LinuxProc, TaskIterationRetainsNextIdentityAcrossEarlierThreadExit) {
  std::vector<linux_model::ProcTask> Tasks{
      {1000, 'R'}, {1001, 'S'}, {1002, 'R'}};
  Kernel->setTaskProvider([&] { return Tasks; });
  auto D = open("/proc/self/task");
  // Dot records are 24 bytes; four-digit task records are 24 bytes too.
  EXPECT_EQ(entries(D, 96),
            (std::vector<std::string>{".", "..", "1000", "1001"}));
  Tasks.erase(Tasks.begin() + 1);
  EXPECT_EQ(entries(D), (std::vector<std::string>{"1002"}));
  EXPECT_EQ(call(ServiceKind::Lseek, D, 2), 2u);
  EXPECT_EQ(entries(D), (std::vector<std::string>{"1000", "1002"}));
}

TEST_P(LinuxProc, ProcCopiesKeepFaultedSuffixAndSeekTraversesCurrentRecords) {
  auto FD = open("/proc/self/status");
  auto S = Kernel->descriptorStatus(FD);
  ASSERT_TRUE(S);
  const auto *Status =
      std::get_if<linux_model::LinuxFiles::DescriptorStatus>(&*S);
  ASSERT_NE(Status, nullptr);
  EXPECT_EQ(Status->BlockSize, 1024u);
  EXPECT_EQ(call(ServiceKind::Read, FD, UINT64_MAX, 1), uint64_t(-14));
  EXPECT_EQ(call(ServiceKind::Lseek, FD, 0, 1), 0u);
  llvm::cantFail(
      cpu().addressSpace()->protect(Base + 4096, 4096, UserAccessible));
  ASSERT_EQ(call(ServiceKind::Read, FD, Base + 4092, 20), 4u);
  EXPECT_EQ(bytes(Base + 4092, 4), (std::vector<uint8_t>{'N', 'a', 'm', 'e'}));
  auto Alias = *call(ServiceKind::Dup, FD);
  EXPECT_EQ(read(Alias, 10), ":\tfixture\n");
  EXPECT_EQ(call(ServiceKind::Lseek, FD, 5), 5u);
  EXPECT_EQ(read(FD, 8), "\tfixture");
  EXPECT_EQ(call(ServiceKind::Lseek, FD, 0, 2), uint64_t(-22));
  EXPECT_EQ(call(ServiceKind::Lseek, FD, 10000), 10000u);
  EXPECT_EQ(read(FD), "");
  EXPECT_EQ(call(ServiceKind::Lseek, FD, 0), 0u);
  EXPECT_EQ(read(FD, 5), "Name:");
}

TEST_P(LinuxProc, SeekingAcrossLongMapRecordsRetainsTheRecordSuffix) {
  std::string LongPath;
  for (unsigned I = 0; I < 16; ++I)
    LongPath += "/" + std::string(254, 'a');
  Options.LinuxFiles->DynamicProc->Mappings.push_back(
      {Base, 8192, 0, 91, 0x801, false, LongPath});
  reset();
  auto FD = open("/proc/self/maps");
  ASSERT_EQ(call(ServiceKind::Lseek, FD, 4095), 4095u);
  const auto Suffix = read(FD);
  EXPECT_EQ(Suffix, LongPath.substr(4095 - 73) + "\n");
  EXPECT_EQ(read(FD), "");
  EXPECT_EQ(call(ServiceKind::Lseek, FD, 0), 0u);
  EXPECT_EQ(read(FD, 21), "20000000-20002000 rw-");
}

TEST_P(LinuxProc, ThreadSelfIsBoundAtOpenAndUnknownOutputsAreUntouched) {
  std::vector<linux_model::ProcTask> Tasks{{1000, 'R'}, {1001, 'R'}};
  Kernel->setTaskProvider([&] { return Tasks; });
  linux_model::ThreadContext Child;
  Child.ID = 1001;
  path("/proc/thread-self/status");
  auto FD = llvm::cantFail(
      Kernel->handle(ServiceKind::OpenAt,
                     {0, 0, {uint64_t(-100), Base, 0, 0, 0, 0}, {}}, &Child));
  ASSERT_TRUE(FD);
  // A subsequent caller with a different TID observes the already-open task.
  EXPECT_NE(read(*FD).find("Pid:\t1001\n"), std::string::npos);
  path("/proc/thread-self");
  auto N = llvm::cantFail(Kernel->handle(
      ServiceKind::ReadlinkAt,
      {0, 0, {uint64_t(-100), Base, Base + 256, 64, 0, 0}, {}}, &Child));
  ASSERT_EQ(N, 14u);
  auto B = bytes(Base + 256, *N);
  EXPECT_EQ(std::string(B.begin(), B.end()), "1000/task/1001");
}

TEST_P(LinuxProc, UnknownProcAndOpaqueFDStateRemainExplicitlyUnsupported) {
  path("/proc/self/smaps");
  EXPECT_FALSE(call(ServiceKind::OpenAt, uint64_t(-100), Base, 0));
  EXPECT_NE(Report.Diagnostic.find("proc entry"), std::string::npos);
  auto FD = open("/proc/self/fdinfo/1");
  EXPECT_FALSE(call(ServiceKind::Read, FD, Base + 256, 100));
  EXPECT_NE(Report.Diagnostic.find("fdinfo"), std::string::npos);
}

TEST_P(LinuxProc, DynamicNamespacePreservesUnrelatedCatalogueLinks) {
  Options.LinuxFiles->SymbolicLinks["/sys/example"] = {'v', 'a', 'l', 'u', 'e'};
  Options.LinuxFiles->SymbolicLinks["/proc/selfish"] = {'o', 't', 'h', 'e',
                                                        'r'};
  reset();
  for (const auto &Name : {"/sys/example", "/proc/selfish"}) {
    path(Name);
    ASSERT_EQ(
        call(ServiceKind::ReadlinkAt, uint64_t(-100), Base, Base + 256, 32),
        5u);
    auto B = bytes(Base + 256, 5);
    EXPECT_EQ(std::string(B.begin(), B.end()),
              std::string(Name) == "/sys/example" ? "value" : "other");
  }
}

TEST(LinuxProcOptions, StrictInputsRejectConflictingAndIncompleteState) {
  for (
      const auto *Text :
      {R"({"linux_files":{"files":[],"dynamic_proc":true}})",
       R"({"linux_files":{"files":[],"dynamic_proc":{"name":"a","parent_pid":-1}}})",
       R"({"linux_files":{"files":[],"dynamic_proc":{"name":"a","parent_pid":0,"extra":0}}})",
       R"({"linux_files":{"files":[{"path":"/proc/1000/maps","bytes_hex":""}],"dynamic_proc":{"name":"a","parent_pid":0}}})",
       R"({"linux_files":{"files":[{"path":"/proc/thread-self/status","bytes_hex":""}],"dynamic_proc":{"name":"a","parent_pid":0}}})",
       R"({"linux_files":{"files":[{"path":"/x","bytes_hex":"","write_limit":1,"track_write_metadata":true}]}})",
       R"({"linux_files":{"files":[{"path":"/x","bytes_hex":"","track_write_metadata":1}]}})"}) {
    auto O = processOptionsFromJSON(Text);
    EXPECT_FALSE(bool(O)) << Text;
    llvm::consumeError(O.takeError());
  }
  auto O = processOptionsFromJSON(
      R"({"linux_files":{"files":[{"path":"/proc/selfish","bytes_hex":""}],"dynamic_proc":{"name":"fixture","parent_pid":12}}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  EXPECT_EQ(O->LinuxFiles->DynamicProc->Name, "fixture");
  EXPECT_EQ(O->LinuxFiles->DynamicProc->ParentPID, 12u);
}

INSTANTIATE_TEST_SUITE_P(Architectures, LinuxProc,
                         testing::Values(GuestArchitecture::X64,
                                         GuestArchitecture::AArch64));
} // namespace
} // namespace neverd::emulation
