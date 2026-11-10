//===- X86CompatibilityPagingTests.cpp - IA-32e narrow addresses --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep compatibility segmentation independent of IA-32e page-table width.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/translate/X86/X86PagedMemory.h"

#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using namespace neverd;
using namespace neverd::translate;

namespace {

class X86CompatibilityPagingTest : public testing::TestWithParam<unsigned> {
protected:
  X86PagingFeatures Features{.PhysicalAddressBits = 48,
                             .PAE = true,
                             .LongMode = true,
                             .LA57 = true,
                             .NX = true};
  X86PagingState Controls{.CR0 = 0x80010031,
                          .CR3 = 0x1000,
                          .CR4 = 0x20,
                          .EFER = 0xd00,
                          .RFlags = 2};
  std::unique_ptr<X86PagedMemory> Memory;
  uint64_t NextTable = 0x2000;

  void SetUp() override {
    if (GetParam() == 5)
      Controls.CR4 |= 0x1000;
    auto RAM =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    RAM.Memory.push_back({0, MemoryPermission::Read | MemoryPermission::Write,
                          0, std::vector<uint8_t>(0x40000)});
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  }

  uint64_t get(uint64_t Address) {
    auto R =
        Memory->physicalMemory().loadScalar(Address, GuestScalarWidth::I64);
    EXPECT_EQ(R.Status, GuestMemoryAccessStatus::Completed);
    return R.Value;
  }

  void put(uint64_t Address, uint64_t Value) {
    ASSERT_EQ(Memory->physicalMemory()
                  .storeScalar(Address, GuestScalarWidth::I64, Value)
                  .Status,
              GuestMemoryAccessStatus::Completed);
  }

  uint64_t map(uint64_t Linear, uint64_t Physical, uint64_t Flags = 7) {
    uint64_t Table = Controls.CR3;
    for (unsigned Level = GetParam(); Level != 0; --Level) {
      const auto Slot = Table + ((Linear >> (12 + 9 * (Level - 1))) & 511) * 8;
      if (Level == 1) {
        put(Slot, Physical | Flags);
        return Slot;
      }
      if (!(get(Slot) & 1)) {
        put(Slot, NextTable | 7);
        NextTable += 0x1000;
      }
      Table = get(Slot) & ~uint64_t(4095);
    }
    return 0;
  }

  X86SegmentedAccess access(MemoryAccessKind Kind = MemoryAccessKind::Read) {
    return {X86SegmentRegister::FS,
            {0xfffffffc, UINT32_MAX, 0x10, 0xc093, false},
            0,
            Kind,
            0,
            X86FullSegmentLimitChoice::Fault};
  }
};

TEST_P(X86CompatibilityPagingTest, SegmentLinearWrapRetainsLongModePaging) {
  const auto Last = map(0xfffff000, 0x20000);
  const auto First = map(0, 0x21000);
  map(0x100000000, 0x22000);
  put(0x20ff8, 0x4433221100000000);
  put(0x21000, 0x88776655);
  put(0x22000, 0xdeadbeef);
  auto A = access();
  std::array<uint8_t, 8> Bytes{};
  auto Read = Memory->loadSegment(A, Bytes);
  ASSERT_TRUE(bool(Read)) << llvm::toString(Read.takeError());
  ASSERT_EQ(Read->Status, X86PageWalkStatus::Completed);
  EXPECT_EQ(Bytes, (std::array<uint8_t, 8>{0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                                           0x77, 0x88}));
  EXPECT_EQ(get(Last) & 0x60, 0x20u);
  EXPECT_EQ(get(First) & 0x60, 0x20u);
  EXPECT_EQ(Memory->controls(), Controls);
  // A 64-bit linear operand at the same starting address does not wrap at
  // 4 GiB. Address-size selection alone cannot authorize compatibility wrap.
  auto Wide = llvm::cantFail(Memory->load({0xfffffffc}, Bytes));
  ASSERT_EQ(Wide.Status, X86PageWalkStatus::Completed);
  EXPECT_EQ(Bytes[4], 0xef);
  EXPECT_EQ(Bytes[7], 0xde);
  Bytes.fill(0xa5);
  A.Kind = MemoryAccessKind::Write;
  auto Write = llvm::cantFail(Memory->storeSegment(A, Bytes));
  ASSERT_EQ(Write.Status, X86PageWalkStatus::Completed);
  EXPECT_EQ(get(0x20ff8), 0xa5a5a5a500000000);
  EXPECT_EQ(get(0x21000), 0xa5a5a5a5u);
  EXPECT_EQ(get(0x22000), 0xdeadbeefu);
  EXPECT_EQ(get(Last) & 0x60, 0x60u);
  EXPECT_EQ(get(First) & 0x60, 0x60u);
}

TEST_P(X86CompatibilityPagingTest, WrappedFaultKeepsCR2AndWholeOperandCommit) {
  const auto Last = map(0xfffff000, 0x20000);
  const auto First = map(0, 0x21000, 0);
  put(0x20ff8, 0x4433221100000000);
  put(0x21000, 0x88776655);
  for (bool Write : {false, true}) {
    SCOPED_TRACE(Write);
    auto A = access(Write ? MemoryAccessKind::Write : MemoryAccessKind::Read);
    std::array<uint8_t, 8> Bytes;
    Bytes.fill(0xa5);
    auto R =
        Write ? Memory->storeSegment(A, Bytes) : Memory->loadSegment(A, Bytes);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Status, X86PageWalkStatus::Exception);
    ASSERT_TRUE(R->Exception);
    EXPECT_EQ(R->Exception->Vector, 14);
    EXPECT_EQ(R->Exception->ErrorCode, Write ? 2u : 0u);
    EXPECT_EQ(R->Exception->CR2, 0u);
    EXPECT_EQ(get(0x20ff8), 0x4433221100000000u);
    EXPECT_EQ(get(0x21000), 0x88776655u);
    EXPECT_EQ(Bytes, (std::array<uint8_t, 8>{0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
                                             0xa5, 0xa5}));
    EXPECT_EQ(get(Last) & 0x60, Write ? 0x60u : 0x20u);
    EXPECT_EQ(get(First) & 0x60, 0u);
  }
}

