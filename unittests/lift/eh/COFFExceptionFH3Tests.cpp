//===- COFFExceptionFH3Tests.cpp - __CxxFrameHandler3 tests -----------===//
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

TEST(COFFExceptionParser, ReconstructsCxxFrameHandler3StateGraph) {
  BinaryImage Img = makeX64ExceptionImage(0x200);
  addPersonalityImport(Img, Img.Base + 0x1100, "__CxxFrameHandler3");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0x3040); // handler data -> FuncInfo3

  uint8_t *FI = X + 0x40;
  writeLE<uint32_t>(FI, 0x19930522);
  writeLE<int32_t>(FI + 4, 2);
  writeLE<uint32_t>(FI + 8, 0x3080);
  writeLE<uint32_t>(FI + 12, 1);
  writeLE<uint32_t>(FI + 16, 0x3090);
  writeLE<uint32_t>(FI + 20, 2);
  writeLE<uint32_t>(FI + 24, 0x30d0);
  writeLE<int32_t>(FI + 28, -8);
  writeLE<uint32_t>(FI + 32, 0);
  writeLE<uint32_t>(FI + 36, 1);

  uint8_t *Unwind = X + 0x80;
  writeLE<int32_t>(Unwind, -1);
  writeLE<uint32_t>(Unwind + 4, 0x1120);
  writeLE<int32_t>(Unwind + 8, 0);
  writeLE<uint32_t>(Unwind + 12, 0x1130);

  uint8_t *Try = X + 0x90;
  writeLE<int32_t>(Try, 0);
  writeLE<int32_t>(Try + 4, 0);
  writeLE<int32_t>(Try + 8, 1);
  writeLE<uint32_t>(Try + 12, 1);
  writeLE<uint32_t>(Try + 16, 0x30b0);

  uint8_t *Catch = X + 0xb0;
  writeLE<uint32_t>(Catch, 0x40);
  writeLE<uint32_t>(Catch + 4, 0); // catch (...)
  writeLE<int32_t>(Catch + 8, 0);
  writeLE<uint32_t>(Catch + 12, 0x1150);
  writeLE<int32_t>(Catch + 16, 0x20);

  uint8_t *IPMap = X + 0xd0;
  writeLE<uint32_t>(IPMap, 0x1000);
  writeLE<int32_t>(IPMap + 4, -1);
  writeLE<uint32_t>(IPMap + 8, 0x1010);
  writeLE<int32_t>(IPMap + 12, 0);

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  F.Personality = ExceptionPersonality::Unknown;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::CxxFrameHandler3);
  ASSERT_TRUE(Decoded.Cxx.has_value());
  EXPECT_TRUE(Decoded.Cxx->hasValidStateGraph());
  ASSERT_EQ(Decoded.Cxx->TryBlocks.size(), 1u);
  ASSERT_EQ(Decoded.Cxx->TryBlocks[0].Handlers.size(), 1u);
  EXPECT_EQ(Decoded.Cxx->TryBlocks[0].Handlers[0].HandlerVA, Img.Base + 0x1150);
}

namespace {

/// Lay out a minimal but valid FH3 graph whose `FuncInfo` word is \p MagicWord.
///
/// Every field the magic declares is filled with something meaningful: an
/// empty exception-specification list at 0x30e0 and `FI_EHS_FLAG`.  Every word
/// *past* the declared end is filled with a pattern that is not a valid RVA,
/// so a decoder reading the newest layout out of an older record is caught
/// picking the pattern up rather than quietly producing a plausible answer.
BinaryImage makeFH3MagicImage(uint32_t MagicWord) {
  BinaryImage Img = makeX64ExceptionImage(0x200);
  addPersonalityImport(Img, Img.Base + 0x1100, "__CxxFrameHandler3");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0x3040);

