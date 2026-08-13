//===- ARMEHABITests.cpp - ARM EHABI index tests ----------------------===//
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

TEST(ARMEHABI, ReadsEveryShapeAnIndexEntryTakes) {
  BinaryImage Img = makeARMImage();
  // Personality routine 0 with three `finish` bytes, which is what the linker
  // leaves for a frame that saves nothing.
  const uint32_t InlineDescriptor = 0x80B0B0B0u;
  write(Img, kExTabVA,
        buildGenericEntry(kExTabVA, kPersonalityVA, 0x00B0B0B0u, {}, {}));
  // Personality routine 1: one further opcode word, and the descriptor list
  // EHABI defines for it, here empty.
  ByteBuilder Compact;
  Compact.u32(0x8101B0B0u);
  Compact.u32(0xB0B0B0B0u);
  Compact.u32(0x00000000u);
  write(Img, kExTabVA + 0x100, Compact.data());

  IndexBuilder Index;
  Index.cantUnwind(kTextVA + 0x000);
  Index.inlineCompact(kTextVA + 0x100, InlineDescriptor);
  Index.tableRef(kTextVA + 0x200, kExTabVA);
  Index.tableRef(kTextVA + 0x300, kExTabVA + 0x100);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  const ExceptionInfo &EH = Img.ExceptionMetadata;
  ASSERT_EQ(EH.Functions.size(), 4u);
  EXPECT_TRUE(EH.hasModel(ExceptionModel::ARMEHABI));
  EXPECT_EQ(EH.ParseStatus, ExceptionParseStatus::Complete);

  const ExceptionFunction *Refuses = frameAt(Img, kTextVA + 0x000);
  ASSERT_NE(Refuses, nullptr);
  ASSERT_TRUE(Refuses->ARMEHABI.has_value());
  EXPECT_EQ(Refuses->ARMEHABI->Kind, ARMEHABIEntryKind::CantUnwind);
  EXPECT_EQ(Refuses->Encoding, ExceptionEncoding::ARMEHABICantUnwind);
  EXPECT_EQ(Refuses->model(), ExceptionModel::ARMEHABI);

  const ExceptionFunction *Inline = frameAt(Img, kTextVA + 0x100);
  ASSERT_NE(Inline, nullptr);
  ASSERT_TRUE(Inline->ARMEHABI.has_value());
  EXPECT_EQ(Inline->ARMEHABI->Kind, ARMEHABIEntryKind::InlineCompact);
  EXPECT_EQ(Inline->Encoding, ExceptionEncoding::ARMEHABIInline);
  EXPECT_EQ(Inline->Personality, ExceptionPersonality::AeabiUnwindCppPr0);
  // Nothing points anywhere: the index word is the whole descriptor.
  EXPECT_EQ(Inline->ARMEHABI->TableEntryVA, 0u);

  const ExceptionFunction *Generic = frameAt(Img, kTextVA + 0x200);
  ASSERT_NE(Generic, nullptr);
  ASSERT_TRUE(Generic->ARMEHABI.has_value());
  EXPECT_EQ(Generic->ARMEHABI->Kind, ARMEHABIEntryKind::Generic);
  EXPECT_EQ(Generic->Encoding, ExceptionEncoding::ARMEHABIGeneric);
  EXPECT_EQ(Generic->ARMEHABI->TableEntryVA, kExTabVA);
  EXPECT_EQ(Generic->PersonalityVA, kPersonalityVA);
  EXPECT_FALSE(Generic->ARMEHABI->PersonalityIndex.has_value());

  const ExceptionFunction *Compacted = frameAt(Img, kTextVA + 0x300);
  ASSERT_NE(Compacted, nullptr);
  ASSERT_TRUE(Compacted->ARMEHABI.has_value());
  EXPECT_EQ(Compacted->ARMEHABI->Kind, ARMEHABIEntryKind::Compact);
  EXPECT_EQ(Compacted->Encoding, ExceptionEncoding::ARMEHABICompact);
  EXPECT_EQ(Compacted->Personality, ExceptionPersonality::AeabiUnwindCppPr1);
  EXPECT_EQ(Compacted->ARMEHABI->ExtraWordCount, 1u);
  // An ARM-defined routine takes scope descriptors, not an LSDA, so nothing
  // may read a call-site table out of the words after its opcodes.
  EXPECT_FALSE(Compacted->Itanium.has_value());
}

