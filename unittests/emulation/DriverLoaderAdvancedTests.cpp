//===- DriverLoaderAdvancedTests.cpp - PE loading contracts ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original PE fixtures exercise rebasing, cookie initialization, and rejected
/// runtime requirements through the loader and the actual emulation session.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "windows/DriverImage.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace neverd::emulation {
namespace {
std::filesystem::path loaderFixture(const char *Name = "loader") {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         (std::string(Name) + ".sys");
}

std::optional<uint64_t> imageInteger(const DriverImage &Image,
                                     uint64_t Address) {
  for (const auto &Region : Image.Regions) {
    if (Address < Region.Address ||
        Address - Region.Address > Region.Bytes.size() ||
        Region.Bytes.size() - (Address - Region.Address) < 8)
      continue;
    return llvm::support::endian::read64le(Region.Bytes.data() + Address -
                                           Region.Address);
  }
  return std::nullopt;
}

TEST(DriverAdvancedLoader, InitializesCookieBeforeCompilerProtectedEntry) {
  auto Image = loadDriverImage(loaderFixture(), 64 * 1024 * 1024);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_NE(Image->SecurityCookieAddress, 0u);
  EXPECT_EQ(imageInteger(*Image, Image->SecurityCookieAddress),
            DriverSecurityCookie);
  EXPECT_EQ(DriverSecurityCookie >> 48, 0u);
  EXPECT_NE(DriverSecurityCookie, 0x2b992ddfa232ULL);
  auto Result = emulateDriver(loaderFixture());
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus);
  EXPECT_EQ(*Result->NTStatus, 0u);
  EXPECT_GT(Result->Instructions, 20u);
}

TEST(DriverAdvancedLoader, RebasedWrapperRetainsAbsoluteDataReferences) {
  for (uint64_t Base :
       {0x140000000ULL, 0x280000000ULL, 0xfffff80010000000ULL}) {
    SCOPED_TRACE(Base);
    DriverOptions Options;
    Options.LoadAddress = Base;
    auto Image = loadDriverImage(loaderFixture(), Options.MemoryLimit, Base);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(Image->Base, Base);
    EXPECT_EQ(Image->PreferredBase, 0x180000000ULL);
    EXPECT_EQ(imageInteger(*Image, Image->SecurityCookieAddress),
              DriverSecurityCookie);
    auto Result = emulateDriver(loaderFixture(), Options);
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    ASSERT_TRUE(Result->NTStatus);
    EXPECT_EQ(*Result->NTStatus, 0u);
    EXPECT_EQ(Result->ImageBase, Base);
    EXPECT_GE(Result->Entry, Base);
  }
}

TEST(DriverAdvancedLoader, RefusesRebaseWhenRelocationsWereStripped) {
  auto Image = loadDriverImage(loaderFixture("success"), 64 * 1024 * 1024,
                               0x280000000ULL);
  ASSERT_FALSE(static_cast<bool>(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("requires unstripped DIR64"),
            std::string::npos);
}

TEST(DriverAdvancedLoader, RejectsExplicitReservedAndNoncanonicalAddresses) {
  for (uint64_t Base : {0x70000000ULL, 0x180000001ULL, 0x0000800000000000ULL}) {
    auto Image = loadDriverImage(loaderFixture(), 64 * 1024 * 1024, Base);
    ASSERT_FALSE(static_cast<bool>(Image));
    EXPECT_FALSE(llvm::toString(Image.takeError()).empty());
  }
}

class DriverAdvancedImageMutation : public ::testing::Test {
protected:
  std::vector<uint8_t> Bytes;
  std::unique_ptr<llvm::object::COFFObjectFile> Object;
  std::filesystem::path Directory;

  void SetUp() override {
    auto Buffer = llvm::MemoryBuffer::getFile(loaderFixture().string());
    ASSERT_TRUE(static_cast<bool>(Buffer));
    const auto Data = (*Buffer)->getBuffer();
    Bytes.assign(Data.bytes_begin(), Data.bytes_end());
    llvm::MemoryBufferRef Reference(
        llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                        Bytes.size()),
        "loader-test");
    auto Parsed = llvm::object::COFFObjectFile::create(Reference);
    ASSERT_TRUE(static_cast<bool>(Parsed))
        << llvm::toString(Parsed.takeError());
    Object = std::move(*Parsed);
    llvm::SmallString<128> Temporary;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-advanced-loader",
                                                      Temporary));
    Directory = Temporary.str().str();
  }
  void TearDown() override {
    std::error_code Ignored;
    std::filesystem::remove_all(Directory, Ignored);
  }
  size_t offset(const void *Pointer) const {
    return static_cast<const uint8_t *>(Pointer) - Bytes.data();
  }
  std::optional<size_t> rvaOffset(uint32_t RVA) {
    uintptr_t Pointer = 0;
    if (auto Error = Object->getRvaPtr(RVA, Pointer)) {
      llvm::consumeError(std::move(Error));
      return std::nullopt;
    }
    return offset(reinterpret_cast<const void *>(Pointer));
  }
  std::filesystem::path writeImage() {
    const auto Path = Directory / "modified.sys";
    std::ofstream Stream(Path, std::ios::binary);
    Stream.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Stream.close();
    EXPECT_TRUE(Stream);
    return Path;
  }
  void rejects(llvm::StringRef Diagnostic, uint64_t LoadAddress = 0) {
    auto Image = loadDriverImage(writeImage(), 64 * 1024 * 1024, LoadAddress);
    ASSERT_FALSE(static_cast<bool>(Image));
    const auto Message = llvm::toString(Image.takeError());
    EXPECT_NE(Message.find(Diagnostic.str()), std::string::npos) << Message;
  }
  std::optional<size_t> cookiePointerOffset() {
    const auto *Config =
        Object->getDataDirectory(llvm::COFF::LOAD_CONFIG_TABLE);
    if (!Config || !Config->Size)
      return std::nullopt;
    return rvaOffset(
        Config->RelativeVirtualAddress +
        offsetof(llvm::object::coff_load_configuration64, SecurityCookie));
  }
};

