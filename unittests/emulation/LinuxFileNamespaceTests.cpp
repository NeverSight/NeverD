//===- LinuxFileNamespaceTests.cpp - Independent raw FD contracts -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Both LP64 ABIs share exact dirent64 bytes, user-copy and open descriptions.
#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxServices.h"

#include "neverd/emulation/ExecutionConfiguration.h"

#include "llvm/Support/Endian.h"

#include <set>

namespace neverd::emulation {
namespace {
using linux_model::ServiceKind;
constexpr uint64_t Base = 0x20000000;
class LinuxFileNamespace : public testing::TestWithParam<GuestArchitecture> {
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
    Options.LinuxFiles->Files["/files/data"] = {'a', 'b', 'c'};
    Options.LinuxFiles->WriteLimits["/files/data"] = 8192;
    LinuxFileMetadata M;
    M.Mode = 0100644;
    M.Inode = 42;
    M.BlockSize = 4096;
    M.Size = 3;
    Options.LinuxFiles->Metadata["/files/data"] = M;
    Options.LinuxFiles->DynamicProcFDs = true;
    Options.LinuxFiles->Directories = {"/empty"};
    Kernel = std::make_unique<linux_model::LinuxServices>(
        cpu(), Layout, Base + 8192, Options, Report);
  }
  std::optional<uint64_t> call(ServiceKind K, uint64_t A = 0, uint64_t B = 0,
                               uint64_t C = 0, uint64_t D = 0) {
    return llvm::cantFail(Kernel->handle(K, {0, 0, {A, B, C, D, 0, 0}, {}}));
  }
  std::optional<uint64_t> openResult(const char *Name, unsigned Flags = 0) {
    path(Name);
    return call(ServiceKind::OpenAt, uint64_t(-100), Base, Flags);
  }
  uint64_t open(const char *Name, unsigned Flags = 0) {
    return *openResult(Name, Flags);
  }
  void path(const char *Name) {
    llvm::StringRef S(Name);
    std::vector<uint8_t> V(S.begin(), S.end());
    V.push_back(0);
    llvm::cantFail(cpu().write(Base, V));
  }
  std::vector<uint8_t> bytes(uint64_t At, unsigned Count) {
    std::vector<uint8_t> V(Count);
    llvm::cantFail(cpu().read(At, V));
    return V;
  }
};

LinuxFileMetadata directorySnapshot() {
  LinuxFileMetadata M;
  M.Device = 7;
  M.Inode = 91;
  M.Mode = 0042775;
  M.UID = 1000;
  M.GID = 1234;
  M.LinkCount = 2;
  M.Size = 40;
  M.BlockSize = 4096;
  M.AccessTime = {3, 4};
  M.ModificationTime = {5, 6};
  M.ChangeTime = {7, 8};
  return M;
}

void enableFileRename(ProcessOptions &Options) {
  auto &F = *Options.LinuxFiles;
  auto &M = F.Metadata.at("/files/data");
  M.Device = 7;
  M.UID = M.GID = 1000;
  M.LinkCount = 1;
  M.Blocks = 8;
  M.AccessTime = {3, 4};
  M.ModificationTime = {5, 6};
  M.ChangeTime = {7, 8};
  F.TrackWriteMetadata.insert("/files/data");
  auto Parent = directorySnapshot();
  Parent.Size = 60;
  F.DirectoryCreation.emplace().Parents["/files"] = Parent;
  F.Directories.push_back("/files");
  F.FileRenames.emplace().Parents.insert("/files");
  F.FileRenames->Limit = 2;
  Options.LinuxTime.emplace().Clocks[0] = {100, 123};
}

TEST_P(LinuxFileNamespace, RenamePreservesOpenObjectIdentityAndLiveNamespace) {
  enableFileRename(Options);
  auto Valid = linux_model::validateFileOptions(*Options.LinuxFiles);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  auto paths = [&](const char *Old, const char *New) {
    path(Old);
    llvm::StringRef S(New);
    std::vector<uint8_t> V(S.begin(), S.end());
    V.push_back(0);
    llvm::cantFail(cpu().write(Base + 128, V));
  };
  auto renameAt = [&] {
    return llvm::cantFail(
        Kernel->handle({0,
                        GetParam() == GuestArchitecture::X64 ? 264u : 38u,
                        {UINT64_MAX, Base, UINT64_MAX, Base + 128, 0, 0},
                        {}}));
  };
  auto fd = open("/files/data", 2);
  ASSERT_EQ(fd, 3u);
  EXPECT_EQ(call(ServiceKind::Read, fd, Base + 512, 1), 1u);
  EXPECT_EQ(call(ServiceKind::Dup, fd), 4u);
  EXPECT_EQ(call(ServiceKind::Fstat, fd, Base + 1024), 0u);
  auto expected =
      bytes(Base + 1024, GetParam() == GuestArchitecture::X64 ? 144 : 128);
  llvm::support::endian::write64le(expected.data() + 104, 100);
  llvm::support::endian::write64le(expected.data() + 112, 123);
  path("/files");
  EXPECT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 4096, 0),
            0u);
  auto expectedDirectory = bytes(Base + 4096, expected.size());
  for (unsigned offset : {88u, 104u}) {
    llvm::support::endian::write64le(expectedDirectory.data() + offset, 100);
    llvm::support::endian::write64le(expectedDirectory.data() + offset + 8,
                                     123);
  }
  paths("/files/data", "/files/moved");
  auto guard = bytes(Base, 8192);
  EXPECT_EQ(renameAt(), 0u);
  EXPECT_EQ(bytes(Base, 8192), guard);
  EXPECT_EQ(call(ServiceKind::Fstat, fd, Base + 1536), 0u);
  EXPECT_EQ(bytes(Base + 1536, expected.size()), expected);
  path("/files");
  EXPECT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 4608, 0),
            0u);
  EXPECT_EQ(bytes(Base + 4608, expectedDirectory.size()), expectedDirectory);
  path("/files/moved");
  EXPECT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 5120, 0),
            0u);
  EXPECT_EQ(bytes(Base + 5120, expected.size()), expected);
  EXPECT_EQ(open("/files/data"), uint64_t(-2));
  auto independent = open("/files/moved");
  ASSERT_EQ(independent, 5u);
  EXPECT_EQ(call(ServiceKind::Read, independent, Base + 512, 3), 3u);
  EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'a', 'b', 'c'}));
  EXPECT_EQ(call(ServiceKind::Read, 4, Base + 512, 1), 1u);
  EXPECT_EQ(bytes(Base + 512, 1), (std::vector<uint8_t>{'b'}));
  llvm::cantFail(cpu().write(Base + 512, {'Z'}));
  EXPECT_EQ(call(ServiceKind::Write, fd, Base + 512, 1), 1u);
  EXPECT_EQ(call(ServiceKind::Lseek, independent, 0, 0), 0u);
  EXPECT_EQ(call(ServiceKind::Read, independent, Base + 512, 3), 3u);
  EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'a', 'b', 'Z'}));
  path("/proc/self/fd/3");
  EXPECT_EQ(call(ServiceKind::ReadlinkAt, uint64_t(-100), Base, Base + 512, 64),
            12u);
  EXPECT_EQ(bytes(Base + 512, 12),
            (std::vector<uint8_t>{'/', 'f', 'i', 'l', 'e', 's', '/', 'm', 'o',
                                  'v', 'e', 'd'}));
  const unsigned directoryFlag =
      GetParam() == GuestArchitecture::X64 ? 0x10000 : 0x4000;
  auto directory = open("/files", directoryFlag);
  ASSERT_EQ(directory, 6u);
  auto count = call(ServiceKind::Getdents64, directory, Base + 2048, 512);
  ASSERT_TRUE(count);
  ASSERT_LE(*count, 512u);
  auto entries = bytes(Base + 2048, *count);
  std::vector<std::string> names;
  for (size_t at = 0; at < entries.size();) {
    auto size = llvm::support::endian::read16le(entries.data() + at + 16);
    ASSERT_GE(size, 24u);
    ASSERT_LE(at + size, entries.size());
    names.emplace_back(
        reinterpret_cast<const char *>(entries.data() + at + 19));
    if (names.back() == "moved")
      EXPECT_EQ(llvm::support::endian::read64le(entries.data() + at), 42u);
    at += size;
  }
  EXPECT_EQ(names, (std::vector<std::string>{".", "..", "moved"}));
  EXPECT_EQ(call(ServiceKind::Close, directory), 0u);
  paths("/files/moved", "/files/data");
  if (GetParam() == GuestArchitecture::X64)
    EXPECT_EQ(llvm::cantFail(
                  Kernel->handle({0, 82, {Base, Base + 128, 0, 0, 0, 0}, {}})),
              0u);
  else
    EXPECT_EQ(renameAt(), 0u);
  paths("/files/data", "/files/moved");
  guard = bytes(Base, 8192);
  EXPECT_FALSE(renameAt());
  EXPECT_NE(Report.Diagnostic.find("reservation exhausted"), std::string::npos);
  EXPECT_EQ(bytes(Base, 8192), guard);
  EXPECT_EQ(open("/files/moved"), uint64_t(-2));
  EXPECT_EQ(call(ServiceKind::Lseek, independent, 0, 0), 0u);
  EXPECT_EQ(call(ServiceKind::Read, independent, Base + 512, 3), 3u);
  EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'a', 'b', 'Z'}));
}

TEST_P(LinuxFileNamespace, RenameRefusalsPreserveBothNamesAndGuestBytes) {
  enableFileRename(Options);
  const auto initial = Options;
  for (unsigned mode = 0; mode != 13; ++mode) {
    SCOPED_TRACE(mode);
    Options = initial;
    auto &F = *Options.LinuxFiles;
    const char *source = "/files/data", *target = "/files/moved";
    if (mode == 0)
      F.FileRenames.reset();
    if (mode == 1)
      Options.LinuxTime.reset();
    if (mode == 2)
      F.TrackWriteMetadata.clear();
    if (mode == 3) {
      F.Files[target] = {};
      F.DirectoryCreation->Parents.at("/files").Size = 80;
    }
    if (mode == 4)
      target = "/files/data";
    if (mode == 5)
      target = "/empty/moved";
    if (mode == 6)
      source = "files/data";
    if (mode == 7)
      source = "/files/missing";
    if (mode == 8)
      F.Metadata.at(source).LinkCount = 2;
    if (mode == 9)
      F.DirectoryCreation->Parents.at("/files").Mode = 0040600;
    if (mode == 10)
      F.DirectoryCreation->Parents.at("/files").Device = 8;
    auto Valid = linux_model::validateFileOptions(F);
    ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
    Kernel = std::make_unique<linux_model::LinuxServices>(
        cpu(), Layout, Base + 8192, Options, Report);
    if (mode == 11) {
      const unsigned directoryFlag =
          GetParam() == GuestArchitecture::X64 ? 0x10000 : 0x4000;
      ASSERT_EQ(open("/files", directoryFlag), 3u);
    }
    path("/files/data");
    ASSERT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 1024, 0),
              0u);
    auto metadata =
        bytes(Base + 1024, GetParam() == GuestArchitecture::X64 ? 144 : 128);
    path(source);
    llvm::StringRef S(target);
    std::vector<uint8_t> V(S.begin(), S.end());
    V.push_back(0);
    llvm::cantFail(cpu().write(Base + 128, V));
    auto before = bytes(Base, 8192);
    EXPECT_FALSE(call(ServiceKind::Rename, Base, mode == 12 ? 0 : Base + 128));
    EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(bytes(Base, 8192), before);
    path("/files/data");
    EXPECT_EQ(call(ServiceKind::Access, Base, 0), 0u);
    EXPECT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 1536, 0),
              0u);
    EXPECT_EQ(bytes(Base + 1536, metadata.size()), metadata);
    path("/files/moved");
    EXPECT_EQ(call(ServiceKind::Access, Base, 0),
              mode == 3 ? 0u : uint64_t(-2));
  }
}

