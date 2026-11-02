//===- LanguageEHItaniumTests.cpp - Itanium LSDA tests ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LanguageEHTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/loader/ELF/ELFImageMetadata.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/loader/SourceUnwind.h"

#include "llvm/BinaryFormat/ELF.h"

#include <limits>

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
  Req.Personality = ExceptionPersonality::GxxPersonalityV0;
  LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});

  ASSERT_TRUE(Result.Info.has_value());
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);
  const ItaniumEHInfo &Info = *Result.Info;
  EXPECT_EQ(Info.TypeTableEntryKind, ItaniumTypeTableEntryKind::CxxRTTI);

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

TEST(ItaniumLSDA, KeepsAdaAndDTypeDescriptorsOpaque) {
  const va_t FuncVA = kTextVA + 0x100;
  const va_t LSDAVA = kDataVA + 0x200;
  const va_t DescriptorVA = kDataVA + 0x600;

  for (ExceptionPersonality Personality :
       {ExceptionPersonality::GnatPersonalityV0,
        ExceptionPersonality::DmdPersonalityV0,
        ExceptionPersonality::DRuntimeEhPersonality,
        ExceptionPersonality::GdcPersonalityV0}) {
    SCOPED_TRACE(getExceptionPersonalityName(Personality));
    BinaryImage Img = makeImage();

    // Both language runtimes put an opaque descriptor in this slot: GNAT uses
    // an Exception_Id and D uses a ClassInfo.  Make its second word look like a
    // valid std::type_info name pointer so an accidental RTTI interpretation
    // produces a convincing but false answer instead of merely failing.
    ByteBuilder Descriptor;
    Descriptor.u64(kDataVA + 0x700);
    Descriptor.u64(kDataVA + 0x680);
    writeData(Img, DescriptorVA, Descriptor.data());
    constexpr char ForgedRTTIName[] = "forged_rtti_name";
    writeData(Img, kDataVA + 0x680,
              std::vector<uint8_t>(ForgedRTTIName,
                                   ForgedRTTIName + sizeof(ForgedRTTIName)));

    LSDABytes Table = buildCatchLSDA(LSDAVA, FuncVA, DescriptorVA);
    writeData(Img, LSDAVA, Table.Bytes);

    LSDAParseRequest Req;
    Req.LSDAVA = LSDAVA;
    Req.FunctionStart = FuncVA;
    Req.FunctionEnd = FuncVA + 0x100;
    Req.Personality = Personality;
    LSDAParseResult Result = parseLSDA(Img, Req, PointerBases{});

    ASSERT_TRUE(Result.Info.has_value());
    EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);
    EXPECT_EQ(Result.Info->TypeTableEntryKind,
              ItaniumTypeTableEntryKind::OpaqueDescriptor);
    ASSERT_EQ(Result.Info->TypeTable.size(), 1u);
    EXPECT_EQ(Result.Info->TypeTable[0].TypeInfoVA, DescriptorVA);
    EXPECT_TRUE(Result.Info->TypeTable[0].TypeName.empty());
    EXPECT_FALSE(Result.Info->TypeTable[0].IsCatchAll);
  }
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

