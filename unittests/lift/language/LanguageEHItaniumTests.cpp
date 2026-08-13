//===- LanguageEHItaniumTests.cpp - Itanium LSDA tests ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "LanguageEHTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::dwarf_eh;
using namespace neverd::language_eh_test;

//===----------------------------------------------------------------------===//
// Itanium LSDA
//===----------------------------------------------------------------------===//

/// Build a `.gcc_except_table` with one catch handler and one cleanup.
///
/// Layout mirrors what clang emits for
///   `try { may_throw(); } catch (int) { ... }` with a live destructor.
struct LSDABytes {
  std::vector<uint8_t> Bytes;
  va_t TypeInfoVA = 0;
};

LSDABytes buildCatchLSDA(va_t LSDAVA, va_t FuncVA, va_t TypeInfoVA) {
  ByteBuilder B;
  B.u8(0xff); // DW_EH_PE_omit: landing pad base defaults to the function
  B.u8(0x00); // DW_EH_PE_absptr type table entries

  // The type-table offset is measured from just after the uleb field.  The
  // table base sits past the action table; compute it after the body is laid
  // out by building the body separately.
  ByteBuilder Body;
  {
    ByteBuilder CallSites;
    // Region 1: protected call, landing pad with an action chain.
    CallSites.u32(0x10); // start
    CallSites.u32(0x10); // length
    CallSites.u32(0x40); // landing pad
    CallSites.uleb(1);   // first action (1-based)
    // Region 2: cleanup-only region with a landing pad and no action.
    CallSites.u32(0x20);
    CallSites.u32(0x08);
    CallSites.u32(0x60);
    CallSites.uleb(0);
    // Region 3: unprotected tail.
    CallSites.u32(0x30);
    CallSites.u32(0x08);
    CallSites.u32(0);
    CallSites.uleb(0);

    Body.u8(0x03); // DW_EH_PE_udata4 call sites
    Body.uleb(CallSites.size());
    for (uint8_t Byte : CallSites.data())
      Body.u8(Byte);

    // Action table: catch type 1, then chain to a cleanup.  The chain link is
    // self-relative to its own position, which is table offset 1, so reaching
    // the record at offset 2 is a displacement of +1.
    Body.sleb(1);
    Body.sleb(1);
    // Entry at offset 2: cleanup, end of chain.
    Body.sleb(0);
    Body.sleb(0);
  }

  // The type-table base is the address just past the last entry, and entries
  // grow downward from it.  Place it immediately after the body.
  const size_t HeaderPrefix = B.size();
  // Emit a one-byte uleb for the offset; the layout below keeps it under 128.
  const size_t OffsetFieldSize = 1;
  const size_t TypeTableBaseOffset = Body.size() + 8; // one 8-byte entry
  B.uleb(TypeTableBaseOffset);
  EXPECT_EQ(B.size() - HeaderPrefix, OffsetFieldSize);

  const size_t AfterOffsetField = B.size();
  for (uint8_t Byte : Body.data())
    B.u8(Byte);
  // Entry 1 lives at base - 8, which is exactly here.
  B.u64(TypeInfoVA);

  EXPECT_EQ(AfterOffsetField + TypeTableBaseOffset, B.size());
  (void)LSDAVA;
  (void)FuncVA;

  LSDABytes Result;
  Result.Bytes = B.data();
  Result.TypeInfoVA = TypeInfoVA;
  return Result;
}
TEST(ItaniumLSDA, DecodesCallSitesActionsAndTypes) {
  BinaryImage Img = makeImage();
  const va_t FuncVA = kTextVA + 0x100;
  const va_t LSDAVA = kDataVA + 0x200;
  const va_t TypeInfoVA = kDataVA + 0x600;

  // A `std::type_info` is a vptr followed by a pointer to the mangled name.
  ByteBuilder TypeInfo;
  TypeInfo.u64(kDataVA + 0x700); // vptr
  TypeInfo.u64(kDataVA + 0x680); // name pointer
  writeData(Img, TypeInfoVA, TypeInfo.data());
  const char kName[] = "i";
  writeData(Img, kDataVA + 0x680,
            std::vector<uint8_t>(kName, kName + sizeof(kName)));

  LSDABytes Table = buildCatchLSDA(LSDAVA, FuncVA, TypeInfoVA);
  writeData(Img, LSDAVA, Table.Bytes);

  LSDAParseRequest Req;
  Req.LSDAVA = LSDAVA;
  Req.FunctionStart = FuncVA;
  Req.FunctionEnd = FuncVA + 0x100;
  LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});

  ASSERT_TRUE(Result.Info.has_value());
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);
  const ItaniumEHInfo &Info = *Result.Info;

  EXPECT_EQ(Info.LandingPadBase, FuncVA);
  ASSERT_EQ(Info.CallSites.size(), 3u);

  EXPECT_EQ(Info.CallSites[0].GuardedRange.Begin, FuncVA + 0x10);
  EXPECT_EQ(Info.CallSites[0].GuardedRange.End, FuncVA + 0x20);
  EXPECT_EQ(Info.CallSites[0].LandingPadVA, FuncVA + 0x40);
  ASSERT_TRUE(Info.CallSites[0].FirstActionOffset.has_value());
  EXPECT_EQ(*Info.CallSites[0].FirstActionOffset, 0u);

  EXPECT_EQ(Info.CallSites[1].LandingPadVA, FuncVA + 0x60);
  EXPECT_FALSE(Info.CallSites[1].FirstActionOffset.has_value());

  EXPECT_EQ(Info.CallSites[2].LandingPadVA, 0u);

  ASSERT_EQ(Info.Actions.size(), 2u);
  EXPECT_TRUE(Info.Actions[0].isCatch());
  EXPECT_EQ(Info.Actions[0].TypeFilter, 1);
  ASSERT_TRUE(Info.Actions[0].NextActionOffset.has_value());
  EXPECT_EQ(*Info.Actions[0].NextActionOffset, 2u);
  EXPECT_TRUE(Info.Actions[1].isCleanup());
  EXPECT_FALSE(Info.Actions[1].NextActionOffset.has_value());

  ASSERT_EQ(Info.TypeTable.size(), 1u);
  EXPECT_EQ(Info.TypeTable[0].Index, 1u);
  EXPECT_EQ(Info.TypeTable[0].TypeInfoVA, TypeInfoVA);
  EXPECT_EQ(Info.TypeTable[0].TypeName, "i");
  EXPECT_FALSE(Info.TypeTable[0].IsCatchAll);
  EXPECT_FALSE(Info.isCleanupOnly());
}

