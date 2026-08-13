//===- GoRuntimePclnTabTests.cpp - Go 1.2 function table tests --------===//
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
// Go 1.2 layout
//===----------------------------------------------------------------------===//

TEST(GoLegacyPclnTab, ReadsTheGo12FunctionTable) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  Work.FuncData = {std::nullopt, std::nullopt};
  GoFuncSpec Other = makeDeferringFunc("main.other", kTextVA + 0x200);
  Other.DeferReturn = 0;
  Other.FuncData = {std::nullopt, std::nullopt};
  T.installPclnTab(
      buildPclnTab(kGo12Magic, {Work, Other}, kTextVA + 0x300).Bytes);

  EXPECT_TRUE(hasGoRuntimeMetadata(T.Img));
  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_EQ(Info.GoModule->PclnTabMagic, kGo12Magic);
  EXPECT_EQ(Info.GoModule->PclnTabVersion, "go1.2");
  EXPECT_EQ(Info.GoModule->FunctionCount, 2u);
  EXPECT_EQ(Info.GoModule->PcHeaderVA, kPclnVA);
  // Names, pc-value tables, and `_func` records all live at offsets from the
  // head of the table, so all three bases are the header itself.
  EXPECT_EQ(Info.GoModule->FuncNameTabVA, kPclnVA);
  EXPECT_EQ(Info.GoModule->PcTabVA, kPclnVA);
  EXPECT_EQ(Info.GoModule->FuncTabVA, kPclnVA + 16);
  EXPECT_FALSE(Info.GoModule->UsesPreGo112FuncLayout);

  // The table is a symbol table too, and on this layout the entries it names
  // are absolute addresses rather than offsets from a base.
  bool NamedWork = false;
  for (const Symbol &S : T.Img.Symbols)
    if (S.Name == "main.work") {
      NamedWork = true;
      EXPECT_EQ(S.Addr, kTextVA + 0x100);
      EXPECT_EQ(S.Size, 0x100u);
    }
  EXPECT_TRUE(NamedWork);

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_EQ(F->CodeRange.Begin, kTextVA + 0x100);
  EXPECT_EQ(F->CodeRange.End, kTextVA + 0x200);
  ASSERT_TRUE(F->Go->DeferReturnOffset.has_value());
  EXPECT_EQ(*F->Go->DeferReturnOffset, 0x20u);
  ASSERT_TRUE(F->Go->FrameSize.has_value());
  EXPECT_EQ(*F->Go->FrameSize, 0x30);
  // The sentinel closes the last function, which is what gives `main.other`
  // an end at all.
  EXPECT_EQ(findRecord(Info, kTextVA + 0x200), nullptr);
}

TEST(GoLegacyPclnTab, ReportsThatTheUnsafePointTableDoesNotExistYet) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  // Table 0 is a register map index on this layout, never an unsafe point
  // table, so decoding it as one would report async-preemption facts the
  // image never stated.
  PCValueTable RegMapIndex;
  RegMapIndex.Steps = {{-1, 0x40}, {0, 0x40}};
  Work.PCData = {RegMapIndex};
  T.installPclnTab(
      buildPclnTab(kGo12Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  EXPECT_TRUE(anyDiagnosticContains(Info.Diagnostics, "PCDATA_UnsafePoint"));
  EXPECT_EQ(Info.ParseStatus, ExceptionParseStatus::Partial);
  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->UnsafePointRanges.empty());
}

TEST(GoLegacyPclnTab, ReadsThePreGo112RecordShape) {
  GoTestImage T;
  // The older shape has no `deferreturn`, so a record is only kept when the
  // body reaches a runtime entry point the table also names.
  GoFuncSpec Work;
  Work.Name = "main.work";
  Work.EntryVA = kTextVA + 0x100;
  Work.DeferReturn = 0x20; // the legacy frame size field, not a code offset
  Work.PcSP.Steps = {{0x30, 0x80}};
  Work.FuncData = {std::nullopt, std::nullopt};
  GoFuncSpec Panic;
  Panic.Name = "runtime.gopanic";
  Panic.EntryVA = kTextVA + 0x400;
  Panic.FuncData = {std::nullopt};
  T.installPclnTab(buildPclnTab(kGo12Magic, {Work, Panic}, kTextVA + 0x500,
                                /*MinLC=*/1, /*PreGo112Record=*/true)
                       .Bytes);
  T.writeText(kTextVA + 0x110, makeCall(kTextVA + 0x110, kTextVA + 0x400));

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_TRUE(Info.GoModule->UsesPreGo112FuncLayout);

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_EQ(F->Go->Panics.size(), 1u);
  EXPECT_EQ(F->Go->Panics[0].CallVA, kTextVA + 0x110);
  EXPECT_EQ(F->Go->Panics[0].RuntimeName, "runtime.gopanic");
  // The word the newer shape spends on `deferreturn` held the frame size
  // here, and reporting it would name an address in another function.
  EXPECT_FALSE(F->Go->DeferReturnOffset.has_value());
  EXPECT_EQ(F->Go->FuncID, 0);
}

