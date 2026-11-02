//===- ReadOnlyBytesTests.cpp - Immutable storage and fixup boundaries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check independent instruction, scalar and runtime-pointer storage roles.
//===----------------------------------------------------------------------===//

#include "../../lib/loader/ReadOnlyBytesIndex.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/loader/ReadOnlyBytesScope.h"
#include "neverd/object/ELFLayout.h"
#include "neverd/support/BinaryEncoding.h"

#include <cstdint>
#include <utility>

namespace neverd {
namespace {
constexpr va_t TextVA = 0x401000;
constexpr va_t ReadVA = TextVA + 0x10;
constexpr va_t OperandVA = ReadVA + 1;
constexpr va_t TargetVA = 0x402020;

BinaryImage codeImage() {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x400000;
  Segment Code;
  Code.VA = TextVA;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.resize(0x100, 0xcc);
  Code.Data[0x10] = 0xb8;
  writeLE<uint32_t>(Code.Data.data() + 0x11, TargetVA);
  Code.Data[0x15] = 0xc3;
  Code.Size = Code.FileSz = Code.Data.size();
  Section Section;
  Section.VA = Code.VA;
  Section.Flags = Code.Flags;
  Section.Size = Section.FileSz = Code.Size;
  Image.Segments.push_back(std::move(Code));
  Image.Sections.push_back(std::move(Section));
  Image.BaseRelocations.push_back({OperandVA, 3});
  return Image;
}

TEST(ImmutableImageScalar, ELFRelroRangeKeepsScalarsButNotRelocatedPointers) {
  auto Image = codeImage();
  Image.Format = BinaryFormat::ELF;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.BaseRelocations.clear();
  Image.Segments.front().Flags =
      SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Sections.front().Flags = Image.Segments.front().Flags;
  Image.Sections.front().Name = ".data.rel.ro";
  EXPECT_FALSE(readImmutableImageScalar(Image, ReadVA, 1));
  Image.ELFMetadata.emplace();
  Image.ELFMetadata->Type = llvm::ELF::ET_DYN;
  const uint64_t Extent = 0x410000 - ReadVA;
  Image.ELFMetadata->ProgramHeaders.push_back(
      {llvm::ELF::PT_GNU_RELRO, 0, 0, ReadVA, Extent, Extent, 1});
  EXPECT_EQ(readImmutableImageScalar(Image, ReadVA, 1), 0xb8u);
  Image.BaseRelocations.push_back({ReadVA - 3, 10});
  EXPECT_FALSE(readImmutableImageScalar(Image, ReadVA, 1));
  Image.BaseRelocations.clear();
  Image.Sections.front().Name = ".data.rel.ro.local";
  EXPECT_EQ(readImmutableImageScalar(Image, ReadVA, 1), 0xb8u);
  for (const char *Name :
       {".data", ".data.rel.rotten", ".data.rel.ro_mutable", ".data.rel.ro2"}) {
    SCOPED_TRACE(Name);
    Image.Sections.front().Name = Name;
    EXPECT_EQ(readImmutableImageScalar(Image, ReadVA, 1), 0xb8u);
  }
  Image.Sections.front().Name = ".data.rel.ro";
  Image.Sections.push_back(Image.Sections.front());
  EXPECT_FALSE(readImmutableImageScalar(Image, ReadVA, 1));
}

TEST(ImmutableImageScalar, NamesAndSectionFlagsCannotProtectWritableMappings) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
      for (const char *Name :
           {"__DATA_CONST", ".data.rel.ro", ".data.rel.ro.local", ".rdata"})
        for (bool WritableSection : {false, true}) {
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(static_cast<unsigned>(Architecture));
          SCOPED_TRACE(Name);
          SCOPED_TRACE(WritableSection);
          auto Image = codeImage();
          Image.Format = Format;
          Image.Arch = Architecture;
          Image.Bits = Architecture == Arch::X86 || Architecture == Arch::ARM
                           ? Bitness::Bits32
                           : Bitness::Bits64;
          Image.BaseRelocations.clear();
          auto &Segment = Image.Segments.front();
          Segment.Name = Name;
          Segment.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
          auto &Section = Image.Sections.front();
          Section.Name = Section.SegmentName = Name;
          Section.Flags =
              WritableSection ? Segment.Flags : SegmentFlags::Readable;
          EXPECT_FALSE(Image.readOnlyAfterRelocations(ReadVA, 1));
          EXPECT_FALSE(readImmutableImageScalar(Image, ReadVA, 1));
          EXPECT_TRUE(isRuntimeWritableAddress(Image, ReadVA));
          EXPECT_TRUE(classifyPointerRelocationAddress(Image, ReadVA).Writable);
          Segment.ReadOnlyAfterRelocations = true;
          EXPECT_EQ(readImmutableImageScalar(Image, ReadVA, 1), 0xb8u);
          EXPECT_FALSE(isRuntimeWritableAddress(Image, ReadVA));
          Segment.ReadOnlyAfterRelocations = false;
          Segment.Flags = SegmentFlags::Readable;
          const bool ConflictingPESection =
              Format == BinaryFormat::COFF && WritableSection;
          EXPECT_EQ(Image.readOnlyAfterRelocations(ReadVA, 1),
                    !ConflictingPESection);
          EXPECT_EQ(readImmutableImageScalar(Image, ReadVA, 1),
                    ConflictingPESection ? std::nullopt
                                         : std::optional<uint64_t>{0xb8});
          EXPECT_EQ(isRuntimeWritableAddress(Image, ReadVA),
                    ConflictingPESection);
          EXPECT_EQ(classifyPointerRelocationAddress(Image, ReadVA).Writable,
                    ConflictingPESection);
          Section.Flags = SegmentFlags::Readable;
          EXPECT_EQ(readImmutableImageScalar(Image, ReadVA, 1), 0xb8u);
          EXPECT_FALSE(isRuntimeWritableAddress(Image, ReadVA));
          EXPECT_TRUE(isRuntimeWritableAddress(Image, TextVA - 1));
        }
}

TEST(ImmutableImageScalar, RelroProofRequiresCompleteNativeRange) {
  auto Image = codeImage();
  Image.Format = BinaryFormat::ELF;
  const va_t Address = TextVA + kPageSize4K - 8;
  Image.Segments.front().Size = 2 * kPageSize4K;
  Image.BaseRelocations.clear();
  Image.Segments.front().Flags =
      SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Sections.front().Flags = Image.Segments.front().Flags;
  Image.ELFMetadata.emplace();
  Image.ELFMetadata->Type = llvm::ELF::ET_DYN;
  Image.ELFMetadata->ProgramHeaders.push_back(
      {llvm::ELF::PT_GNU_RELRO, 0, 0, Address, 8, 8, 1});
  EXPECT_TRUE(Image.readOnlyAfterRelocations(Address, 8));
  EXPECT_FALSE(Image.readOnlyAfterRelocations(Address - 1, 1));
  EXPECT_FALSE(Image.readOnlyAfterRelocations(Address, 9));
  EXPECT_FALSE(Image.readOnlyAfterRelocations(Address + 8, 1));
  EXPECT_FALSE(Image.readOnlyAfterRelocations(Address, 0));
  EXPECT_FALSE(Image.readOnlyAfterRelocations(Address, UINT64_MAX));
  for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::MachO}) {
    Image.Format = Format;
    EXPECT_FALSE(Image.readOnlyAfterRelocations(Address, 8));
  }
  Image.Format = BinaryFormat::ELF;
  Image.IsRelocatable = true;
  EXPECT_FALSE(Image.readOnlyAfterRelocations(Address, 8));
  Image.IsRelocatable = false;
  Image.ELFMetadata->ProgramHeaders.back().MemorySize = UINT64_MAX;
  EXPECT_FALSE(Image.readOnlyAfterRelocations(Address, 8));
  Image.Segments.front().Flags = SegmentFlags::Readable;
  Image.Segments.front().Size = UINT64_MAX;
  EXPECT_FALSE(Image.readOnlyAfterRelocations(Address, 1));
}

