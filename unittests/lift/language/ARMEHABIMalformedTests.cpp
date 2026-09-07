//===- ARMEHABIMalformedTests.cpp - ARM EHABI malformed entry tests ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "ARMEHABITestsDetail.h"
#include "gtest/gtest.h"

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

TEST(ARMEHABI, RejectsOverflowingWideStackAllocations) {
  for (unsigned Mode = 0; Mode < 4; ++Mode) {
    SCOPED_TRACE(Mode);
    BinaryImage Img = makeARMImage();
    ByteBuilder Operand;
    if (Mode == 0)
      Operand.uleb(1);
    else if (Mode == 1)
      Operand.uleb(uint64_t(1) << 62);
    else if (Mode == 2)
      Operand.uleb((~uint64_t(0) - 0x204) / 4 + 1);
    else {
      Operand.pad(9, 0x80);
      Operand.u8(2); // Bit 64 must not be discarded by the ULEB decoder.
    }
    std::vector<uint8_t> Opcodes = {0xB2};
    Opcodes.insert(Opcodes.end(), Operand.data().begin(), Operand.data().end());
    while ((Opcodes.size() - 2) % 4 != 0)
      Opcodes.push_back(0xB0);
    ByteBuilder Entry;
    Entry.u32(0x81000000u | ((Opcodes.size() - 2) / 4 << 16) |
              (uint32_t(Opcodes[0]) << 8) | Opcodes[1]);
    for (size_t I = 2; I < Opcodes.size(); I += 4)
      Entry.u32((uint32_t(Opcodes[I]) << 24) |
                (uint32_t(Opcodes[I + 1]) << 16) |
                (uint32_t(Opcodes[I + 2]) << 8) | Opcodes[I + 3]);
    write(Img, kExTabVA, Entry.data());
    IndexBuilder Index;
    Index.tableRef(kTextVA, kExTabVA);
    Index.install(Img);

    parseARMEHABIExceptions(Img);

    ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
    const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
    if (Mode == 0) {
      EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
      ASSERT_FALSE(F.UnwindOperations.empty());
      EXPECT_EQ(F.UnwindOperations[0].StackOffset, 0x208u);
    } else {
      EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Malformed);
      EXPECT_EQ(Img.ExceptionMetadata.ParseStatus,
                ExceptionParseStatus::Malformed);
      EXPECT_TRUE(F.UnwindOperations.empty());
    }
  }
}

TEST(ARMEHABI, MarksTruncatedInlineOpcodesMalformed) {
  BinaryImage Img = makeARMImage();
  IndexBuilder Index;
  Index.inlineCompact(kTextVA, 0x80000080u);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].ParseStatus,
            ExceptionParseStatus::Malformed);
  EXPECT_EQ(Img.ExceptionMetadata.ParseStatus, ExceptionParseStatus::Malformed);
}

} // namespace