TEST(ItaniumLSDA, FollowsAnActionChainThatPointsBackwards) {
  // Clang emits action records in reverse and links each to the one *before*
  // it, so a chain link is routinely negative.  Reading the link as an offset
  // from the table base instead of from its own position would send this
  // chain off the front of the table.
  BinaryImage Img = makeImage();
  const va_t FuncVA = kTextVA + 0x100;
  const va_t LSDAVA = kDataVA + 0x200;

  ByteBuilder B;
  B.u8(0xff);
  B.u8(0xff);
  ByteBuilder CallSites;
  CallSites.u32(0x00);
  CallSites.u32(0x20);
  CallSites.u32(0x40);
  CallSites.uleb(3); // names the record at table offset 2
  B.u8(0x03);
  B.uleb(CallSites.size());
  for (uint8_t Byte : CallSites.data())
    B.u8(Byte);
  // Offset 0: catch type 1, end of chain.
  B.sleb(1);
  B.sleb(0);
  // Offset 2: catch type 2, chaining back to offset 0.  The link field is at
  // offset 3, so the displacement is -3.
  B.sleb(2);
  B.sleb(-3);
  writeData(Img, LSDAVA, B.data());

  LSDAParseRequest Req;
  Req.LSDAVA = LSDAVA;
  Req.FunctionStart = FuncVA;
  Req.FunctionEnd = FuncVA + 0x100;
  LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});

  ASSERT_TRUE(Result.Info.has_value());
  ASSERT_EQ(Result.Info->Actions.size(), 2u);
  EXPECT_EQ(Result.Info->Actions[0].TableOffset, 0u);
  EXPECT_EQ(Result.Info->Actions[0].TypeFilter, 1);
  EXPECT_FALSE(Result.Info->Actions[0].NextActionOffset.has_value());
  EXPECT_EQ(Result.Info->Actions[1].TableOffset, 2u);
  EXPECT_EQ(Result.Info->Actions[1].TypeFilter, 2);
  ASSERT_TRUE(Result.Info->Actions[1].NextActionOffset.has_value());
  EXPECT_EQ(*Result.Info->Actions[1].NextActionOffset, 0u);
}

