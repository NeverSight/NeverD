//===- COFFExceptionTests.cpp - Windows exception metadata tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "COFFExceptionTestsDetail.h"
#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/support/BinaryEncoding.h"

#include <limits>

namespace {

using namespace neverd;
using namespace neverd::coff_eh_test;

TEST(COFFExceptionModel, BuildsCheckedHalfOpenRanges) {
  auto R = ExceptionAddressRange::fromStartAndSize(0x1000, 0x40);
  ASSERT_TRUE(R.has_value());
  EXPECT_EQ(R->Begin, 0x1000u);
  EXPECT_EQ(R->End, 0x1040u);
  EXPECT_TRUE(R->contains(0x1000));
  EXPECT_TRUE(R->contains(0x103f));
  EXPECT_FALSE(R->contains(0x1040));
  EXPECT_TRUE(R->contains(*R));

  EXPECT_FALSE(ExceptionAddressRange::fromStartAndSize(0x1000, 0));
  EXPECT_FALSE(ExceptionAddressRange::fromStartAndSize(
      std::numeric_limits<va_t>::max() - 3, 8));
  EXPECT_FALSE((ExceptionAddressRange{0x2000, 0x2000}.isValid()));
}

TEST(COFFExceptionModel, CombinesParseStatusConservatively) {
  EXPECT_EQ(mergeExceptionParseStatus(ExceptionParseStatus::Complete,
                                      ExceptionParseStatus::Complete),
            ExceptionParseStatus::Complete);
  EXPECT_EQ(mergeExceptionParseStatus(ExceptionParseStatus::Complete,
                                      ExceptionParseStatus::Partial),
            ExceptionParseStatus::Partial);
  EXPECT_EQ(mergeExceptionParseStatus(ExceptionParseStatus::Malformed,
                                      ExceptionParseStatus::Partial),
            ExceptionParseStatus::Malformed);
}

TEST(COFFExceptionModel, ValidatesCxxStateMaps) {
  CxxExceptionInfo Info;
  Info.MaxState = 3;
  Info.UnwindMap = {{-1, 0x5000}, {0, 0x5010}, {1, 0x5020}};
  Info.IPMap = {{0x1000, -1}, {0x1010, 0}, {0x1020, 2}};
  EXPECT_TRUE(Info.hasValidStateGraph());

  Info.UnwindMap[1].ToState = 1;
  EXPECT_FALSE(Info.hasValidStateGraph());
  Info.UnwindMap[1].ToState = 0;
  Info.IPMap[2].IP = 0x1008;
  EXPECT_FALSE(Info.hasValidStateGraph());
}

TEST(COFFExceptionModel, KeepsPersonalityIdentityAndGSProvenance) {
  EXPECT_TRUE(isSEHPersonality(ExceptionPersonality::CSpecificHandler));
  EXPECT_TRUE(isSEHPersonality(ExceptionPersonality::GSHandlerCheckSEH));
  EXPECT_TRUE(isCxxPersonality(ExceptionPersonality::CxxFrameHandler3));
  EXPECT_TRUE(isCxxPersonality(ExceptionPersonality::GSHandlerCheckEH4));
  EXPECT_TRUE(isGSWrappedPersonality(ExceptionPersonality::GSHandlerCheckEH));
  EXPECT_FALSE(isGSWrappedPersonality(ExceptionPersonality::CxxFrameHandler3));
}

TEST(COFFExceptionModel, QueriesOwningRuntimeFunction) {
  ExceptionInfo EI;
  ExceptionFunction First;
  First.CodeRange = {0x1000, 0x1040};
  First.ParseStatus = ExceptionParseStatus::Complete;
  EI.Functions.push_back(First);

  ExceptionFunction Second;
  Second.CodeRange = {0x2000, 0x2080};
  Second.ParseStatus = ExceptionParseStatus::Partial;
  EI.Functions.push_back(Second);
  EI.rebuildIndex();

  ASSERT_NE(EI.findFunction(0x1020), nullptr);
  EXPECT_EQ(EI.findFunction(0x1020)->CodeRange.Begin, 0x1000u);
  EXPECT_EQ(EI.findFunction(0x207f)->ParseStatus,
            ExceptionParseStatus::Partial);
  EXPECT_EQ(EI.findFunction(0x1040), nullptr);
  EXPECT_EQ(EI.findFunction(0x3000), nullptr);
}

TEST(COFFExceptionParser, DecodesX64V1OperationsAndHandlerLocation) {
  BinaryImage Img = makeX64ExceptionImage();
  uint8_t *X = Img.Segments[1].Data.data();
  X[0] = 1 | (1 << 3); // version 1, exception handler
  X[1] = 5;            // prologue size
  X[2] = 2;            // two unwind slots
  X[3] = 0;
  X[4] = 4;
  X[5] = (3 << 4) | 2; // allocate 32 bytes
  X[6] = 1;
  X[7] = (5 << 4) | 0; // push rbp
  writeLE<uint32_t>(X + 8, 0x1100);

  ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
      Img, Img.Base, 0x2000, 0x1000, 0x1040, 0x3000);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(F.Encoding, ExceptionEncoding::X64UnwindV1);
  ASSERT_EQ(F.UnwindOperations.size(), 2u);
  EXPECT_EQ(F.UnwindOperations[0].Kind, UnwindOperationKind::AllocateSmall);
  EXPECT_EQ(F.UnwindOperations[0].StackOffset, 32u);
  EXPECT_EQ(F.UnwindOperations[1].Kind, UnwindOperationKind::PushNonVolatile);
  EXPECT_EQ(F.UnwindOperations[1].Register, 5u);
  EXPECT_EQ(F.PersonalityVA, Img.Base + 0x1100);
  EXPECT_EQ(F.HandlerDataVA, Img.Base + 0x300c);
}

