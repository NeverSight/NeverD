//===- SortedSearchTests.cpp - Ordered integer search contracts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent rank counts, unsigned boundaries, duplicates and guarded tails
/// for each supported SIMD target and the scalar configuration.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/support/SortedSearch.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#if NEVERD_TEST_HIGHWAY
#include "hwy/targets.h"
#endif
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(__linux__) && defined(__aarch64__)
#include <sys/prctl.h>
#endif

namespace {
template <class Test> void checkTargets(Test Check) {
#if NEVERD_TEST_HIGHWAY
  struct Reset {
    ~Reset() { hwy::SetSupportedTargetsForTest(0); }
  } Restore;
  const auto Targets = hwy::SupportedAndGeneratedTargets();
  ASSERT_FALSE(Targets.empty());
  std::string Names;
  for (const auto Target : Targets) {
    if (!Names.empty())
      Names += ',';
    Names += hwy::TargetName(Target);
  }
  ::testing::Test::RecordProperty("simd_targets", Names);
#if defined(__linux__) && defined(__aarch64__)
  const int VectorLength = prctl(PR_SVE_GET_VL);
  if (VectorLength >= 0)
    ::testing::Test::RecordProperty("sve_vector_bytes",
                                    VectorLength & PR_SVE_VL_LEN_MASK);
#endif
  for (const auto Target : Targets) {
    SCOPED_TRACE(hwy::TargetName(Target));
    hwy::SetSupportedTargetsForTest(Target);
    Check();
  }
#else
  ::testing::Test::RecordProperty("simd_targets", "disabled");
  Check();
#endif
}
} // namespace

TEST(SortedSearch, UnsignedRanksDuplicatesAndAlignments) {
  checkTargets([] {
    EXPECT_EQ(neverd::sorted::lowerBound64(nullptr, 0, UINT64_MAX), 0u);
    EXPECT_FALSE(neverd::sorted::contains64(nullptr, 0, 0));
    std::vector<uint64_t> Storage(
        std::max(size_t{1040},
                 neverd::limits::support::kSortedSearchMinimumSIMDEntries + 1));
    for (const uint64_t Base :
         {uint64_t{0}, uint64_t{1} << 63, UINT64_MAX - 1024}) {
      for (size_t Offset = 0; Offset != 8; ++Offset) {
        auto *Data = Storage.data() + Offset;
        for (size_t I = 0; I != 1024; ++I)
          Data[I] = Base + I / 3;
        for (const size_t Size :
             {0u,  1u,  2u,   15u,  16u,  17u,  31u,  32u,  33u,   63u,
              64u, 65u, 127u, 128u, 129u, 255u, 256u, 257u, 1023u, 1024u}) {
          for (const uint64_t Key :
               {uint64_t{0}, UINT64_MAX, Base, Base - 1, Base + 1,
                Base + Size / 6, Base + Size / 3, Base + Size / 3 + 1}) {
            SCOPED_TRACE(::testing::Message()
                         << Base << ' ' << Offset << ' ' << Size << ' ' << Key);
            const size_t Expected = static_cast<size_t>(
                std::count_if(Data, Data + Size,
                              [Key](uint64_t Value) { return Value < Key; }));
            EXPECT_EQ(neverd::sorted::lowerBound64(Data, Size, Key), Expected);
            EXPECT_EQ(neverd::sorted::contains64(Data, Size, Key),
                      std::find(Data, Data + Size, Key) != Data + Size);
          }
        }
      }
    }
    std::fill(Storage.begin(), Storage.end(), UINT64_MAX);
    EXPECT_EQ(neverd::sorted::lowerBound64(Storage.data(), Storage.size(),
                                           UINT64_MAX),
              0u);
    EXPECT_TRUE(
        neverd::sorted::contains64(Storage.data(), Storage.size(), UINT64_MAX));
  });
}

