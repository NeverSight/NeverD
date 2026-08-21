//===- MachOI386RelocationScatteredTests.cpp - Mach-O i386 scattered and malformed relocation tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "MachOI386RelocationTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::macho_loader::detail;
using namespace neverd::macho_i386_test;

TEST_F(MachOI386Relocation,
       ScatteredVanillaNormalizesNonzeroOriginalSectionBase) {
  auto Bytes = readBinaryFile(fixture("test_macho_i386_nopic.o"));
  auto Text = rawSectionLayout(Bytes, section_names::macho::Text);
  auto Data = rawSectionLayout(Bytes, section_names::macho::Data);
  ASSERT_TRUE(Text.has_value());
  ASSERT_TRUE(Data.has_value());
  auto RelocOffset = findRawRelocation(Bytes, *Text, 0x25);
  ASSERT_TRUE(RelocOffset.has_value());

  constexpr uint32_t Addend = 3;
  writeRawRelocation(Bytes, *RelocOffset,
                     scatteredRelocationWord(
                         0x25, false, 2, llvm::MachO::GENERIC_RELOC_VANILLA),
                     Data->Address);
  writeSectionField(Bytes, *Text, 0x25, Data->Address + Addend);

  auto ImgOrErr = loadBinary(writeMutation("scattered_vanilla.o", Bytes));
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  const Section *LoadedText = Img.getSectionByName(section_names::macho::Text);
  const Section *LoadedData = Img.getSectionByName(section_names::macho::Data);
  ASSERT_NE(LoadedText, nullptr);
  ASSERT_NE(LoadedData, nullptr);
  uint64_t Expected = LoadedData->VA + Addend;
  auto Actual = readLoadedField(Img, LoadedText->VA + 0x25, 4, false);
  ASSERT_TRUE(Actual.has_value());
  EXPECT_EQ(*Actual, Expected);
  EXPECT_NE(Img.WritableRelocDataAddrs.count(Expected), 0u);
}

TEST_F(MachOI386Relocation,
       OrphanPairAndUnsupportedTypesDoNotWriteAndContinue) {
  for (uint32_t Type : {uint32_t(llvm::MachO::GENERIC_RELOC_PAIR),
                        uint32_t(llvm::MachO::GENERIC_RELOC_PB_LA_PTR),
                        uint32_t(llvm::MachO::GENERIC_RELOC_TLV)}) {
    SCOPED_TRACE(Type);
    auto Bytes = readBinaryFile(fixture("test_macho_i386_nopic.o"));
    auto Text = rawSectionLayout(Bytes, section_names::macho::Text);
    ASSERT_TRUE(Text.has_value());
    auto RelocOffset = findRawRelocation(Bytes, *Text, 0x25);
    ASSERT_TRUE(RelocOffset.has_value());
    constexpr uint32_t Sentinel = 0x2468ace0;
    writeRawRelocation(Bytes, *RelocOffset, 0x25,
                       plainRelocationWord(2, false, 2, false, Type));
    writeSectionField(Bytes, *Text, 0x25, Sentinel);

    auto Path =
        writeMutation("unsupported_" + std::to_string(Type) + ".o", Bytes);
    auto ImgOrErr = loadBinary(Path);
    ASSERT_TRUE(static_cast<bool>(ImgOrErr))
        << llvm::toString(ImgOrErr.takeError());
    const BinaryImage &Img = *ImgOrErr;
    const Section *LoadedText = Img.getSectionByName(section_names::macho::Text);
    const Symbol *Local = findSymbol(Img, "_local_bias");
    ASSERT_NE(LoadedText, nullptr);
    ASSERT_NE(Local, nullptr);
    auto Actual = readLoadedField(Img, LoadedText->VA + 0x25, 4, false);
    ASSERT_TRUE(Actual.has_value());
    EXPECT_EQ(*Actual, Sentinel);
    EXPECT_NE(Img.WritableRelocDataAddrs.count(Local->Addr), 0u);
  }
}