  uint8_t *FI = X + 0x40;
  writeLE<uint32_t>(FI, MagicWord);
  writeLE<int32_t>(FI + 4, 1);       // maxState
  writeLE<uint32_t>(FI + 8, 0x3080); // dispUnwindMap
  writeLE<uint32_t>(FI + 12, 0);     // nTryBlocks
  writeLE<uint32_t>(FI + 16, 0);     // dispTryBlockMap
  writeLE<uint32_t>(FI + 20, 1);     // nIPMapEntries
  writeLE<uint32_t>(FI + 24, 0x30d0);
  writeLE<int32_t>(FI + 28, -8); // dispUnwindHelp

  const uint32_t Magic = MagicWord & 0x1FFFFFFFu;
  writeLE<uint32_t>(FI + 32, Magic >= 0x19930521 ? 0x30e0 : 0xdeadbeef);
  writeLE<uint32_t>(FI + 36, Magic >= 0x19930522 ? 1 : 0xdeadbeef);

  uint8_t *Unwind = X + 0x80;
  writeLE<int32_t>(Unwind, -1);
  writeLE<uint32_t>(Unwind + 4, 0);

  uint8_t *IPMap = X + 0xd0;
  writeLE<uint32_t>(IPMap, 0x1000);
  writeLE<int32_t>(IPMap + 4, -1);

  uint8_t *ESTypeList = X + 0xe0;
  writeLE<int32_t>(ESTypeList, 0);     // throw()
  writeLE<uint32_t>(ESTypeList + 4, 0);

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));
  return Img;
}

BinaryImage makeARM32FH3Image() {
  BinaryImage Img = makeX64ExceptionImage(0x300);
  Img.Arch = Arch::ARM;
  Img.Bits = Bitness::Bits32;
  Img.Mode = InstructionMode::Thumb;
  Img.Base = 0x10000000;
  Img.Segments[0].VA = Img.Base + 0x1000;
  Img.Segments[1].VA = Img.Base + 0x3000;
  return Img;
}

} // namespace

TEST(COFFExceptionParser, StopsLegacyFH3RecordsAtTheirDeclaredLength) {
  // EH_MAGIC_NUMBER1 ends after the unwind-help displacement: neither the
  // exception-specification list nor EHFlags is part of the record, and the
  // unreadable words that follow must not be mistaken for them.
  {
    BinaryImage Img = makeFH3MagicImage(0x19930520);
    coff_loader::resolveExceptionHandlers(Img);
    const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
    ASSERT_TRUE(Decoded.Cxx.has_value());
    EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
    EXPECT_EQ(Decoded.Cxx->Version, CxxFuncInfoVersion::Original);
    EXPECT_FALSE(Decoded.Cxx->hasExceptionSpecification());
    EXPECT_EQ(Decoded.Cxx->Flags, 0u);
    EXPECT_FALSE(Decoded.Cxx->IsSynchronous);
  }

  // EH_MAGIC_NUMBER2 owns the list but still not EHFlags, so `throw()` is
  // recovered while the /EHs claim stays unmade.
  {
    BinaryImage Img = makeFH3MagicImage(0x19930521);
    coff_loader::resolveExceptionHandlers(Img);
    const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
    ASSERT_TRUE(Decoded.Cxx.has_value());
    EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
    EXPECT_EQ(Decoded.Cxx->Version, CxxFuncInfoVersion::WithExceptionSpecs);
    EXPECT_TRUE(Decoded.Cxx->hasExceptionSpecification());
    EXPECT_TRUE(Decoded.Cxx->ExceptionSpecTypes.empty());
    EXPECT_EQ(Decoded.Cxx->Flags, 0u);
    EXPECT_FALSE(Decoded.Cxx->IsSynchronous);
  }

  // EH_MAGIC_NUMBER3 owns both.
  {
    BinaryImage Img = makeFH3MagicImage(0x19930522);
    coff_loader::resolveExceptionHandlers(Img);
    const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
    ASSERT_TRUE(Decoded.Cxx.has_value());
    EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
    EXPECT_EQ(Decoded.Cxx->Version, CxxFuncInfoVersion::WithEHFlags);
    EXPECT_TRUE(Decoded.Cxx->IsSynchronous);
  }
}

