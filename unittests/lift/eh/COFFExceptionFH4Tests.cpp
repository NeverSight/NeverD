//===- COFFExceptionFH4Tests.cpp - __CxxFrameHandler4 tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "COFFExceptionTestsDetail.h"
#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/support/BinaryEncoding.h"

namespace {

using namespace neverd;
using namespace neverd::coff_eh_test;

TEST(COFFExceptionParser, ReconstructsCompressedCxxFrameHandler4Graph) {
  BinaryImage Img = makeX64ExceptionImage(0x300);
  addPersonalityImport(Img, Img.Base + 0x1100, "__CxxFrameHandler4");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0x3040); // handler data -> FuncInfo4

  // FuncInfo4: unwind map + try map, followed by the mandatory IP map.
  X[0x40] = 0x18;
  writeLE<uint32_t>(X + 0x41, 0x3080);
  writeLE<uint32_t>(X + 0x45, 0x30a0);
  writeLE<uint32_t>(X + 0x49, 0x30e0);

  // Two unwind entries.  FH4 compressed integers below are all one byte
  // (value << 1).  Entry 1 points five bytes back to entry 0.
  X[0x80] = 4;  // count = 2
  X[0x81] = 14; // direct action, one-byte empty-state sentinel
  writeLE<uint32_t>(X + 0x82, 0x1120);
  X[0x86] = 46; // direct action, predecessor byte distance = 5
  writeLE<uint32_t>(X + 0x87, 0x1130);

  X[0xa0] = 2; // one try block
  X[0xa1] = 0; // try low = 0
  X[0xa2] = 0; // try high = 0
  X[0xa3] = 2; // catch high = 1
  writeLE<uint32_t>(X + 0xa4, 0x30c0);

  X[0xc0] = 2;    // one handler
  X[0xc1] = 0x10; // one function-relative continuation
  writeLE<uint32_t>(X + 0xc2, 0x1150);
  X[0xc6] = 0x60; // continuation = function + 0x30

  X[0xe0] = 4;    // two IP-state entries
  X[0xe1] = 0;    // delta 0
  X[0xe2] = 0;    // encoded state -1
  X[0xe3] = 0x20; // delta 0x10
  X[0xe4] = 2;    // encoded state 0

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::CxxFrameHandler4);
  ASSERT_TRUE(Decoded.Cxx.has_value());
  EXPECT_EQ(Decoded.Cxx->NativeEncoding, CxxExceptionInfo::Encoding::FH4);
  EXPECT_TRUE(Decoded.Cxx->hasValidStateGraph());
  ASSERT_EQ(Decoded.Cxx->UnwindMap.size(), 2u);
  EXPECT_EQ(Decoded.Cxx->UnwindMap[1].ToState, 0);
  ASSERT_EQ(Decoded.Cxx->TryBlocks.size(), 1u);
  ASSERT_EQ(Decoded.Cxx->TryBlocks[0].Handlers.size(), 1u);
  ASSERT_EQ(Decoded.Cxx->TryBlocks[0].Handlers[0].ContinuationVAs.size(), 1u);
  EXPECT_EQ(Decoded.Cxx->TryBlocks[0].Handlers[0].ContinuationVAs[0],
            Img.Base + 0x1030);
  EXPECT_FALSE(Decoded.canRegenerateLanguageMetadata());
}

namespace {

/// Build an FH4 image whose function was split into two contributions, with
/// the IP-to-state field naming the segment directory rather than a map.  The
/// directory files a real map under \p ListedSegmentRVA; the runtime function
/// under test always begins at 0x1000.
BinaryImage makeSeparatedFH4Image(uint32_t ListedSegmentRVA) {
  BinaryImage Img = makeX64ExceptionImage(0x300);
  addPersonalityImport(Img, Img.Base + 0x1100, "__CxxFrameHandler4");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0x3040);

  // isSeparated | UnwindMap, so the IP field addresses a SepIPtoStateMap.
  X[0x40] = 0x02 | 0x08;
  writeLE<uint32_t>(X + 0x41, 0x3080); // dispUnwindMap
  writeLE<uint32_t>(X + 0x45, 0x30a0); // dispIPtoStateMap -> directory

  X[0x80] = 2;  // one unwind entry
  X[0x81] = 14; // direct action, empty-state sentinel
  writeLE<uint32_t>(X + 0x82, 0x1120);