TEST(ImmutableImageScalar, RelroKeepsPartialPagesAndStaticStartupWritable) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    BinaryImage Image;
    Image.Format = BinaryFormat::ELF;
    Image.Arch = Architecture;
    Segment Data;
    Data.VA = 0x10000;
    const uint64_t Page = maximumELFProtectionPageSize(Architecture);
    ASSERT_NE(Page, 0u);
    Data.Size = 2 * Page;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Image.Segments.push_back(Data);
    Image.ELFMetadata.emplace();
    auto &Metadata = *Image.ELFMetadata;
    Metadata.Type = llvm::ELF::ET_DYN;
    Metadata.ProgramHeaders.push_back(
        {llvm::ELF::PT_GNU_RELRO, 0, 0, Data.VA + 3, Page + 4, Page + 4, 1});
    EXPECT_FALSE(Image.readOnlyAfterRelocations(Data.VA + 2, 1));
    EXPECT_TRUE(Image.readOnlyAfterRelocations(Data.VA + 3, 4));
    EXPECT_TRUE(Image.readOnlyAfterRelocations(Data.VA + Page - 1, 1));
    EXPECT_FALSE(Image.readOnlyAfterRelocations(Data.VA + Page - 1, 2));
    EXPECT_FALSE(Image.readOnlyAfterRelocations(Data.VA + Page, 1));
    EXPECT_TRUE(isRuntimeWritableAddress(Image, Data.VA + Page));
    Metadata.ProgramHeaders.back().MemorySize = Page - 4;
    EXPECT_FALSE(Image.readOnlyAfterRelocations(Data.VA + 3, 1));
    Metadata.ProgramHeaders.back().MemorySize = Page - 3;
    EXPECT_TRUE(Image.readOnlyAfterRelocations(Data.VA + 3, 1));

    // Neither an executable nor static PIE promises to call a dynamic
    // loader. The kernel maps PT_LOAD but does not apply GNU_RELRO itself.
    Image.Entry = 0x8000;
    EXPECT_FALSE(Image.readOnlyAfterRelocations(Data.VA + 3, 1));
    Metadata.Type = llvm::ELF::ET_EXEC;
    EXPECT_FALSE(Image.readOnlyAfterRelocations(Data.VA + 3, 1));
    Metadata.Interpreter = "/lib/ld.so";
    EXPECT_TRUE(Image.readOnlyAfterRelocations(Data.VA + 3, 1));
    Metadata.Interpreter.clear();
    Image.Segments.front().ReadOnlyAfterRelocations = true;
    EXPECT_TRUE(Image.readOnlyAfterRelocations(Data.VA + 3, 1));
  }
}

