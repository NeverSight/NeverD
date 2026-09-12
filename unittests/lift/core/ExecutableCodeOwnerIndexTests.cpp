//===- ExecutableCodeOwnerIndexTests.cpp - Scoped ownership lookups -------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"

#include <initializer_list>

using namespace neverd;

namespace {

BinaryImage makeMetadataImage(BinaryFormat Format = BinaryFormat::MachO,
                              bool Thumb = false) {
  BinaryImage Image;
  Image.Format = Format;
  Image.Arch = Thumb ? Arch::ARM : Arch::AArch64;
  Image.Mode = Thumb ? InstructionMode::Thumb : InstructionMode::Default;

  auto addMapping = [&](va_t VA, uint64_t Size, bool Executable) {
    Segment Mapping;
    Mapping.VA = VA;
    Mapping.Size = Size;
    Mapping.Flags = SegmentFlags::Readable;
    if (Executable)
      Mapping.Flags = Mapping.Flags | SegmentFlags::Executable;
    Image.Segments.push_back(Mapping);
    Section Data;
    Data.VA = VA;
    Data.Size = Size;
    Data.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(Data);
  };
  addMapping(0, 0x1000, true);
  addMapping(0x2000, 0x100, false);
  addMapping(InvalidVA - 0x100, 0x101, true);

  // The raw Thumb spelling is instruction-owned, while its even address is
  // data-owned. Only legacy Mach-O IAT semantics may use that raw spelling.
  Section Code;
  Code.VA = 0x401;
  Code.Size = 1;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
  Image.Sections.insert(Image.Sections.begin(), Code);

  Image.Entry = 0x50;
  Image.RuntimeFunctionAddrs = {0x61};
  Image.VerifiedFunctionEntries = {0x70};
  Image.KnownCodeRanges = {{0x100, 0x140}, {0x110, 0x120}, {0x140, 0x148},
                          {0x160, 0x168}, {0x180, 0x180}, {0x198, 0x190}};
  Image.Symbols = {Symbol::makeFunc(0), Symbol::makeFunc(0x200),
                   Symbol::makeFunc(0x220, 0x10),
                   Symbol::makeFunc(0x22e, 0x20),
                   Symbol::makeFunc(0x260, InvalidVA),
                   Symbol::makeFunc(InvalidVA),
                   Symbol::makeFunc(InvalidVA - 0x40, 0x20),
                   Symbol::makeFunc(0x2000), Symbol::makeFunc(0x3000)};
  Symbol DataSymbol;
  DataSymbol.Addr = 0x280;
  DataSymbol.Size = 0x20;
  Image.Symbols.push_back(DataSymbol);
  Image.Imports = {{"", "legacy", 0, 0x401}, {"", "data", 0, 0x380}};
  Image.ImportStubIndices = {{0x301, 0}, {0x305, Image.Imports.size()}};
  Image.ImportStubRanges = {{0x311, 0x314}, {0x312, 0x316}, {0x316, 0x318},
                            {0x320, 0x322}, {0x330, 0x330}, {0x340, 0x338}};
  return Image;
}

void expectSame(const BinaryImage &Image,
                const ExecutableCodeOwnerIndex &Index) {
  for (va_t Addr = 0; Addr < 0x1100; ++Addr)
    EXPECT_EQ(Image.hasExecutableCodeOwnerAt(Addr, &Index),
              Image.hasExecutableCodeOwnerAt(Addr))
        << "address " << Addr;
  for (va_t Offset = 0; Offset <= 0x110; ++Offset) {
    const va_t Addr = InvalidVA - Offset;
    EXPECT_EQ(Image.hasExecutableCodeOwnerAt(Addr, &Index),
              Image.hasExecutableCodeOwnerAt(Addr))
        << "high address " << Addr;
  }
  EXPECT_FALSE(Image.hasExecutableCodeOwnerAt(0x2000, &Index));
  EXPECT_FALSE(Image.hasExecutableCodeOwnerAt(0x3000, &Index));
}

} // namespace

TEST(ExecutableCodeOwnerIndex, PreservesMetadataBoundariesAndRawThumbSpelling) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (bool Thumb : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "format " << static_cast<int>(Format)
                                      << " thumb " << Thumb);
      const BinaryImage Image = makeMetadataImage(Format, Thumb);
      const ExecutableCodeOwnerIndex Index(Image);
      expectSame(Image, Index);
      for (va_t Addr : {0ull, 0x50ull, 0x61ull, 0x70ull, 0x100ull, 0x146ull,
                       0x160ull, 0x200ull, 0x220ull, 0x240ull, 0x260ull,
                       0x301ull, 0x311ull, 0x316ull, 0x320ull})
        EXPECT_TRUE(Image.hasExecutableCodeOwnerAt(Addr, &Index)) << Addr;
      for (va_t Addr : {0x80ull, 0x148ull, 0x158ull, 0x168ull, 0x180ull,
                       0x190ull, 0x202ull, 0x24eull, 0x262ull, 0x280ull,
                       0x300ull, 0x305ull, 0x310ull, 0x318ull, 0x330ull,
                       0x338ull, 0x380ull})
        EXPECT_FALSE(Image.hasExecutableCodeOwnerAt(Addr, &Index)) << Addr;
      EXPECT_TRUE(Image.hasExecutableCodeOwnerAt(InvalidVA, &Index));
      EXPECT_FALSE(Image.hasExecutableCodeOwnerAt(InvalidVA - 0x20, &Index));
      if (Thumb) {
        EXPECT_FALSE(Image.hasExecutableCodeOwnerAt(0x400, &Index));
        EXPECT_EQ(Image.hasExecutableCodeOwnerAt(0x401, &Index),
                  Format == BinaryFormat::MachO);
        // Known ranges retain raw endpoints; typed symbol starts normalize.
        BinaryImage Odd = makeMetadataImage(Format, true);
        Odd.KnownCodeRanges = {{0x801, 0x805}};
        Odd.Symbols = {Symbol::makeFunc(0x901, 3)};
        const ExecutableCodeOwnerIndex OddIndex(Odd);
        EXPECT_FALSE(Odd.hasExecutableCodeOwnerAt(0x801, &OddIndex));
        EXPECT_TRUE(Odd.hasExecutableCodeOwnerAt(0x803, &OddIndex));
        EXPECT_TRUE(Odd.hasExecutableCodeOwnerAt(0x900, &OddIndex));
        EXPECT_TRUE(Odd.hasExecutableCodeOwnerAt(0x902, &OddIndex));
        EXPECT_FALSE(Odd.hasExecutableCodeOwnerAt(0x904, &OddIndex));
        expectSame(Odd, OddIndex);
      }
    }
}

