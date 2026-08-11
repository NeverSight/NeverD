//===- SBFLoaderTests.cpp - Solana SBF ELF loader tests -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SBFFixtureBuilder.h"
#include "gtest/gtest.h"

#include "neverd/Support/BinaryLoading.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"

#include <filesystem>
#include <fstream>

namespace neverd {
namespace {

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

TEST_F(SBFLoaderTest, DoesNotDoubleRebaseLegacyVirtualAddresses) {
  sbf::test::LegacyELFOptions Options;
  Options.VirtualAddressBase = sbf::kBytecodeStart;
  auto Image = loadBinary(
      write("pre-rooted-legacy.so", sbf::test::buildLegacyELF(Options)));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_TRUE(Image->SBF.has_value());
  EXPECT_FALSE(Image->SBF->StrictLayout);
  EXPECT_EQ(Image->Entry, sbf::kBytecodeStart);
  EXPECT_EQ(Image->Base, sbf::kBytecodeStart);
  EXPECT_EQ(Image->SBF->TextVM.Address, sbf::kBytecodeStart);
  ASSERT_NE(Image->getTextSection(), nullptr);
  EXPECT_EQ(Image->getTextSection()->VA, sbf::kBytecodeStart);
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
  const Symbol *Entry = Image->findSymbol("named_entry");
  ASSERT_NE(Entry, nullptr);
  EXPECT_TRUE(Entry->IsFunc);
  EXPECT_EQ(Entry->Addr, sbf::kBytecodeStart);
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

TEST_F(SBFLoaderTest, RejectsMalformedLegacyRelocationEntrySizes) {
  using ELFT = llvm::object::ELF64LE;
  using Elf_Rel = ELFT::Rel;
  auto Bytes = sbf::test::buildLegacyELF({.AddWritableData = true});
  ELFT::Ehdr Header;
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  constexpr size_t RelocationSectionIndex = 2;
  const size_t RelocationSectionOffset = static_cast<size_t>(
      Header.e_shoff + RelocationSectionIndex * sizeof(ELFT::Shdr));
  ASSERT_LE(RelocationSectionOffset + sizeof(ELFT::Shdr), Bytes.size());

  ELFT::Shdr RelocationSection;
  std::memcpy(&RelocationSection, Bytes.data() + RelocationSectionOffset,
              sizeof(RelocationSection));
  RelocationSection.sh_type = llvm::ELF::SHT_REL;
  RelocationSection.sh_flags = 0;
  RelocationSection.sh_size = sizeof(Elf_Rel);
  RelocationSection.sh_link = Header.e_shnum;
  RelocationSection.sh_info = 1;
  RelocationSection.sh_entsize = sizeof(Elf_Rel) - 1;
  std::memcpy(Bytes.data() + RelocationSectionOffset, &RelocationSection,
              sizeof(RelocationSection));

  auto Image = loadBinary(write("malformed-relocation-size.so", Bytes));
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Error = llvm::toString(Image.takeError());
  EXPECT_NE(Error.find("relocation"), std::string::npos) << Error;
  EXPECT_NE(Error.find("entry size"), std::string::npos) << Error;
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
