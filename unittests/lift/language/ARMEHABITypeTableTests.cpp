//===- ARMEHABITypeTableTests.cpp - ARM EHABI language data tests -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "ARMEHABITestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::arm_ehabi;
using namespace neverd::arm_ehabi_test;

//===----------------------------------------------------------------------===//
// Language data
//===----------------------------------------------------------------------===//

/// Build an image whose one frame carries an LSDA appended to its descriptor.
BinaryImage makeLSDAImage(uint8_t TypeTableEncoding, uint32_t SlotValue) {
  BinaryImage Img = makeARMImage();
  writeTypeInfo(Img);

  Symbol Personality;
  Personality.Name = "__gxx_personality_v0";
  Personality.Addr = kPersonalityVA;
  Personality.IsFunc = true;
  Img.Symbols.push_back(std::move(Personality));

  write(Img, kExTabVA,
        buildGenericEntry(kExTabVA, kPersonalityVA, 0x00B0B0B0u, {},
                          buildLSDA(TypeTableEncoding, SlotValue)));

  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);
  return Img;
}

/// The LSDA's first byte, which is where the type-table slot's own address is
/// measured from.  The header is five bytes and the call-site and action
/// tables are ten, leaving the slot one padding byte later.
constexpr va_t kLSDAVA = kExTabVA + 8;
constexpr va_t kTypeSlotVA = kLSDAVA + 16;

TEST(ARMEHABI, RecoversTheCallSiteGraphAppendedToAGenericEntry) {
  BinaryImage Img =
      makeLSDAImage(/*TypeTableEncoding=*/0x00,
                    /*SlotValue=*/static_cast<uint32_t>(kTypeInfoVA));
  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(F.Personality, ExceptionPersonality::GxxPersonalityV0);
  EXPECT_EQ(F.PersonalityName, "__gxx_personality_v0");
  // The language data has no section of its own: it begins where the unwind
  // opcodes stop.
  EXPECT_EQ(F.HandlerDataVA, kLSDAVA);

  ASSERT_TRUE(F.Itanium.has_value());
  const ItaniumEHInfo &LSDA = *F.Itanium;
  EXPECT_EQ(LSDA.LSDAVA, kLSDAVA);
  // The base is the function the index named, which is what makes every
  // displacement in the table resolvable at all.
  EXPECT_EQ(LSDA.LandingPadBase, kTextVA);
  ASSERT_EQ(LSDA.CallSites.size(), 2u);
  EXPECT_EQ(LSDA.CallSites[0].GuardedRange.Begin, kTextVA + 0x10);
  EXPECT_EQ(LSDA.CallSites[0].GuardedRange.End, kTextVA + 0x18);
  EXPECT_EQ(LSDA.CallSites[0].LandingPadVA, kTextVA + 0x40);
  EXPECT_EQ(LSDA.CallSites[1].LandingPadVA, 0u);

  ASSERT_EQ(LSDA.Actions.size(), 1u);
  EXPECT_TRUE(LSDA.Actions[0].isCatch());
  EXPECT_EQ(LSDA.Actions[0].TypeFilter, 1);
  EXPECT_FALSE(LSDA.isCleanupOnly());

  ASSERT_EQ(LSDA.TypeTable.size(), 1u);
  EXPECT_EQ(LSDA.TypeTable[0].TypeInfoVA, kTypeInfoVA);
  EXPECT_EQ(LSDA.TypeTable[0].TypeName, kTypeName);
  EXPECT_FALSE(LSDA.TypeTable[0].IsCatchAll);
  ASSERT_TRUE(F.ARMEHABI.has_value());
  EXPECT_EQ(F.ARMEHABI->TypeTableConvention,
            ARMTypeTableConvention::Absolute);
}