TEST(GoLegacyPclnTab, ProvesWhichPCDataTableHoldsTheStackMapIndex) {
  GoTestImage T;
  const va_t LocalsVA = T.addPayload(buildStackMap(2, {{0b01}, {0b10}}));
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  // Table 0 names an index the map cannot satisfy, which is what rules it out;
  // table 1 stays inside the map's two bitmaps.
  PCValueTable RegMapIndex;
  RegMapIndex.Steps = {{5, 0x80}};
  PCValueTable StackMapIndex;
  StackMapIndex.Steps = {{0, 0x40}, {1, 0x40}};
  Work.PCData = {RegMapIndex, StackMapIndex};
  Work.FuncData = {std::nullopt, LocalsVA};
  T.installPclnTab(
      buildPclnTab(kGo12Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  ASSERT_TRUE(Info.GoModule->StackMapPCDataIndex.has_value());
  EXPECT_EQ(*Info.GoModule->StackMapPCDataIndex, 1u);

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_EQ(F->Go->StackMapRanges.size(), 2u);
  EXPECT_EQ(F->Go->StackMapRanges[0].Index, 0);
  EXPECT_EQ(F->Go->StackMapRanges[1].Index, 1);
}

TEST(GoLegacyPclnTab, ReportsNoStackMapRangesWhenBothCandidatesSurvive) {
  GoTestImage T;
  const va_t LocalsVA = T.addPayload(buildStackMap(2, {{0b01}, {0b10}}));
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable Ambiguous;
  Ambiguous.Steps = {{0, 0x40}, {1, 0x40}};
  Work.PCData = {Ambiguous, Ambiguous};
  Work.FuncData = {std::nullopt, LocalsVA};
  T.installPclnTab(
      buildPclnTab(kGo12Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_FALSE(Info.GoModule->StackMapPCDataIndex.has_value());
  EXPECT_TRUE(
      anyDiagnosticContains(Info.Diagnostics, "PCDATA_StackMapIndex"));

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  // Nothing proved which table selects a bitmap, but the bitmaps themselves
  // were never in doubt.
  EXPECT_TRUE(F->Go->LocalsPointerMap.has_value());
  EXPECT_TRUE(F->Go->StackMapRanges.empty());
}

TEST(GoLegacyPclnTab, ReadsOpenCodedDeferInfoFromItsPreGo116Index) {
  GoTestImage T;
  const va_t DeferInfoVA = T.addPayload(buildContiguousDeferInfo(0x18, 0x10));
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  Work.PcSP.Steps = {{0x20, 0x80}};
  // Six entries is the shape only Go 1.14 and later emit, which is what makes
  // index five unambiguous on a table this old.
  Work.FuncData = {std::nullopt, std::nullopt, std::nullopt,
                   std::nullopt, std::nullopt, DeferInfoVA};
  T.installPclnTab(
      buildPclnTab(kGo12Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->UsesOpenCodedDefers);
  ASSERT_TRUE(F->Go->OpenCodedDeferInfo.has_value());
  EXPECT_EQ(F->Go->OpenCodedDeferInfo->DeferBitsOffset, 0x18u);
  EXPECT_EQ(F->Go->OpenCodedDeferInfo->SlotsOffset, 0x10u);
  ASSERT_EQ(F->Go->OpenCodedDefers.size(), 2u);
  EXPECT_EQ(F->Go->OpenCodedDefers[0].ClosureOffset, -0x10);
  EXPECT_EQ(F->Go->OpenCodedDefers[1].ClosureOffset, -0x08);
}

TEST(GoLegacyPclnTab, RefusesAHeaderWhoseFileTableOffsetIsMissing) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  BuiltPclnTab Tab = buildPclnTab(kGo12Magic, {Work}, kTextVA + 0x200);
  // The slot behind the functab sentinel is the only thing that says where
  // the file table is; a zero there is not a table this decoder can walk.
  Tab.put32(16 + 3 * 8, 0);
  T.installPclnTab(Tab.Bytes);

  EXPECT_FALSE(hasGoRuntimeMetadata(T.Img));
  parseGoExceptions(T.Img);
  EXPECT_FALSE(T.Img.ExceptionMetadata.GoModule.has_value());
}

TEST(GoLegacyPclnTab, StopsAtAFunctabEntryPointingOutsideTheImage) {
  GoTestImage T;
  GoFuncSpec First = makeDeferringFunc("main.first", kTextVA + 0x100);
  GoFuncSpec Second = makeDeferringFunc("main.second", kTextVA + 0x200);
  BuiltPclnTab Tab = buildPclnTab(kGo12Magic, {First, Second},
                                  kTextVA + 0x300);
  // Second entry's funcoff, i.e. the fourth word of the functab.
  Tab.put64(16 + 3 * 8, 0x00F00000);
  T.installPclnTab(Tab.Bytes);

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_EQ(Info.GoModule->FunctionCount, 1u);
  EXPECT_TRUE(anyDiagnosticContains(Info.Diagnostics, "ended early"));
  EXPECT_EQ(Info.ParseStatus, ExceptionParseStatus::Partial);
}

} // namespace
