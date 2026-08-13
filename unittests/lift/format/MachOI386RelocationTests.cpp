//===- MachOI386RelocationTests.cpp - Mach-O i386 relocation formula tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "MachOI386RelocationTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::macho_loader::detail;
using namespace neverd::macho_i386_test;

TEST_F(MachOI386Relocation, VanillaAbsoluteSupportsDeclaredWidths) {
  for (uint8_t Width : {uint8_t(1), uint8_t(2), uint8_t(4)}) {
    SCOPED_TRACE(static_cast<unsigned>(Width));
    auto Value = evaluateI386Vanilla({0x1200, 0x34, 0, Width, false});
    ASSERT_TRUE(Value.has_value());
    EXPECT_EQ(*Value, 0x1234);
  }
}

TEST_F(MachOI386Relocation, VanillaPCRelUsesPlaceAndFieldWidth) {
  auto External = evaluateI386Vanilla({0x2200, 7, 0x2000, 4, true});
  auto Local = evaluateI386Vanilla({0x2200, -3, 0x2000, 2, true});
  ASSERT_TRUE(External.has_value());
  ASSERT_TRUE(Local.has_value());
  EXPECT_EQ(*External, 0x203);
  EXPECT_EQ(*Local, 0x1fb);
}

TEST_F(MachOI386Relocation, VanillaRejectsInvalidWidthAndCheckedOverflow) {
  EXPECT_FALSE(evaluateI386Vanilla({1, 2, 0, 8, false}).has_value());
  EXPECT_FALSE(
      evaluateI386Vanilla({std::numeric_limits<int64_t>::max(), 1, 0, 4, false})
          .has_value());
  EXPECT_FALSE(
      evaluateI386Vanilla({0, 0, std::numeric_limits<uint64_t>::max(), 4, true})
          .has_value());
}

TEST_F(MachOI386Relocation, SectionDifferenceNormalizesOriginalSectionBases) {
  auto Value =
      evaluateI386SectionDifference(0x3000, 0x1800, 0x1200, 0x1000, 0x205);
  ASSERT_TRUE(Value.has_value());
  EXPECT_EQ(*Value, 0x1805);
}

TEST_F(MachOI386Relocation, SectionDifferenceUsesCheckedArithmetic) {
  EXPECT_FALSE(evaluateI386SectionDifference(
                   std::numeric_limits<int64_t>::max(), -1, 0, 0, 0)
                   .has_value());
  EXPECT_FALSE(evaluateI386SectionDifference(
                   0, 0, std::numeric_limits<int64_t>::min(), 1, 0)
                   .has_value());
}

TEST_F(MachOI386Relocation, WriterStoresLittleEndianWidths) {
  std::array<uint8_t, 7> Data{};
  EXPECT_TRUE(writeI386RelocationField(Data, 0, 1, 0xab, false));
  EXPECT_TRUE(writeI386RelocationField(Data, 1, 2, 0xcdef, false));
  EXPECT_TRUE(writeI386RelocationField(Data, 3, 4, 0x12345678, false));
  EXPECT_EQ(Data,
            (std::array<uint8_t, 7>{0xab, 0xef, 0xcd, 0x78, 0x56, 0x34, 0x12}));
}

TEST_F(MachOI386Relocation, WriterChecksSignedAndUnsignedRanges) {
  std::array<uint8_t, 4> Data{0xaa, 0xbb, 0xcc, 0xdd};
  EXPECT_TRUE(writeI386RelocationField(Data, 0, 1, -128, true));
  EXPECT_EQ(Data[0], 0x80);
  const auto Before = Data;
  EXPECT_FALSE(writeI386RelocationField(Data, 0, 1, -129, true));
  EXPECT_FALSE(writeI386RelocationField(Data, 0, 1, 128, true));
  EXPECT_FALSE(writeI386RelocationField(Data, 0, 1, -1, false));
  EXPECT_FALSE(writeI386RelocationField(Data, 0, 1, 256, false));
  EXPECT_EQ(Data, Before);
}

TEST_F(MachOI386Relocation, WriterChecksEveryUnsignedWidthBoundary) {
  struct Case {
    uint8_t Width;
    int64_t Max;
  };
  for (const Case C :
       {Case{1, UINT8_MAX}, Case{2, UINT16_MAX}, Case{4, UINT32_MAX}}) {
    SCOPED_TRACE(static_cast<unsigned>(C.Width));
    std::array<uint8_t, 4> Data{0xaa, 0xbb, 0xcc, 0xdd};
    EXPECT_TRUE(writeI386RelocationField(Data, 0, C.Width, 0, false));
    EXPECT_TRUE(writeI386RelocationField(Data, 0, C.Width, C.Max, false));
    const auto Before = Data;
    EXPECT_FALSE(writeI386RelocationField(Data, 0, C.Width, -1, false));
    EXPECT_FALSE(writeI386RelocationField(Data, 0, C.Width, C.Max + 1, false));
    EXPECT_EQ(Data, Before);
  }
}

TEST_F(MachOI386Relocation, WriterChecksEverySignedWidthBoundary) {
  struct Case {
    uint8_t Width;
    int64_t Min;
    int64_t Max;
  };
  for (const Case C :
       {Case{1, INT8_MIN, INT8_MAX}, Case{2, INT16_MIN, INT16_MAX},
        Case{4, INT32_MIN, INT32_MAX}}) {
    SCOPED_TRACE(static_cast<unsigned>(C.Width));
    std::array<uint8_t, 4> Data{0xaa, 0xbb, 0xcc, 0xdd};
    EXPECT_TRUE(writeI386RelocationField(Data, 0, C.Width, C.Min, true));
    EXPECT_TRUE(writeI386RelocationField(Data, 0, C.Width, C.Max, true));
    const auto Before = Data;
    EXPECT_FALSE(writeI386RelocationField(Data, 0, C.Width, C.Min - 1, true));
    EXPECT_FALSE(writeI386RelocationField(Data, 0, C.Width, C.Max + 1, true));
    EXPECT_EQ(Data, Before);
  }
}

TEST_F(MachOI386Relocation, WriterRejectsInvalidWidthAndTruncatedField) {
  std::array<uint8_t, 4> Data{1, 2, 3, 4};
  const auto Before = Data;
  EXPECT_FALSE(writeI386RelocationField(Data, 0, 8, 0, false));
  EXPECT_FALSE(writeI386RelocationField(Data, 2, 4, 0, false));
  EXPECT_EQ(Data, Before);
}

} // namespace