TEST_P(LinuxFileNamespace, RenameFreedNamesCannotReuseTheMovedFilesInode) {
  enableFileRename(Options);
  auto Valid = linux_model::validateFileOptions(*Options.LinuxFiles);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  ASSERT_EQ(open("/files/data", 2), 3u);
  path("/files/data");
  llvm::cantFail(cpu().write(Base + 128, {'/', 'f', 'i', 'l', 'e', 's', '/',
                                          'm', 'o', 'v', 'e', 'd', 0}));
  ASSERT_EQ(call(ServiceKind::Rename, Base, Base + 128), 0u);
  ASSERT_EQ(call(ServiceKind::MkdirAt, uint64_t(-100), Base, 0755), 0u);
  ASSERT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 1024, 0),
            0u);
  auto directory = bytes(Base + 1024, 128);
  EXPECT_NE(llvm::support::endian::read64le(directory.data() + 8), 42u);
  ASSERT_EQ(
      call(ServiceKind::FstatAt, uint64_t(-100), Base + 128, Base + 1536, 0),
      0u);
  ASSERT_EQ(call(ServiceKind::Fstat, 3, Base + 2048), 0u);
  EXPECT_EQ(bytes(Base + 1536, 128), bytes(Base + 2048, 128));
  EXPECT_EQ(llvm::support::endian::read64le(bytes(Base + 2048, 128).data() + 8),
            42u);
  ASSERT_EQ(call(ServiceKind::Read, 3, Base + 512, 3), 3u);
  EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'a', 'b', 'c'}));
}

void enableFileUnlink(ProcessOptions &Options) {
  enableFileRename(Options);
  Options.LinuxFiles->FileUnlinks.emplace().Parents.insert("/files");
  Options.LinuxFiles->FileUnlinks->Limit = 1;
}

TEST_P(LinuxFileNamespace, UnlinkClosedFileRetiresNameAndUpdatesParent) {
  enableFileUnlink(Options);
  auto &F = *Options.LinuxFiles;
  F.Files["/unrelated"] = {'z'};
  auto Valid = linux_model::validateFileOptions(F);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  auto unrelated = open("/unrelated");
  ASSERT_EQ(unrelated, 3u);
  auto fd = open("/files/data");
  ASSERT_EQ(fd, 4u);
  ASSERT_EQ(call(ServiceKind::Read, fd, Base + 512, 3), 3u);
  ASSERT_EQ(call(ServiceKind::Close, fd), 0u);
  path("/files");
  ASSERT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 1024, 0),
            0u);
  auto expected =
      bytes(Base + 1024, GetParam() == GuestArchitecture::X64 ? 144 : 128);
  llvm::support::endian::write64le(expected.data() + 48, 40);
  for (unsigned offset : {88u, 104u}) {
    llvm::support::endian::write64le(expected.data() + offset, 100);
    llvm::support::endian::write64le(expected.data() + offset + 8, 123);
  }
  path("/files/data");
  auto guard = bytes(Base, 8192);
  ProcessServiceEvent raw{0,
                          GetParam() == GuestArchitecture::X64 ? 263u : 35u,
                          {UINT64_MAX, Base, 0xabcdef0000000000ULL, 0, 0, 0},
                          {}};
  EXPECT_EQ(llvm::cantFail(Kernel->handle(raw)), 0u);
  EXPECT_EQ(bytes(Base, 8192), guard);
  EXPECT_EQ(call(ServiceKind::Access, Base, 0), uint64_t(-2));
  EXPECT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 1536, 0),
            uint64_t(-2));
  EXPECT_EQ(open("/files/data"), uint64_t(-2));
  // A missing-name failure does not consume another mutation reservation.
  path("/files/data");
  EXPECT_EQ(call(ServiceKind::Unlink, Base), uint64_t(-2));
  path("/files");
  EXPECT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 1536, 0),
            0u);
  EXPECT_EQ(bytes(Base + 1536, expected.size()), expected);
  auto directory =
      open("/files", GetParam() == GuestArchitecture::X64 ? 0x10000 : 0x4000);
  ASSERT_EQ(directory, 4u);
  auto count = call(ServiceKind::Getdents64, directory, Base + 2048, 512);
  ASSERT_TRUE(count);
  ASSERT_LE(*count, 512u);
  auto entries = bytes(Base + 2048, *count);
  std::vector<std::string> names;
  for (size_t at = 0; at < entries.size();) {
    auto size = llvm::support::endian::read16le(entries.data() + at + 16);
    ASSERT_GE(size, 24u);
    ASSERT_LE(at + size, entries.size());
    names.emplace_back(
        reinterpret_cast<const char *>(entries.data() + at + 19));
    at += size;
  }
  EXPECT_EQ(names, (std::vector<std::string>{".", ".."}));
  EXPECT_EQ(call(ServiceKind::Close, directory), 0u);
  EXPECT_EQ(call(ServiceKind::Read, unrelated, Base + 512, 1), 1u);
  EXPECT_EQ(bytes(Base + 512, 1), (std::vector<uint8_t>{'z'}));
  path("/files/data");
  EXPECT_FALSE(call(ServiceKind::OpenAt, uint64_t(-100), Base, 0101));
  EXPECT_NE(Report.Diagnostic.find("creation requires"), std::string::npos);
}

TEST_P(LinuxFileNamespace, UnlinkRefusalsPreserveNamesMetadataAndGuestBytes) {
  enableFileUnlink(Options);
  const auto initial = Options;
  for (unsigned mode = 0; mode != 13; ++mode) {
    SCOPED_TRACE(mode);
    Options = initial;
    auto &F = *Options.LinuxFiles;
    auto &M = F.Metadata.at("/files/data");
    auto &D = F.DirectoryCreation->Parents.at("/files");
    if (mode == 0)
      F.FileUnlinks.reset();
    if (mode == 1) {
      auto other = directorySnapshot();
      other.Inode = 92;
      F.DirectoryCreation->Parents["/empty"] = other;
      F.FileUnlinks->Parents = {"/empty"};
    }
    if (mode == 2)
      F.TrackWriteMetadata.clear();
    if (mode == 3)
      M.LinkCount = 2;
    if (mode == 4)
      M.Device = 8;
    if (mode == 5)
      D.Mode = 0040555;
    if (mode == 6) {
      D.Mode = 0041777;
      D.UID = 1001;
      M.UID = 1002;
    }
    if (mode == 7)
      D.Mode = 0040666;
    if (mode == 8)
      M.BlockSize = 16384;
    if (mode == 11) {
      F.DynamicProc = LinuxProcOptions{"fixture", 12};
      F.DynamicProc->Mappings.push_back(
          {Base, 4096, 0, 42, 7, false, "/files/data"});
    }
    if (mode == 12)
      Options.LinuxTime.reset();
    auto Valid = linux_model::validateFileOptions(F);
    ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
    Kernel = std::make_unique<linux_model::LinuxServices>(
        cpu(), Layout, Base + 8192, Options, Report);
    if (mode == 9) {
      ASSERT_EQ(open("/files/data"), 3u);
      ASSERT_EQ(call(ServiceKind::Read, 3, Base + 512, 1), 1u);
      ASSERT_EQ(call(ServiceKind::Dup, 3), 4u);
      ASSERT_EQ(call(ServiceKind::Close, 3), 0u);
    }
    if (mode == 10)
      ASSERT_EQ(open("/files",
                     GetParam() == GuestArchitecture::X64 ? 0x10000 : 0x4000),
                3u);
    path("/files");
    ASSERT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 1024, 0),
              0u);
    auto directory =
        bytes(Base + 1024, GetParam() == GuestArchitecture::X64 ? 144 : 128);
    path("/files/data");
    ASSERT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 1536, 0),
              0u);
    auto metadata = bytes(Base + 1536, directory.size());
    auto guard = bytes(Base, 8192);
    EXPECT_FALSE(call(ServiceKind::Unlink, Base));
    EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(bytes(Base, 8192), guard);
    EXPECT_EQ(call(ServiceKind::Access, Base, 0), 0u);
    EXPECT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 2048, 0),
              0u);
    EXPECT_EQ(bytes(Base + 2048, metadata.size()), metadata);
    path("/files");
    EXPECT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 2560, 0),
              0u);
    EXPECT_EQ(bytes(Base + 2560, directory.size()), directory);
    if (mode == 9) {
      EXPECT_EQ(call(ServiceKind::Read, 4, Base + 512, 2), 2u);
      EXPECT_EQ(bytes(Base + 512, 2), (std::vector<uint8_t>{'b', 'c'}));
    }
  }
}

TEST_P(LinuxFileNamespace, UnlinkReservationIsSeparateAndKeepsTheSecondFile) {
  enableFileUnlink(Options);
  auto &F = *Options.LinuxFiles;
  F.Files["/files/second"] = F.Files.at("/files/data");
  F.WriteLimits["/files/second"] = 8192;
  F.TrackWriteMetadata.insert("/files/second");
  F.Metadata["/files/second"] = F.Metadata.at("/files/data");
  F.Metadata.at("/files/second").Inode = 43;
  F.DirectoryCreation->Parents.at("/files").Size = 80;
  auto Valid = linux_model::validateFileOptions(F);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  path("/absent");
  ASSERT_EQ(call(ServiceKind::Unlink, Base), uint64_t(-2));
  path("/files/data");
  ASSERT_EQ(call(ServiceKind::Unlink, Base), 0u);
  path("/files/second");
  auto guard = bytes(Base, 8192);
  EXPECT_FALSE(call(ServiceKind::Unlink, Base));
  EXPECT_NE(Report.Diagnostic.find("reservation exhausted"), std::string::npos);
  EXPECT_EQ(bytes(Base, 8192), guard);
  EXPECT_EQ(call(ServiceKind::Access, Base, 0), 0u);
  path("/files");
  ASSERT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 1024, 0),
            0u);
  EXPECT_EQ(
      llvm::support::endian::read64le(bytes(Base + 1024, 128).data() + 48),
      60u);
}

TEST_P(LinuxFileNamespace, UnlinkUsesRenamedIdentityAndAdmitsStickyOwners) {
  enableFileUnlink(Options);
  const auto initial = Options;
  for (bool parent_owner : {false, true}) {
    Options = initial;
    auto &F = *Options.LinuxFiles;
    auto &D = F.DirectoryCreation->Parents.at("/files");
    D.Mode = 0041777;
    D.UID = parent_owner ? 1000 : 1001;
    F.Metadata.at("/files/data").UID = parent_owner ? 1001 : 1000;
    Kernel = std::make_unique<linux_model::LinuxServices>(
        cpu(), Layout, Base + 8192, Options, Report);
    path("/files/data");
    const char target[] = "/files/moved";
    llvm::cantFail(cpu().write(
        Base + 128,
        llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(target),
                                sizeof(target))));
    ASSERT_EQ(call(ServiceKind::Rename, Base, Base + 128), 0u);
    ASSERT_EQ(call(ServiceKind::Unlink, Base + 128), 0u);
    EXPECT_EQ(open("/files/data"), uint64_t(-2));
    EXPECT_EQ(open("/files/moved"), uint64_t(-2));
    path("/files");
    EXPECT_EQ(call(ServiceKind::Access, Base, 0), 0u);
  }
}

void enableFileCreation(ProcessOptions &Options, const std::string &Path,
                        uint64_t Limit) {
  enableFileUnlink(Options);
  Options.LinuxFiles->FileCreations.emplace().Files[Path] = {Limit};
  auto Empty = directorySnapshot();
  Empty.Inode = 92;
  Options.LinuxFiles->DirectoryCreation->Parents["/empty"] = Empty;
}

