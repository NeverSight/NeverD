//===- SupportSharedValueTests.cpp - Copy-on-write values -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SharedValue copies share one instance until one of them changes, and a
/// value shared from elsewhere is never changed through its holders.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/support/SharedValue.h"

#include <map>
#include <memory>

namespace {
using neverd::SharedValue;
using Map = std::map<int, int>;

TEST(SharedValue, CopiesShareUntilOneChanges) {
  SharedValue<Map> First = Map{{1, 10}, {2, 20}};
  SharedValue<Map> Second = First;
  EXPECT_EQ(&First.get(), &Second.get());
  Second[3] = 30;
  EXPECT_NE(&First.get(), &Second.get());
  EXPECT_EQ(First.get(), (Map{{1, 10}, {2, 20}}));
  EXPECT_EQ(Second.get(), (Map{{1, 10}, {2, 20}, {3, 30}}));
  // A value no other holds changes in place.
  const Map *Own = &Second.get();
  Second.erase(1);
  EXPECT_EQ(&Second.get(), Own);
  EXPECT_EQ(Second.count(1), 0u);
}

TEST(SharedValue, ASharedInstanceIsNeverChangedThroughAHolder) {
  const auto Table = std::make_shared<const Map>(Map{{5, 50}});
  SharedValue<Map> Run;
  Run.share(Table);
  EXPECT_EQ(&Run.get(), Table.get());
  EXPECT_EQ(Run.at(5), 50);
  Run.try_emplace(6, 60);
  Run.clear();
  EXPECT_EQ(*Table, (Map{{5, 50}}));
  EXPECT_TRUE(Run.empty());
  // A value handed out to share is copied before its holder changes it too.
  SharedValue<Map> Owner = Map{{7, 70}};
  const auto Handed = Owner.shared();
  Owner.emplace(8, 80);
  EXPECT_EQ(*Handed, (Map{{7, 70}}));
  EXPECT_EQ(Owner.size(), 2u);
}

TEST(SharedValue, AnEmptyValueReadsEmptyAndEqualityComparesContents) {
  const SharedValue<Map> None;
  EXPECT_TRUE(None.empty());
  EXPECT_EQ(None.find(1), None.end());
  SharedValue<Map> One = Map{{1, 1}};
  SharedValue<Map> Same = Map{{1, 1}};
  EXPECT_TRUE(One == Same);
  SharedValue<Map> Copy = One;
  EXPECT_TRUE(Copy == One);
  Copy[1] = 2;
  EXPECT_FALSE(Copy == One);
  EXPECT_TRUE(SharedValue<Map>() == SharedValue<Map>(Map{}));
}
} // namespace
