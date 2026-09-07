//===- GoRuntimeMalformedTests.cpp - Go malformed pclntab record tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "GoRuntimeEHTestsDetail.h"
#include "gtest/gtest.h"

namespace {

using namespace neverd;
using namespace neverd::go_loader;
using namespace neverd::go_eh_test;

//===----------------------------------------------------------------------===//
// Malformed records
//===----------------------------------------------------------------------===//

TEST(GoMalformedRecords, RejectsARecordDeclaringMorePCDataTablesThanExist) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);
  // `npcdata` sizes the array the funcdata pointers sit behind, so a count
  // this large would walk the decoder off the end of the record.
  Tab.put32(Tab.RecordOffsets[0] + 32, 1000);
  T.installPclnTab(Tab.Bytes);

  parseGoExceptions(T.Img);
  EXPECT_FALSE(T.Img.ExceptionMetadata.GoModule.has_value());
}

TEST(GoMalformedRecords, RejectsARecordDeclaringMoreFuncDataThanExist) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);
  Tab.put8(Tab.RecordOffsets[0] + 43, 200);
  T.installPclnTab(Tab.Bytes);

  parseGoExceptions(T.Img);
  EXPECT_FALSE(T.Img.ExceptionMetadata.GoModule.has_value());
}

TEST(GoMalformedRecords, RejectsAHeaderWithAnImpossiblePCQuantum) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);
  Tab.put8(6, 3);
  T.installPclnTab(Tab.Bytes);

  EXPECT_FALSE(hasGoRuntimeMetadata(T.Img));
}

TEST(GoMalformedRecords, RejectsAHeaderWhosePointerSizeContradictsTheImage) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);
  Tab.put8(7, 4);
  T.installPclnTab(Tab.Bytes);

  EXPECT_FALSE(hasGoRuntimeMetadata(T.Img));
}

TEST(GoMalformedRecords, RejectsAFunctionNameAddressThatWraps) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);

  constexpr va_t WrappedNameBase = InvalidVA - 3;
  Tab.put64(24, WrappedNameBase - kPclnVA);
  Tab.put32(Tab.RecordOffsets[0] + 8, 8);
  T.installPclnTab(Tab.Bytes);

  Segment High;
  High.Name = ".high";
  High.VA = WrappedNameBase;
  High.Size = 3;
  High.Flags = SegmentFlags::Readable;
  High.Data.assign(3, 'x');
  T.Img.Segments.push_back(std::move(High));

  Segment Low;
  Low.Name = ".low";
  Low.VA = 4;
  Low.Size = 8;
  Low.Flags = SegmentFlags::Readable;
  Low.Data = {'w', 'r', 'a', 'p', 'p', 'e', 'd', 0};
  T.Img.Segments.push_back(std::move(Low));

  parseGoExceptions(T.Img);

  const ExceptionFunction *F =
      findRecord(T.Img.ExceptionMetadata, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->Name.empty());
}

TEST(GoMalformedRecords, LeavesAnImageWithNoPclnTabAlone) {
  GoTestImage T;
  parseGoExceptions(T.Img);
  EXPECT_FALSE(T.Img.ExceptionMetadata.GoModule.has_value());
  EXPECT_TRUE(T.Img.ExceptionMetadata.Diagnostics.empty());
  EXPECT_EQ(T.Img.ExceptionMetadata.ParseStatus,
            ExceptionParseStatus::Complete);
}

} // namespace
