//===- LinuxMemoryTests.cpp - Process mapping and raw syscall semantics ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxMemory.h"

namespace neverd::emulation {
namespace {
#define NEVERD_LINUX_MEMORY_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "fixtures/LinuxMemoryCases.def"
#undef NEVERD_LINUX_MEMORY_VALUE
using linux_model::ServiceKind;

class LinuxMemory : public testing::Test {
protected:
  std::shared_ptr<PhysicalMemory> RAM;
  std::shared_ptr<AddressSpace> Space;
  std::unique_ptr<linux_model::LinuxMemory> Memory;
  ProcessOptions Options;
  ProcessResult Report{ProcessProfile::LinuxELF64,
                       GuestArchitecture::X64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  void SetUp() override {
    RAM = llvm::cantFail(PhysicalMemory::create(UnitLimit));
    Space = llvm::cantFail(AddressSpace::create(RAM, UnitLimit));
    Options.MemoryLimit = UnitLimit;
    Options.StackSize = PageSize;
    const linux_model::MemoryLayout Layout{UserLimit, PageSize};
    Memory = std::make_unique<linux_model::LinuxMemory>(*Space, Layout,
                                                        UnitHeap, Options);
  }
  std::optional<uint64_t> call(ServiceKind Kind, uint64_t A0 = 0,
                               uint64_t A1 = 0, uint64_t A2 = 0,
                               uint64_t A3 = 0, uint64_t A4 = 0,
                               uint64_t A5 = 0) {
    return llvm::cantFail(
        Memory->handle(Kind, {0, 0, {A0, A1, A2, A3, A4, A5}, {}}, Report));
  }
  std::vector<uint8_t> readBytes(uint64_t Address, size_t Size) {
    std::vector<uint8_t> Bytes(Size);
    llvm::cantFail(Space->read(Address, Bytes));
    return Bytes;
  }
  uint64_t map(uint64_t Size, uint64_t Hint = UnitHint) {
    return *call(ServiceKind::Mmap, Hint, Size, ProtRead | ProtWrite,
                 PrivateAnonymous, UINT64_MAX);
  }
};

TEST_F(LinuxMemory, NegativePrctlOptionsNarrowToIntAndPreserveAllMemory) {
  ASSERT_EQ(map(PageSize), UnitHint);
  const std::vector<uint8_t> Before(PageSize, 0xa5);
  llvm::cantFail(Space->write(UnitHint, Before));
  const auto Generation = Space->mappingGeneration();
  const auto Allocated = RAM->allocatedBytes();
  for (uint64_t Option : {0x80000000ULL, 0xffffffffULL, 0xffffffff80000000ULL,
                          0x1234567880000001ULL}) {
    SCOPED_TRACE(Option);
    EXPECT_EQ(call(ServiceKind::Prctl, Option, UINT64_MAX, UINT64_MAX,
                   UINT64_MAX, UINT64_MAX),
              uint64_t(-22));
    EXPECT_TRUE(Report.Diagnostic.empty());
    EXPECT_EQ(readBytes(UnitHint, PageSize), Before);
    EXPECT_EQ(Space->mappingGeneration(), Generation);
    EXPECT_EQ(RAM->allocatedBytes(), Allocated);
  }
}
TEST_F(LinuxMemory, PositiveUnmodeledPrctlDoesNotInventInvalidArgument) {
  for (uint64_t Option : {0ULL, 15ULL, 0x7fffffffULL, 0x800000000000000fULL}) {
    SCOPED_TRACE(Option);
    EXPECT_FALSE(call(ServiceKind::Prctl, Option, UINT64_MAX, UINT64_MAX,
                      UINT64_MAX, UINT64_MAX));
    EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Report.Diagnostic, "prctl operation is unmodeled");
  }
}

TEST_F(LinuxMemory, ResidencyErrorsUseRangesBeforeMappingsAndLeaveMemoryAlone) {
  ASSERT_EQ(map(PageSize), UnitHint);
  llvm::cantFail(Space->writeInteger(UnitHint, 0x8877665544332211, 8));
  const auto Generation = Space->mappingGeneration();
  const uint64_t Allocated = RAM->allocatedBytes();
  EXPECT_EQ(call(ServiceKind::Mincore, 1, 0, UINT64_MAX), uint64_t(0) - 22);
  EXPECT_EQ(call(ServiceKind::Mincore, UserLimit, 1, UINT64_MAX),
            uint64_t(0) - 12);
  EXPECT_EQ(call(ServiceKind::Mincore, 0, 1, UserLimit), uint64_t(0) - 14);
  EXPECT_EQ(call(ServiceKind::Mincore, 0, PageSize + 1, UserLimit - 1),
            uint64_t(0) - 14);
  EXPECT_EQ(call(ServiceKind::Mincore, 0, 1, 0), uint64_t(0) - 12);
  EXPECT_EQ(call(ServiceKind::Mincore, 0, PageSize + 1, UnitHint),
            uint64_t(0) - 12);
  EXPECT_EQ(call(ServiceKind::Mincore, 0, 0, 0), 0u);
  EXPECT_EQ(call(ServiceKind::Mincore, UserLimit, 0, UserLimit), 0u);
  EXPECT_EQ(call(ServiceKind::Mincore, UserLimit, 0, UserLimit + 1),
            uint64_t(0) - 14);
  EXPECT_EQ(Space->mappingGeneration(), Generation);
  EXPECT_EQ(RAM->allocatedBytes(), Allocated);
  EXPECT_EQ(*Space->readInteger(UnitHint, 8), 0x8877665544332211);
}

TEST_F(LinuxMemory,
       MappedResidencyIsUnknownEvenBeforeAHoleOrWithoutPermissions) {
  ASSERT_EQ(map(PageSize), UnitHint);
  for (uint64_t Length : {uint64_t(1), 2 * PageSize}) {
    EXPECT_FALSE(call(ServiceKind::Mincore, UnitHint, Length, 0));
    EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(Report.Diagnostic.find("page residency"), std::string::npos);
  }
  ASSERT_EQ(call(ServiceKind::Mprotect, UnitHint, PageSize, 0), 0u);
  EXPECT_FALSE(call(ServiceKind::Mincore, UnitHint, PageSize, 0));
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint, PageSize), 0u);
  EXPECT_EQ(call(ServiceKind::Mincore, UnitHint, PageSize, 0),
            uint64_t(0) - 12);
}

