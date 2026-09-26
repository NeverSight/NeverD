//===- KernelPhysicalMemoryTests.cpp - Physical pages and exact RAM owners ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Physical identities share guest bytes while allocation slices and pins
/// reject stale access, premature release and partial effects on bad ranges.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/KernelPhysicalMemory.h"

#include <array>

namespace neverd::emulation {
namespace {
constexpr uint64_t Base = 0x100000;
constexpr uint64_t Page = physical::PageSize;

class KernelPhysicalRAM : public ::testing::Test {
protected:
  std::unique_ptr<UnicornBackend> CPU;
  std::unique_ptr<KernelPhysicalMemory> Model;
  void SetUp() override {
    auto Backend = UnicornBackend::create(300 * Page);
    ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
    CPU = std::move(*Backend);
    ASSERT_EQ(llvm::toString(CPU->map(Base, 3 * Page, Read | Write)), "");
    Model = std::make_unique<KernelPhysicalMemory>(*CPU);
  }
  template <typename T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  template <typename T> void rejects(llvm::Expected<T> Value) {
    ASSERT_FALSE(bool(Value));
    llvm::consumeError(Value.takeError());
  }
};

TEST_F(KernelPhysicalRAM, SamePageOwnersSharePFNButNeverByteAuthority) {
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base + 16, 16)), "");
  ASSERT_EQ(llvm::toString(Model->registerRegion(2, Base + 32, 16)), "");
  EXPECT_EQ(take(Model->physicalAddress(Base + 16)),
            physical::PhysicalBase + 16);
  EXPECT_EQ(take(Model->physicalAddress(Base + 32)),
            physical::PhysicalBase + 32);
  EXPECT_GT(take(Model->physicalAddress(Base + 16)), UINT32_MAX);
  EXPECT_NE(take(Model->physicalAddress(Base + 16)), Base + 16);
  EXPECT_EQ(take(Model->ownerForRange(Base + 16, 16)), 1u);
  EXPECT_EQ(take(Model->ownerForRange(Base + 32, 16)), 2u);
  rejects(Model->ownerForRange(Base + 16, 17));
  rejects(Model->physicalAddress(Base + 15));
  ASSERT_EQ(llvm::toString(Model->retire(1)), "");
  rejects(Model->physicalAddress(Base + 16));
  rejects(Model->ownerForRange(Base + 16, 1));
  EXPECT_EQ(take(Model->physicalAddress(Base + 32)),
            physical::PhysicalBase + 32);
  ASSERT_EQ(llvm::toString(Model->registerRegion(3, Base + 16, 16)), "");
  EXPECT_EQ(take(Model->physicalAddress(Base + 16)),
            physical::PhysicalBase + 16);
  EXPECT_NE(llvm::toString(Model->registerRegion(1, Base + 64, 1)), "");
}

TEST_F(KernelPhysicalRAM, MappingRequestsInheritTheExistingPageCacheType) {
  using Cache = KernelPhysicalMemory::CacheType;
  constexpr std::array Types{Cache::Cached, Cache::NonCached,
                             Cache::WriteCombined};
  for (size_t I = 0; I != Types.size(); ++I) {
    const uint64_t Address = Base + I * Page;
    ASSERT_EQ(
        llvm::toString(Model->registerRegion(I + 1, Address, Page, Types[I])),
        "");
    const auto Physical = take(Model->physicalAddress(Address));
    for (Cache Requested : Types) {
      EXPECT_EQ(
          take(Model->cacheTypeForMapping(Address + 3, Page - 3, Requested)),
          Types[I]);
      EXPECT_EQ(take(Model->physicalAddress(Address)), Physical);
    }
  }
  rejects(Model->cacheTypeForMapping(Base, 1, static_cast<Cache>(-1)));
  rejects(Model->cacheTypeForMapping(Base, Page + 1, Cache::Cached));
}