TEST_P(LinuxFileNamespace, ParentWalkRetainsDirectoryFDAndFileIdentity) {
  ASSERT_EQ(openResult("/empty"), 3u);
  path("../files/data");
  ASSERT_EQ(call(ServiceKind::OpenAt, 3, Base), 4u);
  EXPECT_EQ(call(ServiceKind::Read, 4, Base + 512, 1), 1u);
  EXPECT_EQ(bytes(Base + 512, 1), (std::vector<uint8_t>{'a'}));
  ASSERT_EQ(openResult("/files/../files/data"), 5u);
  EXPECT_EQ(call(ServiceKind::Read, 5, Base + 512, 3), 3u);
  EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'a', 'b', 'c'}));
  EXPECT_EQ(call(ServiceKind::Read, 4, Base + 512, 1), 1u);
  EXPECT_EQ(bytes(Base + 512, 1), (std::vector<uint8_t>{'b'}));
  path("../files/data");
  EXPECT_EQ(call(ServiceKind::OpenAt, 99, Base), uint64_t(-9));
  EXPECT_EQ(call(ServiceKind::OpenAt, 4, Base), uint64_t(-20));
  path("/../../files/data");
  EXPECT_EQ(call(ServiceKind::OpenAt, 99, Base), 6u);
  for (const char *Name : {"/files/.", "/files/.///", "/files/..", "/.."}) {
    SCOPED_TRACE(Name);
    ASSERT_EQ(openResult(Name), 7u);
    EXPECT_EQ(call(ServiceKind::Close, 7), 0u);
  }
}

TEST_P(LinuxFileNamespace, ParentWalkPreservesVisitedErrorsAndBoundaries) {
  Options.LinuxFiles->SymbolicLinks["/alias"] = {'/', 'f', 'i', 'l', 'e', 's'};
  for (const char *Name : {"/missing/../files/data", "/missing/.",
                           "/empty/../absent", "/files/./../data"}) {
    SCOPED_TRACE(Name);
    path(Name);
    auto Guard = bytes(Base, 8192);
    ASSERT_EQ(call(ServiceKind::OpenAt, uint64_t(-100), Base), uint64_t(-2));
    EXPECT_EQ(bytes(Base, 8192), Guard);
  }
  for (const char *Name :
       {"/files/data/../data", "/files/data/.", "/files/../files/data/"})
    EXPECT_EQ(openResult(Name), uint64_t(-20));
  EXPECT_FALSE(openResult("/alias/../files/data"));
  EXPECT_NE(Report.Diagnostic.find("link traversal"), std::string::npos);
  ASSERT_EQ(openResult("/files/data"), 3u);
  EXPECT_FALSE(openResult("/proc/self/fd/3/../../files/data"));
  Options.LinuxFiles->DynamicProc.emplace();
  EXPECT_FALSE(openResult("/proc/self/unknown/../maps"));
  EXPECT_NE(Report.Diagnostic.find("proc entry"), std::string::npos);
  ASSERT_EQ(openResult("/proc/self/../self/maps"), 4u);
}

TEST_P(LinuxFileNamespace, ParentWalkSearchStartsAtTheSuppliedDirectoryFD) {
  auto &F = *Options.LinuxFiles;
  F.Files["/private/child/data"] = {'x'};
  F.Directories.push_back("/private/child/deep");
  auto M = directorySnapshot();
  M.UID = M.GID = 1001;
  M.Mode = 0040700;
  F.DirectoryCreation.emplace().Parents["/private"] = M;
  ASSERT_EQ(openResult("/private/child/deep"), 3u);
  path("../data");
  ASSERT_EQ(call(ServiceKind::OpenAt, 3, Base), 4u);
  EXPECT_EQ(call(ServiceKind::Read, 4, Base + 512, 1), 1u);
  EXPECT_EQ(bytes(Base + 512, 1), (std::vector<uint8_t>{'x'}));
  EXPECT_EQ(openResult("/private/child/deep/../data"), uint64_t(-13));
  EXPECT_EQ(openResult("/private/../files/data"), uint64_t(-13));
  path("../../child/data");
  EXPECT_EQ(call(ServiceKind::OpenAt, 3, Base), uint64_t(-13));
}

TEST_P(LinuxFileNamespace, ParentWalkPreservesExactCreationGrants) {
  enableFileCreation(Options, "/empty/new", 3);
  for (const char *Name : {"/missing/../empty/new", "/empty/new/."}) {
    path(Name);
    ASSERT_EQ(call(ServiceKind::OpenAt, uint64_t(-100), Base, 0x42, 0600),
              uint64_t(-2));
  }
  path("/files/../empty/new");
  ASSERT_EQ(call(ServiceKind::OpenAt, uint64_t(-100), Base, 0x42, 0600), 3u);
  llvm::cantFail(cpu().write(Base + 512, {'x', 'y', 'z'}));
  EXPECT_EQ(call(ServiceKind::Write, 3, Base + 512, 3), 3u);
  ASSERT_EQ(openResult("/empty/new"), 4u);
  EXPECT_EQ(call(ServiceKind::Read, 4, Base + 512, 3), 3u);
  EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'x', 'y', 'z'}));
}

TEST_P(LinuxFileNamespace, RelativeCreationDoesNotRevisitDirectoryFDAncestors) {
  auto &F = *Options.LinuxFiles;
  F.Directories.insert(F.Directories.end(),
                       {"/locked", "/locked/inner", "/locked/inner/anchor",
                        "/locked/inner/work", "/locked/inner/anchor/child"});
  auto Locked = directorySnapshot();
  Locked.Mode = 0040700;
  Locked.UID = Locked.GID = 1001;
  Locked.Size = 60;
  Locked.LinkCount = 3;
  auto &Parents = F.DirectoryCreation.emplace().Parents;
  Parents["/locked"] = Locked;
  auto Writable = directorySnapshot();
  Writable.Inode = 92;
  Parents["/locked/inner/work"] = Writable;
  Writable.Inode = 93;
  Parents["/locked/inner/anchor/child"] = Writable;
  auto &Reservations = F.FileCreations.emplace().Files;
  Reservations["/locked/inner/work/new"] = {3};
  Reservations["/locked/inner/anchor/child/new"] = {3};
  Options.LinuxTime.emplace().Clocks[0] = {100, 123};
  auto Valid = linux_model::validateFileOptions(F);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  ASSERT_EQ(openResult("/locked/inner/anchor"), 3u);
  path("/locked/inner/work/new");
  EXPECT_EQ(call(ServiceKind::OpenAt, uint64_t(-100), Base, 0x42, 0600),
            uint64_t(-13));
  for (const char *Name : {"../work/new", "child/new"}) {
    SCOPED_TRACE(Name);
    path(Name);
    auto FD = call(ServiceKind::OpenAt, 3, Base, 0x42, 0600);
    ASSERT_EQ(FD, 4u);
    llvm::cantFail(cpu().write(Base + 512, {'x', 'y', 'z'}));
    EXPECT_EQ(call(ServiceKind::Write, *FD, Base + 512, 3), 3u);
    EXPECT_EQ(call(ServiceKind::Close, *FD), 0u);
    EXPECT_EQ(call(ServiceKind::OpenAt, 3, Base), 4u);
    EXPECT_EQ(call(ServiceKind::Read, 4, Base + 1024, 3), 3u);
    EXPECT_EQ(bytes(Base + 1024, 3), (std::vector<uint8_t>{'x', 'y', 'z'}));
    EXPECT_EQ(call(ServiceKind::Close, 4), 0u);
  }
}

TEST_P(LinuxFileNamespace, InteriorDotAliasesRetainFileAndDescriptorIdentity) {
  ASSERT_EQ(openResult("/files/data"), 3u);
  ASSERT_EQ(call(ServiceKind::Fstat, 3, Base + 1024), 0u);
  const auto Stat =
      bytes(Base + 1024, GetParam() == GuestArchitecture::X64 ? 144 : 128);
  ASSERT_EQ(call(ServiceKind::Close, 3), 0u);
  for (const char *Name :
       {"/./files/data", "/files/./data", "//.//files//.//.//data"}) {
    SCOPED_TRACE(Name);
    ASSERT_EQ(openResult(Name, 2), 3u);
    EXPECT_EQ(call(ServiceKind::Access, Base, 0), 0u);
    EXPECT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 1536),
              0u);
    EXPECT_EQ(bytes(Base + 1536, Stat.size()), Stat);
    EXPECT_EQ(call(ServiceKind::Read, 3, Base + 512, 3), 3u);
    EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'a', 'b', 'c'}));
    path("/./proc/self/./fd/3");
    EXPECT_EQ(
        call(ServiceKind::ReadlinkAt, uint64_t(-100), Base, Base + 512, 64),
        11u);
    EXPECT_EQ(bytes(Base + 512, 11),
              (std::vector<uint8_t>{'/', 'f', 'i', 'l', 'e', 's', '/', 'd', 'a',
                                    't', 'a'}));
    EXPECT_EQ(call(ServiceKind::Close, 3), 0u);
  }
  ASSERT_EQ(openResult("/files/./data", 2), 3u);
  llvm::cantFail(cpu().write(Base + 512, {'Z'}));
  EXPECT_EQ(call(ServiceKind::Write, 3, Base + 512, 1), 1u);
  ASSERT_EQ(openResult("/files/data"), 4u);
  EXPECT_EQ(call(ServiceKind::Read, 4, Base + 512, 3), 3u);
  EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'Z', 'b', 'c'}));
}

TEST_P(LinuxFileNamespace, InteriorDotAliasesPreservePathRefusalsAndFDs) {
  Options.LinuxFiles->SymbolicLinks["/alias"] = {'/', 'f', 'i', 'l', 'e', 's'};
  for (const char *Name : {"/./missing", "/empty/./missing"})
    EXPECT_EQ(openResult(Name), uint64_t(-2));
  for (const char *Name :
       {"/files/data/./child", "/./files/data/", "/files/./data////"})
    EXPECT_EQ(openResult(Name), uint64_t(-20));
  for (const char *Name : {"/alias/./data", "./files/data"}) {
    SCOPED_TRACE(Name);
    path(Name);
    auto Guard = bytes(Base, 8192);
    EXPECT_FALSE(call(ServiceKind::OpenAt, uint64_t(-100), Base));
    EXPECT_EQ(bytes(Base, 8192), Guard);
  }
  EXPECT_EQ(openResult("/./files/data"), 3u);
  EXPECT_EQ(openResult("/./empty/"), 4u);
  path("/files/./data");
  EXPECT_EQ(call(ServiceKind::OpenAt, 4, Base), 5u);
  path("./data");
  EXPECT_EQ(call(ServiceKind::OpenAt, 4, Base), uint64_t(-2));
}

