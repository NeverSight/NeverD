//===- DarwinMemoryTests.cpp - Darwin VM errors and ownership ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinFileTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"

#include <cstring>

namespace neverd::emulation::darwin_model {
namespace {
class DarwinMemoryTest : public testing::TestWithParam<uint64_t> {
protected:
  std::shared_ptr<AddressSpace> Space;
  std::unique_ptr<DarwinMemory> Memory;
  ProcessOptions Options;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "test"};
  uint64_t Page = 0;
  void SetUp() override {
    Page = GetParam();
    auto Physical = PhysicalMemory::create(Page * 4);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 4);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    Options.MemoryLimit = Page * 4;
    Options.StackSize = Page;
    MemoryLayout Layout{Page, 0x100000000ULL, {}};
    Memory = std::make_unique<DarwinMemory>(*Space, Layout, Options);
    Files = std::make_unique<DarwinFiles>(*Space, Options.DarwinFiles);
  }
  ServiceResult call(ServiceKind Kind, std::array<uint64_t, 6> Args) {
    ProcessServiceEvent E{0, 0, Args, std::nullopt};
    auto Value = Memory->handle(Kind, E, *Files, Result);
    EXPECT_TRUE(bool(Value))
        << (Value ? "" : llvm::toString(Value.takeError()));
    if (!Value || !*Value)
      return {UINT64_MAX, true};
    return **Value;
  }
  uint64_t allocate(uint64_t Size) {
    auto Value = call(ServiceKind::Mmap, {0, Size, 3, 0x1002, UINT64_MAX, 0});
    EXPECT_FALSE(Value.Error);
    EXPECT_EQ(Value.Value % Page, 0u);
    return Value.Value;
  }
  ServiceResult fileCall(ServiceKind Kind, std::array<uint64_t, 6> Args) {
    auto Value = Files->handle(Kind, {0, 0, Args, std::nullopt}, Result);
    EXPECT_TRUE(bool(Value))
        << (Value ? "" : llvm::toString(Value.takeError()));
    if (!Value || !*Value)
      return {UINT64_MAX, true};
    return **Value;
  }
  uint64_t openFile(std::vector<uint8_t> Bytes, uint32_t Flags = 0) {
    if (!Options.DarwinFiles)
      Options.DarwinFiles.emplace();
    Options.DarwinFiles->Files["/data"] = std::move(Bytes);
    const uint64_t Address = 0x100000;
    llvm::cantFail(Space->map(Address, Page, Read | Write | UserAccessible));
    const uint8_t Path[] = {'/', 'd', 'a', 't', 'a', 0};
    llvm::cantFail(Space->write(Address, Path));
    auto FD = fileCall(ServiceKind::Open, {Address, Flags});
    EXPECT_FALSE(FD.Error);
    llvm::cantFail(Space->unmap(Address, Page));
    return FD.Value;
  }
};
TEST_P(DarwinMemoryTest, SymbolicDescriptorsRefuseNativePrivateAndSharedModes) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->SymbolicLinks["/alias"] = {'d', 'a', 't', 'a'};
  const auto Scratch = allocate(Page);
  const uint8_t Path[] = {'/', 'a', 'l', 'i', 'a', 's', 0};
  llvm::cantFail(Space->write(Scratch, Path));
  const auto Mapped = Space->mappedBytes();
  const auto Allocated = Space->physicalMemory()->allocatedBytes();
  for (uint64_t Access = 0; Access != 3; ++Access) {
    const auto FD = fileCall(ServiceKind::Open, {Scratch, 0x200000 | Access});
    ASSERT_FALSE(FD.Error);
    for (uint64_t Protection : {1, 2, 3})
      for (uint64_t Flags : {1, 2}) {
        auto Reply =
            call(ServiceKind::Mmap, {0, 16384, Protection, Flags, FD.Value, 0});
        EXPECT_TRUE(Reply.Error);
        EXPECT_EQ(Reply.Value, 22u);
        EXPECT_EQ(Space->mappedBytes(), Mapped);
        EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Allocated);
      }
    EXPECT_FALSE(fileCall(ServiceKind::Close, {FD.Value}).Error);
  }
}

TEST_P(DarwinMemoryTest, SymbolicSharedAdmissionKeepsUnknownMappingBoundaries) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'d', 'a', 't', 'a'};
  Options.DarwinFiles->SymbolicLinks["/alias"] = {'d', 'a', 't', 'a'};
  const auto Scratch = allocate(Page);
  const uint8_t Path[] = {'/', 'a', 'l', 'i', 'a', 's', 0};
  llvm::cantFail(Space->write(Scratch, Path));
  const auto FD = fileCall(ServiceKind::Open, {Scratch, 0x200000});
  ASSERT_FALSE(FD.Error);
  const auto Mapped = Space->mappedBytes();
  for (std::array<uint64_t, 6> Args :
       {std::array<uint64_t, 6>{0, Page, 4, 1, FD.Value, 0},
        {0, Page, 1, 0x1001, FD.Value, 0},
        {0, Page, 1, 3, FD.Value, 0},
        {0, Page, 1, 1, FD.Value, 1},
        {0, Page, 1, 1, 99, 0}}) {
    EXPECT_EQ(call(ServiceKind::Mmap, Args).Value, UINT64_MAX);
    EXPECT_EQ(Result.Diagnostic, diagnostic::MemoryMode);
    EXPECT_EQ(Space->mappedBytes(), Mapped);
  }
  const uint8_t RegularPath[] = {'/', 'd', 'a', 't', 'a', 0};
  llvm::cantFail(Space->write(Scratch, RegularPath));
  const auto Opened = fileCall(ServiceKind::Open, {Scratch});
  ASSERT_FALSE(Opened.Error);
  const auto Regular = Opened.Value;
  EXPECT_EQ(call(ServiceKind::Mmap, {0, Page, 1, 1, Regular, 0}).Value,
            UINT64_MAX);
  EXPECT_EQ(Result.Diagnostic, diagnostic::MemoryMode);
  auto Zero = call(ServiceKind::Mmap, {0, 0, 1, 2, FD.Value, 0});
  EXPECT_TRUE(Zero.Error);
  EXPECT_EQ(Zero.Value, 22u);
}