TEST(SortedSearch, ReadOnlyInputEndsAtAnInaccessiblePage) {
#ifdef _WIN32
  SYSTEM_INFO Info{};
  GetSystemInfo(&Info);
  const size_t Page = Info.dwPageSize;
#else
  const long PageSize = sysconf(_SC_PAGESIZE);
  ASSERT_GT(PageSize, 0);
  const size_t Page = static_cast<size_t>(PageSize);
#endif
  constexpr size_t Largest =
      neverd::limits::support::kSortedSearchMinimumSIMDEntries + 257;
  const size_t Bytes = (Largest * sizeof(uint64_t) + Page - 1) / Page * Page;
#ifdef _WIN32
  auto *Memory = static_cast<uint8_t *>(VirtualAlloc(
      nullptr, Bytes + Page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  ASSERT_NE(Memory, nullptr);
  const auto Release = [](uint8_t *At) {
    EXPECT_TRUE(VirtualFree(At, 0, MEM_RELEASE));
  };
  std::unique_ptr<uint8_t, decltype(Release)> Owner(Memory, Release);
  DWORD Old;
  ASSERT_TRUE(VirtualProtect(Memory + Bytes, Page, PAGE_NOACCESS, &Old));
#else
  auto *Memory =
      static_cast<uint8_t *>(mmap(nullptr, Bytes + Page, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANON, -1, 0));
  ASSERT_NE(Memory, MAP_FAILED);
  const auto Release = [Bytes, Page](uint8_t *At) {
    EXPECT_EQ(munmap(At, Bytes + Page), 0);
  };
  std::unique_ptr<uint8_t, decltype(Release)> Owner(Memory, Release);
  ASSERT_EQ(mprotect(Memory + Bytes, Page, PROT_NONE), 0);
#endif
  auto *Words = reinterpret_cast<uint64_t *>(Memory);
  const size_t Count = Bytes / sizeof(uint64_t);
  for (size_t I = 0; I != Count; ++I)
    std::construct_at(Words + I, static_cast<uint64_t>(I));
#ifdef _WIN32
  ASSERT_TRUE(VirtualProtect(Memory, Bytes, PAGE_READONLY, &Old));
#else
  ASSERT_EQ(mprotect(Memory, Bytes, PROT_READ), 0);
#endif
  checkTargets([&] {
    for (size_t Size = 0; Size <= Largest; ++Size) {
      const auto *At = Words + Count - Size;
      EXPECT_EQ(neverd::sorted::lowerBound64(At, Size, UINT64_MAX), Size);
      EXPECT_EQ(neverd::sorted::lowerBound64(At, Size, 0), 0u);
      EXPECT_FALSE(neverd::sorted::contains64(At, Size, UINT64_MAX));
      EXPECT_EQ(neverd::sorted::contains64(At, Size, 0),
                Size != 0 && At[0] == 0);
      if (Size != 0) {
        EXPECT_EQ(neverd::sorted::lowerBound64(At, Size, At[0]), 0u);
        EXPECT_EQ(neverd::sorted::lowerBound64(At, Size, At[Size - 1]),
                  Size - 1);
        EXPECT_TRUE(neverd::sorted::contains64(At, Size, At[0]));
        EXPECT_TRUE(neverd::sorted::contains64(At, Size, At[Size - 1]));
      }
    }
  });
}

TEST(SortedSearch, EveryInsertionPointAcrossNarrowedTails) {
  checkTargets([] {
    std::vector<uint64_t> Keys(257);
    for (const size_t Repeats : {1u, 3u}) {
      for (size_t I = 0; I != Keys.size(); ++I)
        Keys[I] = (uint64_t{1} << 63) + 2 * (I / Repeats);
      for (size_t Size = 0; Size <= Keys.size(); ++Size) {
        for (size_t Offset = 0; Offset <= 2 * Size + 1; ++Offset) {
          const uint64_t Key = (uint64_t{1} << 63) + Offset - 1;
          const size_t Expected = static_cast<size_t>(
              std::count_if(Keys.begin(), Keys.begin() + Size,
                            [Key](uint64_t Value) { return Value < Key; }));
          ASSERT_EQ(neverd::sorted::lowerBound64(Keys.data(), Size, Key),
                    Expected)
              << Size << ' ' << Offset << ' ' << Repeats;
          ASSERT_EQ(neverd::sorted::contains64(Keys.data(), Size, Key),
                    std::find(Keys.begin(), Keys.begin() + Size, Key) !=
                        Keys.begin() + Size)
              << Size << ' ' << Offset << ' ' << Repeats;
        }
      }
    }
    const size_t Minimum =
        neverd::limits::support::kSortedSearchMinimumSIMDEntries;
    for (const size_t Size :
         {Minimum - 1, Minimum, Minimum + 1, 2 * Minimum + 1}) {
      Keys.resize(Size);
      for (const size_t Repeats : {1u, 3u}) {
        for (size_t I = 0; I != Size; ++I)
          Keys[I] = (uint64_t{1} << 63) + 2 * (I / Repeats);
        EXPECT_EQ(neverd::sorted::lowerBound64(Keys.data(), Size, 0), 0u);
        EXPECT_EQ(neverd::sorted::lowerBound64(Keys.data(), Size, UINT64_MAX),
                  Size);
        for (size_t I = 0; I != Size; ++I) {
          const size_t First = I - I % Repeats;
          EXPECT_TRUE(neverd::sorted::contains64(Keys.data(), Size, Keys[I]));
          EXPECT_FALSE(
              neverd::sorted::contains64(Keys.data(), Size, Keys[I] + 1));
          EXPECT_EQ(neverd::sorted::lowerBound64(Keys.data(), Size, Keys[I]),
                    First);
          EXPECT_EQ(
              neverd::sorted::lowerBound64(Keys.data(), Size, Keys[I] + 1),
              std::min(Size, First + Repeats));
        }
      }
    }
  });
}
