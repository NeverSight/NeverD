//===- DyldSharedCacheTests.cpp - Cache address and slide contracts ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent serialized fixtures exercise cache-relative targets, subcache
/// identity and malformed-input boundaries without copying parser constants.
//===----------------------------------------------------------------------===//

#include "../../../lib/loader/ObjC/ObjCMethodLists.h"
#include "../../../lib/loader/ObjC/ObjCProperties.h"
#include "../../../lib/loader/ObjC/ObjCRelativeLists.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImageFlags.h"
#include "neverd/loader/MachO/DyldSharedCache.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/loader/MachO/MachOMetadataPointer.h"
#include "neverd/loader/ObjC/ObjCMetadataJSON.h"
#include "neverd/loader/ObjC/ObjCMethods.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/support/SHA256.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>

using namespace neverd;
using namespace neverd::macho_loader;

namespace {

constexpr va_t Base = 0x180000000;

struct CacheFixture {
  std::vector<uint8_t> Data = std::vector<uint8_t>(0x5000);

  template <typename T> void set(size_t Offset, T Value) {
    writeLE<T>(Data.data() + Offset, Value);
  }

  explicit CacheFixture(uint32_t Version = 5, va_t Address = Base) {
    std::memcpy(Data.data(), "dyld_v1  arm64e", 16);
    set<uint32_t>(16, 0x200);
    set<uint32_t>(20, 2);
    Data[88] = Address == Base ? 1 : 2;
    set<uint64_t>(224, Address);
    set<uint64_t>(232, Address == Base ? 0x20000 : 0);
    set<uint32_t>(312, 0x280);
    set<uint32_t>(316, 2);
    set<uint32_t>(448, 0x300);
    set<uint32_t>(452, 1);
    for (size_t I = 0; I != 2; ++I) {
      const uint64_t VA = Address + (I ? 0x2000 : 0);
      const uint64_t Size = I ? 0x3000 : 0x1000;
      const uint64_t Offset = I ? 0x1000 : 0;
      const uint32_t Protection = I ? 3 : 5;
      for (size_t Row : {0x200 + I * 32, 0x280 + I * 56}) {
        set<uint64_t>(Row, VA);
        set<uint64_t>(Row + 8, Size);
        set<uint64_t>(Row + 16, Offset);
      }
      set<uint32_t>(0x200 + I * 32 + 24, Protection);
      set<uint32_t>(0x200 + I * 32 + 28, Protection);
      set<uint32_t>(0x280 + I * 56 + 48, Protection);
      set<uint32_t>(0x280 + I * 56 + 52, Protection);
    }
    set<uint64_t>(0x280 + 56 + 24, 0x4400);
    set<uint64_t>(0x280 + 56 + 32, 30);
    set<uint64_t>(0x300, Address + 0x600);
    set<uint32_t>(0x300 + 24, 0x340);
    std::memcpy(Data.data() + 0x340, "/usr/lib/libfixture.dylib", 25);
    set<uint32_t>(0x600, 0xfeedfacf);
    set<uint32_t>(0x604, 0x100000c);
    set<uint32_t>(0x608, 2);
    set<uint32_t>(0x4400, Version);
    set<uint32_t>(0x4404, 0x1000);
    set<uint32_t>(0x4408, 3);
    set<uint64_t>(0x4410, Base);
    set<uint16_t>(0x4418, 8);
    set<uint16_t>(0x441a, 0xffff);
    set<uint16_t>(0x441c, 0);
    if (Version == 3) {
      set<uint64_t>(0x1008, (2ull << 51) | (0xabull << 43) | (Base + 0x80));
      set<uint64_t>(0x1018, (1ull << 63) | (3ull << 49) | (1ull << 48) |
                                (0xbeefull << 32) | 0x700);
      set<uint64_t>(0x3000, Base + 0x90);
    } else {
      set<uint64_t>(0x1008, (2ull << 52) | (0xabull << 34) | 0x80);
      set<uint64_t>(0x1018, (1ull << 63) | (1ull << 51) | (1ull << 50) |
                                (0xbeefull << 34) | 0x700);
      set<uint64_t>(0x3000, 0x90);
    }
  }

  std::unique_ptr<llvm::MemoryBuffer> buffer() const {
    return llvm::MemoryBuffer::getMemBufferCopy(llvm::StringRef(
        reinterpret_cast<const char *>(Data.data()), Data.size()));
  }

  llvm::Expected<DyldSharedCache> parse() const {
    std::vector<std::unique_ptr<llvm::MemoryBuffer>> Files;
    Files.push_back(buffer());
    return DyldSharedCache::parse(std::move(Files));
  }

  void imageLayout(va_t DataAddress = Base + 0x2000) {
    set<uint32_t>(0x610, 2);
    set<uint32_t>(0x614, 224);
    set<uint32_t>(0x620, 0x19);
    set<uint32_t>(0x624, 152);
    std::memcpy(Data.data() + 0x628, "__TEXT", 7);
    set<uint64_t>(0x638, Base + 0x600);
    set<uint64_t>(0x640, 0x400);
    set<uint64_t>(0x648, 0x600);
    set<uint64_t>(0x650, 0x400);
    set<uint32_t>(0x658, 5);
    set<uint32_t>(0x65c, 5);
    set<uint32_t>(0x660, 1);
    std::memcpy(Data.data() + 0x668, "__text", 7);
    std::memcpy(Data.data() + 0x678, "__TEXT", 7);
    set<uint64_t>(0x688, Base + 0x900);
    set<uint64_t>(0x690, 0x10);
    set<uint32_t>(0x698, 0x900);
    set<uint32_t>(0x69c, 2);
    set<uint32_t>(0x6a8, 0x80000400);
    set<uint32_t>(0x6b8, 0x19);
    set<uint32_t>(0x6bc, 72);
    std::memcpy(Data.data() + 0x6c0, "__DATA", 7);
    set<uint64_t>(0x6d0, DataAddress);
    set<uint64_t>(0x6d8, 0x3000);
    set<uint64_t>(0x6e0, 0x1000);
    set<uint64_t>(0x6e8, 0x3000);
    set<uint32_t>(0x6f0, 3);
    set<uint32_t>(0x6f4, 3);
    Data[0x900] = 0x42;
  }

  void imageProjection() {
    imageLayout();
    set<uint32_t>(0x60c, 6);
    set<uint32_t>(0x610, 8);
    set<uint32_t>(0x614, 488);
    set<uint64_t>(0x6d8, 0x1000);
    set<uint64_t>(0x6e8, 0x1000);
    set<uint32_t>(0x700, 0x19);
    set<uint32_t>(0x704, 72);
    std::memcpy(Data.data() + 0x708, "__LINKEDIT", 11);
    set<uint64_t>(0x718, Base + 0x4000);
    set<uint64_t>(0x720, 0x1000);
    set<uint64_t>(0x728, 0x3000);
    set<uint64_t>(0x730, 0x1000);
    set<uint32_t>(0x738, 1);
    set<uint32_t>(0x73c, 1);
    set<uint32_t>(0x748, 2);
    set<uint32_t>(0x74c, 24);
    set<uint32_t>(0x750, 0x3000);
    set<uint32_t>(0x754, 3);
    set<uint32_t>(0x758, 0x3400);
    set<uint32_t>(0x75c, 0x400);
    set<uint32_t>(0x760, 0xb);
    set<uint32_t>(0x764, 80);
    set<uint32_t>(0x774, 2);
    set<uint32_t>(0x778, 2);
    set<uint32_t>(0x77c, 1);
    set<uint32_t>(0x788, 0x3100);
    set<uint32_t>(0x78c, 1);
    set<uint32_t>(0x798, 0x3180);
    set<uint32_t>(0x79c, 3);
    set<uint32_t>(0x7b0, 0x29);
    set<uint32_t>(0x7b4, 16);
    set<uint32_t>(0x7b8, 0x3200);
    set<uint32_t>(0x7bc, 8);
    set<uint32_t>(0x7c0, 0x26);
    set<uint32_t>(0x7c4, 16);
    set<uint32_t>(0x7c8, 0x3220);
    set<uint32_t>(0x7cc, 3);
    set<uint32_t>(0x7d0, 0xd);
    set<uint32_t>(0x7d4, 56);
    set<uint32_t>(0x7d8, 24);
    std::memcpy(Data.data() + 0x7e8, "/usr/lib/libfixture.dylib", 25);
    set<uint32_t>(0x3000, 0x100);
    set<uint8_t>(0x3004, 0xf);
    set<uint8_t>(0x3005, 1);
    set<uint64_t>(0x3008, Base + 0x900);
    set<uint32_t>(0x3010, 0x120);
    set<uint8_t>(0x3014, 0xb);
    set<uint64_t>(0x3018, 0x100);
    set<uint32_t>(0x3020, 0x140);
    set<uint8_t>(0x3024, 1);
    set<uint32_t>(0x3100, 0x160);
    for (uint32_t I = 0; I < 3; ++I)
      set<uint32_t>(0x3180 + I * 4, I);
    set<uint32_t>(0x3200, 0x900);
    set<uint16_t>(0x3204, 4);
    set<uint16_t>(0x3206, 1);
    set<uint8_t>(0x3220, 0x80);
    set<uint8_t>(0x3221, 6);
    std::memcpy(Data.data() + 0x3500, "_target", 8);
    std::memcpy(Data.data() + 0x3520, "_alias", 7);
    std::memcpy(Data.data() + 0x3540, "_import", 8);
    std::memcpy(Data.data() + 0x3560, "_module", 8);
  }

