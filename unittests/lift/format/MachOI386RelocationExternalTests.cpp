//===- MachOI386RelocationExternalTests.cpp - Mach-O i386 external symbol relocation tests -===//
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

TEST_F(MachOI386Relocation, DefinedExternalPCRelRestoresOldPlaceExactly) {
  auto Bytes = readBinaryFile(fixture("test_macho_i386_nopic.o"));
  auto Text = rawSectionLayout(Bytes, section_names::macho::Text);
  auto GlobalIndex = findSymbolIndex(Bytes, "_global_value");
  ASSERT_TRUE(Text.has_value());
  ASSERT_TRUE(GlobalIndex.has_value());
  auto RelocOffset = findRawRelocation(Bytes, *Text, 0x25);
  ASSERT_TRUE(RelocOffset.has_value());

  constexpr int64_t LogicalAddend = 7;
  int64_t OriginalNextPC = int64_t(Text->Address) + 0x25 + 4;
  int32_t EncodedAddend = int32_t(LogicalAddend - OriginalNextPC);
  writeRawRelocation(Bytes, *RelocOffset, 0x25,
                     plainRelocationWord(*GlobalIndex, true, 2, true,
                                         llvm::MachO::GENERIC_RELOC_VANILLA));
  writeSectionField(Bytes, *Text, 0x25, uint32_t(EncodedAddend));

  auto ImgOrErr = loadBinary(writeMutation("external_pcrel.o", Bytes));
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  const Section *LoadedText = Img.getSectionByName(section_names::macho::Text);
  const Symbol *Global = findSymbol(Img, "_global_value");
  const Symbol *Local = findSymbol(Img, "_local_bias");
  ASSERT_NE(LoadedText, nullptr);
  ASSERT_NE(Global, nullptr);
  ASSERT_NE(Local, nullptr);

  va_t Place = LoadedText->VA + 0x25;
  auto Expected = evaluateI386Vanilla(
      {int64_t(Global->Addr), LogicalAddend, Place, 4, true});
  ASSERT_TRUE(Expected.has_value());
  auto Actual = readLoadedField(Img, Place, 4, true);
  ASSERT_TRUE(Actual.has_value());
  EXPECT_EQ(*Actual, *Expected);
  auto SectionValue = readRelocationField(LoadedText->Data, 0x25, 4, true);
  ASSERT_TRUE(SectionValue.has_value());
  EXPECT_EQ(*SectionValue, *Actual);
  EXPECT_NE(Img.WritableRelocDataAddrs.count(Local->Addr), 0u);
}

