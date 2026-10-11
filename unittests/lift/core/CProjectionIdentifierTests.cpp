//===- CProjectionIdentifierTests.cpp - Stable source names --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source identifier collisions must retain their first-free spelling across
/// explicit reservations, canonicalization and independent writer copies.
///
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/CIdentifier.h"
#include "gtest/gtest.h"

#include <utility>

using namespace neverd;

TEST(CProjectionIdentifiers, ExplicitSuffixesKeepTheFirstFreeSpelling) {
  CProjectionIdentifierAllocator Names;
  for (const auto &[Input, Expected] : {
           std::pair{"value_3", "value_3"},
           {"value", "value"},
           {"value", "value_2"},
           {"value", "value_4"},
           {"value_2", "value_2_2"},
           {"value_2_2", "value_2_2_2"},
           {"value", "value_5"},
           {"value_5", "value_5_2"},
       })
    EXPECT_EQ(Names.allocateVerbatim(Input), Expected) << Input;
}

TEST(CProjectionIdentifiers, CanonicalizationSharesTheCollisionDomain) {
  CProjectionIdentifierAllocator Names;
  EXPECT_EQ(Names.allocate("alpha_beta_2"), "alpha_beta_2");
  EXPECT_EQ(Names.allocate("alpha.beta"), "alpha_beta");
  EXPECT_EQ(Names.allocate("alpha-beta"), "alpha_beta_3");
  EXPECT_EQ(Names.allocateVerbatim("alpha_beta"), "alpha_beta_4");
  EXPECT_EQ(Names.allocate("", "alpha_beta"), "alpha_beta_5");
}

TEST(CProjectionIdentifiers, CopiesMovesAndResetKeepIndependentReservations) {
  CProjectionIdentifierAllocator Names;
  EXPECT_EQ(Names.allocateVerbatim("value"), "value");
  EXPECT_EQ(Names.allocateVerbatim("value"), "value_2");
  EXPECT_EQ(Names.allocateVerbatim("value_3"), "value_3");
  auto Copy = Names;
  EXPECT_EQ(Names.allocateVerbatim("value_4"), "value_4");
  EXPECT_EQ(Names.allocateVerbatim("value"), "value_5");
  EXPECT_EQ(Copy.allocateVerbatim("value"), "value_4");
  Copy = Names;
  EXPECT_EQ(Copy.allocateVerbatim("value"), "value_6");
  auto Moved = std::move(Copy);
  EXPECT_EQ(Moved.allocateVerbatim("value"), "value_7");
  EXPECT_EQ(Names.allocateVerbatim("value"), "value_6");
  Names = CProjectionIdentifierAllocator{};
  EXPECT_EQ(Names.allocateVerbatim("value"), "value");
  EXPECT_EQ(Names.allocateVerbatim("value"), "value_2");
}

TEST(CProjectionIdentifiers, DenseCollisionsPreserveEveryReservedName) {
  CProjectionIdentifierAllocator Names;
  for (unsigned I = 2; I <= 1024; I += 2) {
    const std::string Reserved = "nd_store_" + std::to_string(I);
    ASSERT_EQ(Names.allocateVerbatim(Reserved), Reserved);
  }
  EXPECT_EQ(Names.allocateVerbatim("nd_store"), "nd_store");
  for (unsigned I = 1; I <= 2048; ++I) {
    const unsigned Suffix = I < 512 ? 2 * I + 1 : I + 513;
    EXPECT_EQ(Names.allocateVerbatim("nd_store"),
              "nd_store_" + std::to_string(Suffix));
  }
}