  void v2Slides() {
    set<uint64_t>(0x2d8, 64);
    set<uint32_t>(0x4400, 2);
    set<uint32_t>(0x4408, 40);
    set<uint32_t>(0x440c, 3);
    set<uint32_t>(0x4410, 46);
    set<uint32_t>(0x4414, 2);
    set<uint64_t>(0x4418, 0x00ffff0000000000ull);
    set<uint64_t>(0x4420, Base);
    set<uint16_t>(0x4428, 0x8000);
    set<uint16_t>(0x442a, 0x4000);
    set<uint16_t>(0x442c, 1);
    set<uint16_t>(0x442e, 2);
    set<uint16_t>(0x4430, 0x800a);
    set<uint64_t>(0x1008, (4ull << 40) | 0x80);
    set<uint64_t>(0x1018, 0);
    set<uint64_t>(0x1028, 0xab00000000000090ull);
    set<uint64_t>(0x3004, 0xa0);
  }

  void objcRelativeLists() {
    set<uint64_t>(464, 0x480);
    set<uint64_t>(472, 56);
    set<uint32_t>(0x480, 1);
    set<uint64_t>(0x488, 0x500);
    set<uint64_t>(0x4b0, 0x800);
    set<uint32_t>(0x500, 1);
    set<uint32_t>(0x504, 16);
    set<int64_t>(0x508, 0x600 - 0x508);
    set<uint32_t>(0x1300, 8);
    set<uint32_t>(0x1304, 2);
    set<uint64_t>(0x1308, uint64_t(-int64_t(0x208)) << 16);
    set<uint64_t>(0x1310, uint64_t(0xf0) << 16);
    std::memcpy(Data.data() + 0x880, "first", 6);
    std::memcpy(Data.data() + 0x8a0, "second", 7);
    std::memcpy(Data.data() + 0x940, "v16@0:8", 8);
    for (size_t Leaf : {0x1100, 0x1400}) {
      set<uint32_t>(Leaf, 0xc000000c);
      set<uint32_t>(Leaf + 4, 1);
      set<int32_t>(Leaf + 8, Leaf == 0x1100 ? 0x80 : 0xa0);
      set<int32_t>(Leaf + 12, int64_t(0x940) - int64_t(Leaf + 0x100c));
      set<int32_t>(Leaf + 16, int64_t(0x980) - int64_t(Leaf + 0x1010));
    }
  }
};

} // namespace

TEST(DyldSharedCache, MapsFilesAndRetainsBothSlideLayoutsWithoutPatching) {
  for (uint32_t Version : {3u, 5u}) {
    CacheFixture Fixture(Version);
    auto Cache = Fixture.parse();
    ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
    EXPECT_EQ(Cache->baseAddress(), Base);
    ASSERT_EQ(Cache->mappings().size(), 2u);
    ASSERT_EQ(Cache->images().size(), 1u);
    EXPECT_EQ(Cache->images()[0].Path, "/usr/lib/libfixture.dylib");
    EXPECT_EQ(Cache->images()[0].Address, Base + 0x600);
    EXPECT_FALSE(Cache->readBytes(Base + 0x1000, 1));
    EXPECT_FALSE(Cache->readBytes(Base + 0xfff, 2));
    EXPECT_FALSE(Cache->readBytes(Base + 0x4fff, 2));
    EXPECT_FALSE(Cache->readBytes(InvalidVA, 8));
    auto Fixups = Cache->readSlideFixups(1);
    ASSERT_TRUE(bool(Fixups)) << llvm::toString(Fixups.takeError());
    ASSERT_EQ(Fixups->size(), 3u);
    EXPECT_EQ((*Fixups)[0].Slot, Base + 0x2008);
    EXPECT_EQ((*Fixups)[0].Target, 0xab00000000000000ull | (Base + 0x80));
    EXPECT_FALSE((*Fixups)[0].Authentication);
    EXPECT_EQ((*Fixups)[1].Slot, Base + 0x2018);
    ASSERT_TRUE((*Fixups)[1].Authentication);
    const auto &Auth = *(*Fixups)[1].Authentication;
    EXPECT_EQ(Auth.Target, Base + 0x700);
    EXPECT_EQ(Auth.Key, Version == 3 ? 3u : 2u);
    EXPECT_EQ(Auth.Diversity, 0xbeef);
    EXPECT_TRUE(Auth.AddressDiversity);
    EXPECT_EQ(Auth.EncodedValue, (*Fixups)[1].EncodedValue);
    EXPECT_EQ((*Fixups)[2].Slot, Base + 0x4000);
    EXPECT_EQ((*Fixups)[2].Target, Base + 0x90);
    const auto Bytes = Cache->readBytes(Base + 0x2018, 8);
    ASSERT_TRUE(Bytes);
    EXPECT_EQ(readLE<uint64_t>(Bytes->data()), Auth.EncodedValue);
    auto Empty = Cache->readSlideFixups(0);
    ASSERT_TRUE(bool(Empty));
    EXPECT_TRUE(Empty->empty());
  }
}

TEST(DyldSharedCache, V2ExtraChainsRetainNullAndTaggedTargets) {
  CacheFixture Fixture;
  Fixture.v2Slides();
  auto Cache = Fixture.parse();
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  auto Fixups = Cache->readSlideFixups(1);
  ASSERT_TRUE(bool(Fixups)) << llvm::toString(Fixups.takeError());
  ASSERT_EQ(Fixups->size(), 4u);
  const va_t Slots[] = {Base + 0x2008, Base + 0x2018, Base + 0x2028,
                        Base + 0x4004};
  const va_t Targets[] = {Base + 0x80, 0, 0xab00000000000000ull | (Base + 0x90),
                          Base + 0xa0};
  for (size_t I = 0; I != 4; ++I) {
    EXPECT_EQ((*Fixups)[I].Slot, Slots[I]);
    EXPECT_EQ((*Fixups)[I].Target, Targets[I]);
    EXPECT_EQ((*Fixups)[I].SlideVersion, 2u);
    EXPECT_FALSE((*Fixups)[I].Authentication);
    const auto Bytes = Cache->readBytes(Slots[I], 8);
    ASSERT_TRUE(Bytes);
    EXPECT_EQ(readLE<uint64_t>(Bytes->data()), (*Fixups)[I].EncodedValue);
  }
}

TEST(DyldSharedCache, V2MalformedExtraChainsFailTransactionally) {
  using Mutation = std::function<void(CacheFixture &)>;
  const std::vector<Mutation> Cases = {
      [](auto &F) { F.template set<uint64_t>(0x2d8, 39); },
      [](auto &F) { F.template set<uint32_t>(0x4408, 39); },
      [](auto &F) { F.template set<uint32_t>(0x440c, 4); },
      [](auto &F) { F.template set<uint32_t>(0x4410, 63); },
      [](auto &F) { F.template set<uint64_t>(0x4418, 0); },
      [](auto &F) { F.template set<uint64_t>(0x4418, 3); },
      [](auto &F) { F.template set<uint64_t>(0x4418, 0x500); },
      [](auto &F) { F.template set<uint16_t>(0x4428, 0x8002); },
      [](auto &F) { F.template set<uint16_t>(0x4430, 10); },
      [](auto &F) { F.template set<uint16_t>(0x4430, 0x8002); },
      [](auto &F) { F.template set<uint16_t>(0x4430, 0xc00a); },
      [](auto &F) { F.template set<uint16_t>(0x442c, 1023); },
      [](auto &F) { F.template set<uint64_t>(0x1008, (1ull << 40) | 0x80); },
      [](auto &F) { F.template set<uint64_t>(0x1008, (1024ull << 40) | 0x80); },
      [](auto &F) { F.template set<uint64_t>(0x4420, InvalidVA); },
  };
  for (size_t I = 0; I < Cases.size(); ++I) {
    SCOPED_TRACE(I);
    CacheFixture Fixture;
    Fixture.v2Slides();
    Cases[I](Fixture);
    auto Cache = Fixture.parse();
    ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
    auto Fixups = Cache->readSlideFixups(1);
    ASSERT_FALSE(bool(Fixups));
    llvm::consumeError(Fixups.takeError());
  }
}

TEST(DyldSharedCache, SubcacheTargetsUseTheMainCacheBase) {
  CacheFixture Main;
  CacheFixture Sub(5, Base + 0x10000);
  Main.set<uint32_t>(392, 0x380);
  Main.set<uint32_t>(396, 1);
  Main.Data[0x380] = 2;
  Main.set<uint64_t>(0x390, 0x10000);
  std::memcpy(Main.Data.data() + 0x398, ".01", 4);
  // An image header can reside in a different file from its catalogue.
  Main.set<uint64_t>(0x300, Base + 0x10600);
  auto Parse = [&]() {
    std::vector<std::unique_ptr<llvm::MemoryBuffer>> Files;
    Files.push_back(Main.buffer());
    Files.push_back(Sub.buffer());
    return DyldSharedCache::parse(std::move(Files));
  };
  auto Cache = Parse();
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  ASSERT_EQ(Cache->mappings().size(), 4u);
  EXPECT_EQ(Cache->images()[0].Address, Base + 0x10600);
  auto Fixups = Cache->readSlideFixups(3);
  ASSERT_TRUE(bool(Fixups)) << llvm::toString(Fixups.takeError());
  EXPECT_EQ((*Fixups)[1].Slot, Base + 0x12018);
  EXPECT_EQ((*Fixups)[1].Target, Base + 0x700);
  Sub.Data[88] ^= 1;
  auto WrongUUID = Parse();
  ASSERT_FALSE(bool(WrongUUID));
  llvm::consumeError(WrongUUID.takeError());
  Sub.Data[88] ^= 1;
  Main.set<uint64_t>(0x390, 0x20000);
  auto WrongPlacement = Parse();
  ASSERT_FALSE(bool(WrongPlacement));
  llvm::consumeError(WrongPlacement.takeError());
  auto Missing = Main.parse();
  ASSERT_FALSE(bool(Missing));
  llvm::consumeError(Missing.takeError());
}