TEST(ItaniumLSDA, BoundsActionLinksBeforeApplyingSignedDisplacements) {
  for (int64_t Link :
       {std::numeric_limits<int64_t>::max(),
        std::numeric_limits<int64_t>::min(), int64_t(1), int64_t(-1)}) {
    SCOPED_TRACE(Link);
    BinaryImage Img = makeImage();
    const va_t LSDAVA = kDataVA + 0x200;
    ByteBuilder B;
    B.u8(0xff); // default landing-pad base
    B.u8(0xff); // no type table
    B.u8(0x01); // ULEB call sites
    B.uleb(4);
    B.uleb(0);
    B.uleb(1);
    B.uleb(1);
    B.uleb(1); // action at table offset zero
    B.sleb(0);
    B.sleb(Link); // relative to table offset one
    B.sleb(0);
    B.sleb(0);
    writeData(Img, LSDAVA, B.data());

    LSDAParseRequest Req;
    Req.LSDAVA = LSDAVA;
    Req.FunctionStart = kTextVA;
    Req.FunctionEnd = kTextVA + 0x100;
    auto Result = parseLSDA(Img, Req, PointerBases{});
    ASSERT_TRUE(Result.Info.has_value());
    ASSERT_FALSE(Result.Info->Actions.empty());
    if (Link == 1 || Link == -1) {
      EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);
      EXPECT_EQ(Result.Info->Actions[0].NextActionOffset,
                static_cast<uint64_t>(1 + Link));
      EXPECT_EQ(Result.Info->Actions.size(), Link == 1 ? 2u : 1u);
    } else {
      EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Partial);
      EXPECT_FALSE(Result.Info->Actions[0].NextActionOffset.has_value());
      EXPECT_EQ(Result.Info->Actions.size(), 1u);
    }
  }
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
  for (bool PublishExtents : {false, true}) {
    SCOPED_TRACE(PublishExtents);
    BinaryImage Img = makeImage();
    const va_t FuncVA = kTextVA + 0x100;
    const va_t SectionVA = kDataVA;
    const va_t PersonalityVA = kTextVA + 0x800;
    const va_t LSDAVA = kDataVA + 0x400;

    FrameBytes Frame = buildSimpleFrame(SectionVA, FuncVA, 0x80, "zPLR",
                                        PersonalityVA, LSDAVA);
    writeData(Img, SectionVA, Frame.Bytes);

    Section EhFrame;
    EhFrame.Name = ".eh_frame";
    EhFrame.VA = SectionVA;
    EhFrame.Size = Frame.Bytes.size();
    EhFrame.Data = Frame.Bytes;
    Img.Sections.push_back(std::move(EhFrame));

    LSDABytes Table = buildCatchLSDA(LSDAVA, FuncVA, /*TypeInfoVA=*/0);
    writeData(Img, LSDAVA, Table.Bytes);
    if (PublishExtents)
      recordFrameExtents(Img);
    EXPECT_TRUE(Img.ExceptionMetadata.Functions.empty());
    EXPECT_TRUE(Img.ExceptionMetadata.CIEs.empty());

    // Discovery can supply the personality name only after the frame extents
    // have bounded its heuristic scans. Language recovery must see that name.
    Symbol Personality;
    Personality.Name = "__gxx_personality_v0";
    Personality.Addr = PersonalityVA;
    Personality.IsFunc = true;
    Img.Symbols.push_back(std::move(Personality));

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
}

TEST(ItaniumEHDriver, IncompleteFrameSectionsRemainStructuralAtEveryEntry) {
  for (bool PublishExtents : {false, true})
    for (Arch A : {Arch::X86, Arch::X64, Arch::AArch64})
      for (unsigned Damage : {0u, 1u, 2u, 3u, 4u})
        for (bool BadFirst : {false, true}) {
          SCOPED_TRACE(PublishExtents);
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(Damage);
          SCOPED_TRACE(BadFirst);
          BinaryImage Img = makeImage();
          Img.Arch = A;
          Img.Bits = A == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
          std::vector<uint8_t> Bytes;
          for (unsigned I = 0; I < 2; ++I) {
            const bool Bad = (I == 0) == BadFirst;
            auto Frame = buildSimpleFrame(kDataVA + Bytes.size(),
                                          kTextVA + 0x100 + I * 0x100, 0x80,
                                          Bad && Damage == 0   ? "zRX"
                                          : Bad && Damage == 1 ? "zRL"
                                                               : "zR",
                                          0, Bad && Damage == 1 ? 0xdead000 : 0,
                                          Bad && Damage == 1 ? 0x9b : 0x1b);
            // Truncate the FDE's final def_cfa_offset ULEB or replace the
            // CIE's last padding byte with an operand-less advance_loc1.
            // Neither changes the header's extent, but full recovery must
            // report the malformed program.
            if (Bad && Damage == 3) {
              uint8_t &Last =
                  Frame.Bytes[Frame.Bytes.size() - sizeof(uint32_t) - 1];
              ASSERT_EQ(Last, 16u);
              Last = 0x80;
            }
            if (Bad && Damage == 4) {
              ASSERT_EQ(Frame.Bytes[Frame.FDEOffset - 1], 0u);
              Frame.Bytes[Frame.FDEOffset - 1] = 0x02;
            }
            Frame.Bytes.resize(Frame.Bytes.size() - sizeof(uint32_t));
            Bytes.insert(Bytes.end(), Frame.Bytes.begin(), Frame.Bytes.end());
          }
          if (Damage == 2) {
            ByteBuilder Truncated;
            Truncated.u32(0x1000);
            Truncated.u32(0);
            Bytes.insert(Bytes.end(), Truncated.data().begin(),
                         Truncated.data().end());
          } else
            Bytes.resize(Bytes.size() + sizeof(uint32_t));
          Section Frames;
          Frames.Name = ".eh_frame";
          Frames.VA = kDataVA;
          Frames.Size = Bytes.size();
          Frames.Data = Bytes;
          writeData(Img, kDataVA, Bytes);
          Img.Sections.push_back(std::move(Frames));
          if (PublishExtents) {
            recordFrameExtents(Img);
            // CFI validity does not change a frame's encoded code extent.
            ASSERT_EQ(Img.KnownCodeRanges.size(), 2u);
            EXPECT_EQ(Img.KnownCodeRanges[0],
                      std::make_pair(kTextVA + 0x100, kTextVA + 0x180));
            EXPECT_EQ(Img.KnownCodeRanges[1],
                      std::make_pair(kTextVA + 0x200, kTextVA + 0x280));
          }
          parseItaniumExceptions(Img);
          ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 2u);
          const auto Status = Damage >= 2 ? ExceptionParseStatus::Malformed
                                          : ExceptionParseStatus::Partial;
          EXPECT_EQ(Img.ExceptionMetadata.ParseStatus, Status);
          for (const auto &F : Img.ExceptionMetadata.Functions) {
            ASSERT_TRUE(F.Dwarf);
            EXPECT_EQ(F.Dwarf->LSDAVA, 0u);
            EXPECT_EQ(F.ParseStatus, Status);
            ASSERT_TRUE(F.DecodeProvenance);
            EXPECT_EQ(F.DecodeProvenance->Structural.ParseStatus, Status);
            EXPECT_FALSE(isPlainSourceUnwind(F));
            EXPECT_FALSE(F.DecodeProvenance->Structural.Diagnostics.empty());
          }
        }
}