TEST_F(MachOI386Relocation,
       ExternalSymbolIndexEqualToSymtabCountIsRejectedAndContinues) {
  auto Bytes = readBinaryFile(fixture("test_macho_i386_nopic.o"));
  auto Obj = createMachOObject(Bytes);
  auto Text = rawSectionLayout(Bytes, section_names::macho::Text);
  ASSERT_NE(Obj, nullptr);
  ASSERT_TRUE(Text.has_value());
  auto RelocOffset = findRawRelocation(Bytes, *Text, 0x25);
  ASSERT_TRUE(RelocOffset.has_value());

  llvm::MachO::symtab_command Symtab = Obj->getSymtabLoadCommand();
  constexpr uint32_t Sentinel = 0x10293847;
  writeRawRelocation(Bytes, *RelocOffset, 0x25,
                     plainRelocationWord(Symtab.nsyms, false, 2, true,
                                         llvm::MachO::GENERIC_RELOC_VANILLA));
  writeSectionField(Bytes, *Text, 0x25, Sentinel);
  fs::path Path = writeMutation("external_symbol_at_end.o", Bytes);

  ASSERT_EXIT(
      {
        auto ImgOrErr = loadBinary(Path);
        if (!ImgOrErr) {
          llvm::consumeError(ImgOrErr.takeError());
          std::_Exit(1);
        }
        const Section *LoadedText = ImgOrErr->getSectionByName(section_names::macho::Text);
        const Symbol *Local = findSymbol(*ImgOrErr, "_local_bias");
        if (!LoadedText || !Local)
          std::_Exit(2);
        auto Actual =
            readLoadedField(*ImgOrErr, LoadedText->VA + 0x25, 4, false);
        if (!Actual || *Actual != Sentinel)
          std::_Exit(3);
        if (ImgOrErr->WritableRelocDataAddrs.count(Local->Addr) == 0)
          std::_Exit(4);
        std::_Exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST_F(MachOI386Relocation,
       ExternalSymbolIndexMustBeWithinSymtabAndLaterRelocationContinues) {
  auto Bytes = readBinaryFile(fixture("test_macho_i386_nopic.o"));
  auto Obj = createMachOObject(Bytes);
  auto Text = rawSectionLayout(Bytes, section_names::macho::Text);
  auto Data = rawSectionLayout(Bytes, section_names::macho::Data);
  ASSERT_NE(Obj, nullptr);
  ASSERT_TRUE(Text.has_value());
  ASSERT_TRUE(Data.has_value());
  auto RelocOffset = findRawRelocation(Bytes, *Text, 0x25);
  ASSERT_TRUE(RelocOffset.has_value());

  llvm::MachO::symtab_command Symtab = Obj->getSymtabLoadCommand();
  auto DataSection = findOriginalSection(*Obj, Data->Address);
  ASSERT_TRUE(DataSection.has_value());
  constexpr size_t NListSize = sizeof(llvm::MachO::nlist);
  ASSERT_GE(Bytes.size(), Symtab.symoff);
  uint64_t TailDelta = Bytes.size() - Symtab.symoff;
  uint32_t FakeIndex = uint32_t((TailDelta + NListSize - 1) / NListSize);
  if (FakeIndex <= Symtab.nsyms)
    FakeIndex = Symtab.nsyms + 1;
  ASSERT_LE(FakeIndex, 0x00ffffffu);
  size_t FakeOffset = Symtab.symoff + size_t(FakeIndex) * NListSize;
  Bytes.resize(FakeOffset + NListSize, 0);
  writeLE<uint32_t>(Bytes.data() + FakeOffset, 0);
  Bytes[FakeOffset + 4] =
      uint8_t(llvm::MachO::N_SECT) | uint8_t(llvm::MachO::N_EXT);
  Bytes[FakeOffset + 5] = uint8_t(DataSection->Index + 1);
  writeLE<uint16_t>(Bytes.data() + FakeOffset + 6, 0);
  writeLE<uint32_t>(Bytes.data() + FakeOffset + 8, Data->Address);

  constexpr uint32_t Sentinel = 0x13579bdf;
  writeRawRelocation(Bytes, *RelocOffset, 0x25,
                     plainRelocationWord(FakeIndex, false, 2, true,
                                         llvm::MachO::GENERIC_RELOC_VANILLA));
  writeSectionField(Bytes, *Text, 0x25, Sentinel);
  fs::path Path = writeMutation("external_symbol_oob.o", Bytes);

  ASSERT_EXIT(
      {
        auto ImgOrErr = loadBinary(Path);
        if (!ImgOrErr) {
          llvm::consumeError(ImgOrErr.takeError());
          std::_Exit(1);
        }
        const Section *LoadedText = ImgOrErr->getSectionByName(section_names::macho::Text);
        const Symbol *Local = findSymbol(*ImgOrErr, "_local_bias");
        if (!LoadedText || !Local)
          std::_Exit(2);
        auto Actual =
            readLoadedField(*ImgOrErr, LoadedText->VA + 0x25, 4, false);
        if (!Actual || *Actual != Sentinel)
          std::_Exit(3);
        if (ImgOrErr->WritableRelocDataAddrs.count(Local->Addr) == 0)
          std::_Exit(4);
        std::_Exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST_F(MachOI386Relocation,
       ExternalSectionSymbolPastEndIsRejectedAndLaterRelocationContinues) {
  auto Bytes = readBinaryFile(fixture("test_macho_i386_nopic.o"));
  auto Obj = createMachOObject(Bytes);
  auto Text = rawSectionLayout(Bytes, section_names::macho::Text);
  auto Data = rawSectionLayout(Bytes, section_names::macho::Data);
  auto GlobalIndex = findSymbolIndex(Bytes, "_global_value");
  ASSERT_NE(Obj, nullptr);
  ASSERT_TRUE(Text.has_value());
  ASSERT_TRUE(Data.has_value());
  ASSERT_TRUE(GlobalIndex.has_value());
  auto RelocOffset = findRawRelocation(Bytes, *Text, 0x25);
  ASSERT_TRUE(RelocOffset.has_value());

  llvm::MachO::symtab_command Symtab = Obj->getSymtabLoadCommand();
  size_t NListOffset =
      Symtab.symoff + size_t(*GlobalIndex) * sizeof(llvm::MachO::nlist);
  ASSERT_TRUE(
      rangeInBounds(NListOffset, sizeof(llvm::MachO::nlist), Bytes.size()));
  ASSERT_LT(uint64_t(Data->Address) + Data->Size,
            uint64_t(std::numeric_limits<uint32_t>::max()));
  writeLE<uint32_t>(Bytes.data() + NListOffset + 8,
                    Data->Address + Data->Size + 1);

  constexpr uint32_t Sentinel = 0x21436507;
  writeRawRelocation(Bytes, *RelocOffset, 0x25,
                     plainRelocationWord(*GlobalIndex, false, 2, true,
                                         llvm::MachO::GENERIC_RELOC_VANILLA));
  writeSectionField(Bytes, *Text, 0x25, Sentinel);

  auto ImgOrErr =
      loadBinary(writeMutation("external_symbol_past_end.o", Bytes));
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const Section *LoadedText = ImgOrErr->getSectionByName(section_names::macho::Text);
  const Symbol *Local = findSymbol(*ImgOrErr, "_local_bias");
  ASSERT_NE(LoadedText, nullptr);
  ASSERT_NE(Local, nullptr);
  auto Actual = readLoadedField(*ImgOrErr, LoadedText->VA + 0x25, 4, false);
  ASSERT_TRUE(Actual.has_value());
  EXPECT_EQ(*Actual, Sentinel);
  EXPECT_NE(ImgOrErr->WritableRelocDataAddrs.count(Local->Addr), 0u);
}

TEST_F(MachOI386Relocation,
       ExternalSectionSymbolAtEndIsAcceptedAndLaterRelocationContinues) {
  auto Bytes = readBinaryFile(fixture("test_macho_i386_nopic.o"));
  auto Obj = createMachOObject(Bytes);
  auto Text = rawSectionLayout(Bytes, section_names::macho::Text);
  auto Data = rawSectionLayout(Bytes, section_names::macho::Data);
  auto GlobalIndex = findSymbolIndex(Bytes, "_global_value");
  ASSERT_NE(Obj, nullptr);
  ASSERT_TRUE(Text.has_value());
  ASSERT_TRUE(Data.has_value());
  ASSERT_TRUE(GlobalIndex.has_value());
  auto RelocOffset = findRawRelocation(Bytes, *Text, 0x25);
  ASSERT_TRUE(RelocOffset.has_value());

  llvm::MachO::symtab_command Symtab = Obj->getSymtabLoadCommand();
  size_t NListOffset =
      Symtab.symoff + size_t(*GlobalIndex) * sizeof(llvm::MachO::nlist);
  ASSERT_TRUE(
      rangeInBounds(NListOffset, sizeof(llvm::MachO::nlist), Bytes.size()));
  ASSERT_LE(uint64_t(Data->Address) + Data->Size,
            uint64_t(std::numeric_limits<uint32_t>::max()));
  writeLE<uint32_t>(Bytes.data() + NListOffset + 8, Data->Address + Data->Size);

  writeRawRelocation(Bytes, *RelocOffset, 0x25,
                     plainRelocationWord(*GlobalIndex, false, 2, true,
                                         llvm::MachO::GENERIC_RELOC_VANILLA));
  writeSectionField(Bytes, *Text, 0x25, 0);

  auto ImgOrErr = loadBinary(writeMutation("external_symbol_at_end.o", Bytes));
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const Section *LoadedText = ImgOrErr->getSectionByName(section_names::macho::Text);
  const Section *LoadedData = ImgOrErr->getSectionByName(section_names::macho::Data);
  const Symbol *Local = findSymbol(*ImgOrErr, "_local_bias");
  ASSERT_NE(LoadedText, nullptr);
  ASSERT_NE(LoadedData, nullptr);
  ASSERT_NE(Local, nullptr);
  auto Actual = readLoadedField(*ImgOrErr, LoadedText->VA + 0x25, 4, false);
  ASSERT_TRUE(Actual.has_value());
  EXPECT_EQ(*Actual, LoadedData->VA + LoadedData->Size);
  EXPECT_NE(ImgOrErr->WritableRelocDataAddrs.count(Local->Addr), 0u);
}

TEST_F(MachOI386Relocation,
       UndefinedExternalIsUnchangedAndLaterRelocationContinues) {
  auto Bytes = readBinaryFile(fixture("test_macho_i386_nopic.o"));
  auto Obj = createMachOObject(Bytes);
  auto Text = rawSectionLayout(Bytes, section_names::macho::Text);
  auto UndefinedIndex = findSymbolIndex(Bytes, "_i386_global_address");
  ASSERT_NE(Obj, nullptr);
  ASSERT_TRUE(Text.has_value());
  ASSERT_TRUE(UndefinedIndex.has_value());
  auto RelocOffset = findRawRelocation(Bytes, *Text, 0x25);
  ASSERT_TRUE(RelocOffset.has_value());

  llvm::MachO::symtab_command Symtab = Obj->getSymtabLoadCommand();
  size_t NListOffset =
      Symtab.symoff + size_t(*UndefinedIndex) * sizeof(llvm::MachO::nlist);
  ASSERT_TRUE(
      rangeInBounds(NListOffset, sizeof(llvm::MachO::nlist), Bytes.size()));
  Bytes[NListOffset + 4] =
      uint8_t(llvm::MachO::N_UNDF) | uint8_t(llvm::MachO::N_EXT);
  Bytes[NListOffset + 5] = llvm::MachO::NO_SECT;
  writeLE<uint32_t>(Bytes.data() + NListOffset + 8, 0);

  constexpr uint32_t Sentinel = 0x13579bdf;
  writeRawRelocation(Bytes, *RelocOffset, 0x25,
                     plainRelocationWord(*UndefinedIndex, false, 2, true,
                                         llvm::MachO::GENERIC_RELOC_VANILLA));
  writeSectionField(Bytes, *Text, 0x25, Sentinel);

  auto ImgOrErr = loadBinary(writeMutation("undefined_external.o", Bytes));
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
  EXPECT_EQ(findSymbol(Img, "_i386_global_address"), nullptr);
  EXPECT_NE(Img.WritableRelocDataAddrs.count(Local->Addr), 0u);
}

TEST_F(MachOI386Relocation, AbsoluteZeroSymbolIsIgnored) {
  auto Bytes = readBinaryFile(fixture("test_macho_i386_nopic.o"));
  auto Obj = createMachOObject(Bytes);
  auto AbsoluteIndex = findSymbolIndex(Bytes, "_i386_global_address");
  ASSERT_NE(Obj, nullptr);
  ASSERT_TRUE(AbsoluteIndex.has_value());

  llvm::MachO::symtab_command Symtab = Obj->getSymtabLoadCommand();
  size_t NListOffset =
      Symtab.symoff + size_t(*AbsoluteIndex) * sizeof(llvm::MachO::nlist);
  ASSERT_TRUE(
      rangeInBounds(NListOffset, sizeof(llvm::MachO::nlist), Bytes.size()));
  Bytes[NListOffset + 4] =
      uint8_t(llvm::MachO::N_ABS) | uint8_t(llvm::MachO::N_EXT);
  Bytes[NListOffset + 5] = llvm::MachO::NO_SECT;
  writeLE<uint32_t>(Bytes.data() + NListOffset + 8, 0);

  auto ImgOrErr = loadBinary(writeMutation("absolute_zero.o", Bytes));
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_EQ(findSymbol(*ImgOrErr, "_i386_global_address"), nullptr);
  EXPECT_EQ(std::count_if(ImgOrErr->Exports.begin(), ImgOrErr->Exports.end(),
                          [](const Export &E) {
                            return E.Name == "_i386_global_address";
                          }),
            0u);
}

} // namespace