TEST_F(KernelPhysicalRAM, CacheConflictsDoNotPublishOwnersOrNewPages) {
  using Cache = KernelPhysicalMemory::CacheType;
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base + Page + 16, 16,
                                                 Cache::NonCached)),
            "");
  const auto Existing = take(Model->physicalAddress(Base + Page + 16));
  EXPECT_NE(llvm::toString(Model->canRegisterRegion(2, Base, Page + 8)), "");
  EXPECT_NE(llvm::toString(Model->registerRegion(2, Base, Page + 8)), "");
  EXPECT_EQ(Model->find(2), nullptr);
  rejects(Model->physicalAddress(Base));
  ASSERT_EQ(llvm::toString(
                Model->registerRegion(2, Base, Page, Cache::WriteCombined)),
            "");
  EXPECT_EQ(take(Model->physicalAddress(Base)), physical::PhysicalBase + Page);
  EXPECT_EQ(take(Model->physicalAddress(Base + Page + 16)), Existing);
  ASSERT_EQ(llvm::toString(Model->retire(1)), "");
  EXPECT_NE(llvm::toString(Model->registerRegion(3, Base + Page + 16, 16)), "");
  ASSERT_EQ(llvm::toString(Model->registerRegion(3, Base + Page + 16, 16,
                                                 Cache::NonCached)),
            "");
  EXPECT_EQ(take(Model->physicalAddress(Base + Page + 16)), Existing);
  EXPECT_EQ(
      take(Model->cacheTypeForMapping(Base + Page + 16, 16, Cache::Cached)),
      Cache::NonCached);
}

TEST_F(KernelPhysicalRAM, AliasedPinsAndCPUObserveTheSameExistingBytes) {
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base + 16, 64)), "");
  const auto A = take(Model->pin(1, 8, 16));
  const auto B = take(Model->pin(1, 12, 4));
  const std::array<uint8_t, 4> Written{9, 8, 7, 6}, Other{1, 2, 3, 4};
  ASSERT_EQ(llvm::toString(Model->write(A, 4, Written)), "");
  std::array<uint8_t, 4> Bytes{};
  ASSERT_EQ(llvm::toString(Model->read(B, 0, Bytes)), "");
  EXPECT_EQ(Bytes, Written);
  ASSERT_EQ(llvm::toString(CPU->read(Base + 28, Bytes)), "");
  EXPECT_EQ(Bytes, Written);
  ASSERT_EQ(llvm::toString(CPU->write(Base + 28, Other)), "");
  ASSERT_EQ(llvm::toString(Model->read(A, 4, Bytes)), "");
  EXPECT_EQ(Bytes, Other);
}

TEST_F(KernelPhysicalRAM, CrossPageViewUsesBackingRatherThanAnyUserAddress) {
  ASSERT_EQ(llvm::toString(CPU->protect(Base, 3 * Page, 0)), "");
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base + Page - 3, 10)), "");
  const auto Segments = take(Model->describe(1, 1, 8));
  ASSERT_EQ(Segments.size(), 2u);
  EXPECT_EQ(Segments[0].Backing, Base + Page - 2);
  EXPECT_EQ(Segments[0].Physical, physical::PhysicalBase + Page - 2);
  EXPECT_EQ(Segments[0].Length, 2u);
  EXPECT_EQ(Segments[1].Backing, Base + Page);
  EXPECT_EQ(Segments[1].Physical, physical::PhysicalBase + Page);
  EXPECT_EQ(Segments[1].Length, 6u);
  const auto Pin = take(Model->pin(1, 1, 8));
  const std::array<uint8_t, 8> Written{1, 2, 3, 4, 5, 6, 7, 8};
  std::array<uint8_t, 8> Bytes{};
  ASSERT_EQ(llvm::toString(Model->write(Pin, 0, Written)), "");
  ASSERT_EQ(llvm::toString(Model->read(Pin, 0, Bytes)), "");
  EXPECT_EQ(Bytes, Written);
  EXPECT_FALSE(CPU->fault());
}

TEST_F(KernelPhysicalRAM,
       PhysicalPagesRemainStableAcrossRetirementAndNewViews) {
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base + Page, 1)), "");
  EXPECT_EQ(take(Model->physicalAddress(Base + Page)), physical::PhysicalBase);
  ASSERT_EQ(llvm::toString(Model->retire(1)), "");
  ASSERT_EQ(llvm::toString(Model->registerRegion(2, Base, 2 * Page)), "");
  const auto Segments = take(Model->describe(2, 0, 2 * Page));
  ASSERT_EQ(Segments.size(), 2u);
  EXPECT_EQ(Segments[0].Physical, physical::PhysicalBase + Page);
  EXPECT_EQ(Segments[1].Physical, physical::PhysicalBase);
}