TEST(ItaniumLSDA, ReportsCleanupOnlyRecords) {
  BinaryImage Img = makeImage();
  const va_t FuncVA = kTextVA + 0x100;
  const va_t LSDAVA = kDataVA + 0x200;

  ByteBuilder B;
  B.u8(0xff); // omit landing pad base
  B.u8(0xff); // omit type table
  ByteBuilder CallSites;
  CallSites.u32(0x00);
  CallSites.u32(0x20);
  CallSites.u32(0x40);
  CallSites.uleb(0);
  B.u8(0x03);
  B.uleb(CallSites.size());
  for (uint8_t Byte : CallSites.data())
    B.u8(Byte);
  writeData(Img, LSDAVA, B.data());

  LSDAParseRequest Req;
  Req.LSDAVA = LSDAVA;
  Req.FunctionStart = FuncVA;
  Req.FunctionEnd = FuncVA + 0x100;
  LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});

  ASSERT_TRUE(Result.Info.has_value());
  EXPECT_TRUE(Result.Info->isCleanupOnly());
  ASSERT_EQ(Result.Info->CallSites.size(), 1u);
  EXPECT_EQ(Result.Info->CallSites[0].LandingPadVA, FuncVA + 0x40);
  EXPECT_TRUE(Result.Info->TypeTable.empty());
}

TEST(ItaniumLSDA, RecognizesCatchAllTypeSlot) {
  BinaryImage Img = makeImage();
  const va_t FuncVA = kTextVA + 0x100;
  const va_t LSDAVA = kDataVA + 0x200;
  LSDABytes Table = buildCatchLSDA(LSDAVA, FuncVA, /*TypeInfoVA=*/0);
  writeData(Img, LSDAVA, Table.Bytes);

  LSDAParseRequest Req;
  Req.LSDAVA = LSDAVA;
  Req.FunctionStart = FuncVA;
  Req.FunctionEnd = FuncVA + 0x100;
  LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});

  ASSERT_TRUE(Result.Info.has_value());
  ASSERT_EQ(Result.Info->TypeTable.size(), 1u);
  EXPECT_TRUE(Result.Info->TypeTable[0].IsCatchAll);
  EXPECT_TRUE(Result.Info->TypeTable[0].TypeName.empty());
}

TEST(ItaniumLSDA, FlagsCallSiteRangesThatLeaveTheirFunction) {
  BinaryImage Img = makeImage();
  const va_t FuncVA = kTextVA + 0x100;
  const va_t LSDAVA = kDataVA + 0x200;

  ByteBuilder B;
  B.u8(0xff);
  B.u8(0xff);
  ByteBuilder CallSites;
  CallSites.u32(0x00);
  CallSites.u32(0x800); // far past the declared function end
  CallSites.u32(0x40);
  CallSites.uleb(0);
  B.u8(0x03);
  B.uleb(CallSites.size());
  for (uint8_t Byte : CallSites.data())
    B.u8(Byte);
  writeData(Img, LSDAVA, B.data());

  LSDAParseRequest Req;
  Req.LSDAVA = LSDAVA;
  Req.FunctionStart = FuncVA;
  Req.FunctionEnd = FuncVA + 0x100;
  LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});

  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Partial);
  ASSERT_TRUE(Result.Info.has_value());
  EXPECT_EQ(Result.Info->CallSites.size(), 1u);
}

TEST(ItaniumLSDA, RejectsCallSiteTablePastMappedData) {
  BinaryImage Img = makeImage();
  const va_t LSDAVA = kDataVA + 0x200;
  ByteBuilder B;
  B.u8(0xff);
  B.u8(0xff);
  B.u8(0x03);
  B.uleb(0x100000); // longer than the record, and than the segment
  writeData(Img, LSDAVA, B.data());

  LSDAParseRequest Req;
  Req.LSDAVA = LSDAVA;
  Req.FunctionStart = kTextVA + 0x100;
  Req.FunctionEnd = kTextVA + 0x200;
  LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Malformed);
  EXPECT_FALSE(Result.Info.has_value());
}

