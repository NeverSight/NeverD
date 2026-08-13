//===- GoRuntimeStackMapTests.cpp - Go pointer map tests --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "GoRuntimeEHTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::go_loader;
using namespace neverd::go_eh_test;

//===----------------------------------------------------------------------===//
// Stack maps
//===----------------------------------------------------------------------===//

TEST(GoStackMaps, DecodesBothPointerMapsAndTheirBitmaps) {
  GoTestImage T;
  const va_t ArgsVA = T.addPayload(buildStackMap(3, {{0b101}, {0b010}}));
  const va_t LocalsVA =
      T.addPayload(buildStackMap(9, {{0x01, 0x01}, {0xFF, 0x00}}));

  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable StackMapIndex;
  StackMapIndex.Steps = {{0, 0x40}, {1, 0x40}};
  Work.PCData = {std::nullopt, StackMapIndex};
  Work.FuncData = {ArgsVA, LocalsVA};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  // From Go 1.16 the position is fixed by the magic and nothing is probed.
  ASSERT_TRUE(Info.GoModule->StackMapPCDataIndex.has_value());
  EXPECT_EQ(*Info.GoModule->StackMapPCDataIndex, 1u);

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Complete);

  ASSERT_TRUE(F->Go->ArgsPointerMap.has_value());
  EXPECT_EQ(F->Go->ArgsPointerMap->RecordVA, ArgsVA);
  EXPECT_EQ(F->Go->ArgsPointerMap->BitCount, 3u);
  ASSERT_EQ(F->Go->ArgsPointerMap->Bitmaps.size(), 2u);
  const GoStackMapBitmap &Args0 = F->Go->ArgsPointerMap->Bitmaps[0];
  EXPECT_EQ(Args0.Index, 0u);
  EXPECT_TRUE(Args0.isPointerSlot(0));
  EXPECT_FALSE(Args0.isPointerSlot(1));
  EXPECT_TRUE(Args0.isPointerSlot(2));
  // A slot past `nbit` is not described by the map, so it is not a pointer.
  EXPECT_FALSE(Args0.isPointerSlot(3));
  EXPECT_FALSE(F->Go->ArgsPointerMap->Bitmaps[1].isPointerSlot(0));
  EXPECT_TRUE(F->Go->ArgsPointerMap->Bitmaps[1].isPointerSlot(1));

  ASSERT_TRUE(F->Go->LocalsPointerMap.has_value());
  EXPECT_EQ(F->Go->LocalsPointerMap->BitCount, 9u);
  ASSERT_EQ(F->Go->LocalsPointerMap->Bitmaps.size(), 2u);
  // Nine bits occupy two bytes, and the ninth lives in the low bit of the
  // second one.
  EXPECT_EQ(F->Go->LocalsPointerMap->Bitmaps[0].Bits.size(), 2u);
  EXPECT_TRUE(F->Go->LocalsPointerMap->Bitmaps[0].isPointerSlot(8));
  EXPECT_FALSE(F->Go->LocalsPointerMap->Bitmaps[1].isPointerSlot(8));
}

TEST(GoStackMaps, CountsTheBitmapsOfAZeroBitMap) {
  GoTestImage T;
  // The record the linker shares between every function whose argument area
  // holds no pointer: one bitmap, zero bits wide, and so no bytes at all.
  const va_t ArgsVA = T.addPayload(buildStackMap(0, {{}}));
  const va_t LocalsVA = T.addPayload(buildStackMap(4, {{0b0101}}));

  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable StackMapIndex;
  StackMapIndex.Steps = {{0, 0x40}};
  Work.PCData = {std::nullopt, StackMapIndex};
  Work.FuncData = {ArgsVA, LocalsVA};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F =
      findRecord(T.Img.ExceptionMetadata, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Complete);

  // The map declares its bitmap even though it spans no bytes, so the index
  // that names it stays satisfiable and the table it selects into is kept.
  ASSERT_TRUE(F->Go->ArgsPointerMap.has_value());
  EXPECT_EQ(F->Go->ArgsPointerMap->BitCount, 0u);
  ASSERT_EQ(F->Go->ArgsPointerMap->Bitmaps.size(), 1u);
  EXPECT_TRUE(F->Go->ArgsPointerMap->Bitmaps[0].Bits.empty());
  EXPECT_FALSE(F->Go->ArgsPointerMap->Bitmaps[0].isPointerSlot(0));
  ASSERT_EQ(F->Go->StackMapRanges.size(), 1u);
  EXPECT_EQ(F->Go->StackMapRanges[0].Index, 0);
}