// The header byte is not evidence.  EHABI hands a type-table slot to
// `_Unwind_decode_typeinfo_ptr`, which applies the platform's `R_ARM_TARGET2`
// convention and never reads the byte, so a producer is free to leave a bare
// `DW_EH_PE_absptr` there while emitting a relocation that means something
// else entirely -- and Clang does.  A decoder that believes the byte reports a
// displacement as though it were the address of a type.
TEST(ARMEHABI, ReadsATypeTableTheWayTheImageWasLinkedRatherThanAsDeclared) {
  BinaryImage Img =
      makeLSDAImage(/*TypeTableEncoding=*/0x00,
                    /*SlotValue=*/static_cast<uint32_t>(kTypeCellVA -
                                                        kTypeSlotVA));
  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  ASSERT_TRUE(F.Itanium.has_value());
  ASSERT_EQ(F.Itanium->TypeTable.size(), 1u);
  // Declared as though the slot held the pointer; linked so that it holds the
  // distance to a cell that does.
  EXPECT_EQ(F.Itanium->TypeTableEncoding, 0x00);
  EXPECT_EQ(F.Itanium->TypeTable[0].TypeInfoVA, kTypeInfoVA);
  EXPECT_EQ(F.Itanium->TypeTable[0].TypeInfoSlotVA, kTypeCellVA);
  EXPECT_EQ(F.Itanium->TypeTable[0].TypeName, kTypeName);
  ASSERT_TRUE(F.ARMEHABI.has_value());
  EXPECT_EQ(F.ARMEHABI->TypeTableConvention,
            ARMTypeTableConvention::PCRelativeIndirect);
}

TEST(ARMEHABI, HonoursATypeTableEncodingTheHeaderDoesSpell) {
  // `DW_EH_PE_indirect | DW_EH_PE_pcrel | DW_EH_PE_absptr` is what GCC writes
  // for the same relocation Clang leaves bare, and a header that says
  // something is left to say it.
  BinaryImage Img =
      makeLSDAImage(/*TypeTableEncoding=*/0x90,
                    /*SlotValue=*/static_cast<uint32_t>(kTypeCellVA -
                                                        kTypeSlotVA));
  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  ASSERT_TRUE(F.Itanium.has_value());
  ASSERT_EQ(F.Itanium->TypeTable.size(), 1u);
  EXPECT_EQ(F.Itanium->TypeTable[0].TypeInfoVA, kTypeInfoVA);
  EXPECT_EQ(F.Itanium->TypeTable[0].TypeName, kTypeName);
}

// Frames ahead of the one that proves the convention are read again once it
// exists.  The first frame of a real image routinely catches a type defined in
// another shared object, which no reading of the slot can reach from here, so
// the record that settles the question is rarely the first one.
TEST(ARMEHABI, RereadsTheTypeTablesDecodedBeforeTheConventionWasProven) {
  BinaryImage Img = makeARMImage();
  writeTypeInfo(Img);

  Symbol Personality;
  Personality.Name = "__gxx_personality_v0";
  Personality.Addr = kPersonalityVA;
  Personality.IsFunc = true;
  Img.Symbols.push_back(std::move(Personality));

  // The first frame's cell is empty in the file, as a position-independent
  // image leaves every cell the loader will write.  Nothing about it can be
  // followed, so it settles nothing.
  const va_t FirstEntryVA = kExTabVA;
  const va_t FirstSlotVA = FirstEntryVA + 8 + 16;
  const va_t EmptyCellVA = kDataVA + 0x400;
  write(Img, FirstEntryVA,
        buildGenericEntry(FirstEntryVA, kPersonalityVA, 0x00B0B0B0u, {},
                          buildLSDA(0x00, static_cast<uint32_t>(
                                              EmptyCellVA - FirstSlotVA))));

  const va_t SecondEntryVA = kExTabVA + 0x100;
  const va_t SecondSlotVA = SecondEntryVA + 8 + 16;
  write(Img, SecondEntryVA,
        buildGenericEntry(SecondEntryVA, kPersonalityVA, 0x00B0B0B0u, {},
                          buildLSDA(0x00, static_cast<uint32_t>(
                                              kTypeCellVA - SecondSlotVA))));

  IndexBuilder Index;
  Index.tableRef(kTextVA + 0x000, FirstEntryVA);
  Index.tableRef(kTextVA + 0x100, SecondEntryVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 2u);
  const ExceptionFunction &First = Img.ExceptionMetadata.Functions[0];
  ASSERT_TRUE(First.Itanium.has_value());
  ASSERT_EQ(First.Itanium->TypeTable.size(), 1u);
  // Read again with the answer the second frame supplied: the cell holds
  // nothing yet, but it is the cell the catch dispatches through.
  EXPECT_EQ(First.Itanium->TypeTable[0].TypeInfoSlotVA, EmptyCellVA);
  EXPECT_FALSE(First.Itanium->TypeTable[0].IsCatchAll);
  ASSERT_TRUE(First.ARMEHABI.has_value());
  EXPECT_EQ(First.ARMEHABI->TypeTableConvention,
            ARMTypeTableConvention::PCRelativeIndirect);

  const ExceptionFunction &Second = Img.ExceptionMetadata.Functions[1];
  ASSERT_TRUE(Second.Itanium.has_value());
  ASSERT_EQ(Second.Itanium->TypeTable.size(), 1u);
  EXPECT_EQ(Second.Itanium->TypeTable[0].TypeName, kTypeName);
}