TEST(DyldSharedCache, LegacyHeadersUseTheOriginalTables) {
  CacheFixture Main(3);
  std::memcpy(Main.Data.data() + 0x80, Main.Data.data() + 0x200, 64);
  Main.set<uint32_t>(16, 0x80);
  Main.set<uint32_t>(24, 0x300);
  Main.set<uint32_t>(28, 1);
  Main.set<uint64_t>(56, 0x4400);
  Main.set<uint64_t>(64, 30);
  auto Cache = Main.parse();
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  ASSERT_EQ(Cache->images().size(), 1u);
  auto Fixups = Cache->readSlideFixups(1);
  ASSERT_TRUE(bool(Fixups)) << llvm::toString(Fixups.takeError());
  ASSERT_EQ(Fixups->size(), 3u);
  EXPECT_EQ((*Fixups)[1].Authentication->Target, Base + 0x700);
}

TEST(DyldSharedCache, LegacySubcacheEntriesDoNotReadSuffixBytes) {
  CacheFixture Main;
  CacheFixture Sub(5, Base + 0x10000);
  std::memcpy(Main.Data.data() + 400, Main.Data.data() + 0x200, 64);
  Main.set<uint32_t>(16, 400);
  Main.set<uint32_t>(24, 0x300);
  Main.set<uint32_t>(28, 1);
  Main.set<uint32_t>(392, 0x380);
  Main.set<uint32_t>(396, 1);
  Main.Data[0x380] = 2;
  Main.set<uint64_t>(0x390, 0x10000);
  std::memset(Main.Data.data() + 0x398, 0xff, 32);
  std::vector<std::unique_ptr<llvm::MemoryBuffer>> Files;
  Files.push_back(Main.buffer());
  Files.push_back(Sub.buffer());
  auto Cache = DyldSharedCache::parse(std::move(Files));
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  ASSERT_EQ(Cache->mappings().size(), 4u);
  auto Fixups = Cache->readSlideFixups(3);
  ASSERT_TRUE(bool(Fixups)) << llvm::toString(Fixups.takeError());
  EXPECT_EQ((*Fixups)[1].Authentication->Target, Base + 0x700);
}

TEST(DyldSharedCache, AllKeysAndDiversityBitsRetainTheirLayout) {
  for (uint32_t Version : {3u, 5u})
    for (unsigned Key = 0; Key < (Version == 3 ? 4u : 2u); ++Key)
      for (unsigned AddressDiversity = 0; AddressDiversity < 2;
           ++AddressDiversity)
        for (uint64_t Diversity : {0u, 1u, 0xffffu}) {
          CacheFixture F(Version);
          const uint64_t TargetOffset =
              Version == 3 ? 0xffffffffull : 0x3ffffffffull;
          const uint64_t Word =
              (1ull << 63) | TargetOffset |
              (uint64_t(Key) << (Version == 3 ? 49 : 51)) |
              (uint64_t(AddressDiversity) << (Version == 3 ? 48 : 50)) |
              (Diversity << (Version == 3 ? 32 : 34));
          F.set<uint64_t>(0x1018, Word);
          auto Cache = F.parse();
          ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
          auto Fixups = Cache->readSlideFixups(1);
          ASSERT_TRUE(bool(Fixups)) << llvm::toString(Fixups.takeError());
          const auto &Auth = (*Fixups)[1].Authentication;
          ASSERT_TRUE(Auth);
          EXPECT_EQ(Auth->Key, Version == 3 ? Key : Key * 2);
          EXPECT_EQ(Auth->Diversity, Diversity);
          EXPECT_EQ(Auth->AddressDiversity, bool(AddressDiversity));
          EXPECT_EQ(Auth->Target, Base + TargetOffset);
        }
}

TEST(DyldSharedCache, CancellationStopsBeforePublishingResults) {
  CacheFixture F;
  auto Cache = F.parse();
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  unsigned Checks = 0;
  LoadProgress Progress;
  Progress.StopUser = &Checks;
  Progress.Stop = [](void *User) -> int {
    return ++*static_cast<unsigned *>(User) >= 3;
  };
  EXPECT_THROW(
      {
        auto Result = Cache->readSlideFixups(1, Progress);
        if (!Result)
          llvm::consumeError(Result.takeError());
      },
      LoadCancelled);
}

TEST(DyldSharedCache, FileLoadingFollowsValidatedSuffixAndUUID) {
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-dyld-cache", Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  const std::filesystem::path Path =
      std::filesystem::path(std::u8string(Directory.begin(), Directory.end())) /
      "cache";
  auto Subpath = Path;
  Subpath += ".01";
  const auto Write = [](const std::filesystem::path &Name,
                        const CacheFixture &F) {
    std::ofstream File(Name, std::ios::binary);
    File.write(reinterpret_cast<const char *>(F.Data.data()), F.Data.size());
    File.close();
    ASSERT_TRUE(File.good());
  };
  CacheFixture Main;
  CacheFixture Sub(5, Base + 0x10000);
  Main.set<uint32_t>(392, 0x380);
  Main.set<uint32_t>(396, 1);
  Main.Data[0x380] = 2;
  Main.set<uint64_t>(0x390, 0x10000);
  std::memcpy(Main.Data.data() + 0x398, ".01", 4);
  Write(Path, Main);
  auto Missing = DyldSharedCache::open(Path);
  ASSERT_FALSE(bool(Missing));
  llvm::consumeError(Missing.takeError());
  Write(Subpath, Sub);
  auto Cache = DyldSharedCache::open(Path);
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  EXPECT_EQ(Cache->mappings().size(), 4u);
  Sub.Data[88] ^= 1;
  Write(Subpath, Sub);
  auto Stale = DyldSharedCache::open(Path);
  ASSERT_FALSE(bool(Stale));
  llvm::consumeError(Stale.takeError());
  std::memcpy(Main.Data.data() + 0x398, ".01/escape", 11);
  Write(Path, Main);
  auto Invalid = DyldSharedCache::open(Path);
  ASSERT_FALSE(bool(Invalid));
  llvm::consumeError(Invalid.takeError());
}

TEST(DyldSharedCache, CachedImageFileOffsetsResolveThroughTheirSegments) {
  CacheFixture Main;
  CacheFixture Sub(5, Base + 0x10000);
  Main.set<uint32_t>(392, 0x380);
  Main.set<uint32_t>(396, 1);
  Main.Data[0x380] = 2;
  Main.set<uint64_t>(0x390, 0x10000);
  std::memcpy(Main.Data.data() + 0x398, ".01", 4);
  Main.imageLayout(Base + 0x12000);
  Sub.Data[0x1100] = 0x87;
  const auto Parse = [&] {
    std::vector<std::unique_ptr<llvm::MemoryBuffer>> Files;
    Files.push_back(Main.buffer());
    Files.push_back(Sub.buffer());
    return DyldSharedCache::parse(std::move(Files));
  };
  auto Cache = Parse();
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  auto Layout = Cache->readImageLayout(0);
  ASSERT_TRUE(bool(Layout)) << llvm::toString(Layout.takeError());
  ASSERT_EQ(Layout->Segments.size(), 2u);
  ASSERT_EQ(Layout->Sections.size(), 1u);
  EXPECT_EQ(Layout->Sections[0].Name, "__text");
  EXPECT_EQ(Layout->Sections[0].VA, Base + 0x900);
  EXPECT_EQ(Layout->Sections[0].Alignment, 4u);
  EXPECT_TRUE(Layout->Sections[0].isExecutable());
  auto Text = Cache->readImageFileBytes(0, 0x900, 1);
  ASSERT_TRUE(bool(Text)) << llvm::toString(Text.takeError());
  EXPECT_EQ((*Text)[0], 0x42);
  auto Data = Cache->readImageFileBytes(0, 0x1100, 1);
  ASSERT_TRUE(bool(Data)) << llvm::toString(Data.takeError());
  EXPECT_EQ((*Data)[0], 0x87);
  auto Missing = Cache->readImageFileBytes(0, 0xa00, 1);
  ASSERT_FALSE(bool(Missing));
  llvm::consumeError(Missing.takeError());
  Main.set<uint64_t>(0x6e0, 0x600);
  Cache = Parse();
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  auto Ambiguous = Cache->readImageFileBytes(0, 0x900, 1);
  ASSERT_FALSE(bool(Ambiguous));
  llvm::consumeError(Ambiguous.takeError());
}

TEST(DyldSharedCache, CachedImageLayoutRejectsMalformedLoadCommands) {
  using Mutation = std::function<void(CacheFixture &)>;
  const std::vector<Mutation> Cases = {
      [](auto &F) { F.template set<uint32_t>(0x610, 65537); },
      [](auto &F) { F.template set<uint32_t>(0x614, 0xffffffff); },
      [](auto &F) { F.template set<uint32_t>(0x624, 0); },
      [](auto &F) { F.template set<uint32_t>(0x624, 151); },
      [](auto &F) { F.template set<uint32_t>(0x660, 2); },
      [](auto &F) { F.template set<uint64_t>(0x650, 0x5000); },
      [](auto &F) { F.template set<uint64_t>(0x688, Base + 0xa00); },
      [](auto &F) { F.template set<uint32_t>(0x69c, 32); },
      [](auto &F) { F.template set<uint64_t>(0x6d8, InvalidVA); },
      [](auto &F) { F.template set<uint64_t>(0x6d0, Base + 0x800); },
  };
  for (size_t I = 0; I < Cases.size(); ++I) {
    SCOPED_TRACE(I);
    CacheFixture Main;
    Main.imageLayout();
    Cases[I](Main);
    auto Cache = Main.parse();
    ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
    auto Layout = Cache->readImageLayout(0);
    ASSERT_FALSE(bool(Layout));
    llvm::consumeError(Layout.takeError());
  }
}