TEST_P(LinuxFileNamespace, InteriorDotAliasesShareNamespaceMutationGrants) {
  enableFileCreation(Options, "/empty/new", 3);
  Options.LinuxFiles->FileRenames->Parents.insert("/empty");
  Options.LinuxFiles->FileUnlinks->Parents.insert("/empty");
  auto Valid = linux_model::validateFileOptions(*Options.LinuxFiles);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  path("/empty/new/.");
  auto Guard = bytes(Base, 8192);
  EXPECT_EQ(call(ServiceKind::OpenAt, uint64_t(-100), Base, 0x42, 0600),
            uint64_t(-2));
  EXPECT_EQ(bytes(Base, 8192), Guard);
  path("/./empty/./new");
  ASSERT_EQ(call(ServiceKind::OpenAt, uint64_t(-100), Base, 0x42, 0600), 3u);
  llvm::cantFail(cpu().write(Base + 512, {'x', 'y', 'z'}));
  EXPECT_EQ(call(ServiceKind::Write, 3, Base + 512, 3), 3u);
  EXPECT_EQ(call(ServiceKind::Close, 3), 0u);
  ASSERT_EQ(openResult("/empty/new"), 3u);
  EXPECT_EQ(call(ServiceKind::Read, 3, Base + 512, 3), 3u);
  EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'x', 'y', 'z'}));
  EXPECT_EQ(call(ServiceKind::Close, 3), 0u);
  const char Target[] = "/empty/./moved";
  llvm::cantFail(cpu().write(
      Base + 128,
      llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Target),
                              sizeof(Target))));
  path("/./empty/new");
  EXPECT_EQ(call(ServiceKind::Rename, Base, Base + 128), 0u);
  EXPECT_EQ(openResult("/empty/./new"), uint64_t(-2));
  ASSERT_EQ(openResult("/empty/moved"), 3u);
  EXPECT_EQ(call(ServiceKind::Read, 3, Base + 512, 3), 3u);
  EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'x', 'y', 'z'}));
  EXPECT_EQ(call(ServiceKind::Close, 3), 0u);
  path("/./empty/./moved");
  EXPECT_EQ(call(ServiceKind::UnlinkAt, uint64_t(-100), Base), 0u);
  EXPECT_EQ(openResult("/empty/moved"), uint64_t(-2));
}

TEST_P(LinuxFileNamespace, FileCreationAfterRenameKeepsBothObjectIdentities) {
  enableFileCreation(Options, "/files/data", 3);
  Options.LinuxFiles->DynamicProc = LinuxProcOptions{"fixture", 12};
  auto Valid = linux_model::validateFileOptions(*Options.LinuxFiles);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  ASSERT_EQ(open("/files/data"), 3u);
  ASSERT_EQ(call(ServiceKind::Read, 3, Base + 512, 1), 1u);
  const char Backup[] = "/files/backup";
  llvm::cantFail(cpu().write(
      Base + 128,
      llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Backup),
                              sizeof(Backup))));
  path("/files/data");
  ASSERT_EQ(call(ServiceKind::Rename, Base, Base + 128), 0u);
  ASSERT_EQ(call(ServiceKind::Fstat, 3, Base + 1024), 0u);
  const bool X64 = GetParam() == GuestArchitecture::X64;
  const unsigned StatSize = X64 ? 144 : 128;
  const auto OldStat = bytes(Base + 1024, StatSize);
  path("/files");
  ASSERT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 1536, 0),
            0u);
  auto Parent = bytes(Base + 1536, StatSize);
  llvm::support::endian::write64le(Parent.data() + 48, 80);
  path("/files/data");
  const auto Guard = bytes(Base, 8192);
  ProcessServiceEvent Open{0,
                           X64 ? 257u : 56u,
                           {UINT64_MAX, Base,
                            0xabcdef0000000000ULL | 0100 | 01000 | 2,
                            0x1234567800000000ULL | 0666, 0, 0},
                           {}};
  ASSERT_EQ(llvm::cantFail(Kernel->handle(Open)), 4u);
  EXPECT_EQ(bytes(Base, 8192), Guard);
  ASSERT_EQ(call(ServiceKind::Fstat, 3, Base + 2048), 0u);
  EXPECT_EQ(bytes(Base + 2048, StatSize), OldStat);
  ASSERT_EQ(call(ServiceKind::Fstat, 4, Base + 2560), 0u);
  auto NewStat = bytes(Base + 2560, StatSize);
  const auto Inode = llvm::support::endian::read64le(NewStat.data() + 8);
  EXPECT_NE(Inode, 0u);
  EXPECT_NE(Inode, 42u);
  EXPECT_NE(Inode, 91u);
  EXPECT_NE(Inode, 92u);
  std::vector<uint8_t> Expected(StatSize, 0);
  auto Put32 = [&](unsigned At, uint32_t Value) {
    llvm::support::endian::write32le(Expected.data() + At, Value);
  };
  auto Put64 = [&](unsigned At, uint64_t Value) {
    llvm::support::endian::write64le(Expected.data() + At, Value);
  };
  Put64(0, 7);
  Put64(8, Inode);
  Put32(X64 ? 24 : 16, 0100644);
  if (X64)
    Put64(16, 1);
  else
    Put32(20, 1);
  Put32(X64 ? 28 : 24, 1000);
  Put32(X64 ? 32 : 28, 1234);
  Put64(56, 4096);
  for (unsigned At : {72u, 88u, 104u}) {
    Put64(At, 100);
    Put64(At + 8, 123);
  }
  EXPECT_EQ(NewStat, Expected);
  ASSERT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 3072, 0),
            0u);
  EXPECT_EQ(bytes(Base + 3072, StatSize), NewStat);
  path("/files");
  ASSERT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 3584, 0),
            0u);
  EXPECT_EQ(bytes(Base + 3584, StatSize), Parent);
  ASSERT_EQ(open("/files", X64 ? 0x10000 : 0x4000), 5u);
  auto Count = call(ServiceKind::Getdents64, 5, Base + 4096, 1024);
  ASSERT_TRUE(Count);
  ASSERT_LE(*Count, 1024u);
  const auto Entries = bytes(Base + 4096, *Count);
  std::map<std::string, uint64_t> Identities;
  for (size_t At = 0; At < Entries.size();) {
    const auto Size = llvm::support::endian::read16le(Entries.data() + At + 16);
    ASSERT_GE(Size, 24u);
    ASSERT_LE(At + Size, Entries.size());
    Identities.emplace(reinterpret_cast<const char *>(Entries.data() + At + 19),
                       llvm::support::endian::read64le(Entries.data() + At));
    At += Size;
  }
  EXPECT_EQ(Identities.size(), 4u);
  EXPECT_EQ(Identities.at("."), 91u);
  EXPECT_EQ(Identities.at("backup"), 42u);
  EXPECT_EQ(Identities.at("data"), Inode);
  ASSERT_EQ(call(ServiceKind::Close, 5), 0u);
  ASSERT_EQ(open("/proc/self/fdinfo/4"), 5u);
  const std::string Info =
      std::string("pos:\t0\nflags:\t") + (X64 ? "0100002" : "0400002") +
      "\nmnt_id:\t1\nino:\t" + std::to_string(Inode) + "\n";
  ASSERT_EQ(call(ServiceKind::Read, 5, Base + 4096, 256), Info.size());
  EXPECT_EQ(bytes(Base + 4096, Info.size()),
            (std::vector<uint8_t>(Info.begin(), Info.end())));
  ASSERT_EQ(call(ServiceKind::Close, 5), 0u);
  llvm::cantFail(cpu().write(Base + 512, std::vector<uint8_t>{'x', 'y', 'z'}));
  ASSERT_EQ(call(ServiceKind::Write, 4, Base + 512, 3), 3u);
  ASSERT_EQ(call(ServiceKind::Read, 3, Base + 768, 2), 2u);
  EXPECT_EQ(bytes(Base + 768, 2), (std::vector<uint8_t>{'b', 'c'}));
  ASSERT_EQ(call(ServiceKind::Lseek, 4, 0, 0), 0u);
  ASSERT_EQ(call(ServiceKind::Read, 4, Base + 768, 3), 3u);
  EXPECT_EQ(bytes(Base + 768, 3), (std::vector<uint8_t>{'x', 'y', 'z'}));
  ASSERT_EQ(call(ServiceKind::Fstat, 4, Base + 2560), 0u);
  Put64(48, 3);
  Put64(64, 8);
  EXPECT_EQ(bytes(Base + 2560, StatSize), Expected);
  const char Moved[] = "/files/new";
  llvm::cantFail(cpu().write(
      Base + 128,
      llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Moved),
                              sizeof(Moved))));
  path("/files/data");
  ASSERT_EQ(call(ServiceKind::Rename, Base, Base + 128), 0u);
  path("/proc/self/fd/4");
  ASSERT_EQ(call(ServiceKind::ReadlinkAt, UINT64_MAX, Base, Base + 384, 128),
            sizeof(Moved) - 1);
  EXPECT_EQ(bytes(Base + 384, sizeof(Moved) - 1),
            (std::vector<uint8_t>(Moved, Moved + sizeof(Moved) - 1)));
  ASSERT_EQ(call(ServiceKind::Close, 4), 0u);
  ASSERT_EQ(call(ServiceKind::Unlink, Base + 128), 0u);
  EXPECT_EQ(call(ServiceKind::Access, Base + 128, 0), uint64_t(-2));
  path("/files/data");
  EXPECT_FALSE(call(ServiceKind::OpenAt, UINT64_MAX, Base, 0102, 0666));
  EXPECT_NE(Report.Diagnostic.find("already consumed"), std::string::npos);
  EXPECT_EQ(call(ServiceKind::Access, Base, 0), uint64_t(-2));
  ASSERT_EQ(open("/files/backup"), 4u);
  ASSERT_EQ(call(ServiceKind::Read, 4, Base + 768, 3), 3u);
  EXPECT_EQ(bytes(Base + 768, 3), (std::vector<uint8_t>{'a', 'b', 'c'}));
  ASSERT_EQ(call(ServiceKind::Fstat, 4, Base + 2048), 0u);
  EXPECT_EQ(bytes(Base + 2048, StatSize), OldStat);
}

TEST_P(LinuxFileNamespace, FileCreationRefusalsKeepNamesMetadataBytesAndGrant) {
  enableFileCreation(Options, "/empty/new", 3);
  const auto Initial = Options;
  for (unsigned Case = 0; Case != 10; ++Case) {
    SCOPED_TRACE(Case);
    Options = Initial;
    auto &F = *Options.LinuxFiles;
    if (Case == 0)
      F.FileCreations.reset();
    if (Case == 1)
      F.FileCreations->Files = {{"/empty/other", {3}}};
    if (Case == 2)
      F.DirectoryCreation->Parents.at("/empty").Mode = 0040555;
    if (Case == 3)
      Options.LinuxTime.reset();
    if (Case == 5) {
      F.DynamicProc = LinuxProcOptions{"fixture", 12};
      F.DynamicProc->Mappings.push_back(
          {Base, 4096, 0, 99, 7, false, "/empty/new"});
    }
    if (Case == 7)
      F.DescriptorLimit = 3;
    auto Valid = linux_model::validateFileOptions(F);
    ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
    Kernel = std::make_unique<linux_model::LinuxServices>(
        cpu(), Layout, Base + 8192, Options, Report);
    if (Case == 4)
      ASSERT_EQ(open("/empty",
                     GetParam() == GuestArchitecture::X64 ? 0x10000 : 0x4000),
                3u);
    path("/empty");
    ASSERT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 1024, 0),
              0u);
    auto Parent =
        bytes(Base + 1024, GetParam() == GuestArchitecture::X64 ? 144 : 128);
    path(Case == 9 ? "/empty/new/" : "/empty/new");
    auto Guard = bytes(Base, 8192);
    unsigned Flags = 0102;
    if (Case == 8)
      Flags |= GetParam() == GuestArchitecture::X64 ? 0x4000 : 0x10000;
    auto Result = call(ServiceKind::OpenAt, UINT64_MAX, Base, Flags,
                       Case == 6 ? 04666 : 0666);
    if (Case == 2 || Case == 7)
      EXPECT_EQ(Result, uint64_t(Case == 2 ? -13 : -24));
    else
      EXPECT_FALSE(Result);
    EXPECT_EQ(bytes(Base, 8192), Guard);
    EXPECT_EQ(call(ServiceKind::Access, Base, 0), uint64_t(-2));
    path("/empty");
    ASSERT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 1536, 0),
              0u);
    EXPECT_EQ(bytes(Base + 1536, Parent.size()), Parent);
    if (Case == 4)
      ASSERT_EQ(call(ServiceKind::Close, 3), 0u);
    F.FileCreations = Initial.LinuxFiles->FileCreations;
    F.DirectoryCreation->Parents.at("/empty").Mode = directorySnapshot().Mode;
    F.DynamicProc.reset();
    F.DescriptorLimit = Initial.LinuxFiles->DescriptorLimit;
    Options.LinuxTime = Initial.LinuxTime;
    path("/empty/new");
    EXPECT_EQ(call(ServiceKind::OpenAt, UINT64_MAX, Base, 0102, 0666), 3u);
    EXPECT_EQ(call(ServiceKind::Close, 3), 0u);
  }
}