TEST_P(DarwinMemoryTest,
       FileMutationWaitsForAllMappedRangesAfterDescriptorClose) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->WritableFiles.insert("/data");
  auto FD = openFile(std::vector<uint8_t>(Page * 2, 'x'), 2);
  auto A = call(ServiceKind::Mmap, {0, Page * 2, 3, 2, FD, 0});
  auto B = call(ServiceKind::Mmap, {0, Page, 0, 2, FD, 0});
  ASSERT_FALSE(A.Error);
  ASSERT_FALSE(B.Error);
  EXPECT_EQ(fileCall(ServiceKind::Ftruncate, {FD, 0}).Value, UINT64_MAX);
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  EXPECT_FALSE(fileCall(ServiceKind::Close, {FD}).Error);
  const auto Scratch = allocate(Page);
  const uint8_t Path[] = {'/', 'd', 'a', 't', 'a', 0};
  llvm::cantFail(Space->write(Scratch, Path));
  EXPECT_EQ(fileCall(ServiceKind::Open, {Scratch, 0x402}).Value, UINT64_MAX);
  auto Reopened = fileCall(ServiceKind::Open, {Scratch, 2});
  ASSERT_FALSE(Reopened.Error);
  EXPECT_EQ(Reopened.Value, FD);
  EXPECT_FALSE(call(ServiceKind::Munmap, {A.Value, Page}).Error);
  EXPECT_FALSE(call(ServiceKind::Mprotect, {B.Value, Page, 3}).Error);
  EXPECT_EQ(fileCall(ServiceKind::Write, {FD, Scratch, 1}).Value, UINT64_MAX);
  EXPECT_FALSE(call(ServiceKind::Munmap, {A.Value + Page, Page}).Error);
  EXPECT_EQ(fileCall(ServiceKind::Ftruncate, {FD, 0}).Value, UINT64_MAX);
  EXPECT_FALSE(call(ServiceKind::Munmap, {B.Value, Page}).Error);
  EXPECT_EQ(fileCall(ServiceKind::Pwrite, {FD, Scratch + 1, 1, 0}).Value, 1u);
  auto Fresh = call(ServiceKind::Mmap, {0, Page, 1, 2, FD, 0});
  ASSERT_FALSE(Fresh.Error);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Fresh.Value, 1)), 'd');
  EXPECT_FALSE(call(ServiceKind::Munmap, {Fresh.Value, Page}).Error);
  EXPECT_FALSE(fileCall(ServiceKind::Ftruncate, {FD, 0}).Error);
}

TEST_P(DarwinMemoryTest, WriteOnlyNoneMappingRetainsLeaseAndCanGainReadWrite) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->WritableFiles.insert("/data");
  auto FD = openFile({'a', 'b'}, 1);
  for (auto Prot : {1u, 2u, 3u}) {
    for (auto Length : {uint64_t(0), Page}) {
      auto Denied = call(ServiceKind::Mmap, {0, Length, Prot, 2, FD, 0});
      EXPECT_TRUE(Denied.Error);
      EXPECT_EQ(Denied.Value, 13u);
    }
  }
  EXPECT_FALSE(call(ServiceKind::Mmap, {0, 0, 0, 2, FD, 0}).Error);
  EXPECT_FALSE(fileCall(ServiceKind::Ftruncate, {FD, 2}).Error);
  auto M = call(ServiceKind::Mmap, {0, Page, 0, 2, FD, 0});
  ASSERT_FALSE(M.Error);
  EXPECT_EQ(fileCall(ServiceKind::Ftruncate, {FD, 0}).Value, UINT64_MAX);
  EXPECT_FALSE(call(ServiceKind::Mprotect, {M.Value, Page, 3}).Error);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(M.Value, 1)), 'a');
  EXPECT_FALSE(call(ServiceKind::Mprotect, {M.Value, Page, 0}).Error);
  EXPECT_EQ(fileCall(ServiceKind::Ftruncate, {FD, 0}).Value, UINT64_MAX);
  EXPECT_FALSE(call(ServiceKind::Munmap, {M.Value, Page}).Error);
  EXPECT_FALSE(fileCall(ServiceKind::Ftruncate, {FD, 0}).Error);
}

