//===- MachOI386RelocationTests.cpp - Mach-O i386 relocations ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"
#include "gtest/gtest.h"

#include "neverd/Object/SectionNames.h"
#include "neverd/Support/BinaryEncoding.h"
#include "neverd/Support/BinaryLoading.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/loader/MachO/MachORelocations.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace {

using namespace neverd::macho_loader::detail;

namespace fs = std::filesystem;
using namespace neverd;

fs::path fixture(llvm::StringRef Name) {
  return fs::path(TEST_OBJ_DIR) / Name.str();
}

class MachOI386Relocation : public NeverDLiftTest {
protected:
  fs::path writeMutation(llvm::StringRef Name,
                         const std::vector<uint8_t> &Bytes) const {
    fs::path Path = tmpFile(Name.str());
    std::fstream Out(Path, std::ios::binary | std::ios::out | std::ios::trunc);
    Out.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    EXPECT_TRUE(Out.good());
    return Path;
  }
};

struct MachOI386PipelineCase {
  const char *FixtureName;
  const char *TestName;
};

void PrintTo(const MachOI386PipelineCase &TestCase, std::ostream *Out) {
  *Out << TestCase.FixtureName;
}

class MachOI386Pipeline
    : public MachOI386Relocation,
      public ::testing::WithParamInterface<MachOI386PipelineCase> {};

std::vector<uint8_t> readBinaryFile(const fs::path &Path) {
  std::ifstream In(Path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(In), {});
}

std::unique_ptr<llvm::object::MachOObjectFile>
createMachOObject(const std::vector<uint8_t> &Bytes) {
  llvm::StringRef Data(reinterpret_cast<const char *>(Bytes.data()),
                       Bytes.size());
  auto ObjOrErr = llvm::object::ObjectFile::createMachOObjectFile(
      llvm::MemoryBufferRef(Data, "MachOI386Relocation fixture"));
  if (!ObjOrErr) {
    ADD_FAILURE() << llvm::toString(ObjOrErr.takeError());
    return nullptr;
  }
  return std::move(*ObjOrErr);
}

std::optional<llvm::object::SectionRef>
findSection(const llvm::object::MachOObjectFile &Obj, llvm::StringRef Name) {
  for (const llvm::object::SectionRef &Sec : Obj.sections()) {
    auto NameOrErr = Sec.getName();
    if (!NameOrErr) {
      llvm::consumeError(NameOrErr.takeError());
      continue;
    }
    if (*NameOrErr == Name)
      return Sec;
  }
  return std::nullopt;
}

std::vector<llvm::object::RelocationRef>
relocations(const llvm::object::SectionRef &Sec) {
  std::vector<llvm::object::RelocationRef> Result;
  for (const llvm::object::RelocationRef &Reloc : Sec.relocations())
    Result.push_back(Reloc);
  return Result;
}

struct OriginalSection {
  size_t Index = 0;
  uint64_t Base = 0;
};

std::optional<OriginalSection>
findOriginalSection(const llvm::object::MachOObjectFile &Obj, uint64_t Addr) {
  size_t Index = 0;
  for (const llvm::object::SectionRef &Sec : Obj.sections()) {
    uint64_t Base = Sec.getAddress();
    uint64_t Size = Sec.getSize();
    if (Addr >= Base && Addr - Base < Size)
      return OriginalSection{Index, Base};
    ++Index;
  }
  return std::nullopt;
}

std::optional<int64_t> readRelocationField(llvm::ArrayRef<uint8_t> Data,
                                           uint64_t Offset, uint8_t Width,
                                           bool SignedValue) {
  if (!rangeInBounds(Offset, Width, Data.size()))
    return std::nullopt;
  const uint8_t *Field = Data.data() + Offset;
  if (SignedValue) {
    switch (Width) {
    case 1:
      return readLE<int8_t>(Field);
    case 2:
      return readLE<int16_t>(Field);
    case 4:
      return readLE<int32_t>(Field);
    default:
      return std::nullopt;
    }
  }
  switch (Width) {
  case 1:
    return readLE<uint8_t>(Field);
  case 2:
    return readLE<uint16_t>(Field);
  case 4:
    return readLE<uint32_t>(Field);
  default:
    return std::nullopt;
  }
}

std::optional<int64_t> readLoadedField(const BinaryImage &Img, va_t Address,
                                       uint8_t Width, bool SignedValue) {
  const uint8_t *Field = Img.readVA(Address, Width);
  if (!Field)
    return std::nullopt;
  return readRelocationField(llvm::ArrayRef(Field, Width), 0, Width,
                             SignedValue);
}

const Symbol *findSymbol(const BinaryImage &Img, llvm::StringRef Name) {
  auto It = std::find_if(Img.Symbols.begin(), Img.Symbols.end(),
                         [&](const Symbol &Sym) { return Sym.Name == Name; });
  return It == Img.Symbols.end() ? nullptr : &*It;
}

struct RawSectionLayout {
  uint32_t Address = 0;
  uint32_t Size = 0;
  uint32_t FileOffset = 0;
  uint32_t RelocationOffset = 0;
  uint32_t RelocationCount = 0;
};

