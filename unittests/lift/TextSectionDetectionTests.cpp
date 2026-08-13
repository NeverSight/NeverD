//===- TextSectionDetectionTests.cpp - Code-section detection -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Unit tests for the centralised text-section predicate
/// (section_names::isTextSectionName) and the flag-based fallback in
/// BinaryImage::getTextSection() that recovers the primary code section when
/// the canonical name is absent — the case that lets NeverD harden binaries
/// whose code section was renamed by a packer/protector (VMProtect ".vmp0",
/// UPX "UPX1", Themida, randomised names).
///
//===----------------------------------------------------------------------===//

#include "neverd/object/SectionNames.h"
#include "neverd/loader/BinaryImage.h"

#include <gtest/gtest.h>

using namespace neverd;

namespace {

Section makeSection(std::string Name, va_t VA, uint64_t Size, bool Exec) {
  Section S;
  S.Name = std::move(Name);
  S.VA = VA;
  S.Size = Size;
  S.FileOff = VA; // arbitrary-but-consistent for these layout-free tests
  S.Flags = Exec ? (SegmentFlags::Readable | SegmentFlags::Executable)
                 : SegmentFlags::Readable;
  return S;
}

Segment makeSegment(std::string Name, va_t VA, uint64_t Size, bool Exec) {
  Segment S;
  S.Name = std::move(Name);
  S.VA = VA;
  S.Size = Size;
  S.FileOff = VA;
  S.Flags = Exec ? (SegmentFlags::Readable | SegmentFlags::Executable)
                 : SegmentFlags::Readable;
  return S;
}

} // namespace

//===----------------------------------------------------------------------===//
// isTextSectionName — single authoritative predicate
//===----------------------------------------------------------------------===//

TEST(IsTextSectionName, AcceptsCanonicalNames) {
  EXPECT_TRUE(section_names::isTextSectionName(section_names::elf::Text));
  EXPECT_TRUE(section_names::isTextSectionName(section_names::macho::Text));
}

TEST(IsTextSectionName, AcceptsElfFunctionSplitSections) {
  EXPECT_TRUE(section_names::isTextSectionName(".text.hot"));
  EXPECT_TRUE(section_names::isTextSectionName(".text.unlikely"));
  EXPECT_TRUE(section_names::isTextSectionName(".text.startup"));
  EXPECT_TRUE(section_names::isTextSectionName(".text.foo.bar"));
}

TEST(IsTextSectionName, AcceptsCoffGroupedSections) {
  EXPECT_TRUE(section_names::isTextSectionName(".text$mn"));
  EXPECT_TRUE(section_names::isTextSectionName(".text$x"));
}

TEST(IsTextSectionName, RejectsTextbssAndLookalikes) {
  // The mandatory '.'/'$' separator after ".text" excludes these.
  EXPECT_FALSE(section_names::isTextSectionName(".textbss"));
  EXPECT_FALSE(section_names::isTextSectionName(".text2"));
  EXPECT_FALSE(section_names::isTextSectionName(".textsomething"));
  EXPECT_FALSE(section_names::isTextSectionName("__textbss"));
}

TEST(IsTextSectionName, RejectsUnrelatedNames) {
  EXPECT_FALSE(section_names::isTextSectionName(section_names::elf::Data));
  EXPECT_FALSE(section_names::isTextSectionName(section_names::elf::Rodata));
  EXPECT_FALSE(section_names::isTextSectionName(section_names::elf::Bss));
  EXPECT_FALSE(section_names::isTextSectionName(section_names::macho::Data));
  EXPECT_FALSE(section_names::isTextSectionName("text")); // no leading dot
  EXPECT_FALSE(section_names::isTextSectionName(".tex"));
  EXPECT_FALSE(section_names::isTextSectionName(""));
}

TEST(IsElfImageDataSectionName, AcceptsEmbeddedDataSections) {
  EXPECT_TRUE(section_names::isElfImageDataSectionName(section_names::elf::Rodata));
  EXPECT_TRUE(section_names::isElfImageDataSectionName(section_names::elf::Data));
  EXPECT_TRUE(section_names::isElfImageDataSectionName(section_names::elf::Bss));
  EXPECT_TRUE(
      section_names::isElfImageDataSectionName(section_names::elf::DataRelRo));
  EXPECT_TRUE(section_names::isElfImageDataSectionName(".rodata.str1.1"));
  EXPECT_TRUE(section_names::isElfImageDataSectionName(".data.rel.local"));
}

