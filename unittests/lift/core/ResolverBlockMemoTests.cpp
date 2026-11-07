//===- ResolverBlockMemoTests.cpp - Stable recursive query storage --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "jumptable/ResolverBlockMemo.h"

#include <cstdint>
#include <limits>
#include <tuple>

using neverd::detail::ResolverBlockMemo;

namespace {
struct TrackedValue {
  int Value;
  int &Alive;
  int &Constructed;
  TrackedValue(int Value, int &Alive, int &Constructed)
      : Value(Value), Alive(Alive), Constructed(Constructed) {
    ++Alive;
    ++Constructed;
  }
  ~TrackedValue() { --Alive; }
};

struct CollidingValue {
  int Value;
  bool operator==(const CollidingValue &) const = default;
};
llvm::hash_code hash_value(const CollidingValue &) {
  return llvm::hash_code(0);
}
} // namespace

TEST(ResolverBlockMemo, RecursiveGrowthKeepsReferencesAndOwnsEveryValue) {
  using Key = std::tuple<int, int>;
  int Alive = 0, Constructed = 0;
  {
    ResolverBlockMemo<Key, TrackedValue> Memo;
    const Key First{0, 0};
    const auto [Initial, Inserted] =
        Memo.try_emplace(First, 17, Alive, Constructed);
    ASSERT_TRUE(Inserted);
    auto *Address = &Initial->second;
    const auto [Duplicate, NewDuplicate] =
        Memo.try_emplace(First, 99, Alive, Constructed);
    ASSERT_FALSE(NewDuplicate);
    EXPECT_TRUE(Duplicate == Initial);
    EXPECT_EQ(Duplicate->second.Value, 17);
    EXPECT_EQ(Constructed, 1);

    constexpr int Count = 20000;
    for (int I = 1; I <= Count; ++I) {
      const auto [Nested, New] =
          Memo.try_emplace(Key{I, I}, I, Alive, Constructed);
      ASSERT_TRUE(New);
      EXPECT_EQ(Nested->second.Value, I);
      // A caller retains its original memo entry across recursive insertions.
      ASSERT_EQ(&Initial->second, Address);
      EXPECT_EQ(Initial->second.Value, 17);
    }
    EXPECT_EQ(Memo.size(), Count + 1u);
    EXPECT_EQ(Alive, Count + 1);
    EXPECT_EQ(Memo.erase(First), 1u);
    EXPECT_EQ(Memo.erase(First), 0u);
    EXPECT_EQ(Alive, Count);
    Memo.clear();
    EXPECT_EQ(Memo.size(), 0u);
    EXPECT_EQ(Alive, 0);
    ASSERT_TRUE(Memo.try_emplace(First, 23, Alive, Constructed).second);
    EXPECT_EQ(Memo.find(First)->second.Value, 23);
  }
  EXPECT_EQ(Alive, 0);
}

TEST(ResolverBlockMemo, FullWidthCoordinatesRemainDistinct) {
  using Key = std::tuple<int, int, uint8_t, uint64_t, uint16_t, bool, bool>;
  ResolverBlockMemo<Key, int> Memo;
  int Next = 0;
  for (int Block : {std::numeric_limits<int>::min(),
                    std::numeric_limits<int>::max(), -1, -2})
    for (int Seq : {std::numeric_limits<int>::min(),
                    std::numeric_limits<int>::max(), -1, -2})
      for (uint64_t Offset : {uint64_t{0}, UINT64_MAX, UINT64_MAX - 1})
        for (bool First : {false, true})
          for (bool Second : {false, true}) {
            const Key K{Block,      Seq,   UINT8_MAX, Offset,
                        UINT16_MAX, First, Second};
            ASSERT_TRUE(Memo.try_emplace(K, Next++).second);
          }
  EXPECT_EQ(Memo.size(), static_cast<size_t>(Next));
  int Expected = 0;
  for (int Block : {std::numeric_limits<int>::min(),
                    std::numeric_limits<int>::max(), -1, -2})
    for (int Seq : {std::numeric_limits<int>::min(),
                    std::numeric_limits<int>::max(), -1, -2})
      for (uint64_t Offset : {uint64_t{0}, UINT64_MAX, UINT64_MAX - 1})
        for (bool First : {false, true})
          for (bool Second : {false, true}) {
            const Key K{Block,      Seq,   UINT8_MAX, Offset,
                        UINT16_MAX, First, Second};
            const auto Found = Memo.find(K);
            ASSERT_FALSE(Found == Memo.end());
            EXPECT_EQ(Found->second, Expected++);
            Memo.erase(Found);
            EXPECT_TRUE(Memo.find(K) == Memo.end());
          }
  EXPECT_EQ(Memo.size(), 0u);
}

TEST(ResolverBlockMemo, CollisionsCannotAliasOrEraseOtherQueries) {
  using Key = std::tuple<int, CollidingValue>;
  ResolverBlockMemo<Key, int> Memo;
  constexpr int Count = 256;
  for (int I = 0; I < Count; ++I)
    ASSERT_TRUE(Memo.try_emplace(Key{0, {I}}, I).second);
  for (int I = 0; I < Count; ++I) {
    const auto Found = Memo.find(Key{0, {I}});
    ASSERT_FALSE(Found == Memo.end());
    EXPECT_EQ(Found->second, I);
  }
  for (int I = 0; I < Count; I += 2)
    ASSERT_EQ(Memo.erase(Key{0, {I}}), 1u);
  for (int I = 1; I < Count; I += 2) {
    const auto Found = Memo.find(Key{0, {I}});
    ASSERT_FALSE(Found == Memo.end());
    EXPECT_EQ(Found->second, I);
  }
  EXPECT_EQ(Memo.size(), Count / 2u);
}