TEST_P(DarwinMemoryTest, UnlinkedFileBudgetSurvivesPartialUnmapAndFinalClose) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/other"] = {};
  Options.DarwinFiles->WritableFiles = {"/data", "/other"};
  Options.DarwinFiles->MutableDirectories.insert("/");
  const auto FD = openFile(std::vector<uint8_t>(Page * 2, 'x'), 2);
  auto Mapping = call(ServiceKind::Mmap, {0, Page * 2, 1, 2, FD, 0});
  ASSERT_FALSE(Mapping.Error);
  const auto Scratch = allocate(Page);
  const uint8_t Other[] = {'/', 'o', 't', 'h', 'e', 'r', 0};
  llvm::cantFail(Space->write(Scratch, Other));
  const auto B = fileCall(ServiceKind::Open, {Scratch, 2});
  ASSERT_FALSE(B.Error);
  const uint64_t Capacity = darwin_file_limits::Bytes - 2 * 6 - 2 * 7 - 2;
  EXPECT_FALSE(
      fileCall(ServiceKind::Ftruncate, {B.Value, Capacity - Page * 2}).Error);
  const uint8_t Path[] = {'/', 'd', 'a', 't', 'a', 0};
  llvm::cantFail(Space->write(Scratch, Path));
  EXPECT_FALSE(fileCall(ServiceKind::Unlink, {Scratch}).Error);
  EXPECT_FALSE(fileCall(ServiceKind::Close, {FD}).Error);
  EXPECT_EQ(fileCall(ServiceKind::Ftruncate, {B.Value, Capacity - Page * 2 + 1})
                .Value,
            UINT64_MAX);
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Mapping.Value, 1)), 'x');
  EXPECT_FALSE(call(ServiceKind::Munmap, {Mapping.Value, Page}).Error);
  EXPECT_EQ(fileCall(ServiceKind::Ftruncate, {B.Value, Capacity - Page * 2 + 1})
                .Value,
            UINT64_MAX);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Mapping.Value + Page, 1)), 'x');
  EXPECT_FALSE(call(ServiceKind::Munmap, {Mapping.Value + Page, Page}).Error);
  EXPECT_FALSE(fileCall(ServiceKind::Ftruncate, {B.Value, Capacity}).Error);
  auto Missing = fileCall(ServiceKind::Open, {Scratch});
  EXPECT_TRUE(Missing.Error);
  EXPECT_EQ(Missing.Value, 2u);
}

TEST_P(DarwinMemoryTest,
       RecreatedNamesKeepMappingsAndStorageLifetimesSeparate) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/other"] = {};
  Options.DarwinFiles->WritableFiles = {"/data", "/other"};
  Options.DarwinFiles->MutableDirectories.insert("/");
  const auto OldFD = openFile(std::vector<uint8_t>(Page * 2, 'x'), 2);
  auto Old = call(ServiceKind::Mmap, {0, Page * 2, 1, 2, OldFD, 0});
  ASSERT_FALSE(Old.Error);
  const auto Scratch = allocate(Page);
  const uint8_t Path[] = {'/', 'd', 'a', 't', 'a', 0};
  llvm::cantFail(Space->write(Scratch, Path));
  EXPECT_FALSE(fileCall(ServiceKind::Unlink, {Scratch}).Error);
  const auto NewFD = fileCall(ServiceKind::Open, {Scratch, 0xa02});
  ASSERT_FALSE(NewFD.Error);
  EXPECT_FALSE(fileCall(ServiceKind::Ftruncate, {NewFD.Value, Page}).Error);
  EXPECT_EQ(
      fileCall(ServiceKind::Pwrite, {NewFD.Value, Scratch + 1, 1, 0}).Value,
      1u);
  auto Fresh = call(ServiceKind::Mmap, {0, Page, 1, 2, NewFD.Value, 0});
  ASSERT_FALSE(Fresh.Error);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Old.Value, 1)), 'x');
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Fresh.Value, 1)), 'd');
  EXPECT_EQ(fileCall(ServiceKind::Ftruncate, {OldFD, 0}).Value, UINT64_MAX);
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  EXPECT_FALSE(fileCall(ServiceKind::Close, {OldFD}).Error);
  EXPECT_FALSE(fileCall(ServiceKind::Unlink, {Scratch}).Error);
  EXPECT_FALSE(fileCall(ServiceKind::Close, {NewFD.Value}).Error);
  const uint8_t Other[] = {'/', 'o', 't', 'h', 'e', 'r', 0};
  llvm::cantFail(Space->write(Scratch, Other));
  const auto B = fileCall(ServiceKind::Open, {Scratch, 2});
  ASSERT_FALSE(B.Error);
  const uint64_t Capacity = darwin_file_limits::Bytes - 2 * 6 - 2 * 7 - 2;
  const uint64_t Occupied = Page * 3 + 6;
  EXPECT_FALSE(
      fileCall(ServiceKind::Ftruncate, {B.Value, Capacity - Occupied}).Error);
  EXPECT_EQ(fileCall(ServiceKind::Ftruncate, {B.Value, Capacity - Occupied + 1})
                .Value,
            UINT64_MAX);
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_FALSE(call(ServiceKind::Munmap, {Old.Value, Page}).Error);
  EXPECT_EQ(fileCall(ServiceKind::Ftruncate, {B.Value, Capacity - Occupied + 1})
                .Value,
            UINT64_MAX);
  EXPECT_FALSE(call(ServiceKind::Munmap, {Old.Value + Page, Page}).Error);
  EXPECT_FALSE(
      fileCall(ServiceKind::Ftruncate, {B.Value, Capacity - Page - 6}).Error);
  EXPECT_EQ(
      fileCall(ServiceKind::Ftruncate, {B.Value, Capacity - Page - 5}).Value,
      UINT64_MAX);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Fresh.Value, 1)), 'd');
  EXPECT_FALSE(call(ServiceKind::Munmap, {Fresh.Value, Page}).Error);
  EXPECT_FALSE(fileCall(ServiceKind::Ftruncate, {B.Value, Capacity}).Error);
}

