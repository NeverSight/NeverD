//===- SBFLoaderTests.cpp - Solana SBF ELF loader tests -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SBFFixtureBuilder.h"
#include "gtest/gtest.h"

#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

namespace neverd {
namespace {

struct DebugSymbolRecord {
  llvm::StringRef Name;
  size_t EntrySlot;
  uint64_t Size;
  uint8_t Type = llvm::ELF::STT_FUNC;
};

std::vector<uint8_t>
buildDebugSymbolELF(bool Strict, llvm::ArrayRef<DebugSymbolRecord> Records) {
  using ELFT = llvm::object::ELF64LE;
  using namespace llvm::ELF;
  std::vector<uint8_t> Text(2 * sbf::kInstructionSize, 0);
  for (size_t Slot = 0; Slot < 2; ++Slot)
    Text[Slot * sbf::kInstructionSize] =
        sbf::getOpcodeInfo(sbf::Opcode::EXIT)->Encoding;

  std::vector<uint8_t> Bytes;
  if (Strict) {
    sbf::test::StrictELFOptions Options;
    Options.AddDebugSymbols = true;
    Options.Text = Text;
    for (const DebugSymbolRecord &Record : Records)
      Options.FunctionSymbols.push_back(
          {Record.Name.str(), Record.EntrySlot, 0});
    Bytes = sbf::test::buildStrictELF(Options);
  } else {
    sbf::test::LegacyELFOptions Options;
    Options.Text = Text;
    Bytes = sbf::test::buildLegacyELF(Options);
    ELFT::Ehdr Header;
    std::memcpy(&Header, Bytes.data(), sizeof(Header));
    std::vector<ELFT::Shdr> Sections(Header.e_shnum);
    std::memcpy(Sections.data(), Bytes.data() + Header.e_shoff,
                Sections.size() * sizeof(ELFT::Shdr));
    const auto OriginalNames = Sections[Header.e_shstrndx];
    std::string SectionNames(
        reinterpret_cast<const char *>(Bytes.data() + OriginalNames.sh_offset),
        OriginalNames.sh_size);
    const uint32_t StringTableName = static_cast<uint32_t>(SectionNames.size());
    SectionNames.append(".strtab", sizeof(".strtab"));
    const uint32_t SymbolTableName = static_cast<uint32_t>(SectionNames.size());
    SectionNames.append(".symtab", sizeof(".symtab"));
    std::string SymbolNames(1, '\0');
    std::vector<ELFT::Sym> Symbols(Records.size() + 1);
    for (size_t Index = 0; Index < Records.size(); ++Index) {
      Symbols[Index + 1].st_name = static_cast<uint32_t>(SymbolNames.size());
      SymbolNames.append(Records[Index].Name.data(),
                         Records[Index].Name.size());
      SymbolNames.push_back('\0');
    }

    // Keep the legacy fixture's section spans in file order. The optional
    // debug tables live after its existing text and section-name data.
    const uint64_t NamesOffset = OriginalNames.sh_offset;
    const uint64_t StringsOffset = NamesOffset + SectionNames.size();
    const uint64_t SymbolsOffset = llvm::alignTo(
        StringsOffset + SymbolNames.size(), kELF64RecordAlignment);
    const uint64_t HeadersOffset =
        llvm::alignTo(SymbolsOffset + Symbols.size() * sizeof(ELFT::Sym),
                      kELF64RecordAlignment);
    Sections[Header.e_shstrndx].sh_size = SectionNames.size();
    ELFT::Shdr Strings{};
    Strings.sh_name = StringTableName;
    Strings.sh_type = SHT_STRTAB;
    Strings.sh_offset = StringsOffset;
    Strings.sh_size = SymbolNames.size();
    Strings.sh_addralign = 1;
    ELFT::Shdr SymbolTable{};
    SymbolTable.sh_name = SymbolTableName;
    SymbolTable.sh_type = SHT_SYMTAB;
    SymbolTable.sh_offset = SymbolsOffset;
    SymbolTable.sh_size = Symbols.size() * sizeof(ELFT::Sym);
    SymbolTable.sh_link = static_cast<uint32_t>(Sections.size());
    SymbolTable.sh_info = 1;
    SymbolTable.sh_addralign = kELF64RecordAlignment;
    SymbolTable.sh_entsize = sizeof(ELFT::Sym);
    Sections.push_back(Strings);
    Sections.push_back(SymbolTable);
    Header.e_shoff = HeadersOffset;
    Header.e_shnum = static_cast<uint16_t>(Sections.size());
    Bytes.resize(NamesOffset);
    Bytes.resize(HeadersOffset + Sections.size() * sizeof(ELFT::Shdr), 0);
    std::memcpy(Bytes.data(), &Header, sizeof(Header));
    std::memcpy(Bytes.data() + NamesOffset, SectionNames.data(),
                SectionNames.size());
    std::memcpy(Bytes.data() + StringsOffset, SymbolNames.data(),
                SymbolNames.size());
    std::memcpy(Bytes.data() + SymbolsOffset, Symbols.data(),
                Symbols.size() * sizeof(ELFT::Sym));
    std::memcpy(Bytes.data() + HeadersOffset, Sections.data(),
                Sections.size() * sizeof(ELFT::Shdr));
  }

  ELFT::Ehdr Header;
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  ELFT::Shdr SymbolTable;
  std::memcpy(&SymbolTable,
              Bytes.data() + Header.e_shoff +
                  (Header.e_shnum - 1) * sizeof(ELFT::Shdr),
              sizeof(SymbolTable));
  for (size_t Index = 0; Index < Records.size(); ++Index) {
    const uint64_t Offset =
        SymbolTable.sh_offset + (Index + 1) * sizeof(ELFT::Sym);
    ELFT::Sym Symbol;
    std::memcpy(&Symbol, Bytes.data() + Offset, sizeof(Symbol));
    Symbol.setBindingAndType(STB_GLOBAL, Records[Index].Type);
    Symbol.st_shndx = Strict ? SHN_ABS : 1;
    Symbol.st_value = (Strict ? sbf::kBytecodeStart : 0) +
                      Records[Index].EntrySlot * sbf::kInstructionSize;
    Symbol.st_size = Records[Index].Size;
    std::memcpy(Bytes.data() + Offset, &Symbol, sizeof(Symbol));
  }
  return Bytes;
}

class SBFLoaderTest : public ::testing::Test {
protected:
  void SetUp() override {
    llvm::SmallString<128> UniqueDirectory;
    const std::error_code Error = llvm::sys::fs::createUniqueDirectory(
        "neverd-sbf-loader", UniqueDirectory);
    ASSERT_FALSE(Error) << Error.message();
    Directory = UniqueDirectory.c_str();
  }

