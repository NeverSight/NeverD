//===- SBFLoaderTests.cpp - Solana SBF ELF loader tests -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SBFFixtureBuilder.h"
#include "gtest/gtest.h"

#include "neverd/Support/BinaryLoading.h"

#include "llvm/Support/Error.h"

#include <filesystem>
#include <fstream>

namespace neverd {
namespace {

class SBFLoaderTest : public ::testing::Test {
protected:
  void SetUp() override {
    Directory = std::filesystem::temp_directory_path() /
                ("neverd-sbf-loader-" + std::to_string(++Sequence));
    std::filesystem::create_directories(Directory);
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

  inline static unsigned Sequence = 0;
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
    EXPECT_EQ(Image->SBF->TextVM.Address, sbf::kBytecodeStart);
    EXPECT_EQ(Image->SBF->RodataVM.Address, sbf::kRodataStartV3);
    ASSERT_NE(Image->getTextSection(), nullptr);
    EXPECT_EQ(Image->getTextSection()->Data.size(), sbf::kInstructionSize);
    ASSERT_EQ(Image->getFunctionSymbols().size(), 1u);
    EXPECT_EQ(Image->getFunctionSymbols()[0]->Name, sbf::kEntrySymbolName);
  }
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