  // Segment directory: two contributions, the second of which is the one the
  // decoder must ignore.
  X[0xa0] = 4;                              // count = 2
  writeLE<uint32_t>(X + 0xa1, ListedSegmentRVA);
  writeLE<uint32_t>(X + 0xa5, 0x30e0);      // map for that contribution
  writeLE<uint32_t>(X + 0xa9, 0x1800);      // an unrelated contribution
  writeLE<uint32_t>(X + 0xad, 0x3100);      // whose map must not be selected

  X[0xe0] = 4;    // two IP-state entries, relative to this contribution
  X[0xe1] = 0;    // delta 0
  X[0xe2] = 0;    // encoded state -1
  X[0xe3] = 0x20; // delta 0x10
  X[0xe4] = 2;    // encoded state 0

  X[0x100] = 2; // the decoy map: one entry naming state 0 at offset 0
  X[0x101] = 0;
  X[0x102] = 2;

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));
  return Img;
}

} // namespace

TEST(COFFExceptionParser, SelectsSeparatedFH4MapForItsCodeContribution) {
  BinaryImage Img = makeSeparatedFH4Image(/*ListedSegmentRVA=*/0x1000);
  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(Decoded.Cxx.has_value());
  EXPECT_TRUE(Decoded.Cxx->IsSeparated);
  EXPECT_TRUE(Decoded.Cxx->hasValidStateGraph());
  ASSERT_EQ(Decoded.Cxx->IPMap.size(), 2u);
  EXPECT_EQ(Decoded.Cxx->IPMap[0].IP, Img.Base + 0x1000);
  EXPECT_EQ(Decoded.Cxx->IPMap[0].State, -1);
  EXPECT_EQ(Decoded.Cxx->IPMap[1].IP, Img.Base + 0x1010);
  EXPECT_EQ(Decoded.Cxx->IPMap[1].State, 0);
}

TEST(COFFExceptionParser, TreatsUnlistedSeparatedFH4ContributionAsStateless) {
  // A contribution the directory does not name has no states at all, which is
  // the same conclusion the runtime reaches.  It is a complete decode, not a
  // failed lookup, and it must not inherit another contribution's map.
  BinaryImage Img = makeSeparatedFH4Image(/*ListedSegmentRVA=*/0x1400);
  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(Decoded.Cxx.has_value());
  EXPECT_TRUE(Decoded.Cxx->IsSeparated);
  EXPECT_EQ(Decoded.Cxx->UnwindMap.size(), 1u);
  EXPECT_TRUE(Decoded.Cxx->IPMap.empty());
}

TEST(COFFExceptionParser, StopsFH4RepeatedHandlerExpansionAtBudget) {
  BinaryImage Img = makeX64ExceptionImage(0x100);
  addPersonalityImport(Img, Img.Base + 0x1100, "__CxxFrameHandler4");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0x3040); // handler data -> FuncInfo4

  X[0x40] = 0x10; // try map plus mandatory IP map
  writeLE<uint32_t>(X + 0x41, 0x3080);
  writeLE<uint32_t>(X + 0x45, 0x30c0);

  // Canonical three-byte encoding of 65,536 try records.  Every record may
  // legally reference the same handler map, so a per-table limit alone would
  // permit quadratic normalized-graph expansion.
  X[0x80] = 3;
  X[0x81] = 0;
  X[0x82] = 8;
  X[0x83] = 0; // try low
  X[0x84] = 0; // try high
  X[0x85] = 0; // catch high
  writeLE<uint32_t>(X + 0x86, 0x30a0);
  X[0xa0] = 2; // one handler; aggregate budget is already exhausted
  X[0xc0] = 2; // one mandatory IP-state entry (not reached)
  X[0xc1] = 0;
  X[0xc2] = 0;

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1080};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::CxxFrameHandler4);
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_FALSE(Decoded.Cxx.has_value());
  ASSERT_FALSE(Decoded.Diagnostics.empty());
  EXPECT_NE(Decoded.Diagnostics.back().find("aggregate language graph"),
            std::string::npos);
}

TEST(COFFExceptionParser, RejectsNonCanonicalFH4CompressedInteger) {
  BinaryImage Img = makeX64ExceptionImage(0x100);
  addPersonalityImport(Img, Img.Base + 0x1100, "__CxxFrameHandler4");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0x3040);
  X[0x40] = 0;
  writeLE<uint32_t>(X + 0x41, 0x3080);
  X[0x80] = 5; // overlong two-byte encoding of the value one
  X[0x81] = 0;

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].ParseStatus,
            ExceptionParseStatus::Malformed);
}

} // namespace
