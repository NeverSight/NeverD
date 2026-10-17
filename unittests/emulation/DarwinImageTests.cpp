//===- DarwinImageTests.cpp - Mach-O admission and raw thread entry -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinTestImage.h"
#include "gtest/gtest.h"
#include "os/darwin/process/DarwinProcess.h"

#include "neverd/loader/MachO/MachOExecutionImage.h"

namespace neverd::emulation {
namespace {
using darwin_test::Image;
using darwin_test::TemporaryImage;
using namespace llvm::MachO;
void rejects(const Image &Image) {
  TemporaryImage File(Image);
  auto Loaded =
      darwin_model::loadImage(File.path(), darwin_model::macOSProfile(), {});
  ASSERT_FALSE(bool(Loaded));
  llvm::consumeError(Loaded.takeError());
}
TEST(DarwinImage, OriginalBytesAndPageZeroHaveSeparateMappingOwnership) {
  for (bool X64 : {false, true}) {
    Image I(X64);
    TemporaryImage File(I);
    auto Loaded =
        darwin_model::loadImage(File.path(), darwin_model::macOSProfile(), {});
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    EXPECT_EQ(Loaded->Memory.MinimumAddress, 0x100000000ULL);
    EXPECT_EQ(Loaded->Memory.PageSize, X64 ? 4096u : 16384u);
    ASSERT_EQ(Loaded->Plan.Regions.size(), 1u);
    EXPECT_EQ(Loaded->Plan.MappedBytes, 16384u);
    EXPECT_EQ(Loaded->Plan.Entry, 0x100001000ULL);
    EXPECT_EQ(Loaded->Plan.Regions.front().Bytes, I.Bytes);
  }
}
TEST(DarwinImage, RawThreadEntryKeepsItsExistingMinimumBudget) {
  for (bool X64 : {false, true}) {
    Image I(X64);
    TemporaryImage File(I);
    ProcessOptions Options;
    Options.StackSize = 16384;
    auto Full = darwin_model::loadImage(File.path(),
                                        darwin_model::macOSProfile(), Options);
    ASSERT_TRUE(bool(Full)) << llvm::toString(Full.takeError());
    EXPECT_EQ(Full->MetadataBytes, 0u);
    EXPECT_FALSE(Full->HasTLSCallbacks);
    EXPECT_FALSE(Full->HasGlobalCallbacks);
    EXPECT_TRUE(Full->ExecutableSpans.empty());
    Options.MemoryLimit =
        Full->Plan.MappedBytes + Options.StackSize + Full->Memory.PageSize;
    auto Exact = darwin_model::loadImage(File.path(),
                                         darwin_model::macOSProfile(), Options);
    ASSERT_TRUE(bool(Exact)) << llvm::toString(Exact.takeError());
    --Options.MemoryLimit;
    auto Under = darwin_model::loadImage(File.path(),
                                         darwin_model::macOSProfile(), Options);
    ASSERT_FALSE(bool(Under));
    llvm::consumeError(Under.takeError());
  }
}
TEST(DarwinImage, FileBudgetIncludesBytesOutsideMappedSegments) {
  for (bool X64 : {false, true}) {
    SCOPED_TRACE(X64);
    ProcessOptions Options;
    Options.StackSize = 16384;
    Options.MemoryLimit = 65536;
    Image I(X64);
    I.Bytes.resize(Options.MemoryLimit, 0);
    {
      TemporaryImage File(I);
      auto Loaded = darwin_model::loadImage(
          File.path(), darwin_model::macOSProfile(), Options);
      ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
      EXPECT_EQ(Loaded->Plan.MappedBytes, 16384u);
    }
    I.Bytes.push_back(0);
    TemporaryImage File(I);
    auto Loaded = darwin_model::loadImage(
        File.path(), darwin_model::macOSProfile(), Options);
    ASSERT_FALSE(bool(Loaded));
    EXPECT_NE(llvm::toString(Loaded.takeError()).find("file byte limit"),
              std::string::npos);
  }
}
TEST(DarwinImage, FileReaderRejectsNonfilesAndEmbeddedNULPaths) {
  TemporaryImage File(Image{});
  auto Directory = loadMachOExecutionImage(File.path().parent_path());
  ASSERT_FALSE(bool(Directory));
  EXPECT_NE(llvm::toString(Directory.takeError()).find("regular file"),
            std::string::npos);
  auto NulPath = File.path().native();
  NulPath.push_back(0);
  NulPath += File.path().filename().native();
  auto Truncated = loadMachOExecutionImage(std::filesystem::path(NulPath));
  ASSERT_FALSE(bool(Truncated));
  EXPECT_NE(llvm::toString(Truncated.takeError()).find("invalid input path"),
            std::string::npos);
}
TEST(DarwinImage, RejectsAmbiguousPlatformsEntriesAndNoncanonicalThreadState) {
  Image Wrong;
  Wrong.u32(Wrong.Platform + 8, PLATFORM_IOS);
  rejects(Wrong);
  Image Duplicate;
  auto Added = Duplicate.command(LC_BUILD_VERSION, 24);
  Duplicate.u32(Added + 8, PLATFORM_MACOS);
  rejects(Duplicate);
  Image Both;
  Added = Both.command(LC_MAIN, 24);
  Both.u64(Added + 8, 4096);
  rejects(Both);
  Image State;
  State.u64(State.Thread + 16, 7);
  rejects(State);
  Image Count;
  Count.u32(Count.Thread + 12, 67);
  rejects(Count);
  Image Flags;
  Flags.u32(24, MH_ALLOW_STACK_EXECUTION);
  rejects(Flags);
}
TEST(DarwinImage, RejectsEncryptionFixupsSubtypesAndRequiredCommands) {
  Image Encrypted;
  auto Added = Encrypted.command(LC_ENCRYPTION_INFO_64, 24);
  Encrypted.u32(Added + 16, 1);
  rejects(Encrypted);
  Image Fixups;
  Added = Fixups.command(LC_DYLD_CHAINED_FIXUPS, 16);
  Fixups.u32(Added + 8, 8192);
  Fixups.u32(Added + 12, 16);
  rejects(Fixups);
  Image PAC;
  PAC.u32(8, CPU_SUBTYPE_ARM64E);
  rejects(PAC);
  Image Unknown;
  Unknown.command(LC_REQ_DYLD | 0x1234, 8);
  rejects(Unknown);
}
TEST(DarwinImage, IntelLibraryCapabilityBitDoesNotChangeTheGuestISA) {
  Image I(true);
  I.u32(8, 0x80000003u);
  TemporaryImage File(I);
  auto Loaded =
      darwin_model::loadImage(File.path(), darwin_model::macOSProfile(), {});
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ(Loaded->Architecture, GuestArchitecture::X64);
  I.u32(8, CPU_SUBTYPE_X86_64_H);
  rejects(I);
}
TEST(DarwinImage, RawExecutablesCannotClaimDylibIdentity) {
  for (bool X64 : {false, true}) {
    Image I(X64);
    const auto ID = I.command(LC_ID_DYLIB, 40);
    I.u32(ID + 8, 24);
    std::copy_n("example.dylib", 14, I.Bytes.data() + ID + 24);
    rejects(I);
  }
}
TEST(DarwinImage, UnusedProviderAddressesRemainAvailableToRawImages) {
  for (bool X64 : {false, true}) {
    Image I(X64);
    const uint64_t Base = darwin_model::value::LibSystemGate;
    I.u64(I.Text + 24, Base);
    I.u64(I.Thread + 16 + (X64 ? 128 : 256), Base + 4096);
    TemporaryImage File(I);
    auto R =
        darwin_model::loadImage(File.path(), darwin_model::macOSProfile(), {});
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Plan.Entry, Base + 4096);
    EXPECT_TRUE(R->Imports.empty());
    EXPECT_TRUE(R->Memory.Reserved.empty());
  }
}
TEST(DarwinImage,
     DynamicDependenciesAndThreadLocalSectionsRequireAnExplicitModel) {
  Image Dynamic;
  auto Added = Dynamic.command(LC_LOAD_DYLIB, 48);
  Dynamic.u32(Added + 8, 24);
  std::copy_n("libUnknown.dylib", 17, Dynamic.Bytes.data() + Added + 24);
  rejects(Dynamic);
  for (auto Type : {S_MOD_INIT_FUNC_POINTERS, S_THREAD_LOCAL_ZEROFILL}) {
    Image Section;
    Section.Bytes.resize(32768, 0);
    Added = Section.command(LC_SEGMENT_64, 152);
    std::copy_n("__DATA", 6, Section.Bytes.data() + Added + 8);
    Section.u64(Added + 24, 0x100004000ULL);
    Section.u64(Added + 32, 16384);
    Section.u64(Added + 40, 16384);
    Section.u64(Added + 48, 16384);
    Section.u32(Added + 56, 3);
    Section.u32(Added + 60, 3);
    Section.u32(Added + 64, 1);
    std::copy_n("__item", 6, Section.Bytes.data() + Added + 72);
    std::copy_n("__DATA", 6, Section.Bytes.data() + Added + 88);
    Section.u64(Added + 104, 0x100004000ULL);
    Section.u64(Added + 112, 8);
    Section.u32(Added + 120, 16384);
    Section.u32(Added + 124, 3);
    Section.u32(Added + 136, Type);
    rejects(Section);
  }
}
TEST(DarwinImage, RejectsRangeOverflowsOverlapAndInvalidPageZero) {
  Image Zero;
  Zero.u32(Zero.PageZero + 60, 1);
  rejects(Zero);
  Image Overlap;
  Overlap.u64(Overlap.Text + 24, 0xffffc000);
  rejects(Overlap);
  Image Overflow;
  Overflow.u64(Overflow.Text + 32, UINT64_MAX);
  rejects(Overflow);
  Image File;
  File.u64(File.Text + 48, File.Bytes.size() + 1);
  rejects(File);
  Image Misaligned;
  Misaligned.u64(Misaligned.Text + 24, 0x100001000);
  rejects(Misaligned);
}
TEST(DarwinImage, AllowsRoundedLinkeditTailWithoutRelaxingPageAlignment) {
  Image I;
  I.u64(I.Text + 32, 5000);
  I.u64(I.Text + 48, 5000);
  I.Bytes[5000] = 0xa5;
  I.Bytes.back() = 0x5a;
  TemporaryImage File(I);
  auto Loaded =
      darwin_model::loadImage(File.path(), darwin_model::macOSProfile(), {});
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  EXPECT_EQ(Loaded->Plan.MappedBytes, 16384u);
  EXPECT_EQ(Loaded->Memory.Maximum.front().Size, 16384u);
  EXPECT_EQ(Loaded->Plan.Regions.front().Bytes, I.Bytes);
}
TEST(DarwinImage, HeaderRequiresExecuteEvenWhenAnotherSegmentOwnsTheEntry) {
  Image I;
  I.Bytes.resize(32768, 0);
  const auto Code = I.command(LC_SEGMENT_64, 72);
  std::copy_n("__CODE", 6, I.Bytes.data() + Code + 8);
  I.u64(Code + 24, 0x100004000ULL);
  I.u64(Code + 32, 16384);
  I.u64(Code + 40, 16384);
  I.u64(Code + 48, 16384);
  I.u32(Code + 56, 5);
  I.u32(Code + 60, 5);
  I.u64(I.Thread + 16 + 256, 0x100005000ULL);
  std::copy_n(I.Bytes.data() + 4096, 12, I.Bytes.data() + 20480);
  {
    TemporaryImage File(I);
    auto Loaded =
        darwin_model::loadImage(File.path(), darwin_model::macOSProfile(), {});
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  }
  I.u32(I.Text + 60, 1);
  rejects(I);
}
TEST(DarwinImage, StackAlignmentBudgetAndStartupStringsAreValidated) {
  Image I;
  TemporaryImage File(I);
  for (unsigned Case = 0; Case != 4; ++Case) {
    ProcessOptions Options;
    if (Case == 0)
      Options.StackSize = 4096;
    else if (Case == 1)
      Options.MemoryLimit = Options.StackSize + 16384;
    else if (Case == 2)
      Options.Arguments = {std::string("a\0b", 3)};
    else
      Options.Environment = {std::string(Options.StackSize, 'x')};
    auto Result =
        emulateProcess(File.path(), ProcessProfile::MacOSMachO64, Options);
    ASSERT_FALSE(bool(Result)) << Case;
    llvm::consumeError(Result.takeError());
  }
}

} // namespace
} // namespace neverd::emulation