TEST_P(LinuxFileNamespace,
       FileCreationPermissionsAndByteLimitFollowTheNewObject) {
  enableFileCreation(Options, "/empty/new", 1);
  path("/empty/new");
  ASSERT_EQ(call(ServiceKind::OpenAt, UINT64_MAX, Base, 0102, 0400), 3u);
  llvm::cantFail(cpu().write(Base + 512, std::vector<uint8_t>{'x'}));
  ASSERT_EQ(call(ServiceKind::Write, 3, Base + 512, 1), 1u);
  auto Guard = bytes(Base, 8192);
  EXPECT_FALSE(call(ServiceKind::Write, 3, Base + 512, 1));
  EXPECT_EQ(bytes(Base, 8192), Guard);
  ASSERT_EQ(call(ServiceKind::Lseek, 3, 0, 0), 0u);
  ASSERT_EQ(call(ServiceKind::Read, 3, Base + 768, 1), 1u);
  EXPECT_EQ(bytes(Base + 768, 1), (std::vector<uint8_t>{'x'}));
  ASSERT_EQ(call(ServiceKind::Close, 3), 0u);
  EXPECT_EQ(call(ServiceKind::OpenAt, UINT64_MAX, Base, 1, 0), uint64_t(-13));
  EXPECT_EQ(call(ServiceKind::OpenAt, UINT64_MAX, Base, 01000, 0),
            uint64_t(-13));
  ASSERT_EQ(call(ServiceKind::OpenAt, UINT64_MAX, Base, 0, 0), 3u);
  ASSERT_EQ(call(ServiceKind::Read, 3, Base + 768, 1), 1u);
  EXPECT_EQ(bytes(Base + 768, 1), (std::vector<uint8_t>{'x'}));
  path("/empty");
  ASSERT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 1024, 0),
            0u);
  EXPECT_EQ(
      llvm::support::endian::read64le(bytes(Base + 1024, 128).data() + 48),
      60u);
}

TEST_P(LinuxFileNamespace, FileCreationCountsPreserveEveryLiveGeneration) {
  enableFileCreation(Options, "/files/data", 3);
  Options.LinuxFiles->FileCreations->Files.at("/files/data").Count = 2;
  Options.LinuxFiles->FileUnlinks->Limit = 2;
  auto Valid = linux_model::validateFileOptions(*Options.LinuxFiles);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  const unsigned StatSize = GetParam() == GuestArchitecture::X64 ? 144 : 128;
  auto move = [&](const char *Target) {
    path("/files/data");
    llvm::StringRef Name(Target);
    std::vector<uint8_t> Value(Name.begin(), Name.end());
    Value.push_back(0);
    llvm::cantFail(cpu().write(Base + 128, Value));
    return call(ServiceKind::Rename, Base, Base + 128);
  };
  auto create = [&](unsigned Mode) {
    path("/files/data");
    return call(ServiceKind::OpenAt, UINT64_MAX, Base, 0102, Mode);
  };
  ASSERT_EQ(open("/files/data"), 3u);
  ASSERT_EQ(call(ServiceKind::Read, 3, Base + 512, 1), 1u);
  ASSERT_EQ(move("/files/backup"), 0u);
  ASSERT_EQ(call(ServiceKind::Fstat, 3, Base + 1024), 0u);
  const auto Original = bytes(Base + 1024, StatSize);
  ASSERT_EQ(create(0666), 4u);
  llvm::cantFail(cpu().write(Base + 512, std::vector<uint8_t>{'o', 'n', 'e'}));
  ASSERT_EQ(call(ServiceKind::Write, 4, Base + 512, 3), 3u);
  ASSERT_EQ(call(ServiceKind::Lseek, 4, 1, 0), 1u);
  // Opening an existing object with O_CREAT cannot spend a future generation.
  ASSERT_EQ(create(0600), 5u);
  ASSERT_EQ(call(ServiceKind::Close, 5), 0u);
  ASSERT_EQ(move("/files/first"), 0u);
  ASSERT_EQ(call(ServiceKind::Fstat, 4, Base + 1536), 0u);
  const auto First = bytes(Base + 1536, StatSize);
  const auto FirstInode = llvm::support::endian::read64le(First.data() + 8);
  path("/files/data");
  auto Guard = bytes(Base, 8192);
  EXPECT_FALSE(call(ServiceKind::OpenAt, UINT64_MAX, Base, 0102, 04666));
  EXPECT_EQ(bytes(Base, 8192), Guard);
  ASSERT_EQ(create(0600), 5u);
  ASSERT_EQ(call(ServiceKind::Fstat, 5, Base + 2048), 0u);
  const auto Second = bytes(Base + 2048, StatSize);
  const auto SecondInode = llvm::support::endian::read64le(Second.data() + 8);
  EXPECT_NE(FirstInode, 42u);
  EXPECT_NE(SecondInode, 42u);
  EXPECT_NE(FirstInode, SecondInode);
  EXPECT_EQ(llvm::support::endian::read64le(Second.data() + 48), 0u);
  EXPECT_EQ(llvm::support::endian::read64le(Second.data() + 64), 0u);
  EXPECT_EQ(
      llvm::support::endian::read32le(
          Second.data() + (GetParam() == GuestArchitecture::X64 ? 24 : 16)),
      0100600u);
  ASSERT_EQ(call(ServiceKind::Fstat, 3, Base + 2560), 0u);
  EXPECT_EQ(bytes(Base + 2560, StatSize), Original);
  ASSERT_EQ(call(ServiceKind::Fstat, 4, Base + 3072), 0u);
  EXPECT_EQ(bytes(Base + 3072, StatSize), First);
  llvm::cantFail(cpu().write(Base + 512, std::vector<uint8_t>{'t', 'w', 'o'}));
  ASSERT_EQ(call(ServiceKind::Write, 5, Base + 512, 3), 3u);
  Guard = bytes(Base, 8192);
  EXPECT_FALSE(call(ServiceKind::Write, 5, Base + 512, 1));
  EXPECT_EQ(bytes(Base, 8192), Guard);
  ASSERT_EQ(call(ServiceKind::Read, 4, Base + 768, 2), 2u);
  EXPECT_EQ(bytes(Base + 768, 2), (std::vector<uint8_t>{'n', 'e'}));
  ASSERT_EQ(call(ServiceKind::Read, 3, Base + 768, 2), 2u);
  EXPECT_EQ(bytes(Base + 768, 2), (std::vector<uint8_t>{'b', 'c'}));
  ASSERT_EQ(call(ServiceKind::Lseek, 5, 0, 0), 0u);
  ASSERT_EQ(call(ServiceKind::Read, 5, Base + 768, 3), 3u);
  EXPECT_EQ(bytes(Base + 768, 3), (std::vector<uint8_t>{'t', 'w', 'o'}));
  path("/files");
  ASSERT_EQ(call(ServiceKind::FstatAt, UINT64_MAX, Base, Base + 3584, 0), 0u);
  EXPECT_EQ(
      llvm::support::endian::read64le(bytes(Base + 3584, StatSize).data() + 48),
      100u);
  ASSERT_EQ(call(ServiceKind::Close, 5), 0u);
  path("/files/data");
  ASSERT_EQ(call(ServiceKind::Unlink, Base), 0u);
  Guard = bytes(Base, 8192);
  EXPECT_FALSE(call(ServiceKind::OpenAt, UINT64_MAX, Base, 0102, 0666));
  EXPECT_NE(Report.Diagnostic.find("already consumed"), std::string::npos);
  EXPECT_EQ(bytes(Base, 8192), Guard);
  EXPECT_EQ(call(ServiceKind::Access, Base, 0), uint64_t(-2));
  ASSERT_EQ(call(ServiceKind::Fstat, 4, Base + 3072), 0u);
  EXPECT_EQ(bytes(Base + 3072, StatSize), First);
  ASSERT_EQ(call(ServiceKind::Close, 4), 0u);
  path("/files/first");
  ASSERT_EQ(call(ServiceKind::Unlink, Base), 0u);
  path("/files");
  ASSERT_EQ(call(ServiceKind::FstatAt, UINT64_MAX, Base, Base + 3584, 0), 0u);
  EXPECT_EQ(
      llvm::support::endian::read64le(bytes(Base + 3584, StatSize).data() + 48),
      60u);
  ASSERT_EQ(call(ServiceKind::Fstat, 3, Base + 2560), 0u);
  EXPECT_EQ(bytes(Base + 2560, StatSize), Original);
}

TEST_P(LinuxFileNamespace, FileCreationCountsRetainInodesThroughTheFinalSlot) {
  enableFileCreation(Options, "/empty/new", 0);
  auto &F = *Options.LinuxFiles;
  F.FileCreations->Files.at("/empty/new").Count =
      LinuxFileCreationOptions::MaxFiles;
  F.FileUnlinks->Parents.insert("/empty");
  F.FileUnlinks->Limit = LinuxFileCreationOptions::MaxFiles;
  auto Valid = linux_model::validateFileOptions(F);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  std::set<uint64_t> Inodes{42, 91, 92};
  const unsigned StatSize = GetParam() == GuestArchitecture::X64 ? 144 : 128;
  for (unsigned I = 0; I != LinuxFileCreationOptions::MaxFiles; ++I) {
    SCOPED_TRACE(I);
    path("/empty/new");
    ASSERT_EQ(call(ServiceKind::OpenAt, UINT64_MAX, Base, 0102, 0666), 3u);
    ASSERT_EQ(call(ServiceKind::Fstat, 3, Base + 1024), 0u);
    const auto Stat = bytes(Base + 1024, StatSize);
    EXPECT_TRUE(
        Inodes.insert(llvm::support::endian::read64le(Stat.data() + 8)).second);
    EXPECT_EQ(llvm::support::endian::read64le(Stat.data() + 48), 0u);
    EXPECT_EQ(call(ServiceKind::Read, 3, Base + 512, 1), 0u);
    ASSERT_EQ(call(ServiceKind::Close, 3), 0u);
    ASSERT_EQ(call(ServiceKind::Unlink, Base), 0u);
    EXPECT_EQ(call(ServiceKind::Access, Base, 0), uint64_t(-2));
  }
  EXPECT_EQ(Inodes.size(), LinuxFileCreationOptions::MaxFiles + 3u);
  const auto Guard = bytes(Base, 8192);
  EXPECT_FALSE(call(ServiceKind::OpenAt, UINT64_MAX, Base, 0102, 0666));
  EXPECT_NE(Report.Diagnostic.find("already consumed"), std::string::npos);
  EXPECT_EQ(bytes(Base, 8192), Guard);
  path("/empty");
  ASSERT_EQ(call(ServiceKind::FstatAt, UINT64_MAX, Base, Base + 1536, 0), 0u);
  EXPECT_EQ(
      llvm::support::endian::read64le(bytes(Base + 1536, StatSize).data() + 48),
      40u);
}