std::optional<RawSectionLayout>
rawSectionLayout(const std::vector<uint8_t> &Bytes, llvm::StringRef Name) {
  auto Obj = createMachOObject(Bytes);
  if (!Obj)
    return std::nullopt;
  auto Sec = findSection(*Obj, Name);
  if (!Sec)
    return std::nullopt;
  llvm::MachO::section Raw = Obj->getSection(Sec->getRawDataRefImpl());
  if (!rangeInBounds(Raw.offset, Raw.size, Bytes.size()) ||
      !rangeInBounds(Raw.reloff, uint64_t(Raw.nreloc) * 8, Bytes.size()))
    return std::nullopt;
  return RawSectionLayout{Raw.addr, Raw.size, Raw.offset, Raw.reloff,
                          Raw.nreloc};
}

std::optional<size_t> rawSectionHeaderOffset(const std::vector<uint8_t> &Bytes,
                                             llvm::StringRef Name) {
  auto Obj = createMachOObject(Bytes);
  if (!Obj || Obj->is64Bit())
    return std::nullopt;
  const char *Base = Obj->getData().data();
  for (const auto &LC : Obj->load_commands()) {
    if (LC.C.cmd != llvm::MachO::LC_SEGMENT)
      continue;
    auto Seg = Obj->getSegmentLoadCommand(LC);
    for (uint32_t I = 0; I < Seg.nsects; ++I) {
      auto Sec = Obj->getSection(LC, I);
      if (readMachOName(Sec.sectname) != Name)
        continue;
      const char *Header = LC.Ptr + sizeof(llvm::MachO::segment_command) +
                           size_t(I) * sizeof(llvm::MachO::section);
      if (Header < Base)
        return std::nullopt;
      size_t Offset = static_cast<size_t>(Header - Base);
      if (!rangeInBounds(Offset, sizeof(llvm::MachO::section), Bytes.size()))
        return std::nullopt;
      return Offset;
    }
  }
  return std::nullopt;
}

std::optional<size_t> findRawRelocation(const std::vector<uint8_t> &Bytes,
                                        const RawSectionLayout &Section,
                                        uint32_t Address) {
  for (uint32_t I = 0; I < Section.RelocationCount; ++I) {
    size_t Offset = Section.RelocationOffset + size_t(I) * 8;
    uint32_t Word0 = readLE<uint32_t>(Bytes.data() + Offset);
    uint32_t RelocAddress =
        Word0 & llvm::MachO::R_SCATTERED ? Word0 & 0x00ffffffu : Word0;
    if (RelocAddress == Address)
      return Offset;
  }
  return std::nullopt;
}

std::optional<uint32_t> findSymbolIndex(const std::vector<uint8_t> &Bytes,
                                        llvm::StringRef Name) {
  auto Obj = createMachOObject(Bytes);
  if (!Obj)
    return std::nullopt;
  llvm::MachO::symtab_command Symtab = Obj->getSymtabLoadCommand();
  for (uint32_t I = 0; I < Symtab.nsyms; ++I) {
    auto Sym = Obj->getSymbolByIndex(I);
    auto NameOrErr = Sym->getName();
    if (!NameOrErr) {
      llvm::consumeError(NameOrErr.takeError());
      continue;
    }
    if (*NameOrErr == Name)
      return I;
  }
  return std::nullopt;
}

uint32_t plainRelocationWord(uint32_t SymbolNumber, bool IsPCRel,
                             uint32_t Length, bool IsExternal, uint32_t Type) {
  return (SymbolNumber & 0x00ffffffu) | (uint32_t(IsPCRel) << 24) |
         ((Length & 3u) << 25) | (uint32_t(IsExternal) << 27) |
         ((Type & 0xfu) << 28);
}

uint32_t scatteredRelocationWord(uint32_t Address, bool IsPCRel,
                                 uint32_t Length, uint32_t Type) {
  return llvm::MachO::R_SCATTERED | (uint32_t(IsPCRel) << 30) |
         ((Length & 3u) << 28) | ((Type & 0xfu) << 24) |
         (Address & 0x00ffffffu);
}

void writeRawRelocation(std::vector<uint8_t> &Bytes, size_t Offset,
                        uint32_t Word0, uint32_t Word1) {
  writeLE<uint32_t>(Bytes.data() + Offset, Word0);
  writeLE<uint32_t>(Bytes.data() + Offset + 4, Word1);
}

void writeSectionField(std::vector<uint8_t> &Bytes,
                       const RawSectionLayout &Section, uint32_t Address,
                       uint32_t Value) {
  writeLE<uint32_t>(Bytes.data() + Section.FileOffset + Address, Value);
}

TEST_F(MachOI386Relocation, VanillaAbsoluteSupportsDeclaredWidths) {
  for (uint8_t Width : {uint8_t(1), uint8_t(2), uint8_t(4)}) {
    SCOPED_TRACE(static_cast<unsigned>(Width));
    auto Value = evaluateI386Vanilla({0x1200, 0x34, 0, Width, false});
    ASSERT_TRUE(Value.has_value());
    EXPECT_EQ(*Value, 0x1234);
  }
}