TEST(COFFExceptionParser, RejectsTruncatedX64UnwindSlots) {
  BinaryImage Img = makeX64ExceptionImage(4);
  uint8_t *X = Img.Segments[1].Data.data();
  X[0] = 1;
  X[1] = 4;
  X[2] = 3;
  X[3] = 0;

  ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
      Img, Img.Base, 0x2000, 0x1000, 0x1040, 0x3000);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Malformed);
  EXPECT_FALSE(F.Diagnostics.empty());
}

TEST(COFFExceptionParser, AcceptsZeroX64UnwindSlotsAtSectionEnd) {
  BinaryImage Img = makeX64ExceptionImage(4);
  uint8_t *X = Img.Segments[1].Data.data();
  X[0] = 1;
  X[1] = 0;
  X[2] = 0;
  X[3] = 0;

  ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
      Img, Img.Base, 0x2000, 0x1000, 0x1040, 0x3000);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_TRUE(F.UnwindOperations.empty());
}

TEST(COFFExceptionParser, RejectsInvalidX64V1CodeOrdering) {
  BinaryImage Img = makeX64ExceptionImage();
  uint8_t *X = Img.Segments[1].Data.data();
  X[0] = 1;
  X[1] = 5;
  X[2] = 2;
  X[4] = 1;
  X[5] = (5 << 4) | 0;
  X[6] = 4; // Later slot must describe an earlier prologue instruction.
  X[7] = (3 << 4) | 2;

  ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
      Img, Img.Base, 0x2000, 0x1000, 0x1040, 0x3000);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Malformed);
}

TEST(COFFExceptionParser, AcceptsX64V1OperationsAtTheSameCodeOffset) {
  BinaryImage Img = makeX64ExceptionImage();
  uint8_t *X = Img.Segments[1].Data.data();
  X[0] = 1;
  X[1] = 4;
  X[2] = 2;
  X[3] = 5; // rbp is the frame register
  X[4] = 4;
  X[5] = 3; // set frame pointer
  X[6] = 4;
  X[7] = (3 << 4) | 2; // allocate 32 bytes at the same prologue offset

  ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
      Img, Img.Base, 0x2000, 0x1000, 0x1040, 0x3000);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_EQ(F.UnwindOperations.size(), 2u);
  EXPECT_EQ(F.UnwindOperations[0].Kind, UnwindOperationKind::SetFramePointer);
  EXPECT_EQ(F.UnwindOperations[1].Kind, UnwindOperationKind::AllocateSmall);
}

