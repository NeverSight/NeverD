//===- PEProgramExportsTests.cpp - Export identity and malformed tables --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/loader/COFF/PEProgramExports.h"

#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstring>

namespace neverd {
namespace {
using namespace llvm::object;
using namespace llvm::COFF;
#define NEVERD_PE_EXPORT_TEST_VALUE(Name, Value)                               \
  constexpr uint64_t Name = Value;
#define NEVERD_PE_EXPORT_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#include "PEProgramExportsCases.def"
#undef NEVERD_PE_EXPORT_TEST_VALUE
#undef NEVERD_PE_EXPORT_TEST_TEXT
class PEProgramExportsTest : public testing::Test {
protected:
  std::vector<uint8_t> File;
  uint64_t Directories = 0, Sections = 0;
  template <class T> void store(uint64_t Offset, const T &Value) {
    ASSERT_LE(Offset + sizeof(T), File.size());
    std::memcpy(File.data() + Offset, &Value, sizeof(T));
  }
  void text(uint64_t Offset, llvm::StringRef Name) {
    ASSERT_LT(Offset + Name.size(), File.size());
    std::memcpy(File.data() + Offset, Name.data(), Name.size());
    File[Offset + Name.size()] = 0;
  }
  void put32(uint64_t Offset, uint32_t Value) {
    llvm::support::endian::write32le(File.data() + Offset, Value);
  }
  void put16(uint64_t Offset, uint16_t Value) {
    llvm::support::endian::write16le(File.data() + Offset, Value);
  }
  void exportField(uint64_t Field, uint32_t Value) {
    put32(ExportOffset + Field, Value);
  }
  template <class Header> void build(uint16_t Magic) {
    File.assign(ExportOffset + SectionBytes, 0);
    dos_header DOS{};
    std::memcpy(DOS.Magic, DOSMagic, sizeof(DOS.Magic));
    DOS.AddressOfNewExeHeader = PEOffset;
    store(0, DOS);
    std::memcpy(File.data() + PEOffset, PEMagic, sizeof(PEMagic));
    coff_file_header COFF{};
    COFF.Machine = Magic == PE32Header::PE32_PLUS ? IMAGE_FILE_MACHINE_AMD64
                                                  : IMAGE_FILE_MACHINE_I386;
    COFF.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_DLL;
    COFF.NumberOfSections = 2;
    COFF.SizeOfOptionalHeader = sizeof(Header) + sizeof(data_directory);
    store(CoffOffset, COFF);
    Header H{};
    H.Magic = Magic;
    H.SizeOfHeaders = HeaderBytes;
    H.SizeOfImage = ImageSize;
    H.NumberOfRvaAndSize = 1;
    store(OptionalOffset, H);
    Directories = OptionalOffset + sizeof(Header);
    data_directory D{};
    D.RelativeVirtualAddress = ExportRVA;
    D.Size = ExportSize;
    store(Directories, D);
    Sections = Directories + sizeof(D);
    coff_section Code{};
    Code.VirtualAddress = CodeRVA;
    Code.VirtualSize = 2 * SectionBytes;
    Code.PointerToRawData = HeaderBytes;
    Code.SizeOfRawData = SectionBytes;
    store(Sections, Code);
    coff_section Exports{};
    Exports.VirtualAddress = ExportRVA;
    Exports.VirtualSize = SectionBytes;
    Exports.PointerToRawData = ExportOffset;
    Exports.SizeOfRawData = SectionBytes;
    store(Sections + sizeof(Code), Exports);
    export_directory_table_entry E{};
    E.NameRVA = ExportRVA + ModuleOffset;
    E.OrdinalBase = OrdinalBase;
    E.AddressTableEntries = AddressCount;
    E.NumberOfNamePointers = NameCount;
    E.ExportAddressTableRVA = ExportRVA + AddressesOffset;
    E.NamePointerRVA = ExportRVA + PointersOffset;
    E.OrdinalTableRVA = ExportRVA + OrdinalsOffset;
    store(ExportOffset, E);
    text(ExportOffset + ModuleOffset, ModuleName);
    text(ExportOffset + ForwarderOffset, ForwarderName);
    const uint32_t RVAs[] = {CodeRVA, 0, DataRVA, ExportRVA + ForwarderOffset};
    for (size_t I = 0; I < std::size(RVAs); ++I)
      put32(ExportOffset + AddressesOffset + I * sizeof(uint32_t), RVAs[I]);
    const char *Names[] = {AliasName, CodeName, ForwardName, HoleName,
                           DataName};
    const uint16_t Ordinals[] = {0, 0, 3, 1, 2};
    for (size_t I = 0; I < std::size(Names); ++I) {
      text(ExportOffset + NamesOffset + I * NameStride, Names[I]);
      put32(ExportOffset + PointersOffset + I * sizeof(uint32_t),
            ExportRVA + NamesOffset + I * NameStride);
      put16(ExportOffset + OrdinalsOffset + I * sizeof(uint16_t), Ordinals[I]);
    }
  }
  void SetUp() override { build<pe32plus_header>(PE32Header::PE32_PLUS); }
  void rejects() {
    auto R = readPEProgramExports(File);
    EXPECT_FALSE(bool(R));
    if (!R)
      llvm::consumeError(R.takeError());
  }
};
TEST_F(PEProgramExportsTest, RetainsAliasesHolesForwardersAndZeroFilledData) {
  auto R = readPEProgramExports(File);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Module, ModuleName);
  ASSERT_EQ(R->Entries.size(), AddressCount);
  const auto &Code = R->Entries[0];
  EXPECT_EQ(Code.Ordinal, OrdinalBase);
  EXPECT_EQ(Code.RVA, CodeRVA);
  EXPECT_EQ(Code.Kind, PEExportKind::Address);
  EXPECT_EQ(Code.Names, (std::vector<std::string>{AliasName, CodeName}));
  EXPECT_EQ(R->Entries[1].Kind, PEExportKind::Hole);
  EXPECT_EQ(R->Entries[1].Names, (std::vector<std::string>{HoleName}));
  EXPECT_EQ(R->Entries[2].RVA, DataRVA);
  EXPECT_EQ(R->Entries[2].Kind, PEExportKind::Address);
  EXPECT_EQ(R->Entries[3].Kind, PEExportKind::Forwarder);
  EXPECT_EQ(R->Entries[3].Forwarder, ForwarderName);
}
TEST_F(PEProgramExportsTest, DecodesPE32WithoutUsingPE32PlusOffsets) {
  build<pe32_header>(PE32Header::PE32);
  auto R = readPEProgramExports(File);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  ASSERT_EQ(R->Entries.size(), AddressCount);
  EXPECT_EQ(R->Entries.back().Forwarder, ForwarderName);
}
TEST_F(PEProgramExportsTest, SupportsHeaderBackedExportMetadata) {
  const uint64_t Spare = Sections + 2 * sizeof(coff_section);
  std::memcpy(File.data() + Spare, File.data() + ExportOffset,
              sizeof(export_directory_table_entry));
  put32(Directories, Spare);
  put32(Directories + sizeof(uint32_t), sizeof(export_directory_table_entry));
  auto R = readPEProgramExports(File);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  // Classification comes from the actual directory extent, not a section name.
  EXPECT_EQ(R->Entries.back().Kind, PEExportKind::Address);
}
TEST_F(PEProgramExportsTest,
       RetainsExternalMetadataFootprintsAndExactWorkCharges) {
  const uint64_t NameRVA = CodeRVA + NameStride;
  text(HeaderBytes + NameStride, ModuleName);
  exportField(offsetof(export_directory_table_entry, NameRVA), NameRVA);
  auto R = readPEProgramExports(File);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  auto Covered = [&](uint64_t RVA) {
    return std::any_of(
        R->Metadata.begin(), R->Metadata.end(),
        [&](const auto &M) { return RVA >= M.RVA && RVA - M.RVA < M.Size; });
  };
  EXPECT_TRUE(Covered(NameRVA));
  EXPECT_TRUE(Covered(NameRVA + sizeof(ModuleName) - 1));
  EXPECT_TRUE(Covered(ExportRVA + AddressesOffset));
  EXPECT_FALSE(Covered(CodeRVA));
  EXPECT_FALSE(Covered(DataRVA));
  PEExportLimits Exact{R->BytesRead, R->RecordsRead};
  auto Fits = readPEProgramExports(File, Exact);
  ASSERT_TRUE(bool(Fits)) << llvm::toString(Fits.takeError());
  --Exact.Bytes;
  auto Short = readPEProgramExports(File, Exact);
  EXPECT_FALSE(bool(Short));
  llvm::consumeError(Short.takeError());
}
TEST_F(PEProgramExportsTest, RejectsEveryTruncatedHeaderAndSectionTable) {
  const auto Original = File;
  for (uint64_t Size = 0; Size < Sections + 2 * sizeof(coff_section); ++Size) {
    File.assign(Original.begin(), Original.begin() + Size);
    rejects();
  }
}
TEST_F(PEProgramExportsTest,
       RejectsInconsistentDirectoryPairsAndTablePointers) {
  const auto Original = File;
  for (auto Field :
       {offsetof(export_directory_table_entry, NameRVA),
        offsetof(export_directory_table_entry, ExportAddressTableRVA),
        offsetof(export_directory_table_entry, NamePointerRVA),
        offsetof(export_directory_table_entry, OrdinalTableRVA)}) {
    File = Original;
    exportField(Field, 0);
    rejects();
  }
  File = Original;
  put32(Directories, 0);
  rejects();
  File = Original;
  put32(Directories + sizeof(uint32_t), 0);
  rejects();
}
TEST_F(PEProgramExportsTest, RejectsOrdinalOverflowAndOutOfRangeNameIndices) {
  exportField(offsetof(export_directory_table_entry, OrdinalBase), UINT32_MAX);
  rejects();
  exportField(offsetof(export_directory_table_entry, OrdinalBase), OrdinalBase);
  put16(ExportOffset + OrdinalsOffset, AddressCount);
  rejects();
}
TEST_F(PEProgramExportsTest, RejectsDuplicateAndUnsortedNames) {
  text(ExportOffset + NamesOffset + NameStride, AliasName);
  rejects();
  text(ExportOffset + NamesOffset + NameStride, CodeName);
  text(ExportOffset + NamesOffset, DataName);
  rejects();
}
TEST_F(PEProgramExportsTest, BoundsForwarderTerminationByDirectoryEnd) {
  put32(Directories + sizeof(uint32_t),
        ForwarderOffset + sizeof(ForwarderName) - 1);
  rejects();
}
TEST_F(PEProgramExportsTest, RejectsTargetsAndTablesOutsideTheirOwners) {
  const auto Original = File;
  put32(ExportOffset + AddressesOffset, ImageSize);
  rejects();
  File = Original;
  exportField(offsetof(export_directory_table_entry, ExportAddressTableRVA),
              ExportRVA + SectionBytes - sizeof(uint32_t));
  rejects();
  File = Original;
  exportField(offsetof(export_directory_table_entry, ExportAddressTableRVA),
              DataRVA);
  rejects();
}
TEST_F(PEProgramExportsTest, RejectsAmbiguousRawAndVirtualSections) {
  put32(Sections + sizeof(coff_section) +
            offsetof(coff_section, PointerToRawData),
        HeaderBytes);
  rejects();
  put32(Sections + sizeof(coff_section) +
            offsetof(coff_section, PointerToRawData),
        ExportOffset);
  put32(Sections + sizeof(coff_section) +
            offsetof(coff_section, VirtualAddress),
        CodeRVA);
  rejects();
}
TEST_F(PEProgramExportsTest, AppliesIndependentByteRecordAndStringBudgets) {
  for (const auto &Limits : {PEExportLimits{0}, PEExportLimits{File.size(), 1},
                             PEExportLimits{File.size(), 100, 1}}) {
    auto R = readPEProgramExports(File, Limits);
    EXPECT_FALSE(bool(R));
    if (!R)
      llvm::consumeError(R.takeError());
  }
}
TEST_F(PEProgramExportsTest,
       DistinguishesAbsentDirectoryFromMalformedDirectory) {
  put32(Directories, 0);
  put32(Directories + sizeof(uint32_t), 0);
  auto R = readPEProgramExports(File);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_TRUE(R->Entries.empty());
  EXPECT_TRUE(R->Module.empty());
}
} // namespace
} // namespace neverd