TEST(IsElfImageDataSectionName, RejectsTextAndUnknown) {
  EXPECT_FALSE(
      section_names::isElfImageDataSectionName(section_names::elf::Text));
  EXPECT_FALSE(section_names::isElfImageDataSectionName(".vmp0"));
  EXPECT_FALSE(section_names::isElfImageDataSectionName(""));
}

TEST(IsELFExecutableMapSection, AcceptsCodeOutputSections) {
  EXPECT_TRUE(section_names::isELFExecutableMapSection(section_names::elf::Text));
  EXPECT_TRUE(section_names::isELFExecutableMapSection(section_names::elf::Init));
  EXPECT_TRUE(section_names::isELFExecutableMapSection(section_names::elf::Plt));
}

TEST(IsELFExecutableMapSection, RejectsDataSections) {
  EXPECT_FALSE(section_names::isELFExecutableMapSection(section_names::elf::Data));
  EXPECT_FALSE(
      section_names::isELFExecutableMapSection(section_names::elf::Rodata));
}

TEST(IsMachOExecutableMapSection, AcceptsTextStubs) {
  EXPECT_TRUE(section_names::isMachOExecutableMapSection(
      section_names::macho::TextSeg, section_names::macho::Text));
  EXPECT_TRUE(section_names::isMachOExecutableMapSection(
      section_names::macho::TextSeg, section_names::macho::Stubs));
}

TEST(IsMachOExecutableMapSection, RejectsOtherSegments) {
  EXPECT_FALSE(section_names::isMachOExecutableMapSection(
      section_names::macho::DataSeg, section_names::macho::Text));
  EXPECT_FALSE(section_names::isMachOExecutableMapSection(
      section_names::macho::TextSeg, section_names::macho::Data));
}

//===----------------------------------------------------------------------===//
// getTextSection — named lookup still wins when present
//===----------------------------------------------------------------------===//

TEST(GetTextSection, NamedElfTextWinsOverHeuristic) {
  BinaryImage Img;
  Img.Format = BinaryFormat::ELF;
  // A small ".text" plus a much larger exec section that also holds the entry.
  // The named lookup must take precedence over the size/entry heuristic.
  Img.Sections.push_back(
      makeSection(section_names::elf::Text, 0x1000, 0x100, /*Exec=*/true));
  Img.Sections.push_back(makeSection(".other", 0x2000, 0x7000, /*Exec=*/true));
  Img.Entry = 0x2000;

  const Section *T = Img.getTextSection();
  ASSERT_NE(T, nullptr);
  EXPECT_EQ(T->Name, section_names::elf::Text);
}

TEST(GetTextSection, NamedMachOTextWins) {
  BinaryImage Img;
  Img.Format = BinaryFormat::MachO;
  Img.Sections.push_back(
      makeSection(section_names::macho::Text, 0x4000, 0x200, /*Exec=*/true));

  const Section *T = Img.getTextSection();
  ASSERT_NE(T, nullptr);
  EXPECT_EQ(T->Name, section_names::macho::Text);
}

//===----------------------------------------------------------------------===//
// getTextSection — flag-based fallback for renamed / packed code sections
//===----------------------------------------------------------------------===//

TEST(GetTextSection, FallsBackToExecutableSectionWhenNoText) {
  // Mimics a VMProtect-packed PE: no ".text", code lives in ".vmp0".
  BinaryImage Img;
  Img.Format = BinaryFormat::ELF;
  Img.Sections.push_back(makeSection(".vmp0", 0x1000, 0x3000, /*Exec=*/true));
  Img.Entry = 0x1000;

  const Section *T = Img.getTextSection();
  ASSERT_NE(T, nullptr);
  EXPECT_EQ(T->Name, ".vmp0");
  EXPECT_EQ(T->VA, 0x1000u);
  EXPECT_EQ(T->Size, 0x3000u);
}