TEST_F(KernelPhysicalRAM, PinsPreventRetirementBeforeAnyOwnerMutation) {
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base, 32)), "");
  const auto Pin = take(Model->pin(1, 0, 16));
  EXPECT_NE(llvm::toString(Model->canRetire(1)), "");
  EXPECT_NE(llvm::toString(Model->retire(1)), "");
  ASSERT_NE(Model->find(1), nullptr);
  EXPECT_EQ(Model->find(1)->Backing, Base);
  EXPECT_EQ(take(Model->ownerForRange(Base, 32)), 1u);
  EXPECT_EQ(llvm::toString(Model->canUnpin(Pin)), "");
  // Preflight does not release the pin or permit its owner to retire.
  EXPECT_NE(llvm::toString(Model->canRetire(1)), "");
  EXPECT_NE(llvm::toString(Model->canUnpin(Pin + 1)), "");
  ASSERT_EQ(llvm::toString(Model->unpin(Pin)), "");
  EXPECT_NE(llvm::toString(Model->canUnpin(Pin)), "");
  ASSERT_EQ(llvm::toString(Model->retire(1)), "");
  EXPECT_EQ(Model->find(1), nullptr);
  std::array<uint8_t, 1> Byte{};
  EXPECT_NE(llvm::toString(Model->read(Pin, 0, Byte)), "");
  EXPECT_NE(llvm::toString(Model->unpin(Pin)), "");
  EXPECT_NE(llvm::toString(Model->retire(1)), "");
}

TEST_F(KernelPhysicalRAM, IgnoredPinMustBeOwnedAndDoesNotHideOtherPins) {
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base, 32)), "");
  ASSERT_EQ(llvm::toString(Model->registerRegion(2, Base + 32, 32)), "");
  const auto A = take(Model->pin(1, 0, 16));
  const auto B = take(Model->pin(1, 8, 8));
  const auto C = take(Model->pin(2, 0, 16));
  EXPECT_NE(llvm::toString(Model->canRetire(1, A)), "");
  EXPECT_NE(llvm::toString(Model->canRetire(1, C)), "");
  EXPECT_NE(llvm::toString(Model->canRetire(1, C + 1)), "");
  ASSERT_EQ(llvm::toString(Model->unpin(B)), "");
  EXPECT_EQ(llvm::toString(Model->canRetire(1, A)), "");
  EXPECT_NE(llvm::toString(Model->retire(1)), "");
  ASSERT_EQ(llvm::toString(Model->unpin(A)), "");
  EXPECT_NE(llvm::toString(Model->canRetire(1, A)), "");
  EXPECT_EQ(llvm::toString(Model->retire(1)), "");
}

TEST_F(KernelPhysicalRAM, GroupReleaseOnlyExcludesItsExactLivePins) {
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base, 32)), "");
  ASSERT_EQ(llvm::toString(Model->registerRegion(2, Base + 64, 32)), "");
  ASSERT_EQ(llvm::toString(Model->registerRegion(3, Base + Page, 32)), "");
  const auto A = take(Model->pin(1, 0, 16));
  const auto B = take(Model->pin(1, 8, 8));
  const auto C = take(Model->pin(2, 0, 16));
  const auto Retained = take(Model->pin(3, 0, 16));
  const auto External = take(Model->pin(2, 8, 8));
  const std::array<std::pair<uint64_t, uint64_t>, 2> Ranges{
      {{Base, 32}, {Base + 64, 32}}};
  const std::array<uint64_t, 4> Pins{A, B, C, Retained};
  const std::array<uint8_t, 4> Written{1, 2, 3, 4};
  ASSERT_EQ(llvm::toString(Model->write(C, 8, Written)), "");
  EXPECT_NE(llvm::toString(Model->canReleaseRanges(Ranges, Pins)), "");
  EXPECT_NE(llvm::toString(Model->canReleaseRanges(Ranges, {})), "");
  std::array<uint8_t, 4> Bytes{};
  ASSERT_EQ(llvm::toString(Model->read(External, 0, Bytes)), "");
  EXPECT_EQ(Bytes, Written);
  ASSERT_EQ(llvm::toString(Model->unpin(External)), "");
  EXPECT_EQ(llvm::toString(Model->canReleaseRanges(Ranges, Pins)), "");
  // The plan can unlock a descriptor whose owner survives this retirement;
  // validation still leaves every pin and owner intact until the commit phase.
  for (uint64_t Pin : Pins)
    EXPECT_EQ(llvm::toString(Model->canUnpin(Pin)), "");
  for (uint64_t Owner : {1u, 2u, 3u})
    EXPECT_NE(llvm::toString(Model->canRetire(Owner)), "");
  ASSERT_EQ(llvm::toString(Model->read(C, 8, Bytes)), "");
  EXPECT_EQ(Bytes, Written);
  for (uint64_t Pin : Pins)
    ASSERT_EQ(llvm::toString(Model->unpin(Pin)), "");
  EXPECT_EQ(llvm::toString(Model->canReleaseRanges(Ranges, {})), "");
  EXPECT_EQ(llvm::toString(Model->retire(1)), "");
  EXPECT_EQ(llvm::toString(Model->retire(2)), "");
  EXPECT_NE(Model->find(3), nullptr);
}