TEST_F(LinuxMemory, ResidencyObservationsPreserveBitsGuardsAndPermissions) {
  Options.LinuxKernel.emplace().ResidencyObservations =
      std::vector<LinuxResidencyObservation>{{UnitHint, {0, 1, 0}},
                                             {UnitHint, {1, 0, 1}}};
  ASSERT_EQ(map(3 * PageSize), UnitHint);
  const uint64_t Out = UnitHint + 4 * PageSize;
  ASSERT_EQ(map(PageSize, Out), Out);
  std::vector<uint8_t> Expected(PageSize, 0xa5);
  llvm::cantFail(Space->write(Out, Expected));
  ASSERT_EQ(call(ServiceKind::Mprotect, UnitHint, PageSize, 0), 0u);
  ASSERT_EQ(
      call(ServiceKind::Mprotect, UnitHint + PageSize, PageSize, ProtRead), 0u);
  const auto Generation = Space->mappingGeneration();
  const auto Allocated = RAM->allocatedBytes();
  ASSERT_EQ(call(ServiceKind::Mincore, UnitHint, 2 * PageSize + 1, Out + 7),
            0u);
  Expected[7] = 0;
  Expected[8] = 1;
  Expected[9] = 0;
  EXPECT_EQ(readBytes(Out, PageSize), Expected);
  ASSERT_EQ(call(ServiceKind::Mincore, UnitHint, 3 * PageSize, Out + 7), 0u);
  Expected[7] = 1;
  Expected[8] = 0;
  Expected[9] = 1;
  EXPECT_EQ(readBytes(Out, PageSize), Expected);
  EXPECT_FALSE(call(ServiceKind::Mincore, UnitHint, 3 * PageSize, Out));
  EXPECT_NE(Report.Diagnostic.find("exhausted"), std::string::npos);
  EXPECT_EQ(Space->mappingGeneration(), Generation);
  EXPECT_EQ(RAM->allocatedBytes(), Allocated);
}
TEST_F(LinuxMemory, ResidencyEarlyErrorsDoNotConsumeButCopyFaultDoes) {
  Options.LinuxKernel.emplace().ResidencyObservations =
      std::vector<LinuxResidencyObservation>{{UnitHint, {0}}, {UnitHint, {1}}};
  ASSERT_EQ(map(PageSize), UnitHint);
  EXPECT_EQ(call(ServiceKind::Mincore, UnitHint + 1, PageSize, 0),
            uint64_t(-22));
  EXPECT_EQ(call(ServiceKind::Mincore, UnitHint, UINT64_MAX, 0), uint64_t(-12));
  EXPECT_EQ(call(ServiceKind::Mincore, UnitHint, PageSize, UINT64_MAX),
            uint64_t(-14));
  EXPECT_EQ(call(ServiceKind::Mincore, 0, PageSize, 0), uint64_t(-12));
  EXPECT_EQ(call(ServiceKind::Mincore, UnitHint, 0, UnitHint), 0u);
  // The first observed query reaches copy_to_user, so even EFAULT consumes it.
  EXPECT_EQ(call(ServiceKind::Mincore, UnitHint, PageSize, 0), uint64_t(-14));
  EXPECT_EQ(call(ServiceKind::Mincore, UnitHint, PageSize, UnitHint), 0u);
  EXPECT_EQ(*Space->readInteger(UnitHint, 1), 1u);
  EXPECT_FALSE(call(ServiceKind::Mincore, UnitHint, PageSize, UnitHint));
}
TEST_F(LinuxMemory, ResidencyMismatchAndUnknownBackingDoNotConsumeInput) {
  // Input RAM is not an anonymous mapping allocated by LinuxMemory.
  llvm::cantFail(Space->map(UnitHint, PageSize, Read | Write | UserAccessible));
  Options.LinuxKernel.emplace().ResidencyObservations =
      std::vector<LinuxResidencyObservation>{{UnitHint, {1}}};
  EXPECT_FALSE(call(ServiceKind::Mincore, UnitHint, PageSize, UnitHint));
  EXPECT_NE(Report.Diagnostic.find("backing"), std::string::npos);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint, PageSize), 0u);
  ASSERT_EQ(map(2 * PageSize), UnitHint);
  EXPECT_FALSE(
      call(ServiceKind::Mincore, UnitHint + PageSize, PageSize, UnitHint));
  EXPECT_FALSE(call(ServiceKind::Mincore, UnitHint, 2 * PageSize, UnitHint));
  EXPECT_NE(Report.Diagnostic.find("match"), std::string::npos);
  EXPECT_EQ(call(ServiceKind::Mincore, UnitHint, PageSize, UnitHint), 0u);
  EXPECT_EQ(*Space->readInteger(UnitHint, 1), 1u);
}
TEST_F(LinuxMemory, ResidencyHoleKeepsCompletedPrefixAndCannotResurrectPages) {
  Options.LinuxKernel.emplace().ResidencyObservations =
      std::vector<LinuxResidencyObservation>{{UnitHint, {1, 0, 1}},
                                             {UnitHint, {0, 1, 0}}};
  ASSERT_EQ(map(3 * PageSize), UnitHint);
  const uint64_t Out = UnitHint + 4 * PageSize;
  ASSERT_EQ(map(PageSize, Out), Out);
  llvm::cantFail(Space->writeInteger(Out, 0xa5a5a5a5, 4));
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint + PageSize, PageSize), 0u);
  EXPECT_EQ(call(ServiceKind::Mincore, UnitHint, 3 * PageSize, Out),
            uint64_t(-12));
  EXPECT_EQ(*Space->readInteger(Out, 4), 0xa5a5a501u);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint, PageSize), 0u);
  EXPECT_EQ(call(ServiceKind::Mincore, UnitHint, 3 * PageSize, Out),
            uint64_t(-12));
  ASSERT_EQ(map(2 * PageSize), UnitHint);
  EXPECT_EQ(call(ServiceKind::Mincore, UnitHint, 3 * PageSize, Out), 0u);
  EXPECT_EQ(*Space->readInteger(Out, 4), 0xa5000100u);
}
TEST_F(LinuxMemory, ResidencySplitCopyFaultPreservesOnlyCompletedChunks) {
  Options.LinuxKernel.emplace().ResidencyObservations =
      std::vector<LinuxResidencyObservation>{{UnitHint, {1, 0, 1}}};
  ASSERT_EQ(map(3 * PageSize), UnitHint);
  ASSERT_EQ(call(ServiceKind::Mprotect, UnitHint, PageSize, ProtRead), 0u);
  const uint64_t Out = UnitHint + 4 * PageSize;
  ASSERT_EQ(map(PageSize, Out), Out);
  llvm::cantFail(Space->writeInteger(Out + PageSize - 1, 0xa5, 1));
  EXPECT_EQ(
      call(ServiceKind::Mincore, UnitHint, 3 * PageSize, Out + PageSize - 1),
      uint64_t(-14));
  EXPECT_EQ(*Space->readInteger(Out + PageSize - 1, 1), 1u);
}
TEST_F(LinuxMemory, ResidencyPartialChunkIsExplicitlyUnsupported) {
  Options.LinuxKernel.emplace().ResidencyObservations =
      std::vector<LinuxResidencyObservation>{{UnitHint, {1, 0, 1}}};
  ASSERT_EQ(map(3 * PageSize), UnitHint);
  const uint64_t Out = UnitHint + 4 * PageSize;
  ASSERT_EQ(map(PageSize, Out), Out);
  llvm::cantFail(Space->writeInteger(Out + PageSize - 1, 0xa5, 1));
  EXPECT_FALSE(
      call(ServiceKind::Mincore, UnitHint, 3 * PageSize, Out + PageSize - 1));
  EXPECT_NE(Report.Diagnostic.find("partial"), std::string::npos);
  EXPECT_EQ(*Space->readInteger(Out + PageSize - 1, 1), 0xa5u);
}
TEST_F(LinuxMemory, ResidencyPageSizedChunksKeepFullPrefixBeforeCopyFault) {
  Memory.reset();
  Space.reset();
  RAM.reset();
  Options.MemoryLimit = 8192 * PageSize;
  RAM = llvm::cantFail(PhysicalMemory::create(Options.MemoryLimit));
  Space = llvm::cantFail(AddressSpace::create(RAM, Options.MemoryLimit));
  std::vector<uint8_t> Bits(4097);
  for (size_t I = 0; I < Bits.size(); ++I)
    Bits[I] = I % 2;
  Options.LinuxKernel.emplace().ResidencyObservations =
      std::vector<LinuxResidencyObservation>{{UnitHint, Bits}};
  Memory = std::make_unique<linux_model::LinuxMemory>(
      *Space, linux_model::MemoryLayout{UserLimit, PageSize}, UnitHeap,
      Options);
  ASSERT_EQ(map(Bits.size() * PageSize), UnitHint);
  const uint64_t Out = UnitHint + 8191 * PageSize;
  ASSERT_EQ(map(PageSize, Out), Out);
  EXPECT_EQ(call(ServiceKind::Mincore, UnitHint, Bits.size() * PageSize, Out),
            uint64_t(-14));
  Bits.resize(4096);
  EXPECT_EQ(readBytes(Out, 4096), Bits);
}