TEST(ItaniumEHDriver, InvalidRangeRetainsItsIdentifiableFrameEntry) {
  for (Arch A : {Arch::X86, Arch::X64, Arch::AArch64}) {
    auto Img = makeImage();
    Img.Arch = A;
    Img.Bits = A == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
    const va_t Entry = kTextVA + 0x100;
    Img.Symbols.push_back({"sized_function", Entry, 0x80, true});
    const auto Frame = buildSimpleFrame(kDataVA, Entry, 0, "zR", 0, 0);
    writeData(Img, kDataVA, Frame.Bytes);
    Section Frames;
    Frames.Name = ".eh_frame";
    Frames.VA = kDataVA;
    Frames.Size = Frame.Bytes.size();
    Frames.Data = Frame.Bytes;
    Img.Sections.push_back(std::move(Frames));
    parseItaniumExceptions(Img);
    ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
    const auto &F = Img.ExceptionMetadata.Functions.front();
    ASSERT_TRUE(F.Dwarf);
    EXPECT_EQ(F.Dwarf->InitialLocation, Entry);
    EXPECT_FALSE(F.CodeRange.isValid());
    EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Malformed);
    ASSERT_TRUE(F.DecodeProvenance);
    EXPECT_EQ(F.DecodeProvenance->Structural.ParseStatus,
              ExceptionParseStatus::Malformed);
    EXPECT_FALSE(isPlainSourceUnwind(F));
  }
}

TEST(ItaniumEHDriver, LanguageRefreshPreservesIncompleteFrameDecoding) {
  BinaryImage Img = makeImage();
  const va_t FuncVA = kTextVA + 0x100;
  const va_t LSDAVA = kDataVA + 0x400;
  const va_t PersonalityVA = kTextVA + 0x800;
  Img.Symbols.push_back({"__gxx_personality_v0", PersonalityVA, 0, true});
  auto Frame =
      buildSimpleFrame(kDataVA, FuncVA, 0x80, "zPLRX", PersonalityVA, LSDAVA);
  writeData(Img, kDataVA, Frame.Bytes);
  Section Frames;
  Frames.Name = ".eh_frame";
  Frames.VA = kDataVA;
  Frames.Size = Frame.Bytes.size();
  Frames.Data = Frame.Bytes;
  Img.Sections.push_back(std::move(Frames));
  writeData(Img, LSDAVA, buildCatchLSDA(LSDAVA, FuncVA, 0).Bytes);
  parseItaniumExceptions(Img);
  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  auto &F = Img.ExceptionMetadata.Functions.front();
  ASSERT_TRUE(F.DecodeProvenance);
  ASSERT_EQ(F.DecodeProvenance->Structural.ParseStatus,
            ExceptionParseStatus::Partial);
  ASSERT_EQ(F.DecodeProvenance->Language.ParseStatus,
            ExceptionParseStatus::Complete);
  const auto Diagnostics = F.DecodeProvenance->Structural.Diagnostics;
  EXPECT_TRUE(refreshItaniumLanguageData(Img, F));
  EXPECT_EQ(F.DecodeProvenance->Structural.Diagnostics, Diagnostics);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_FALSE(isPlainSourceUnwind(F));
}