TEST(DyldSharedCache, SectionFileRangesUseTheCachedSegmentLayout) {
  CacheFixture Fixture;
  Fixture.imageLayout();
  Fixture.set<uint32_t>(0x698, 0x300);
  auto Cache = Fixture.parse();
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  auto Layout = Cache->readImageLayout(0);
  ASSERT_TRUE(bool(Layout)) << llvm::toString(Layout.takeError());
  EXPECT_EQ(Layout->Sections[0].FileOff, 0x900u);
  EXPECT_EQ(readLE<uint32_t>(Layout->HeaderBytes.data() + 0x98), 0x300u);
}

TEST(DyldSharedCache, RejectsMalformedAddressSpacesAndCatalogues) {
  using Mutation = std::function<void(CacheFixture &)>;
  const std::vector<Mutation> Cases = {
      [](auto &F) { F.Data.resize(103); },
      [](auto &F) { F.Data[0] = 'x'; },
      [](auto &F) { F.template set<uint32_t>(16, 0xfffffff0); },
      [](auto &F) { F.template set<uint32_t>(20, 0xffffffff); },
      [](auto &F) { F.template set<uint64_t>(0x208, InvalidVA); },
      [](auto &F) { F.template set<uint64_t>(0x210, 0x4800); },
      [](auto &F) { F.template set<uint64_t>(0x280, Base + 8); },
      [](auto &F) {
        F.template set<uint64_t>(0x220, Base + 0x800);
        F.template set<uint64_t>(0x2b8, Base + 0x800);
      },
      [](auto &F) {
        F.template set<uint64_t>(0x230, 0x800);
        F.template set<uint64_t>(0x2c8, 0x800);
      },
      [](auto &F) { F.template set<uint32_t>(452, 65537); },
      [](auto &F) { F.template set<uint64_t>(0x300, Base + 0x1000); },
      [](auto &F) { F.template set<uint32_t>(0x318, 0x5000); },
      [](auto &F) { F.Data[0x340] = 0; },
      [](auto &F) {
        F.template set<uint32_t>(0x318, 0x4ffe);
        F.Data[0x4ffe] = 'a';
        F.Data[0x4fff] = 'b';
      },
      [](auto &F) { F.template set<uint64_t>(0x2d0, 0x4fff); },
  };
  for (size_t I = 0; I < Cases.size(); ++I) {
    SCOPED_TRACE(I);
    CacheFixture F;
    Cases[I](F);
    auto Cache = F.parse();
    ASSERT_FALSE(bool(Cache));
    EXPECT_FALSE(llvm::toString(Cache.takeError()).empty());
  }
}

TEST(DyldSharedCache, RejectsMalformedChainsWithoutPublishingTheirPrefix) {
  using Mutation = std::function<void(CacheFixture &)>;
  for (uint32_t Version : {3u, 5u}) {
    const std::vector<Mutation> Cases = {
        [](auto &F) { F.template set<uint32_t>(0x4400, 99); },
        [](auto &F) { F.template set<uint32_t>(0x4404, 1024); },
        [](auto &F) { F.template set<uint32_t>(0x4408, 4); },
        [](auto &F) { F.template set<uint16_t>(0x4418, 7); },
        [](auto &F) { F.template set<uint16_t>(0x4418, 0x1000); },
        [](auto &F) { F.template set<uint64_t>(0x4410, InvalidVA); },
        [=](auto &F) {
          F.template set<uint64_t>(0x1018, uint64_t(0x7ff)
                                               << (Version == 3 ? 51 : 52));
        },
        [=](auto &F) {
          F.template set<uint64_t>(0x1018, uint64_t(1)
                                               << (Version == 3 ? 62 : 42));
        },
    };
    for (size_t I = 0; I < Cases.size(); ++I) {
      SCOPED_TRACE(Version);
      SCOPED_TRACE(I);
      CacheFixture F(Version);
      Cases[I](F);
      auto Cache = F.parse();
      ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
      auto Fixups = Cache->readSlideFixups(1);
      ASSERT_FALSE(bool(Fixups));
      EXPECT_FALSE(llvm::toString(Fixups.takeError()).empty());
    }
  }
}

TEST(DyldSharedCache, RangeReadsValidateWholePagesAndRetainOverlap) {
  for (uint32_t Version : {2u, 3u, 5u}) {
    SCOPED_TRACE(Version);
    CacheFixture Fixture(Version);
    if (Version == 2)
      Fixture.v2Slides();
    auto Cache = Fixture.parse();
    ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
    auto Part = Cache->readSlideFixupsInRange(Base + 0x201b, 1);
    ASSERT_TRUE(bool(Part)) << llvm::toString(Part.takeError());
    ASSERT_EQ(Part->size(), 1u);
    EXPECT_EQ((*Part)[0].Slot, Base + 0x2018);
    auto Gap = Cache->readSlideFixupsInRange(Base + 0x2800, 0x800);
    ASSERT_TRUE(bool(Gap)) << llvm::toString(Gap.takeError());
    EXPECT_TRUE(Gap->empty());
    auto Empty = Cache->readSlideFixupsInRange(Base + 0x2018, 0);
    ASSERT_TRUE(bool(Empty));
    EXPECT_TRUE(Empty->empty());
    auto Overflow = Cache->readSlideFixupsInRange(InvalidVA - 1, 8);
    ASSERT_FALSE(bool(Overflow));
    llvm::consumeError(Overflow.takeError());
    auto All = Cache->readSlideFixupsInRange(Base, 0x5000);
    ASSERT_TRUE(bool(All)) << llvm::toString(All.takeError());
    EXPECT_EQ(All->size(), Version == 2 ? 4u : 3u);

    // Corruption after the requested pointer still invalidates its page.
    Fixture.set<uint64_t>(
        0x1018, Version == 2 ? 1024ull << 40
                             : uint64_t(0x7ff) << (Version == 3 ? 51 : 52));
    Cache = Fixture.parse();
    ASSERT_TRUE(bool(Cache));
    auto Broken = Cache->readSlideFixupsInRange(Base + 0x2008, 8);
    ASSERT_FALSE(bool(Broken));
    llvm::consumeError(Broken.takeError());
    // Independent pages remain readable without decoding the entire mapping.
    auto OtherPage = Cache->readSlideFixupsInRange(Base + 0x4000, 16);
    ASSERT_TRUE(bool(OtherPage)) << llvm::toString(OtherPage.takeError());
    ASSERT_EQ(OtherPage->size(), 1u);
  }
}

TEST(DyldSharedCache, MetadataRecipesResolveOutsideTheSelectedImage) {
  for (uint32_t Version : {2u, 3u, 5u}) {
    SCOPED_TRACE(Version);
    CacheFixture Fixture(Version);
    if (Version == 2)
      Fixture.v2Slides();
    auto Parsed = Fixture.parse();
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    BinaryImage Image;
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.MachOCPUSubtype = 2;
    Image.MachOHasChainedFixups = true;
    Image.MachOSharedCache =
        std::make_shared<DyldSharedCache>(std::move(*Parsed));
    Segment Segment;
    Segment.VA = Base + 0x2000;
    Segment.Size = Segment.FileSz = 0x3000;
    Segment.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    auto Bytes = Image.MachOSharedCache->readBytes(Segment.VA, Segment.Size);
    ASSERT_TRUE(Bytes);
    Segment.Data.assign(Bytes->begin(), Bytes->end());
    Image.Segments.push_back(std::move(Segment));
    auto Fixups = Image.MachOSharedCache->readSlideFixups(1);
    ASSERT_TRUE(bool(Fixups));
    if (Version != 2)
      Image.MachOAuthenticatedPointers.emplace(Base + 0x2018,
                                               *(*Fixups)[1].Authentication);
    EXPECT_EQ(readMetadataPointerTarget(Image, Base + 0x2018),
              Version == 2 ? 0 : Base + 0x700);
    EXPECT_EQ(Image.readVA(Base + 0x700, 1), nullptr);
    EXPECT_NE(readMetadataBytes(Image, Base + 0x700, 1), nullptr);
    EXPECT_FALSE(readMetadataPointerTarget(Image, Base + 0x201c));
    EXPECT_FALSE(readMetadataBytes(Image, Base + 0x1000, 1));
    EXPECT_FALSE(readMetadataBytes(Image, InvalidVA - 1, 8));

    // A local edit cannot fall back to the original cache's signing recipe.
    Image.Segments[0].Data[0x18] ^= 1;
    EXPECT_FALSE(readMetadataPointerTarget(Image, Base + 0x2018));
    Image.Segments[0].Data[0x18] ^= 1;
    Image.ImportPtrSlots.emplace(Base + 0x201c, "conflict");
    EXPECT_FALSE(readMetadataPointerTarget(Image, Base + 0x2018));
    Image.ImportPtrSlots.clear();
    if (Version != 2) {
      const auto Entry = Image.MachOAuthenticatedPointers.find(Base + 0x2018);
      ASSERT_NE(Entry, Image.MachOAuthenticatedPointers.end());
      auto &Recipe = Entry->second;
      Recipe.LibraryOrdinal = llvm::MachO::BIND_SPECIAL_DYLIB_SELF;
      EXPECT_FALSE(readMetadataPointerTarget(Image, Entry->first));
      Recipe.LibraryOrdinal.reset();
      EXPECT_TRUE(readMetadataPointerTarget(Image, Entry->first));
      Recipe.Diversity ^= 1;
      EXPECT_FALSE(readMetadataPointerTarget(Image, Base + 0x2018));
      Recipe.Diversity ^= 1;
      Image.CodePtrRelocSlots.insert(Base + 0x201c);
      EXPECT_FALSE(readMetadataPointerTarget(Image, Base + 0x2018));
      Image.CodePtrRelocSlots.clear();
    }

    const va_t Last = Version == 2 ? Base + 0x4004 : Base + 0x4000;
    const va_t Target = Version == 2 ? Base + 0xa0 : Base + 0x90;
    EXPECT_EQ(readMetadataPointerTarget(Image, Last), Target);
    writeLE<uint64_t>(Image.Segments[0].Data.data() + Last - Base - 0x2000,
                      Target);
    if (Version != 3)
      EXPECT_FALSE(readMetadataPointerTarget(Image, Last));
    else
      EXPECT_EQ(readMetadataPointerTarget(Image, Last), Target);
    Image.MachOResolvedChainedPointerSlots.insert(Last);
    EXPECT_EQ(readMetadataPointerTarget(Image, Last), Target);
    Image.MachOChainedFixupsAmbiguous = true;
    EXPECT_FALSE(readMetadataPointerTarget(Image, Last));
  }
}