TEST_F(LinuxMemory, ResidencyHeapGrowthSharesObservationOwnership) {
  Options.LinuxKernel.emplace().ResidencyObservations =
      std::vector<LinuxResidencyObservation>{{UnitHeap, {0, 1}}};
  ASSERT_EQ(call(ServiceKind::Brk, UnitHeap + 2 * PageSize),
            UnitHeap + 2 * PageSize);
  EXPECT_EQ(call(ServiceKind::Mincore, UnitHeap, 2 * PageSize, UnitHeap + 9),
            0u);
  EXPECT_EQ(*Space->readInteger(UnitHeap + 9, 2), 0x100u);
}

TEST_F(LinuxMemory, ResidencyNamesCannotGrantOwnershipAndUnmapRetiresIt) {
  auto &Kernel = Options.LinuxKernel.emplace();
  Kernel.AnonymousVMANames = LinuxAnonymousVMANames::AndroidUserPointer;
  Kernel.ResidencyObservations = std::vector<LinuxResidencyObservation>{
      {UnitHint, {1}}, {UnitHint, {0}}, {UnitHint, {1}}};
  ASSERT_EQ(llvm::toString(
                Space->map(UnitHint, PageSize, Read | Write | UserAccessible)),
            "");
  llvm::cantFail(Space->writeInteger(UnitHint + 32, 0, 1));
  ASSERT_EQ(call(ServiceKind::Prctl, 0x53564d41, 0, UnitHint, PageSize,
                 UnitHint + 32),
            0u);
  EXPECT_FALSE(call(ServiceKind::Mincore, UnitHint, PageSize, UnitHint));
  EXPECT_NE(Report.Diagnostic.find("backing"), std::string::npos);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint, PageSize), 0u);
  ASSERT_EQ(map(PageSize), UnitHint);
  ASSERT_EQ(call(ServiceKind::Mincore, UnitHint, PageSize, UnitHint), 0u);
  EXPECT_EQ(*Space->readInteger(UnitHint, 1), 1u);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint, PageSize), 0u);
  ASSERT_EQ(llvm::toString(
                Space->map(UnitHint, PageSize, Read | Write | UserAccessible)),
            "");
  EXPECT_FALSE(call(ServiceKind::Mincore, UnitHint, PageSize, UnitHint));
  EXPECT_NE(Report.Diagnostic.find("backing"), std::string::npos);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint, PageSize), 0u);
  ASSERT_EQ(map(PageSize), UnitHint);
  EXPECT_EQ(call(ServiceKind::Mincore, UnitHint, PageSize, UnitHint), 0u);
  EXPECT_EQ(*Space->readInteger(UnitHint, 1), 0u);
}
TEST_F(LinuxMemory, ResidencyUnknownAdjacentBackingKeepsTheKnownPrefix) {
  Options.LinuxKernel.emplace().ResidencyObservations =
      std::vector<LinuxResidencyObservation>{{UnitHint, {1, 0}}};
  ASSERT_EQ(map(PageSize), UnitHint);
  ASSERT_EQ(llvm::toString(Space->map(UnitHint + PageSize, PageSize,
                                      Read | Write | UserAccessible)),
            "");
  llvm::cantFail(Space->writeInteger(UnitHint + 32, 0xa5a5, 2));
  EXPECT_FALSE(
      call(ServiceKind::Mincore, UnitHint, 2 * PageSize, UnitHint + 32));
  EXPECT_NE(Report.Diagnostic.find("backing"), std::string::npos);
  EXPECT_EQ(*Space->readInteger(UnitHint + 32, 2), 0xa501u);
  EXPECT_FALSE(
      call(ServiceKind::Mincore, UnitHint, 2 * PageSize, UnitHint + 32));
  EXPECT_NE(Report.Diagnostic.find("exhausted"), std::string::npos);
}

TEST_F(LinuxMemory, ResidencyMMIOBoundariesDoNotReadOrWriteDevices) {
  const uint64_t DeviceAddress = UnitHint + 4 * PageSize;
  unsigned Transactions = 0;
  GuestMMIOCallbacks Device;
  Device.Validate = [&](uint64_t, uint64_t, bool) {
    ++Transactions;
    return llvm::Error::success();
  };
  Device.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    ++Transactions;
    return 0;
  };
  Device.Write = [&](uint64_t, unsigned, uint64_t) {
    ++Transactions;
    return llvm::Error::success();
  };
  ASSERT_EQ(llvm::toString(
                Space->mapMMIO(DeviceAddress, PageSize, std::move(Device))),
            "");
  Options.LinuxKernel.emplace().ResidencyObservations =
      std::vector<LinuxResidencyObservation>{
          {UnitHint, {1}}, {UnitHint, {0}}, {DeviceAddress, {1}}};
  ASSERT_EQ(map(PageSize), UnitHint);
  // An unrelated device does not prevent an ordinary RAM query or affect proc's
  // separate refusal to render unmodeled device mappings.
  EXPECT_EQ(call(ServiceKind::Mincore, UnitHint, PageSize, UnitHint), 0u);
  EXPECT_EQ(*Space->readInteger(UnitHint, 1), 1u);
  auto Proc = Memory->procMappings();
  ASSERT_FALSE(bool(Proc));
  llvm::consumeError(Proc.takeError());
  EXPECT_FALSE(call(ServiceKind::Mincore, UnitHint, PageSize, DeviceAddress));
  EXPECT_NE(Report.Diagnostic.find("device memory"), std::string::npos);
  EXPECT_FALSE(call(ServiceKind::Mincore, DeviceAddress, PageSize, UnitHint));
  EXPECT_NE(Report.Diagnostic.find("backing"), std::string::npos);
  EXPECT_EQ(*Space->readInteger(UnitHint, 1), 1u);
  EXPECT_EQ(Transactions, 0u);
}