TEST_F(MachOI386Relocation, VanillaPCRelUsesPlaceAndFieldWidth) {
  auto External = evaluateI386Vanilla({0x2200, 7, 0x2000, 4, true});
  auto Local = evaluateI386Vanilla({0x2200, -3, 0x2000, 2, true});
  ASSERT_TRUE(External.has_value());
  ASSERT_TRUE(Local.has_value());
  EXPECT_EQ(*External, 0x203);
  EXPECT_EQ(*Local, 0x1fb);
}

TEST_F(MachOI386Relocation, VanillaRejectsInvalidWidthAndCheckedOverflow) {
  EXPECT_FALSE(evaluateI386Vanilla({1, 2, 0, 8, false}).has_value());
  EXPECT_FALSE(
      evaluateI386Vanilla({std::numeric_limits<int64_t>::max(), 1, 0, 4, false})
          .has_value());
  EXPECT_FALSE(
      evaluateI386Vanilla({0, 0, std::numeric_limits<uint64_t>::max(), 4, true})
          .has_value());
}

TEST_F(MachOI386Relocation, SectionDifferenceNormalizesOriginalSectionBases) {
  auto Value =
      evaluateI386SectionDifference(0x3000, 0x1800, 0x1200, 0x1000, 0x205);
  ASSERT_TRUE(Value.has_value());
  EXPECT_EQ(*Value, 0x1805);
}

TEST_F(MachOI386Relocation, SectionDifferenceUsesCheckedArithmetic) {
  EXPECT_FALSE(evaluateI386SectionDifference(
                   std::numeric_limits<int64_t>::max(), -1, 0, 0, 0)
                   .has_value());
  EXPECT_FALSE(evaluateI386SectionDifference(
                   0, 0, std::numeric_limits<int64_t>::min(), 1, 0)
                   .has_value());
}

TEST_F(MachOI386Relocation, WriterStoresLittleEndianWidths) {
  std::array<uint8_t, 7> Data{};
  EXPECT_TRUE(writeI386RelocationField(Data, 0, 1, 0xab, false));
  EXPECT_TRUE(writeI386RelocationField(Data, 1, 2, 0xcdef, false));
  EXPECT_TRUE(writeI386RelocationField(Data, 3, 4, 0x12345678, false));
  EXPECT_EQ(Data,
            (std::array<uint8_t, 7>{0xab, 0xef, 0xcd, 0x78, 0x56, 0x34, 0x12}));
}

TEST_F(MachOI386Relocation, WriterChecksSignedAndUnsignedRanges) {
  std::array<uint8_t, 4> Data{0xaa, 0xbb, 0xcc, 0xdd};
  EXPECT_TRUE(writeI386RelocationField(Data, 0, 1, -128, true));
  EXPECT_EQ(Data[0], 0x80);
  const auto Before = Data;
  EXPECT_FALSE(writeI386RelocationField(Data, 0, 1, -129, true));
  EXPECT_FALSE(writeI386RelocationField(Data, 0, 1, 128, true));
  EXPECT_FALSE(writeI386RelocationField(Data, 0, 1, -1, false));
  EXPECT_FALSE(writeI386RelocationField(Data, 0, 1, 256, false));
  EXPECT_EQ(Data, Before);
}

TEST_F(MachOI386Relocation, WriterChecksEveryUnsignedWidthBoundary) {
  struct Case {
    uint8_t Width;
    int64_t Max;
  };
  for (const Case C :
       {Case{1, UINT8_MAX}, Case{2, UINT16_MAX}, Case{4, UINT32_MAX}}) {
    SCOPED_TRACE(static_cast<unsigned>(C.Width));
    std::array<uint8_t, 4> Data{0xaa, 0xbb, 0xcc, 0xdd};
    EXPECT_TRUE(writeI386RelocationField(Data, 0, C.Width, 0, false));
    EXPECT_TRUE(writeI386RelocationField(Data, 0, C.Width, C.Max, false));
    const auto Before = Data;
    EXPECT_FALSE(writeI386RelocationField(Data, 0, C.Width, -1, false));
    EXPECT_FALSE(writeI386RelocationField(Data, 0, C.Width, C.Max + 1, false));
    EXPECT_EQ(Data, Before);
  }
}

TEST_F(MachOI386Relocation, WriterChecksEverySignedWidthBoundary) {
  struct Case {
    uint8_t Width;
    int64_t Min;
    int64_t Max;
  };
  for (const Case C :
       {Case{1, INT8_MIN, INT8_MAX}, Case{2, INT16_MIN, INT16_MAX},
        Case{4, INT32_MIN, INT32_MAX}}) {
    SCOPED_TRACE(static_cast<unsigned>(C.Width));
    std::array<uint8_t, 4> Data{0xaa, 0xbb, 0xcc, 0xdd};
    EXPECT_TRUE(writeI386RelocationField(Data, 0, C.Width, C.Min, true));
    EXPECT_TRUE(writeI386RelocationField(Data, 0, C.Width, C.Max, true));
    const auto Before = Data;
    EXPECT_FALSE(writeI386RelocationField(Data, 0, C.Width, C.Min - 1, true));
    EXPECT_FALSE(writeI386RelocationField(Data, 0, C.Width, C.Max + 1, true));
    EXPECT_EQ(Data, Before);
  }
}

