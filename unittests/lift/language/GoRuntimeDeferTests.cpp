//===- GoRuntimeDeferTests.cpp - Go open-coded defer record tests -----===//
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
// Open-coded defer records
//
// Go 1.22 changed how `FUNCDATA_OpenCodedDeferInfo` is spelled without
// changing the pclntab magic, so the two layouts share `Go120Magic` and only
// the bytes say which one a record is.  Reading the older spelling as the
// newer one turns a slot count into a frame offset, which is rejected --
// silently costing every function in a pre-1.22 image its open-coded defer
// state, and, because the same read is what confirms the funcdata base, the
// base along with it.
//===----------------------------------------------------------------------===//

/// An image whose functions each carry one open-coded defer record.  Every
/// offset in such a record has to land inside the frame, so the frame size is
/// what bounds how deep a slot a test can describe.
GoTestImage buildDeferInfoImage(const std::vector<std::vector<uint8_t>> &Records,
                                int32_t FrameSize = 0x30) {
  GoTestImage T;
  std::vector<GoFuncSpec> Funcs;
  for (size_t I = 0; I < Records.size(); ++I) {
    const va_t RecordVA = T.addPayload(Records[I]);
    GoFuncSpec Work = makeDeferringFunc("main.work" + std::to_string(I),
                                        kTextVA + 0x100 * (I + 1));
    Work.PcSP.Steps = {{FrameSize, 0x80}};
    Work.FuncData = {std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                     RecordVA};
    Funcs.push_back(std::move(Work));
  }
  T.installPclnTab(
      buildPclnTab(kGo116Magic, Funcs,
                   kTextVA + 0x100 * (Records.size() + 1))
          .Bytes);
  return T;
}

TEST(GoOpenCodedDefers, ReadsTheEnumeratedRecordWrittenBeforeGo122) {
  GoTestImage T = buildDeferInfoImage(
      {buildEnumeratedDeferInfo(0x28, {0x18, 0x10, 0x08})});

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_EQ(Info.GoModule->OpenCodedDeferLayout,
            GoOpenCodedDeferLayout::Enumerated);

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->Go->OpenCodedDeferInfo.has_value());
  EXPECT_EQ(F->Go->OpenCodedDeferInfo->DeferBitsOffset, 0x28u);
  EXPECT_EQ(F->Go->OpenCodedDeferInfo->SlotsOffset, 0x18u);
  EXPECT_TRUE(F->Go->OpenCodedDeferInfo->SlotCountIsExact);
  ASSERT_EQ(F->Go->OpenCodedDefers.size(), 3u);
  EXPECT_EQ(F->Go->OpenCodedDefers[0].ClosureOffset, -0x18);
  EXPECT_EQ(F->Go->OpenCodedDefers[1].ClosureOffset, -0x10);
  EXPECT_EQ(F->Go->OpenCodedDefers[2].ClosureOffset, -0x08);
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Complete);
}