TEST(ImmutablePE32Code, IndexedAndOrdinaryReadersKeepEveryFixupBoundary) {
  for (unsigned Mutation = 0; Mutation != 29; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = codeImage();
    RelocatedAddressField Field;
    Field.Width = 4;
    Field.TargetVA = TargetVA;
    Field.EncodedValue = TargetVA;
    bool Expected = Mutation <= 5 || Mutation == 10 || Mutation == 17 ||
                    Mutation == 18 || Mutation == 25;
    switch (Mutation) {
    case 1:
      Image.BaseRelocations.insert(Image.BaseRelocations.begin(),
                                   {ReadVA + 0x40, 3});
      Image.BaseRelocations.push_back({ReadVA - 8, 3});
      break;
    case 2:
      Image.BaseRelocations.push_back({ReadVA, 0});
      break;
    case 3:
      Image.CodeAddressRelocOperands.emplace(OperandVA, Field);
      break;
    case 4:
      Image.DataAddressRelocOperands.emplace(OperandVA, Field);
      break;
    case 5:
      Image.CodeAddressRelocOperands.emplace(OperandVA, Field);
      Image.DataAddressRelocOperands.emplace(OperandVA, Field);
      break;
    case 6:
      Image.BaseRelocations.clear();
      Image.CodeAddressRelocOperands.emplace(OperandVA, Field);
      break;
    case 7:
      Image.BaseRelocations.push_back(Image.BaseRelocations.front());
      break;
    case 8:
      Image.BaseRelocations.front().Type = 10;
      break;
    case 9:
      Image.BaseRelocations.push_back({ReadVA - 3, 3});
      break;
    case 10:
      Image.BaseRelocations.push_back({ReadVA - 4, 3});
      break;
    case 11:
      ++Field.TargetVA;
      Image.CodeAddressRelocOperands.emplace(OperandVA, Field);
      break;
    case 12:
      Field.Width = 8;
      Image.CodeAddressRelocOperands.emplace(OperandVA, Field);
      break;
    case 13:
      Field.PCRelativeFromInstructionEnd = true;
      Image.CodeAddressRelocOperands.emplace(OperandVA, Field);
      break;
    case 14:
      Field.Kind = RelocatedAddressFieldKind::I386ELFGOTOFF;
      Image.CodeAddressRelocOperands.emplace(OperandVA, Field);
      break;
    case 15:
      Image.CodeAddressRelocOperands.emplace(OperandVA + 1, Field);
      break;
    case 16:
    case 17:
      Field.Width = 255;
      Image.CodeAddressRelocOperands.emplace(
          ReadVA - (Mutation == 16 ? 254 : 255), Field);
      break;
    case 18:
      Field.Width = 0;
      Image.CodeAddressRelocOperands.emplace(ReadVA, Field);
      break;
    case 19:
      Image.CodePtrRelocSlots.insert(ReadVA - 3);
      break;
    case 20:
      Image.DataPtrRelocSlots.insert(ReadVA - 3);
      break;
    case 21:
      Image.RelCodeRelocSlots.insert(ReadVA - 3);
      break;
    case 22:
      Image.RelDataPtrRelocSlots.insert(ReadVA - 3);
      break;
    case 23:
      Image.RuntimeCallablePointerSlots.push_back({ReadVA - 3});
      break;
    case 24:
    case 25: {
      RelocationEntry Entry;
      Entry.Address = ReadVA - (Mutation == 24 ? 7 : 8);
      Image.Relocations.push_back(Entry);
      break;
    }
    case 26:
      Image.Segments.front().Flags =
          Image.Segments.front().Flags | SegmentFlags::Writable;
      break;
    case 27:
      Image.Segments.push_back(Image.Segments.front());
      break;
    case 28:
      Image.Segments.front().FileSz = 0x15;
      break;
    default:
      break;
    }
    const auto Ordinary =
        readImmutablePE32CodeBytes(Image, ReadVA, 6, {OperandVA});
    size_t Work = 0;
    IndexedPE32CodeBytes Index(Image, Work, 10000);
    const auto Indexed = Index.read(ReadVA, 6, {OperandVA});
    EXPECT_EQ(Ordinary.has_value(), Expected);
    EXPECT_EQ(Indexed, Ordinary);
    size_t BoundedWork = 0;
    EXPECT_EQ(readImmutablePE32CodeBytes(Image, ReadVA, 6, {OperandVA},
                                         BoundedWork, 10000),
              Ordinary);
    EXPECT_GT(BoundedWork, 0u);
    size_t RelocatedWork = 0;
    EXPECT_EQ(readRelocatedPE32CodeBytes(Image, ReadVA, 6, {OperandVA},
                                         RelocatedWork, 10000),
              Ordinary);
    EXPECT_GT(Work, 0u);
    const auto Raw = readImmutableCodeBytes(Image, ReadVA, 6);
    {
      ReadOnlyBytesScope Scope(Image);
      EXPECT_EQ(readImmutablePE32CodeBytes(Image, ReadVA, 6, {OperandVA}),
                Ordinary);
      EXPECT_EQ(readImmutableCodeBytes(Image, ReadVA, 6), Raw);
      size_t ScopedWork = 0;
      EXPECT_EQ(readImmutablePE32CodeBytes(Image, ReadVA, 6, {OperandVA},
                                           ScopedWork, 10000),
                Ordinary);
      EXPECT_EQ(ScopedWork, BoundedWork);
      ScopedWork = 0;
      EXPECT_EQ(readRelocatedPE32CodeBytes(Image, ReadVA, 6, {OperandVA},
                                           ScopedWork, 10000),
                Ordinary);
      EXPECT_EQ(ScopedWork, RelocatedWork);
      for (unsigned Attempt = 0; Attempt != 2; ++Attempt) {
        ScopedWork = 0;
        IndexedPE32CodeBytes ScopedIndex(Image, ScopedWork, 10000);
        EXPECT_EQ(ScopedIndex.read(ReadVA, 6, {OperandVA}), Indexed);
        EXPECT_EQ(ScopedWork, Work);
      }
    }
    if (Expected) {
      ASSERT_TRUE(Indexed);
      EXPECT_EQ(*Indexed,
                (std::vector<uint8_t>{0xb8, 0x20, 0x20, 0x40, 0, 0xc3}));
    }
  }
}

