//===- ARMEHABIMalformedTests.cpp - ARM EHABI malformed entry tests ---===//
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
// Malformed entries
//===----------------------------------------------------------------------===//

TEST(ARMEHABI, RefusesACompactEntryFromAnUndefinedVendor) {
  BinaryImage Img = makeARMImage();
  ByteBuilder Entry;
  Entry.u32(0x90B0B0B0u); // bits 30-28 select a vendor that does not exist
  write(Img, kExTabVA, Entry.data());

  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].ParseStatus,
            ExceptionParseStatus::Malformed);
  EXPECT_TRUE(Img.ExceptionMetadata.Functions[0].UnwindOperations.empty());
}

TEST(ARMEHABI, RefusesAPersonalityIndexTheABIDoesNotDefine) {
  BinaryImage Img = makeARMImage();
  ByteBuilder Entry;
  Entry.u32(0x8300B0B0u); // ARM defined routines 0, 1 and 2, and no others
  write(Img, kExTabVA, Entry.data());

  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].ParseStatus,
            ExceptionParseStatus::Malformed);
}

TEST(ARMEHABI, RefusesAnInlineEntryNamingARoutineThatCannotFitInline) {
  BinaryImage Img = makeARMImage();
  IndexBuilder Index;
  // Routine 1 needs a word count, and the index word has no field for one.
  Index.inlineCompact(kTextVA, 0x8101B0B0u);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].ParseStatus,
            ExceptionParseStatus::Malformed);
}

TEST(ARMEHABI, RefusesAnEntryDeclaringOpcodeWordsItDoesNotHave) {
  BinaryImage Img = makeARMImage();
  // Placed so that the words the count claims run past the end of the segment
  // rather than merely past the end of the entry.
  const va_t EdgeVA = kTableVA + kTableSize - 8;
  ByteBuilder Entry;
  Entry.u32(prel31(kPersonalityVA, EdgeVA));
  Entry.u32(0xFF000000u); // 255 further opcode words
  write(Img, EdgeVA, Entry.data());

  IndexBuilder Index;
  Index.tableRef(kTextVA, EdgeVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].ParseStatus,
            ExceptionParseStatus::Malformed);
}

} // namespace