TEST_F(KernelPhysicalRAM, InvalidGroupReleaseNeverConsumesPins) {
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base, 32)), "");
  const auto Pin = take(Model->pin(1, 0, 16));
  const std::array<uint8_t, 1> Written{0x67};
  ASSERT_EQ(llvm::toString(Model->write(Pin, 0, Written)), "");
  EXPECT_NE(llvm::toString(Model->canReleaseRanges({{Base, 32}}, {Pin, Pin})),
            "");
  EXPECT_NE(llvm::toString(Model->canReleaseRanges({{Base, 32}}, {Pin + 1})),
            "");
  EXPECT_NE(llvm::toString(Model->canReleaseRanges({{Base, 0}}, {Pin})), "");
  EXPECT_NE(llvm::toString(Model->canReleaseRanges(
                {{Base, 32}, {UINT64_MAX - 1, 4}}, {Pin})),
            "");
  std::array<uint8_t, 1> Bytes{};
  ASSERT_EQ(llvm::toString(Model->read(Pin, 0, Bytes)), "");
  EXPECT_EQ(Bytes, Written);
  EXPECT_NE(llvm::toString(Model->retire(1)), "");
  EXPECT_EQ(llvm::toString(Model->canReleaseRanges({{Base, 32}}, {Pin})), "");
}

TEST_F(KernelPhysicalRAM, InvalidRangesDoNotPublishOwnerOrConsumePageIdentity) {
  for (auto Range : {std::array<uint64_t, 2>{Base, 0},
                     std::array<uint64_t, 2>{UINT64_MAX - 1, 4},
                     std::array<uint64_t, 2>{Base + 3 * Page - 1, 2}}) {
    EXPECT_NE(llvm::toString(Model->registerRegion(1, Range[0], Range[1])), "");
    EXPECT_EQ(Model->find(1), nullptr);
  }
  EXPECT_NE(llvm::toString(Model->registerRegion(0, Base, 1)), "");
  ASSERT_EQ(llvm::toString(Model->canRegisterRegion(1, Base, 32)), "");
  EXPECT_EQ(Model->find(1), nullptr);
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base, 32)), "");
  EXPECT_EQ(take(Model->physicalAddress(Base)), physical::PhysicalBase);
  EXPECT_NE(llvm::toString(Model->registerRegion(1, Base + Page, 1)), "");
  EXPECT_NE(llvm::toString(Model->registerRegion(2, Base - 1, 2)), "");
  EXPECT_NE(llvm::toString(Model->registerRegion(2, Base + 31, 2)), "");
  ASSERT_EQ(llvm::toString(Model->registerRegion(2, Base + Page, 1)), "");
  EXPECT_EQ(take(Model->physicalAddress(Base + Page)),
            physical::PhysicalBase + Page);
}

TEST_F(KernelPhysicalRAM, InvalidPinSpanHasNoReadOrWritePrefixEffects) {
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base, 32)), "");
  const auto Pin = take(Model->pin(1, 8, 8));
  const std::array<uint8_t, 2> Original{0x12, 0x34};
  ASSERT_EQ(llvm::toString(Model->write(Pin, 6, Original)), "");
  std::array<uint8_t, 4> Bytes{1, 2, 3, 4};
  EXPECT_NE(llvm::toString(Model->write(Pin, 6, Bytes)), "");
  EXPECT_NE(llvm::toString(Model->read(Pin, 6, Bytes)), "");
  EXPECT_EQ(Bytes, (std::array<uint8_t, 4>{1, 2, 3, 4}));
  std::array<uint8_t, 2> ReadBack{};
  ASSERT_EQ(llvm::toString(Model->read(Pin, 6, ReadBack)), "");
  EXPECT_EQ(ReadBack, Original);
  EXPECT_NE(llvm::toString(Model->write(Pin, UINT64_MAX, {})), "");
  EXPECT_EQ(llvm::toString(Model->write(Pin, 8, {})), "");
  rejects(Model->pin(1, 31, 2));
  rejects(Model->pin(1, 0, 0));
  rejects(Model->describe(1, 31, 2));
  EXPECT_FALSE(CPU->fault());
}