TEST_P(X86CompatibilityPagingTest, SegmentFaultPrecedesPageWalk) {
  auto A = access();
  A.Cache.Limit = 3;
  A.Cache.Attributes = 0x93;
  const auto Before = Memory->physicalMemory().snapshotMemoryRegions();
  for (auto Segment : {X86SegmentRegister::FS, X86SegmentRegister::SS}) {
    A.Segment = Segment;
    std::array<uint8_t, 8> Bytes{};
    auto R = Memory->loadSegment(A, Bytes);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Status, X86PageWalkStatus::Exception);
    ASSERT_TRUE(R->Exception);
    EXPECT_EQ(R->Exception->Vector,
              Segment == X86SegmentRegister::SS ? 12 : 13);
    EXPECT_FALSE(R->Exception->CR2);
  }
  EXPECT_EQ(Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
            Before.front().Bytes);
}

TEST_P(X86CompatibilityPagingTest, SystemTableAddressesRemainFullWidth) {
  constexpr uint64_t High = 0xffff800000006000;
  map(High, 0x20000);
  map(uint32_t(High), 0x21000);
  put(0x20000, 0x1122334455667788);
  put(0x21000, 0x8877665544332211);
  std::array<uint8_t, 8> Bytes{};
  X86LinearAccess A{High, MemoryAccessKind::Read, 3, true};
  auto R = llvm::cantFail(Memory->load(A, Bytes));
  ASSERT_EQ(R.Status, X86PageWalkStatus::Completed);
  EXPECT_EQ(Bytes, (std::array<uint8_t, 8>{0x88, 0x77, 0x66, 0x55, 0x44, 0x33,
                                           0x22, 0x11}));
  A.Wrap32 = true;
  auto Invalid = Memory->load(A, Bytes);
  ASSERT_FALSE(bool(Invalid));
  llvm::consumeError(Invalid.takeError());
  A.Address = uint32_t(High);
  Invalid = Memory->load(A, Bytes);
  ASSERT_FALSE(bool(Invalid));
  llvm::consumeError(Invalid.takeError());
  A.Wrap32 = false;
  A.Address = uint64_t(1) << (GetParam() == 5 ? 56 : 47);
  R = llvm::cantFail(Memory->load(A, Bytes));
  ASSERT_TRUE(R.Exception);
  EXPECT_EQ(R.Exception->Vector, 13);
}

TEST_P(X86CompatibilityPagingTest, AdmissionRejectsWideAndVM86Operands) {
  auto Before = Memory->physicalMemory().snapshotMemoryRegions();
  for (uint64_t Address : {uint64_t(1) << 32, UINT64_MAX}) {
    auto Invalid = Memory->probe(
        {Address, MemoryAccessKind::Read, 0, false, false, true}, 4);
    ASSERT_FALSE(bool(Invalid));
    llvm::consumeError(Invalid.takeError());
  }
  auto InvalidControls = Controls;
  InvalidControls.RFlags |= 0x20000;
  llvm::cantFail(Memory->bindControls(InvalidControls));
  std::array<uint8_t, 4> Bytes{};
  auto Invalid = Memory->loadSegment(access(), Bytes);
  ASSERT_FALSE(bool(Invalid));
  llvm::consumeError(Invalid.takeError());
  EXPECT_EQ(Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
            Before.front().Bytes);
}

INSTANTIATE_TEST_SUITE_P(IA32e, X86CompatibilityPagingTest,
                         testing::Values(4, 5));

} // namespace