TEST_P(DarwinMemoryTest, RenameReplacementRetainsEachMappedObjectUntilRelease) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/target"] = std::vector<uint8_t>(Page * 2, 'T');
  Options.DarwinFiles->Files["/other"] = {};
  Options.DarwinFiles->WritableFiles = {"/data", "/other"};
  Options.DarwinFiles->MutableDirectories.insert("/");
  const auto A = openFile(std::vector<uint8_t>(Page, 'S'), 2);
  const auto Source = call(ServiceKind::Mmap, {0, Page, 1, 2, A, 0});
  ASSERT_FALSE(Source.Error);
  const auto Scratch = allocate(Page);
  const uint8_t Data[] = {'/', 'd', 'a', 't', 'a', 0};
  const uint8_t Target[] = {'/', 't', 'a', 'r', 'g', 'e', 't', 0};
  const uint8_t Other[] = {'/', 'o', 't', 'h', 'e', 'r', 0};
  llvm::cantFail(Space->write(Scratch, Data));
  llvm::cantFail(Space->write(Scratch + 32, Target));
  llvm::cantFail(Space->write(Scratch + 64, Other));
  const auto T = fileCall(ServiceKind::Open, {Scratch + 32});
  ASSERT_FALSE(T.Error);
  const auto Old = call(ServiceKind::Mmap, {0, Page * 2, 1, 2, T.Value, 0});
  ASSERT_FALSE(Old.Error);
  EXPECT_FALSE(fileCall(ServiceKind::Close, {T.Value}).Error);
  EXPECT_FALSE(fileCall(ServiceKind::Rename, {Scratch, Scratch + 32}).Error);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Source.Value, 1)), 'S');
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Old.Value, 1)), 'T');
  EXPECT_EQ(fileCall(ServiceKind::Ftruncate, {A, 0}).Value, UINT64_MAX);
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  const auto B = fileCall(ServiceKind::Open, {Scratch + 64, 2});
  ASSERT_FALSE(B.Error);
  // 36 bytes of fixed paths/references/grant and an eight-byte renamed path.
  const uint64_t Capacity = darwin_file_limits::Bytes - 44;
  EXPECT_FALSE(
      fileCall(ServiceKind::Ftruncate, {B.Value, Capacity - Page * 3}).Error);
  EXPECT_FALSE(call(ServiceKind::Munmap, {Old.Value, Page}).Error);
  EXPECT_EQ(fileCall(ServiceKind::Ftruncate, {B.Value, Capacity - Page * 3 + 1})
                .Value,
            UINT64_MAX);
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Old.Value + Page, 1)), 'T');
  EXPECT_FALSE(call(ServiceKind::Munmap, {Old.Value + Page, Page}).Error);
  EXPECT_FALSE(
      fileCall(ServiceKind::Ftruncate, {B.Value, Capacity - Page}).Error);
  EXPECT_FALSE(fileCall(ServiceKind::Close, {A}).Error);
  EXPECT_FALSE(call(ServiceKind::Munmap, {Source.Value, Page}).Error);
  EXPECT_EQ(
      fileCall(ServiceKind::Ftruncate, {B.Value, Capacity - Page + 1}).Value,
      UINT64_MAX);
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_FALSE(fileCall(ServiceKind::Unlink, {Scratch + 32}).Error);
  EXPECT_FALSE(fileCall(ServiceKind::Ftruncate, {B.Value, Capacity + 8}).Error);
  EXPECT_EQ(fileCall(ServiceKind::Ftruncate, {B.Value, Capacity + 9}).Value,
            UINT64_MAX);
}

TEST_P(DarwinMemoryTest, FailedAndLegacyZeroFileMappingsDoNotRetainLeases) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->WritableFiles.insert("/data");
  auto FD = openFile({'a'}, 2);
  EXPECT_FALSE(call(ServiceKind::Mmap, {0, 0, 1, 2, FD, 0}).Error);
  EXPECT_FALSE(fileCall(ServiceKind::Ftruncate, {FD, Page}).Error);
  EXPECT_EQ(call(ServiceKind::Mmap, {0, Page * 2, 1, 2, FD, 0}).Value,
            UINT64_MAX);
  EXPECT_FALSE(fileCall(ServiceKind::Ftruncate, {FD, Page * 4}).Error);
  auto Full = allocate(Page * 4);
  auto NoSpace = call(ServiceKind::Mmap, {0, Page, 1, 2, FD, 0});
  EXPECT_TRUE(NoSpace.Error);
  EXPECT_EQ(NoSpace.Value, 12u);
  EXPECT_FALSE(fileCall(ServiceKind::Ftruncate, {FD, 0}).Error);
  EXPECT_FALSE(call(ServiceKind::Munmap, {Full, Page * 4}).Error);
}