TEST(DyldSharedCache, RelativeSelectorBaseReadsModernAndLegacyLayouts) {
  for (uint32_t Version : {1u, 2u}) {
    CacheFixture Fixture;
    Fixture.set<uint64_t>(464, 0x480);
    Fixture.set<uint64_t>(472, Version == 1 ? 56 : 72);
    Fixture.set<uint32_t>(0x480, Version);
    Fixture.set<uint64_t>(0x4b0, 0x800);
    auto Cache = Fixture.parse();
    ASSERT_TRUE(bool(Cache));
    auto BaseAddress = Cache->objcSelectorLayout();
    ASSERT_TRUE(bool(BaseAddress)) << llvm::toString(BaseAddress.takeError());
    ASSERT_TRUE(*BaseAddress);
    EXPECT_EQ((*BaseAddress)->Encoding,
              DyldCacheSelectorEncoding::BaseRelative);
    EXPECT_EQ((*BaseAddress)->Base, Base + 0x800);
    Fixture.set<uint64_t>(0x4b0, 0);
    Cache = Fixture.parse();
    ASSERT_TRUE(bool(Cache));
    BaseAddress = Cache->objcSelectorLayout();
    ASSERT_TRUE(bool(BaseAddress));
    EXPECT_FALSE(*BaseAddress);
  }
  CacheFixture Fixture;
  Fixture.imageLayout();
  std::memcpy(Fixture.Data.data() + 0x340, "/usr/lib/libobjc.A.dylib",
              sizeof("/usr/lib/libobjc.A.dylib"));
  std::memcpy(Fixture.Data.data() + 0x668, "__objc_opt_ro", 14);
  Fixture.set<uint64_t>(0x690, 48);
  Fixture.set<uint32_t>(0x900, 16);
  Fixture.set<int64_t>(0x928, -0x100);
  auto Cache = Fixture.parse();
  ASSERT_TRUE(bool(Cache));
  auto BaseAddress = Cache->objcSelectorLayout();
  ASSERT_TRUE(bool(BaseAddress)) << llvm::toString(BaseAddress.takeError());
  ASSERT_TRUE(*BaseAddress);
  EXPECT_EQ((*BaseAddress)->Encoding, DyldCacheSelectorEncoding::BaseRelative);
  EXPECT_EQ((*BaseAddress)->Base, Base + 0x800);
  Fixture.set<uint64_t>(0x928, uint64_t(1) << 63);
  Cache = Fixture.parse();
  ASSERT_TRUE(bool(Cache));
  BaseAddress = Cache->objcSelectorLayout();
  ASSERT_FALSE(bool(BaseAddress));
  llvm::consumeError(BaseAddress.takeError());
}

TEST(DyldSharedCache, RelativeSelectorBaseRejectsMalformedOptimizationHeaders) {
  auto Empty = DyldSharedCache().objcSelectorLayout();
  ASSERT_FALSE(bool(Empty));
  llvm::consumeError(Empty.takeError());
  using Mutation = std::function<void(CacheFixture &)>;
  const std::vector<Mutation> Cases = {
      [](auto &F) { F.template set<uint64_t>(464, 0); },
      [](auto &F) { F.template set<uint64_t>(472, 0); },
      [](auto &F) { F.template set<uint64_t>(464, InvalidVA); },
      [](auto &F) { F.template set<uint64_t>(464, 0x1000); },
      [](auto &F) { F.template set<uint64_t>(472, 55); },
      [](auto &F) { F.template set<uint32_t>(0x480, 99); },
      [](auto &F) { F.template set<uint32_t>(0x480, 2); },
      [](auto &F) { F.template set<uint64_t>(0x4b0, InvalidVA); },
      [](auto &F) { F.template set<uint64_t>(0x4b0, 0x1000); },
  };
  for (size_t I = 0; I < Cases.size(); ++I) {
    SCOPED_TRACE(I);
    CacheFixture Fixture;
    Fixture.set<uint64_t>(464, 0x480);
    Fixture.set<uint64_t>(472, 56);
    Fixture.set<uint32_t>(0x480, 1);
    Fixture.set<uint64_t>(0x4b0, 0x800);
    Cases[I](Fixture);
    auto Cache = Fixture.parse();
    ASSERT_TRUE(bool(Cache));
    auto BaseAddress = Cache->objcSelectorLayout();
    ASSERT_FALSE(bool(BaseAddress));
    llvm::consumeError(BaseAddress.takeError());
  }
}

TEST(DyldSharedCache, CompactMethodSelectorsUseTheVersionedCacheLayout) {
  for (uint32_t LayoutVersion : {1u, 15u, 16u}) {
    for (uint32_t Flags : {0x8000000cu, 0xc000000cu}) {
      SCOPED_TRACE(LayoutVersion);
      SCOPED_TRACE(Flags);
      CacheFixture Fixture;
      if (LayoutVersion == 1) {
        Fixture.set<uint64_t>(464, 0x480);
        Fixture.set<uint64_t>(472, 56);
        Fixture.set<uint32_t>(0x480, 1);
        Fixture.set<uint64_t>(0x4b0, 0x800);
      } else {
        Fixture.imageLayout();
        std::memcpy(Fixture.Data.data() + 0x340, "/usr/lib/libobjc.A.dylib",
                    sizeof("/usr/lib/libobjc.A.dylib"));
        std::memcpy(Fixture.Data.data() + 0x668, "__objc_opt_ro", 14);
        Fixture.set<uint64_t>(0x690, LayoutVersion == 15 ? 32 : 48);
        Fixture.set<uint32_t>(0x900, LayoutVersion);
        Fixture.set<int64_t>(0x928, -0x100);
      }
      Fixture.set<uint32_t>(0x1100, Flags);
      Fixture.set<uint32_t>(0x1104, 1);
      Fixture.set<int32_t>(0x1108, LayoutVersion == 15 ? 0x880 - 0x2108 : 0x80);
      Fixture.set<int32_t>(0x110c, 0x940 - 0x210c);
      Fixture.set<int32_t>(0x1110, 0x980 - 0x2110);
      std::memcpy(Fixture.Data.data() + 0x880, "selector:", 10);
      std::memcpy(Fixture.Data.data() + 0x940, "v24@0:8q16", 11);
      BinaryImage Image;
      Image.Format = BinaryFormat::MachO;
      Image.Arch = Arch::AArch64;
      Image.Bits = Bitness::Bits64;
      auto Cache = Fixture.parse();
      ASSERT_TRUE(bool(Cache));
      Image.MachOSharedCache =
          std::make_shared<DyldSharedCache>(std::move(*Cache));
      size_t Remaining = 1;
      std::string Diagnostic;
      auto Methods =
          objc::readMethodList(Image, Base + 0x2100, Remaining, Diagnostic);
      ASSERT_TRUE(Methods) << Diagnostic;
      ASSERT_EQ(Methods->size(), 1u);
      EXPECT_EQ(Methods->front().Selector, "selector:");
      EXPECT_EQ(Methods->front().TypeEncoding, "v24@0:8q16");
      EXPECT_EQ(Methods->front().Implementation, Base + 0x980);
      EXPECT_EQ(Remaining, 0u);
      if (LayoutVersion != 15) {
        std::memcpy(Fixture.Data.data() + 0x780, "backwards:", 11);
        Fixture.set<int32_t>(0x1108, -0x80);
        Cache = Fixture.parse();
        ASSERT_TRUE(bool(Cache));
        Image.MachOSharedCache =
            std::make_shared<DyldSharedCache>(std::move(*Cache));
        Remaining = 1;
        Methods =
            objc::readMethodList(Image, Base + 0x2100, Remaining, Diagnostic);
        ASSERT_TRUE(Methods) << Diagnostic;
        EXPECT_EQ(Methods->front().Selector, "backwards:");
      }
      if (LayoutVersion == 1)
        Fixture.set<uint64_t>(0x4b0, 0);
      else if (LayoutVersion == 16)
        Fixture.set<uint64_t>(0x928, 0);
      else
        Fixture.set<uint64_t>(0x690, 31);
      Cache = Fixture.parse();
      ASSERT_TRUE(bool(Cache));
      Image.MachOSharedCache =
          std::make_shared<DyldSharedCache>(std::move(*Cache));
      Remaining = 1;
      EXPECT_FALSE(
          objc::readMethodList(Image, Base + 0x2100, Remaining, Diagnostic));
      EXPECT_FALSE(Diagnostic.empty());
      EXPECT_EQ(Remaining, 1u);
    }
  }
}