/// An AArch64 image whose `.eh_frame` holds one frame for \p FrameVA, with
/// \p Code written at \p CodeVA.
BinaryImage makeAArch64FrameImage(va_t CodeVA, const std::vector<uint8_t> &Code,
                                  va_t FrameVA, uint64_t FrameSize) {
  BinaryImage Img = makeImage();
  Img.Arch = Arch::AArch64;
  writeData(Img, CodeVA, Code);
  FrameBytes Frame = buildSimpleFrame(kDataVA, FrameVA, FrameSize, "zR", 0, 0);
  writeData(Img, kDataVA, Frame.Bytes);
  Section EhFrame;
  EhFrame.Name = ".eh_frame";
  EhFrame.VA = kDataVA;
  EhFrame.Size = Frame.Bytes.size();
  EhFrame.Data = Frame.Bytes;
  Img.Sections.push_back(std::move(EhFrame));
  return Img;
}

const Symbol *functionAt(const BinaryImage &Img, va_t VA) {
  for (const Symbol &Sym : Img.Symbols)
    if (Sym.IsFunc && Sym.Addr == VA)
      return &Sym;
  return nullptr;
}

// bionic's syscall stubs open their frame after the `bti c` landing pad.
const std::vector<uint8_t> kBTIStub = {
    0x5f, 0x24, 0x03, 0xd5, // bti c
    0x08, 0x04, 0x80, 0xd2, // mov x8, #32
    0x01, 0x00, 0x00, 0xd4, // svc #0
    0xc0, 0x03, 0x5f, 0xd6, // ret
};

TEST(ItaniumEHDriver, StartsAnAArch64FunctionAtTheLandingPadBeforeItsFrame) {
  const va_t StubVA = kTextVA + 0x100;
  BinaryImage Img = makeAArch64FrameImage(StubVA, kBTIStub, StubVA + 4, 12);

  parseItaniumExceptions(Img);

  // The frame still describes the code it covers...
  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].CodeRange.Begin, StubVA + 4);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].FunctionEntry, StubVA);
  // ...but the function begins at its landing pad.
  const Symbol *Stub = functionAt(Img, StubVA);
  ASSERT_NE(Stub, nullptr);
  EXPECT_EQ(Stub->Size, 16u);
  EXPECT_EQ(functionAt(Img, StubVA + 4), nullptr);
}

TEST(ItaniumEHDriver, DropsTheSearchTableEntryInsideAPaddedFunction) {
  const va_t StubVA = kTextVA + 0x100;
  BinaryImage Img = makeAArch64FrameImage(StubVA, kBTIStub, StubVA + 4, 12);
  // The ELF loader makes each .eh_frame_hdr entry, the frame's first
  // address, a function before the frames are read.
  Img.Symbols.push_back(Symbol::makeFunc(StubVA + 4));

  parseItaniumExceptions(Img);

  ASSERT_NE(functionAt(Img, StubVA), nullptr);
  EXPECT_EQ(functionAt(Img, StubVA + 4), nullptr);
}

TEST(ItaniumEHDriver, LeavesAFrameThatNoLandingPadPrecedesWhereItBegins) {
  const va_t StubVA = kTextVA + 0x100;
  std::vector<uint8_t> Code = kBTIStub;
  // The word before the frame is the previous function's `ret`.
  Code[0] = 0xc0, Code[1] = 0x03, Code[2] = 0x5f, Code[3] = 0xd6;
  BinaryImage Img = makeAArch64FrameImage(StubVA, Code, StubVA + 4, 12);

  parseItaniumExceptions(Img);

  EXPECT_NE(functionAt(Img, StubVA + 4), nullptr);
  EXPECT_EQ(functionAt(Img, StubVA), nullptr);
  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].FunctionEntry, 0u);
}