TEST_F(KernelPhysicalRAM, PreviewsDoNotReserveOrReusePinIdentities) {
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base, 32)), "");
  EXPECT_EQ(llvm::toString(Model->canPin(1, 0, 16)), "");
  EXPECT_EQ(llvm::toString(Model->canPin(1, 0, 16)), "");
  const auto A = take(Model->pin(1, 0, 16));
  ASSERT_EQ(llvm::toString(Model->unpin(A)), "");
  const auto B = take(Model->pin(1, 0, 16));
  EXPECT_GT(B, A);
  std::array<uint8_t, 1> Byte{};
  EXPECT_NE(llvm::toString(Model->write(A, 0, Byte)), "");
  EXPECT_EQ(llvm::toString(Model->write(B, 0, Byte)), "");
}

TEST_F(KernelPhysicalRAM, PinCapacityFailureLeavesLiveOwnersAndViewsIntact) {
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base, 1)), "");
  std::vector<uint64_t> Pins;
  for (uint64_t I = 0; I != physical::PinLimit; ++I)
    Pins.push_back(take(Model->pin(1, 0, 1)));
  EXPECT_NE(llvm::toString(Model->canPin(1, 0, 1)), "");
  rejects(Model->pin(1, 0, 1));
  ASSERT_EQ(llvm::toString(Model->unpin(Pins.front())), "");
  const auto Next = take(Model->pin(1, 0, 1));
  EXPECT_GT(Next, Pins.back());
  EXPECT_NE(llvm::toString(Model->retire(1)), "");
}

TEST_F(KernelPhysicalRAM, OwnerTombstoneLimitIsCumulativeAndNeverReusesTokens) {
  for (uint64_t I = 1; I <= physical::RegionLimit; ++I) {
    ASSERT_EQ(llvm::toString(Model->registerRegion(I, Base, 1)), "");
    ASSERT_EQ(llvm::toString(Model->retire(I)), "");
  }
  EXPECT_NE(llvm::toString(
                Model->canRegisterRegion(physical::RegionLimit + 1, Base, 1)),
            "");
  EXPECT_NE(llvm::toString(Model->registerRegion(1, Base, 1)), "");
  EXPECT_EQ(Model->find(1), nullptr);
}

TEST_F(KernelPhysicalRAM, PhysicalPageShortageDoesNotPublishAPrefix) {
  ASSERT_EQ(llvm::toString(CPU->map(Base + 3 * Page, 254 * Page, 0)), "");
  auto Shortage = Model->registerRegion(1, Base, 257 * Page);
  EXPECT_TRUE(Shortage.isA<PhysicalMemoryLimitError>());
  llvm::consumeError(std::move(Shortage));
  EXPECT_EQ(Model->find(1), nullptr);
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base, 256 * Page)), "");
  EXPECT_EQ(take(Model->physicalAddress(Base)), physical::PhysicalBase);
  EXPECT_EQ(take(Model->physicalAddress(Base + 256 * Page - 1)),
            physical::PhysicalBase + physical::PhysicalSize - 1);
  EXPECT_NE(llvm::toString(Model->registerRegion(2, Base + 256 * Page, 1)), "");
}