TEST(ImmutablePE32Code, ScopedReadsKeepImageLifetimeAndExactBudgets) {
  auto Image = codeImage();
  auto Foreign = codeImage();
  Foreign.BaseRelocations.push_back(Foreign.BaseRelocations.front());
  size_t Required = 0;
  const auto Expected = readImmutablePE32CodeBytes(
      Image, ReadVA, 6, {OperandVA}, Required, 10000);
  ASSERT_TRUE(Expected);
  {
    ReadOnlyBytesScope Scope(Image);
    const auto Check = [&] {
      EXPECT_EQ(readImmutablePE32CodeBytes(Image, ReadVA, 6, {OperandVA}),
                Expected);
      EXPECT_FALSE(readImmutablePE32CodeBytes(Foreign, ReadVA, 6, {OperandVA}));
    };
    Check();
    {
      ReadOnlyBytesScope Nested(Image);
      Check();
    }
    {
      ReadOnlyBytesScope Other(Foreign);
      Check();
    }
    Check();
    for (size_t Limit : {size_t{0}, Required - 1, Required}) {
      size_t Work = 0;
      EXPECT_EQ(bool(readImmutablePE32CodeBytes(Image, ReadVA, 6, {OperandVA},
                                                Work, Limit)),
                Limit == Required);
      EXPECT_EQ(Work, Limit);
      EXPECT_FALSE(readImmutablePE32CodeBytes(Image, ReadVA, 6, {OperandVA},
                                              Work, Limit));
    }
  }
  Image.BaseRelocations.push_back(Image.BaseRelocations.front());
  EXPECT_FALSE(readImmutablePE32CodeBytes(Image, ReadVA, 6, {OperandVA}));
  {
    ReadOnlyBytesScope Fresh(Image);
    EXPECT_FALSE(readImmutablePE32CodeBytes(Image, ReadVA, 6, {OperandVA}));
  }
  // Type-zero base records are padding for the PE operand reader, but raw
  // immutable byte copies still reject their overlapping storage.
  Image.BaseRelocations = {{ReadVA, 0}};
  ReadOnlyBytesScope Padding(Image);
  EXPECT_TRUE(readImmutablePE32CodeBytes(Image, ReadVA, 6, {}));
  EXPECT_FALSE(readImmutableCodeBytes(Image, ReadVA, 6));
}