TEST_P(DarwinMemoryTest, PartialUnmapRetiresBudgetAndFreshMappingIsZero) {
  const auto Address = allocate(Page * 4);
  ASSERT_FALSE(bool(Space->writeInteger(Address + Page, 0xff, 1)));
  auto Full = call(ServiceKind::Mmap, {0, Page, 3, 0x1002, UINT64_MAX, 0});
  EXPECT_TRUE(Full.Error);
  EXPECT_EQ(Full.Value, 12u);
  EXPECT_EQ(call(ServiceKind::Munmap, {Address + Page, Page, 0, 0, 0, 0}).Value,
            0u);
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Page * 3);
  auto Reused =
      call(ServiceKind::Mmap, {Address + Page, Page, 3, 0x1002, UINT64_MAX, 0});
  ASSERT_FALSE(Reused.Error);
  EXPECT_EQ(Reused.Value, Address + Page);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Reused.Value, 1)), 0u);
}
TEST_P(DarwinMemoryTest, ProtectionAcrossHoleCannotChangeEarlierPages) {
  auto Address = allocate(Page * 3);
  EXPECT_FALSE(
      call(ServiceKind::Munmap, {Address + Page, Page, 0, 0, 0, 0}).Error);
  auto Failed = call(ServiceKind::Mprotect, {Address, Page * 3, 0, 0, 0, 0});
  EXPECT_TRUE(Failed.Error);
  EXPECT_EQ(Failed.Value, 12u);
  EXPECT_TRUE(
      llvm::cantFail(Space->canAccess(Address, Page, Write | UserAccessible)));
  EXPECT_TRUE(llvm::cantFail(
      Space->canAccess(Address + Page * 2, Page, Write | UserAccessible)));
}
TEST_P(DarwinMemoryTest, AlignmentNoneAndWriteOnlyUseDarwinPageRules) {
  const auto Address = allocate(Page);
  auto Bad = call(ServiceKind::Mprotect, {Address + 1, Page, 1, 0, 0, 0});
  EXPECT_TRUE(Bad.Error);
  EXPECT_EQ(Bad.Value, 22u);
  if (Page == 16384) {
    Bad = call(ServiceKind::Munmap, {Address + 4096, 4096, 0, 0, 0, 0});
    EXPECT_TRUE(Bad.Error);
    EXPECT_EQ(Bad.Value, 22u);
  }
  EXPECT_FALSE(call(ServiceKind::Mprotect, {Address, Page, 0, 0, 0, 0}).Error);
  EXPECT_FALSE(
      llvm::cantFail(Space->canAccess(Address, 1, Read | UserAccessible)));
  EXPECT_FALSE(call(ServiceKind::Mprotect, {Address, Page, 2, 0, 0, 0}).Error);
  EXPECT_TRUE(llvm::cantFail(
      Space->canAccess(Address, Page, Read | Write | UserAccessible)));
}
TEST_P(DarwinMemoryTest, MaximumProtectionFailureIsAtomicAcrossSegments) {
  const uint64_t Address = 0x100000000ULL;
  ASSERT_FALSE(bool(Space->map(Address, Page * 2, Read | UserAccessible)));
  ProcessOptions Options;
  Options.MemoryLimit = Page * 4;
  Options.StackSize = Page;
  MemoryLayout Layout{
      Page, Address, {{Address, Page, 3}, {Address + Page, Page, 1}}};
  Memory = std::make_unique<DarwinMemory>(*Space, Layout, Options);
  auto Denied = call(ServiceKind::Mprotect, {Address, Page * 2, 3, 0, 0, 0});
  EXPECT_TRUE(Denied.Error);
  EXPECT_EQ(Denied.Value, 13u);
  EXPECT_FALSE(
      llvm::cantFail(Space->canAccess(Address, 1, Write | UserAccessible)));
  EXPECT_TRUE(llvm::cantFail(
      Space->canAccess(Address, Page * 2, Read | UserAccessible)));
}
TEST_P(DarwinMemoryTest, UnsupportedModesHaveNoMappingEffects) {
  for (auto Args : {std::array<uint64_t, 6>{0, Page, 7, 0x1002, UINT64_MAX, 0},
                    std::array<uint64_t, 6>{0, Page, 3, 0x1012, UINT64_MAX, 0},
                    std::array<uint64_t, 6>{0, Page, 3, 2, 0, 0}}) {
    ProcessServiceEvent E{0, 197, Args, std::nullopt};
    auto Value = Memory->handle(ServiceKind::Mmap, E, *Files, Result);
    ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
    EXPECT_FALSE(*Value);
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Space->mappedBytes(), 0u);
    EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), 0u);
  }
}
TEST_P(DarwinMemoryTest, RawMmapZeroLengthAndInvalidUnmapRemainDistinct) {
  const auto Before = Space->mappedBytes();
  for (auto Kind : {ServiceKind::Mmap, ServiceKind::Munmap}) {
    auto Zero = call(Kind, {0, 0, 3, 0x1002, UINT64_MAX, 0});
    EXPECT_EQ(Zero.Error, Kind == ServiceKind::Munmap);
    EXPECT_EQ(Zero.Value, Kind == ServiceKind::Munmap ? 22u : 0u);
    auto Overflow = call(Kind, {0, UINT64_MAX, 3, 0x1002, UINT64_MAX, 0});
    EXPECT_TRUE(Overflow.Error);
    EXPECT_EQ(Overflow.Value, 22u);
  }
  EXPECT_EQ(Space->mappedBytes(), Before);
}
TEST_P(DarwinMemoryTest, NonfixedHintRoundsUpAndSearchesAboveOccupiedHint) {
  const uint64_t Hint = 0x2000000000ULL;
  auto First =
      call(ServiceKind::Mmap, {Hint + 1, Page, 3, 0x1002, UINT64_MAX, 0});
  ASSERT_FALSE(First.Error);
  EXPECT_EQ(First.Value, Hint + Page);
  auto Second =
      call(ServiceKind::Mmap, {Hint + 1, Page, 3, 0x1002, UINT64_MAX, 0});
  ASSERT_FALSE(Second.Error);
  EXPECT_EQ(Second.Value, Hint + Page * 2);
  auto Fallback = call(ServiceKind::Mmap,
                       {value::UserLimit - 1, Page, 3, 0x1002, UINT64_MAX, 0});
  ASSERT_FALSE(Fallback.Error);
  EXPECT_LT(Fallback.Value, Hint);
}
TEST_P(DarwinMemoryTest, PrivateFileMappingsCopyPagesWithoutChangingFileState) {
  std::vector<uint8_t> Bytes(Page + 19);
  for (size_t I = 0; I < Bytes.size(); ++I)
    Bytes[I] = uint8_t(I * 17 + 5);
  const auto FD = openFile(Bytes);
  EXPECT_EQ(fileCall(ServiceKind::Lseek, {FD, 7, 0}).Value, 7u);
  const auto First = call(ServiceKind::Mmap, {0, 1, 3, 2, FD, 0});
  ASSERT_FALSE(First.Error);
  // mmap admits a whole page even when the requested byte count is one.
  EXPECT_EQ(llvm::cantFail(Space->readInteger(First.Value + Page - 1, 1)),
            Bytes[Page - 1]);
  const auto Tail = call(ServiceKind::Mmap, {0, 19, 0, 0x40002, FD, Page});
  ASSERT_FALSE(Tail.Error);
  EXPECT_FALSE(
      llvm::cantFail(Space->canAccess(Tail.Value, 1, Read | UserAccessible)));
  EXPECT_FALSE(call(ServiceKind::Mprotect, {Tail.Value, Page, 2}).Error);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Tail.Value, 1)), Bytes[Page]);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Tail.Value + 18, 1)),
            Bytes.back());
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Tail.Value + 19, 1)), 0u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Tail.Value + Page - 1, 1)), 0u);
  llvm::cantFail(Space->writeInteger(First.Value, 0xaa, 1));
  llvm::cantFail(Space->writeInteger(Tail.Value + Page - 1, 0xbb, 1));
  const auto Second = call(ServiceKind::Mmap, {0, Page, 1, 2, FD, 0});
  ASSERT_FALSE(Second.Error);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Second.Value, 1)), Bytes[0]);
  EXPECT_EQ(Options.DarwinFiles->Files.at("/data"), Bytes);
  EXPECT_EQ(fileCall(ServiceKind::Lseek, {FD, 0, 1}).Value, 7u);
  EXPECT_FALSE(fileCall(ServiceKind::Close, {FD}).Error);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(First.Value, 1)), 0xaau);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Tail.Value + Page - 1, 1)),
            0xbbu);
  EXPECT_EQ(call(ServiceKind::Mmap, {0, 1, 1, 2, FD, 0}).Value, 9u);
}
TEST_P(DarwinMemoryTest, FileMappingValidationPrecedesDescriptorAndBudget) {
  const auto FD = openFile({1, 2, 3});
  const auto Address = allocate(Page * 4);
  for (auto Args : {std::array<uint64_t, 6>{0, 0, 1, 0x40002, 99, 0},
                    std::array<uint64_t, 6>{0, 1, 1, 0x40002, 99, 1},
                    std::array<uint64_t, 6>{0, UINT64_MAX, 1, 2, 99, 0},
                    std::array<uint64_t, 6>{0, Page, 1, 2, 99, 0 - Page}}) {
    const auto V = call(ServiceKind::Mmap, Args);
    EXPECT_TRUE(V.Error);
    EXPECT_EQ(V.Value, 22u);
  }
  for (uint64_t Length : {uint64_t(0), Page}) {
    auto Bad = call(ServiceKind::Mmap, {0, Length, 1, 2, 99, 0});
    EXPECT_TRUE(Bad.Error);
    EXPECT_EQ(Bad.Value, 9u);
  }
  const auto Zero = call(ServiceKind::Mmap, {0, 0, 1, 2, FD, 0});
  EXPECT_FALSE(Zero.Error);
  EXPECT_EQ(Zero.Value, 0u);
  const auto Full = call(ServiceKind::Mmap, {0, Page, 1, 2, FD, 0});
  EXPECT_TRUE(Full.Error);
  EXPECT_EQ(Full.Value, 12u);
  EXPECT_EQ(Space->mappedBytes(), Page * 4);
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Page * 4);
  EXPECT_FALSE(call(ServiceKind::Munmap, {Address, Page}).Error);
  const auto Reused = call(ServiceKind::Mmap, {Address, 3, 1, 2, FD, 0});
  EXPECT_FALSE(Reused.Error);
  EXPECT_EQ(Reused.Value, Address);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Address, 1)), 1u);
}
TEST_P(DarwinMemoryTest, UnknownFileMappingEffectsStopBeforeAllocation) {
  const auto FD = openFile({1, 2, 3});
  for (auto Args : {std::array<uint64_t, 6>{0, 1, 1, 2, FD, 1},
                    std::array<uint64_t, 6>{0, Page + 1, 1, 2, FD, 0},
                    std::array<uint64_t, 6>{0, Page, 1, 2, FD, Page},
                    std::array<uint64_t, 6>{0, 1, 1, 2, FD, uint64_t(1) << 63},
                    std::array<uint64_t, 6>{0, Page, 1, 1, FD, 0},
                    std::array<uint64_t, 6>{0, Page, 5, 2, FD, 0},
                    std::array<uint64_t, 6>{0, Page, 1, 0x12, FD, 0},
                    std::array<uint64_t, 6>{0, Page, 1, 2, 1, 0}}) {
    auto V = Memory->handle(ServiceKind::Mmap, {0, 197, Args, std::nullopt},
                            *Files, Result);
    ASSERT_TRUE(bool(V)) << llvm::toString(V.takeError());
    EXPECT_FALSE(*V);
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_FALSE(Result.Diagnostic.empty());
    EXPECT_EQ(Space->mappedBytes(), 0u);
    EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), 0u);
    EXPECT_EQ(fileCall(ServiceKind::Lseek, {FD, 0, 1}).Value, 0u);
  }
}
TEST_P(DarwinMemoryTest, EmptyFilesHaveNoMappablePageButAllowLegacyZeroLength) {
  const auto FD = openFile({});
  const auto Zero = call(ServiceKind::Mmap, {0, 0, 1, 2, FD, 0});
  EXPECT_FALSE(Zero.Error);
  EXPECT_EQ(Zero.Value, 0u);
  auto V = Memory->handle(ServiceKind::Mmap,
                          {0, 197, {0, 1, 1, 2, FD, 0}, std::nullopt}, *Files,
                          Result);
  ASSERT_TRUE(bool(V)) << llvm::toString(V.takeError());
  EXPECT_FALSE(*V);
  EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Space->mappedBytes(), 0u);
}
TEST_P(DarwinMemoryTest, SymbolicLinkOpensRetainTheActualTargetMappingLease) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->WritableFiles.insert("/data");
  Options.DarwinFiles->SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
  const auto Direct = openFile(std::vector<uint8_t>(Page, 'x'), 2);
  const auto Scratch = allocate(Page);
  const uint8_t Link[] = {'/', 'l', 'i', 'n', 'k', 0};
  llvm::cantFail(Space->write(Scratch, Link));
  const auto Alias = fileCall(ServiceKind::Open, {Scratch, 2});
  ASSERT_FALSE(Alias.Error);
  auto A = call(ServiceKind::Mmap, {0, Page, 1, 2, Alias.Value, 0});
  ASSERT_FALSE(A.Error);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(A.Value, 1)), 'x');
  EXPECT_FALSE(fileCall(ServiceKind::Close, {Alias.Value}).Error);
  EXPECT_EQ(fileCall(ServiceKind::Ftruncate, {Direct, 0}).Value, UINT64_MAX);
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  EXPECT_FALSE(call(ServiceKind::Munmap, {A.Value, Page}).Error);
  EXPECT_FALSE(fileCall(ServiceKind::Ftruncate, {Direct, 0}).Error);
  EXPECT_EQ(fileCall(ServiceKind::ReadLink, {Scratch, Scratch + 64, 4}).Value,
            4u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Scratch + 64, 4)), 0x61746164u);
}

