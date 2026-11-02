//===- WindowsExportNameTests.cpp - Original export symbol name bounds ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keeps PE export symbol limits separate from Windows module/path names.
//===----------------------------------------------------------------------===//

#include "../loader/PEHeaderFixture.h"
#include "../web/NativeFixture.h"
#include "WindowsNativeTestSupport.h"
#include "gtest/gtest.h"
#include "os/windows/process/WindowsProcessModules.h"

#include "neverd/loader/COFF/PEProgramExports.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace neverd::emulation {
namespace {
namespace win = windows_process;

std::string exportImage(GuestArchitecture ISA, size_t NameBytes) {
  const auto Machine = ISA == GuestArchitecture::X64
                           ? llvm::COFF::IMAGE_FILE_MACHINE_AMD64
                           : llvm::COFF::IMAGE_FILE_MACHINE_ARM64;
  auto Bytes = neverd::test::peHeaderImage(true, Machine);
  const uint64_t RawSize = llvm::alignTo(0x200 + NameBytes + 1, 0x200);
  Bytes.resize(0x200 + RawSize, '\0');
  auto Put = [&](uint64_t Offset, uint64_t Value, unsigned Width = 4) {
    neverd::web::test::nativePut(Bytes, Offset, Value, Width);
  };
  Put(0x98 + 56, llvm::alignTo(0x1000 + RawSize, 0x1000));
  Put(0x188 + 8, RawSize);
  Put(0x188 + 16, RawSize);
  Put(0x98 + 112, 0x1100);
  Put(0x98 + 116, 0x80);
  // One original named export of the owned leaf function at RVA 0x1000.
  Put(0x300 + 12, 0x1150);
  Put(0x300 + 16, 1);
  Put(0x300 + 20, 1);
  Put(0x300 + 24, 1);
  Put(0x300 + 28, 0x1140);
  Put(0x300 + 32, 0x1144);
  Put(0x300 + 36, 0x1148);
  Put(0x340, 0x1000);
  Put(0x344, 0x1200);
  Put(0x348, 0, 2);
  Bytes.replace(0x350, 11, "exports.exe");
  Bytes.replace(0x400, NameBytes, std::string(NameBytes, 'A'));
  if (ISA == GuestArchitecture::AArch64) {
    llvm::support::endian::write32le(Bytes.data() + 0x200,
                                     0x52800540); // mov w0,42
    llvm::support::endian::write32le(Bytes.data() + 0x204, 0xd65f03c0); // ret
  }
  return Bytes;
}

class WindowsExportNames : public testing::Test {
protected:
  std::filesystem::path Root;
  void SetUp() override {
    llvm::SmallString<128> Path;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-export-name", Path));
    Root = Path.str().str();
  }
  void TearDown() override {
    if (!Root.empty())
      std::filesystem::remove_all(Root);
  }
  std::filesystem::path write(const std::string &Bytes) {
    const auto Path = Root / "exports.exe";
    std::error_code Error;
    llvm::raw_fd_ostream Output(Path.string(), Error);
    EXPECT_FALSE(Error);
    Output << Bytes;
    Output.close();
    return Path;
  }
};

TEST_F(WindowsExportNames, UsesTheSharedSymbolLimitForOriginalImages) {
  for (auto ISA : {GuestArchitecture::X64, GuestArchitecture::AArch64})
    for (size_t Length : {size_t(255), size_t(256), size_t(372),
                          size_t(pe_export_defaults::StringBytes - 1)}) {
      SCOPED_TRACE(static_cast<unsigned>(ISA));
      SCOPED_TRACE(Length);
      const auto Bytes = exportImage(ISA, Length);
      auto Original = readPEProgramExports(llvm::ArrayRef(
          reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size()));
      ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
      ASSERT_EQ(Original->Entries.size(), 1u);
      ASSERT_EQ(Original->Entries.front().Names.size(), 1u);
      auto Image = win::loadImage(write(Bytes), process_defaults::Memory);
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      EXPECT_EQ(Image->Exports.Entries.front().Names,
                Original->Entries.front().Names);
      EXPECT_EQ(Image->Exports.Entries.front().Names.front().size(), Length);
    }
}

TEST_F(WindowsExportNames,
       RetainsSharedBoundsMalformedStringAndBudgetFailures) {
  for (auto ISA : {GuestArchitecture::X64, GuestArchitecture::AArch64}) {
    auto Excessive =
        win::loadImage(write(exportImage(ISA, pe_export_defaults::StringBytes)),
                       process_defaults::Memory);
    EXPECT_FALSE(bool(Excessive));
    if (!Excessive)
      llvm::consumeError(Excessive.takeError());
    auto Bytes = exportImage(ISA, 372);
    Bytes.replace(0x400, Bytes.size() - 0x400,
                  std::string(Bytes.size() - 0x400, 'A'));
    auto Unterminated = win::loadImage(write(Bytes), process_defaults::Memory);
    EXPECT_FALSE(bool(Unterminated));
    if (!Unterminated)
      llvm::consumeError(Unterminated.takeError());
    const auto Path = write(exportImage(ISA, 372));
    win::ImageReadBudget Budget{process_defaults::Memory,
                                process_defaults::Memory};
    Budget.Records = 1;
    auto Exhausted = win::loadProgramImage(Path, Budget, false);
    EXPECT_FALSE(bool(Exhausted));
    if (!Exhausted)
      llvm::consumeError(Exhausted.takeError());
  }
}

TEST_F(WindowsExportNames, NativeWindowsLoadsTheSameLongSymbolImage) {
#if !defined(_WIN32) || !defined(_M_X64)
  GTEST_SKIP() << "Native x64 Windows oracle requires an x64 Windows host";
#else
  auto Status = native_test::observeNativeProcess(
      write(exportImage(GuestArchitecture::X64, 372)), Root / "stdout",
      Root / "stderr", 10);
  ASSERT_TRUE(bool(Status)) << llvm::toString(Status.takeError());
  EXPECT_EQ(*Status, 42u);
#endif
}
} // namespace
} // namespace neverd::emulation