TEST(COFFExceptionParser, SplitsBBTFlagsOutOfTheFH3MagicField) {
  // `magicNumber` is 29 bits wide; BBT sets the three bits above it in place.
  // A decoder that compares the whole word rejects every BBT-processed image.
  BinaryImage Img = makeFH3MagicImage(0x19930522 | (1u << 29));
  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  ASSERT_TRUE(Decoded.Cxx.has_value());
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(Decoded.Cxx->Magic, 0x19930522u);
  EXPECT_EQ(Decoded.Cxx->BBTFlags, 1u);
  EXPECT_EQ(Decoded.Cxx->Version, CxxFuncInfoVersion::WithEHFlags);
}

TEST(COFFExceptionParser, DecodesFH3ExceptionSpecificationList) {
  BinaryImage Img = makeX64ExceptionImage(0x200);
  addPersonalityImport(Img, Img.Base + 0x1100, "__CxxFrameHandler3");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0x3040);

  uint8_t *FI = X + 0x40;
  writeLE<uint32_t>(FI, 0x19930522);
  writeLE<int32_t>(FI + 4, 1);
  writeLE<uint32_t>(FI + 8, 0x3080);
  writeLE<uint32_t>(FI + 12, 0);
  writeLE<uint32_t>(FI + 16, 0);
  writeLE<uint32_t>(FI + 20, 1);
  writeLE<uint32_t>(FI + 24, 0x30d0);
  writeLE<int32_t>(FI + 28, -8);
  writeLE<uint32_t>(FI + 32, 0x30e0); // dispESTypeList
  writeLE<uint32_t>(FI + 36, 1);      // FI_EHS_FLAG

  uint8_t *Unwind = X + 0x80;
  writeLE<int32_t>(Unwind, -1);
  writeLE<uint32_t>(Unwind + 4, 0);

  uint8_t *IPMap = X + 0xd0;
  writeLE<uint32_t>(IPMap, 0x1000);
  writeLE<int32_t>(IPMap + 4, -1);

  uint8_t *ESTypeList = X + 0xe0;
  writeLE<int32_t>(ESTypeList, 2);         // nCount
  writeLE<uint32_t>(ESTypeList + 4, 0x30f0); // dispTypeArray

  uint8_t *Specs = X + 0xf0;
  writeLE<uint32_t>(Specs, 0x40);      // adjectives
  writeLE<uint32_t>(Specs + 4, 0x3180);  // type descriptor
  writeLE<uint32_t>(Specs + 20, 0);
  writeLE<uint32_t>(Specs + 24, 0x3190);

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(Decoded.Cxx.has_value());
  EXPECT_TRUE(Decoded.Cxx->hasExceptionSpecification());
  EXPECT_TRUE(Decoded.Cxx->IsSynchronous);
  ASSERT_EQ(Decoded.Cxx->ExceptionSpecTypes.size(), 2u);
  EXPECT_EQ(Decoded.Cxx->ExceptionSpecTypes[0].Adjectives, 0x40u);
  EXPECT_EQ(Decoded.Cxx->ExceptionSpecTypes[0].TypeDescriptorVA,
            Img.Base + 0x3180);
  EXPECT_EQ(Decoded.Cxx->ExceptionSpecTypes[1].TypeDescriptorVA,
            Img.Base + 0x3190);
}