TEST_F(LinuxMemory, RoundedPagesAreZeroAndPartialUnmapReclaimsTheirOwners) {
  ASSERT_EQ(map(RegionPages * PageSize - 1), UnitHint);
  EXPECT_EQ(RAM->allocatedBytes(), RegionPages * PageSize);
  EXPECT_EQ(*Space->readInteger(UnitHint, sizeof(uint64_t)), 0u);
  ASSERT_EQ(llvm::toString(Space->writeInteger(UnitHint + PageSize, FirstByte,
                                               sizeof(uint64_t))),
            "");
  const auto Snapshot = llvm::cantFail(Space->mappings());
  ASSERT_EQ(Snapshot.size(), 1u);
  EXPECT_EQ(Snapshot.front().Size, RegionPages * PageSize);
  EXPECT_EQ(call(ServiceKind::Munmap, UnitHint + PageSize, PageSize), 0u);
  EXPECT_EQ(RAM->allocatedBytes(), (RegionPages - 1) * PageSize);
  EXPECT_EQ(llvm::cantFail(Space->mappings()).size(), 2u);
  ASSERT_EQ(map(PageSize, UnitHint + PageSize), UnitHint + PageSize);
  EXPECT_EQ(*Space->readInteger(UnitHint + PageSize, sizeof(uint64_t)), 0u);
  EXPECT_EQ(call(ServiceKind::Munmap, UnitHint, RegionPages * PageSize), 0u);
  EXPECT_EQ(call(ServiceKind::Munmap, UnitHint, RegionPages * PageSize), 0u);
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
  EXPECT_TRUE(llvm::cantFail(Space->mappings()).empty());
  // A metadata snapshot cannot pin RAM or resurrect the retired mappings.
  EXPECT_EQ(Snapshot.front().Size, RegionPages * PageSize);
}

TEST_F(LinuxMemory, PermissionChangeKeepsTheCommittedPrefixBeforeAHole) {
  ASSERT_EQ(map(RegionPages * PageSize), UnitHint);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint + PageSize, PageSize), 0u);
  EXPECT_EQ(
      call(ServiceKind::Mprotect, UnitHint, RegionPages * PageSize, ProtRead),
      uint64_t(0) - NoMemory);
  EXPECT_FALSE(*Space->canAccess(UnitHint, PageSize, Write));
  EXPECT_TRUE(*Space->canAccess(UnitHint, PageSize, Read | UserAccessible));
  EXPECT_TRUE(*Space->canAccess(UnitHint + 2 * PageSize, PageSize, Write));
  EXPECT_EQ(call(ServiceKind::Mprotect, UnitHint, 1, 0), 0u);
  EXPECT_TRUE(*Space->canAccess(UnitHint, PageSize, UserAccessible));
  EXPECT_FALSE(*Space->canAccess(UnitHint, 1, Read));
  EXPECT_EQ(
      call(ServiceKind::Mprotect, UnitHint, PageSize, ProtRead | ProtWrite),
      0u);
  const auto Generation = Space->mappingGeneration();
  EXPECT_EQ(call(ServiceKind::Mprotect, UnitHint + 1, PageSize, ProtRead),
            uint64_t(0) - InvalidArgument);
  EXPECT_EQ(Space->mappingGeneration(), Generation);
}

TEST_F(LinuxMemory,
       AllocationFailurePublishesNothingAndFreedCapacityIsReusable) {
  ASSERT_EQ(map(UnitLimit), UnitHint);
  const auto Generation = Space->mappingGeneration();
  EXPECT_EQ(map(PageSize), uint64_t(0) - NoMemory);
  EXPECT_EQ(Space->mappingGeneration(), Generation);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint + PageSize, PageSize), 0u);
  const auto After = Space->mappingGeneration();
  EXPECT_EQ(map(2 * PageSize), uint64_t(0) - NoMemory);
  EXPECT_EQ(Space->mappingGeneration(), After);
  EXPECT_EQ(RAM->allocatedBytes(), UnitLimit - PageSize);
  EXPECT_EQ(map(PageSize, UnitHint + PageSize), UnitHint + PageSize);
  EXPECT_EQ(RAM->allocatedBytes(), UnitLimit);
}

TEST_F(LinuxMemory, PlacementQueriesActualMappingsAndReservesStackGuards) {
  ASSERT_EQ(llvm::toString(Space->map(UnitHint, PageSize, Read | Write)), "");
  ASSERT_EQ(llvm::toString(
                Space->writeInteger(UnitHint, FirstByte, sizeof(uint64_t))),
            "");
  EXPECT_NE(map(PageSize), UnitHint);
  EXPECT_EQ(*Space->readInteger(UnitHint, sizeof(uint64_t)), FirstByte);
  ASSERT_EQ(llvm::toString(Space->unmap(UnitHint, PageSize)), "");
  EXPECT_EQ(map(PageSize), UnitHint);
  const uint64_t Guard = linux_model::StackTop - 2 * PageSize;
  EXPECT_NE(map(PageSize, Guard), Guard);
  EXPECT_FALSE(*Space->canAccess(Guard, PageSize, 0));
}

TEST_F(LinuxMemory, RawBreakRetainsItsOldValueOnCollisionAndAllocationFailure) {
  EXPECT_EQ(call(ServiceKind::Brk), UnitHeap);
  EXPECT_EQ(call(ServiceKind::Brk, UnitHeap + HeapOffset),
            UnitHeap + HeapOffset);
  ASSERT_EQ(llvm::toString(
                Space->writeInteger(UnitHeap, FirstByte, sizeof(uint64_t))),
            "");
  EXPECT_EQ(map(PageSize, UnitHeap + 3 * PageSize), UnitHeap + 3 * PageSize);
  const uint64_t End = UnitHeap + 2 * PageSize;
  EXPECT_EQ(call(ServiceKind::Brk, End), End);
  EXPECT_EQ(call(ServiceKind::Brk, End + 1), End);
  EXPECT_EQ(call(ServiceKind::Brk, UnitHeap - 1), End);
  EXPECT_EQ(call(ServiceKind::Brk, UINT64_MAX), End);
  EXPECT_EQ(call(ServiceKind::Brk, UnitHeap + 2 * UnitLimit), End);
  EXPECT_EQ(call(ServiceKind::Brk, UnitHeap + HeapOffset),
            UnitHeap + HeapOffset);
  EXPECT_EQ(*Space->readInteger(UnitHeap, sizeof(uint64_t)), FirstByte);
  EXPECT_EQ(call(ServiceKind::Brk, End), End);
  EXPECT_EQ(*Space->readInteger(UnitHeap + PageSize, sizeof(uint64_t)), 0u);
}

