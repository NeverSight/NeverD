//===- LanguageEHItaniumSJLJTests.cpp - Itanium setjmp/longjmp table tests -===//
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
// setjmp/longjmp call-site tables
//===----------------------------------------------------------------------===//

/// An SJLJ LSDA with three call sites and a one-slot type table.
///
/// \p CallSiteEncoding goes in the header byte that the address form would
/// read its columns with.  It is a parameter because it has to make no
/// difference: no personality consults it on this path, so producers disagree
/// about what belongs there.
std::vector<uint8_t> buildSJLJLSDA(uint8_t CallSiteEncoding, va_t TypeInfoVA) {
  ByteBuilder B;
  B.u8(0xff); // landing-pad base omitted; it defaults to the function start
  B.u8(0x00); // DW_EH_PE_absptr type-table slots

  ByteBuilder Body;
  {
    ByteBuilder CallSites;
    // Index 1: selector 0, and no action at all.
    CallSites.uleb(0);
    CallSites.uleb(0);
    // Index 2: selector 1, action record 1 -- table offset 0, a catch.
    CallSites.uleb(1);
    CallSites.uleb(1);
    // Index 3: selector 2, action record 3 -- table offset 2, a cleanup.
    CallSites.uleb(2);
    CallSites.uleb(3);

    Body.u8(CallSiteEncoding);
    Body.uleb(CallSites.size());
    for (uint8_t Byte : CallSites.data())
      Body.u8(Byte);

    Body.sleb(1); // offset 0: catch on slot 1
    Body.sleb(0); // end of chain
    Body.sleb(0); // offset 2: cleanup
    Body.sleb(0); // end of chain
  }

  // The table grows downward from a base just past its first slot.
  const size_t TypeTableBaseOffset = Body.size() + 8;
  B.uleb(TypeTableBaseOffset);
  for (uint8_t Byte : Body.data())
    B.u8(Byte);
  B.u64(TypeInfoVA);
  return B.data();
}

TEST(ItaniumSJLJ, DecodesTheIndexFormAndEverythingItReaches) {
  // An SJLJ table names no code, and that was taken to mean there was nothing
  // in it worth reading.  There is: an action offset means here exactly what
  // it means in the address form, so the catch this frame installs -- and the
  // type it catches -- are as recoverable as in any other record.  Only the
  // question of *where* the catch applies is unanswerable from the table, and
  // that is because the answer is in the function's stores rather than here.
  BinaryImage Img = makeImage();
  const va_t FuncVA = kTextVA + 0x100;
  const va_t LSDAVA = kDataVA + 0x200;
  const va_t TypeInfoVA = kDataVA + 0x600;

  ByteBuilder TypeInfo;
  TypeInfo.u64(kDataVA + 0x700); // vptr
  TypeInfo.u64(kDataVA + 0x680); // name pointer
  writeData(Img, TypeInfoVA, TypeInfo.data());
  const char kName[] = "St13runtime_error";
  writeData(Img, kDataVA + 0x680,
            std::vector<uint8_t>(kName, kName + sizeof(kName)));
  writeData(Img, LSDAVA, buildSJLJLSDA(0x01, TypeInfoVA));

  LSDAParseRequest Req;
  Req.LSDAVA = LSDAVA;
  Req.FunctionStart = FuncVA;
  Req.FunctionEnd = FuncVA + 0x100;
  Req.IsSJLJ = true;
  LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});

  ASSERT_TRUE(Result.Info.has_value());
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);
  const ItaniumEHInfo &Info = *Result.Info;
  EXPECT_FALSE(Info.IsCallSiteAddressForm);

  ASSERT_EQ(Info.CallSites.size(), 3u);
  // The numbering starts at one because that is what the frame stores: the
  // personality reserves zero and below for "nothing here" and "terminate".
  EXPECT_EQ(Info.CallSites[0].CallSiteIndex, 1u);
  EXPECT_EQ(Info.CallSites[1].CallSiteIndex, 2u);
  EXPECT_EQ(Info.CallSites[2].CallSiteIndex, 3u);

  EXPECT_EQ(Info.CallSites[0].NativeLandingPad, 0u);
  EXPECT_FALSE(Info.CallSites[0].FirstActionOffset.has_value());
  EXPECT_EQ(Info.CallSites[1].NativeLandingPad, 1u);
  ASSERT_TRUE(Info.CallSites[1].FirstActionOffset.has_value());
  EXPECT_EQ(*Info.CallSites[1].FirstActionOffset, 0u);
  EXPECT_EQ(Info.CallSites[2].NativeLandingPad, 2u);
  ASSERT_TRUE(Info.CallSites[2].FirstActionOffset.has_value());
  EXPECT_EQ(*Info.CallSites[2].FirstActionOffset, 2u);

  // Nothing in this form is an address, so nothing decoded from it may claim
  // to be one.
  for (const ItaniumCallSite &Site : Info.CallSites) {
    EXPECT_FALSE(Site.GuardedRange.isValid());
    EXPECT_EQ(Site.LandingPadVA, 0u);
  }

  ASSERT_EQ(Info.Actions.size(), 2u);
  EXPECT_TRUE(Info.Actions[0].isCatch());
  EXPECT_EQ(Info.Actions[0].TypeFilter, 1);
  EXPECT_TRUE(Info.Actions[1].isCleanup());
  EXPECT_FALSE(Info.isCleanupOnly());

  ASSERT_EQ(Info.TypeTable.size(), 1u);
  EXPECT_EQ(Info.TypeTable[0].TypeInfoVA, TypeInfoVA);
  EXPECT_EQ(Info.TypeTable[0].TypeName, "St13runtime_error");
}

