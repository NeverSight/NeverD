//===- EVMLoaderTests.cpp - EVM BinaryImage loader tests ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Support/BinaryLoading.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Error.h"

#include <filesystem>
#include <fstream>

namespace neverd {
namespace {

class EVMLoaderTest : public ::testing::Test {
protected:
  void SetUp() override {
    Directory = std::filesystem::temp_directory_path() /
                ("neverd-evm-loader-" + std::to_string(++Sequence));
    std::filesystem::create_directories(Directory);
  }

  void TearDown() override {
    std::error_code EC;
    std::filesystem::remove_all(Directory, EC);
  }

  std::filesystem::path write(llvm::StringRef Name,
                              llvm::StringRef Contents) const {
    const auto Path = Directory / Name.str();
    std::ofstream Output(Path, std::ios::binary);
    Output.write(Contents.data(),
                 static_cast<std::streamsize>(Contents.size()));
    Output.close();
    return Path;
  }

  inline static unsigned Sequence = 0;
  std::filesystem::path Directory;
};

TEST_F(EVMLoaderTest, AutoDetectsAndBuildsAnEVMImage) {
  const auto Path = write("counter.evm", "0x6001600055");
  auto Image = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());

  EXPECT_EQ(Image->Arch, Arch::EVM);
  EXPECT_EQ(Image->Format, BinaryFormat::EVM);
  EXPECT_EQ(Image->Bits, Bitness::Bits256);
  EXPECT_EQ(Image->getPointerSize(), 32u);
  EXPECT_EQ(Image->getFormatName(), std::string("EVM"));
  EXPECT_EQ(Image->Entry, 0u);
  EXPECT_EQ(Image->Raw, (std::vector<uint8_t>{0x60, 0x01, 0x60, 0x00, 0x55}));

  ASSERT_EQ(Image->Segments.size(), 1u);
  EXPECT_EQ(Image->Segments[0].Name, "EVM_CODE");
  EXPECT_TRUE(Image->Segments[0].isReadable());
  EXPECT_TRUE(Image->Segments[0].isExecutable());
  ASSERT_NE(Image->getTextSection(), nullptr);
  EXPECT_EQ(Image->getTextSection()->Name, ".evm.code");
  ASSERT_EQ(Image->getFunctionSymbols().size(), 1u);
  EXPECT_EQ(Image->getFunctionSymbols()[0]->Name, "evm_entry");
}

TEST_F(EVMLoaderTest, RefusesUnknownTextFiles) {
  const auto Path = write("notes.txt", "hello, not bytecode");
  EXPECT_EQ(Loader::create(Path), nullptr);
}

TEST_F(EVMLoaderTest, DetectsValidatedHexWithoutAnExtension) {
  const auto Path = write("contract", "6001600055");
  auto Image = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Arch, Arch::EVM);
}

TEST_F(EVMLoaderTest, PreservesDetailedErrorsForExplicitEVMFiles) {
  const auto Path = write("broken.evm", "0x600");
  auto Image = loadBinary(Path);
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Error = llvm::toString(Image.takeError());
  EXPECT_NE(Error.find("odd number"), std::string::npos);
  EXPECT_EQ(Error.find("unknown binary format"), std::string::npos);
}

} // namespace
} // namespace neverd