TEST_F(LinuxMemory, BreakShrinkCannotReportRemovingAnAlreadyUnmappedTail) {
  const uint64_t End = UnitHeap + 3 * PageSize;
  ASSERT_EQ(call(ServiceKind::Brk, End), End);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHeap + PageSize, 2 * PageSize), 0u);
  EXPECT_EQ(call(ServiceKind::Brk, UnitHeap + HeapOffset), End);
  EXPECT_EQ(call(ServiceKind::Brk), End);
}

TEST_F(LinuxMemory, UnsupportedMappingContractsHaveNoReturnOrMemoryEffects) {
  const auto Generation = Space->mappingGeneration();
  EXPECT_FALSE(call(ServiceKind::Mmap, UnitHint, PageSize, ProtRead | ProtWrite,
                    PrivateAnonymous | MapFixed));
  EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_FALSE(Report.Diagnostic.empty());
  EXPECT_FALSE(
      call(ServiceKind::Mmap, UnitHint, PageSize, ProtWrite, PrivateAnonymous));
  EXPECT_FALSE(call(ServiceKind::Mprotect, UnitHint, PageSize, ProtExecute));
  EXPECT_EQ(Space->mappingGeneration(), Generation);
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
}

TEST_F(LinuxMemory, EmptyAndOverflowingRangesFollowTheirOwnSystemCallRules) {
  EXPECT_EQ(map(0), uint64_t(0) - InvalidArgument);
  EXPECT_EQ(map(UINT64_MAX), uint64_t(0) - NoMemory);
  EXPECT_EQ(call(ServiceKind::Mmap, 0, PageSize, ProtRead, PrivateAnonymous,
                 UINT64_MAX, 1),
            uint64_t(0) - InvalidArgument);
  EXPECT_EQ(call(ServiceKind::Mprotect, 0, 0, ProtRead), 0u);
  EXPECT_EQ(call(ServiceKind::Mprotect, 1, 0, ProtRead),
            uint64_t(0) - InvalidArgument);
  EXPECT_EQ(call(ServiceKind::Mprotect, UnitHint, UINT64_MAX, ProtRead),
            uint64_t(0) - NoMemory);
  EXPECT_EQ(call(ServiceKind::Munmap, 0, 0), uint64_t(0) - InvalidArgument);
  EXPECT_EQ(call(ServiceKind::Munmap, UserLimit, PageSize),
            uint64_t(0) - InvalidArgument);
  EXPECT_EQ(call(ServiceKind::Munmap, UnitHint, UINT64_MAX),
            uint64_t(0) - InvalidArgument);
  EXPECT_EQ(call(ServiceKind::Munmap, UnitHint, PageSize), 0u);
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
}

TEST_F(LinuxMemory, MergeAdviceSplitsAndRejoinsWithoutChangingMemory) {
  ASSERT_EQ(map(3 * PageSize), UnitHint);
  ASSERT_EQ(llvm::toString(Space->writeInteger(UnitHint, FirstByte, 8)), "");
  const auto Generation = Space->mappingGeneration();
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint, 3 * PageSize - 1, 12), 0u);
  EXPECT_TRUE(Memory->isMergeable(UnitHint + 3 * PageSize - 1));
  EXPECT_FALSE(Memory->isMergeable(UnitHint - 1));
  EXPECT_FALSE(Memory->isMergeable(UnitHint + 3 * PageSize));
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint + PageSize, 1, 13), 0u);
  EXPECT_TRUE(Memory->isMergeable(UnitHint));
  EXPECT_FALSE(Memory->isMergeable(UnitHint + PageSize));
  EXPECT_TRUE(Memory->isMergeable(UnitHint + 2 * PageSize));
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint + PageSize, PageSize, 12), 0u);
  EXPECT_TRUE(Memory->isMergeable(UnitHint + PageSize));
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint, 3 * PageSize, 12), 0u);
  EXPECT_EQ(Space->mappingGeneration(), Generation);
  EXPECT_EQ(RAM->allocatedBytes(), 3 * PageSize);
  EXPECT_EQ(*Space->readInteger(UnitHint, 8), FirstByte);
  EXPECT_TRUE(*Space->canAccess(UnitHint, 3 * PageSize, Read | Write));
}

TEST_F(LinuxMemory, MergeAdviceVisitsBothSidesOfHolesAndIgnoresProtection) {
  ASSERT_EQ(map(3 * PageSize), UnitHint);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint + PageSize, PageSize), 0u);
  ASSERT_EQ(call(ServiceKind::Mprotect, UnitHint, PageSize, 0), 0u);
  ASSERT_EQ(
      call(ServiceKind::Mprotect, UnitHint + 2 * PageSize, PageSize, ProtRead),
      0u);
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint - PageSize, 5 * PageSize, 12),
            uint64_t(0) - NoMemory);
  EXPECT_TRUE(Memory->isMergeable(UnitHint));
  EXPECT_FALSE(Memory->isMergeable(UnitHint + PageSize));
  EXPECT_TRUE(Memory->isMergeable(UnitHint + 2 * PageSize));
  EXPECT_FALSE(*Space->canAccess(UnitHint, 1, Read));
  EXPECT_FALSE(*Space->canAccess(UnitHint + 2 * PageSize, 1, Write));
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint, 3 * PageSize, 13),
            uint64_t(0) - NoMemory);
  EXPECT_FALSE(Memory->isMergeable(UnitHint));
  EXPECT_FALSE(Memory->isMergeable(UnitHint + 2 * PageSize));
}

TEST_F(LinuxMemory, UnmapAndBreakShrinkRetireAdviceWithoutPinningPages) {
  ASSERT_EQ(map(3 * PageSize), UnitHint);
  ASSERT_EQ(call(ServiceKind::Madvise, UnitHint, 3 * PageSize, 12), 0u);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint + PageSize, PageSize), 0u);
  EXPECT_FALSE(Memory->isMergeable(UnitHint + PageSize));
  EXPECT_EQ(RAM->allocatedBytes(), 2 * PageSize);
  ASSERT_EQ(map(PageSize, UnitHint + PageSize), UnitHint + PageSize);
  EXPECT_FALSE(Memory->isMergeable(UnitHint + PageSize));
  EXPECT_TRUE(Memory->isMergeable(UnitHint));
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint, 3 * PageSize), 0u);
  EXPECT_FALSE(Memory->isMergeable(UnitHint));
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
  ASSERT_EQ(call(ServiceKind::Brk, UnitHeap + 2 * PageSize),
            UnitHeap + 2 * PageSize);
  ASSERT_EQ(call(ServiceKind::Madvise, UnitHeap, 2 * PageSize, 12), 0u);
  ASSERT_EQ(call(ServiceKind::Brk, UnitHeap + 1), UnitHeap + 1);
  EXPECT_TRUE(Memory->isMergeable(UnitHeap));
  EXPECT_FALSE(Memory->isMergeable(UnitHeap + PageSize));
  ASSERT_EQ(call(ServiceKind::Brk, UnitHeap + 2 * PageSize),
            UnitHeap + 2 * PageSize);
  EXPECT_FALSE(Memory->isMergeable(UnitHeap + PageSize));
}