TEST(ImmutablePE32Code, OversizedScopedIndexesKeepOrdinaryRefusals) {
  for (size_t Count : {limits::loader::kMaxReadOnlyVectorFixups,
                       limits::loader::kMaxReadOnlyVectorFixups + 1}) {
    SCOPED_TRACE(Count);
    auto Image = codeImage();
    Image.BaseRelocations.resize(Count, {ReadVA + 0x40, 3});
    for (bool Conflict : {false, true}) {
      SCOPED_TRACE(Conflict);
      if (Conflict)
        Image.BaseRelocations.back() = Image.BaseRelocations.front();
      size_t Required = 0;
      const auto Expected = readImmutablePE32CodeBytes(
          Image, ReadVA, 6, {OperandVA}, Required, Count + 10000);
      EXPECT_EQ(bool(Expected), !Conflict);
      size_t IndexedWork = 0;
      IndexedPE32CodeBytes OrdinaryIndex(Image, IndexedWork, SIZE_MAX);
      EXPECT_EQ(OrdinaryIndex.read(ReadVA, 6, {OperandVA}), Expected);
      ReadOnlyBytesScope Scope(Image);
      size_t Work = 0;
      EXPECT_EQ(readImmutablePE32CodeBytes(Image, ReadVA, 6, {OperandVA}, Work,
                                           Count + 10000),
                Expected);
      EXPECT_EQ(Work, Required);
      for (unsigned Attempt = 0; Attempt != 2; ++Attempt) {
        Work = 0;
        IndexedPE32CodeBytes ScopedIndex(Image, Work, SIZE_MAX);
        EXPECT_EQ(ScopedIndex.read(ReadVA, 6, {OperandVA}), Expected);
        EXPECT_EQ(Work, IndexedWork);
      }
    }
  }
}

TEST(ImmutableScalar, ScalarBitsDoNotGrantPointerIdentity) {
  auto Image = codeImage();
  Image.BaseRelocations.clear();
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  for (uint32_t Size : {1, 2, 4, 8}) {
    writeLE<uint64_t>(Image.Segments.front().Data.data() + 0x40,
                      UINT64_C(0xfedcba9876543210));
    EXPECT_EQ(readImmutableImageScalar(Image, TextVA + 0x40, Size),
              UINT64_C(0xfedcba9876543210) & (UINT64_MAX >> ((8 - Size) * 8)));
  }
  writeLE<uint64_t>(Image.Segments.front().Data.data() + 0x40, TextVA);
  EXPECT_FALSE(readImmutableImageScalar(Image, TextVA + 0x40, 8));
  EXPECT_EQ(readImmutableImageScalar(Image, TextVA + 0x40, 4), TextVA);
  EXPECT_FALSE(readImmutableImageScalar(Image, TextVA + 0x40, 0));
  EXPECT_FALSE(readImmutableImageScalar(Image, TextVA + 0x40, 9));
  Image.BaseRelocations.push_back({TextVA + 0x42, 3});
  EXPECT_FALSE(readImmutableImageScalar(Image, TextVA + 0x40, 4));
}

TEST(ImmutableScalar, BudgetChargesTheInventoryBeforeARead) {
  auto Image = codeImage();
  Image.BaseRelocations.clear();
  size_t Remaining = 10000;
  ASSERT_EQ(readImmutableImageScalar(Image, ReadVA, 1, &Remaining), 0xb8u);
  const size_t Required = 10000 - Remaining;
  ASSERT_GT(Required, 0u);
  for (size_t Budget : {size_t{0}, Required - 1, Required}) {
    Remaining = Budget;
    EXPECT_EQ(
        readImmutableImageScalar(Image, ReadVA, 1, &Remaining).has_value(),
        Budget == Required);
    EXPECT_EQ(Remaining, Budget == Required ? 0 : Budget);
  }
  // An unrelated fixup still contributes to the cold-query work. The failed
  // reservation must leave room for the caller's independent arithmetic.
  Image.BaseRelocations.push_back({ReadVA + 0x40, 3});
  Remaining = Required;
  EXPECT_FALSE(readImmutableImageScalar(Image, ReadVA, 1, &Remaining));
  EXPECT_EQ(Remaining, Required);
  ++Remaining;
  EXPECT_EQ(readImmutableImageScalar(Image, ReadVA, 1, &Remaining), 0xb8u);
  EXPECT_EQ(Remaining, 0u);
}

TEST(ImmutableScalar, WideOperandFixupsCannotSupplyStableBytes) {
  for (bool CodeOperand : {false, true})
    for (uint8_t Width : {8, 16, UINT8_MAX}) {
      auto Image = codeImage();
      Image.BaseRelocations.clear();
      RelocatedAddressField Field;
      Field.Width = Width;
      auto &Operands = CodeOperand ? Image.CodeAddressRelocOperands
                                   : Image.DataAddressRelocOperands;
      // The field starts more than one pointer width before the read but its
      // final byte still changes that byte when relocated. Malformed wide
      // metadata is not permission to freeze the file's initial contents.
      Operands.emplace(ReadVA - (Width - 1), Field);
      EXPECT_FALSE(readImmutableCodeBytes(Image, ReadVA, 1));
      EXPECT_FALSE(readImmutableImageScalar(Image, ReadVA, 1));
      Operands.clear();
      Operands.emplace(ReadVA - Width, Field);
      EXPECT_EQ(readImmutableImageScalar(Image, ReadVA, 1), 0xb8u);
    }
}