TEST_P(LinuxFileNamespace, UnlinkErrorsPreserveNamespaceBytesAndOpenCursors) {
  auto fd = open("/files/data");
  ASSERT_EQ(fd, 3u);
  for (auto [text, error] : {std::pair{"/absent", 2},
                             {"", 2},
                             {"//absent///", 2},
                             {"/files/data/child", 20},
                             {"/files/data/", 20}}) {
    path(text);
    auto before = bytes(Base, 8192);
    ProcessServiceEvent raw{0,
                            GetParam() == GuestArchitecture::X64 ? 263u : 35u,
                            {UINT64_MAX, Base, 0xabcdef0000000000ULL, 0, 0, 0},
                            {}};
    EXPECT_EQ(llvm::cantFail(Kernel->handle(raw)), uint64_t(0) - error);
    EXPECT_EQ(bytes(Base, 8192), before);
    if (GetParam() == GuestArchitecture::X64) {
      raw.Number = 87;
      raw.Arguments = {Base, 0, 0, 0, 0, 0};
      EXPECT_EQ(llvm::cantFail(Kernel->handle(raw)), uint64_t(0) - error);
    }
  }
  llvm::cantFail(cpu().write(Base + 8191, std::vector<uint8_t>{0}));
  EXPECT_EQ(call(ServiceKind::Unlink, Base + 8191), uint64_t(-2));
  EXPECT_EQ(call(ServiceKind::Unlink, Base + 8192), uint64_t(-14));
  EXPECT_EQ(call(ServiceKind::Unlink, 0), uint64_t(-14));
  llvm::cantFail(cpu().write(Base, std::vector<uint8_t>(4096, 'x')));
  EXPECT_EQ(call(ServiceKind::Unlink, Base), uint64_t(-36));
  EXPECT_EQ(call(ServiceKind::Read, fd, Base + 512, 3), 3u);
  EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'a', 'b', 'c'}));
  EXPECT_EQ(call(ServiceKind::Close, fd), 0u);
  EXPECT_EQ(open("/files/data"), 3u);
}
TEST_P(LinuxFileNamespace, UnlinkDoesNotInventRemovalOrRelativeWalks) {
  Options.LinuxFiles->SymbolicLinks["/link"] = {'/', 'x'};
  for (auto [text, flags] : {std::pair{"/files/data", 0u},
                             {"/empty", 0u},
                             {"/link", 0u},
                             {"/link/child", 0u},
                             {"files/missing", 0u},
                             {"/files/../missing", 0u},
                             {"/absent", 512u},
                             {"/absent", 0x80000000u}}) {
    Kernel = std::make_unique<linux_model::LinuxServices>(
        cpu(), Layout, Base + 8192, Options, Report);
    path(text);
    auto before = bytes(Base, 8192);
    EXPECT_FALSE(call(ServiceKind::UnlinkAt, UINT64_MAX, Base, flags));
    EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(bytes(Base, 8192), before);
  }
  Options.LinuxFiles.reset();
  Kernel = std::make_unique<linux_model::LinuxServices>(
      cpu(), Layout, Base + 8192, Options, Report);
  path("/absent");
  EXPECT_FALSE(call(ServiceKind::Unlink, Base));
  EXPECT_NE(Report.Diagnostic.find("explicit linux_files"), std::string::npos);
}
TEST_P(LinuxFileNamespace, UnlinkSeesRuntimeCreatedDirectories) {
  Options.LinuxFiles->DirectoryCreation.emplace().Parents["/empty"] =
      directorySnapshot();
  Options.LinuxTime.emplace().Clocks[0] = {100, 123};
  ASSERT_FALSE(bool(linux_model::validateFileOptions(*Options.LinuxFiles)));
  path("/empty/new");
  EXPECT_EQ(call(ServiceKind::MkdirAt, uint64_t(-100), Base, 0755), 0u);
  auto before = bytes(Base, 8192);
  EXPECT_FALSE(call(ServiceKind::UnlinkAt, UINT64_MAX, Base, 0));
  EXPECT_NE(Report.Diagnostic.find("existing or unresolved object"),
            std::string::npos);
  EXPECT_EQ(bytes(Base, 8192), before);
}

TEST_P(LinuxFileNamespace,
       CreatedDirectoriesShareInodesMetadataAndOpenCursors) {
  Options.LinuxFiles->DirectoryCreation.emplace().Parents["/empty"] =
      directorySnapshot();
  Options.LinuxTime.emplace().Clocks[0] = {100, 123};
  ASSERT_FALSE(bool(linux_model::validateFileOptions(*Options.LinuxFiles)));
  auto D = open("/empty");
  ASSERT_EQ(D, 3u);
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 512, 48), 48u);
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 512, 48), 0u);
  path("//empty//z///");
  ProcessServiceEvent Raw{0,
                          GetParam() == GuestArchitecture::X64 ? 258u : 34u,
                          {UINT64_MAX, Base, 0xabcdef0000001fffULL, 0, 0, 0},
                          {}};
  EXPECT_EQ(llvm::cantFail(Kernel->handle(Raw)), 0u);
  const auto Child = open("/empty/z");
  EXPECT_EQ(call(ServiceKind::Fstat, Child, Base + 1024), 0u);
  const unsigned Size = GetParam() == GuestArchitecture::X64 ? 144 : 128;
  auto S = bytes(Base + 1024, Size);
  auto Word = [&](unsigned Offset) {
    return llvm::support::endian::read64le(S.data() + Offset);
  };
  auto Int = [&](unsigned Offset) {
    return llvm::support::endian::read32le(S.data() + Offset);
  };
  const bool X64 = GetParam() == GuestArchitecture::X64;
  EXPECT_EQ(Word(0), 7u);
  const auto Inode = Word(8);
  EXPECT_NE(Inode, 91u);
  EXPECT_NE(Inode, 42u);
  EXPECT_NE(Inode, 0u);
  EXPECT_EQ(Int(X64 ? 24 : 16), 0043755u);
  EXPECT_EQ(Int(X64 ? 16 : 20), 2u);
  EXPECT_EQ(Int(X64 ? 28 : 24), 1000u);
  EXPECT_EQ(Int(X64 ? 32 : 28), 1234u);
  EXPECT_EQ(Word(48), 40u);
  EXPECT_EQ(Word(56), 4096u);
  EXPECT_EQ(Word(64), 0u);
  for (unsigned At : {72u, 88u, 104u}) {
    EXPECT_EQ(Word(At), 100u);
    EXPECT_EQ(Word(At + 8), 123u);
  }
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 512, 24), 24u);
  auto E = bytes(Base + 512, 24);
  EXPECT_EQ(llvm::support::endian::read64le(E.data()), Inode);
  EXPECT_EQ(E[18], 4);
  EXPECT_EQ(E[19], 'z');
  EXPECT_EQ(E[20], 0);
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 512, 48), 0u);
  // Insert a lexically earlier sibling after EOF. Existing cookies do not
  // repeat z or hide the new a; dup consumes the original description.
  ASSERT_TRUE(Kernel->clock().advanceTo(1000000007, Report));
  path("/empty/a");
  EXPECT_EQ(call(ServiceKind::MkdirAt, uint64_t(-100), Base, 0700), 0u);
  const auto Alias = call(ServiceKind::Dup, D);
  ASSERT_TRUE(Alias);
  EXPECT_EQ(call(ServiceKind::Getdents64, *Alias, Base + 512, 24), 24u);
  EXPECT_EQ(bytes(Base + 531, 2), (std::vector<uint8_t>{'a', 0}));
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 512, 48), 0u);
  EXPECT_EQ(call(ServiceKind::Fstat, D, Base + 1024), 0u);
  S = bytes(Base + 1024, Size);
  EXPECT_EQ(Word(8), 91u);
  EXPECT_EQ(Int(X64 ? 16 : 20), 4u);
  EXPECT_EQ(Word(48), 80u);
  EXPECT_EQ(Word(72), 3u); // noatime, including enumeration
  EXPECT_EQ(Word(80), 4u);
  EXPECT_EQ(Word(88), 101u);
  EXPECT_EQ(Word(96), 130u);
  const auto ParentAfter = S;
  EXPECT_EQ(call(ServiceKind::MkdirAt, 0, Base, 0), uint64_t(-17));
  EXPECT_EQ(call(ServiceKind::Fstat, D, Base + 1024), 0u);
  EXPECT_EQ(bytes(Base + 1024, Size), ParentAfter);
  EXPECT_EQ(call(ServiceKind::Close, Child), 0u);
  EXPECT_EQ(call(ServiceKind::Fstat, open("/empty/z"), Base + 1024), 0u);
  S = bytes(Base + 1024, Size);
  EXPECT_EQ(Word(8), Inode);
  path("/empty/z");
  EXPECT_EQ(call(ServiceKind::FstatAt, UINT64_MAX, Base, Base + 1280, 0), 0u);
  EXPECT_EQ(bytes(Base + 1280, Size), S);
  EXPECT_EQ(call(ServiceKind::Lseek, D, 0, 0), 0u);
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 512, 96), 96u);
  EXPECT_EQ(bytes(Base + 579, 2), (std::vector<uint8_t>{'z', 0}));
  EXPECT_EQ(bytes(Base + 603, 2), (std::vector<uint8_t>{'a', 0}));
}
TEST_P(LinuxFileNamespace, DirectoryAdmissionIsAtomicForTimeCapacityAndMode) {
  auto &C = Options.LinuxFiles->DirectoryCreation.emplace();
  C.Parents["/empty"] = directorySnapshot();
  C.Limit = 1;
  path("/empty/new");
  EXPECT_FALSE(call(ServiceKind::MkdirAt, 0, Base, 0777));
  EXPECT_EQ(call(ServiceKind::FaccessAt, 0, Base, 0), uint64_t(-2));
  Options.LinuxTime.emplace().Clocks[0] = {100, 0};
  EXPECT_EQ(call(ServiceKind::MkdirAt, 0, Base, 0500), 0u);
  path("/empty/new/sub");
  EXPECT_EQ(call(ServiceKind::MkdirAt, 0, Base, 0777), uint64_t(-13));
  path("/empty/second");
  EXPECT_FALSE(call(ServiceKind::MkdirAt, 0, Base, 0777));
  EXPECT_NE(Report.Diagnostic.find("reservation"), std::string::npos);
  EXPECT_EQ(call(ServiceKind::FaccessAt, 0, Base, 0), uint64_t(-2));
  path("/empty/new");
  EXPECT_EQ(call(ServiceKind::MkdirAt, 0, Base, 0777), uint64_t(-17));
}
TEST_P(LinuxFileNamespace, DirectoryModesUseCallerIdentityAndParentSetgid) {
  auto &C = Options.LinuxFiles->DirectoryCreation.emplace();
  auto M = directorySnapshot();
  M.Mode = 0040777;
  M.UID = 777;
  M.GID = 888;
  C.Parents["/empty"] = M;
  C.Umask = 0077;
  Options.LinuxTime.emplace().Clocks[0] = {100, 0};
  path("/empty/new");
  EXPECT_EQ(call(ServiceKind::MkdirAt, 0, Base, 07777), 0u);
  const auto F = open("/empty/new");
  EXPECT_EQ(call(ServiceKind::Fstat, F, Base + 512), 0u);
  const bool X64 = GetParam() == GuestArchitecture::X64;
  auto S = bytes(Base + 512, 144);
  auto Int = [&](unsigned Offset) {
    return llvm::support::endian::read32le(S.data() + Offset);
  };
  EXPECT_EQ(Int(X64 ? 24 : 16), 0041700u);
  EXPECT_EQ(Int(X64 ? 28 : 24), 1000u);
  EXPECT_EQ(Int(X64 ? 32 : 28), 1000u);
  path("/empty/new/nested");
  EXPECT_EQ(call(ServiceKind::MkdirAt, 0, Base, 0777), 0u);
  EXPECT_EQ(call(ServiceKind::Fstat, F, Base + 512), 0u);
  S = bytes(Base + 512, 144);
  EXPECT_EQ(Int(X64 ? 16 : 20), 3u);
}