TEST(COFFExceptionParser, UsesARM32HandlerTypeStrideForFH3CatchMaps) {
  BinaryImage Img = makeARM32FH3Image();
  addPersonalityImport(Img, Img.Base + 0x1100, "__CxxFrameHandler3");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0x3040);

  uint8_t *FI = X + 0x40;
  writeLE<uint32_t>(FI, 0x19930522);
  writeLE<int32_t>(FI + 4, 2);
  writeLE<uint32_t>(FI + 8, 0x3080);
  writeLE<uint32_t>(FI + 12, 1);
  writeLE<uint32_t>(FI + 16, 0x3090);
  writeLE<uint32_t>(FI + 20, 1);
  writeLE<uint32_t>(FI + 24, 0x30d0);
  writeLE<int32_t>(FI + 28, -8);
  writeLE<uint32_t>(FI + 32, 0);
  writeLE<uint32_t>(FI + 36, 1);

  uint8_t *Unwind = X + 0x80;
  writeLE<int32_t>(Unwind, -1);
  writeLE<uint32_t>(Unwind + 4, 0);
  writeLE<int32_t>(Unwind + 8, 0);
  writeLE<uint32_t>(Unwind + 12, 0);

  uint8_t *Try = X + 0x90;
  writeLE<int32_t>(Try, 0);
  writeLE<int32_t>(Try + 4, 0);
  writeLE<int32_t>(Try + 8, 1);
  writeLE<uint32_t>(Try + 12, 2);
  writeLE<uint32_t>(Try + 16, 0x30b0);

  // ARM32 HandlerType has four 32-bit fields.  The handler code RVAs retain
  // their Thumb bit in the table and are normalized by the decoder.
  uint8_t *Catch = X + 0xb0;
  writeLE<uint32_t>(Catch, 0x40);
  writeLE<uint32_t>(Catch + 4, 0);
  writeLE<int32_t>(Catch + 8, 0);
  writeLE<uint32_t>(Catch + 12, 0x1151);
  writeLE<uint32_t>(Catch + 16, 0x80);
  writeLE<uint32_t>(Catch + 20, 0);
  writeLE<int32_t>(Catch + 24, 0);
  writeLE<uint32_t>(Catch + 28, 0x1161);

  uint8_t *IPMap = X + 0xd0;
  writeLE<uint32_t>(IPMap, 0x1001);
  writeLE<int32_t>(IPMap + 4, 0);

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.PersonalityVA = Img.Base + 0x1101;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(Decoded.Cxx.has_value());
  ASSERT_EQ(Decoded.Cxx->TryBlocks.size(), 1u);
  ASSERT_EQ(Decoded.Cxx->TryBlocks.front().Handlers.size(), 2u);
  const CxxCatchHandler &First =
      Decoded.Cxx->TryBlocks.front().Handlers[0];
  const CxxCatchHandler &Second =
      Decoded.Cxx->TryBlocks.front().Handlers[1];
  EXPECT_EQ(First.Adjectives, 0x40u);
  EXPECT_EQ(First.HandlerVA, Img.Base + 0x1150);
  EXPECT_EQ(First.ParentFrameOffset, 0);
  EXPECT_EQ(Second.Adjectives, 0x80u);
  EXPECT_EQ(Second.HandlerVA, Img.Base + 0x1160);
  EXPECT_EQ(Second.ParentFrameOffset, 0);
}