TEST(ImmutablePE32Code, IndexedQueriesChargeColdAndRepeatedWork) {
  const auto Image = codeImage();
  size_t Required = 0;
  {
    IndexedPE32CodeBytes Index(Image, Required, 10000);
    ASSERT_TRUE(Index.read(ReadVA, 6, {OperandVA}));
  }
  for (size_t Limit : {size_t{0}, Required - 1, Required}) {
    size_t Work = 0;
    IndexedPE32CodeBytes Index(Image, Work, Limit);
    EXPECT_EQ(Index.read(ReadVA, 6, {OperandVA}).has_value(),
              Limit == Required);
    EXPECT_EQ(Work, Limit);
    EXPECT_FALSE(Index.read(ReadVA, 6, {OperandVA}));
  }
  size_t Work = 0;
  IndexedPE32CodeBytes Index(Image, Work, 10000);
  ASSERT_TRUE(Index.read(ReadVA, 6, {OperandVA}));
  const auto Before = Work;
  EXPECT_TRUE(Index.read(ReadVA, 6, {OperandVA}));
  EXPECT_GT(Work, Before);
  EXPECT_FALSE(Index.read(ReadVA, 6, {OperandVA, OperandVA}));
  EXPECT_FALSE(Index.read(ReadVA, 6, {ReadVA - 1}));
  EXPECT_FALSE(Index.read(ReadVA, 0, {}));
  EXPECT_FALSE(Index.read(UINT64_MAX, 6, {}));
}

TEST(ImmutablePE32Code, ScopedInstructionIndexesRetainConstructionCharges) {
  const auto Image = codeImage();
  size_t Required = 0;
  {
    IndexedPE32CodeBytes Index(Image, Required, 10000);
    ASSERT_TRUE(Index.read(ReadVA, 6, {OperandVA}));
  }
  ReadOnlyBytesScope Scope(Image);
  for (size_t Limit :
       {size_t{0}, size_t{1}, Required - 1, Required, Required}) {
    size_t Work = 0;
    IndexedPE32CodeBytes Index(Image, Work, Limit);
    EXPECT_EQ(bool(Index.read(ReadVA, 6, {OperandVA})), Limit == Required);
    EXPECT_EQ(Work, Limit);
    EXPECT_FALSE(Index.read(ReadVA, 6, {OperandVA}));
  }
  auto Foreign = codeImage();
  Foreign.BaseRelocations.push_back(Foreign.BaseRelocations.front());
  {
    ReadOnlyBytesScope Other(Foreign);
    size_t Work = 0;
    IndexedPE32CodeBytes Index(Foreign, Work, 10000);
    EXPECT_FALSE(Index.read(ReadVA, 6, {OperandVA}));
  }
  size_t Work = 0;
  IndexedPE32CodeBytes Restored(Image, Work, Required);
  EXPECT_TRUE(Restored.read(ReadVA, 6, {OperandVA}));
  EXPECT_EQ(Work, Required);
}