TEST_P(LinuxFileNamespace, DirectoryMetadataRejectsInodeAndNamespaceConflicts) {
  auto &C = Options.LinuxFiles->DirectoryCreation.emplace();
  C.Parents["/empty"] = directorySnapshot();
  const auto Good = *Options.LinuxFiles;
  for (unsigned Mode = 0; Mode != 6; ++Mode) {
    *Options.LinuxFiles = Good;
    auto &D = *Options.LinuxFiles->DirectoryCreation;
    if (Mode == 0)
      D.Parents["/empty"].Inode = 42;
    if (Mode == 1)
      Options.LinuxFiles->Files["/empty/child"] = {};
    if (Mode == 2)
      Options.LinuxFiles->Directories.push_back("/empty/sub");
    if (Mode == 3)
      D.Parents["/empty"].AccessTime.Nanoseconds = 1000000000;
    if (Mode == 4)
      D.Parents["/empty"].Mode = 0100755;
    if (Mode == 5)
      D.Parents["/proc/self/fd"] = directorySnapshot();
    auto E = linux_model::validateFileOptions(*Options.LinuxFiles);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
}

TEST_P(LinuxFileNamespace, WorkingDirectoryHasExactBytesAndKernelErrorOrder) {
  Options.LinuxFiles->WorkingDirectory = "/empty";
  EXPECT_EQ(call(ServiceKind::Getcwd, Base + 512, 7), 7u);
  EXPECT_EQ(bytes(Base + 512, 8),
            (std::vector<uint8_t>{'/', 'e', 'm', 'p', 't', 'y', 0, 0}));
  for (uint64_t Size : {0u, 1u, 6u})
    EXPECT_EQ(call(ServiceKind::Getcwd, UINT64_MAX, Size), uint64_t(-34));
  EXPECT_EQ(call(ServiceKind::Getcwd, UINT64_MAX, 7), uint64_t(-14));
  EXPECT_EQ(call(ServiceKind::Getcwd, Base + 1024, UINT64_MAX), 7u);
  EXPECT_EQ(bytes(Base + 1024, 7), bytes(Base + 512, 7));
  ProcessServiceEvent Raw{0,
                          GetParam() == GuestArchitecture::X64 ? 79u : 17u,
                          {Base + 1536, 7, 0, 0, 0, 0},
                          {}};
  EXPECT_EQ(llvm::cantFail(Kernel->handle(Raw)), 7u);
  EXPECT_EQ(bytes(Base + 1536, 7), bytes(Base + 512, 7));
  llvm::cantFail(
      cpu().addressSpace()->protect(Base + 4096, 4096, Read | UserAccessible));
  EXPECT_FALSE(call(ServiceKind::Getcwd, Base + 4093, 7));
  EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(bytes(Base + 4093, 3), (std::vector<uint8_t>{0, 0, 0}));
  Options.LinuxFiles->WorkingDirectory = "/";
  EXPECT_EQ(call(ServiceKind::Getcwd, Base + 512, 2), 2u);
  EXPECT_EQ(bytes(Base + 512, 2), (std::vector<uint8_t>{'/', 0}));
}

TEST_P(LinuxFileNamespace, WorkingDirectoryRequiresExplicitDirectoryIdentity) {
  EXPECT_FALSE(call(ServiceKind::Getcwd, Base + 512, 16));
  EXPECT_NE(Report.Diagnostic.find("not supplied"), std::string::npos);
  for (const char *Path : {"", "relative", "/missing", "/files/data", "/files",
                           "/empty/", "//empty", "/empty/.."}) {
    Options.LinuxFiles->WorkingDirectory = Path;
    auto E = linux_model::validateFileOptions(*Options.LinuxFiles);
    EXPECT_TRUE(bool(E)) << Path;
    llvm::consumeError(std::move(E));
  }
  Options.LinuxFiles->WorkingDirectory = "/empty";
  auto E = linux_model::validateFileOptions(*Options.LinuxFiles);
  EXPECT_FALSE(bool(E));
  llvm::consumeError(std::move(E));
}

TEST_P(LinuxFileNamespace,
       RepeatedSeparatorsShareFileBytesAndCapturedMetadata) {
  const auto Writer = open("///files////data", 2);
  ASSERT_EQ(Writer, 3u);
  for (const char *Name :
       {"/files/data", "//files/data", "/files//data", "////files/////data"}) {
    const auto Reader = open(Name);
    ASSERT_EQ(Reader, 4u);
    EXPECT_EQ(call(ServiceKind::Read, Reader, Base + 512, 3), 3u);
    EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'a', 'b', 'c'}));
    EXPECT_EQ(call(ServiceKind::FaccessAt, uint64_t(-100), Base, 0), 0u);
    EXPECT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 1024, 0),
              0u);
    EXPECT_EQ(call(ServiceKind::Fstat, Reader, Base + 1280), 0u);
    const unsigned Size = GetParam() == GuestArchitecture::X64 ? 144 : 128;
    EXPECT_EQ(bytes(Base + 1024, Size), bytes(Base + 1280, Size));
    EXPECT_EQ(call(ServiceKind::Close, Reader), 0u);
  }
  llvm::cantFail(cpu().write(Base + 512, {'x', 'y'}));
  EXPECT_EQ(call(ServiceKind::Write, Writer, Base + 512, 2), 2u);
  const auto Reader = open("/files/data");
  EXPECT_EQ(call(ServiceKind::Read, Reader, Base + 512, 3), 3u);
  EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'x', 'y', 'c'}));
  EXPECT_EQ(call(ServiceKind::Lseek, Writer, 0, 1), 2u);
}

TEST_P(LinuxFileNamespace, SeparatorAliasesRetainDirectoryAndLinkBoundaries) {
  EXPECT_EQ(open("/files//missing"), uint64_t(-2));
  EXPECT_EQ(open("///files//data////"), uint64_t(-20));
  EXPECT_EQ(open("//files//data//child"), uint64_t(-20));
  const auto Directory = open("///empty////");
  ASSERT_EQ(Directory, 3u);
  EXPECT_EQ(call(ServiceKind::Getdents64, Directory, Base + 512, 48), 48u);
  path("//files///data");
  EXPECT_EQ(call(ServiceKind::MkdirAt, uint64_t(-100), Base, 0777),
            uint64_t(-17));
  path("//missing///child");
  EXPECT_EQ(call(ServiceKind::MkdirAt, uint64_t(-100), Base, 0777),
            uint64_t(-2));
  const auto File = open("//files/data");
  ASSERT_EQ(File, 4u);
  path("///proc//self///fd//4");
  EXPECT_EQ(
      call(ServiceKind::ReadlinkAt, uint64_t(-100), Base, Base + 512, 128),
      11u);
  EXPECT_EQ(bytes(Base + 512, 11),
            (std::vector<uint8_t>{'/', 'f', 'i', 'l', 'e', 's', '/', 'd', 'a',
                                  't', 'a'}));
  path("//proc//self//fd//4///");
  EXPECT_FALSE(call(ServiceKind::OpenAt, uint64_t(-100), Base, 0));
  EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
}

TEST_P(LinuxFileNamespace,
       SeparatorFoldingKeepsOriginalPathLengthAndUnknownDotWalks) {
  for (const char *Name : {"files//data"}) {
    path(Name);
    EXPECT_FALSE(call(ServiceKind::OpenAt, uint64_t(-100), Base, 0));
  }
  std::string Long(4096, '/');
  path(Long.c_str());
  EXPECT_EQ(call(ServiceKind::OpenAt, uint64_t(-100), Base, 0), uint64_t(-36));
  // The input catalogue still has one canonical spelling per object.
  Options.LinuxFiles->Files["/files//other"] = {};
  auto Error = linux_model::validateFileOptions(*Options.LinuxFiles);
  EXPECT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
}

TEST_P(LinuxFileNamespace, IoctlTraceKeepsEntryPointerAndRefusedCallBytes) {
  auto &D = Options.LinuxFiles->Devices["/dev/example"];
  D.AccessMode = 2;
  // The output overwrites the pointer cell. Observations still read the
  // originally selected destination, not a new address from output bytes.
  D.Ioctls = {{0x12345678,
               0,
               {},
               {{0, {}, {}, std::vector<uint8_t>(8, 0)},
                {0, 0, {0xa5, 0x5a}, {0x12, 0x34}}}}};
  auto &T = Options.LinuxFiles->IoctlTrace.emplace();
  T.Requests[0x12345678] = {{0, 8, {}}, {0, 2, 0}};
  llvm::cantFail(linux_model::validateFileOptions(*Options.LinuxFiles));
  Kernel = std::make_unique<linux_model::LinuxServices>(
      cpu(), Layout, Base + 8192, Options, Report);
  const auto FD = open("/dev/example", 2);
  llvm::cantFail(cpu().writeInteger(Base + 256, Base + 512, 8));
  llvm::cantFail(cpu().write(Base + 512, {0xa5, 0x5a}));
  ProcessServiceEvent E{0x1234,
                        GetParam() == GuestArchitecture::X64 ? 16u : 29u,
                        {FD | (uint64_t(7) << 32),
                         0x12345678 | (uint64_t(9) << 32), Base + 256, 0, 0, 0},
                        {}};
  ASSERT_EQ(llvm::cantFail(Kernel->handle(E)), 0u);
  ASSERT_EQ(Report.IoctlTrace.size(), 1u);
  const auto &C = Report.IoctlTrace[0];
  EXPECT_EQ(C.PC, 0x1234u);
  EXPECT_EQ(C.FD, FD);
  EXPECT_EQ(C.Request, 0x12345678u);
  EXPECT_FALSE(C.ReturnAddress);
  ASSERT_EQ(C.Memory.size(), 2u);
  EXPECT_EQ(C.Memory[1].Address, Base + 512);
  EXPECT_EQ(C.Memory[1].Before, (std::vector<uint8_t>{0xa5, 0x5a}));
  EXPECT_EQ(C.Memory[1].After, (std::vector<uint8_t>{0x12, 0x34}));
  EXPECT_EQ(C.Memory[0].After, std::vector<uint8_t>(8, 0));
  // A repeated transaction is unsupported, not turned into a trace success.
  const auto Before = bytes(Base, 8192);
  EXPECT_FALSE(llvm::cantFail(Kernel->handle(E)));
  EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(bytes(Base, 8192), Before);
  ASSERT_EQ(Report.IoctlTrace.size(), 2u);
  EXPECT_FALSE(Report.IoctlTrace[1].Result);
  EXPECT_EQ(Report.IoctlTrace[1].Memory[0].Before,
            Report.IoctlTrace[1].Memory[0].After);
}