TEST_F(MachOI386Relocation, WriterRejectsInvalidWidthAndTruncatedField) {
  std::array<uint8_t, 4> Data{1, 2, 3, 4};
  const auto Before = Data;
  EXPECT_FALSE(writeI386RelocationField(Data, 0, 8, 0, false));
  EXPECT_FALSE(writeI386RelocationField(Data, 2, 4, 0, false));
  EXPECT_EQ(Data, Before);
}

TEST_F(MachOI386Relocation, RealFixturesContainRequiredRawRelocationShapes) {
  auto PICBytes = readBinaryFile(fixture("test_macho_i386.o"));
  auto NoPICBytes = readBinaryFile(fixture("test_macho_i386_nopic.o"));
  ASSERT_FALSE(PICBytes.empty());
  ASSERT_FALSE(NoPICBytes.empty());
  auto PIC = createMachOObject(PICBytes);
  auto NoPIC = createMachOObject(NoPICBytes);
  ASSERT_NE(PIC, nullptr);
  ASSERT_NE(NoPIC, nullptr);
  EXPECT_FALSE(PIC->is64Bit());
  EXPECT_EQ(PIC->getHeader().cputype, llvm::MachO::CPU_TYPE_I386);
  EXPECT_EQ(PIC->getHeader().filetype, llvm::MachO::MH_OBJECT);

  auto PICText = findSection(*PIC, section_names::macho::Text);
  ASSERT_TRUE(PICText.has_value());
  auto PICRelocs = relocations(*PICText);
  size_t DifferencePairs = 0;
  for (size_t I = 0; I < PICRelocs.size(); ++I) {
    auto Info = PIC->getRelocation(PICRelocs[I].getRawDataRefImpl());
    uint32_t Type = PIC->getAnyRelocationType(Info);
    if (Type != llvm::MachO::GENERIC_RELOC_SECTDIFF &&
        Type != llvm::MachO::GENERIC_RELOC_LOCAL_SECTDIFF)
      continue;
    ASSERT_LT(I + 1, PICRelocs.size());
    auto PairInfo = PIC->getRelocation(PICRelocs[I + 1].getRawDataRefImpl());
    EXPECT_TRUE(PIC->isRelocationScattered(Info));
    EXPECT_TRUE(PIC->isRelocationScattered(PairInfo));
    EXPECT_EQ(PIC->getAnyRelocationType(PairInfo),
              llvm::MachO::GENERIC_RELOC_PAIR);
    EXPECT_EQ(PIC->getAnyRelocationLength(Info), 2u);
    EXPECT_FALSE(PIC->getAnyRelocationPCRel(Info));
    ++DifferencePairs;
    ++I;
  }
  EXPECT_GE(DifferencePairs, 3u);

  auto NoPICText = findSection(*NoPIC, section_names::macho::Text);
  ASSERT_TRUE(NoPICText.has_value());
  size_t LocalVanilla = 0;
  for (const auto &Reloc : relocations(*NoPICText)) {
    auto Info = NoPIC->getRelocation(Reloc.getRawDataRefImpl());
    if (NoPIC->getAnyRelocationType(Info) ==
            llvm::MachO::GENERIC_RELOC_VANILLA &&
        !NoPIC->isRelocationScattered(Info) &&
        !NoPIC->getPlainRelocationExternal(Info) &&
        !NoPIC->getAnyRelocationPCRel(Info) &&
        NoPIC->getAnyRelocationLength(Info) == 2u)
      ++LocalVanilla;
  }
  EXPECT_GE(LocalVanilla, 3u);

  for (const auto *Obj : {PIC.get(), NoPIC.get()}) {
    auto Data = findSection(*Obj, section_names::macho::Data);
    ASSERT_TRUE(Data.has_value());
    auto DataRelocs = relocations(*Data);
    ASSERT_GE(DataRelocs.size(), 2u);
    for (const auto &Reloc : DataRelocs) {
      auto Info = Obj->getRelocation(Reloc.getRawDataRefImpl());
      EXPECT_EQ(Obj->getAnyRelocationType(Info),
                llvm::MachO::GENERIC_RELOC_VANILLA);
      EXPECT_FALSE(Obj->isRelocationScattered(Info));
      EXPECT_FALSE(Obj->getPlainRelocationExternal(Info));
      EXPECT_FALSE(Obj->getAnyRelocationPCRel(Info));
      EXPECT_EQ(Obj->getAnyRelocationLength(Info), 2u);
    }
  }
}