TEST_F(KernelPhysicalRAM, ReleaseRangeIncludesPaddingAndEveryIntersectedOwner) {
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base + 16, 16)), "");
  ASSERT_EQ(llvm::toString(Model->registerRegion(2, Base + 48, 16)), "");
  const auto A = take(Model->pin(1, 0, 1));
  const auto B = take(Model->pin(2, 0, 1));
  EXPECT_EQ(llvm::toString(Model->canReleaseRange(Base, 16)), "");
  EXPECT_NE(llvm::toString(Model->canReleaseRange(Base, 17)), "");
  EXPECT_EQ(llvm::toString(Model->canReleaseRange(Base + 32, 16)), "");
  EXPECT_NE(llvm::toString(Model->canReleaseRange(Base, Page, A)), "");
  EXPECT_NE(llvm::toString(Model->canReleaseRange(Base + 48, 16, A)), "");
  EXPECT_EQ(llvm::toString(Model->canReleaseRange(Base + 48, 16, B)), "");
  // A pin elsewhere in an owner still prevents partial owner retirement.
  EXPECT_NE(llvm::toString(Model->canReleaseRange(Base + 63, 1)), "");
  ASSERT_EQ(llvm::toString(Model->unpin(B)), "");
  EXPECT_EQ(llvm::toString(Model->canReleaseRange(Base, Page, A)), "");
  EXPECT_NE(llvm::toString(Model->retire(1)), "");
  EXPECT_NE(Model->find(2), nullptr);
  EXPECT_NE(llvm::toString(Model->canReleaseRange(UINT64_MAX, 2)), "");
}
TEST_F(KernelPhysicalRAM, GrowingPinPreservesIdentityAndSameBytesAcrossPages) {
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base, 2 * Page)), "");
  const auto Pin = take(Model->pin(1, Page - 2, 2));
  ASSERT_EQ(llvm::toString(Model->canExtendPin(Pin, 6)), "");
  std::array<uint8_t, 4> Bytes{};
  EXPECT_NE(llvm::toString(Model->read(Pin, 2, Bytes)), "");
  ASSERT_EQ(llvm::toString(Model->extendPin(Pin, 6)), "");
  const std::array<uint8_t, 4> Written{5, 6, 7, 8};
  ASSERT_EQ(llvm::toString(Model->write(Pin, 2, Written)), "");
  ASSERT_EQ(llvm::toString(CPU->read(Base + Page, Bytes)), "");
  EXPECT_EQ(Bytes, Written);
  EXPECT_NE(llvm::toString(Model->retire(1)), "");
  ASSERT_EQ(llvm::toString(Model->unpin(Pin)), "");
  ASSERT_EQ(llvm::toString(Model->retire(1)), "");
}

TEST_F(KernelPhysicalRAM, PinGrowthCannotShrinkOrBorrowAdjacentOwnerBytes) {
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base, 16)), "");
  ASSERT_EQ(llvm::toString(Model->registerRegion(2, Base + 16, 16)), "");
  const auto Pin = take(Model->pin(1, 8, 4));
  EXPECT_NE(llvm::toString(Model->extendPin(Pin, 3)), "");
  EXPECT_NE(llvm::toString(Model->extendPin(Pin, 9)), "");
  EXPECT_NE(llvm::toString(Model->extendPin(Pin + 1, 4)), "");
  std::array<uint8_t, 1> Byte{};
  ASSERT_EQ(llvm::toString(Model->read(Pin, 3, Byte)), "");
  EXPECT_NE(llvm::toString(Model->read(Pin, 4, Byte)), "");
  ASSERT_EQ(llvm::toString(Model->extendPin(Pin, 8)), "");
  ASSERT_EQ(llvm::toString(Model->read(Pin, 7, Byte)), "");
  EXPECT_NE(llvm::toString(Model->read(Pin, 8, Byte)), "");
}

TEST_F(KernelPhysicalRAM, ExistingPinCanGrowWithAllPinSlotsOccupied) {
  ASSERT_EQ(llvm::toString(Model->registerRegion(1, Base, 16)), "");
  std::vector<uint64_t> Pins;
  for (uint64_t I = 0; I < physical::PinLimit; ++I)
    Pins.push_back(take(Model->pin(1, 0, 1)));
  rejects(Model->pin(1, 0, 1));
  ASSERT_EQ(llvm::toString(Model->extendPin(Pins.front(), 16)), "");
  std::array<uint8_t, 1> Byte{};
  ASSERT_EQ(llvm::toString(Model->read(Pins.front(), 15, Byte)), "");
  EXPECT_NE(llvm::toString(Model->read(Pins.back(), 1, Byte)), "");
  for (const auto Pin : Pins)
    ASSERT_EQ(llvm::toString(Model->unpin(Pin)), "");
  ASSERT_EQ(llvm::toString(Model->retire(1)), "");
}

} // namespace
} // namespace neverd::emulation