TEST_P(LinuxFileNamespace,
       IoctlTraceSelectionPrecedesBudgetAndPreservesRefusals) {
  auto &D = Options.LinuxFiles->Devices["/dev/example"];
  D.AccessMode = 2;
  D.Ioctls = {{7, 0, {}, {}},
              {8, 37, {}, {{0, {}, {0xa5}, {0x7f}}}},
              {7, -13, {}, {}},
              {8, -22, {}, {}}};
  auto &T = Options.LinuxFiles->IoctlTrace.emplace();
  T.IncludeUnlisted = false;
  T.EventLimit = 1;
  T.ByteLimit = 2;
  T.Requests[8] = {{0, 1, {}}};
  llvm::cantFail(linux_model::validateFileOptions(*Options.LinuxFiles));
  Kernel = std::make_unique<linux_model::LinuxServices>(
      cpu(), Layout, Base + 8192, Options, Report);
  const auto FD = open("/dev/example", 2);
  llvm::cantFail(cpu().write(Base + 256, {0xa5}));
  EXPECT_EQ(call(ServiceKind::Ioctl, FD, 7, UINT64_MAX), 0u);
  EXPECT_TRUE(Report.IoctlTrace.empty());
  EXPECT_EQ(call(ServiceKind::Ioctl, FD, 8 | (uint64_t(9) << 32), Base + 256),
            37u);
  ASSERT_EQ(Report.IoctlTrace.size(), 1u);
  EXPECT_EQ(Report.IoctlTrace[0].Request, 8u);
  EXPECT_EQ(Report.IoctlTrace[0].Memory[0].Before,
            (std::vector<uint8_t>{0xa5}));
  EXPECT_EQ(Report.IoctlTrace[0].Memory[0].After, (std::vector<uint8_t>{0x7f}));
  EXPECT_EQ(call(ServiceKind::Ioctl, FD, 7, UINT64_MAX), uint64_t(-13));
  EXPECT_FALSE(Report.IoctlTraceTruncated);
  EXPECT_EQ(call(ServiceKind::Ioctl, FD, 8, Base + 256), uint64_t(-22));
  EXPECT_TRUE(Report.IoctlTraceTruncated);
  const auto Before = bytes(Base, 8192);
  EXPECT_FALSE(call(ServiceKind::Ioctl, FD, 7, UINT64_MAX));
  EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(bytes(Base, 8192), Before);
  EXPECT_EQ(Report.IoctlTrace.size(), 1u);
}

TEST_P(LinuxFileNamespace,
       IoctlTraceEmptySelectionAndMetadataOnlyRequestsRemainDistinct) {
  for (bool Select : {false, true}) {
    SCOPED_TRACE(Select);
    Report = {ProcessProfile::LinuxELF64,
              GetParam(),
              ExecutionBackendKind::Unicorn,
              {}};
    auto &D = Options.LinuxFiles->Devices["/dev/example"];
    D.AccessMode = 2;
    D.Ioctls = {{7, 0, {}, {}}, {8, 1, {}, {}}};
    auto &T = Options.LinuxFiles->IoctlTrace.emplace();
    T.IncludeUnlisted = false;
    T.EventLimit = 1;
    T.ByteLimit = 1;
    if (Select)
      T.Requests[8] = {};
    Kernel = std::make_unique<linux_model::LinuxServices>(
        cpu(), Layout, Base + 8192, Options, Report);
    const auto FD = open("/dev/example", 2);
    EXPECT_EQ(call(ServiceKind::Ioctl, FD, 7, UINT64_MAX), 0u);
    EXPECT_EQ(call(ServiceKind::Ioctl, FD, 8, UINT64_MAX), 1u);
    EXPECT_FALSE(call(ServiceKind::Ioctl, FD, 9, UINT64_MAX));
    EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_FALSE(Report.IoctlTraceTruncated);
    ASSERT_EQ(Report.IoctlTrace.size(), Select ? 1u : 0u);
    if (Select) {
      EXPECT_EQ(Report.IoctlTrace[0].Request, 8u);
      EXPECT_EQ(Report.IoctlTrace[0].Result, 1u);
      EXPECT_TRUE(Report.IoctlTrace[0].Memory.empty());
    }
  }
}

TEST_P(LinuxFileNamespace,
       DirentLayoutPaddingAndSeekCookiesAreIndependentOfHostABI) {
  auto FD = open("/empty");
  llvm::cantFail(cpu().write(Base + 256, std::vector<uint8_t>(64, 0xa5)));
  ASSERT_EQ(llvm::cantFail(Kernel->handle(
                {0,
                 GetParam() == GuestArchitecture::X64 ? 217u : 61u,
                 {FD, Base + 256, 48, 0, 0, 0},
                 {}})),
            48u);
  auto Actual = bytes(Base + 256, 64);
  std::vector<uint8_t> Expected(64, 0xa5);
  for (unsigned Offset : {0, 24}) {
    // Model-owned inode identities must be stable and distinct, but are not
    // captured native inode numbers. Check all other bytes independently.
    std::copy_n(Actual.begin() + Offset, 8, Expected.begin() + Offset);
    llvm::support::endian::write64le(Expected.data() + Offset + 8,
                                     Offset ? 2 : 1);
    Expected[Offset + 16] = 24;
    Expected[Offset + 17] = 0;
    Expected[Offset + 18] = 4;
    Expected[Offset + 19] = '.';
    if (Offset) {
      Expected[Offset + 20] = '.';
      Expected[Offset + 21] = 0;
    } else
      Expected[Offset + 20] = 0;
  }
  EXPECT_EQ(Actual, Expected);
  EXPECT_NE(llvm::support::endian::read64le(Actual.data()), 0u);
  EXPECT_NE(llvm::support::endian::read64le(Actual.data()),
            llvm::support::endian::read64le(Actual.data() + 24));
  EXPECT_EQ(call(ServiceKind::Getdents64, FD, UINT64_MAX, 0), 0u);
  EXPECT_EQ(call(ServiceKind::Lseek, FD, 0, 0), 0u);
  EXPECT_EQ(call(ServiceKind::Getdents64, FD, Base + 256, 0x80000000),
            uint64_t(-22));
  EXPECT_EQ(call(ServiceKind::Lseek, FD, 0, 1), 0u);
  ASSERT_EQ(call(ServiceKind::Getdents64, FD, Base + 256, 24), 24u);
  EXPECT_EQ(bytes(Base + 256, 24),
            std::vector<uint8_t>(Expected.begin(), Expected.begin() + 24));
}
TEST_P(LinuxFileNamespace,
       PartialWriteCommitsOnlyReadablePagesAndSharesObjectBytes) {
  const auto FD = open("/files/data", 2), Alias = *call(ServiceKind::Dup, FD),
             Reader = open("/files/data");
  llvm::cantFail(cpu().write(Base + 4092, {'W', 'X', 'Y', 'Z'}));
  llvm::cantFail(
      cpu().addressSpace()->protect(Base + 4096, 4096, UserAccessible));
  ASSERT_EQ(call(ServiceKind::Write, FD, Base + 4092, 8), 4u);
  EXPECT_EQ(call(ServiceKind::Lseek, Alias, 0, 1), 4u);
  EXPECT_EQ(call(ServiceKind::Read, Reader, Base + 512, 8), 4u);
  EXPECT_EQ(bytes(Base + 512, 4), (std::vector<uint8_t>{'W', 'X', 'Y', 'Z'}));
  EXPECT_EQ(call(ServiceKind::Write, FD, Base + 4096, 1), uint64_t(-14));
  EXPECT_EQ(call(ServiceKind::Lseek, Alias, 0, 1), 4u);
  EXPECT_EQ(call(ServiceKind::Close, FD), 0u);
  EXPECT_EQ(call(ServiceKind::Write, Alias, Base + 4092, 1), 1u);
  EXPECT_FALSE(call(ServiceKind::Fstat, Reader, Base + 512));
  EXPECT_NE(Report.Diagnostic.find("invalidated"), std::string::npos);
}
TEST_P(LinuxFileNamespace, ProcCookiesSkipClosedSlotsAndEOFRequiresRewind) {
  const auto D = open("/proc/self/fd"), F = open("/files/data");
  ASSERT_EQ(D, 3u);
  ASSERT_EQ(F, 4u);
  EXPECT_EQ(call(ServiceKind::Close, 1), 0u);
  ASSERT_EQ(call(ServiceKind::Getdents64, D, Base + 256, 1024), 144u);
  auto B = bytes(Base + 256, 144);
  const uint64_t Cookies[] = {1, 2, 4, 5, 6, 258};
  const char *Names[] = {".", "..", "0", "2", "3", "4"};
  for (unsigned I = 0; I < 6; ++I) {
    EXPECT_EQ(llvm::support::endian::read64le(B.data() + 24 * I + 8),
              Cookies[I]);
    EXPECT_EQ(std::string(reinterpret_cast<char *>(B.data() + 24 * I + 19)),
              Names[I]);
  }
  llvm::cantFail(
      cpu().write(Base, {'/', 'p', 'r', 'o', 'c', '/', 's', 'e', 'l', 'f', 0}));
  llvm::cantFail(cpu().write(Base + 512, std::vector<uint8_t>(8, 0xa5)));
  EXPECT_EQ(call(ServiceKind::ReadlinkAt, UINT64_MAX, Base, Base + 512, 8), 4u);
  EXPECT_EQ(bytes(Base + 512, 8),
            (std::vector<uint8_t>{'1', '0', '0', '0', 0xa5, 0xa5, 0xa5, 0xa5}));
  EXPECT_EQ(call(ServiceKind::Dup, F), 1u);
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 256, 1024), 0u);
  EXPECT_EQ(call(ServiceKind::Lseek, D, 3, 0), 3u);
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 256, 24), 24u);
  EXPECT_EQ(bytes(Base + 256 + 19, 2), (std::vector<uint8_t>{'1', 0}));
}
TEST_P(LinuxFileNamespace,
       BudgetRefusalAndExclusiveOpenDoNotTruncateOrAdvance) {
  const auto F = open("/files/data", 2);
  EXPECT_FALSE(
      call(ServiceKind::OpenAt, uint64_t(-100), Base,
           0100 | (GetParam() == GuestArchitecture::X64 ? 00200000 : 040000)));

  EXPECT_EQ(open("/files/data", 0100 | 0200 | 01000 | 1), uint64_t(-17));
  EXPECT_EQ(call(ServiceKind::Lseek, F, 8191, 0), 8191u);
  EXPECT_FALSE(call(ServiceKind::Write, F, Base, 2));
  EXPECT_EQ(call(ServiceKind::Lseek, F, 0, 1), 8191u);
  EXPECT_EQ(call(ServiceKind::Lseek, F, 0, 0), 0u);
  EXPECT_EQ(call(ServiceKind::Read, F, Base + 512, 8), 3u);
  EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'a', 'b', 'c'}));
}
TEST_P(LinuxFileNamespace, AppendValidatesOriginalCursorBeforeSelectingEOF) {
  const auto F = open("/files/data", 1 | 02000),
             Alias = *call(ServiceKind::Dup, F), Reader = open("/files/data");
  ASSERT_EQ(call(ServiceKind::Lseek, F, INT64_MAX, 0), uint64_t(INT64_MAX));
  EXPECT_EQ(call(ServiceKind::Write, F, Base, 1), uint64_t(-22));
  EXPECT_EQ(call(ServiceKind::Lseek, Alias, 0, 1), uint64_t(INT64_MAX));
  EXPECT_EQ(call(ServiceKind::Read, Reader, Base + 512, 8), 3u);
  EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'a', 'b', 'c'}));
  // Refusal leaves captured metadata valid and no bytes or cursor committed.
  EXPECT_EQ(call(ServiceKind::Fstat, Reader, Base + 512), 0u);
  EXPECT_EQ(call(ServiceKind::Write, F, UINT64_MAX, 1), uint64_t(-14));
  ASSERT_EQ(call(ServiceKind::Lseek, Alias, 100, 0), 100u);
  llvm::cantFail(cpu().write(Base + 256, {'Z'}));
  EXPECT_EQ(call(ServiceKind::Write, F, Base + 256, 1), 1u);
  EXPECT_EQ(call(ServiceKind::Lseek, Alias, 0, 1), 4u);
  EXPECT_EQ(call(ServiceKind::Read, Reader, Base + 512, 8), 1u);
  EXPECT_EQ(bytes(Base + 512, 1), (std::vector<uint8_t>{'Z'}));
}
INSTANTIATE_TEST_SUITE_P(LP64, LinuxFileNamespace,
                         testing::Values(GuestArchitecture::X64,
                                         GuestArchitecture::AArch64));
} // namespace
} // namespace neverd::emulation