TEST(COFFExceptionParser, UsesARM32HandlerTypeStrideForFH3ESTypeLists) {
  BinaryImage Img = makeARM32FH3Image();
  addPersonalityImport(Img, Img.Base + 0x1100, "__CxxFrameHandler3");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0x3040);

  uint8_t *FI = X + 0x40;
  writeLE<uint32_t>(FI, 0x19930522);
  writeLE<int32_t>(FI + 4, 1);
  writeLE<uint32_t>(FI + 8, 0x3080);
  writeLE<uint32_t>(FI + 12, 0);
  writeLE<uint32_t>(FI + 16, 0);
  writeLE<uint32_t>(FI + 20, 1);
  writeLE<uint32_t>(FI + 24, 0x30d0);
  writeLE<int32_t>(FI + 28, -8);
  writeLE<uint32_t>(FI + 32, 0x30e0);
  writeLE<uint32_t>(FI + 36, 1);

  uint8_t *Unwind = X + 0x80;
  writeLE<int32_t>(Unwind, -1);
  writeLE<uint32_t>(Unwind + 4, 0);
  uint8_t *IPMap = X + 0xd0;
  writeLE<uint32_t>(IPMap, 0x1001);
  writeLE<int32_t>(IPMap + 4, -1);

  uint8_t *ESTypeList = X + 0xe0;
  writeLE<int32_t>(ESTypeList, 2);
  writeLE<uint32_t>(ESTypeList + 4, 0x3100);
  uint8_t *Specs = X + 0x100;
  writeLE<uint32_t>(Specs, 0x40);
  writeLE<uint32_t>(Specs + 4, 0x3180);
  writeLE<uint32_t>(Specs + 8, 0xaaaaaaaa);
  writeLE<uint32_t>(Specs + 12, 0xbbbbbbbb);
  writeLE<uint32_t>(Specs + 16, 0x80);
  writeLE<uint32_t>(Specs + 20, 0x3190);
  writeLE<uint32_t>(Specs + 24, 0xcccccccc);
  writeLE<uint32_t>(Specs + 28, 0xdddddddd);

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1200};
  F.PersonalityVA = Img.Base + 0x1101;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(Decoded.Cxx.has_value());
  ASSERT_EQ(Decoded.Cxx->ExceptionSpecTypes.size(), 2u);
  EXPECT_EQ(Decoded.Cxx->ExceptionSpecTypes[0].Adjectives, 0x40u);
  EXPECT_EQ(Decoded.Cxx->ExceptionSpecTypes[0].TypeDescriptorVA,
            Img.Base + 0x3180);
  EXPECT_EQ(Decoded.Cxx->ExceptionSpecTypes[1].Adjectives, 0x80u);
  EXPECT_EQ(Decoded.Cxx->ExceptionSpecTypes[1].TypeDescriptorVA,
            Img.Base + 0x3190);
}

