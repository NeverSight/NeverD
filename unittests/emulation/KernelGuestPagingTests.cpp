//===- KernelGuestPagingTests.cpp - Guest page walks and RAM identity ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest page walks and RAM identity.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/kernel/KernelGuestPageTables.h"
#include "os/windows/kernel/KernelPhysicalMemory.h"

#include "neverd/emulation/AddressSpace.h"

namespace neverd::emulation {
namespace {
class KernelGuestPaging : public ::testing::Test {
protected:
  static constexpr uint64_t Base = 0x10000000, Alias = 0x30000000;
  static constexpr uint64_t Page = 4096, Mask = 0x000ffffffffff000ULL;
  static constexpr uint64_t Capacity = 8 * 1024 * 1024;
  std::unique_ptr<UnicornBackend> CPU;
  std::unique_ptr<KernelPhysicalMemory> Physical;
  std::unique_ptr<KernelGuestPageTables> Paging;
  void SetUp() override {
    CPU = llvm::cantFail(UnicornBackend::create(Capacity));
    llvm::cantFail(CPU->map(Base, 4 * Page, Read | Write));
    Physical = std::make_unique<KernelPhysicalMemory>(*CPU);
    Paging = std::make_unique<KernelGuestPageTables>(*CPU, *Physical);
  }
  void initialize() { llvm::cantFail(Paging->initialize(Capacity)); }
  uint64_t integer(uint64_t Address) {
    return llvm::cantFail(CPU->readInteger(Address, 8));
  }
  // An independent walk through public physical-to-virtual translation. This
  // deliberately uses architectural masks and bit positions, not builder code.
  uint64_t leaf(uint64_t Address) {
    uint64_t Frame = llvm::cantFail(Paging->root());
    for (unsigned Shift : {39u, 30u, 21u, 12u}) {
      const auto VA = llvm::cantFail(Paging->virtualAddress(Frame));
      if (!VA) {
        ADD_FAILURE() << "missing page-table frame";
        return 0;
      }
      const auto Entry = integer(VA + ((Address >> Shift) & 511) * 8);
      if (!(Entry & 1) || Shift == 12)
        return Entry;
      Frame = Entry & Mask;
    }
    return 0;
  }
  uint64_t walk(uint64_t Address) {
    const auto PTE = leaf(Address);
    return PTE & 1 ? (PTE & Mask) + Address % Page : 0;
  }
  template <typename T> std::string rejected(llvm::Expected<T> Value) {
    if (Value) {
      ADD_FAILURE() << "unexpected successful result";
      return {};
    }
    return llvm::toString(Value.takeError());
  }
};

TEST_F(KernelGuestPaging, WalksMatchExistingPhysicalOwnersAndRecursivePTEs) {
  llvm::cantFail(Physical->registerRegion(1, Base, Page));
  const auto Existing = llvm::cantFail(Physical->physicalAddress(Base + 21));
  initialize();
  EXPECT_EQ(walk(Base + 21), Existing);
  EXPECT_EQ(llvm::cantFail(Paging->physicalAddress(Base + 21)), Existing);
  const auto Root = llvm::cantFail(Paging->root());
  const auto RootVA = llvm::cantFail(Paging->virtualAddress(Root));
  unsigned Found = 0, Self = 0;
  for (unsigned I = 0; I < 512; ++I)
    if ((integer(RootVA + I * 8) & Mask) == Root) {
      ++Found;
      Self = I;
    }
  ASSERT_EQ(Found, 1u);
  const uint64_t PTEBase = 0xffff000000000000ULL | (uint64_t(Self) << 39);
  EXPECT_EQ(PTEBase, Paging->pteBase());
  const auto PTEVA = PTEBase + ((Base >> 9) & 0x7ffffffff8ULL);
  EXPECT_EQ(integer(PTEVA), leaf(Base));
  EXPECT_EQ(walk(PTEVA), llvm::cantFail(Paging->physicalAddress(PTEVA)));
  EXPECT_FALSE(llvm::cantFail(CPU->canAccess(PTEVA, 8, Write)));
  EXPECT_FALSE(llvm::cantFail(CPU->canAccess(RootVA, 8, Execute)));
  EXPECT_EQ(llvm::cantFail(Paging->root()), Root);
}

TEST_F(KernelGuestPaging, LeafRightsDescribeUserReadWriteAndNXState) {
  llvm::cantFail(CPU->protect(Base, Page, Read | Execute));
  llvm::cantFail(
      CPU->protect(Base + Page, Page, Read | Write | UserAccessible));
  llvm::cantFail(CPU->protect(Base + 2 * Page, Page, Read));
  llvm::cantFail(CPU->protect(Base + 3 * Page, Page, 0));
  initialize();
  EXPECT_EQ(leaf(Base) & 7, 1u);
  EXPECT_EQ(leaf(Base) >> 63, 0u);
  EXPECT_EQ(leaf(Base + Page) & 7, 7u);
  EXPECT_EQ(leaf(Base + Page) >> 63, 1u);
  EXPECT_EQ(leaf(Base + 2 * Page) & 7, 1u);
  EXPECT_EQ(leaf(Base + 2 * Page) >> 63, 1u);
  EXPECT_EQ(walk(Base + 3 * Page), 0u);
  EXPECT_FALSE(rejected(Paging->physicalAddress(Base + 3 * Page)).empty());
}

TEST_F(KernelGuestPaging,
       AliasesRetainFramesAndRefreshOnProtectRetireAndReuse) {
  initialize();
  const auto Original = walk(Base + 17);
  llvm::cantFail(CPU->writeInteger(Base + 17, 0x1234, 8));
  llvm::cantFail(CPU->mapAlias(Alias, Base, Page, Read | Write));
  EXPECT_EQ(walk(Alias + 17), Original);
  EXPECT_EQ(integer(Alias + 17), 0x1234u);
  llvm::cantFail(CPU->protect(Alias, Page, Read));
  EXPECT_EQ(leaf(Alias) & 2, 0u);
  EXPECT_EQ(leaf(Base) & 2, 2u);
  llvm::cantFail(CPU->unmapAlias(Alias, Page));
  EXPECT_EQ(walk(Alias), 0u);
  EXPECT_FALSE(rejected(Paging->physicalAddress(Alias)).empty());
  llvm::cantFail(CPU->mapAlias(Alias, Base + Page, Page, Read));
  EXPECT_EQ(walk(Alias + 17), walk(Base + Page + 17));
  EXPECT_NE(walk(Alias + 17), Original);
}

TEST_F(KernelGuestPaging,
       CatalogPromotionDoesNotRenumberFramesOrConsumeTheMDLWindow) {
  initialize();
  const auto Before = walk(Base + 32);
  EXPECT_EQ(llvm::cantFail(Physical->frameAddress(Base + 32)), Before);
  llvm::cantFail(Physical->registerRegion(2, Base + 32, 64));
  EXPECT_EQ(llvm::cantFail(Physical->physicalAddress(Base + 32)), Before);
  EXPECT_EQ(walk(Base + 32), Before);
  auto Plan = llvm::cantFail(Physical->planAllocatedPages(
      physical::PhysicalBase, physical::PhysicalBase + Page - 1, 0, 1));
  ASSERT_EQ(Plan.size(), 1u);
  EXPECT_EQ(Plan[0], physical::PhysicalBase);
  EXPECT_EQ(Physical->physicalRangeSize(), physical::PhysicalSize + Capacity);
}

TEST_F(KernelGuestPaging, UnknownPhysicalFramesHaveNoInventedVirtualMapping) {
  initialize();
  EXPECT_EQ(llvm::cantFail(Paging->virtualAddress(0)), 0u);
  EXPECT_EQ(llvm::cantFail(Paging->virtualAddress(UINT64_MAX)), 0u);
  EXPECT_EQ(llvm::cantFail(Paging->virtualAddress(physical::PhysicalBase)), 0u);
}

TEST_F(KernelGuestPaging,
       RejectsRecursiveWindowConflictsBeforePublishingTables) {
  initialize();
  llvm::cantFail(CPU->map(Paging->pteBase() + Page, Page, Read));
  EXPECT_NE(rejected(Paging->root()).find("recursive"), std::string::npos);
  EXPECT_EQ(integer(Paging->TableBase), 0u);
}

TEST_F(KernelGuestPaging, CapacityFailureLeavesThePreviousRootBytesUnchanged) {
  initialize();
  const auto Root = llvm::cantFail(Paging->root());
  const auto RootVA = llvm::cantFail(Paging->virtualAddress(Root));
  std::array<uint8_t, Page> Before{}, After{};
  llvm::cantFail(CPU->read(RootVA, Before));
  for (uint64_t I = 1; I <= 86; ++I)
    llvm::cantFail(CPU->map(I << 39, Page, Read));
  EXPECT_NE(rejected(Paging->root()).find("storage capacity"),
            std::string::npos);
  llvm::cantFail(CPU->read(RootVA, After));
  EXPECT_EQ(Before, After);
}

TEST_F(KernelGuestPaging, LateCacheSelectionInvalidatesTheGuestProjection) {
  const auto Plan = llvm::cantFail(Physical->planAllocatedPages(
      physical::PhysicalBase, physical::PhysicalBase + Page - 1, 0, 1));
  llvm::cantFail(Physical->registerAllocatedPages(1, Base, Plan, std::nullopt));
  initialize();
  EXPECT_EQ(leaf(Base) & 0x18, 0u);
  llvm::cantFail(Physical->commitMappingCache(
      Base, Page, KernelPhysicalMemory::CacheType::NonCached));
  EXPECT_EQ(leaf(Base) & 0x18, 0x18u);
  EXPECT_EQ(walk(Base), Plan[0]);
}

TEST_F(KernelGuestPaging,
       GuestFrameCapacityIsFiniteAndAliasesDoNotSpendItTwice) {
  llvm::cantFail(Physical->enableGuestFrames(Page));
  const auto First = llvm::cantFail(Physical->frameAddress(Base + 19));
  llvm::cantFail(CPU->mapAlias(Alias, Base, Page, Read));
  EXPECT_EQ(llvm::cantFail(Physical->frameAddress(Alias + 19)), First);
  EXPECT_NE(rejected(Physical->frameAddress(Base + Page)).find("capacity"),
            std::string::npos);
  EXPECT_EQ(llvm::cantFail(Physical->frameAddress(Base + 19)), First);
}
TEST_F(KernelGuestPaging, UnmodeledPATStateFailsBeforePublishingATable) {
  auto Plan = llvm::cantFail(Physical->planAllocatedPages(
      physical::PhysicalBase, physical::PhysicalBase + Page - 1, 0, 1));
  llvm::cantFail(Physical->registerAllocatedPages(
      1, Base, Plan, KernelPhysicalMemory::CacheType::WriteCombined));
  initialize();
  EXPECT_NE(rejected(Paging->root()).find("PAT"), std::string::npos);
  EXPECT_EQ(integer(Paging->TableBase), 0u);
}
} // namespace
} // namespace neverd::emulation