TEST(DyldSharedCache, RelativeListArraysRetainTheirConditionalImageOrigins) {
  CacheFixture Fixture;
  Fixture.objcRelativeLists();
  auto Cache = Fixture.parse();
  ASSERT_TRUE(bool(Cache));
  auto Origin = Cache->objcImageAddress(0);
  ASSERT_TRUE(bool(Origin)) << llvm::toString(Origin.takeError());
  EXPECT_EQ(*Origin, Base + 0x600);
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.MachOSharedCache = std::make_shared<DyldSharedCache>(std::move(*Cache));
  size_t Remaining = 4;
  std::string Diagnostic;
  auto Methods =
      objc::readMethodList(Image, Base + 0x2301, Remaining, Diagnostic);
  ASSERT_TRUE(Methods) << Diagnostic;
  ASSERT_EQ(Methods->size(), 2u);
  EXPECT_EQ((*Methods)[0].Selector, "first");
  EXPECT_EQ((*Methods)[1].Selector, "second");
  for (const auto &Method : *Methods) {
    ASSERT_TRUE(Method.CacheOrigin);
    EXPECT_EQ(Method.CacheOrigin->ImageIndex, 0u);
    EXPECT_EQ(Method.CacheOrigin->ImageAddress, Base + 0x600);
    EXPECT_EQ(Method.Implementation, Base + 0x980);
  }
  EXPECT_EQ(Remaining, 0u);
  Image.MachOSharedCache.reset();
  Remaining = 4;
  auto Missing = objc::readListReferences(Image, Base + 0x2301, Remaining);
  ASSERT_FALSE(bool(Missing));
  llvm::consumeError(Missing.takeError());
  EXPECT_EQ(Remaining, 4u);
}

TEST(DyldSharedCache, RelativeListFailuresDoNotPublishTheirPrefix) {
  using Mutation = std::function<void(CacheFixture &)>;
  const std::vector<Mutation> Cases = {
      [](auto &F) { F.template set<uint32_t>(0x1300, 16); },
      [](auto &F) { F.template set<uint32_t>(0x1304, 5); },
      [](auto &F) { F.template set<uint64_t>(0x1310, (0xf0ull << 16) | 1); },
      [](auto &F) { F.template set<uint64_t>(0x1310, 1); },
      [](auto &F) { F.template set<uint64_t>(0x1310, 0xf1ull << 16); },
      [](auto &F) { F.template set<uint64_t>(0x1310, 0xfffffff00000); },
      [](auto &F) { F.template set<uint64_t>(0x488, 0); },
      [](auto &F) { F.template set<uint32_t>(0x504, 32); },
      [](auto &F) { F.template set<uint32_t>(0x500, 65537); },
      [](auto &F) { F.template set<int64_t>(0x508, 0x100); },
  };
  for (size_t I = 0; I < Cases.size(); ++I) {
    SCOPED_TRACE(I);
    CacheFixture Fixture;
    Fixture.objcRelativeLists();
    Cases[I](Fixture);
    auto Cache = Fixture.parse();
    ASSERT_TRUE(bool(Cache));
    BinaryImage Image;
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.MachOSharedCache =
        std::make_shared<DyldSharedCache>(std::move(*Cache));
    size_t Remaining = 4;
    auto References = objc::readListReferences(Image, Base + 0x2301, Remaining);
    ASSERT_FALSE(bool(References));
    llvm::consumeError(References.takeError());
    EXPECT_EQ(Remaining, 4u);
  }
}

TEST(DyldSharedCache, RelativePropertyListsUseTheSameOriginAndPointerReader) {
  CacheFixture Fixture;
  Fixture.objcRelativeLists();
  for (size_t Leaf : {0x1100, 0x1400})
    Fixture.set<uint32_t>(Leaf, 16);
  Fixture.set<uint64_t>(0x1018, (1ull << 63) | (30ull << 52) | 0x2301);
  Fixture.set<uint64_t>(0x1108, (1ull << 52) | 0x880);
  Fixture.set<uint64_t>(0x1110, (95ull << 52) | 0x940);
  Fixture.set<uint64_t>(0x1408, (1ull << 52) | 0x8a0);
  Fixture.set<uint64_t>(0x1410, 0x960);
  std::memcpy(Fixture.Data.data() + 0x940, "Tq,R", 5);
  std::memcpy(Fixture.Data.data() + 0x960, "Ti,R", 5);
  auto Cache = Fixture.parse();
  ASSERT_TRUE(bool(Cache));
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.MachOSharedCache = std::make_shared<DyldSharedCache>(std::move(*Cache));
  ObjCProperty Owner;
  Owner.OwnerName = Owner.ClassName = "CacheClass";
  size_t Remaining = 4;
  ASSERT_TRUE(objc::readPropertyList(Image, Base + 0x2018, Owner, Remaining));
  ASSERT_EQ(Image.ObjCProperties.size(), 2u);
  EXPECT_EQ(Image.ObjCProperties[0].Name, "first");
  EXPECT_EQ(Image.ObjCProperties[1].Name, "second");
  for (const auto &Property : Image.ObjCProperties) {
    ASSERT_TRUE(Property.CacheOrigin);
    EXPECT_EQ(Property.CacheOrigin->ImageAddress, Base + 0x600);
    EXPECT_EQ(Property.Status, "supported");
  }
  EXPECT_EQ(Remaining, 0u);
  auto Metadata = objcMetadataJSON(Image);
  const auto *Properties = Metadata.getArray("properties");
  ASSERT_TRUE(Properties);
  ASSERT_EQ(Properties->size(), 2u);
  const auto *Origin =
      Properties->front().getAsObject()->getObject("cache_origin");
  ASSERT_TRUE(Origin);
  EXPECT_EQ(Origin->getInteger("objc_image_index"), 0);
  EXPECT_EQ(Origin->getString("image_address"), "0x180000600");
  EXPECT_EQ(Origin->getString("condition"), "image_loaded");
}

TEST(DyldSharedCache, RelativeListsAcceptEmptyHeadersAndExtendedImageRows) {
  for (uint32_t Stride : {16, 24}) {
    SCOPED_TRACE(Stride);
    CacheFixture Fixture;
    Fixture.objcRelativeLists();
    Fixture.set<uint32_t>(0x504, Stride);
    Fixture.set<uint64_t>(0x1310, 0);
    Fixture.set<uint32_t>(0x1100, 3);
    Fixture.set<uint32_t>(0x1104, 0);
    auto Cache = Fixture.parse();
    ASSERT_TRUE(bool(Cache));
    BinaryImage Image;
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.MachOSharedCache =
        std::make_shared<DyldSharedCache>(std::move(*Cache));
    size_t Remaining = 2;
    std::string Diagnostic = "previous failure";
    auto Methods =
        objc::readMethodList(Image, Base + 0x2301, Remaining, Diagnostic);
    ASSERT_TRUE(Methods) << Diagnostic;
    EXPECT_TRUE(Methods->empty());
    EXPECT_TRUE(Diagnostic.empty());
    EXPECT_EQ(Remaining, 0u);
    Fixture.set<uint32_t>(0x1104, 1);
    Cache = Fixture.parse();
    ASSERT_TRUE(bool(Cache));
    Image.MachOSharedCache =
        std::make_shared<DyldSharedCache>(std::move(*Cache));
    Remaining = 3;
    EXPECT_FALSE(
        objc::readMethodList(Image, Base + 0x2301, Remaining, Diagnostic));
    EXPECT_FALSE(Diagnostic.empty());
  }
}

TEST(DyldSharedCache, ImageProjectionPreservesAddressesAndCompactsSymbolNames) {
  CacheFixture Fixture;
  Fixture.imageProjection();
  // Signed cached images can retain a stale original section file offset.
  Fixture.set<uint32_t>(0x698, 0x11);
  auto Cache = Fixture.parse();
  ASSERT_TRUE(bool(Cache));
  auto Prepared = Cache->prepareImage(0);
  ASSERT_TRUE(bool(Prepared)) << llvm::toString(Prepared.takeError());
  EXPECT_LT(Prepared->size(), 0x2000u);
  const auto U32 = [&](size_t At) {
    return readLE<uint32_t>(Prepared->data() + At);
  };
  const auto U64 = [&](size_t At) {
    return readLE<uint64_t>(Prepared->data() + At);
  };
  EXPECT_EQ(U64(0x38), Base + 0x600);
  EXPECT_EQ(U64(0x48), 0u);
  EXPECT_EQ(U32(0x98), 0x300u);
  EXPECT_EQ((*Prepared)[0x300], 0x42u);
  EXPECT_EQ(U64(0xe0), 0x400u);
  EXPECT_EQ(U64(0x128), 0x1400u);
  EXPECT_EQ(U64(0x130), Prepared->size() - 0x1400);
  const size_t Symbols = U32(0x150), Strings = U32(0x158);
  EXPECT_EQ(U32(0x154), 3u);
  EXPECT_EQ(U32(0x15c), 32u);
  EXPECT_EQ(U64(Symbols + 8), Base + 0x900);
  const auto Text = [&](size_t Index) {
    return llvm::StringRef(
        reinterpret_cast<const char *>(Prepared->data() + Strings + Index));
  };
  EXPECT_EQ(Text(U32(Symbols)), "_target");
  EXPECT_EQ(Text(U32(Symbols + 16)), "_alias");
  EXPECT_EQ(Text(U64(Symbols + 24)), "_target");
  EXPECT_EQ(Text(U32(Symbols + 32)), "_import");
  EXPECT_EQ(Text(U32(U32(0x188))), "_module");
  for (uint32_t I = 0; I < 3; ++I)
    EXPECT_EQ(U32(U32(0x198) + I * 4), I);
  EXPECT_EQ(U32(U32(0x1b8)), 0x300u);
  auto Source = Cache->readImageFileBytes(0, 0x3000, 48);
  ASSERT_TRUE(bool(Source)) << llvm::toString(Source.takeError());
  EXPECT_EQ(readLE<uint32_t>(Source->data()), 0x100u);
  EXPECT_EQ(readLE<uint64_t>(Source->data() + 24), 0x100u);
  const llvm::StringRef Bytes(reinterpret_cast<const char *>(Prepared->data()),
                              Prepared->size());
  auto Object = openMachOBuffer(llvm::MemoryBufferRef(Bytes, "cached-image"));
  ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
  EXPECT_EQ((*Object)->getHeader64().cpusubtype, 2u);
}