TEST_F(MachOI386Relocation,
       ObjectSectionsProjectToExactNonOverlappingSegments) {
  for (llvm::StringRef Name : {llvm::StringRef("test_macho_i386.o"),
                               llvm::StringRef("test_macho_i386_nopic.o")}) {
    SCOPED_TRACE(Name.str());
    auto ImgOrErr = loadBinary(fixture(Name));
    ASSERT_TRUE(static_cast<bool>(ImgOrErr))
        << llvm::toString(ImgOrErr.takeError());
    const BinaryImage &Img = *ImgOrErr;

    size_t NonEmptySections =
        std::count_if(Img.Sections.begin(), Img.Sections.end(),
                      [](const Section &Sec) { return Sec.Size != 0; });
    ASSERT_EQ(Img.Segments.size(), NonEmptySections);
    for (const Section &Sec : Img.Sections) {
      if (Sec.Size == 0)
        continue;
      auto It = std::find_if(Img.Segments.begin(), Img.Segments.end(),
                             [&](const Segment &Seg) {
                               return Seg.VA == Sec.VA && Seg.Size == Sec.Size;
                             });
      ASSERT_NE(It, Img.Segments.end()) << Sec.SegmentName << ',' << Sec.Name;
      EXPECT_EQ(It->Name, Sec.SegmentName);
      EXPECT_EQ(It->FileOff, Sec.FileOff);
      EXPECT_EQ(It->FileSz, Sec.FileSz);
      EXPECT_EQ(It->Flags, Sec.Flags);
      EXPECT_EQ(It->Data, Sec.Data);
    }

    const Section *Text = Img.getSectionByName(section_names::macho::Text);
    const Section *Const = Img.getSectionByName(section_names::macho::Const);
    const Segment *TextSegment = Img.getTextSegment();
    ASSERT_NE(Text, nullptr);
    ASSERT_NE(Const, nullptr);
    ASSERT_NE(TextSegment, nullptr);
    EXPECT_TRUE(Text->isExecutable());
    EXPECT_FALSE(Const->isExecutable());
    EXPECT_EQ(TextSegment->VA, Text->VA);
    EXPECT_EQ(TextSegment->Size, Text->Size);

    for (size_t I = 0; I < Img.Segments.size(); ++I) {
      const Segment &A = Img.Segments[I];
      ASSERT_LE(A.Size, InvalidVA - A.VA);
      for (size_t J = I + 1; J < Img.Segments.size(); ++J) {
        const Segment &B = Img.Segments[J];
        ASSERT_LE(B.Size, InvalidVA - B.VA);
        EXPECT_TRUE(A.VA + A.Size <= B.VA || B.VA + B.Size <= A.VA)
            << A.Name << " overlaps " << B.Name;
      }
    }

    const Section *Bss = Img.getSectionByName(section_names::macho::Bss);
    ASSERT_NE(Bss, nullptr);
    EXPECT_EQ(Bss->FileSz, 0u);
    ASSERT_EQ(Bss->Data.size(), Bss->Size);
    EXPECT_TRUE(std::all_of(Bss->Data.begin(), Bss->Data.end(),
                            [](uint8_t Byte) { return Byte == 0; }));
  }
}

TEST_F(MachOI386Relocation, OverlappingObjectSectionsReturnControlledError) {
  auto Bytes = readBinaryFile(fixture("test_macho_i386_nopic.o"));
  auto Text = rawSectionLayout(Bytes, section_names::macho::Text);
  auto DataHeader = rawSectionHeaderOffset(Bytes, section_names::macho::Data);
  ASSERT_TRUE(Text.has_value());
  ASSERT_TRUE(DataHeader.has_value());
  size_t AddressOffset = *DataHeader + offsetof(llvm::MachO::section, addr);
  ASSERT_TRUE(rangeInBounds(AddressOffset, sizeof(uint32_t), Bytes.size()));
  writeLE<uint32_t>(Bytes.data() + AddressOffset, Text->Address);

  auto ImgOrErr = loadBinary(writeMutation("overlapping_sections.o", Bytes));
  ASSERT_FALSE(static_cast<bool>(ImgOrErr));
  std::string Error = llvm::toString(ImgOrErr.takeError());
  EXPECT_NE(Error.find("relocatable sections overlap"), std::string::npos)
      << Error;
}