TEST_F(DriverAdvancedImageMutation, RejectsCookieOutsideImage) {
  const auto Offset = cookiePointerOffset();
  ASSERT_TRUE(Offset);
  llvm::support::endian::write64le(Bytes.data() + *Offset, 0x12345000);
  rejects("SecurityCookie");
}

TEST_F(DriverAdvancedImageMutation, RejectsCookieInExecutableStorage) {
  const auto Offset = cookiePointerOffset();
  ASSERT_TRUE(Offset);
  for (const auto &Reference : Object->sections()) {
    const auto *Section = Object->getCOFFSection(Reference);
    if (!(Section->Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE))
      continue;
    llvm::support::endian::write64le(Bytes.data() + *Offset,
                                     Object->getImageBase() +
                                         Section->VirtualAddress);
    rejects("writable nonexecutable");
    return;
  }
  FAIL() << "missing executable fixture section";
}

TEST_F(DriverAdvancedImageMutation,
       RejectsUnsupportedNonzeroLoadConfigRequirement) {
  const auto *Config = Object->getDataDirectory(llvm::COFF::LOAD_CONFIG_TABLE);
  ASSERT_NE(Config, nullptr);
  const auto Offset = rvaOffset(
      Config->RelativeVirtualAddress +
      offsetof(llvm::object::coff_load_configuration64, GlobalFlagsSet));
  ASSERT_TRUE(Offset);
  llvm::support::endian::write32le(Bytes.data() + *Offset, 1);
  rejects("GlobalFlagsSet");
}

TEST_F(DriverAdvancedImageMutation,
       RejectsTruncatedDeclaredLoadConfigBeforeLLVMUsesIt) {
  const auto *Config = Object->getDataDirectory(llvm::COFF::LOAD_CONFIG_TABLE);
  ASSERT_NE(Config, nullptr);
  const auto Offset = rvaOffset(Config->RelativeVirtualAddress);
  ASSERT_TRUE(Offset);
  llvm::support::endian::write32le(Bytes.data() + *Offset, UINT32_MAX);
  rejects("truncated declared load configuration");
}

TEST_F(DriverAdvancedImageMutation, RejectsDuplicateRelocationTarget) {
  const auto *Directory =
      Object->getDataDirectory(llvm::COFF::BASE_RELOCATION_TABLE);
  ASSERT_NE(Directory, nullptr);
  const auto Offset = rvaOffset(Directory->RelativeVirtualAddress);
  ASSERT_TRUE(Offset);
  const uint32_t BlockSize =
      llvm::support::endian::read32le(Bytes.data() + *Offset + 4);
  ASSERT_GE(BlockSize, 12u);
  const auto Entry =
      llvm::support::endian::read16le(Bytes.data() + *Offset + 8);
  ASSERT_EQ(Entry >> 12, llvm::COFF::IMAGE_REL_BASED_DIR64);
  llvm::support::endian::write16le(Bytes.data() + *Offset + 10, Entry);
  rejects("duplicate or overlapping");
}

TEST_F(DriverAdvancedImageMutation,
       RejectsMissingCookiePointerRelocationOnRebase) {
  const auto *Config = Object->getDataDirectory(llvm::COFF::LOAD_CONFIG_TABLE);
  const auto *Directory =
      Object->getDataDirectory(llvm::COFF::BASE_RELOCATION_TABLE);
  ASSERT_NE(Config, nullptr);
  ASSERT_NE(Directory, nullptr);
  const uint64_t Target =
      uint64_t(Config->RelativeVirtualAddress) +
      offsetof(llvm::object::coff_load_configuration64, SecurityCookie);
  const auto Start = rvaOffset(Directory->RelativeVirtualAddress);
  ASSERT_TRUE(Start);
  for (size_t Offset = *Start; Offset < *Start + Directory->Size;) {
    const uint32_t Page =
        llvm::support::endian::read32le(Bytes.data() + Offset);
    const uint32_t BlockSize =
        llvm::support::endian::read32le(Bytes.data() + Offset + 4);
    ASSERT_GE(BlockSize, 8u);
    for (size_t Index = 8; Index < BlockSize; Index += 2) {
      const uint16_t Entry =
          llvm::support::endian::read16le(Bytes.data() + Offset + Index);
      if ((Entry >> 12) != llvm::COFF::IMAGE_REL_BASED_DIR64 ||
          uint64_t(Page) + (Entry & 0xfff) != Target)
        continue;
      llvm::support::endian::write16le(Bytes.data() + Offset + Index, 0);
      rejects("requires its load configuration DIR64", 0x280000000ULL);
      return;
    }
    Offset += BlockSize;
  }
  FAIL() << "fixture must relocate its cookie pointer";
}
} // namespace
} // namespace neverd::emulation