TEST(ExecutableCodeOwnerIndex,
     FreshScopeObservesEditsAndMismatchUsesLiveImage) {
  BinaryImage Image = makeMetadataImage();
  {
    const ExecutableCodeOwnerIndex Index(Image);
    expectSame(Image, Index);
    BinaryImage Other = Image;
    Other.ImportStubRanges = {{0x700, 0x710}};
    Other.Symbols = {Symbol::makeFunc(0x720, 4)};
    EXPECT_TRUE(Other.hasExecutableCodeOwnerAt(0x700, &Index));
    EXPECT_TRUE(Other.hasExecutableCodeOwnerAt(0x720, &Index));
    EXPECT_FALSE(Other.hasExecutableCodeOwnerAt(0x220, &Index));
    expectSame(Other, Index);
  }
  Image.ImportStubIndices.clear();
  Image.ImportStubRanges = {{0x750, 0x760}};
  Image.KnownCodeRanges = {{0x770, 0x780}};
  Image.Symbols = {Symbol::makeFunc(0x790, 4)};
  {
    const ExecutableCodeOwnerIndex Index(Image);
    expectSame(Image, Index);
    EXPECT_FALSE(Image.hasExecutableCodeOwnerAt(0x301, &Index));
    EXPECT_FALSE(Image.hasExecutableCodeOwnerAt(0x220, &Index));
    EXPECT_TRUE(Image.hasExecutableCodeOwnerAt(0x750, &Index));
    EXPECT_TRUE(Image.hasExecutableCodeOwnerAt(0x770, &Index));
    EXPECT_TRUE(Image.hasExecutableCodeOwnerAt(0x790, &Index));
  }
  // Legacy import preclassification depends on the exact mapping, format
  // and mode. Every mutation below begins a new operation and index.
  Image.Arch = Arch::ARM;
  Image.Mode = InstructionMode::Thumb;
  {
    const ExecutableCodeOwnerIndex Index(Image);
    EXPECT_TRUE(Image.hasExecutableCodeOwnerAt(0x401, &Index));
  }
  Image.Sections.front().Type = 0;
  {
    const ExecutableCodeOwnerIndex Index(Image);
    EXPECT_FALSE(Image.hasExecutableCodeOwnerAt(0x401, &Index));
  }
  Image.Sections.front().Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
  Image.Format = BinaryFormat::ELF;
  {
    const ExecutableCodeOwnerIndex Index(Image);
    EXPECT_FALSE(Image.hasExecutableCodeOwnerAt(0x401, &Index));
  }
  Image.Format = BinaryFormat::MachO;
  Image.Imports.clear();
  {
    const ExecutableCodeOwnerIndex Index(Image);
    EXPECT_FALSE(Image.hasExecutableCodeOwnerAt(0x401, &Index));
  }
}

TEST(ExecutableCodeOwnerIndex, KeepsMappingPrecedenceAndLiveEntryEvidence) {
  BinaryImage Image = makeMetadataImage();
  // First matching mapping wins even when a later RX mapping and typed
  // function cover the same address.
  Segment Data;
  Data.VA = 0x220;
  Data.Size = 0x10;
  Data.Flags = SegmentFlags::Readable;
  Image.Segments.insert(Image.Segments.begin(), Data);
  {
    const ExecutableCodeOwnerIndex Index(Image);
    EXPECT_FALSE(Image.hasExecutableCodeOwnerAt(0x220, &Index));
    EXPECT_TRUE(Image.hasExecutableCodeOwnerAt(0x230, &Index));
    expectSame(Image, Index);
    // These sets are read by the canonical predicate, never snapshotted.
    Image.Entry = 0x800;
    Image.RuntimeFunctionAddrs.insert(0x810);
    Image.VerifiedFunctionEntries.insert(0x820);
    EXPECT_TRUE(Image.hasExecutableCodeOwnerAt(0x800, &Index));
    EXPECT_TRUE(Image.hasExecutableCodeOwnerAt(0x810, &Index));
    EXPECT_TRUE(Image.hasExecutableCodeOwnerAt(0x820, &Index));
  }
  Image.Segments.front().Flags =
      SegmentFlags::Readable | SegmentFlags::Executable;
  {
    const ExecutableCodeOwnerIndex Index(Image);
    EXPECT_TRUE(Image.hasExecutableCodeOwnerAt(0x220, &Index));
  }
  // An overlapping first data section hides the later instruction section.
  Section FirstData = Image.Sections[1];
  Image.Sections.insert(Image.Sections.begin(), FirstData);
  {
    const ExecutableCodeOwnerIndex Index(Image);
    EXPECT_FALSE(Image.hasExecutableCodeOwnerAt(0x401, &Index));
    expectSame(Image, Index);
  }
}