TEST_F(MachOI386Relocation,
       RealFixturesUseExactFormulasAndSectionAwareProvenance) {
  for (llvm::StringRef Name : {llvm::StringRef("test_macho_i386.o"),
                               llvm::StringRef("test_macho_i386_nopic.o")}) {
    SCOPED_TRACE(Name.str());
    auto Bytes = readBinaryFile(fixture(Name));
    ASSERT_FALSE(Bytes.empty());
    auto Obj = createMachOObject(Bytes);
    ASSERT_NE(Obj, nullptr);

    auto ImgOrErr = loadBinary(fixture(Name));
    ASSERT_TRUE(static_cast<bool>(ImgOrErr))
        << llvm::toString(ImgOrErr.takeError());
    const BinaryImage &Img = *ImgOrErr;
    ASSERT_EQ(Img.Sections.size(),
              static_cast<size_t>(
                  std::distance(Obj->section_begin(), Obj->section_end())));

    size_t SecIndex = 0;
    for (const llvm::object::SectionRef &Sec : Obj->sections()) {
      SCOPED_TRACE(SecIndex);
      auto ContentsOrErr = Sec.getContents();
      ASSERT_TRUE(static_cast<bool>(ContentsOrErr))
          << llvm::toString(ContentsOrErr.takeError());
      llvm::StringRef Contents = *ContentsOrErr;
      llvm::ArrayRef<uint8_t> RawData(
          reinterpret_cast<const uint8_t *>(Contents.data()), Contents.size());
      auto Relocs = relocations(Sec);

      for (size_t I = 0; I < Relocs.size(); ++I) {
        auto Info = Obj->getRelocation(Relocs[I].getRawDataRefImpl());
        uint32_t Type = Obj->getAnyRelocationType(Info);
        uint32_t Length = Obj->getAnyRelocationLength(Info);
        ASSERT_LE(Length, 2u);
        uint8_t Width = uint8_t(1u << Length);
        uint64_t Offset = Relocs[I].getOffset();
        va_t Place = Img.Sections[SecIndex].VA + Offset;

        if (Type == llvm::MachO::GENERIC_RELOC_VANILLA) {
          bool IsPCRel = Obj->getAnyRelocationPCRel(Info);
          auto Existing = readRelocationField(RawData, Offset, Width, IsPCRel);
          ASSERT_TRUE(Existing.has_value());

          int64_t Target = 0;
          int64_t Addend = *Existing;
          if (Obj->isRelocationScattered(Info)) {
            uint64_t EncodedTarget = Obj->getScatteredRelocationValue(Info);
            auto TargetSec = findOriginalSection(*Obj, EncodedTarget);
            ASSERT_TRUE(TargetSec.has_value());
            Target = Img.Sections[TargetSec->Index].VA;
            Addend -= TargetSec->Base;
          } else {
            ASSERT_FALSE(Obj->getPlainRelocationExternal(Info));
            uint32_t TargetSectionNumber =
                Obj->getPlainRelocationSymbolNum(Info);
            ASSERT_GT(TargetSectionNumber, 0u);
            ASSERT_LE(TargetSectionNumber, Img.Sections.size());
            size_t TargetIndex = TargetSectionNumber - 1;
            Target = Img.Sections[TargetIndex].VA;
            Addend -= Obj->getAnyRelocationSection(Info).getAddress();
            if (IsPCRel)
              Addend += Sec.getAddress() + Offset + Width;
          }

          auto Expected =
              evaluateI386Vanilla({Target, Addend, Place, Width, IsPCRel});
          ASSERT_TRUE(Expected.has_value());
          auto Actual = readLoadedField(Img, Place, Width, IsPCRel);
          ASSERT_TRUE(Actual.has_value());
          EXPECT_EQ(*Actual, *Expected);
          continue;
        }

        if (Type != llvm::MachO::GENERIC_RELOC_SECTDIFF &&
            Type != llvm::MachO::GENERIC_RELOC_LOCAL_SECTDIFF)
          continue;

        ASSERT_LT(I + 1, Relocs.size());
        auto PairInfo = Obj->getRelocation(Relocs[I + 1].getRawDataRefImpl());
        ASSERT_TRUE(Obj->isRelocationScattered(Info));
        ASSERT_TRUE(Obj->isRelocationScattered(PairInfo));
        ASSERT_EQ(Obj->getAnyRelocationType(PairInfo),
                  llvm::MachO::GENERIC_RELOC_PAIR);
        auto Existing = readRelocationField(RawData, Offset, Width, true);
        ASSERT_TRUE(Existing.has_value());

        uint64_t EncodedA = Obj->getScatteredRelocationValue(Info);
        uint64_t EncodedB = Obj->getScatteredRelocationValue(PairInfo);
        auto SectionA = findOriginalSection(*Obj, EncodedA);
        auto SectionB = findOriginalSection(*Obj, EncodedB);
        ASSERT_TRUE(SectionA.has_value());
        ASSERT_TRUE(SectionB.has_value());
        int64_t FinalA =
            Img.Sections[SectionA->Index].VA + (EncodedA - SectionA->Base);
        int64_t FinalB =
            Img.Sections[SectionB->Index].VA + (EncodedB - SectionB->Base);
        auto Expected = evaluateI386SectionDifference(FinalA, FinalB, EncodedA,
                                                      EncodedB, *Existing);
        ASSERT_TRUE(Expected.has_value());
        auto Actual = readLoadedField(Img, Place, Width, true);
        ASSERT_TRUE(Actual.has_value());
        EXPECT_EQ(*Actual, *Expected);
        ++I;
      }
      ++SecIndex;
    }

    const Section *Text = Img.getSectionByName(section_names::macho::Text);
    const Section *ReadOnly = Img.getSectionByName(section_names::macho::Const);
    const Section *Data = Img.getSectionByName(section_names::macho::Data);
    ASSERT_NE(Text, nullptr);
    ASSERT_NE(ReadOnly, nullptr);
    ASSERT_NE(Data, nullptr);
    EXPECT_TRUE(Text->isReadable());
    EXPECT_FALSE(Text->isWritable());
    EXPECT_TRUE(Text->isExecutable());
    EXPECT_TRUE(ReadOnly->isReadable());
    EXPECT_FALSE(ReadOnly->isWritable());
    EXPECT_FALSE(ReadOnly->isExecutable());
    EXPECT_TRUE(Data->isReadable());
    EXPECT_TRUE(Data->isWritable());
    EXPECT_FALSE(Data->isExecutable());
    EXPECT_NE(Img.CodePtrRelocSlots.count(Data->VA + 8), 0u);
    EXPECT_NE(Img.CodeRefTargets.count(Text->VA), 0u);

    const Symbol *ReadOnlyValue = findSymbol(Img, "_readonly_value");
    const Symbol *ReadOnlySlot = findSymbol(Img, "_i386_readonly_dispatch");
    ASSERT_NE(ReadOnlyValue, nullptr);
    ASSERT_NE(ReadOnlySlot, nullptr);
    EXPECT_NE(Img.RelocDataAddrs.count(ReadOnlyValue->Addr), 0u);
    EXPECT_NE(Img.DataPtrRelocSlots.count(ReadOnlySlot->Addr), 0u);
    EXPECT_EQ(Img.WritableRelocDataAddrs.count(ReadOnlyValue->Addr), 0u);
    EXPECT_EQ(Img.CodeRefTargets.count(ReadOnlyValue->Addr), 0u);
    EXPECT_EQ(Img.CodePtrRelocSlots.count(ReadOnlySlot->Addr), 0u);

    if (Name.ends_with("_nopic.o")) {
      for (llvm::StringRef SymbolName :
           {llvm::StringRef("_global_value"), llvm::StringRef("_local_bias")}) {
        const Symbol *Sym = findSymbol(Img, SymbolName);
        ASSERT_NE(Sym, nullptr);
        EXPECT_NE(Img.WritableRelocDataAddrs.count(Sym->Addr), 0u);
        EXPECT_EQ(Img.CodeRefTargets.count(Sym->Addr), 0u);
        EXPECT_EQ(Img.RelocDataAddrs.count(Sym->Addr), 0u);
      }
    }
  }
}

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

    macho_loader::applyObjectRelocations(*Obj, *ImgOrErr);
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

