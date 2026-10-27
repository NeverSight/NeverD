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
#include "neverd/loader/ReadOnlyBytes.h"
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
    size_t RelocatedWork = 0;
    EXPECT_EQ(readRelocatedPE32CodeBytes(Image, ReadVA, 6, {OperandVA},
                                         RelocatedWork, 10000),
              Ordinary);
    EXPECT_GT(Work, 0u);
    if (Expected) {
      ASSERT_TRUE(Indexed);
      EXPECT_EQ(*Indexed,
                (std::vector<uint8_t>{0xb8, 0x20, 0x20, 0x40, 0, 0xc3}));
    }
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

TEST(ImmutablePE32Code, InteriorPointerStorageDoesNotGrantAnOrdinaryEntry) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (unsigned Mutation = 0; Mutation != 32; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Base << " " << Mutation);
      auto Image = codeImage();
      Image.Base = Base;
      Image.Segments.front().VA = Image.Sections.front().VA = Base + 0x1000;
      Image.BaseRelocations.clear();
      va_t Slot = Base + 0x2008;
      const va_t Target = Base + 0x1010;
      Segment Data;
      Data.VA = Base + 0x2000;
      Data.Flags = SegmentFlags::Readable;
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
      EXPECT_EQ(Storage.has_value(), Mutation < 2 || Mutation == 27);
      if (Storage)
        EXPECT_EQ(*Storage, Target);
      EXPECT_EQ(readImmutableImageCodePointer(Image, Slot).has_value(),
                Mutation == 1);
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
