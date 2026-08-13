//===- GoRuntimeMalformedTests.cpp - Go malformed pclntab record tests -===//
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

TEST(GoMalformedRecords, LeavesAnImageWithNoPclnTabAlone) {
  GoTestImage T;
  parseGoExceptions(T.Img);
  EXPECT_FALSE(T.Img.ExceptionMetadata.GoModule.has_value());
  EXPECT_TRUE(T.Img.ExceptionMetadata.Diagnostics.empty());
  EXPECT_EQ(T.Img.ExceptionMetadata.ParseStatus,
            ExceptionParseStatus::Complete);
}

} // namespace