TEST(ImmutablePE32Code, InteriorPointerStorageDoesNotGrantAnOrdinaryEntry) {
  for (const auto [Base, Executable] :
       {std::pair{0x400000u, false}, std::pair{0x400000u, true},
        std::pair{0x18000000u, false}, std::pair{0x18000000u, true}})
    for (unsigned Mutation = 0; Mutation != 32; ++Mutation) {
      SCOPED_TRACE(::testing::Message()
                   << Base << ':' << Executable << ':' << Mutation);
      auto Image = codeImage();
      Image.Base = Base;
      Image.Segments.front().VA = Image.Sections.front().VA = Base + 0x1000;
      Image.BaseRelocations.clear();
      va_t Slot = Base + 0x2008;
      const va_t Target = Base + 0x1010;
      Segment Data;
      Data.VA = Base + 0x2000;
      Data.Flags = Executable
                       ? SegmentFlags::Readable | SegmentFlags::Executable
                       : SegmentFlags::Readable;
      Data.Size = Data.FileSz = 0x100;
      Data.Data.resize(0x100);
      writeLE<uint32_t>(Data.Data.data() + 8, Target);
      Section DataSection;
      DataSection.VA = Data.VA;
      DataSection.Flags = Data.Flags;
      DataSection.Size = DataSection.FileSz = Data.Size;
      Image.Segments.push_back(std::move(Data));
      Image.Sections.push_back(std::move(DataSection));
      Image.CodePtrRelocSlots.insert(Slot);
      Image.BaseRelocations.push_back({Slot, 3});
      switch (Mutation) {
      case 1:
        Image.Symbols.push_back(Symbol::makeFunc(Target));
        break;
      case 2:
        Image.BaseRelocations.clear();
        break;
      case 3:
        Image.BaseRelocations.push_back({Slot, 3});
        break;
      case 4:
        Image.BaseRelocations.front().Type = 10;
        break;
      case 5:
        Image.BaseRelocations.push_back({Slot - 3, 3});
        break;
      case 6:
        Image.CodePtrRelocSlots.clear();
        break;
      case 7:
        Image.DataPtrRelocSlots.insert(Slot);
        break;
      case 8:
        Image.DataPtrRelocTargetOwners.emplace(Slot, Base + 0x1000);
        break;
      case 9:
        Image.RuntimeCallablePointerSlots.push_back({Slot});
        break;
      case 10:
        Image.ConflictingImportStorageSlots.insert(Slot);
        break;
      case 11:
        Image.Segments.back().Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 12:
        Image.Sections.back().Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 13:
        Image.Segments.push_back(Image.Segments.back());
        break;
      case 14:
        Image.Sections.push_back(Image.Sections.back());
        break;
      case 15:
        Image.Segments.back().FileSz = 11;
        break;
      case 16:
        Image.Sections.back().FileSz = 11;
        break;
      case 17:
        Image.Segments.front().Flags =
            Image.Segments.front().Flags | SegmentFlags::Writable;
        break;
      case 18:
        Image.Sections.front().Flags =
            Image.Sections.front().Flags | SegmentFlags::Writable;
        break;
      case 19:
        Image.Sections.front().Flags = SegmentFlags::Readable;
        break;
      case 20:
        writeLE<uint32_t>(Image.Segments.back().Data.data() + 8, Base + 0x3000);
        break;
      case 21:
        Image.Bits = Bitness::Bits64;
        break;
      case 22:
        Image.Format = BinaryFormat::ELF;
        break;
      case 23:
        Image.IsRelocatable = true;
        break;
      case 24:
        Image.Arch = Arch::ARM;
        break;
      case 25:
        Image.CodePtrRelocSlots.insert(Slot + 1);
        break;
      case 26:
        Image.BaseRelocations.push_back({Target, 3});
        break;
      case 27:
        Image.BaseRelocations.push_back({Slot - 4, 3});
        break;
      case 28:
        Image.BaseRelocations.push_back({Slot - 7, 10});
        break;
      case 29:
        Image.Segments.front().FileSz = 0x10;
        break;
      case 30:
        Image.Base = uint64_t(UINT32_MAX) + 1;
        break;
      case 31:
        Slot += uint64_t(UINT32_MAX) + 1;
        Image.Segments.back().VA += uint64_t(UINT32_MAX) + 1;
        Image.Sections.back().VA = Image.Segments.back().VA;
        Image.CodePtrRelocSlots = {Slot};
        Image.BaseRelocations = {{Slot, 3}};
        break;
      default:
        break;
      }
      const auto Storage = readImmutablePE32CodePointerStorage(Image, Slot);
      size_t BudgetWork = 0;
      EXPECT_EQ(
          readImmutablePE32CodePointerStorage(Image, Slot, BudgetWork, 100000),
          Storage);
      EXPECT_EQ(Storage.has_value(),
                !Executable && (Mutation < 2 || Mutation == 27));
      {
        ReadOnlyBytesScope Scope(Image);
        size_t Work = 0;
        EXPECT_EQ(
            readImmutablePE32CodePointerStorage(Image, Slot, Work, 100000),
            Storage);
        EXPECT_EQ(Work, BudgetWork);
        EXPECT_FALSE(readImmutableImageBytes(Image, Slot, 4));
      }
      if (Storage)
        EXPECT_EQ(*Storage, Target);
      EXPECT_EQ(readImmutableImageCodePointer(Image, Slot).has_value(),
                !Executable && Mutation == 1);
      size_t DispatchWork = 0;
      const auto Dispatch =
          readImmutablePE32JumpTableTarget(Image, Slot, DispatchWork, 100000);
      EXPECT_EQ(Dispatch.has_value(), Mutation < 2 || Mutation == 27);
      if (Dispatch) {
        EXPECT_EQ(*Dispatch, Target);
        size_t Exact = 0, Short = 0;
        EXPECT_EQ(
            readImmutablePE32JumpTableTarget(Image, Slot, Exact, DispatchWork),
            Dispatch);
        EXPECT_EQ(Exact, DispatchWork);
        EXPECT_FALSE(readImmutablePE32JumpTableTarget(Image, Slot, Short,
                                                      DispatchWork - 1));
        EXPECT_EQ(Short, DispatchWork - 1);
      }
      EXPECT_FALSE(readImmutableImageBytes(Image, Slot, 4));
    }
}