TEST(ItaniumEHDriver, LeavesALandingPadAnotherFrameCovers) {
  const va_t StubVA = kTextVA + 0x100;
  BinaryImage Img = makeImage();
  Img.Arch = Arch::AArch64;
  writeData(Img, StubVA, kBTIStub);
  // Two frames: one that covers the landing pad, and the stub's own. Each
  // built frame section ends in a terminator; the first one's is dropped.
  FrameBytes First = buildSimpleFrame(kDataVA, StubVA - 8, 12, "zR", 0, 0);
  std::vector<uint8_t> Bytes(First.Bytes.begin(), First.Bytes.end() - 4);
  FrameBytes Second =
      buildSimpleFrame(kDataVA + Bytes.size(), StubVA + 4, 12, "zR", 0, 0);
  Bytes.insert(Bytes.end(), Second.Bytes.begin(), Second.Bytes.end());
  writeData(Img, kDataVA, Bytes);
  Section EhFrame;
  EhFrame.Name = ".eh_frame";
  EhFrame.VA = kDataVA;
  EhFrame.Size = Bytes.size();
  EhFrame.Data = Bytes;
  Img.Sections.push_back(std::move(EhFrame));

  parseItaniumExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 2u);
  EXPECT_NE(functionAt(Img, StubVA + 4), nullptr);
  EXPECT_EQ(functionAt(Img, StubVA), nullptr);
}

/// An x86-64 image whose `.eh_frame` holds one frame of \p FrameSize bytes
/// at \p FrameVA.
BinaryImage makeX64FrameImage(va_t FrameVA, uint64_t FrameSize) {
  BinaryImage Img = makeImage();
  FrameBytes Frame = buildSimpleFrame(kDataVA, FrameVA, FrameSize, "zR", 0, 0);
  writeData(Img, kDataVA, Frame.Bytes);
  Section EhFrame;
  EhFrame.Name = ".eh_frame";
  EhFrame.VA = kDataVA;
  EhFrame.Size = Frame.Bytes.size();
  EhFrame.Data = Frame.Bytes;
  Img.Sections.push_back(std::move(EhFrame));
  return Img;
}

// The .eh_frame_hdr search table names only each function's entry.  Its frame
// proves the extent, and heuristic discovery must not guess a function start
// inside it -- here an instruction byte equal to int3 before a prologue-like
// sequence.
TEST(ItaniumEHDriver, GivesSearchTableFunctionsTheirFrameBeforeDiscovery) {
  const va_t FuncVA = kTextVA + 0x100;
  const std::vector<uint8_t> PaddingLookalike = {0xcc, 0x55, 0x48, 0x89, 0xe5};
  BinaryImage Guessed = makeX64FrameImage(FuncVA, 0x80);
  writeData(Guessed, FuncVA + 0x3f, PaddingLookalike);
  Guessed.Symbols.push_back(Symbol::makeFunc(FuncVA));
  scanPaddingBoundaries(Guessed);
  // Without the frame, the scan takes the sequence for a new function.
  ASSERT_NE(functionAt(Guessed, FuncVA + 0x40), nullptr);

  BinaryImage Img = makeX64FrameImage(FuncVA, 0x80);
  writeData(Img, FuncVA + 0x3f, PaddingLookalike);
  Img.Symbols.push_back(Symbol::makeFunc(FuncVA));
  recordFrameExtents(Img);
  scanPaddingBoundaries(Img);

  const Symbol *Function = functionAt(Img, FuncVA);
  ASSERT_NE(Function, nullptr);
  EXPECT_EQ(Function->Size, 0x80u);
  EXPECT_EQ(functionAt(Img, FuncVA + 0x40), nullptr);
  ASSERT_EQ(Img.KnownCodeRanges.size(), 1u);
  EXPECT_EQ(Img.KnownCodeRanges.front(), std::make_pair(FuncVA, FuncVA + 0x80));
}

TEST(ItaniumEHDriver, KeepsTheStatedSizeOfANamedFunction) {
  const va_t FuncVA = kTextVA + 0x100;
  BinaryImage Img = makeX64FrameImage(FuncVA, 0x80);
  Symbol Start = Symbol::makeFunc(FuncVA);
  Start.Name = "_start";
  Img.Symbols.push_back(std::move(Start));

  recordFrameExtents(Img);

  const Symbol *Function = functionAt(Img, FuncVA);
  ASSERT_NE(Function, nullptr);
  EXPECT_EQ(Function->Size, 0u);
  EXPECT_EQ(Img.KnownCodeRanges.size(), 1u);
}