TEST(DyldSharedCache,
     ImageProjectionRejectsInvalidOffsetsAndUnsupportedCommands) {
  using Mutation = std::function<void(CacheFixture &)>;
  const std::vector<Mutation> Cases = {
      [](auto &F) { F.template set<uint32_t>(0x754, 0xffffffff); },
      [](auto &F) { F.template set<uint32_t>(0x750, 0x3ff0); },
      [](auto &F) { F.template set<uint32_t>(0x758, 0x3fff); },
      [](auto &F) { F.template set<uint32_t>(0x3000, 0x400); },
      [](auto &F) { F.template set<uint64_t>(0x3018, uint64_t(-1)); },
      [](auto &F) { F.template set<uint32_t>(0x3100, 0x400); },
      [](auto &F) { F.template set<uint32_t>(0x798, 0x3fff); },
      [](auto &F) { F.template set<uint32_t>(0x3200, 0x100); },
      [](auto &F) { F.template set<uint32_t>(0x7bc, 7); },
      [](auto &F) { F.template set<uint32_t>(0x7c0, 2); },
      [](auto &F) { F.template set<uint32_t>(0x7c0, 0x80000035); },
  };
  for (size_t I = 0; I < Cases.size(); ++I) {
    SCOPED_TRACE(I);
    CacheFixture Fixture;
    Fixture.imageProjection();
    Cases[I](Fixture);
    auto Cache = Fixture.parse();
    ASSERT_TRUE(bool(Cache));
    auto Prepared = Cache->prepareImage(0);
    ASSERT_FALSE(bool(Prepared));
    llvm::consumeError(Prepared.takeError());
  }
  CacheFixture Fixture;
  Fixture.imageProjection();
  auto Cache = Fixture.parse();
  ASSERT_TRUE(bool(Cache));
  LoadProgress Progress;
  Progress.Stop = [](void *) { return 1; };
  EXPECT_THROW((void)Cache->prepareImage(0, Progress), LoadCancelled);
}

TEST(DyldSharedCache, ProjectionHandlesCommandOrderAndSectionRelocations) {
  CacheFixture Fixture;
  Fixture.imageProjection();
  std::vector<uint8_t> Symtab(Fixture.Data.begin() + 0x748,
                              Fixture.Data.begin() + 0x760);
  std::memmove(Fixture.Data.data() + 0x748, Fixture.Data.data() + 0x760, 80);
  std::memcpy(Fixture.Data.data() + 0x798, Symtab.data(), Symtab.size());
  Fixture.set<uint32_t>(0x6a0, 0x3240);
  Fixture.set<uint32_t>(0x6a4, 1);
  Fixture.set<uint32_t>(0x3240, 4);
  Fixture.set<uint32_t>(0x3244, 0x06000000);
  auto Cache = Fixture.parse();
  ASSERT_TRUE(bool(Cache));
  auto Prepared = Cache->prepareImage(0);
  ASSERT_TRUE(bool(Prepared)) << llvm::toString(Prepared.takeError());
  const uint32_t Relocations = readLE<uint32_t>(Prepared->data() + 0xa0);
  EXPECT_GE(Relocations, 0x1400u);
  ASSERT_LE(size_t(Relocations) + 8, Prepared->size());
  EXPECT_EQ(readLE<uint32_t>(Prepared->data() + Relocations), 4u);
  EXPECT_EQ(readLE<uint32_t>(Prepared->data() + Relocations + 4), 0x06000000u);
  const llvm::StringRef Bytes(reinterpret_cast<const char *>(Prepared->data()),
                              Prepared->size());
  auto Object = openMachOBuffer(llvm::MemoryBufferRef(Bytes, "command-order"));
  ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
  EXPECT_EQ((*Object)->getSymtabLoadCommand().strsize, 32u);
}

TEST(DyldSharedCache, SelectedImageLoadingRetainsSlideAndInputIdentity) {
  CacheFixture Fixture;
  Fixture.imageProjection();
  // Add a pointer section while keeping the source linkedit offsets fixed.
  std::memmove(Fixture.Data.data() + 0x750, Fixture.Data.data() + 0x700, 0x108);
  std::memset(Fixture.Data.data() + 0x700, 0, 80);
  Fixture.set<uint32_t>(0x614, 568);
  Fixture.set<uint32_t>(0x6bc, 152);
  Fixture.set<uint32_t>(0x6f8, 1);
  std::memcpy(Fixture.Data.data() + 0x700, "__got", 6);
  std::memcpy(Fixture.Data.data() + 0x710, "__DATA", 7);
  Fixture.set<uint64_t>(0x720, Base + 0x2008);
  Fixture.set<uint64_t>(0x728, 24);
  Fixture.set<uint32_t>(0x730, 0x1008);
  Fixture.set<uint32_t>(0x734, 3);
  Fixture.set<uint32_t>(0x740, 6);
  // The isolated __LINKEDIT does not belong to the projected VM address space.
  Fixture.set<uint16_t>(0x441c, 0xffff);
  Fixture.set<uint32_t>(0x900, 0xd65f03c0);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-cache-load", Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  const std::filesystem::path Path =
      std::filesystem::path(Directory.str().str()) / "cache";
  {
    std::ofstream File(Path, std::ios::binary);
    File.write(reinterpret_cast<const char *>(Fixture.Data.data()),
               Fixture.Data.size());
    ASSERT_TRUE(File.good());
  }
  BinaryLoadOptions Options;
  Options.Choice.MachOSharedCacheImage = "/usr/lib/libfixture.dylib";
  Options.OnlyFunctionEntries = {Base + 0x900};
  auto Image = loadBinary(Path, Options);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  ASSERT_TRUE(Image->MachOSharedCache);
  EXPECT_EQ(Image->MachOSharedCacheImagePath,
            Options.Choice.MachOSharedCacheImage);
  EXPECT_EQ(Image->InputFileSHA256, sha256(Fixture.Data));
  ASSERT_EQ(Image->MachOSharedCacheFileSHA256.size(), 1u);
  EXPECT_EQ(Image->MachOSharedCacheFileSHA256[0], Image->InputFileSHA256);
  EXPECT_NE(sha256(Image->Raw), Image->InputFileSHA256);
  EXPECT_EQ(readLE<uint64_t>(Image->readVA(Base + 0x2008, 8)),
            0xab00000000000000ull | (Base + 0x80));
  EXPECT_TRUE(Image->MachOResolvedChainedPointerSlots.count(Base + 0x2008));
  EXPECT_EQ(readLE<uint64_t>(Image->readVA(Base + 0x2018, 8)),
            readLE<uint64_t>(Fixture.Data.data() + 0x1018));
  EXPECT_EQ(Image->MachOAuthenticatedPointers.at(Base + 0x2018).Target,
            Base + 0x700);
  EXPECT_EQ(Image->MachOSharedCacheSymbolPointers.at(Base + 0x2018), "_import");
  EXPECT_FALSE(Image->ImportPtrSlots.count(Base + 0x2018));
  EXPECT_FALSE(Image->ImportStorageSlots.count(Base + 0x2018));
  EXPECT_FALSE(Image->CodePtrRelocSlots.count(Base + 0x2018));
  EXPECT_FALSE(Image->DataPtrRelocSlots.count(Base + 0x2018));
  EXPECT_EQ(readMetadataPointerTarget(*Image, Base + 0x2018), Base + 0x700);
  EXPECT_FALSE(Image->getSegmentFor(Base + 0x4000));
  EXPECT_TRUE(readMetadataBytes(*Image, Base + 0x4000, 8));
  // Optimized cache GOTs retain surviving entries at their original indices.
  for (const char *Name : {"__got", "__auth_got"}) {
    SCOPED_TRACE(Name);
    std::memset(Fixture.Data.data() + 0x700, 0, 16);
    std::memcpy(Fixture.Data.data() + 0x700, Name, std::strlen(Name));
    Fixture.set<uint32_t>(0x740, 0);
    {
      std::ofstream File(Path, std::ios::binary);
      File.write(reinterpret_cast<const char *>(Fixture.Data.data()),
                 Fixture.Data.size());
      ASSERT_TRUE(File.good());
    }
    auto Optimized = loadBinary(Path, Options);
    ASSERT_TRUE(bool(Optimized)) << llvm::toString(Optimized.takeError());
    EXPECT_EQ(Optimized->MachOSharedCacheSymbolPointers.at(Base + 0x2018),
              "_import");
    EXPECT_TRUE(Optimized->ImportPtrSlots.empty());
    EXPECT_TRUE(Optimized->ImportStorageSlots.empty());
  }
  Fixture.set<uint32_t>(0x744, UINT32_MAX);
  {
    std::ofstream File(Path, std::ios::binary);
    File.write(reinterpret_cast<const char *>(Fixture.Data.data()),
               Fixture.Data.size());
    ASSERT_TRUE(File.good());
  }
  auto InvalidIndices = loadBinary(Path, Options);
  ASSERT_FALSE(bool(InvalidIndices));
  llvm::consumeError(InvalidIndices.takeError());
  auto MissingSelection = loadBinary(Path);
  ASSERT_FALSE(bool(MissingSelection));
  EXPECT_NE(llvm::toString(MissingSelection.takeError()).find("--cache-image"),
            std::string::npos);
  Options.Choice.MachOSharedCacheImage = "/absent";
  auto MissingImage = loadBinary(Path, Options);
  ASSERT_FALSE(bool(MissingImage));
  llvm::consumeError(MissingImage.takeError());
  Options.Choice.Format = BinaryFormat::Raw;
  auto WrongChoice = loadBinary(Path, Options);
  ASSERT_FALSE(bool(WrongChoice));
  llvm::consumeError(WrongChoice.takeError());
  Options.Choice.Format = BinaryFormat::Unknown;
  auto Buffer = Fixture.buffer();
  auto BufferLoad = loadBinaryBuffer(Buffer->getMemBufferRef(), Options);
  ASSERT_FALSE(bool(BufferLoad));
  llvm::consumeError(BufferLoad.takeError());
}