TEST(COFFExceptionParser, AcceptsFH3IPMapAcrossSharedFuncInfoGroup) {
  BinaryImage Img = makeX64ExceptionImage(0x200);
  addPersonalityImport(Img, Img.Base + 0x1100, "__CxxFrameHandler3");
  addPersonalityImport(Img, Img.Base + 0x1110, "__CxxFrameHandler3");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0x3040);
  writeLE<uint32_t>(X + 4, 0x3040);

  uint8_t *FI = X + 0x40;
  writeLE<uint32_t>(FI, 0x19930522);
  writeLE<int32_t>(FI + 4, 2);
  writeLE<uint32_t>(FI + 8, 0x3080);
  writeLE<uint32_t>(FI + 12, 1);
  writeLE<uint32_t>(FI + 16, 0x3090);
  writeLE<uint32_t>(FI + 20, 2);
  writeLE<uint32_t>(FI + 24, 0x30d0);
  writeLE<int32_t>(FI + 28, -8);
  writeLE<uint32_t>(FI + 32, 0);
  writeLE<uint32_t>(FI + 36, 1);

  uint8_t *Unwind = X + 0x80;
  writeLE<int32_t>(Unwind, -1);
  writeLE<uint32_t>(Unwind + 4, 0);
  writeLE<int32_t>(Unwind + 8, 0);
  writeLE<uint32_t>(Unwind + 12, 0);

  uint8_t *Try = X + 0x90;
  writeLE<int32_t>(Try, 0);
  writeLE<int32_t>(Try + 4, 0);
  writeLE<int32_t>(Try + 8, 1);
  writeLE<uint32_t>(Try + 12, 1);
  writeLE<uint32_t>(Try + 16, 0x30b0);

  uint8_t *Catch = X + 0xb0;
  writeLE<uint32_t>(Catch, 0x40);
  writeLE<uint32_t>(Catch + 4, 0);
  writeLE<int32_t>(Catch + 8, 0);
  writeLE<uint32_t>(Catch + 12, 0x1150);
  writeLE<int32_t>(Catch + 16, 0);

  uint8_t *IPMap = X + 0xd0;
  writeLE<uint32_t>(IPMap, 0x1000);
  writeLE<int32_t>(IPMap + 4, 0);
  writeLE<uint32_t>(IPMap + 8, 0x1150);
  writeLE<int32_t>(IPMap + 12, 1);

  ExceptionFunction Parent;
  Parent.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1080};
  Parent.PersonalityVA = Img.Base + 0x1100;
  Parent.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(Parent));

  ExceptionFunction CatchFunclet;
  CatchFunclet.CodeRange = {Img.Base + 0x1150, Img.Base + 0x1180};
  CatchFunclet.PersonalityVA = Img.Base + 0x1110;
  CatchFunclet.HandlerDataVA = Img.Base + 0x3004;
  Img.ExceptionMetadata.Functions.push_back(std::move(CatchFunclet));

  coff_loader::resolveExceptionHandlers(Img);
  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 2u);
  const ExceptionFunction &DecodedParent = Img.ExceptionMetadata.Functions[0];
  const ExceptionFunction &DecodedCatch = Img.ExceptionMetadata.Functions[1];
  EXPECT_EQ(DecodedParent.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(DecodedCatch.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(DecodedParent.Cxx.has_value());
  ASSERT_TRUE(DecodedCatch.Cxx.has_value());
  EXPECT_EQ(DecodedParent.Cxx->NativeFuncInfoVA, Img.Base + 0x3040);
  EXPECT_EQ(DecodedCatch.Cxx->NativeFuncInfoVA,
            DecodedParent.Cxx->NativeFuncInfoVA);
  EXPECT_TRUE(DecodedParent.Cxx->IsSeparated);
  EXPECT_FALSE(DecodedParent.Cxx->IsCatchFunclet);
  EXPECT_TRUE(DecodedCatch.Cxx->IsSeparated);
  EXPECT_TRUE(DecodedCatch.Cxx->IsCatchFunclet);
  EXPECT_TRUE(DecodedParent.Cxx->hasValidStateGraph());
  EXPECT_TRUE(DecodedCatch.Cxx->hasValidStateGraph());
}

TEST(COFFExceptionParser, StopsFH3AggregateGraphExpansionAtBudget) {
  BinaryImage Img = makeX64ExceptionImage(0x100);
  addPersonalityImport(Img, Img.Base + 0x1100, "__CxxFrameHandler3");
  uint8_t *X = Img.Segments[1].Data.data();
  writeLE<uint32_t>(X, 0x3040); // handler data -> FuncInfo3

  uint8_t *FI = X + 0x40;
  writeLE<uint32_t>(FI, 0x19930522);
  writeLE<int32_t>(FI + 4, 1 << 16); // valid per-table maximum
  writeLE<uint32_t>(FI + 8, 0);
  writeLE<uint32_t>(FI + 12, 1); // individually valid, aggregate is too large
  writeLE<uint32_t>(FI + 16, 0);
  writeLE<uint32_t>(FI + 20, 0);
  writeLE<uint32_t>(FI + 24, 0);
  writeLE<int32_t>(FI + 28, 0);
  writeLE<uint32_t>(FI + 32, 0);
  writeLE<uint32_t>(FI + 36, 1);

  ExceptionFunction F;
  F.CodeRange = {Img.Base + 0x1000, Img.Base + 0x1080};
  F.PersonalityVA = Img.Base + 0x1100;
  F.HandlerDataVA = Img.Base + 0x3000;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));

  coff_loader::resolveExceptionHandlers(Img);
  const ExceptionFunction &Decoded = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(Decoded.Personality, ExceptionPersonality::CxxFrameHandler3);
  EXPECT_EQ(Decoded.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_FALSE(Decoded.Cxx.has_value());
  ASSERT_FALSE(Decoded.Diagnostics.empty());
  EXPECT_NE(Decoded.Diagnostics.back().find("aggregate language graph"),
            std::string::npos);
}

} // namespace