TEST(ItaniumEHDriver, COFFNamedUnsizedFunctionOwnsItsInteriorBeforeDiscovery) {
  const va_t FuncVA = kTextVA + 0x100;
  BinaryImage Img = makeX64FrameImage(FuncVA, 0x80);
  Img.Format = BinaryFormat::COFF;
  const std::vector<uint8_t> PaddingLookalike = {0xcc, 0x55, 0x48, 0x89, 0xe5};
  writeData(Img, FuncVA + 0x3f, PaddingLookalike);
  Symbol Start = Symbol::makeFunc(FuncVA);
  Start.Name = "named_function";
  Start.Origin = NameOrigin::Stated;
  Img.Symbols.push_back(Start);
  recordFrameExtents(Img);
  runPostLoadDiscovery(Img, "test-coff-frame-before-discovery");
  const Symbol *Function = functionAt(Img, FuncVA);
  ASSERT_NE(Function, nullptr);
  EXPECT_EQ(Function->Name, "named_function");
  EXPECT_EQ(Function->Size, 0u);
  EXPECT_EQ(functionAt(Img, FuncVA + 0x40), nullptr);
}

// A linker emits one frame for a whole PLT section; its first stub is not a
// function of that size.
TEST(ItaniumEHDriver, LeavesAPLTFrameToItsStubs) {
  const va_t StubsVA = kTextVA + 0x100;
  BinaryImage Img = makeX64FrameImage(StubsVA, 0x48);
  Section Stubs;
  Stubs.Name = ".plt.got";
  Stubs.VA = StubsVA;
  Stubs.Size = 0x48;
  Stubs.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Stubs));
  Img.Symbols.push_back(Symbol::makeFunc(StubsVA));

  recordFrameExtents(Img);

  const Symbol *First = functionAt(Img, StubsVA);
  ASSERT_NE(First, nullptr);
  EXPECT_EQ(First->Size, 0u);
}

TEST(ItaniumEHDriver, ExtentsFirstStillDropTheEntryInsideAPaddedFunction) {
  const va_t StubVA = kTextVA + 0x100;
  BinaryImage Img = makeAArch64FrameImage(StubVA, kBTIStub, StubVA + 4, 12);
  Img.Symbols.push_back(Symbol::makeFunc(StubVA + 4));

  recordFrameExtents(Img);
  parseItaniumExceptions(Img);

  const Symbol *Stub = functionAt(Img, StubVA);
  ASSERT_NE(Stub, nullptr);
  EXPECT_EQ(Stub->Size, 16u);
  EXPECT_EQ(functionAt(Img, StubVA + 4), nullptr);
}