TEST(ItaniumSJLJ, IgnoresTheCallSiteEncodingByteTheProducerWrote) {
  // GCC writes `DW_EH_PE_uleb128` into that byte and LLVM writes
  // `DW_EH_PE_udata4`, and both then emit ULEB128 pairs regardless, because
  // the personality never reads the byte on this path.  Honouring it would
  // decode LLVM's output as four-byte columns; honouring a `DW_EH_PE_omit`
  // there would skip a table that reads perfectly well.
  const va_t FuncVA = kTextVA + 0x100;
  const va_t LSDAVA = kDataVA + 0x200;
  const va_t TypeInfoVA = kDataVA + 0x600;

  for (uint8_t Encoding : {uint8_t(0x01), uint8_t(0x03), uint8_t(0xff)}) {
    BinaryImage Img = makeImage();
    ByteBuilder TypeInfo;
    TypeInfo.u64(kDataVA + 0x700);
    TypeInfo.u64(kDataVA + 0x680);
    writeData(Img, TypeInfoVA, TypeInfo.data());
    const char kName[] = "St13runtime_error";
    writeData(Img, kDataVA + 0x680,
              std::vector<uint8_t>(kName, kName + sizeof(kName)));
    writeData(Img, LSDAVA, buildSJLJLSDA(Encoding, TypeInfoVA));

    LSDAParseRequest Req;
    Req.LSDAVA = LSDAVA;
    Req.FunctionStart = FuncVA;
    Req.FunctionEnd = FuncVA + 0x100;
    Req.IsSJLJ = true;
    LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});

    const unsigned Spelling = Encoding;
    ASSERT_TRUE(Result.Info.has_value()) << Spelling;
    EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete) << Spelling;
    ASSERT_EQ(Result.Info->CallSites.size(), 3u) << Spelling;
    ASSERT_EQ(Result.Info->TypeTable.size(), 1u) << Spelling;
    EXPECT_EQ(Result.Info->TypeTable[0].TypeName, "St13runtime_error")
        << Spelling;
  }
}

TEST(ItaniumSJLJ, KeepsTheWholeEntriesWhenBytesAreLeftOver) {
  // The declared length is the only terminator this form has, so a length that
  // does not divide into pairs leaves a byte over.  Returning the record empty
  // for it would discard entries that are individually sound, and with them
  // the action chain and the catch types -- which is the whole of what an SJLJ
  // record has to give.
  BinaryImage Img = makeImage();
  const va_t FuncVA = kTextVA + 0x100;
  const va_t LSDAVA = kDataVA + 0x200;

  ByteBuilder B;
  B.u8(0xff); // landing-pad base omitted
  B.u8(0xff); // no type table, so no offset field follows
  ByteBuilder CallSites;
  CallSites.uleb(0);
  CallSites.uleb(0);
  CallSites.uleb(7); // a selector with no action to pair it
  B.u8(0x01);
  B.uleb(CallSites.size());
  for (uint8_t Byte : CallSites.data())
    B.u8(Byte);
  writeData(Img, LSDAVA, B.data());

  LSDAParseRequest Req;
  Req.LSDAVA = LSDAVA;
  Req.FunctionStart = FuncVA;
  Req.FunctionEnd = FuncVA + 0x100;
  Req.IsSJLJ = true;
  LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});

  ASSERT_TRUE(Result.Info.has_value());
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Partial);
  ASSERT_EQ(Result.Info->CallSites.size(), 1u);
  EXPECT_EQ(Result.Info->CallSites[0].CallSiteIndex, 1u);
}

} // namespace