  void TearDown() override {
    std::error_code Error;
    std::filesystem::remove_all(Directory, Error);
  }

  std::filesystem::path write(llvm::StringRef Name,
                              llvm::ArrayRef<uint8_t> Contents) const {
    const auto Path = Directory / Name.str();
    std::ofstream Output(Path, std::ios::binary);
    Output.write(reinterpret_cast<const char *>(Contents.data()),
                 static_cast<std::streamsize>(Contents.size()));
    return Path;
  }

  std::filesystem::path Directory;
};

TEST_F(SBFLoaderTest, RejectsWritableLegacyDataLikeTheOfficialVM) {
  sbf::test::LegacyELFOptions Options;
  Options.AddWritableData = true;
  const auto Path =
      write("writable-data.so", sbf::test::buildLegacyELF(Options));
  auto Image = loadBinary(Path);
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Error = llvm::toString(Image.takeError());
  EXPECT_NE(Error.find("writable"), std::string::npos) << Error;
  EXPECT_NE(Error.find(".data"), std::string::npos) << Error;
}

TEST_F(SBFLoaderTest, MapsLegacyTextByNameWithoutSHFAlloc) {
  sbf::test::LegacyELFOptions Options;
  Options.TextIsAllocatable = false;

  auto Image =
      loadBinary(write("nonalloc-text.so", sbf::test::buildLegacyELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const Section *Text = Image->getSectionByName(sbf::kTextSectionName);
  ASSERT_NE(Text, nullptr);
  EXPECT_EQ(Text->Data.size(), sbf::kInstructionSize);
}

TEST_F(SBFLoaderTest, PreservesLogicalNoBitsTextWithoutInventingBytes) {
  sbf::test::LegacyELFOptions Options;
  Options.TextIsNoBits = true;

  auto Image =
      loadBinary(write("nobits-text.so", sbf::test::buildLegacyELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const Section *Text = Image->getSectionByName(sbf::kTextSectionName);
  ASSERT_NE(Text, nullptr);
  EXPECT_TRUE(Text->Data.empty());
  EXPECT_EQ(Text->FileSz, 0u);
  EXPECT_EQ(Text->Size, sbf::kInstructionSize);
  ASSERT_TRUE(Image->SBF.has_value());
  EXPECT_EQ(Image->SBF->TextFile.Size, 0u);
  EXPECT_EQ(Image->SBF->TextVM.Size, sbf::kInstructionSize);
}

TEST_F(SBFLoaderTest, PreservesAFileBackedPartialInstructionTail) {
  sbf::test::LegacyELFOptions Options;
  Options.Text.resize(sbf::kInstructionSize + 1);

  auto Image =
      loadBinary(write("partial-text.so", sbf::test::buildLegacyELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const Section *Text = Image->getSectionByName(sbf::kTextSectionName);
  ASSERT_NE(Text, nullptr);
  EXPECT_EQ(Text->Data.size(), Options.Text.size());
  ASSERT_TRUE(Image->SBF.has_value());
  EXPECT_EQ(Image->SBF->TextFile.Size, Options.Text.size());
  EXPECT_EQ(Image->SBF->TextVM.Size, Options.Text.size());
}

TEST_F(SBFLoaderTest, IgnoresUnknownAllocatableLegacySections) {
  constexpr llvm::StringLiteral UnknownSectionName(".mystery");
  for (bool IsNoBits : {false, true}) {
    SCOPED_TRACE(IsNoBits ? "nobits" : "progbits");
    sbf::test::LegacyELFOptions Options;
    Options.AddReadOnlyData = true;
    Options.DataSectionName = UnknownSectionName.str();
    Options.DataIsNoBits = IsNoBits;

    auto Image =
        loadBinary(write(IsNoBits ? "unknown-nobits.so" : "unknown-progbits.so",
                         sbf::test::buildLegacyELF(Options)));
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(Image->getSectionByName(UnknownSectionName), nullptr);
    ASSERT_TRUE(Image->SBF.has_value());
    EXPECT_EQ(Image->SBF->LegacyReadOnlySections.size(), 1u);
  }
}

TEST_F(SBFLoaderTest, RejectsLegacySectionNameAtOfficialByteLimit) {
  sbf::test::LegacyELFOptions Options;
  Options.DataSectionName = std::string(sbf::kELFSectionNameByteLimit, 'x');

  auto Image = loadBinary(
      write("long-section-name.so", sbf::test::buildLegacyELF(Options)));
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Error = llvm::toString(Image.takeError());
  EXPECT_NE(Error.find("section name"), std::string::npos) << Error;
  EXPECT_NE(Error.find("byte limit"), std::string::npos) << Error;
}

TEST_F(SBFLoaderTest, AcceptsLegacySectionNameBelowOfficialByteLimit) {
  sbf::test::LegacyELFOptions Options;
  Options.DataSectionName = std::string(sbf::kELFSectionNameByteLimit - 1, 'x');

  auto Image = loadBinary(
      write("bounded-section-name.so", sbf::test::buildLegacyELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
}

TEST_F(SBFLoaderTest, RejectsNonNullLegacySectionZero) {
  using ELFT = llvm::object::ELF64LE;
  auto Bytes = sbf::test::buildLegacyELF();
  ELFT::Ehdr Header;
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  ELFT::Shdr SectionZero;
  std::memcpy(&SectionZero, Bytes.data() + Header.e_shoff, sizeof(SectionZero));
  SectionZero.sh_type = llvm::ELF::SHT_PROGBITS;
  std::memcpy(Bytes.data() + Header.e_shoff, &SectionZero, sizeof(SectionZero));

  auto Image = loadBinary(write("non-null-section-zero.so", Bytes));
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Error = llvm::toString(Image.takeError());
  EXPECT_NE(Error.find("section zero"), std::string::npos) << Error;
}

TEST_F(SBFLoaderTest, RejectsInvalidLegacyELFHeaderRecordSizes) {
  using ELFT = llvm::object::ELF64LE;
  auto Bytes = sbf::test::buildLegacyELF();
  ELFT::Ehdr Header;
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  Header.e_ehsize = 0;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));

  auto Image = loadBinary(write("invalid-header-record-size.so", Bytes));
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Error = llvm::toString(Image.takeError());
  EXPECT_NE(Error.find("header layout"), std::string::npos) << Error;
}

TEST_F(SBFLoaderTest, RejectsDescendingLegacyLoadProgramHeaders) {
  using ELFT = llvm::object::ELF64LE;
  sbf::test::LegacyDynamicELFOptions Options;
  Options.EmitDynamicRel = false;
  auto Bytes = sbf::test::buildLegacyDynamicELF(Options);
  ELFT::Ehdr Header;
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  std::array<ELFT::Phdr, 2> ProgramHeaders;
  std::memcpy(ProgramHeaders.data(), Bytes.data() + Header.e_phoff,
              sizeof(ProgramHeaders));
  ProgramHeaders[0].p_vaddr = 1;
  ProgramHeaders[0].p_paddr = 1;
  ProgramHeaders[1].p_type = llvm::ELF::PT_LOAD;
  ProgramHeaders[1].p_vaddr = 0;
  ProgramHeaders[1].p_paddr = 0;
  std::memcpy(Bytes.data() + Header.e_phoff, ProgramHeaders.data(),
              sizeof(ProgramHeaders));

  auto Image = loadBinary(write("descending-load-phdrs.so", Bytes));
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Error = llvm::toString(Image.takeError());
  EXPECT_NE(Error.find("program header"), std::string::npos) << Error;
  EXPECT_NE(Error.find("ascending"), std::string::npos) << Error;
}

TEST_F(SBFLoaderTest, RejectsOutOfBoundsLegacyLoadProgramHeaders) {
  using ELFT = llvm::object::ELF64LE;
  sbf::test::LegacyDynamicELFOptions Options;
  Options.EmitDynamicRel = false;
  auto Bytes = sbf::test::buildLegacyDynamicELF(Options);
  ELFT::Ehdr Header;
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  ELFT::Phdr LoadHeader;
  std::memcpy(&LoadHeader, Bytes.data() + Header.e_phoff, sizeof(LoadHeader));
  LoadHeader.p_offset = Bytes.size();
  LoadHeader.p_filesz = 1;
  std::memcpy(Bytes.data() + Header.e_phoff, &LoadHeader, sizeof(LoadHeader));

  auto Image = loadBinary(write("out-of-bounds-load-phdr.so", Bytes));
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Error = llvm::toString(Image.takeError());
  EXPECT_NE(Error.find("PT_LOAD"), std::string::npos) << Error;
  EXPECT_NE(Error.find("file bounds"), std::string::npos) << Error;
}

TEST_F(SBFLoaderTest, RejectsLegacyHeaderTableOverlap) {
  using ELFT = llvm::object::ELF64LE;
  auto Bytes = sbf::test::buildLegacyELF();
  ELFT::Ehdr Header;
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  Header.e_phoff = 0;
  Header.e_phnum = 1;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));

  auto Image = loadBinary(write("overlapping-header-tables.so", Bytes));
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Error = llvm::toString(Image.takeError());
  EXPECT_NE(Error.find("header tables overlap"), std::string::npos) << Error;
}

TEST_F(SBFLoaderTest, RejectsLegacySectionOverlapAndFileDisorder) {
  using ELFT = llvm::object::ELF64LE;
  auto BuildMutation = []() {
    auto Bytes = sbf::test::buildLegacyELF();
    ELFT::Ehdr Header;
    std::memcpy(&Header, Bytes.data(), sizeof(Header));
    const uint64_t SectionOffset =
        Header.e_shoff + Header.e_shstrndx * sizeof(ELFT::Shdr);
    ELFT::Shdr Section;
    std::memcpy(&Section, Bytes.data() + SectionOffset, sizeof(Section));
    return std::tuple(std::move(Bytes), SectionOffset, Section);
  };

  auto [OverlappingBytes, OverlappingOffset, OverlappingSection] =
      BuildMutation();
  OverlappingSection.sh_offset = 0;
  OverlappingSection.sh_size = 1;
  std::memcpy(OverlappingBytes.data() + OverlappingOffset, &OverlappingSection,
              sizeof(OverlappingSection));
  auto OverlappingImage =
      loadBinary(write("section-overlaps-header.so", OverlappingBytes));
  ASSERT_FALSE(static_cast<bool>(OverlappingImage));
  const std::string OverlapError = llvm::toString(OverlappingImage.takeError());
  EXPECT_NE(OverlapError.find("overlaps a header"), std::string::npos)
      << OverlapError;

  auto [DisorderedBytes, DisorderedOffset, DisorderedSection] = BuildMutation();
  DisorderedSection.sh_offset = sizeof(ELFT::Ehdr);
  DisorderedSection.sh_size = 0;
  std::memcpy(DisorderedBytes.data() + DisorderedOffset, &DisorderedSection,
              sizeof(DisorderedSection));
  auto DisorderedImage =
      loadBinary(write("sections-out-of-order.so", DisorderedBytes));
  ASSERT_FALSE(static_cast<bool>(DisorderedImage));
  const std::string DisorderError = llvm::toString(DisorderedImage.takeError());
  EXPECT_NE(DisorderError.find("file order"), std::string::npos)
      << DisorderError;
}

TEST_F(SBFLoaderTest, RejectsDuplicateLegacyMetadataSectionNames) {
  using Duplicate = sbf::test::LegacyDuplicateMetadataSection;
  constexpr std::array Duplicates = {Duplicate::Symtab, Duplicate::Strtab,
                                     Duplicate::Dynstr};
  for (const Duplicate Value : Duplicates) {
    SCOPED_TRACE(static_cast<unsigned>(Value));
    sbf::test::LegacyDynamicELFOptions Options;
    Options.EmitDynamicRel = false;
    Options.DuplicateMetadataSection = Value;

    auto Image = loadBinary(write("duplicate-metadata-name.so",
                                  sbf::test::buildLegacyDynamicELF(Options)));
    ASSERT_FALSE(static_cast<bool>(Image));
    const std::string Error = llvm::toString(Image.takeError());
    EXPECT_NE(Error.find("multiple"), std::string::npos) << Error;
  }
}

TEST_F(SBFLoaderTest, IgnoresMalformedLegacyStaticSymbolTable) {
  sbf::test::LegacyDynamicELFOptions Options;
  Options.EmitDynamicRel = false;
  Options.AddSameNamedStaticFunction = true;
  Options.MalformStaticSymbolTableSize = true;

  auto Image = loadBinary(write("malformed-debug-symtab.so",
                                sbf::test::buildLegacyDynamicELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->findSymbol(Options.DynamicSymbolName), nullptr);
  ASSERT_TRUE(Image->SBF.has_value());
  EXPECT_EQ(Image->SBF->DebugEnrichment, sbf::DebugEnrichmentStatus::Malformed);
}

TEST_F(SBFLoaderTest, AppliesTheOfficialLegacyDebugSymbolNameLimit) {
  const std::string RetainedName(sbf::kRelocationSymbolNameByteLimit + 1, 'r');
  sbf::test::LegacyDynamicELFOptions Retained;
  Retained.EmitDynamicRel = false;
  Retained.AddSameNamedStaticFunction = true;
  Retained.StaticSymbolName = RetainedName;
  auto RetainedImage = loadBinary(write(
      "long-debug-symbol.so", sbf::test::buildLegacyDynamicELF(Retained)));
  ASSERT_TRUE(static_cast<bool>(RetainedImage))
      << llvm::toString(RetainedImage.takeError());
  EXPECT_NE(RetainedImage->findSymbol(RetainedName), nullptr);
  ASSERT_TRUE(RetainedImage->SBF.has_value());
  EXPECT_EQ(RetainedImage->SBF->DebugEnrichment,
            sbf::DebugEnrichmentStatus::Complete);

  const std::string SkippedName(sbf::kELFDebugSymbolNameByteLimit, 's');
  sbf::test::LegacyDynamicELFOptions Skipped = Retained;
  Skipped.StaticSymbolName = SkippedName;
  auto SkippedImage = loadBinary(write(
      "overlong-debug-symbol.so", sbf::test::buildLegacyDynamicELF(Skipped)));
  ASSERT_TRUE(static_cast<bool>(SkippedImage))
      << llvm::toString(SkippedImage.takeError());
  EXPECT_EQ(SkippedImage->findSymbol(SkippedName), nullptr);
  ASSERT_TRUE(SkippedImage->SBF.has_value());
  EXPECT_EQ(SkippedImage->SBF->DebugEnrichment,
            sbf::DebugEnrichmentStatus::Malformed);
}

TEST_F(SBFLoaderTest, AcceptsLegacyTextImmediatelyBeforeRegionBoundary) {
  sbf::test::LegacyELFOptions Options;
  Options.VirtualAddressBase = sbf::kMemoryRegionSize - sbf::kInstructionSize;
  auto Image = loadBinary(
      write("before-boundary-legacy.so", sbf::test::buildLegacyELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_TRUE(Image->SBF.has_value());
  const va_t ExpectedAddress = sbf::kStackStart - sbf::kInstructionSize;
  EXPECT_EQ(Image->Entry, ExpectedAddress);
  EXPECT_EQ(Image->Base, ExpectedAddress);
  EXPECT_EQ(Image->SBF->TextVM.Address, ExpectedAddress);
  ASSERT_NE(Image->getTextSection(), nullptr);
  EXPECT_EQ(Image->getTextSection()->VA, ExpectedAddress);
}

TEST_F(SBFLoaderTest, RejectsLegacyTextAtBytecodeRegionBoundary) {
  sbf::test::LegacyELFOptions Options;
  Options.VirtualAddressBase = sbf::kBytecodeStart;
  auto Image = loadBinary(
      write("boundary-legacy.so", sbf::test::buildLegacyELF(Options)));
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Error = llvm::toString(Image.takeError());
  EXPECT_NE(Error.find("virtual address"), std::string::npos) << Error;
}

TEST_F(SBFLoaderTest, RejectsLegacyTextCrossingBytecodeRegionBoundary) {
  sbf::test::LegacyELFOptions Options;
  Options.VirtualAddressBase = sbf::kMemoryRegionSize - sbf::kInstructionSize;
  Options.Text.resize(sbf::kInstructionSize + sbf::kInstructionSize);
  auto Image = loadBinary(
      write("crossing-boundary-legacy.so", sbf::test::buildLegacyELF(Options)));
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Error = llvm::toString(Image.takeError());
  EXPECT_NE(Error.find("virtual address"), std::string::npos) << Error;
}

TEST_F(SBFLoaderTest, LoadsSectionlessStrictV3AndV4Programs) {
  for (sbf::Version TheVersion : {sbf::Version::V3, sbf::Version::V4}) {
    SCOPED_TRACE(sbf::versionName(TheVersion).str());
    sbf::test::StrictELFOptions Options;
    Options.TheVersion = TheVersion;
    Options.AddRodata = true;
    const auto Path = write("strict.so", sbf::test::buildStrictELF(Options));
    auto Image = loadBinary(Path);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(Image->Arch, Arch::SBF);
    EXPECT_EQ(Image->Format, BinaryFormat::ELF);
    ASSERT_TRUE(Image->SBF.has_value());
    EXPECT_EQ(Image->SBF->Version, TheVersion);
    EXPECT_TRUE(Image->SBF->StrictLayout);
    EXPECT_EQ(Image->SBF->DebugEnrichment,
              sbf::DebugEnrichmentStatus::Unavailable);
    EXPECT_EQ(Image->SBF->TextVM.Address, sbf::kBytecodeStart);
    EXPECT_EQ(Image->SBF->RodataVM.Address, sbf::kRodataStartV3);
    ASSERT_NE(Image->getTextSection(), nullptr);
    EXPECT_EQ(Image->getTextSection()->Data.size(), sbf::kInstructionSize);
    ASSERT_EQ(Image->getFunctionSymbols().size(), 1u);
    EXPECT_EQ(Image->getFunctionSymbols()[0]->Name, sbf::kEntrySymbolName);
  }
}

TEST_F(SBFLoaderTest, EnrichesStrictProgramsFromValidOptionalSymbols) {
  sbf::test::StrictELFOptions Options;
  Options.AddDebugSymbols = true;
  auto Image =
      loadBinary(write("strict-debug.so", sbf::test::buildStrictELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_TRUE(Image->SBF.has_value());
  EXPECT_EQ(Image->SBF->DebugEnrichment, sbf::DebugEnrichmentStatus::Complete);
  const Symbol *Entry =
      Image->findSymbol(sbf::test::kDefaultStrictDebugSymbolName);
  ASSERT_NE(Entry, nullptr);
  EXPECT_TRUE(Entry->IsFunc);
  EXPECT_EQ(Entry->Addr, sbf::kBytecodeStart);
}

TEST_F(SBFLoaderTest, DeduplicatesDebugSymbolsByNameAddressAndRawType) {
  using namespace llvm::ELF;
  const std::array<DebugSymbolRecord, 8> Records = {{
      {"named_entry", 0, 8},
      {"named_entry", 0, 8},
      {"entry_alias", 0, 8},
      {"named_entry", 1, 8},
      {"named_entry", 0, 8, STT_OBJECT},
      {"named_entry", 0, 8, STT_OBJECT},
      {"named_entry", 0, 8, STT_NOTYPE},
      {"named_entry", 0, 8, STT_GNU_IFUNC},
  }};
  for (bool Strict : {false, true}) {
    SCOPED_TRACE(Strict ? "strict" : "legacy");
    auto Image = loadBinary(
        write("debug-identities.so", buildDebugSymbolELF(Strict, Records)));
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    ASSERT_TRUE(Image->SBF.has_value());
    EXPECT_EQ(Image->SBF->DebugEnrichment,
              sbf::DebugEnrichmentStatus::Complete);
    EXPECT_EQ(Image->Symbols.size(), 6u);
    const auto Functions = Image->getFunctionSymbols();
    ASSERT_EQ(Functions.size(), 3u);
    EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                            [](const Symbol *Symbol) {
                              return Symbol->Name == "named_entry" &&
                                     Symbol->Addr == sbf::kBytecodeStart;
                            }),
              1);
    EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                            [](const Symbol *Symbol) {
                              return Symbol->Name == "named_entry" &&
                                     Symbol->Addr == sbf::kBytecodeStart +
                                                         sbf::kInstructionSize;
                            }),
              1);
    const Symbol *Alias = Image->findSymbol("entry_alias");
    ASSERT_NE(Alias, nullptr);
    EXPECT_TRUE(Alias->IsFunc);
    EXPECT_EQ(Alias->Addr, sbf::kBytecodeStart);
    // SBF still classifies only STT_FUNC as a function. In particular,
    // distinct non-function raw types must not collapse into one record.
    EXPECT_EQ(std::count_if(Image->Symbols.begin(), Image->Symbols.end(),
                            [](const Symbol &Symbol) {
                              return Symbol.Name == "named_entry" &&
                                     Symbol.Addr == sbf::kBytecodeStart &&
                                     !Symbol.IsFunc;
                            }),
              3);
    EXPECT_EQ(Image->findSymbol(sbf::kEntrySymbolName), nullptr);
  }
}

TEST_F(SBFLoaderTest,
       CompletesUnknownDebugSymbolSizeWithoutReplacingKnownSize) {
  const std::array<DebugSymbolRecord, 7> Records = {{
      {"unknown_then_known", 0, 0},
      {"unknown_then_known", 0, 16},
      {"unknown_then_known", 0, 8},
      {"unknown_then_known", 0, 0},
      {"known_then_unknown", 1, 8},
      {"known_then_unknown", 1, 0},
      {"known_then_unknown", 1, 16},
  }};
  for (bool Strict : {false, true}) {
    SCOPED_TRACE(Strict ? "strict" : "legacy");
    auto Image = loadBinary(
        write("debug-sizes.so", buildDebugSymbolELF(Strict, Records)));
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    ASSERT_EQ(Image->getFunctionSymbols().size(), 2u);
    const Symbol *Completed = Image->findSymbol("unknown_then_known");
    ASSERT_NE(Completed, nullptr);
    EXPECT_EQ(Completed->Size, 16u);
    const Symbol *Preserved = Image->findSymbol("known_then_unknown");
    ASSERT_NE(Preserved, nullptr);
    EXPECT_EQ(Preserved->Size, 8u);
  }
}

TEST_F(SBFLoaderTest, AddsSingleEntryUnlessDebugFunctionAlreadyCoversIt) {
  for (bool Strict : {false, true}) {
    SCOPED_TRACE(Strict ? "strict" : "legacy");
    for (bool CoversEntry : {false, true}) {
      SCOPED_TRACE(CoversEntry ? "named entry" : "synthetic entry");
      const size_t Slot = CoversEntry ? 0 : 1;
      const std::array<DebugSymbolRecord, 4> Records = {{
          {"named_function", Slot, 8},
          {"named_function", Slot, 8},
          {"entry_data", 0, 8, llvm::ELF::STT_OBJECT},
          {"entry_data", 0, 8, llvm::ELF::STT_OBJECT},
      }};
      auto Image = loadBinary(
          write("debug-entry.so", buildDebugSymbolELF(Strict, Records)));
      ASSERT_TRUE(static_cast<bool>(Image))
          << llvm::toString(Image.takeError());
      EXPECT_EQ(Image->Entry, sbf::kBytecodeStart);
      EXPECT_EQ(Image->Symbols.size(), CoversEntry ? 2u : 3u);
      EXPECT_EQ(Image->getFunctionSymbols().size(), CoversEntry ? 1u : 2u);
      const Symbol *Entry = Image->findSymbol(sbf::kEntrySymbolName);
      if (CoversEntry) {
        EXPECT_EQ(Entry, nullptr);
      } else {
        ASSERT_NE(Entry, nullptr);
        EXPECT_TRUE(Entry->IsFunc);
        EXPECT_EQ(Entry->Addr, Image->Entry);
      }
    }
  }
}

TEST_F(SBFLoaderTest, AppliesTheOfficialStrictDebugSymbolNameLimit) {
  const std::string RetainedName(sbf::kRelocationSymbolNameByteLimit + 1, 'r');
  sbf::test::StrictELFOptions Retained;
  Retained.AddDebugSymbols = true;
  Retained.DebugSymbolName = RetainedName;
  auto RetainedImage = loadBinary(write("long-strict-debug-symbol.so",
                                        sbf::test::buildStrictELF(Retained)));
  ASSERT_TRUE(static_cast<bool>(RetainedImage))
      << llvm::toString(RetainedImage.takeError());
  EXPECT_NE(RetainedImage->findSymbol(RetainedName), nullptr);

  const std::string SkippedName(sbf::kELFDebugSymbolNameByteLimit, 's');
  sbf::test::StrictELFOptions Skipped = Retained;
  Skipped.DebugSymbolName = SkippedName;
  auto SkippedImage = loadBinary(write("overlong-strict-debug-symbol.so",
                                       sbf::test::buildStrictELF(Skipped)));
  ASSERT_TRUE(static_cast<bool>(SkippedImage))
      << llvm::toString(SkippedImage.takeError());
  EXPECT_EQ(SkippedImage->findSymbol(SkippedName), nullptr);
}

TEST_F(SBFLoaderTest, IgnoresMalformedOptionalSectionsForStrictExecution) {
  using ELFT = llvm::object::ELF64LE;
  auto Bytes = sbf::test::buildStrictELF();
  ASSERT_GE(Bytes.size(), sizeof(ELFT::Ehdr));
  ELFT::Ehdr Header;
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  Header.e_shoff = Bytes.size() + sizeof(ELFT::Shdr);
  Header.e_shnum = 1;
  Header.e_shstrndx = llvm::ELF::SHN_UNDEF;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));

  auto Image = loadBinary(write("strict-malformed-debug.so", Bytes));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_TRUE(Image->SBF.has_value());
  EXPECT_EQ(Image->SBF->DebugEnrichment, sbf::DebugEnrichmentStatus::Malformed);
  ASSERT_EQ(Image->getFunctionSymbols().size(), 1u);
  EXPECT_EQ(Image->getFunctionSymbols()[0]->Name, sbf::kEntrySymbolName);
}

TEST_F(SBFLoaderTest, DoesNotTreatOptionalStrictRelocationsAsRuntimeInput) {
  using ELFT = llvm::object::ELF64LE;
  auto Bytes = sbf::test::buildStrictELF({.AddDebugSymbols = true});
  ELFT::Ehdr Header;
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  ASSERT_EQ(Header.e_shnum, 4u);
  const size_t LastSectionOffset =
      static_cast<size_t>(Header.e_shoff + 3 * sizeof(ELFT::Shdr));
  ELFT::Shdr LastSection;
  std::memcpy(&LastSection, Bytes.data() + LastSectionOffset,
              sizeof(LastSection));
  LastSection.sh_type = llvm::ELF::SHT_RELA;
  std::memcpy(Bytes.data() + LastSectionOffset, &LastSection,
              sizeof(LastSection));

  auto Image = loadBinary(write("strict-optional-relocation.so", Bytes));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_TRUE(Image->SBF.has_value());
  EXPECT_EQ(Image->SBF->DebugEnrichment, sbf::DebugEnrichmentStatus::Complete);
  EXPECT_TRUE(Image->Relocations.empty());
}

TEST_F(SBFLoaderTest, IgnoresMalformedUnusedLegacyRelocationSections) {
  sbf::test::LegacyDynamicELFOptions Options;
  Options.EmitDynamicRel = false;
  Options.AddMalformedUnusedRel = true;
  auto Image = loadBinary(write("unused-malformed-relocation.so",
                                sbf::test::buildLegacyDynamicELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_TRUE(Image->Relocations.empty());
}

TEST_F(SBFLoaderTest, LoadsProgramDynamicRelocationsWithRawProvenance) {
  constexpr llvm::StringLiteral DynamicName("runtime_call");
  sbf::test::LegacyDynamicELFOptions Options;
  Options.DynamicSymbolName = DynamicName.str();
  Options.Relocations.push_back({0, sbf::Relocation::Call32,
                                 sbf::test::kLegacyFixtureDynamicSymbolIndex});

  auto Image = loadBinary(write("program-dynamic-rel.so",
                                sbf::test::buildLegacyDynamicELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->Relocations.size(), 1u);
  const RelocationEntry &Relocation = Image->Relocations.front();
  ASSERT_TRUE(Relocation.ELF.has_value());
  const ELFRelocationProvenance &Provenance = *Relocation.ELF;
  EXPECT_EQ(Provenance.Source, ELFRelocationSource::ProgramDynamicTable);
  EXPECT_EQ(Provenance.Ordinal, 0u);
  EXPECT_NE(Provenance.TableVirtualAddress, 0u);
  EXPECT_EQ(Provenance.TableFileOffset, Provenance.TableVirtualAddress);
  ASSERT_NE(Image->getTextSection(), nullptr);
  EXPECT_EQ(Provenance.RawOffset, Image->getTextSection()->FileOff);
  EXPECT_EQ(Relocation.Address, Image->getTextSection()->VA);
  EXPECT_EQ(Relocation.Type, static_cast<uint32_t>(sbf::Relocation::Call32));
  EXPECT_EQ(Relocation.SymbolIndex,
            sbf::test::kLegacyFixtureDynamicSymbolIndex);
  using ELFT = llvm::object::ELF64LE;
  ELFT::Rel ExpectedRelocation{};
  ExpectedRelocation.setSymbolAndType(
      sbf::test::kLegacyFixtureDynamicSymbolIndex,
      static_cast<uint32_t>(sbf::Relocation::Call32), false);
  EXPECT_EQ(Provenance.RawInfo, ExpectedRelocation.getRInfo(false));
  EXPECT_EQ(Relocation.SymbolName, DynamicName);
  ASSERT_TRUE(Provenance.Symbol.has_value());
  ASSERT_TRUE(Provenance.Symbol->Name.has_value());
  EXPECT_EQ(*Provenance.Symbol->Name, DynamicName);
  EXPECT_EQ(Provenance.Symbol->NameOffset, 1u);
  EXPECT_EQ(Provenance.Symbol->SectionIndex, llvm::ELF::SHN_UNDEF);
}

TEST_F(SBFLoaderTest, PreservesRawNonCanonicalRelocationSites) {
  using ELFT = llvm::object::ELF64LE;
  constexpr uint64_t RawHeaderOffset = offsetof(ELFT::Ehdr, e_flags);
  sbf::test::LegacyDynamicRelocationSpec Spec;
  Spec.Type = sbf::Relocation::Relative64;
  Spec.SymbolIndex = 0;
  Spec.RawOffset = RawHeaderOffset;
  sbf::test::LegacyDynamicELFOptions Options;
  Options.Relocations.push_back(Spec);

  auto Image = loadBinary(write("raw-header-relocation.so",
                                sbf::test::buildLegacyDynamicELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->Relocations.size(), 1u);
  const RelocationEntry &Relocation = Image->Relocations.front();
  ASSERT_TRUE(Relocation.ELF.has_value());
  EXPECT_EQ(Relocation.ELF->RawOffset, RawHeaderOffset);
  ASSERT_NE(Image->getTextSection(), nullptr);
  EXPECT_FALSE(Relocation.ELF->RawOffset >= Image->getTextSection()->FileOff &&
               Relocation.ELF->RawOffset - Image->getTextSection()->FileOff <
                   Image->getTextSection()->FileSz);
}

TEST_F(SBFLoaderTest, FallsBackToSectionDynamicRelocations) {
  sbf::test::LegacyDynamicELFOptions Options;
  Options.DynamicSource = sbf::test::LegacyDynamicTableSource::SectionHeader;
  Options.Relocations.push_back({0, sbf::Relocation::Call32,
                                 sbf::test::kLegacyFixtureDynamicSymbolIndex});

  auto Image = loadBinary(write("section-dynamic-rel.so",
                                sbf::test::buildLegacyDynamicELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->Relocations.size(), 1u);
  ASSERT_TRUE(Image->Relocations.front().ELF.has_value());
  EXPECT_EQ(Image->Relocations.front().ELF->Source,
            ELFRelocationSource::SectionDynamicTable);
}

TEST_F(SBFLoaderTest, InvalidProgramDynamicPayloadFallsBackToSection) {
  sbf::test::LegacyDynamicELFOptions Options;
  Options.DynamicSource = sbf::test::LegacyDynamicTableSource::
      InvalidProgramHeaderWithSectionFallback;
  Options.Relocations.push_back({0, sbf::Relocation::Call32,
                                 sbf::test::kLegacyFixtureDynamicSymbolIndex});

  auto Image = loadBinary(write("invalid-program-valid-section-dynamic.so",
                                sbf::test::buildLegacyDynamicELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->Relocations.size(), 1u);
  ASSERT_TRUE(Image->Relocations.front().ELF.has_value());
  EXPECT_EQ(Image->Relocations.front().ELF->Source,
            ELFRelocationSource::SectionDynamicTable);
}

TEST_F(SBFLoaderTest, PreservesDynamicRelocationOrdinals) {
  sbf::test::LegacyDynamicELFOptions Options;
  Options.Relocations.push_back({0, sbf::Relocation::Call32,
                                 sbf::test::kLegacyFixtureDynamicSymbolIndex});
  Options.Relocations.push_back(
      {0, sbf::Relocation::Abs64, sbf::test::kLegacyFixtureDynamicSymbolIndex});

  auto Image = loadBinary(write("ordered-dynamic-rel.so",
                                sbf::test::buildLegacyDynamicELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->Relocations.size(), 2u);
  ASSERT_TRUE(Image->Relocations[0].ELF.has_value());
  ASSERT_TRUE(Image->Relocations[1].ELF.has_value());
  EXPECT_EQ(Image->Relocations[0].ELF->Ordinal, 0u);
  EXPECT_EQ(Image->Relocations[1].ELF->Ordinal, 1u);
}

TEST_F(SBFLoaderTest, RejectsMisalignedDynamicRelocationTable) {
  sbf::test::LegacyDynamicELFOptions Options;
  Options.MisalignDynamicRel = true;
  Options.Relocations.push_back({0, sbf::Relocation::Call32,
                                 sbf::test::kLegacyFixtureDynamicSymbolIndex});

  auto Image = loadBinary(write("misaligned-dynamic-rel.so",
                                sbf::test::buildLegacyDynamicELF(Options)));
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Error = llvm::toString(Image.takeError());
  EXPECT_NE(Error.find("DT_REL"), std::string::npos) << Error;
  EXPECT_NE(Error.find("misaligned"), std::string::npos) << Error;
}

TEST_F(SBFLoaderTest, ParsesRelocationsBeforeLegacyEntryAlignment) {
  using ELFT = llvm::object::ELF64LE;
  constexpr uint64_t EntryMisalignment = 1;
  sbf::test::LegacyDynamicELFOptions Options;
  Options.MisalignDynamicRel = true;
  Options.Relocations.push_back({0, sbf::Relocation::Call32,
                                 sbf::test::kLegacyFixtureDynamicSymbolIndex});
  std::vector<uint8_t> Bytes = sbf::test::buildLegacyDynamicELF(Options);
  ASSERT_GE(Bytes.size(), sizeof(ELFT::Ehdr));
  ELFT::Ehdr Header;
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  Header.e_entry = static_cast<uint64_t>(Header.e_entry) + EntryMisalignment;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));

  auto Image = loadBinary(write("relocation-before-entry.so", Bytes));
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Error = llvm::toString(Image.takeError());
  EXPECT_NE(Error.find("DT_REL"), std::string::npos) << Error;
  EXPECT_NE(Error.find("misaligned"), std::string::npos) << Error;
  EXPECT_EQ(Error.find("entry point"), std::string::npos) << Error;
}

TEST_F(SBFLoaderTest, IgnoresDynamicRelaTagsForLegacyExecution) {
  sbf::test::LegacyDynamicELFOptions Options;
  Options.EmitDynamicRel = false;
  Options.AddIgnoredDynamicRela = true;

  auto Image = loadBinary(write("ignored-dynamic-rela.so",
                                sbf::test::buildLegacyDynamicELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_TRUE(Image->Relocations.empty());
}

TEST_F(SBFLoaderTest, BindsRelocationsToTheExactDynamicSymbolRecord) {
  constexpr llvm::StringLiteral SharedName("same_name");
  constexpr uint64_t DynamicValue = 0x1234;
  sbf::test::LegacyDynamicELFOptions Options;
  Options.DynamicSymbolName = SharedName.str();
  Options.DynamicSymbolValue = DynamicValue;
  Options.DynamicSymbolType = llvm::ELF::STT_OBJECT;
  Options.DynamicSymbolSectionIndex = llvm::ELF::SHN_ABS;
  Options.AddSameNamedStaticFunction = true;
  Options.Relocations.push_back(
      {0, sbf::Relocation::Abs64, sbf::test::kLegacyFixtureDynamicSymbolIndex});

  auto Image = loadBinary(write("same-name-symbol-tables.so",
                                sbf::test::buildLegacyDynamicELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const Symbol *DebugSymbol = Image->findSymbol(SharedName);
  ASSERT_NE(DebugSymbol, nullptr);
  EXPECT_TRUE(DebugSymbol->IsFunc);
  ASSERT_EQ(Image->Relocations.size(), 1u);
  const RelocationEntry &Relocation = Image->Relocations.front();
  ASSERT_TRUE(Relocation.ELF.has_value());
  ASSERT_TRUE(Relocation.ELF->Symbol.has_value());
  EXPECT_EQ(Relocation.ELF->Symbol->Value, DynamicValue);
  using ELFT = llvm::object::ELF64LE;
  ELFT::Sym ExpectedDynamicSymbol{};
  ExpectedDynamicSymbol.setBindingAndType(llvm::ELF::STB_GLOBAL,
                                          llvm::ELF::STT_OBJECT);
  EXPECT_EQ(Relocation.ELF->Symbol->Info, ExpectedDynamicSymbol.st_info);
  EXPECT_EQ(Relocation.ELF->Symbol->Binding, llvm::ELF::STB_GLOBAL);
  EXPECT_EQ(Relocation.ELF->Symbol->Type, llvm::ELF::STT_OBJECT);
  EXPECT_EQ(Relocation.ELF->Symbol->SectionIndex, llvm::ELF::SHN_ABS);
  EXPECT_TRUE(Relocation.ELF->Symbol->isDefined());
  EXPECT_FALSE(Relocation.ELF->Symbol->isFunction());
  EXPECT_FALSE(Relocation.ELF->Symbol->Name.has_value());
}

TEST_F(SBFLoaderTest, UndefinedDynamicSymbolDoesNotBecomeStaticInternal) {
  constexpr llvm::StringLiteral SharedName("undefined_runtime_call");
  sbf::test::LegacyDynamicELFOptions Options;
  Options.DynamicSymbolName = SharedName.str();
  Options.DynamicSymbolType = llvm::ELF::STT_FUNC;
  Options.DynamicSymbolSectionIndex = llvm::ELF::SHN_UNDEF;
  Options.AddSameNamedStaticFunction = true;
  Options.Relocations.push_back({0, sbf::Relocation::Call32,
                                 sbf::test::kLegacyFixtureDynamicSymbolIndex});

  auto Image = loadBinary(write("undefined-dynamic-defined-static.so",
                                sbf::test::buildLegacyDynamicELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const Symbol *DebugSymbol = Image->findSymbol(SharedName);
  ASSERT_NE(DebugSymbol, nullptr);
  EXPECT_TRUE(DebugSymbol->IsFunc);
  ASSERT_EQ(Image->Relocations.size(), 1u);
  const RelocationEntry &Relocation = Image->Relocations.front();
  ASSERT_TRUE(Relocation.ELF.has_value());
  ASSERT_TRUE(Relocation.ELF->Symbol.has_value());
  EXPECT_EQ(Relocation.ELF->Symbol->SectionIndex, llvm::ELF::SHN_UNDEF);
  EXPECT_EQ(Relocation.ELF->Symbol->Value, 0u);
  using ELFT = llvm::object::ELF64LE;
  ELFT::Sym ExpectedDynamicSymbol{};
  ExpectedDynamicSymbol.setBindingAndType(llvm::ELF::STB_GLOBAL,
                                          llvm::ELF::STT_FUNC);
  EXPECT_EQ(Relocation.ELF->Symbol->Info, ExpectedDynamicSymbol.st_info);
  EXPECT_FALSE(Relocation.ELF->Symbol->isDefined());
  EXPECT_TRUE(Relocation.ELF->Symbol->isFunction());
}

TEST_F(SBFLoaderTest, EnforcesDynamicCallSymbolNameByteBoundary) {
  sbf::test::LegacyDynamicELFOptions Accepted;
  Accepted.DynamicSymbolName =
      std::string(sbf::kRelocationSymbolNameByteLimit - 1, 'a');
  Accepted.Relocations.push_back({0, sbf::Relocation::Call32,
                                  sbf::test::kLegacyFixtureDynamicSymbolIndex});
  auto AcceptedImage = loadBinary(
      write("max-dynamic-name.so", sbf::test::buildLegacyDynamicELF(Accepted)));
  ASSERT_TRUE(static_cast<bool>(AcceptedImage))
      << llvm::toString(AcceptedImage.takeError());

  sbf::test::LegacyDynamicELFOptions Rejected = Accepted;
  Rejected.DynamicSymbolName =
      std::string(sbf::kRelocationSymbolNameByteLimit, 'b');
  auto RejectedImage = loadBinary(write(
      "overlong-dynamic-name.so", sbf::test::buildLegacyDynamicELF(Rejected)));
  ASSERT_FALSE(static_cast<bool>(RejectedImage));
  const std::string Error = llvm::toString(RejectedImage.takeError());
  EXPECT_NE(Error.find("symbol name"), std::string::npos) << Error;
  EXPECT_NE(Error.find("byte relocation limit"), std::string::npos) << Error;
}

TEST_F(SBFLoaderTest, Abs64DoesNotRequireDynamicSymbolNameResolution) {
  sbf::test::LegacyDynamicELFOptions Options;
  Options.DynamicSymbolName =
      std::string(sbf::kRelocationSymbolNameByteLimit, 'a');
  Options.DynamicSymbolType = llvm::ELF::STT_OBJECT;
  Options.DynamicSymbolSectionIndex = llvm::ELF::SHN_ABS;
  Options.Relocations.push_back(
      {0, sbf::Relocation::Abs64, sbf::test::kLegacyFixtureDynamicSymbolIndex});

  auto Image = loadBinary(write("abs64-overlong-unused-name.so",
                                sbf::test::buildLegacyDynamicELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->Relocations.size(), 1u);
  ASSERT_TRUE(Image->Relocations.front().ELF.has_value());
  ASSERT_TRUE(Image->Relocations.front().ELF->Symbol.has_value());
  EXPECT_FALSE(Image->Relocations.front().ELF->Symbol->Name.has_value());
  EXPECT_TRUE(Image->Relocations.front().SymbolName.empty());
}

TEST_F(SBFLoaderTest, RejectsOutOfBoundsNonAllocatableLegacySections) {
  using ELFT = llvm::object::ELF64LE;
  auto Bytes = sbf::test::buildLegacyELF();
  ELFT::Ehdr Header;
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  constexpr size_t DataSectionIndex = 2;
  const size_t DataSectionOffset = static_cast<size_t>(
      Header.e_shoff + DataSectionIndex * sizeof(ELFT::Shdr));
  ASSERT_LE(DataSectionOffset + sizeof(ELFT::Shdr), Bytes.size());

  ELFT::Shdr DataSection;
  std::memcpy(&DataSection, Bytes.data() + DataSectionOffset,
              sizeof(DataSection));
  DataSection.sh_offset = Bytes.size();
  DataSection.sh_size = 1;
  std::memcpy(Bytes.data() + DataSectionOffset, &DataSection,
              sizeof(DataSection));

  auto Image = loadBinary(write("out-of-bounds-nonalloc.so", Bytes));
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Error = llvm::toString(Image.takeError());
  EXPECT_NE(Error.find("section"), std::string::npos) << Error;
  EXPECT_NE(Error.find("file bounds"), std::string::npos) << Error;
}

TEST_F(SBFLoaderTest, RejectsStrictEMSBPFButAcceptsItForLegacyVersions) {
  sbf::test::StrictELFOptions Strict;
  Strict.Machine = sbf::kELFMachineSBPF;
  auto StrictImage =
      loadBinary(write("strict-em-sbpf.so", sbf::test::buildStrictELF(Strict)));
  ASSERT_FALSE(static_cast<bool>(StrictImage));
  EXPECT_NE(llvm::toString(StrictImage.takeError()).find("EM_BPF"),
            std::string::npos);

  for (sbf::Version TheVersion :
       {sbf::Version::V0, sbf::Version::V1, sbf::Version::V2}) {
    SCOPED_TRACE(sbf::versionName(TheVersion).str());
    sbf::test::LegacyELFOptions Legacy;
    Legacy.TheVersion = TheVersion;
    Legacy.Machine = sbf::kELFMachineSBPF;
    auto Image = loadBinary(
        write("legacy-em-sbpf.so", sbf::test::buildLegacyELF(Legacy)));
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    ASSERT_TRUE(Image->SBF.has_value());
    EXPECT_EQ(Image->SBF->Version, TheVersion);
    EXPECT_FALSE(Image->SBF->StrictLayout);
  }
}

} // namespace
} // namespace neverd