TEST_F(MachOI386Relocation,
       InvalidLengthAndTruncatedFieldDoNotWriteAndContinue) {
  {
    auto Bytes = readBinaryFile(fixture("test_macho_i386_nopic.o"));
    auto Text = rawSectionLayout(Bytes, section_names::macho::Text);
    ASSERT_TRUE(Text.has_value());
    auto RelocOffset = findRawRelocation(Bytes, *Text, 0x25);
    ASSERT_TRUE(RelocOffset.has_value());
    constexpr uint32_t Sentinel = 0x10293847;
    writeRawRelocation(Bytes, *RelocOffset, 0x25,
                       plainRelocationWord(2, false, 3, false,
                                           llvm::MachO::GENERIC_RELOC_VANILLA));
    writeSectionField(Bytes, *Text, 0x25, Sentinel);

    auto ImgOrErr = loadBinary(writeMutation("invalid_length.o", Bytes));
    ASSERT_TRUE(static_cast<bool>(ImgOrErr))
        << llvm::toString(ImgOrErr.takeError());
    const BinaryImage &Img = *ImgOrErr;
    const Section *LoadedText = Img.getSectionByName(section_names::macho::Text);
    const Symbol *Local = findSymbol(Img, "_local_bias");
    ASSERT_NE(LoadedText, nullptr);
    ASSERT_NE(Local, nullptr);
    auto Actual = readLoadedField(Img, LoadedText->VA + 0x25, 4, false);
    ASSERT_TRUE(Actual.has_value());
    EXPECT_EQ(*Actual, Sentinel);
    EXPECT_NE(Img.WritableRelocDataAddrs.count(Local->Addr), 0u);
  }

  {
    auto Bytes = readBinaryFile(fixture("test_macho_i386_nopic.o"));
    auto Text = rawSectionLayout(Bytes, section_names::macho::Text);
    ASSERT_TRUE(Text.has_value());
    auto RelocOffset = findRawRelocation(Bytes, *Text, 0x25);
    ASSERT_TRUE(RelocOffset.has_value());
    uint32_t TruncatedOffset = Text->Size - 2;
    size_t FieldOffset = Text->FileOffset + TruncatedOffset;
    Bytes[FieldOffset] = 0xa5;
    Bytes[FieldOffset + 1] = 0x5a;
    std::array<uint8_t, 2> Before{Bytes[FieldOffset], Bytes[FieldOffset + 1]};
    writeRawRelocation(Bytes, *RelocOffset, TruncatedOffset,
                       plainRelocationWord(2, false, 2, false,
                                           llvm::MachO::GENERIC_RELOC_VANILLA));

    auto ImgOrErr = loadBinary(writeMutation("truncated_field.o", Bytes));
    ASSERT_TRUE(static_cast<bool>(ImgOrErr))
        << llvm::toString(ImgOrErr.takeError());
    const BinaryImage &Img = *ImgOrErr;
    const Section *LoadedText = Img.getSectionByName(section_names::macho::Text);
    const Symbol *Local = findSymbol(Img, "_local_bias");
    ASSERT_NE(LoadedText, nullptr);
    ASSERT_NE(Local, nullptr);
    const uint8_t *After = Img.readVA(LoadedText->VA + TruncatedOffset, 2);
    ASSERT_NE(After, nullptr);
    EXPECT_TRUE(std::equal(Before.begin(), Before.end(), After));
    EXPECT_NE(Img.WritableRelocDataAddrs.count(Local->Addr), 0u);
  }
}