// The enumerated record can say something the contiguous one cannot, and this
// is it: before Go 1.22 the compiler had no reason to put the closure slots
// next to each other, so a frame can hold two that are half a frame apart.
// Reading such a record as a run would report slots the function never had.
TEST(GoOpenCodedDefers, KeepsSlotsThatAreNotOneRun) {
  GoTestImage T =
      buildDeferInfoImage({buildEnumeratedDeferInfo(0x28, {0x20, 0x08})});

  parseGoExceptions(T.Img);

  const ExceptionFunction *F =
      findRecord(T.Img.ExceptionMetadata, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  // Read as a run the same first slot would have reported four.
  ASSERT_EQ(F->Go->OpenCodedDefers.size(), 2u);
  EXPECT_EQ(F->Go->OpenCodedDefers[0].ClosureOffset, -0x20);
  EXPECT_EQ(F->Go->OpenCodedDefers[1].ClosureOffset, -0x08);
}

// The record Go wrote from 1.14 until deferred functions became argumentless
// in 1.18.  Its leading maximum argument frame shifts every field after it, so
// a reader that does not expect it takes that size for the bitmask offset and
// the bitmask offset for a slot count, which is how an image of this vintage
// loses every open-coded defer it has.
TEST(GoOpenCodedDefers, ReadsTheLegacyRecordWrittenBeforeGo118) {
  GoTestImage T = buildDeferInfoImage({buildLegacyEnumeratedDeferInfo(
      /*MaxArgSize=*/0x10, /*DeferBits=*/0x28,
      {LegacyDefer{0x10, 0x18, {}}, LegacyDefer{0x00, 0x08, {}}})});

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_EQ(Info.GoModule->OpenCodedDeferLayout,
            GoOpenCodedDeferLayout::LegacyEnumerated);

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->Go->OpenCodedDeferInfo.has_value());
  EXPECT_EQ(F->Go->OpenCodedDeferInfo->DeferBitsOffset, 0x28u);
  EXPECT_EQ(F->Go->OpenCodedDeferInfo->SlotsOffset, 0x18u);
  EXPECT_TRUE(F->Go->OpenCodedDeferInfo->SlotCountIsExact);
  ASSERT_EQ(F->Go->OpenCodedDefers.size(), 2u);
  EXPECT_EQ(F->Go->OpenCodedDefers[0].ClosureOffset, -0x18);
  EXPECT_EQ(F->Go->OpenCodedDefers[1].ClosureOffset, -0x08);
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Complete);
}

// An argument list is variable-length filler between one defer's closure slot
// and the next one's, so a reader that does not walk it reads the second
// defer's fields out of the first defer's arguments.
TEST(GoOpenCodedDefers, WalksTheArgumentListSeparatingTwoLegacyDefers) {
  GoTestImage T = buildDeferInfoImage({buildLegacyEnumeratedDeferInfo(
      /*MaxArgSize=*/0x10, /*DeferBits=*/0x28,
      {LegacyDefer{0x10, 0x18, {{0x20, 0x08, 0x00}, {0x28, 0x08, 0x08}}},
       LegacyDefer{0x00, 0x08, {}}})});

  parseGoExceptions(T.Img);

  const ExceptionFunction *F =
      findRecord(T.Img.ExceptionMetadata, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->Go->OpenCodedDeferInfo.has_value());
  ASSERT_EQ(F->Go->OpenCodedDefers.size(), 2u);
  EXPECT_EQ(F->Go->OpenCodedDefers[0].ClosureOffset, -0x18);
  EXPECT_EQ(F->Go->OpenCodedDefers[1].ClosureOffset, -0x08);
}