TEST(DyldSharedCache, OptionalRealCacheAllImagesHaveValidAnalysisProjections) {
  const char *Path = std::getenv("NEVERD_DYLD_CACHE");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_DYLD_CACHE to an arm64e cache";
  auto Cache = DyldSharedCache::open(Path);
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  size_t BytesTotal = 0;
  for (size_t I = 0; I < Cache->images().size(); ++I) {
    SCOPED_TRACE(Cache->images()[I].Path);
    auto Prepared = Cache->prepareImage(I);
    ASSERT_TRUE(bool(Prepared)) << llvm::toString(Prepared.takeError());
    BytesTotal += Prepared->size();
    const llvm::StringRef Bytes(
        reinterpret_cast<const char *>(Prepared->data()), Prepared->size());
    auto Object =
        openMachOBuffer(llvm::MemoryBufferRef(Bytes, Cache->images()[I].Path));
    ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
  }
  RecordProperty("images", Cache->images().size());
  RecordProperty("total_projection_bytes", BytesTotal);
}

TEST(DyldSharedCache, OptionalRealCacheLoadsFoundationThroughSharedLoader) {
  const char *Path = std::getenv("NEVERD_DYLD_CACHE");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_DYLD_CACHE to a cache containing Foundation";
  BinaryLoadOptions Options;
  Options.Choice.MachOSharedCacheImage =
      "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";
  auto Image = loadBinary(Path, Options);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  EXPECT_FALSE(Image->ObjCClasses.empty());
  EXPECT_FALSE(Image->ObjCMethods.empty());
  for (const auto &Diagnostic : Image->ObjCMetadataDiagnostics)
    ADD_FAILURE() << Diagnostic;
  EXPECT_FALSE(Image->MachOAuthenticatedPointers.empty());
  EXPECT_FALSE(Image->MachOSharedCacheSymbolPointers.empty());
  EXPECT_TRUE(Image->ObjCSourceReferences.empty());
  EXPECT_FALSE(Image->ObjCMetadataReferences.empty());
  size_t Ivars = 0;
  for (const auto &Class : Image->ObjCClasses) {
    EXPECT_EQ(Class.IvarStatus, "metadata_only") << Class.Name;
    Ivars += Class.Ivars.size();
  }
  EXPECT_GT(Ivars, 0u);
  RecordProperty("ivars", Ivars);
  RecordProperty("metadata_references", Image->ObjCMetadataReferences.size());
  EXPECT_EQ(Image->MachOSharedCacheFileSHA256.size(), 2u);
  RecordProperty("methods", Image->ObjCMethods.size());
  RecordProperty("auth_slots", Image->MachOAuthenticatedPointers.size());
  RecordProperty("symbol_slots", Image->MachOSharedCacheSymbolPointers.size());
}

TEST(DyldSharedCache,
     OptionalRealCacheImageProjectionHasBoundedLinkeditStorage) {
  const char *Path = std::getenv("NEVERD_DYLD_CACHE");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_DYLD_CACHE to a cache containing Foundation";
  auto Cache = DyldSharedCache::open(Path);
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  const auto Entry = llvm::find_if(Cache->images(), [](const auto &Image) {
    return llvm::StringRef(Image.Path).ends_with("/Foundation");
  });
  ASSERT_NE(Entry, Cache->images().end());
  auto Prepared = Cache->prepareImage(Entry - Cache->images().begin());
  ASSERT_TRUE(bool(Prepared)) << llvm::toString(Prepared.takeError());
  EXPECT_LT(Prepared->size(), 64u << 20);
  const llvm::StringRef Bytes(reinterpret_cast<const char *>(Prepared->data()),
                              Prepared->size());
  auto Object =
      openMachOBuffer(llvm::MemoryBufferRef(Bytes, "cached-Foundation"));
  ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
  EXPECT_GT((*Object)->getSymtabLoadCommand().nsyms, 0u);
  EXPECT_LT((*Object)->getSymtabLoadCommand().strsize, 16u << 20);
  RecordProperty("projection_bytes", Prepared->size());
  RecordProperty("symbol_string_bytes",
                 (*Object)->getSymtabLoadCommand().strsize);
}

TEST(DyldSharedCache, OptionalRealCachePreservesAllSlideRecipes) {
  const char *Path = std::getenv("NEVERD_DYLD_CACHE");
  if (!Path)
    GTEST_SKIP()
        << "set NEVERD_DYLD_CACHE to an arm64e cache with its subcaches";
  auto Cache = DyldSharedCache::open(Path);
  ASSERT_TRUE(bool(Cache)) << llvm::toString(Cache.takeError());
  ASSERT_FALSE(Cache->images().empty());
  for (size_t I = 0; I < Cache->images().size(); ++I) {
    SCOPED_TRACE(Cache->images()[I].Path);
    auto Layout = Cache->readImageLayout(I);
    ASSERT_TRUE(bool(Layout)) << llvm::toString(Layout.takeError());
    EXPECT_FALSE(Layout->Segments.empty());
  }
  size_t Authenticated = 0;
  size_t Plain = 0;
  for (size_t I = 0; I < Cache->mappings().size(); ++I) {
    auto Fixups = Cache->readSlideFixups(I);
    ASSERT_TRUE(bool(Fixups)) << llvm::toString(Fixups.takeError());
    for (const auto &Fixup : *Fixups) {
      const auto Bytes = Cache->readBytes(Fixup.Slot, 8);
      ASSERT_TRUE(Bytes);
      ASSERT_EQ(readLE<uint64_t>(Bytes->data()), Fixup.EncodedValue);
      if (Fixup.Authentication)
        ++Authenticated;
      else
        ++Plain;
    }
  }
  EXPECT_GT(Authenticated, 0u);
  EXPECT_GT(Plain, 0u);
  RecordProperty("authenticated_recipes", Authenticated);
  RecordProperty("plain_fixups", Plain);
  RecordProperty("images", Cache->images().size());
  RecordProperty("mappings", Cache->mappings().size());
}

TEST(DyldSharedCache,
     OptionalRealCacheObjectiveCMetadataCrossesImageBoundaries) {
  const char *Path = std::getenv("NEVERD_DYLD_CACHE");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_DYLD_CACHE to a cache containing Foundation";
  auto Parsed = DyldSharedCache::open(Path);
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  auto Cache = std::make_shared<DyldSharedCache>(std::move(*Parsed));
  const auto Entry = llvm::find_if(Cache->images(), [](const auto &Image) {
    return llvm::StringRef(Image.Path).ends_with("/Foundation");
  });
  ASSERT_NE(Entry, Cache->images().end());
  auto Layout = Cache->readImageLayout(Entry - Cache->images().begin());
  ASSERT_TRUE(bool(Layout)) << llvm::toString(Layout.takeError());
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.MachOCPUSubtype = 2;
  Image.MachOHasChainedFixups = true;
  Image.MachOSharedCache = Cache;
  Image.Sections = Layout->Sections;
  for (const auto &Mapping : Layout->Segments) {
    if (Mapping.Name == "__LINKEDIT")
      continue;
    Segment Segment;
    Segment.Name = Mapping.Name;
    Segment.VA = Mapping.Address;
    Segment.Size = Mapping.Size;
    Segment.FileOff = Mapping.FileOffset;
    Segment.FileSz = Mapping.FileSize;
    Segment.Flags = machoProtToNd(Mapping.InitialProtection);
    const auto Bytes = Cache->readBytes(Segment.VA, Segment.FileSz);
    ASSERT_TRUE(Bytes);
    Segment.Data.assign(Bytes->begin(), Bytes->end());
    Image.Segments.push_back(std::move(Segment));
  }
  parseObjCMethods(Image);
  EXPECT_GT(Image.ObjCClasses.size(), 0u);
  EXPECT_GT(Image.ObjCMethods.size(), 0u);
  RecordProperty("classes", Image.ObjCClasses.size());
  RecordProperty("methods", Image.ObjCMethods.size());
  RecordProperty("diagnostics", Image.ObjCMetadataDiagnostics.size());
  std::string Messages;
  for (const auto &Message : Image.ObjCMetadataDiagnostics)
    Messages += Message + "\n";
  RecordProperty("diagnostic_messages", Messages);
  EXPECT_TRUE(Image.ObjCMetadataDiagnostics.empty()) << Messages;
  const size_t Named =
      llvm::count_if(Image.ObjCMethods, [](const auto &Method) {
        return !Method.Selector.empty();
      });
  EXPECT_EQ(Named, Image.ObjCMethods.size());
  RecordProperty("named_methods", Named);
  const size_t Conditional =
      llvm::count_if(Image.ObjCMethods, [](const auto &Method) {
        return Method.CacheOrigin.has_value();
      });
  EXPECT_GT(Conditional, 0u);
  RecordProperty("conditional_methods", Conditional);
  RecordProperty("properties", Image.ObjCProperties.size());
  size_t External = 0;
  for (const auto &Method : Image.ObjCMethods) {
    if (Image.isCodeAddress(Method.Implementation))
      continue;
    ++External;
    EXPECT_FALSE(objcMethodHasSourceBody(Method));
  }
  EXPECT_GT(External, 0u);
  RecordProperty("external_methods", External);
}