TEST(GetTextSection, EntryContainingSectionBeatsLargest) {
  // Two executable sections, none named ".text". The one *containing the
  // entry point* must win even though it is the smaller of the two.
  BinaryImage Img;
  Img.Format = BinaryFormat::ELF;
  Img.Sections.push_back(makeSection(".big", 0x1000, 0x4000, /*Exec=*/true));
  Img.Sections.push_back(makeSection(".small", 0x8000, 0x100, /*Exec=*/true));
  Img.Entry = 0x8050; // inside ".small"

  const Section *T = Img.getTextSection();
  ASSERT_NE(T, nullptr);
  EXPECT_EQ(T->Name, ".small");
}

TEST(GetTextSection, LargestExecutableWhenEntryUnknown) {
  // Entry == 0 (or outside every section): fall back to the largest exec sec.
  BinaryImage Img;
  Img.Format = BinaryFormat::ELF;
  Img.Sections.push_back(makeSection(".a", 0x1000, 0x100, /*Exec=*/true));
  Img.Sections.push_back(makeSection(".b", 0x2000, 0x900, /*Exec=*/true));
  Img.Sections.push_back(makeSection(".c", 0x3000, 0x200, /*Exec=*/true));
  Img.Entry = 0;

  const Section *T = Img.getTextSection();
  ASSERT_NE(T, nullptr);
  EXPECT_EQ(T->Name, ".b");
}

TEST(GetTextSection, LargestExecutableWhenEntryOutsideAllSections) {
  BinaryImage Img;
  Img.Format = BinaryFormat::ELF;
  Img.Sections.push_back(makeSection(".a", 0x1000, 0x100, /*Exec=*/true));
  Img.Sections.push_back(makeSection(".b", 0x2000, 0x900, /*Exec=*/true));
  Img.Entry = 0xdead0000; // not contained anywhere

  const Section *T = Img.getTextSection();
  ASSERT_NE(T, nullptr);
  EXPECT_EQ(T->Name, ".b");
}

TEST(GetTextSection, IgnoresNonExecutableSections) {
  // A renamed-code binary still has data sections; a bigger data section must
  // never be picked over a smaller executable one.
  BinaryImage Img;
  Img.Format = BinaryFormat::ELF;
  Img.Sections.push_back(
      makeSection(section_names::elf::Rodata, 0x1000, 0x9000, /*Exec=*/false));
  Img.Sections.push_back(makeSection(".vmp0", 0x20000, 0x80, /*Exec=*/true));
  Img.Entry = 0;

  const Section *T = Img.getTextSection();
  ASSERT_NE(T, nullptr);
  EXPECT_EQ(T->Name, ".vmp0");
}

TEST(GetTextSection, ReturnsNullWhenNoExecutableSection) {
  BinaryImage Img;
  Img.Format = BinaryFormat::ELF;
  Img.Sections.push_back(
      makeSection(section_names::elf::Data, 0x1000, 0x1000, /*Exec=*/false));
  Img.Sections.push_back(
      makeSection(section_names::elf::Rodata, 0x2000, 0x1000, /*Exec=*/false));
  Img.Entry = 0x1000;

  EXPECT_EQ(Img.getTextSection(), nullptr);
}

TEST(GetTextSection, MachOFallbackToExecutableSection) {
  // No "__text"; renamed Mach-O code section recovered via the exec flag.
  BinaryImage Img;
  Img.Format = BinaryFormat::MachO;
  Img.Sections.push_back(makeSection("__mytext", 0x4000, 0x500, /*Exec=*/true));
  Img.Entry = 0x4000;

  const Section *T = Img.getTextSection();
  ASSERT_NE(T, nullptr);
  EXPECT_EQ(T->Name, "__mytext");
}

//===----------------------------------------------------------------------===//
// getTextSegment — composes with the new getTextSection fallback
//===----------------------------------------------------------------------===//

TEST(GetTextSegment, ResolvesSegmentOfRenamedCodeSection) {
  BinaryImage Img;
  Img.Format = BinaryFormat::ELF;
  // Executable LOAD segment with a renamed code section living inside it.
  Img.Segments.push_back(makeSegment("exec_seg", 0x1000, 0x10000,
                                      /*Exec=*/true));
  Img.Sections.push_back(makeSection(".vmp0", 0x1000, 0x2000, /*Exec=*/true));
  Img.Entry = 0x1000;

  const Segment *Seg = Img.getTextSegment();
  ASSERT_NE(Seg, nullptr);
  EXPECT_EQ(Seg->Name, "exec_seg");
}