TEST(ARMEHABI, TakesEachFunctionsExtentFromTheEntryAfterIt) {
  BinaryImage Img = makeARMImage();
  IndexBuilder Index;
  Index.cantUnwind(kTextVA + 0x000);
  Index.cantUnwind(kTextVA + 0x040);
  Index.cantUnwind(kTextVA + 0x0C0);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 3u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].CodeRange.End, kTextVA + 0x040);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[1].CodeRange.End, kTextVA + 0x0C0);
  // Nothing follows the last entry, so its extent is what the section it
  // starts in can hold.
  EXPECT_EQ(Img.ExceptionMetadata.Functions[2].CodeRange.End,
            kTextVA + kTextSize);
}

TEST(ARMEHABI, SeedsFunctionDiscoveryFromTheIndex) {
  BinaryImage Img = makeARMImage();
  IndexBuilder Index;
  Index.cantUnwind(kTextVA + 0x000);
  Index.cantUnwind(kTextVA + 0x040);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  // The index covers every function the linker placed, which is the strongest
  // evidence a stripped image of this target has about where they begin.
  ASSERT_EQ(Img.Symbols.size(), 2u);
  EXPECT_EQ(Img.Symbols[0].Addr, kTextVA + 0x000);
  EXPECT_TRUE(Img.Symbols[0].IsFunc);
  EXPECT_EQ(Img.Symbols[0].Size, 0x40u);
  EXPECT_EQ(Img.Symbols[1].Addr, kTextVA + 0x040);
}

TEST(ARMEHABI, SortsAnIndexThatArrivedOutOfOrder) {
  BinaryImage Img = makeARMImage();
  IndexBuilder Index;
  Index.cantUnwind(kTextVA + 0x080);
  Index.cantUnwind(kTextVA + 0x000);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  // An unwinder binary-searches the index, so one that is not sorted is one no
  // unwinder could use.  Recovering the extents anyway means saying so.
  EXPECT_EQ(Img.ExceptionMetadata.ParseStatus, ExceptionParseStatus::Partial);
  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 2u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].CodeRange.Begin, kTextVA);
  EXPECT_EQ(Img.ExceptionMetadata.Functions[0].CodeRange.End, kTextVA + 0x080);
}

TEST(ARMEHABI, FindsAnIndexByTheSectionTypeTheABIReservedForIt) {
  BinaryImage Img = makeARMImage();
  IndexBuilder Index;
  Index.cantUnwind(kTextVA);
  Index.install(Img);
  // A name is a convention; the type is what ARM reserved, and an index under
  // an unexpected name is still an index.
  Img.Sections.back().Name = ".unexpected";
  Img.Sections.back().Type = llvm::ELF::SHT_ARM_EXIDX;

  parseARMEHABIExceptions(Img);

  EXPECT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
}

TEST(ARMEHABI, ReadsNoIndexOutOfAnObjectTheLinkerHasNotSeen) {
  BinaryImage Img = makeARMImage();
  Img.IsRelocatable = true;
  IndexBuilder Index;
  Index.cantUnwind(kTextVA);
  Index.cantUnwind(kTextVA + 0x40);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  // Every field is owed by a relocation that has not been applied, so each
  // entry's displacement reads as zero and names its own address.  Frames
  // built from that would land on whatever sits at the bottom of the
  // synthesized layout.
  EXPECT_TRUE(Img.ExceptionMetadata.Functions.empty());
  EXPECT_TRUE(Img.Symbols.empty());
}

TEST(ARMEHABI, LeavesAnImageOfAnotherMachineAlone) {
  BinaryImage Img = makeARMImage();
  Img.Arch = Arch::AArch64;
  IndexBuilder Index;
  Index.cantUnwind(kTextVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  // The section name is not reserved to this machine, and its words only mean
  // what they mean at this pointer size.
  EXPECT_TRUE(Img.ExceptionMetadata.Functions.empty());
  EXPECT_FALSE(Img.ExceptionMetadata.hasModel(ExceptionModel::ARMEHABI));
}

} // namespace