TEST(ImmutablePE32Code, BudgetedStorageQueriesReplayCurrentInputs) {
  for (bool Pointer : {false, true}) {
    SCOPED_TRACE(Pointer);
    auto Image = codeImage();
    Image.BaseRelocations.clear();
    Segment Data;
    Data.VA = 0x402000;
    Data.Flags = SegmentFlags::Readable;
    Data.Size = Data.FileSz = 0x100;
    Data.Data.resize(0x100);
    writeLE<uint32_t>(Data.Data.data(), 37);
    writeLE<uint32_t>(Data.Data.data() + 8, ReadVA);
    Section Owner;
    Owner.VA = Data.VA;
    Owner.Flags = Data.Flags;
    Owner.Size = Owner.FileSz = Data.Size;
    Image.Segments.push_back(std::move(Data));
    Image.Sections.push_back(std::move(Owner));
    Image.CodePtrRelocSlots.insert(0x402008);
    Image.BaseRelocations.push_back({0x402008, 3});
    const auto Query = [&](size_t &Work, size_t Limit) {
      if (Pointer)
        return readImmutablePE32CodePointerStorage(Image, 0x402008, Work, Limit)
            .has_value();
      return readImmutableImageBytes(Image, 0x402000, 4, Work, Limit)
          .has_value();
    };
    size_t Required = 0;
    ASSERT_TRUE(Query(Required, 10000));
    ASSERT_GT(Required, 0u);
    for (size_t Limit : {size_t{0}, Required - 1, Required}) {
      size_t Work = 0;
      EXPECT_EQ(Query(Work, Limit), Limit == Required);
      EXPECT_EQ(Work, Limit);
      EXPECT_FALSE(Query(Work, Limit));
      EXPECT_EQ(Work, Limit);
    }
    size_t Work = 0;
    ASSERT_TRUE(Query(Work, 10000));
    const size_t First = Work;
    ASSERT_TRUE(Query(Work, 10000));
    EXPECT_EQ(Work, First * 2);
    Image.BaseRelocations.push_back({Pointer ? 0x402008u : 0x402000u, 3});
    EXPECT_FALSE(Query(Work, 10000));
    EXPECT_GT(Work, First * 2);
  }
}

TEST(ImmutablePE32Code, RetainedOperandsRequireCompleteRelocations) {
  auto Image = codeImage();
  auto &Code = Image.Segments.front().Data;
  Code[0x15] = 0xb9;
  writeLE<uint32_t>(Code.data() + 0x16, TargetVA + 4);
  Code[0x1a] = 0xc3;
  const std::vector<va_t> Operands{OperandVA, ReadVA + 6};
  Image.BaseRelocations.clear();
  for (unsigned Count = 0; Count != 3; ++Count) {
    SCOPED_TRACE(Count);
    const auto Ordinary =
        readImmutablePE32CodeBytes(Image, ReadVA, 11, Operands);
    ASSERT_TRUE(Ordinary);
    size_t OrdinaryWork = 0;
    EXPECT_EQ(readImmutablePE32CodeBytes(Image, ReadVA, 11, Operands,
                                         OrdinaryWork, 10000),
              Ordinary);
    ASSERT_GT(OrdinaryWork, 0u);
    for (size_t Limit : {size_t{0}, OrdinaryWork - 1, OrdinaryWork}) {
      size_t Used = 0;
      EXPECT_EQ(bool(readImmutablePE32CodeBytes(Image, ReadVA, 11, Operands,
                                                Used, Limit)),
                Limit == OrdinaryWork);
      EXPECT_EQ(Used, Limit);
    }
    size_t Work = 0;
    const auto Retained =
        readRelocatedPE32CodeBytes(Image, ReadVA, 11, Operands, Work, 10000);
    EXPECT_EQ(bool(Retained), Count == 2);
    if (Retained)
      EXPECT_EQ(Retained, Ordinary);
    if (Count < 2)
      Image.BaseRelocations.push_back({Operands[Count], 3});
  }
  for (uint32_t I = 0; I != 250000; ++I) {
    RelocatedAddressField Field;
    Field.Width = 4;
    Field.TargetVA = Field.EncodedValue = 0x70000000;
    Image.DataAddressRelocOperands.emplace(0x60000000 + I * 4, Field);
  }
  const auto Query = [&](size_t &Work, size_t Limit) {
    return readRelocatedPE32CodeBytes(Image, ReadVA, 11, Operands, Work, Limit);
  };
  size_t Required = 0;
  ASSERT_TRUE(Query(Required, 10000));
  for (size_t Limit : {size_t{0}, Required - 1, Required}) {
    size_t Work = 0;
    EXPECT_EQ(bool(Query(Work, Limit)), Limit == Required);
    EXPECT_EQ(Work, Limit);
  }
  size_t Work = 0;
  ASSERT_TRUE(Query(Work, 10000));
  ASSERT_TRUE(Query(Work, 10000));
  EXPECT_EQ(Work, Required * 2);
  RelocatedAddressField Wide;
  Wide.Width = UINT8_MAX;
  Image.DataAddressRelocOperands.emplace(ReadVA - UINT8_MAX + 1, Wide);
  EXPECT_FALSE(Query(Work, 10000));
  EXPECT_GT(Work, Required * 2);
}
} // namespace
} // namespace neverd
