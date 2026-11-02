//===- FileBufferTests.cpp - Bounded binary file loading --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// File-size boundaries, exact byte identity and stopped file preparation.
///
//===----------------------------------------------------------------------===//

#include "../web/NativeFixture.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Loader.h"
#include "neverd/loader/Raw/RawLoader.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/support/LoadProgress.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SHA256.h"

#include <fstream>
#include <string>
#include <system_error>

namespace neverd {
namespace {

TEST(NativeFileBuffer, PageBoundariesPreserveTheEntireInput) {
  const size_t Boundary = 4 * llvm::sys::Process::getPageSizeEstimate();
  llvm::SmallString<128> Path;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("native-file", "bin", Path));
  llvm::FileRemover Remove(Path);
  for (const auto &Original :
       {web::test::nativeELF(), web::test::nativeELF(true),
        web::test::nativePE(), web::test::nativeMachO()}) {
    for (const auto Size : {Boundary - 1, Boundary, Boundary + 1}) {
      SCOPED_TRACE(Size);
      auto Bytes = Original;
      Bytes.resize(Size, '\x5a');
      std::ofstream File(Path.str().str(), std::ios::binary);
      File.write(Bytes.data(), Bytes.size());
      File.close();
      ASSERT_TRUE(File);
      const llvm::MemoryBufferRef Buffer(Bytes, "captured-native-input");
      auto Captured = loadBinaryBuffer(Buffer);
      ASSERT_TRUE(bool(Captured)) << llvm::toString(Captured.takeError());
      auto Loaded = loadBinary(Path.str().str());
      ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
      SCOPED_TRACE(static_cast<unsigned>(Loaded->Format));
      EXPECT_EQ(Loaded->InputFileSHA256,
                llvm::SHA256::hash(llvm::arrayRefFromStringRef(Bytes)));
      EXPECT_EQ(Loaded->Raw.size(), Bytes.size());
      EXPECT_EQ(Loaded->Raw, Captured->Raw);
      EXPECT_EQ(Loaded->Entry, Captured->Entry);
      EXPECT_EQ(Loaded->Arch, Captured->Arch);
      EXPECT_EQ(Loaded->Symbols.size(), Captured->Symbols.size());
      ASSERT_EQ(Loaded->Segments.size(), Captured->Segments.size());
      for (size_t I = 0; I < Loaded->Segments.size(); ++I) {
        EXPECT_EQ(Loaded->Segments[I].VA, Captured->Segments[I].VA);
        EXPECT_EQ(Loaded->Segments[I].Data, Captured->Segments[I].Data);
      }
    }
  }
}

TEST(NativeFileBuffer, StoppedReadReleasesTheFileAndCanRetry) {
  const size_t Size = 4 * llvm::sys::Process::getPageSizeEstimate();
  llvm::SmallString<128> Path;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("stopped-file", "bin", Path));
  llvm::FileRemover Remove(Path);
  for (auto Bytes : {web::test::nativeELF(), web::test::nativeELF(true),
                     web::test::nativePE(), web::test::nativeMachO()}) {
    Bytes.resize(Size, '\x5a');
    {
      std::ofstream File(Path.str().str(), std::ios::binary);
      File.write(Bytes.data(), Bytes.size());
      File.close();
      ASSERT_TRUE(File);
    }
    auto Reader = Loader::create(Path.str().str());
    ASSERT_TRUE(Reader);
    bool Stopped = false;
    LoadProgress Progress;
    Progress.User = &Stopped;
    Progress.Callback = [](void *User, const char *Phase,
                           unsigned long long Done, unsigned long long,
                           const char *) {
      if (llvm::StringRef(Phase) == "read" && Done == 1)
        *static_cast<bool *>(User) = true;
    };
    Progress.StopUser = &Stopped;
    Progress.Stop = [](void *User) -> int {
      return *static_cast<bool *>(User);
    };
    auto Cancelled = Reader->loadWithProgress(Path.str().str(), Progress);
    ASSERT_FALSE(bool(Cancelled));
    EXPECT_TRUE(Stopped);
    EXPECT_EQ(llvm::errorToErrorCode(Cancelled.takeError()),
              std::make_error_code(std::errc::operation_canceled));
    ASSERT_FALSE(llvm::sys::fs::remove(Path));
    {
      std::ofstream File(Path.str().str(), std::ios::binary);
      File.write(Bytes.data(), Bytes.size());
      File.close();
      ASSERT_TRUE(File);
    }
    auto Retried = Reader->load(Path.str().str());
    ASSERT_TRUE(bool(Retried)) << llvm::toString(Retried.takeError());
    EXPECT_EQ(Retried->InputFileSHA256,
              llvm::SHA256::hash(llvm::arrayRefFromStringRef(Bytes)));
    EXPECT_EQ(Retried->Raw.size(), Bytes.size());
  }
}

TEST(RawFileBuffer, SizeBoundariesKeepTheLastByteAndRejectEmptyInput) {
  const size_t Page = llvm::sys::Process::getPageSizeEstimate();
  llvm::SmallString<128> Path;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("raw-file", "bin", Path));
  llvm::FileRemover Remove(Path);
  for (const auto Size : {size_t(0), size_t(1), Page - 1, Page, Page + 1,
                          4 * Page - 1, 4 * Page, 4 * Page + 1}) {
    SCOPED_TRACE(Size);
    const std::string Bytes(Size, '\x5a');
    std::ofstream File(Path.str().str(), std::ios::binary);
    File.write(Bytes.data(), Bytes.size());
    File.close();
    ASSERT_TRUE(File);
    RawLoadOptions Options;
    Options.TheArch = Arch::X64;
    RawLoader Reader(Options);
    auto Loaded = Reader.load(Path.str().str());
    if (!Size) {
      ASSERT_FALSE(bool(Loaded));
      EXPECT_NE(llvm::toString(Loaded.takeError()).find("end of the file"),
                std::string::npos);
      continue;
    }
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    EXPECT_EQ(Loaded->InputFileSHA256,
              llvm::SHA256::hash(llvm::arrayRefFromStringRef(Bytes)));
    EXPECT_EQ(
        llvm::StringRef(reinterpret_cast<const char *>(Loaded->Raw.data()),
                        Loaded->Raw.size()),
        Bytes);
    ASSERT_EQ(Loaded->Segments.size(), 1U);
    EXPECT_EQ(Loaded->Segments.front().Data, Loaded->Raw);
    Options.Offset = Size - 1;
    Options.Size = 1;
    RawLoader TailReader(Options);
    auto Tail = TailReader.load(Path.str().str());
    ASSERT_TRUE(bool(Tail)) << llvm::toString(Tail.takeError());
    EXPECT_EQ(Tail->InputFileSHA256, Loaded->InputFileSHA256);
    EXPECT_EQ(Tail->Raw, Loaded->Raw);
    ASSERT_EQ(Tail->Segments.size(), 1U);
    ASSERT_EQ(Tail->Segments.front().Data.size(), 1U);
    EXPECT_EQ(Tail->Segments.front().Data.front(), 0x5a);
  }
}

} // namespace
} // namespace neverd