TEST_F(MachOI386Relocation, SectionDefinedFunctionAtZeroIsRetainedAndDetected) {
  auto Path = fixture("test_macho_i386.o");
  auto ImgOrErr = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  EXPECT_EQ(Img.Format, BinaryFormat::MachO);
  EXPECT_EQ(Img.Arch, Arch::X86);
  EXPECT_EQ(Img.Bits, Bitness::Bits32);
  EXPECT_TRUE(Img.IsRelocatable);

  auto Add =
      std::find_if(Img.Symbols.begin(), Img.Symbols.end(),
                   [](const Symbol &S) { return S.Name == "_i386_add"; });
  ASSERT_NE(Add, Img.Symbols.end());
  EXPECT_TRUE(Add->IsFunc);
  EXPECT_EQ(Add->Addr, 0u);
  for (llvm::StringRef DataName :
       {"_global_value", "_local_bias", "_readonly_value", "_i386_dispatch",
        "_i386_readonly_dispatch"}) {
    EXPECT_EQ(std::count_if(Img.Symbols.begin(), Img.Symbols.end(),
                            [&](const Symbol &S) {
                              return S.Name == DataName && S.IsFunc;
                            }),
              0u);
    EXPECT_EQ(
        std::count_if(Img.Exports.begin(), Img.Exports.end(),
                      [&](const Export &E) { return E.Name == DataName; }),
        0u);
  }

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  auto Functions = Detector.detect(Img, Dec);
  EXPECT_NE(std::find(Functions.begin(), Functions.end(),
                      std::make_pair(va_t(0), std::string("_i386_add"))),
            Functions.end());
  for (llvm::StringRef DataName :
       {"_global_value", "_local_bias", "_readonly_value", "_i386_dispatch",
        "_i386_readonly_dispatch"})
    EXPECT_EQ(
        std::count_if(Functions.begin(), Functions.end(),
                      [&](const auto &F) { return F.second == DataName; }),
        0u);
}

TEST_F(MachOI386Relocation,
       ZExtConstantDoesNotCreatePointerMirrorWithoutPointerSlots) {
  auto Bytes = readBinaryFile(fixture("test_macho_i386.o"));
  auto Data = rawSectionLayout(Bytes, section_names::macho::Data);
  ASSERT_TRUE(Data.has_value());
  for (uint32_t Offset : {8u, 12u}) {
    auto RelocOffset = findRawRelocation(Bytes, *Data, Offset);
    ASSERT_TRUE(RelocOffset.has_value());
    writeRawRelocation(Bytes, *RelocOffset, Offset,
                       plainRelocationWord(2, false, 2, false,
                                           llvm::MachO::GENERIC_RELOC_VANILLA));
    writeSectionField(Bytes, *Data, Offset, Data->Address);
  }

  fs::path Path = writeMutation("zext_without_pointer_slots.o", Bytes);
  auto ImgOrErr = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_TRUE(ImgOrErr->CodePtrRelocSlots.empty());
  EXPECT_TRUE(ImgOrErr->DataPtrRelocSlots.empty());

  auto LLVM = liftToLLVMIR(Path);
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  EXPECT_EQ(LLVM.out.find("@__nd_codeptr_"), std::string::npos);
}