TEST_F(MachOI386Relocation,
       MalformedSectionDifferencePairDoesNotWriteAndLaterSectionsContinue) {
  enum class Mutation { NonzeroAddress, LengthMismatch, PCRelMismatch };
  for (Mutation M : {Mutation::NonzeroAddress, Mutation::LengthMismatch,
                     Mutation::PCRelMismatch}) {
    SCOPED_TRACE(static_cast<unsigned>(M));
    auto Bytes = readBinaryFile(fixture("test_macho_i386.o"));
    auto Text = rawSectionLayout(Bytes, section_names::macho::Text);
    ASSERT_TRUE(Text.has_value());
    auto DifferenceOffset = findRawRelocation(Bytes, *Text, 0x2b);
    ASSERT_TRUE(DifferenceOffset.has_value());
    ASSERT_TRUE(rangeInBounds(*DifferenceOffset + 8, 8, Bytes.size()));

    uint32_t DifferenceWord0 =
        readLE<uint32_t>(Bytes.data() + *DifferenceOffset);
    size_t PairOffset = *DifferenceOffset + 8;
    uint32_t PairValue = readLE<uint32_t>(Bytes.data() + PairOffset + 4);
    uint32_t Address = 0;
    bool IsPCRel = ((DifferenceWord0 >> 30) & 1u) != 0;
    uint32_t Length = (DifferenceWord0 >> 28) & 3u;
    switch (M) {
    case Mutation::NonzeroAddress:
      Address = 1;
      break;
    case Mutation::LengthMismatch:
      Length = Length == 2 ? 1 : 2;
      break;
    case Mutation::PCRelMismatch:
      IsPCRel = !IsPCRel;
      break;
    }
    writeRawRelocation(Bytes, PairOffset,
                       scatteredRelocationWord(Address, IsPCRel, Length,
                                               llvm::MachO::GENERIC_RELOC_PAIR),
                       PairValue);
    constexpr uint32_t Sentinel = 0x10203040;
    writeSectionField(Bytes, *Text, 0x2b, Sentinel);
    auto Obj = createMachOObject(Bytes);
    ASSERT_NE(Obj, nullptr);

    fs::path Path = writeMutation(
        "malformed_pair_" + std::to_string(static_cast<unsigned>(M)) + ".o",
        Bytes);
    auto ImgOrErr = loadBinary(Path);
    ASSERT_TRUE(static_cast<bool>(ImgOrErr))
        << llvm::toString(ImgOrErr.takeError());
    auto LoadedText =
        std::find_if(ImgOrErr->Sections.begin(), ImgOrErr->Sections.end(),
                     [](const Section &Sec) { return Sec.Name == section_names::macho::Text; });
    auto LoadedData =
        std::find_if(ImgOrErr->Sections.begin(), ImgOrErr->Sections.end(),
                     [](const Section &Sec) { return Sec.Name == section_names::macho::Data; });
    ASSERT_NE(LoadedText, ImgOrErr->Sections.end());
    ASSERT_NE(LoadedData, ImgOrErr->Sections.end());
    auto DataSegment = std::find_if(
        ImgOrErr->Segments.begin(), ImgOrErr->Segments.end(),
        [&](const Segment &Seg) {
          return Seg.VA == LoadedData->VA && Seg.Size == LoadedData->Size;
        });
    ASSERT_NE(DataSegment, ImgOrErr->Segments.end());
    constexpr va_t ShiftedDataVA = 0x1000;
    LoadedData->VA = ShiftedDataVA;
    DataSegment->VA = ShiftedDataVA;
    ImgOrErr->CodePtrRelocSlots.clear();

    if (llvm::Error Err = macho_loader::applyObjectRelocations(*Obj, *ImgOrErr))
      ADD_FAILURE() << llvm::toString(std::move(Err));
    auto Actual = readLoadedField(*ImgOrErr, LoadedText->VA + 0x2b, 4, false);
    ASSERT_TRUE(Actual.has_value());
    EXPECT_EQ(*Actual, Sentinel);
    EXPECT_NE(ImgOrErr->CodePtrRelocSlots.count(ShiftedDataVA + 8), 0u);
  }
}

TEST_F(MachOI386Relocation,
       MissingSectionDifferencePairDoesNotWriteUnrelatedSectionsContinue) {
  auto Bytes = readBinaryFile(fixture("test_macho_i386.o"));
  auto Text = rawSectionLayout(Bytes, section_names::macho::Text);
  ASSERT_TRUE(Text.has_value());
  auto DifferenceOffset = findRawRelocation(Bytes, *Text, 0x2b);
  ASSERT_TRUE(DifferenceOffset.has_value());
  ASSERT_TRUE(rangeInBounds(*DifferenceOffset + 8, 8, Bytes.size()));

  size_t PairOffset = *DifferenceOffset + 8;
  uint32_t PairWord0 = readLE<uint32_t>(Bytes.data() + PairOffset);
  uint32_t PairWord1 = readLE<uint32_t>(Bytes.data() + PairOffset + 4);
  ASSERT_NE(PairWord0 & llvm::MachO::R_SCATTERED, 0u);
  writeRawRelocation(Bytes, PairOffset,
                     scatteredRelocationWord(
                         PairWord0 & 0x00ffffffu, false, (PairWord0 >> 28) & 3u,
                         llvm::MachO::GENERIC_RELOC_PB_LA_PTR),
                     PairWord1);
  constexpr uint32_t Sentinel = 0x31415926;
  writeSectionField(Bytes, *Text, 0x2b, Sentinel);

  auto ImgOrErr = loadBinary(writeMutation("missing_pair.o", Bytes));
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  const Section *LoadedText = Img.getSectionByName(section_names::macho::Text);
  const Section *LoadedData = Img.getSectionByName(section_names::macho::Data);
  ASSERT_NE(LoadedText, nullptr);
  ASSERT_NE(LoadedData, nullptr);
  auto Actual = readLoadedField(Img, LoadedText->VA + 0x2b, 4, false);
  ASSERT_TRUE(Actual.has_value());
  EXPECT_EQ(*Actual, Sentinel);
  EXPECT_NE(Img.CodePtrRelocSlots.count(LoadedData->VA + 8), 0u);
}

} // namespace