TEST(ItaniumLSDA, DecodesExceptionSpecificationLists) {
  BinaryImage Img = makeImage();
  const va_t FuncVA = kTextVA + 0x100;
  const va_t LSDAVA = kDataVA + 0x200;
  const va_t TypeInfoVA = kDataVA + 0x600;

  ByteBuilder B;
  B.u8(0xff);
  B.u8(0x00); // absptr type table

  ByteBuilder Body;
  ByteBuilder CallSites;
  CallSites.u32(0x00);
  CallSites.u32(0x20);
  CallSites.u32(0x40);
  CallSites.uleb(1);
  Body.u8(0x03);
  Body.uleb(CallSites.size());
  for (uint8_t Byte : CallSites.data())
    Body.u8(Byte);
  // A negative filter selects exception-specification list 1.
  Body.sleb(-1);
  Body.sleb(0);

  const size_t TypeTableBaseOffset = Body.size() + 8;
  B.uleb(TypeTableBaseOffset);
  const size_t AfterOffsetField = B.size();
  for (uint8_t Byte : Body.data())
    B.u8(Byte);
  B.u64(TypeInfoVA); // type-table entry 1, below the base
  ASSERT_EQ(AfterOffsetField + TypeTableBaseOffset, B.size());
  // The specification list grows upward from the base: index 1 is at base + 0.
  B.uleb(1);
  B.uleb(0);
  writeData(Img, LSDAVA, B.data());

  ByteBuilder TypeInfo;
  TypeInfo.u64(kDataVA + 0x700);
  TypeInfo.u64(kDataVA + 0x680);
  writeData(Img, TypeInfoVA, TypeInfo.data());
  const char kName[] = "St9exception";
  writeData(Img, kDataVA + 0x680,
            std::vector<uint8_t>(kName, kName + sizeof(kName)));

  LSDAParseRequest Req;
  Req.LSDAVA = LSDAVA;
  Req.FunctionStart = FuncVA;
  Req.FunctionEnd = FuncVA + 0x100;
  LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});

  ASSERT_TRUE(Result.Info.has_value());
  ASSERT_EQ(Result.Info->Actions.size(), 1u);
  EXPECT_TRUE(Result.Info->Actions[0].isExceptionSpecification());
  ASSERT_EQ(Result.Info->ExceptionSpecs.size(), 1u);
  EXPECT_EQ(Result.Info->ExceptionSpecs[0].Index, 1u);
  ASSERT_EQ(Result.Info->ExceptionSpecs[0].TypeIndices.size(), 1u);
  EXPECT_EQ(Result.Info->ExceptionSpecs[0].TypeIndices[0], 1u);
}
//===----------------------------------------------------------------------===//
// End-to-end normalization
//===----------------------------------------------------------------------===//

TEST(ItaniumEHDriver, NormalizesFrameSectionIntoExceptionMetadata) {
  BinaryImage Img = makeImage();
  const va_t FuncVA = kTextVA + 0x100;
  const va_t SectionVA = kDataVA;
  const va_t PersonalityVA = kTextVA + 0x800;
  const va_t LSDAVA = kDataVA + 0x400;

  Symbol Personality;
  Personality.Name = "__gxx_personality_v0";
  Personality.Addr = PersonalityVA;
  Personality.IsFunc = true;
  Img.Symbols.push_back(std::move(Personality));

  FrameBytes Frame =
      buildSimpleFrame(SectionVA, FuncVA, 0x80, "zPLR", PersonalityVA, LSDAVA);
  writeData(Img, SectionVA, Frame.Bytes);

  Section EhFrame;
  EhFrame.Name = ".eh_frame";
  EhFrame.VA = SectionVA;
  EhFrame.Size = Frame.Bytes.size();
  EhFrame.Data = Frame.Bytes;
  Img.Sections.push_back(std::move(EhFrame));

  LSDABytes Table = buildCatchLSDA(LSDAVA, FuncVA, /*TypeInfoVA=*/0);
  writeData(Img, LSDAVA, Table.Bytes);

  parseItaniumExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(F.Encoding, ExceptionEncoding::DwarfFDE);
  EXPECT_EQ(F.model(), ExceptionModel::Itanium);
  EXPECT_EQ(F.Personality, ExceptionPersonality::GxxPersonalityV0);
  EXPECT_EQ(F.PersonalityName, "__gxx_personality_v0");
  EXPECT_EQ(F.CodeRange.Begin, FuncVA);
  EXPECT_EQ(F.CodeRange.End, FuncVA + 0x80);
  ASSERT_TRUE(F.Dwarf.has_value());
  ASSERT_TRUE(F.Itanium.has_value());
  EXPECT_EQ(F.Itanium->CallSites.size(), 3u);
  EXPECT_TRUE(Img.ExceptionMetadata.hasModel(ExceptionModel::Itanium));
  EXPECT_FALSE(F.canRegenerateLanguageMetadata());

  // The FDE proves both the entry and the extent, which is stronger evidence
  // than any heuristic scan produces.
  const ExceptionFunction *Found =
      Img.ExceptionMetadata.findFunction(FuncVA + 0x10);
  ASSERT_NE(Found, nullptr);
  EXPECT_EQ(Found->CodeRange.Begin, FuncVA);
}

TEST(ItaniumEHDriver, IgnoresAnImageWithoutAFrameSection) {
  BinaryImage Img = makeImage();
  parseItaniumExceptions(Img);
  EXPECT_TRUE(Img.ExceptionMetadata.Functions.empty());
  EXPECT_FALSE(Img.ExceptionMetadata.hasModel(ExceptionModel::Itanium));
}

} // namespace
