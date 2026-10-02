//===- WindowsProcessMemoryTests.cpp - Reservation and backing invariants -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/windows/process/WindowsProcess.h"

namespace neverd::emulation {
namespace {
namespace win = windows_process;
using namespace win::value;
#define NEVERD_WINDOWS_TEST_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "WindowsProcessTestData.def"
#undef NEVERD_WINDOWS_TEST_VALUE
class WindowsProcessMemory : public testing::Test {
protected:
  std::shared_ptr<PhysicalMemory> RAM;
  std::shared_ptr<AddressSpace> Space;
  std::unique_ptr<win::VirtualMemory> Memory;
  void initialize(uint64_t PhysicalPages, uint64_t MappingPages) {
    RAM = llvm::cantFail(PhysicalMemory::create(PhysicalPages * PageSize));
    Space = llvm::cantFail(AddressSpace::create(RAM, MappingPages * PageSize));
    const win::Image Image{
        GuestArchitecture::X64, VMUnitImage, PageSize, VMUnitImage, {}, {}};
    ProcessOptions Options;
    Options.StackSize = PageSize;
    Memory = std::make_unique<win::VirtualMemory>(*Space, Image, Options);
  }
  win::MemoryResult allocate(uint64_t Address, uint64_t Size,
                             uint32_t Type = MemReserve | MemCommit,
                             uint32_t Protection = PageReadWrite) {
    return llvm::cantFail(Memory->allocate(Address, Size, Type, Protection));
  }
  win::MemoryInformation query(uint64_t Address) {
    return *llvm::cantFail(Memory->query(Address));
  }
};
TEST_F(WindowsProcessMemory, SparseReservationsDoNotAllocateOrWalkEveryPage) {
  initialize(2, 2);
  auto R = allocate(0, VMSparseSize, MemReserve);
  ASSERT_NE(R.Value, 0u);
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
  EXPECT_EQ(Space->mappedBytes(), 0u);
  EXPECT_EQ(query(R.Value).Size, VMSparseSize);
  auto Failed = allocate(R.Value, VMSparseSize, MemCommit);
  EXPECT_EQ(Failed.Error, ErrorNotEnoughMemory);
  EXPECT_EQ(query(R.Value).State, MemReserve);
  EXPECT_EQ(llvm::cantFail(Memory->free(R.Value, 0, MemDecommit)).Value, 1u);
  EXPECT_EQ(llvm::cantFail(Memory->free(R.Value, 0, MemRelease)).Value, 1u);
}
TEST_F(WindowsProcessMemory, MappingBudgetFailureRollsBackAllPublishedPages) {
  initialize(4, 1);
  auto R = allocate(VMUnitAddress, 2 * PageSize);
  EXPECT_EQ(R.Error, ErrorNotEnoughMemory);
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
  EXPECT_EQ(Space->mappedBytes(), 0u);
  EXPECT_EQ(query(VMUnitAddress).State, MemFree);
  R = allocate(VMUnitAddress, PageSize);
  EXPECT_EQ(R.Value, VMUnitAddress);
}
TEST_F(WindowsProcessMemory, ShortagePreservesExistingDataPermissionsAndHoles) {
  initialize(3, 3);
  auto R = allocate(VMUnitAddress, 8 * PageSize, MemReserve);
  ASSERT_EQ(R.Value, VMUnitAddress);
  ASSERT_EQ(allocate(R.Value, 2 * PageSize, MemCommit).Value, R.Value);
  llvm::cantFail(Space->writeInteger(R.Value, VMUnitByte, 1));
  auto Failed = allocate(R.Value, 4 * PageSize, MemCommit, PageReadOnly);
  EXPECT_EQ(Failed.Error, ErrorNotEnoughMemory);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(R.Value, 1)), VMUnitByte);
  EXPECT_EQ(query(R.Value).Protection, PageReadWrite);
  EXPECT_EQ(query(R.Value + 2 * PageSize).State, MemReserve);
  EXPECT_EQ(RAM->allocatedBytes(), 2 * PageSize);
  ASSERT_EQ(llvm::cantFail(Memory->free(R.Value, PageSize, MemDecommit)).Value,
            1u);
  EXPECT_EQ(RAM->allocatedBytes(), PageSize);
  ASSERT_EQ(allocate(R.Value + 2 * PageSize, 2 * PageSize, MemCommit).Value,
            R.Value + 2 * PageSize);
  EXPECT_EQ(RAM->allocatedBytes(), 3 * PageSize);
  EXPECT_EQ(llvm::cantFail(Memory->free(R.Value, 0, MemRelease)).Value, 1u);
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
}
TEST_F(WindowsProcessMemory,
       QueryUsesCurrentMappingPermissionsAndAllocationIdentity) {
  initialize(2, 2);
  auto R = allocate(VMUnitAddress, PageSize);
  ASSERT_EQ(R.Value, VMUnitAddress);
  auto Other = allocate(VMUnitAddress + ImageAlignment, PageSize);
  ASSERT_EQ(Other.Value, VMUnitAddress + ImageAlignment);
  llvm::cantFail(Space->protect(R.Value, PageSize, Read | UserAccessible));
  EXPECT_EQ(query(R.Value).Protection, PageReadOnly);
  EXPECT_EQ(query(R.Value).AllocationProtection, PageReadWrite);
  EXPECT_EQ(query(Other.Value).AllocationBase, Other.Value);
  auto Failed = llvm::cantFail(
      Memory->protect(R.Value, ImageAlignment + PageSize, PageNoAccess));
  EXPECT_EQ(Failed.Error, ErrorInvalidAddress);
  EXPECT_EQ(query(R.Value).Protection, PageReadOnly);
}
TEST_F(WindowsProcessMemory,
       RuntimeReservationsAndAddressLimitsStayAuthoritative) {
  initialize(2, 2);
  for (uint64_t Address : {TEB, GateBase, HeapHandle, HeapBase, VMUnitImage,
                           StackTop - PageSize, StackTop}) {
    auto R = allocate(Address, PageSize);
    EXPECT_EQ(R.Error, ErrorInvalidAddress) << Address;
  }
  EXPECT_FALSE(llvm::cantFail(Memory->query(UserLimit)));
  EXPECT_NE(allocate(UINT64_MAX, PageSize).Error, 0u);
  EXPECT_NE(allocate(0, UINT64_MAX).Error, 0u);
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
  llvm::cantFail(Space->map(TEB, PageSize, Read | Write | UserAccessible));
  auto R = llvm::cantFail(Memory->protect(TEB, PageSize, PageNoAccess));
  EXPECT_TRUE(R.Unsupported);
  EXPECT_TRUE(llvm::cantFail(Space->canAccess(TEB, PageSize, Read | Write)));
}
} // namespace
} // namespace neverd::emulation