TEST(GoStackMaps, StartsEachBitmapOnItsOwnByteBoundary) {
  GoTestImage T;
  // A bit count that is an exact multiple of eight is where a stride computed
  // one byte out stops being harmless, because the rounding that hides the
  // error for every other width contributes nothing here.
  const va_t ArgsVA = T.addPayload(buildStackMap(8, {{0x01}, {0x80}, {0x24}}));
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  Work.FuncData = {ArgsVA};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->Go->ArgsPointerMap.has_value());
  const std::vector<GoStackMapBitmap> &Bitmaps =
      F->Go->ArgsPointerMap->Bitmaps;
  ASSERT_EQ(Bitmaps.size(), 3u);
  for (const GoStackMapBitmap &Bitmap : Bitmaps)
    EXPECT_EQ(Bitmap.Bits.size(), 1u);
  EXPECT_EQ(Bitmaps[0].Bits[0], 0x01);
  EXPECT_EQ(Bitmaps[1].Bits[0], 0x80);
  EXPECT_EQ(Bitmaps[2].Bits[0], 0x24);
  EXPECT_TRUE(Bitmaps[1].isPointerSlot(7));
  EXPECT_TRUE(Bitmaps[2].isPointerSlot(2));
  EXPECT_TRUE(Bitmaps[2].isPointerSlot(5));
  EXPECT_FALSE(Bitmaps[2].isPointerSlot(3));
}

TEST(GoStackMaps, TiesEachBitmapToThePCRangeThatSelectsIt) {
  GoTestImage T;
  const va_t ArgsVA = T.addPayload(buildStackMap(1, {{0b1}, {0b0}, {0b1}}));
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable StackMapIndex;
  StackMapIndex.Steps = {{-1, 0x08}, {0, 0x18}, {2, 0x20}};
  Work.PCData = {std::nullopt, StackMapIndex};
  Work.FuncData = {ArgsVA};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_EQ(F->Go->StackMapRanges.size(), 3u);
  // The unset value is kept as the table spelled it rather than resolved to
  // index zero the way the runtime's prologue fallback would.
  EXPECT_EQ(F->Go->StackMapRanges[0].Index, -1);
  EXPECT_EQ(F->Go->StackMapRanges[0].Range.Begin, kTextVA + 0x100);
  EXPECT_EQ(F->Go->StackMapRanges[0].Range.End, kTextVA + 0x108);
  EXPECT_EQ(F->Go->StackMapRanges[1].Index, 0);
  EXPECT_EQ(F->Go->StackMapRanges[1].Range.End, kTextVA + 0x120);
  EXPECT_EQ(F->Go->StackMapRanges[2].Index, 2);
  EXPECT_EQ(F->Go->StackMapRanges[2].Range.End, kTextVA + 0x140);
}

TEST(GoStackMaps, RefusesAMapThatClaimsMoreBitmapsThanAFrameCanHave) {
  GoTestImage T;
  std::vector<uint8_t> Oversized = buildStackMap(8, {{0xFF}});
  // `n` alone decides how much the decoder would read, so it is the field a
  // funcdata pointer resolved through the wrong base most easily turns into a
  // large allocation.
  std::memcpy(Oversized.data(), "\xFF\xFF\xFF\x7F", 4);
  const va_t ArgsVA = T.addPayload(Oversized);

  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  Work.FuncData = {ArgsVA};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_FALSE(F->Go->ArgsPointerMap.has_value());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_TRUE(
      anyDiagnosticContains(F->Diagnostics, "not a readable stackmap"));
}

TEST(GoStackMaps, RefusesAMapWhoseBitmapsAreNotFullyMapped) {
  GoTestImage T;
  // Three bitmaps are declared but only the first one's bytes exist.
  std::vector<uint8_t> Truncated = buildStackMap(8, {{0xFF}});
  std::memcpy(Truncated.data(), "\x03\x00\x00\x00", 4);
  const va_t ArgsVA = T.addPayload(Truncated);
  // Shrink the data segment so the missing bitmap bytes are off the end.
  T.Img.Segments[1].Size = static_cast<size_t>(ArgsVA + Truncated.size() -
                                               kDataVA);
  T.Img.Segments[1].Data.resize(T.Img.Segments[1].Size);

  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  Work.FuncData = {ArgsVA};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_FALSE(F->Go->ArgsPointerMap.has_value());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Partial);
}

TEST(GoStackMaps, DropsRangesThatSelectABitmapTheFunctionDoesNotHave) {
  GoTestImage T;
  const va_t ArgsVA = T.addPayload(buildStackMap(4, {{0b1010}}));
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable StackMapIndex;
  StackMapIndex.Steps = {{0, 0x10}, {3, 0x10}};
  Work.PCData = {std::nullopt, StackMapIndex};
  Work.FuncData = {ArgsVA};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  // The map itself is sound and stays; only the selection into it is dropped.
  EXPECT_TRUE(F->Go->ArgsPointerMap.has_value());
  EXPECT_TRUE(F->Go->StackMapRanges.empty());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_TRUE(anyDiagnosticContains(F->Diagnostics, "names no bitmap"));
}

TEST(GoStackMaps, AcceptsAMapWithNoBitsAtAll) {
  GoTestImage T;
  const va_t ArgsVA = T.addPayload(buildStackMap(0, {}));
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  Work.FuncData = {ArgsVA};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->Go->ArgsPointerMap.has_value());
  EXPECT_EQ(F->Go->ArgsPointerMap->BitCount, 0u);
  EXPECT_TRUE(F->Go->ArgsPointerMap->Bitmaps.empty());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Complete);
}

} // namespace
