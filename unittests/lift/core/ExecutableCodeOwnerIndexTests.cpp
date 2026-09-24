//===- ExecutableCodeOwnerIndexTests.cpp - Scoped ownership lookups -------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"

#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <set>
#include <thread>

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
  Image.Symbols = {Symbol::makeFunc(0),
                   Symbol::makeFunc(0x200),
                   Symbol::makeFunc(0x220, 0x10),
                   Symbol::makeFunc(0x22e, 0x20),
                   Symbol::makeFunc(0x260, InvalidVA),
                   Symbol::makeFunc(InvalidVA),
                   Symbol::makeFunc(InvalidVA - 0x40, 0x20),
                   Symbol::makeFunc(0x2000),
                   Symbol::makeFunc(0x3000)};
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
      for (va_t Addr :
           {0x80ull, 0x148ull, 0x158ull, 0x168ull, 0x180ull, 0x190ull, 0x202ull,
            0x24eull, 0x262ull, 0x280ull, 0x300ull, 0x305ull, 0x310ull,
            0x318ull, 0x330ull, 0x338ull, 0x380ull})
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

TEST(ExecutableCodeOwnerIndex,
     FunctionSymbolStartsAreScopedAndSafeForParallelQueries) {
  BinaryImage Image = makeMetadataImage(BinaryFormat::ELF);
  for (va_t Addr = 0x500; Addr < 0x900; Addr += 4)
    Image.Symbols.push_back(Symbol::makeFunc(Addr));
  const ExecutableCodeOwnerIndex Index(Image);
  const BinaryImage &ReadOnlyImage = Image;
  std::atomic<bool> Start{false};
  std::atomic<unsigned> Mismatches{0};
  std::vector<std::thread> Workers;
  for (unsigned Worker = 0; Worker < 8; ++Worker)
    Workers.emplace_back([&] {
      while (!Start.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (unsigned Pass = 0; Pass < 4; ++Pass)
        for (va_t Addr = 0x500; Addr < 0x900; ++Addr) {
          const bool Expected = Addr % 4 == 0;
          if (ReadOnlyImage.hasFunctionSymbolAt(Addr, &Index) != Expected ||
              ReadOnlyImage.hasFunctionSymbolAt(Addr) != Expected)
            Mismatches.fetch_add(1, std::memory_order_relaxed);
        }
    });
  Start.store(true, std::memory_order_release);
  for (std::thread &Worker : Workers)
    Worker.join();
  EXPECT_EQ(Mismatches.load(), 0u);

  BinaryImage Other = Image;
  Other.Symbols = {Symbol::makeFunc(0xa00)};
  EXPECT_TRUE(Other.hasFunctionSymbolAt(0xa00, &Index));
  EXPECT_FALSE(Other.hasFunctionSymbolAt(0x500, &Index));
  Image.Symbols = {Symbol::makeFunc(0xb00)};
  EXPECT_TRUE(Image.hasFunctionSymbolAt(0xb00));
  EXPECT_FALSE(Image.hasFunctionSymbolAt(0x500));
  const ExecutableCodeOwnerIndex Updated(Image);
  EXPECT_TRUE(Image.hasFunctionSymbolAt(0xb00, &Updated));
  EXPECT_FALSE(Image.hasFunctionSymbolAt(0x500, &Updated));
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

TEST(ExecutableCodeOwnerIndex, FunctionEndsKeepExactRawEntriesAndSmallestEnd) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (bool Thumb : {false, true}) {
      BinaryImage Image = makeMetadataImage(Format, Thumb);
      Image.KnownCodeRanges = {
          {0x100, 0x140}, {0x100, 0x120}, {0x110, 0x150},
          {0x140, 0x180}, {0x180, 0x180}, {0x190, 0x188},
          {0x200, InvalidVA}, {0x300, 0x340}, {0x300, 0x310},
          {0x401, 0x411}, {0, 0x10}, {InvalidVA - 8, InvalidVA}};
      Image.Symbols = {Symbol::makeFunc(0, 4),
                       Symbol::makeFunc(0x100, 0x18),
                       Symbol::makeFunc(0x200, 0x20),
                       Symbol::makeFunc(0x220),
                       Symbol::makeFunc(0x230, InvalidVA),
                       Symbol::makeFunc(0x240, InvalidVA - 0x240),
                       Symbol::makeFunc(0x300, 0x20),
                       Symbol::makeFunc(0x401, 4),
                       Symbol::makeFunc(InvalidVA - 16, 8),
                       Symbol::makeFunc(InvalidVA, 1)};
      Symbol Data;
      Data.Addr = 0x500;
      Data.Size = 4;
      Image.Symbols.push_back(Data);
      const std::pair<va_t, va_t> Expected[] = {
          {0, 4}, {1, InvalidVA}, {0x100, 0x118}, {0x108, InvalidVA},
          {0x110, 0x150}, {0x140, 0x180}, {0x180, InvalidVA},
          {0x190, InvalidVA}, {0x200, 0x220}, {0x220, InvalidVA},
          {0x230, InvalidVA}, {0x240, InvalidVA}, {0x300, 0x310},
          {0x400, InvalidVA}, {0x401, 0x405}, {0x402, InvalidVA},
          {0x500, InvalidVA}, {InvalidVA - 16, InvalidVA - 8},
          {InvalidVA - 8, InvalidVA}, {InvalidVA, InvalidVA}};
      for (unsigned Order = 0; Order < 2; ++Order) {
        {
          const ExecutableCodeOwnerIndex Index(Image);
          for (const auto &[Entry, End] : Expected) {
            EXPECT_EQ(Image.getFunctionMetadataEnd(Entry), End) << Entry;
            EXPECT_EQ(Image.getFunctionMetadataEnd(Entry, &Index), End)
                << Entry;
          }
        }
        std::reverse(Image.KnownCodeRanges.begin(), Image.KnownCodeRanges.end());
        std::reverse(Image.Symbols.begin(), Image.Symbols.end());
      }
    }
}

TEST(ExecutableCodeOwnerIndex, FunctionEndScopeObservesEditsAndImageMismatch) {
  BinaryImage Image = makeMetadataImage();
  Image.KnownCodeRanges = {{0x100, 0x140}};
  Image.Symbols = {Symbol::makeFunc(0x200, 0x20)};
  {
    const ExecutableCodeOwnerIndex Index(Image);
    EXPECT_EQ(Image.getFunctionMetadataEnd(0x100, &Index), 0x140u);
    EXPECT_EQ(Image.getFunctionMetadataEnd(0x200, &Index), 0x220u);
    EXPECT_EQ(Image.getFunctionMetadataEnd(0x300, &Index), InvalidVA);
    BinaryImage Other = Image;
    Other.KnownCodeRanges = {{0x100, 0x110}, {0x300, 0x340}};
    Other.Symbols.clear();
    EXPECT_EQ(Other.getFunctionMetadataEnd(0x100, &Index), 0x110u);
    EXPECT_EQ(Other.getFunctionMetadataEnd(0x200, &Index), InvalidVA);
    EXPECT_EQ(Other.getFunctionMetadataEnd(0x300, &Index), 0x340u);
  }
  Image.KnownCodeRanges = {{0x300, 0x310}};
  Image.Symbols = {Symbol::makeFunc(0x100, 8)};
  const ExecutableCodeOwnerIndex Updated(Image);
  EXPECT_EQ(Image.getFunctionMetadataEnd(0x100, &Updated), 0x108u);
  EXPECT_EQ(Image.getFunctionMetadataEnd(0x200, &Updated), InvalidVA);
  EXPECT_EQ(Image.getFunctionMetadataEnd(0x300, &Updated), 0x310u);
}

TEST(ExecutableCodeOwnerIndex, IndexedFunctionEndsPreserveNativeInteriorRoots) {
  enum class Bound { Metadata, Exception, NextEntry, NextOnly, Containing, None };
  constexpr va_t Entry = 0x1000;
  for (Arch TargetArch : {Arch::AArch64, Arch::X64})
    for (Bound Kind : {Bound::Metadata, Bound::Exception, Bound::NextEntry,
                       Bound::NextOnly, Bound::Containing, Bound::None}) {
      SCOPED_TRACE(static_cast<int>(TargetArch));
      SCOPED_TRACE(static_cast<int>(Kind));
      BinaryImage Image;
      Image.Format = BinaryFormat::MachO;
      Image.Arch = TargetArch;
      Image.Bits = Bitness::Bits64;
      Image.Entry = Entry;
      Segment Text;
      Text.VA = Entry;
      Text.Size = Text.FileSz = 0x40;
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Text.Data.resize(Text.Size);
      for (size_t Offset = 0; Offset < Text.Size; Offset += 0x10) {
        if (TargetArch == Arch::AArch64) {
          Text.Data[Offset] = 0xc0;
          Text.Data[Offset + 1] = 0x03;
          Text.Data[Offset + 2] = 0x5f;
          Text.Data[Offset + 3] = 0xd6; // ret
        } else {
          Text.Data[Offset] = 0xc3; // ret
        }
      }
      Image.Segments.push_back(std::move(Text));
      Segment Data;
      Data.VA = 0x8000;
      Data.Size = Data.FileSz = 3 * sizeof(uint64_t);
      Data.Flags = SegmentFlags::Readable;
      Data.Data.resize(Data.Size);
      for (size_t I = 0; I < 3; ++I) {
        const va_t Target = Entry + (I + 1) * 0x10;
        for (unsigned Byte = 0; Byte < sizeof(uint64_t); ++Byte)
          Data.Data[I * sizeof(uint64_t) + Byte] =
              static_cast<uint8_t>(Target >> (8 * Byte));
        Image.CodePtrRelocSlots.insert(Data.VA + I * sizeof(uint64_t));
      }
      Image.Segments.push_back(std::move(Data));
      Image.KnownCodeRanges = {{Entry, Entry + 0x40},
                               {Entry + 0x10, Entry + 0x40}};
      Image.Symbols = {Symbol::makeFunc(Entry, 0x30)};
      std::set<va_t> Entries{Entry};
      std::set<va_t> Expected{Entry, Entry + 0x10, Entry + 0x20};
      if (Kind == Bound::Exception || Kind == Bound::Containing) {
        ExceptionFunction Metadata;
        Metadata.Kind = RuntimeFunctionKind::Primary;
        Metadata.CodeRange = {
            Kind == Bound::Exception ? Entry : Entry - 0x10, Entry + 0x20};
        Image.ExceptionMetadata.Functions.push_back(Metadata);
        if (Kind == Bound::Exception)
          Expected.erase(Entry + 0x20);
      }
      if (Kind == Bound::NextEntry) {
        Entries.insert(Entry + 0x20);
        Expected.erase(Entry + 0x20);
      }
      if (Kind == Bound::NextOnly || Kind == Bound::None) {
        Image.KnownCodeRanges = {{Entry, InvalidVA}};
        Image.Symbols = {Symbol::makeFunc(Entry, InvalidVA - Entry)};
        if (Kind == Bound::NextOnly)
          Entries.insert(Entry + 0x30);
        else
          Expected = {Entry};
      }

      const ExecutableCodeOwnerIndex Index(Image);
      BinaryImage Other = Image;
      Other.KnownCodeRanges = {{Entry, Entry + 0x10}};
      Other.Symbols.clear();
      const ExecutableCodeOwnerIndex ForeignIndex(Other);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(TargetArch));
      CFGBuilder Builder;
      Builder.setKnownFuncEntries(&Entries);
      std::set<va_t> LiveRoots;
      for (const ExecutableCodeOwnerIndex *Owners :
           {static_cast<const ExecutableCodeOwnerIndex *>(nullptr), &Index,
            &ForeignIndex}) {
        Builder.setExecutableCodeOwnerIndex(Owners);
        const LowFunc Func = Builder.build(Image, Dec, Entry);
        std::set<va_t> Starts;
        for (const auto &Block : Func.Blocks)
          Starts.insert(Block.StartAddr);
        EXPECT_EQ(Starts, Expected);
        EXPECT_EQ(Func.DecodedInstructionCount, Expected.size());
        EXPECT_EQ(Func.LiftedInstructionCount, Expected.size());
        if (!Owners)
          LiveRoots = Func.ModuleAnalysisRoots;
        else
          EXPECT_EQ(Func.ModuleAnalysisRoots, LiveRoots);
      }
    }
}