TEST(ItaniumEHDriver, IndexedExtensionExtentsRequireCompletePrograms) {
  for (unsigned Damage : {0u, 1u, 2u}) {
    SCOPED_TRACE(Damage);
    const va_t FuncVA = kTextVA + 0x100;
    const va_t ExtraFuncVA = kTextVA + 0x200;
    const va_t ExtraVA = kDataVA + 0x200;
    const va_t HeaderVA = kDataVA + 0x600;
    BinaryImage Img = makeImage();
    FrameBytes Primary = buildSimpleFrame(kDataVA, FuncVA, 0x80, "zR", 0, 0);
    writeData(Img, kDataVA, Primary.Bytes);
    Section Frames;
    Frames.Name = ".eh_frame";
    Frames.VA = kDataVA;
    Frames.Size = Primary.Bytes.size();
    Frames.Data = Primary.Bytes;
    Img.Sections.push_back(std::move(Frames));

    // This CIE/FDE pair lives outside the section. The runtime index may
    // extend discovery only after its full program has been validated.
    FrameBytes Extra = buildSimpleFrame(ExtraVA, ExtraFuncVA, 0x40, "zR", 0, 0);
    if (Damage == 1) {
      uint8_t &Last = Extra.Bytes[Extra.Bytes.size() - sizeof(uint32_t) - 1];
      ASSERT_EQ(Last, 16u);
      Last = 0x80; // truncated def_cfa_offset ULEB
    }
    if (Damage == 2) {
      ASSERT_EQ(Extra.Bytes[Extra.FDEOffset - 1], 0u);
      Extra.Bytes[Extra.FDEOffset - 1] = 0x02; // operand-less advance_loc1
    }
    writeData(Img, ExtraVA, Extra.Bytes);

    ByteBuilder Header;
    Header.u8(1);    // version
    Header.u8(0x00); // absolute frame pointer
    Header.u8(0x03); // udata4 count
    Header.u8(0x00); // absolute search-table pointers
    Header.u64(kDataVA);
    Header.u32(2);
    Header.u64(FuncVA);
    Header.u64(kDataVA + Primary.FDEOffset);
    Header.u64(ExtraFuncVA);
    Header.u64(ExtraVA + Extra.FDEOffset);
    writeData(Img, HeaderVA, Header.data());
    Img.ELFMetadata.emplace();
    Img.ELFMetadata->ProgramHeaders.push_back({llvm::ELF::PT_GNU_EH_FRAME, 0, 0,
                                               HeaderVA, Header.size(),
                                               Header.size(), 1});

    recordFrameExtents(Img);
    const size_t Expected = Damage ? 1 : 2;
    ASSERT_EQ(Img.KnownCodeRanges.size(), Expected);
    EXPECT_EQ(Img.KnownCodeRanges.front(),
              std::make_pair(FuncVA, FuncVA + 0x80));
    if (!Damage)
      EXPECT_EQ(Img.KnownCodeRanges.back(),
                std::make_pair(ExtraFuncVA, ExtraFuncVA + 0x40));
    parseItaniumExceptions(Img);
    EXPECT_EQ(Img.ExceptionMetadata.Functions.size(), Expected);
    EXPECT_EQ(Img.ExceptionMetadata.ParseStatus,
              Damage ? ExceptionParseStatus::Malformed
                     : ExceptionParseStatus::Complete);
  }
}

TEST(ItaniumEHDriver, RelocatableExtentsDoNotDecodeUnlinkedFrames) {
  const va_t FuncVA = kTextVA + 0x100;
  BinaryImage Img = makeX64FrameImage(FuncVA, 0x80);
  Img.IsRelocatable = true;
  recordFrameExtents(Img);
  EXPECT_TRUE(Img.KnownCodeRanges.empty());
  EXPECT_TRUE(Img.ExceptionMetadata.Functions.empty());

  // No linked extents were requested. The later read must use the bytes
  // after relocation, rather than retain an unlinked frame's guessed address.
  const va_t LinkedVA = FuncVA + 0x100;
  FrameBytes Linked = buildSimpleFrame(kDataVA, LinkedVA, 0x40, "zR", 0, 0);
  Img.Sections.front().Data = Linked.Bytes;
  Img.Sections.front().Size = Linked.Bytes.size();
  writeData(Img, kDataVA, Linked.Bytes);
  parseItaniumExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions.front();
  EXPECT_EQ(F.CodeRange.Begin, LinkedVA);
  EXPECT_EQ(F.CodeRange.End, LinkedVA + 0x40);
}

TEST(ItaniumEHDriver, StandaloneReadsObserveChangedFrameBytes) {
  const va_t FuncVA = kTextVA + 0x100;
  BinaryImage Img = makeX64FrameImage(FuncVA, 0x80);
  recordFrameExtents(Img);

  // Extent discovery must not retain a partial decode for exception recovery.
  // The full reader must observe the image's current frame bytes.
  FrameBytes Changed = buildSimpleFrame(kDataVA, FuncVA, 0x40, "zR", 0, 0);
  Img.Sections.front().Data = Changed.Bytes;
  Img.Sections.front().Size = Changed.Bytes.size();
  writeData(Img, kDataVA, Changed.Bytes);
  parseItaniumExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions.front();
  EXPECT_EQ(F.CodeRange.Begin, FuncVA);
  EXPECT_EQ(F.CodeRange.End, FuncVA + 0x40);
}

TEST(ItaniumEHDriver, IgnoresAnImageWithoutAFrameSection) {
  BinaryImage Img = makeImage();
  recordFrameExtents(Img);
  parseItaniumExceptions(Img);
  EXPECT_TRUE(Img.ExceptionMetadata.Functions.empty());
  EXPECT_FALSE(Img.ExceptionMetadata.hasModel(ExceptionModel::Itanium));
}

} // namespace