TEST_F(LinuxMemory, MergeAdviceValidatesRangesAndConsumesOnlyTheLowInt) {
  ASSERT_EQ(map(PageSize), UnitHint);
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint, 1, (1ULL << 32) | 12), 0u);
  EXPECT_TRUE(Memory->isMergeable(UnitHint));
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint + 1, 0, 13),
            uint64_t(0) - InvalidArgument);
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint, UINT64_MAX, 13),
            uint64_t(0) - InvalidArgument);
  EXPECT_EQ(call(ServiceKind::Madvise, uint64_t(0) - PageSize, PageSize, 13),
            uint64_t(0) - InvalidArgument);
  EXPECT_EQ(call(ServiceKind::Madvise, uint64_t(0) - PageSize, 0, 13), 0u);
  EXPECT_EQ(call(ServiceKind::Madvise, UserLimit, PageSize, 13),
            uint64_t(0) - NoMemory);
  EXPECT_TRUE(Memory->isMergeable(UnitHint));
}

TEST_F(LinuxMemory, UnsupportedAdviceCannotInventReturnsOrChangePolicy) {
  ASSERT_EQ(map(PageSize), UnitHint);
  ASSERT_EQ(call(ServiceKind::Madvise, UnitHint, PageSize, 12), 0u);
  ASSERT_EQ(llvm::toString(Space->writeInteger(UnitHint, FirstByte, 8)), "");
  const auto Generation = Space->mappingGeneration();
  for (uint64_t Advice : {0u, 8u, 0xffffffffu}) {
    EXPECT_FALSE(call(ServiceKind::Madvise, UnitHint, PageSize, Advice));
    EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(Report.Diagnostic.find("madvise"), std::string::npos);
    EXPECT_TRUE(Memory->isMergeable(UnitHint));
    EXPECT_EQ(*Space->readInteger(UnitHint, 8), FirstByte);
  }
  EXPECT_EQ(Space->mappingGeneration(), Generation);
}
TEST_F(LinuxMemory, DiscardRoundsPagesAndPreservesNamesPermissionsAndPolicy) {
  Options.LinuxKernel.emplace().AnonymousVMANames =
      LinuxAnonymousVMANames::AndroidUserPointer;
  ASSERT_EQ(map(3 * PageSize), UnitHint);
  std::vector<uint8_t> Bytes(3 * PageSize, 0xa5);
  llvm::cantFail(Space->write(UnitHint, Bytes));
  ASSERT_EQ(call(ServiceKind::Prctl, 0x53564d41, 0, UnitHint, 3 * PageSize, 73),
            0u);
  ASSERT_EQ(call(ServiceKind::Madvise, UnitHint, 3 * PageSize, 12), 0u);
  ASSERT_EQ(call(ServiceKind::Mprotect, UnitHint, 3 * PageSize, ProtRead), 0u);
  const auto Allocated = RAM->allocatedBytes();
  const auto Mapped = Space->mappedBytes();
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint + PageSize, 1,
                 (uint64_t(1) << 32) | 4),
            0u);
  std::fill_n(Bytes.begin() + PageSize, PageSize, 0);
  EXPECT_EQ(readBytes(UnitHint, Bytes.size()), Bytes);
  EXPECT_EQ(RAM->allocatedBytes(), Allocated);
  EXPECT_EQ(Space->mappedBytes(), Mapped);
  auto Maps = llvm::cantFail(Memory->procMappings());
  ASSERT_EQ(Maps.size(), 1u);
  EXPECT_EQ(Maps[0].AnonymousName, 73u);
  EXPECT_EQ(Maps[0].Permissions, Read | UserAccessible);
  EXPECT_TRUE(Memory->isMergeable(UnitHint + PageSize));
  ASSERT_EQ(call(ServiceKind::Mprotect, UnitHint, 3 * PageSize, 0), 0u);
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint, 3 * PageSize, 4), 0u);
  EXPECT_FALSE(llvm::cantFail(Space->canAccess(UnitHint, PageSize, Read)));
  ASSERT_EQ(call(ServiceKind::Mprotect, UnitHint, 3 * PageSize, ProtRead), 0u);
  EXPECT_EQ(readBytes(UnitHint, Bytes.size()),
            std::vector<uint8_t>(Bytes.size()));
}
TEST_F(LinuxMemory, DiscardRetiresOnlyItsMappingAndPreservesPinnedAliases) {
  ASSERT_EQ(map(PageSize), UnitHint);
  std::vector<uint8_t> Bytes(PageSize, 0x73);
  llvm::cantFail(Space->write(UnitHint, Bytes));
  auto Pinned = llvm::cantFail(Space->pinBacking(UnitHint, PageSize));
  auto Witness = llvm::cantFail(Pinned.watchWrites());
  const auto Alias = UnitHint + 8 * PageSize;
  llvm::cantFail(Space->mapAlias(Alias, UnitHint, PageSize,
                                 Read | Write | UserAccessible));
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint, PageSize, 4), 0u);
  EXPECT_EQ(readBytes(UnitHint, PageSize), std::vector<uint8_t>(PageSize));
  EXPECT_EQ(readBytes(Alias, PageSize), Bytes);
  std::vector<uint8_t> Retained(PageSize);
  llvm::cantFail(Pinned.read(0, Retained));
  EXPECT_EQ(Retained, Bytes);
  EXPECT_TRUE(llvm::cantFail(Witness.intact()));
  auto Fresh = llvm::cantFail(Space->pinBacking(UnitHint, PageSize));
  EXPECT_FALSE(Pinned.overlaps(Fresh));
  EXPECT_EQ(RAM->allocatedBytes(), 2 * PageSize);
  llvm::cantFail(Space->writeInteger(UnitHint, 19, 1));
  EXPECT_EQ(readBytes(Alias, PageSize), Bytes);
}
TEST_F(LinuxMemory, DiscardHolesKeepBothCompletedSidesAndRetireOldData) {
  ASSERT_EQ(map(3 * PageSize), UnitHint);
  llvm::cantFail(
      Space->write(UnitHint, std::vector<uint8_t>(3 * PageSize, 0xa5)));
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint + PageSize, PageSize), 0u);
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint, 3 * PageSize, 4),
            uint64_t(-12));
  EXPECT_EQ(readBytes(UnitHint, PageSize), std::vector<uint8_t>(PageSize));
  EXPECT_EQ(readBytes(UnitHint + 2 * PageSize, PageSize),
            std::vector<uint8_t>(PageSize));
  EXPECT_FALSE(
      llvm::cantFail(Space->canAccess(UnitHint + PageSize, PageSize, 0)));
  EXPECT_EQ(RAM->allocatedBytes(), 2 * PageSize);
}
TEST_F(LinuxMemory, DiscardChecksAlignmentOverflowAndZeroBeforeBacking) {
  ASSERT_EQ(map(PageSize), UnitHint);
  llvm::cantFail(Space->write(UnitHint, std::vector<uint8_t>(PageSize, 0xa5)));
  const auto Generation = Space->mappingGeneration();
  for (auto Pair :
       {std::pair{UnitHint + 1, uint64_t(0)}, std::pair{UnitHint, UINT64_MAX},
        std::pair{UINT64_MAX - PageSize + 1, PageSize}})
    EXPECT_EQ(call(ServiceKind::Madvise, Pair.first, Pair.second, 4),
              uint64_t(-22));
  EXPECT_EQ(call(ServiceKind::Madvise, UINT64_MAX - PageSize + 1, 0, 4), 0u);
  EXPECT_EQ(call(ServiceKind::Madvise, UserLimit, PageSize, 4), uint64_t(-12));
  EXPECT_EQ(call(ServiceKind::Madvise, 0, PageSize, 4), uint64_t(-12));
  EXPECT_EQ(Space->mappingGeneration(), Generation);
  EXPECT_EQ(readBytes(UnitHint, PageSize),
            std::vector<uint8_t>(PageSize, 0xa5));
}
TEST_F(LinuxMemory, DiscardBreakPagesPreservesBreakPosition) {
  const uint64_t End = UnitHeap + PageSize + 37;
  ASSERT_EQ(call(ServiceKind::Brk, End), End);
  llvm::cantFail(
      Space->write(UnitHeap, std::vector<uint8_t>(2 * PageSize, 0xa5)));
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHeap, 2 * PageSize, 4), 0u);
  EXPECT_EQ(call(ServiceKind::Brk), End);
  EXPECT_EQ(readBytes(UnitHeap, 2 * PageSize),
            std::vector<uint8_t>(2 * PageSize));
}
TEST_F(LinuxMemory, DiscardUnknownBackingDoesNotInventSuccessOrEraseIt) {
  ASSERT_EQ(map(PageSize), UnitHint);
  llvm::cantFail(Space->map(UnitHint + PageSize, PageSize, Read | Write));
  llvm::cantFail(
      Space->write(UnitHint, std::vector<uint8_t>(2 * PageSize, 0xa5)));
  EXPECT_FALSE(call(ServiceKind::Madvise, UnitHint, 2 * PageSize, 4));
  EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Report.Diagnostic.find("owned private anonymous"),
            std::string::npos);
  EXPECT_EQ(readBytes(UnitHint, PageSize), std::vector<uint8_t>(PageSize));
  EXPECT_EQ(readBytes(UnitHint + PageSize, PageSize),
            std::vector<uint8_t>(PageSize, 0xa5));
}
TEST_F(LinuxMemory, DiscardCannotUseAnAnonymousNameAsAllocationProvenance) {
  Options.LinuxKernel.emplace().AnonymousVMANames =
      LinuxAnonymousVMANames::AndroidUserPointer;
  llvm::cantFail(Space->map(UnitHint, PageSize, Read | Write | UserAccessible));
  std::vector<uint8_t> Bytes(PageSize, 0xa5);
  llvm::cantFail(Space->write(UnitHint, Bytes));
  ASSERT_EQ(call(ServiceKind::Prctl, 0x53564d41, 0, UnitHint, PageSize, 73),
            0u);
  const auto Generation = Space->mappingGeneration();
  EXPECT_FALSE(call(ServiceKind::Madvise, UnitHint, PageSize, 4));
  EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Report.Diagnostic.find("owned private anonymous"),
            std::string::npos);
  EXPECT_EQ(Space->mappingGeneration(), Generation);
  EXPECT_EQ(readBytes(UnitHint, PageSize), Bytes);
}
TEST_F(LinuxMemory, DiscardRetiresProvenanceAtSharedUnmap) {
  ASSERT_EQ(map(PageSize), UnitHint);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint, PageSize), 0u);
  llvm::cantFail(Space->map(UnitHint, PageSize, Read | Write | UserAccessible));
  std::vector<uint8_t> Bytes(PageSize, 0xa5);
  llvm::cantFail(Space->write(UnitHint, Bytes));
  EXPECT_FALSE(call(ServiceKind::Madvise, UnitHint, PageSize, 4));
  EXPECT_EQ(readBytes(UnitHint, PageSize), Bytes);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint, PageSize), 0u);
  ASSERT_EQ(map(PageSize), UnitHint);
  llvm::cantFail(Space->write(UnitHint, Bytes));
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint, PageSize, 4), 0u);
  EXPECT_EQ(readBytes(UnitHint, PageSize), std::vector<uint8_t>(PageSize));
}
TEST_F(LinuxMemory, DiscardBackingBudgetIsARefusalBeforeRemovingThePage) {
  auto SmallRAM = llvm::cantFail(PhysicalMemory::create(PageSize));
  auto SmallSpace = llvm::cantFail(AddressSpace::create(SmallRAM, PageSize));
  ProcessOptions SmallOptions;
  SmallOptions.MemoryLimit = PageSize;
  SmallOptions.StackSize = PageSize;
  linux_model::LinuxMemory Small(*SmallSpace, {UserLimit, PageSize}, UnitHeap,
                                 SmallOptions);
  auto Mapped =
      llvm::cantFail(Small.handle(ServiceKind::Mmap,
                                  {0,
                                   0,
                                   {UnitHint, PageSize, ProtRead | ProtWrite,
                                    PrivateAnonymous, UINT64_MAX, 0},
                                   {}},
                                  Report));
  ASSERT_EQ(Mapped, UnitHint);
  std::vector<uint8_t> Bytes(PageSize, 0xa5), Actual(PageSize);
  llvm::cantFail(SmallSpace->write(UnitHint, Bytes));
  const auto Generation = SmallSpace->mappingGeneration();
  EXPECT_FALSE(llvm::cantFail(
      Small.handle(ServiceKind::Madvise,
                   {0, 0, {UnitHint, PageSize, 4, 0, 0, 0}, {}}, Report)));
  EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Report.Diagnostic.find("model memory limit"), std::string::npos);
  llvm::cantFail(SmallSpace->read(UnitHint, Actual));
  EXPECT_EQ(Actual, Bytes);
  EXPECT_EQ(SmallSpace->mappingGeneration(), Generation);
  EXPECT_EQ(SmallRAM->allocatedBytes(), PageSize);
}
TEST_F(LinuxMemory, DeviceSnapshotsSharePagesAndRetireOnlyTheLastAlias) {
  LinuxDeviceOptions Device;
  auto &Input = Device.ReadOnlyMappings.emplace_back();
  Input.Offset = PageSize;
  Input.Bytes.resize(3 * PageSize);
  for (size_t I = 0; I < Input.Bytes.size(); ++I)
    Input.Bytes[I] = 0x31 + I / PageSize;
  auto DeviceMap = [&](uint64_t Hint, uint64_t Size, uint64_t Offset) {
    return llvm::cantFail(Memory->mapDevice(
        {0, 0, {Hint, Size, 1, 1, 3, Offset}, {}}, &Device, Report));
  };
  ASSERT_EQ(DeviceMap(UnitHint, 3 * PageSize - 1, PageSize), UnitHint);
  const uint64_t Alias = UnitHint + 4 * PageSize;
  ASSERT_EQ(DeviceMap(Alias, 2 * PageSize, 2 * PageSize), Alias);
  EXPECT_EQ(Space->mappedBytes(), 5 * PageSize);
  EXPECT_EQ(RAM->allocatedBytes(), 3 * PageSize);
  EXPECT_EQ(*Space->readInteger(UnitHint, 1), 0x31u);
  EXPECT_EQ(*Space->readInteger(Alias, 1), 0x32u);
  EXPECT_EQ(*Space->readInteger(Alias + 2 * PageSize - 1, 1), 0x33u);
  EXPECT_FALSE(*Space->canAccess(Alias, 1, Write));
  // A stopped observer writes backing solely to distinguish real aliasing
  // from copied bytes. This is not a modeled guest write or device producer.
  llvm::cantFail(Space->writeBacking(UnitHint + PageSize, {0x78}));
  EXPECT_EQ(*Space->readInteger(Alias, 1), 0x78u);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint + PageSize, PageSize), 0u);
  EXPECT_EQ(RAM->allocatedBytes(), 3 * PageSize);
  ASSERT_EQ(call(ServiceKind::Munmap, Alias, 2 * PageSize), 0u);
  EXPECT_EQ(RAM->allocatedBytes(), 2 * PageSize);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint, 3 * PageSize), 0u);
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
  ASSERT_EQ(DeviceMap(UnitHint, 1, 2 * PageSize), UnitHint);
  EXPECT_EQ(*Space->readInteger(UnitHint, 1), 0x32u);
}