// A static link keeps no symbol for the personality routine it resolved, so
// the frames of a stripped static binary name a routine and nothing names it
// back.  Their tables are still there and still readable, and nothing else in
// such an image can recover a handler -- but a reading taken on no promise has
// to carry its own evidence.
TEST(ARMEHABI, ReadsATableBehindAnUnnamedPersonalityThatProvesItself) {
  BinaryImage Img = makeARMImage();
  writeTypeInfo(Img);
  write(Img, kExTabVA,
        buildGenericEntry(kExTabVA, kPersonalityVA, 0x00B0B0B0u, {},
                          buildLSDA(0x00, static_cast<uint32_t>(kTypeInfoVA))));
  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(F.Personality, ExceptionPersonality::None);
  ASSERT_TRUE(F.Itanium.has_value());
  EXPECT_EQ(F.Itanium->CallSites.size(), 2u);
  EXPECT_EQ(F.Itanium->TypeTable.size(), 1u);
}

TEST(ARMEHABI, InventsNoTableBehindAnUnnamedPersonalityFromBytesThatAreNotOne) {
  BinaryImage Img = makeARMImage();
  // Nothing follows the opcodes but the zero fill of the segment, which is
  // what an entry whose personality takes no data looks like.
  write(Img, kExTabVA,
        buildGenericEntry(kExTabVA, kPersonalityVA, 0x00B0B0B0u, {}, {}));
  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  EXPECT_FALSE(F.Itanium.has_value());
  EXPECT_EQ(F.HandlerDataVA, 0u);
  // Bytes that did not read as a table are not a fault in an image that never
  // claimed they were one; the frame's own unwinding decoded fine.
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(Img.ExceptionMetadata.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(ARMEHABI, ReadsNoLanguageDataForAnARMDefinedPersonality) {
  BinaryImage Img = makeARMImage();
  writeTypeInfo(Img);
  // The same bytes an LSDA would occupy, behind a routine that does not read
  // them as one.  EHABI gives personality routine 1 a scope-descriptor list,
  // and a decoder that reaches for a call-site table here invents one.
  ByteBuilder Entry;
  Entry.u32(0x8100B0B0u);
  write(Img, kExTabVA, Entry.data());
  write(Img, kExTabVA + 4,
        buildLSDA(0x00, static_cast<uint32_t>(kTypeInfoVA)));

  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  EXPECT_EQ(F.Personality, ExceptionPersonality::AeabiUnwindCppPr1);
  EXPECT_FALSE(F.Itanium.has_value());
  EXPECT_NE(F.ParseStatus, ExceptionParseStatus::Malformed);
}

TEST(ARMEHABI, KeepsTheDWARFTablesAnImageCarriesBesideItsIndex) {
  // `-fasynchronous-unwind-tables` leaves both, and each describes the same
  // frames from a different direction.  Neither reader may discard the other's
  // records.
  BinaryImage Img = makeARMImage();
  ExceptionFunction Existing;
  Existing.Encoding = ExceptionEncoding::DwarfFDE;
  Existing.CodeRange = {kTextVA, kTextVA + 0x40};
  Img.ExceptionMetadata.Functions.push_back(std::move(Existing));
  Img.ExceptionMetadata.addModel(ExceptionModel::Itanium);

  IndexBuilder Index;
  Index.cantUnwind(kTextVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 2u);
  EXPECT_TRUE(Img.ExceptionMetadata.hasModel(ExceptionModel::Itanium));
  EXPECT_TRUE(Img.ExceptionMetadata.hasModel(ExceptionModel::ARMEHABI));
}

} // namespace