TEST_F(MachOI386Relocation,
       DispatchUsesRelinkablePointerToRecompiledZeroAddressFunction) {
  auto PICBytes = readBinaryFile(fixture("test_macho_i386.o"));
  auto PICObj = createMachOObject(PICBytes);
  auto PICText = PICObj ? findSection(*PICObj, section_names::macho::Text) : std::nullopt;
  auto PICData = rawSectionLayout(PICBytes, section_names::macho::Data);
  ASSERT_NE(PICObj, nullptr);
  ASSERT_TRUE(PICText.has_value());
  ASSERT_TRUE(PICData.has_value());

  std::optional<uint64_t> DispatchImmediateOffset;
  for (const auto &Reloc : relocations(*PICText)) {
    auto Info = PICObj->getRelocation(Reloc.getRawDataRefImpl());
    uint32_t Type = PICObj->getAnyRelocationType(Info);
    if ((Type == llvm::MachO::GENERIC_RELOC_SECTDIFF ||
         Type == llvm::MachO::GENERIC_RELOC_LOCAL_SECTDIFF) &&
        PICObj->getScatteredRelocationValue(Info) == PICData->Address + 8) {
      DispatchImmediateOffset = Reloc.getOffset();
      break;
    }
  }
  ASSERT_TRUE(DispatchImmediateOffset.has_value());

  auto PICImgOrErr = loadBinary(fixture("test_macho_i386.o"));
  ASSERT_TRUE(static_cast<bool>(PICImgOrErr))
      << llvm::toString(PICImgOrErr.takeError());
  const Section *LoadedText = PICImgOrErr->getSectionByName(section_names::macho::Text);
  const Section *LoadedData = PICImgOrErr->getSectionByName(section_names::macho::Data);
  ASSERT_NE(LoadedText, nullptr);
  ASSERT_NE(LoadedData, nullptr);
  EXPECT_EQ(PICImgOrErr->CodePtrRelocSlots.count(LoadedText->VA +
                                                 *DispatchImmediateOffset),
            0u);
  EXPECT_EQ(PICImgOrErr->CodePtrRelocSlots.count(LoadedData->VA), 0u);
  EXPECT_EQ(PICImgOrErr->CodePtrRelocSlots.count(LoadedData->VA + 4), 0u);
  EXPECT_NE(PICImgOrErr->CodePtrRelocSlots.count(LoadedData->VA + 8), 0u);
  EXPECT_NE(PICImgOrErr->WritableRelocDataAddrs.count(LoadedData->VA + 8), 0u);

  for (llvm::StringRef Name : {llvm::StringRef("test_macho_i386.o"),
                               llvm::StringRef("test_macho_i386_nopic.o")}) {
    SCOPED_TRACE(Name.str());
    auto LLVM = liftToLLVMIR(fixture(Name));
    ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
    EXPECT_NE(LLVM.out.find("@_i386_call_dispatch"), std::string::npos);
    EXPECT_NE(LLVM.out.find("ptrtoint (ptr @_i386_add to i32)"),
              std::string::npos);
  }
}

TEST_P(MachOI386Pipeline, CompletesLiftAndDecompilation) {
  const fs::path Path = fixture(GetParam().FixtureName);
  verifyAllStages(Path);
  verifyNoUnlifted(Path);

  auto ImgOrErr = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const Symbol *Global = findSymbol(*ImgOrErr, "_global_value");
  const Symbol *Local = findSymbol(*ImgOrErr, "_local_bias");
  ASSERT_NE(Global, nullptr);
  ASSERT_NE(Local, nullptr);

  auto LLVM = liftToLLVMIR(Path);
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  EXPECT_NE(LLVM.out.find("@_i386_add"), std::string::npos);
  EXPECT_NE(LLVM.out.find("@_i386_global_address"), std::string::npos);
  auto HasGlobalReference = [&](llvm::StringRef Name, va_t Address) {
    if (LLVM.out.find(Name.str()) != std::string::npos ||
        LLVM.out.find("i64 " + std::to_string(Address) + " to ptr") !=
            std::string::npos)
      return true;
    const Segment *Seg = ImgOrErr->getSegmentFor(Address);
    if (!Seg)
      return false;
    std::string Mirror = "@__nd_codeptr_" + llvm::utohexstr(Seg->VA);
    if (LLVM.out.find(Mirror) == std::string::npos)
      return false;
    uint64_t Offset = Address - Seg->VA;
    return Offset == 0 ||
           LLVM.out.find(Mirror + ", i64 " + std::to_string(Offset)) !=
               std::string::npos;
  };
  EXPECT_TRUE(HasGlobalReference("_global_value", Global->Addr));
  EXPECT_TRUE(HasGlobalReference("_local_bias", Local->Addr));

  auto Decompile = decompileToHighC(Path);
  ASSERT_EQ(Decompile.exitCode, 0) << Decompile.err;
  auto CBytes = readBinaryFile(tmpFile("decompiled_high.c"));
  ASSERT_FALSE(CBytes.empty());
  llvm::StringRef C(reinterpret_cast<const char *>(CBytes.data()),
                    CBytes.size());
  EXPECT_TRUE(C.contains("i386_add"));
  EXPECT_TRUE(C.contains("i386_global_address"));
  EXPECT_FALSE(C.contains_insensitive("unlifted"));
}

INSTANTIATE_TEST_SUITE_P(
    ThinObjects, MachOI386Pipeline,
    ::testing::Values(MachOI386PipelineCase{"test_macho_i386.o", "PIC"},
                      MachOI386PipelineCase{"test_macho_i386_nopic.o",
                                            "NoPIC"}),
    [](const ::testing::TestParamInfo<MachOI386PipelineCase> &Info) {
      return Info.param.TestName;
    });

} // namespace