TEST_F(LinuxMemory, DeviceProtectionPolicySplitsCoalescedRangesAndRetires) {
  LinuxDeviceOptions Device;
  Device.ReadOnlyMappings = {{0, std::vector<uint8_t>(2 * PageSize, 0x42)}};
  ASSERT_EQ(map(PageSize), UnitHint);
  ASSERT_EQ(call(ServiceKind::Mprotect, UnitHint, PageSize, 1), 0u);
  ASSERT_EQ(llvm::cantFail(Memory->mapDevice(
                {0, 0, {UnitHint + PageSize, 2 * PageSize, 1, 1, 3, 0}, {}},
                &Device, Report)),
            UnitHint + PageSize);
  ASSERT_EQ(llvm::cantFail(Space->mappings()).size(), 1u);
  EXPECT_EQ(call(ServiceKind::Mprotect, UnitHint, 3 * PageSize, 3),
            uint64_t(-13));
  EXPECT_TRUE(*Space->canAccess(UnitHint, 1, Write));
  EXPECT_FALSE(*Space->canAccess(UnitHint + PageSize, 1, Write));
  EXPECT_EQ(call(ServiceKind::Mprotect, UnitHint + PageSize, PageSize, 5),
            uint64_t(-13));
  EXPECT_EQ(call(ServiceKind::Mprotect, UnitHint + PageSize, 2 * PageSize, 0),
            0u);
  EXPECT_FALSE(*Space->canAccess(UnitHint + PageSize, 1, Read));
  EXPECT_EQ(call(ServiceKind::Mprotect, UnitHint + PageSize, 2 * PageSize, 1),
            0u);
  EXPECT_EQ(*Space->readInteger(UnitHint + PageSize, 1), 0x42u);
  EXPECT_FALSE(call(ServiceKind::Madvise, UnitHint + PageSize, PageSize, 12));
  EXPECT_FALSE(Memory->isMergeable(UnitHint + PageSize));
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint + PageSize, PageSize), 0u);
  ASSERT_EQ(map(PageSize, UnitHint + PageSize), UnitHint + PageSize);
  EXPECT_EQ(call(ServiceKind::Mprotect, UnitHint + PageSize, PageSize, 5), 0u);
  EXPECT_EQ(call(ServiceKind::Mprotect, UnitHint + 2 * PageSize, PageSize, 3),
            uint64_t(-13));
}