// Three images with the same magic and three different records: whichever
// spelling the bytes are is the one that has to be read, because the header
// cannot say.  One magic covers the Go 1.22 rewrite outright, and the Go 1.18
// one happened inside the span of another.
TEST(GoOpenCodedDefers, PicksTheLayoutTheRecordsProveNotTheMagic) {
  GoTestImage Legacy = buildDeferInfoImage({buildLegacyEnumeratedDeferInfo(
      0x10, 0x28, {LegacyDefer{0x10, 0x18, {}}, LegacyDefer{0, 0x08, {}}})});
  GoTestImage Old = buildDeferInfoImage(
      {buildEnumeratedDeferInfo(0x28, {0x18, 0x10, 0x08})});
  GoTestImage New = buildDeferInfoImage({buildContiguousDeferInfo(0x28, 0x18)});

  parseGoExceptions(Legacy.Img);
  parseGoExceptions(Old.Img);
  parseGoExceptions(New.Img);

  ASSERT_TRUE(Legacy.Img.ExceptionMetadata.GoModule.has_value());
  ASSERT_TRUE(Old.Img.ExceptionMetadata.GoModule.has_value());
  ASSERT_TRUE(New.Img.ExceptionMetadata.GoModule.has_value());
  EXPECT_EQ(Legacy.Img.ExceptionMetadata.GoModule->PclnTabMagic,
            New.Img.ExceptionMetadata.GoModule->PclnTabMagic);
  EXPECT_EQ(Old.Img.ExceptionMetadata.GoModule->PclnTabMagic,
            New.Img.ExceptionMetadata.GoModule->PclnTabMagic);
  EXPECT_EQ(Legacy.Img.ExceptionMetadata.GoModule->OpenCodedDeferLayout,
            GoOpenCodedDeferLayout::LegacyEnumerated);
  EXPECT_EQ(Old.Img.ExceptionMetadata.GoModule->OpenCodedDeferLayout,
            GoOpenCodedDeferLayout::Enumerated);
  EXPECT_EQ(New.Img.ExceptionMetadata.GoModule->OpenCodedDeferLayout,
            GoOpenCodedDeferLayout::Contiguous);

  // All three name the same first slot, and only the newest cannot say how
  // many follow it.
  const ExceptionFunction *LegacyF =
      findRecord(Legacy.Img.ExceptionMetadata, kTextVA + 0x100);
  const ExceptionFunction *OldF =
      findRecord(Old.Img.ExceptionMetadata, kTextVA + 0x100);
  const ExceptionFunction *NewF =
      findRecord(New.Img.ExceptionMetadata, kTextVA + 0x100);
  ASSERT_NE(LegacyF, nullptr);
  ASSERT_NE(OldF, nullptr);
  ASSERT_NE(NewF, nullptr);
  EXPECT_EQ(LegacyF->Go->OpenCodedDeferInfo->SlotsOffset, 0x18u);
  EXPECT_EQ(OldF->Go->OpenCodedDeferInfo->SlotsOffset, 0x18u);
  EXPECT_EQ(NewF->Go->OpenCodedDeferInfo->SlotsOffset, 0x18u);
  EXPECT_TRUE(LegacyF->Go->OpenCodedDeferInfo->SlotCountIsExact);
  EXPECT_TRUE(OldF->Go->OpenCodedDeferInfo->SlotCountIsExact);
  EXPECT_FALSE(NewF->Go->OpenCodedDeferInfo->SlotCountIsExact);
}

// A frame holding the maximum eight open-coded defers reads the same either
// way: eight is both a slot count and a pointer-aligned slot offset, so the
// older spelling's second word is exactly what the newer one expects there.
// Such a record proves nothing and must not be allowed to outvote one that
// does, which it would if merely parsing counted as evidence.
TEST(GoOpenCodedDefers, LetsOnlyDistinguishingRecordsDecideTheLayout) {
  const std::vector<uint32_t> EightSlots = {0x40, 0x38, 0x30, 0x28,
                                            0x20, 0x18, 0x10, 0x08};
  GoTestImage T = buildDeferInfoImage(
      {
          buildEnumeratedDeferInfo(0x48, EightSlots),
          buildEnumeratedDeferInfo(0x48, EightSlots),
          buildEnumeratedDeferInfo(0x28, {0x20, 0x08}),
      },
      /*FrameSize=*/0x80);

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_EQ(Info.GoModule->OpenCodedDeferLayout,
            GoOpenCodedDeferLayout::Enumerated);

  // The ambiguous records are then read the way the image was decided to be
  // written, which is the whole point of deciding it once rather than per
  // record: read as a run, each would have reported one slot instead of eight.
  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_EQ(F->Go->OpenCodedDefers.size(), EightSlots.size());
}

// A slot offset is pointer aligned and a slot count is at most eight, so
// neither reading accepts a second word that is neither.
TEST(GoOpenCodedDefers, ReportsARecordNoLayoutCanRead) {
  GoTestImage T = buildDeferInfoImage({buildContiguousDeferInfo(0x40, 0x0d)});

  parseGoExceptions(T.Img);

  const ExceptionFunction *F =
      findRecord(T.Img.ExceptionMetadata, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->UsesOpenCodedDefers);
  EXPECT_FALSE(F->Go->OpenCodedDeferInfo.has_value());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_TRUE(anyDiagnosticContains(F->Diagnostics,
                                    "does not describe a frame"));
}

} // namespace
