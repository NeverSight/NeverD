//===- GoRuntimeEHTests.cpp - Go pc-value table tests -----------------===//
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
// pc-value decoding
//===----------------------------------------------------------------------===//

TEST(GoPCValue, DecodesUnsafePointRangesWithEveryDefinedKind) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable UnsafePoints;
  UnsafePoints.Steps = {{-1, 0x10}, {-2, 0x08}, {-3, 0x04},
                        {-4, 0x04}, {-5, 0x04}, {7, 0x08}};
  Work.PCData = {UnsafePoints};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  const std::vector<GoUnsafePointRange> &Ranges = F->Go->UnsafePointRanges;
  ASSERT_EQ(Ranges.size(), 6u);

  EXPECT_EQ(Ranges[0].Range.Begin, kTextVA + 0x100);
  EXPECT_EQ(Ranges[0].Range.End, kTextVA + 0x110);
  EXPECT_EQ(Ranges[0].Kind, GoUnsafePointKind::Safe);
  EXPECT_EQ(Ranges[0].NativeValue, -1);

  EXPECT_EQ(Ranges[1].Range.Begin, kTextVA + 0x110);
  EXPECT_EQ(Ranges[1].Range.End, kTextVA + 0x118);
  EXPECT_EQ(Ranges[1].Kind, GoUnsafePointKind::Unsafe);

  // Both restart spellings normalize together, and only the native value
  // still tells them apart.
  EXPECT_EQ(Ranges[2].Kind, GoUnsafePointKind::RestartSequence);
  EXPECT_EQ(Ranges[2].NativeValue, -3);
  EXPECT_EQ(Ranges[3].Kind, GoUnsafePointKind::RestartSequence);
  EXPECT_EQ(Ranges[3].NativeValue, -4);

  EXPECT_EQ(Ranges[4].Kind, GoUnsafePointKind::RestartAtEntry);
  EXPECT_EQ(Ranges[5].Kind, GoUnsafePointKind::Unknown);
  EXPECT_EQ(Ranges[5].NativeValue, 7);
  EXPECT_EQ(Ranges[5].Range.End, kTextVA + 0x12c);
}

TEST(GoPCValue, ScalesProgramCounterDeltasByMinLC) {
  GoTestImage T(Arch::AArch64);
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable UnsafePoints;
  UnsafePoints.Steps = {{-1, 4}, {-2, 2}};
  Work.PCData = {UnsafePoints};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200, /*MinLC=*/4).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_EQ(F->Go->UnsafePointRanges.size(), 2u);
  EXPECT_EQ(F->Go->UnsafePointRanges[0].Range.End, kTextVA + 0x110);
  EXPECT_EQ(F->Go->UnsafePointRanges[1].Range.End, kTextVA + 0x118);
}

TEST(GoPCValue, ReportsATableThatRunsOffTheImage) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable UnsafePoints;
  UnsafePoints.Steps = {{-2, 0x10}};
  Work.PCData = {UnsafePoints};
  BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);
  // Point the table past everything the image maps.  The offset is still a
  // number the record could hold, so only the read can reject it.
  Tab.put32(Tab.PCDataArrayOffsets[0], 0x00F00000);
  T.installPclnTab(Tab.Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->UnsafePointRanges.empty());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_TRUE(anyDiagnosticContains(F->Diagnostics,
                                    "not a readable pc-value table"));
}

TEST(GoPCValue, RejectsAValueDeltaThatOverflowsTheAccumulator) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  // Two maximal positive deltas in a row take the running value past what an
  // int32 holds, which no real table does and which must not wrap.
  ByteBuilder Raw;
  for (unsigned I = 0; I < 2; ++I) {
    Raw.varint(0xFFFFFFFEu); // zigzag(INT32_MAX)
    Raw.varint(4);
  }
  Raw.u8(0);
  PCValueTable UnsafePoints;
  UnsafePoints.RawBytes = Raw.data();
  Work.PCData = {UnsafePoints};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->UnsafePointRanges.empty());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Partial);
}

TEST(GoPCValue, RejectsAnUnterminatedVarint) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable UnsafePoints;
  UnsafePoints.RawBytes = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00};
  Work.PCData = {UnsafePoints};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->UnsafePointRanges.empty());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Partial);
}

TEST(GoPCValue, LeavesUnsafePointsEmptyWhenTheRecordDeclaresNoTable) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  Work.PCData = {std::nullopt};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->UnsafePointRanges.empty());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Complete);
}

} // namespace