TEST_F(LinuxMemory, DeviceMappingRefusalAndBudgetsPublishNoPartialPages) {
  LinuxDeviceOptions Device;
  Device.ReadOnlyMappings = {
      {PageSize, std::vector<uint8_t>(3 * PageSize, 0x51)}};
  auto DeviceMap = [&](uint64_t Size, uint64_t Offset, uint64_t Prot = 1) {
    return llvm::cantFail(Memory->mapDevice(
        {0, 0, {UnitHint, Size, Prot, 1, 3, Offset}, {}}, &Device, Report));
  };
  EXPECT_EQ(DeviceMap(0, PageSize), uint64_t(-22));
  EXPECT_EQ(DeviceMap(1, PageSize + 1), uint64_t(-22));
  EXPECT_FALSE(DeviceMap(PageSize, 0));
  EXPECT_FALSE(DeviceMap(2 * PageSize, 3 * PageSize));
  EXPECT_FALSE(DeviceMap(PageSize, UINT64_MAX - 4095));
  EXPECT_EQ(DeviceMap(PageSize, PageSize, 3), uint64_t(-13));
  EXPECT_EQ(DeviceMap(PageSize, PageSize, 5), uint64_t(-13));
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
  EXPECT_EQ(Space->mappedBytes(), 0u);
  ASSERT_EQ(map(6 * PageSize, UnitHeap), UnitHeap);
  auto Generation = Space->mappingGeneration();
  EXPECT_EQ(DeviceMap(3 * PageSize, PageSize), uint64_t(-12));
  EXPECT_EQ(Space->mappingGeneration(), Generation);
  EXPECT_EQ(RAM->allocatedBytes(), 6 * PageSize);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHeap, 6 * PageSize), 0u);
  ASSERT_EQ(DeviceMap(3 * PageSize, PageSize, 0), UnitHint);
  EXPECT_FALSE(*Space->canAccess(UnitHint, 1, Read));
  EXPECT_EQ(call(ServiceKind::Mprotect, UnitHint, 3 * PageSize, 1), 0u);
  EXPECT_EQ(*Space->readInteger(UnitHint + 2 * PageSize, 1), 0x51u);
}
} // namespace
} // namespace neverd::emulation