TEST_P(DarwinMemoryTest, FixedLinkReplacementSeparatesOldAndNewMappingLeases) {
  Options.DarwinFiles = darwin_test::mixedSymbolicLinkOptions();
  auto &O = *Options.DarwinFiles;
  O.Files["/work/data"] = std::vector<uint8_t>(Page, 'x');
  O.Metadata["/work/data"].Size = Page;
  O.Metadata["/work/data"].Blocks = Page / 512;
  ASSERT_FALSE(bool(validateFileOptions(O)));
  const auto Scratch = allocate(Page);
  auto Path = [&](const char *Text, uint64_t Offset = 0) {
    llvm::cantFail(Space->write(
        Scratch + Offset,
        llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Text),
                                std::strlen(Text) + 1)));
  };
  Path("/static/data-link");
  const auto Old = fileCall(ServiceKind::Open, {Scratch, 2});
  ASSERT_FALSE(Old.Error);
  const auto OldMap = call(ServiceKind::Mmap, {0, Page, 1, 2, Old.Value, 0});
  ASSERT_FALSE(OldMap.Error);
  Path("/static/data-link/");
  Path("/work/renamed", 128);
  EXPECT_FALSE(fileCall(ServiceKind::Rename, {Scratch, Scratch + 128}).Error);
  Path("/static/alias/renamed");
  EXPECT_FALSE(fileCall(ServiceKind::Unlink, {Scratch}).Error);
  Path("/static/data-link");
  const auto Replacement = fileCall(ServiceKind::Open, {Scratch, 0x202, 0600});
  ASSERT_FALSE(Replacement.Error) << Replacement.Value << Result.Diagnostic;
  // The old mapping's lease cannot inhibit mutations of the replacement.
  EXPECT_FALSE(fileCall(ServiceKind::Ftruncate, {Replacement.Value, 0}).Error);
  llvm::cantFail(Space->write(Scratch, std::vector<uint8_t>(Page, 'z')));
  EXPECT_EQ(
      fileCall(ServiceKind::Write, {Replacement.Value, Scratch, Page}).Value,
      Page);
  const auto NewMap =
      call(ServiceKind::Mmap, {0, Page, 1, 2, Replacement.Value, 0});
  ASSERT_FALSE(NewMap.Error);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(OldMap.Value, 1)), 'x');
  EXPECT_EQ(llvm::cantFail(Space->readInteger(NewMap.Value, 1)), 'z');
  EXPECT_EQ(fileCall(ServiceKind::Ftruncate, {Old.Value, 0}).Value, UINT64_MAX);
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  EXPECT_FALSE(fileCall(ServiceKind::Close, {Old.Value}).Error);
  EXPECT_FALSE(call(ServiceKind::Munmap, {OldMap.Value, Page}).Error);
  EXPECT_EQ(fileCall(ServiceKind::Ftruncate, {Replacement.Value, 0}).Value,
            UINT64_MAX);
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  EXPECT_FALSE(call(ServiceKind::Munmap, {NewMap.Value, Page}).Error);
  EXPECT_FALSE(fileCall(ServiceKind::Ftruncate, {Replacement.Value, 0}).Error);
}