TEST(COFFExceptionParser, DecodesX64V3PayloadAndWODPool) {
  BinaryImage Img = makeX64ExceptionImage();
  uint8_t *X = Img.Segments[1].Data.data();
  X[0] = 3;
  X[1] = 8;
  X[2] = 6;            // twelve payload bytes
  X[3] = 2 | (1 << 5); // two prolog ops, one epilog
  X[4] = 4;
  X[5] = 1; // prolog IP offsets
  X[6] = 2 << 3;
  writeLE<uint16_t>(X + 7, 0x30); // epilog descriptor
  writeLE<uint16_t>(X + 9, 0);    // first WOD
  X[11] = 5;                      // last instruction offset
  X[12] = 1;
  X[13] = 4; // epilog IP offsets
  X[14] = 0x38;
  X[15] = 0x2c; // alloc_small 32, push rbp

  ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
      Img, Img.Base, 0x2000, 0x1000, 0x1080, 0x3000);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(F.Encoding, ExceptionEncoding::X64UnwindV3);
  ASSERT_EQ(F.UnwindOperations.size(), 2u);
  EXPECT_EQ(F.UnwindOperations[0].Kind, UnwindOperationKind::AllocateSmall);
  EXPECT_EQ(F.UnwindOperations[1].Kind, UnwindOperationKind::PushNonVolatile);
  ASSERT_EQ(F.Epilogs.size(), 1u);
  EXPECT_EQ(F.Epilogs[0].StartOffset, 0x30);
  ASSERT_EQ(F.Epilogs[0].Operations.size(), 2u);
}

TEST(COFFExceptionParser, RejectsTruncatedX64V3Payload) {
  BinaryImage Img = makeX64ExceptionImage(8);
  uint8_t *X = Img.Segments[1].Data.data();
  X[0] = 3;
  X[2] = 6;

  ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
      Img, Img.Base, 0x2000, 0x1000, 0x1040, 0x3000);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Malformed);
}

TEST(COFFExceptionParser, RejectsInvalidX64V3OrderingAndInheritance) {
  {
    BinaryImage Img = makeX64ExceptionImage();
    uint8_t *X = Img.Segments[1].Data.data();
    X[0] = 3;
    X[1] = 8;
    X[2] = 2;
    X[3] = 2;
    X[4] = 1;
    X[5] = 4; // Prologue operation offsets must be descending.
    X[6] = 0x38;
    X[7] = 0x2c;

    ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
        Img, Img.Base, 0x2000, 0x1000, 0x1080, 0x3000);
    EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Malformed);
  }

  {
    BinaryImage Img = makeX64ExceptionImage();
    uint8_t *X = Img.Segments[1].Data.data();
    X[0] = 3;
    X[1] = 0;
    X[2] = 6;
    X[3] = 2 << 5;
    X[4] = 1 << 3;                 // One-operation full descriptor.
    writeLE<int16_t>(X + 5, 0x20); // Ascending from function start.
    writeLE<uint16_t>(X + 7, 0);   // First WOD.
    X[9] = 2;                      // Last instruction offset.
    X[10] = 1;                     // Operation IP offset.
    X[11] = 1;                     // Inherited descriptor changes flags.
    writeLE<int16_t>(X + 12, 0x10);
    X[14] = 0x2c;

    ExceptionFunction F = coff_loader::decodeX64ExceptionFunction(
        Img, Img.Base, 0x2000, 0x1000, 0x1080, 0x3000);
    EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Malformed);
  }
}

} // namespace