TEST_P(DarwinMemoryTest, FixedLinkTargetBudgetWaitsForTheLastOldMappedRange) {
  Options.DarwinFiles = darwin_test::mixedSymbolicLinkOptions();
  auto &O = *Options.DarwinFiles;
  // Actual catalogue charges: file name; directory names; link names+targets;
  // CWD; writable and mutation references; mutable directory reference.
  constexpr uint64_t Fixed = 11 + 8 + 6 + 21 + 30 + 32 + 2 + 11 + 11 + 6;
  const auto Size = darwin_file_limits::Bytes - Fixed;
  O.Files["/work/data"] = std::vector<uint8_t>(Size, 'x');
  O.Metadata["/work/data"].Size = Size;
  O.Metadata["/work/data"].Blocks = ((Size + 4095) / 4096) * 8;
  ASSERT_FALSE(bool(validateFileOptions(O)));
  const auto Scratch = allocate(Page);
  const char Path[] = "/static/data-link/";
  llvm::cantFail(Space->write(
      Scratch, llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Path),
                                       sizeof(Path))));
  const auto Old = fileCall(ServiceKind::Open, {Scratch});
  ASSERT_FALSE(Old.Error);
  const auto Mapping =
      call(ServiceKind::Mmap, {0, Page * 2, 1, 2, Old.Value, 0});
  ASSERT_FALSE(Mapping.Error);
  EXPECT_FALSE(fileCall(ServiceKind::Unlink, {Scratch}).Error);
  // Remove only the slash: O_CREAT follows the fixed link to its missing
  // target.
  llvm::cantFail(Space->writeInteger(Scratch + sizeof(Path) - 2, 0, 1));
  EXPECT_FALSE(fileCall(ServiceKind::Close, {Old.Value}).Error);
  for (unsigned Part = 0; Part != 2; ++Part) {
    EXPECT_EQ(fileCall(ServiceKind::Open, {Scratch, 0x202, 0600}).Value,
              UINT64_MAX);
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
    EXPECT_EQ(
        llvm::cantFail(Space->readInteger(Mapping.Value + Page * Part, 1)),
        'x');
    EXPECT_FALSE(
        call(ServiceKind::Munmap, {Mapping.Value + Page * Part, Page}).Error);
  }
  const auto New = fileCall(ServiceKind::Open, {Scratch, 0x202, 0600});
  ASSERT_FALSE(New.Error);
  EXPECT_EQ(New.Value, Old.Value);
  EXPECT_FALSE(
      fileCall(ServiceKind::Fstat64, {New.Value, Scratch + 256}).Error);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Scratch + 256 + 8, 8)),
            darwin_test::CreationPolicy.FirstInode);
}

INSTANTIATE_TEST_SUITE_P(Pages, DarwinMemoryTest, testing::Values(4096, 16384));
} // namespace
} // namespace neverd::emulation::darwin_model
