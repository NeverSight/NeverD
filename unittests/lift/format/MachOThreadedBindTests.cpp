//===- MachOThreadedBindTests.cpp - Original arm64e chain coverage --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercises independent threaded-bind bytes through both the utility and
/// public loader, including all-or-nothing failure and header-base identity.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/loader/MachO/MachOMetadataPointer.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <vector>

using namespace neverd;
using namespace llvm::MachO;

namespace {
#define NEVERD_THREADED_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define NEVERD_THREADED_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_THREADED_BYTES(Name, ...)                                       \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "MachOThreadedBindFixtures.def"

struct ThreadedImage {
  BinaryImage Image;
  std::vector<uint8_t> Bytes;
  macho_loader::DyldInfoOffsets Info;

  explicit ThreadedImage(llvm::ArrayRef<uint8_t> Stream = kOneImport) {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.MachOCPUSubtype = kSubtype;
    Image.Base = 0; // A __PAGEZERO-style minimum is not the Mach-header base.
    Image.DynInfo.NeededLibs = {kLibrary};
    Image.MachODylibReferences = {{kLibrary, false}};
    Segment S;
    S.Name = kDataName;
    S.VA = kData;
    S.Size = S.FileSz = kSegmentSize;
    S.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    S.Data.resize(S.Size);
    Image.Segments.push_back(S);
    Section Sec;
    Sec.Name = kSectionName;
    Sec.SegmentName = S.Name;
    Sec.VA = S.VA;
    Sec.Size = Sec.FileSz = S.Size;
    Sec.Flags = S.Flags;
    Image.Sections.push_back(Sec);
    stream(Stream);
  }

  void stream(llvm::ArrayRef<uint8_t> Stream) {
    Bytes.resize(kStreamOffset);
    Bytes.insert(Bytes.end(), Stream.begin(), Stream.end());
    Info.BindOff = kStreamOffset;
    Info.BindSize = Stream.size();
  }
  void word(size_t Offset, uint64_t Word) {
    writeLE<uint64_t>(Image.Segments[0].Data.data() + Offset, Word);
  }
  void parse(va_t Base = kBase) {
    macho_loader::parseBindStreams(Bytes.data(), Bytes.size(), Info, Image,
                                   Base);
  }
  void expectFailure() {
    const auto Original = Image.Segments[0].Data;
    parse();
    EXPECT_NE(Image.MachOChainedFixupsError.find(kError), std::string::npos);
    EXPECT_TRUE(Image.MachOAuthenticatedPointers.empty());
    EXPECT_TRUE(Image.MachOResolvedChainedPointerSlots.empty());
    EXPECT_TRUE(Image.DyldBindSlots.empty());
    EXPECT_TRUE(Image.ImportStorageSlots.empty());
    EXPECT_TRUE(Image.Imports.empty());
    EXPECT_EQ(Image.Segments[0].Data, Original);
  }

  std::vector<uint8_t> linkedFile() const {
    auto File = Bytes;
    File.resize(kDataFileOffset + kSegmentSize);
    mach_header_64 H{};
    H.magic = MH_MAGIC_64;
    H.cputype = CPU_TYPE_ARM64;
    H.cpusubtype = kSubtype;
    H.filetype = MH_BUNDLE;
    H.ncmds = 3;
    H.sizeofcmds = 2 * sizeof(segment_command_64) + sizeof(dyld_info_command);
    std::memcpy(File.data(), &H, sizeof(H));
    segment_command_64 S{};
    S.cmd = LC_SEGMENT_64;
    S.cmdsize = sizeof(S);
    std::memcpy(S.segname, kTextName, sizeof(kTextName));
    S.vmaddr = kBase;
    S.vmsize = kSegmentSize;
    S.filesize = kDataFileOffset;
    S.initprot = S.maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
    std::memcpy(File.data() + sizeof(H), &S, sizeof(S));
    std::memset(S.segname, 0, sizeof(S.segname));
    std::memcpy(S.segname, kDataName, sizeof(kDataName));
    S.vmaddr = kData;
    S.fileoff = kDataFileOffset;
    S.filesize = kSegmentSize;
    S.initprot = S.maxprot = VM_PROT_READ | VM_PROT_WRITE;
    std::memcpy(File.data() + sizeof(H) + sizeof(S), &S, sizeof(S));
    dyld_info_command D{};
    D.cmd = LC_DYLD_INFO_ONLY;
    D.cmdsize = sizeof(D);
    D.bind_off = Info.BindOff;
    D.bind_size = Info.BindSize;
    D.weak_bind_off = Info.WeakBindOff;
    D.weak_bind_size = Info.WeakBindSize;
    std::memcpy(File.data() + sizeof(H) + 2 * sizeof(S), &D, sizeof(D));
    std::memcpy(File.data() + kDataFileOffset, Image.Segments[0].Data.data(),
                kSegmentSize);
    return File;
  }
};
} // namespace

TEST(MachOThreadedBind, AllKeysAndAddressDiversityRetainOriginalSignedWords) {
  for (unsigned Key = 0; Key < kKeyCount; ++Key)
    for (bool AddressDiversity : {false, true}) {
      ThreadedImage F;
      const uint64_t Raw =
          (kAuthBind & ~kSigningFieldsMask) | (uint64_t(Key) << kKeyShift) |
          (uint64_t(AddressDiversity) << kAddressDiversityShift);
      F.word(0, Raw);
      F.parse();
      ASSERT_TRUE(F.Image.MachOChainedFixupsError.empty());
      ASSERT_EQ(F.Image.MachOAuthenticatedPointers.size(), 1);
      const auto &A = F.Image.MachOAuthenticatedPointers.at(kData);
      EXPECT_EQ(A.EncodedValue, Raw);
      EXPECT_EQ(A.Symbol, kSymbol);
      EXPECT_EQ(A.Module, kLibrary);
      EXPECT_EQ(A.Addend, kTableAddend);
      EXPECT_EQ(A.Key, Key);
      EXPECT_EQ(A.Diversity, kBindDiversity);
      EXPECT_EQ(A.AddressDiversity, AddressDiversity);
      EXPECT_TRUE(A.WeakImport);
      EXPECT_EQ(readLE<uint64_t>(F.Image.readVA(kData, kPointerBytes)), Raw);
      EXPECT_TRUE(F.Image.DyldBindSlots.empty());
      EXPECT_TRUE(F.Image.ImportStorageSlots.empty());
      EXPECT_FALSE(readImmutableImagePointer(F.Image, kData));
    }
}

TEST(MachOThreadedBind, MixedChainsKeepBaseAndSignedAddends) {
  ThreadedImage F;
  F.word(0, kAuthRebase | kNext8);
  F.word(kPointerBytes, kPlainRebase | kNext8);
  F.word(kSecondRootOffset, kPlainBind);
  F.parse();
  ASSERT_TRUE(F.Image.MachOChainedFixupsError.empty());
  ASSERT_EQ(F.Image.MachOAuthenticatedPointers.size(), 1);
  const auto &A = F.Image.MachOAuthenticatedPointers.at(kData);
  EXPECT_EQ(A.Target, kBase + kRebaseTargetOffset);
  EXPECT_EQ(readLE<uint64_t>(F.Image.readVA(kData, kPointerBytes)),
            kAuthRebase | kNext8);
  EXPECT_EQ(
      readLE<uint64_t>(F.Image.readVA(kData + kPointerBytes, kPointerBytes)),
      kPlainRebase);
  EXPECT_EQ(
      F.Image.MachOResolvedChainedPointerSlots.count(kData + kPointerBytes), 1);
  ASSERT_EQ(F.Image.DyldBindSlots.size(), 1);
  const auto &Bind = F.Image.DyldBindSlots.at(kData + kSecondRootOffset);
  EXPECT_EQ(Bind.Name, kSymbol);
  EXPECT_EQ(Bind.Addend, kCombinedAddend);
  EXPECT_TRUE(F.Image.MachOHasChainedFixups);
}

TEST(MachOThreadedBind, RootsAreIndependentAndPayloadBytesAreNotOpcodes) {
  ThreadedImage F(kTwoRoots);
  F.word(0, kAuthRebase);
  F.word(kSecondRootOffset, kPlainRebase);
  F.parse();
  ASSERT_TRUE(F.Image.MachOChainedFixupsError.empty());
  EXPECT_EQ(F.Image.MachOAuthenticatedPointers.size(), 1);
  EXPECT_EQ(
      F.Image.MachOResolvedChainedPointerSlots.count(kData + kSecondRootOffset),
      1);

  ThreadedImage Ordinary(kOrdinaryPayload);
  Ordinary.parse(InvalidVA);
  EXPECT_TRUE(Ordinary.Image.MachOChainedFixupsError.empty());
  EXPECT_FALSE(Ordinary.Image.MachOHasChainedFixups);
  EXPECT_EQ(Ordinary.Image.DyldBindSlots.size(), 1);
}

TEST(MachOThreadedBind, MalformedBytecodeNeverPublishesAValidPrefix) {
  const llvm::ArrayRef<uint8_t> Cases[] = {
      kMissingSize,      kBadSize,        kIncompleteTable, kMissingTable,
      kRepeatedTable,    kMissingSegment, kBadSegment,      kMisalignedRoot,
      kUnknownSubopcode, kUnknownOpcode,  kTruncatedSymbol, kTruncatedSLEB,
      kOverflowULEB,     kDuplicateRoots};
  for (llvm::ArrayRef<uint8_t> Stream : Cases) {
    ThreadedImage F(Stream);
    F.word(0, kAuthRebase);
    F.expectFailure();
  }
  std::vector<uint8_t> Prefix(std::begin(kOrdinaryPrefix),
                              std::end(kOrdinaryPrefix));
  Prefix.insert(Prefix.end(), std::begin(kRebasesOnly), std::end(kRebasesOnly));
  ThreadedImage F(Prefix);
  F.expectFailure();
}

TEST(MachOThreadedBind, InvalidWordsAndStorageNeverPublishEarlierRoots) {
  for (unsigned Mutation = 0; Mutation < kStorageMutationCount; ++Mutation) {
    ThreadedImage F(kTwoRoots);
    F.word(0, kPlainRebase);
    F.word(kSecondRootOffset, kAuthRebase);
    switch (Mutation) {
    case 0:
      F.word(kSecondRootOffset, kAuthBind); // Empty ordinal table.
      break;
    case 1:
      F.word(kSecondRootOffset, kAuthBind | kReservedOrdinalBit);
      break;
    case 2:
      F.Image.Segments[0].FileSz = kSecondRootOffset;
      break;
    case 3:
      F.Image.Segments[0].Flags = SegmentFlags::Readable;
      break;
    case 4:
      F.Image.Segments[0].Flags =
          F.Image.Segments[0].Flags | SegmentFlags::Executable;
      break;
    case 5:
      F.Image.MachOHasChainedFixups = true;
      break;
    case 6:
      F.Image.IsRelocatable = true;
      break;
    case 7:
      F.Image.MachOCPUSubtype = 0;
      break;
    case 8:
      F.word(kSecondRootOffset, kAuthRebase | kLargestNext);
      break;
    case 9:
      F.Image.Bits = Bitness::Bits32;
      break;
    }
    F.expectFailure();
  }
  ThreadedImage MissingBase(kRebasesOnly);
  MissingBase.word(0, kAuthRebase);
  MissingBase.parse(InvalidVA);
  EXPECT_FALSE(MissingBase.Image.MachOChainedFixupsError.empty());
  EXPECT_TRUE(MissingBase.Image.MachOAuthenticatedPointers.empty());
}

TEST(MachOThreadedBind, PublicLoaderPreservesRecipesAndRefusesMalformedInput) {
  ThreadedImage F(kRebasesOnly);
  F.word(0, kAuthRebase);
  auto File = F.linkedFile();
  File[kStreamOffset + kPublicSegmentOpcodeOffset] =
      kPublicSegmentOpcode; // Data is load-command segment one.
  auto load = [&]() {
    return MachOLoader().loadBuffer(llvm::MemoryBufferRef(
        llvm::StringRef(reinterpret_cast<const char *>(File.data()),
                        File.size()),
        kBufferName));
  };
  auto Image = load();
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->MachOAuthenticatedPointers.size(), 1);
  EXPECT_EQ(Image->MachOAuthenticatedPointers.at(kData).Target,
            kBase + kRebaseTargetOffset);
  EXPECT_EQ(readLE<uint64_t>(Image->readVA(kData, kPointerBytes)), kAuthRebase);
  EXPECT_TRUE(Image->MachOHasChainedFixups);
  // An unclassified nonzero value is not a proved local metadata pointer.
  writeLE<uint64_t>(File.data() + kDataFileOffset + kPointerBytes,
                    kPlainRebase);
  auto WithUnclassifiedWord = load();
  ASSERT_TRUE(bool(WithUnclassifiedWord));
  EXPECT_FALSE(macho_loader::readMetadataPointerTarget(*WithUnclassifiedWord,
                                                       kData + kPointerBytes));
  File[kStreamOffset + kPublicApplyOpcodeOffset] = kInvalidApplyOpcode;
  auto Invalid = load();
  ASSERT_FALSE(bool(Invalid));
  EXPECT_NE(llvm::toString(Invalid.takeError()).find(kError),
            std::string::npos);
}

TEST(MachOThreadedBind, ClassicLinkerRecoversStrippedObjectiveCMetadata) {
#if defined(__APPLE__) && defined(__aarch64__)
#ifdef NEVERD_TEST_CLANG
  const auto Compiler = llvm::sys::findProgramByName(NEVERD_TEST_CLANG);
#else
  const auto Compiler = llvm::sys::findProgramByName(kClang);
#endif
  const auto Xcrun = llvm::sys::findProgramByName(kXcrun);
  if (!Compiler || !Xcrun)
    GTEST_SKIP() << kNativeOnly;
  llvm::SmallString<128> SourcePath, BundlePath, LogPath, LinkerPathFile;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTempPrefix, kSourceExtension,
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTempPrefix, kBundleExtension,
                                                  BundlePath));
  llvm::FileRemover RemoveBundle(BundlePath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile(kTempPrefix, kLogExtension, LogPath));
  llvm::FileRemover RemoveLog(LogPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTempPrefix, kPathExtension,
                                                  LinkerPathFile));
  llvm::FileRemover RemoveLinkerPath(LinkerPathFile);
  const llvm::StringRef FindArgs[] = {*Xcrun, kFind, kClassicLinker};
  const std::optional<llvm::StringRef> FindRedirects[] = {
      std::nullopt, LinkerPathFile.str(), LogPath.str()};
  if (llvm::sys::ExecuteAndWait(*Xcrun, FindArgs, std::nullopt, FindRedirects,
                                kToolTimeoutSeconds))
    GTEST_SKIP() << kClassicUnavailable;
  const auto LinkerFile = llvm::MemoryBuffer::getFile(LinkerPathFile);
  ASSERT_TRUE(LinkerFile);
  const std::string LinkerOption =
      std::string(kLinkerPrefix) + (*LinkerFile)->getBuffer().trim().str();
  std::error_code FileError;
  {
    llvm::raw_fd_ostream Source(SourcePath, FileError);
    ASSERT_FALSE(FileError);
    Source << kObjCSource;
  }
  for (llvm::StringRef Optimization : {kO0, kO2}) {
    const llvm::StringRef Args[] = {*Compiler,    kTargetOption,  kTarget,
                                    kBundle,      kDynamicLookup, kStripLocals,
                                    LinkerOption, Optimization,   SourcePath,
                                    kOutput,      BundlePath};
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, LogPath.str()};
    std::string Error;
    const int Status =
        llvm::sys::ExecuteAndWait(*Compiler, Args, std::nullopt, Redirects,
                                  kToolTimeoutSeconds, 0, &Error);
    auto Log = llvm::MemoryBuffer::getFile(LogPath);
    ASSERT_EQ(Status, 0) << Error
                         << (Log ? (*Log)->getBuffer().str() : std::string());
    auto Bytes = llvm::MemoryBuffer::getFile(BundlePath);
    ASSERT_TRUE(Bytes);
    auto Object =
        llvm::object::ObjectFile::createObjectFile((*Bytes)->getMemBufferRef());
    ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
    const auto *MachO =
        llvm::dyn_cast<llvm::object::MachOObjectFile>(Object->get());
    ASSERT_TRUE(MachO);
    bool Threaded = false;
    for (const auto &Command : MachO->load_commands()) {
      EXPECT_NE(Command.C.cmd, LC_DYLD_CHAINED_FIXUPS);
      if (Command.C.cmd != LC_DYLD_INFO_ONLY)
        continue;
      const auto Info = MachO->getDyldInfoLoadCommand(Command);
      ASSERT_GT(Info.bind_size, 0);
      ASSERT_LT(Info.bind_off, (*Bytes)->getBufferSize());
      Threaded =
          uint8_t((*Bytes)->getBuffer()[Info.bind_off]) == kThreadedTableOpcode;
    }
    ASSERT_TRUE(Threaded);
    auto Image = MachOLoader().loadBuffer((*Bytes)->getMemBufferRef());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    EXPECT_TRUE(Image->isArm64e());
    EXPECT_TRUE(Image->MachOHasChainedFixups);
    EXPECT_FALSE(Image->MachOAuthenticatedPointers.empty());
    bool SawWeakFunction = false;
    for (const auto &[Slot, Recipe] : Image->MachOAuthenticatedPointers) {
      ASSERT_TRUE(Recipe.EncodedValue);
      EXPECT_EQ(Recipe.LibraryOrdinal.has_value(), !Recipe.Symbol.empty());
      EXPECT_EQ(readLE<uint64_t>(Image->readVA(Slot, kPointerBytes)),
                *Recipe.EncodedValue);
      if (Recipe.Symbol == kWeakFunction) {
        SawWeakFunction = true;
        EXPECT_EQ(Recipe.LibraryOrdinal, kWeakLookupOrdinal);
        EXPECT_FALSE(Recipe.WeakImport);
      }
    }
    ASSERT_FALSE(Image->DyldBindSlots.empty());
    bool SawWeakValue = false;
    for (const auto &[Slot, Binding] : Image->DyldBindSlots) {
      EXPECT_TRUE(Binding.LibraryOrdinal);
      if (Binding.Name == kWeakValue) {
        SawWeakValue = true;
        EXPECT_EQ(Binding.LibraryOrdinal, kWeakLookupOrdinal);
        EXPECT_FALSE(Binding.WeakImport);
      }
    }
    EXPECT_TRUE(SawWeakFunction);
    EXPECT_TRUE(SawWeakValue);
    ASSERT_EQ(Image->ObjCClasses.size(), 1);
    ASSERT_EQ(Image->ObjCMethods.size(), 1);
    const auto &Method = Image->ObjCMethods.front();
    EXPECT_EQ(Method.ClassName, kClass);
    EXPECT_EQ(Method.Selector, kSelector);
    EXPECT_EQ(Method.Status, kSupported);
    EXPECT_TRUE(Image->isCodeAddress(Method.Implementation));
    EXPECT_EQ(Image->getFunctionNameAt(Method.Implementation), kMethod);
    EXPECT_TRUE(Method.TypeHint);
  }
#else
  GTEST_SKIP() << kNativeOnly;
#endif
}

TEST(MachOThreadedBind, OrdinalTableStateMustBeCompleteAndUnambiguous) {
  ThreadedImage MissingLibrary;
  MissingLibrary.word(0, kAuthBind);
  MissingLibrary.Image.MachODylibReferences.front().Name.clear();
  MissingLibrary.expectFailure();

  struct Mutation {
    const char *Name;
    size_t Index;
    uint8_t Byte;
  };
  const Mutation Mutations[] = {
#define NEVERD_THREADED_MUTATION(Name, Index, Byte) {Name, Index, Byte},
#include "MachOThreadedBindFixtures.def"
  };
  for (const auto &M : Mutations) {
    SCOPED_TRACE(M.Name);
    auto Stream =
        std::vector<uint8_t>(std::begin(kOneImport), std::end(kOneImport));
    Stream[M.Index] = M.Byte;
    ThreadedImage F(Stream);
    F.word(0, kAuthBind);
    F.expectFailure();
  }
}

TEST(MachOThreadedBind, SignedAddendAndImageBaseOverflowPublishNothing) {
  auto Stream =
      std::vector<uint8_t>(std::begin(kOneImport), std::end(kOneImport));
  Stream.erase(Stream.begin() + kAddendByteOffset);
  Stream.insert(Stream.begin() + kAddendByteOffset, std::begin(kMinimumAddend),
                std::end(kMinimumAddend));
  ThreadedImage Addend(Stream);
  Addend.word(0,
              kPlainBind); // INT64_MIN plus the inline -1 is not representable.
  Addend.expectFailure();

  ThreadedImage Base(kRebasesOnly);
  Base.word(0, kAuthRebase);
  Base.parse(std::numeric_limits<va_t>::max() - 1);
  EXPECT_FALSE(Base.Image.MachOChainedFixupsError.empty());
  EXPECT_TRUE(Base.Image.MachOAuthenticatedPointers.empty());
  EXPECT_EQ(readLE<uint64_t>(Base.Image.readVA(kData, kPointerBytes)),
            kAuthRebase);
}

TEST(MachOThreadedBind, OrdinalsKeepDuplicateNamesAndExactLibraryIdentity) {
  ThreadedImage F(kTwoImports);
  F.Image.MachODylibReferences.insert(F.Image.MachODylibReferences.begin(),
                                      {kOtherLibrary, false});
  F.Image.MachODylibReferences.back().Weak = true;
  // The display library list intentionally has a different shape. Only the
  // original dependency load-command ordinal can identify this binding.
  F.Image.DynInfo.NeededLibs = {kOtherLibrary};
  F.word(0, kAuthBind | kNext8);
  F.word(kPointerBytes, kAuthBind | 1);
  F.parse();
  ASSERT_TRUE(F.Image.MachOChainedFixupsError.empty());
  ASSERT_EQ(F.Image.MachOAuthenticatedPointers.size(), 2);
  const auto &First = F.Image.MachOAuthenticatedPointers.at(kData);
  const auto &Second =
      F.Image.MachOAuthenticatedPointers.at(kData + kPointerBytes);
  EXPECT_EQ(First.Symbol, kSymbol);
  EXPECT_EQ(Second.Symbol, kSymbol);
  EXPECT_EQ(First.Addend, kOtherTableAddend);
  EXPECT_EQ(Second.Addend, kTableAddend);
  EXPECT_EQ(First.Module, kLibrary);
  EXPECT_EQ(Second.Module, kLibrary);
  EXPECT_TRUE(First.WeakImport);
  EXPECT_TRUE(Second.WeakImport);
}

TEST(MachOThreadedBind, RootSegmentsUseLoadCommandOrder) {
  ThreadedImage F(kUnsortedSegments);
  auto Other = F.Image.Segments.front();
  Other.VA = kBase;
  writeLE<uint64_t>(Other.Data.data(), kAuthRebase);
  F.Image.Segments.push_back(std::move(Other));
  F.word(kSecondRootOffset, kAuthRebase);
  F.parse();
  ASSERT_TRUE(F.Image.MachOChainedFixupsError.empty());
  EXPECT_EQ(F.Image.MachOAuthenticatedPointers.size(), 2);
  EXPECT_EQ(F.Image.MachOAuthenticatedPointers.count(kBase), 1);
  EXPECT_EQ(F.Image.MachOAuthenticatedPointers.count(kData + kSecondRootOffset),
            1);
}

TEST(MachOThreadedBind, IndirectTableEvidenceCanRefineButCannotDisagree) {
  for (bool Authenticated : {false, true}) {
    ThreadedImage Valid;
    Valid.word(0, Authenticated ? kAuthBind : kPlainBind);
    Valid.Image.ImportPtrSlots[kData] = kSymbol;
    ASSERT_TRUE(Valid.Image.recordImportStorageSlot(
        kData, kSymbol, 0, ImportStorageEvidence::PointerTable));
    Valid.parse();
    ASSERT_TRUE(Valid.Image.MachOChainedFixupsError.empty());
    if (Authenticated) {
      EXPECT_EQ(Valid.Image.MachOAuthenticatedPointers.size(), 1);
      EXPECT_FALSE(Valid.Image.isValidImportStorageSlot(kData, kSymbol));
      EXPECT_TRUE(Valid.Image.DyldBindSlots.empty());
    } else {
      EXPECT_EQ(Valid.Image.ImportStorageSlots.at(kData).Addend,
                kCombinedAddend);
      EXPECT_EQ(Valid.Image.DyldBindSlots.at(kData).Addend, kCombinedAddend);
    }

    ThreadedImage Conflict;
    Conflict.word(0, kPlainRebase | kNext8);
    Conflict.word(kPointerBytes, Authenticated ? kAuthBind : kPlainBind);
    Conflict.Image.ImportPtrSlots[kData + kPointerBytes] = kOtherSymbol;
    const auto Original = Conflict.Image.Segments[0].Data;
    Conflict.parse();
    EXPECT_FALSE(Conflict.Image.MachOChainedFixupsError.empty());
    EXPECT_EQ(Conflict.Image.Segments[0].Data, Original);
    EXPECT_TRUE(Conflict.Image.MachOAuthenticatedPointers.empty());
    EXPECT_TRUE(Conflict.Image.MachOResolvedChainedPointerSlots.empty());
    EXPECT_TRUE(Conflict.Image.Imports.empty());
    EXPECT_EQ(Conflict.Image.ImportPtrSlots.at(kData + kPointerBytes),
              kOtherSymbol);
  }
}

TEST(MachOThreadedBind, MixedAuxiliaryStreamsFailBeforePublication) {
  for (auto Member : {&macho_loader::DyldInfoOffsets::RebaseSize,
                      &macho_loader::DyldInfoOffsets::LazyBindSize,
                      &macho_loader::DyldInfoOffsets::WeakBindSize}) {
    ThreadedImage F(kRebasesOnly);
    F.word(0, kAuthRebase);
    F.Info.*Member = 1;
    F.expectFailure();
  }
}

TEST(MachOThreadedBind, PublicLoaderRetainsWeakStreamAdmission) {
  ThreadedImage F(kRebasesOnly);
  F.word(0, kAuthRebase);
  F.Info.WeakBindOff = F.Info.BindOff + F.Info.BindSize;
  F.Info.WeakBindSize = 1;
  auto File = F.linkedFile();
  File[kStreamOffset + kPublicSegmentOpcodeOffset] = kPublicSegmentOpcode;
  auto Image = MachOLoader().loadBuffer(llvm::MemoryBufferRef(
      llvm::StringRef(reinterpret_cast<const char *>(File.data()), File.size()),
      kBufferName));
  ASSERT_FALSE(bool(Image));
  const std::string Error = llvm::toString(Image.takeError());
  EXPECT_NE(Error.find(kError), std::string::npos) << Error;
}

TEST(MachOThreadedBind, RootsRequireUniqueStorageBeforeLinkedit) {
  ThreadedImage Alias(kRebasesOnly);
  Alias.word(0, kAuthRebase);
  Alias.Image.Segments.push_back(Alias.Image.Segments.front());
  Alias.expectFailure();

  ThreadedImage Linkedit(kRebasesOnly);
  Linkedit.word(0, kAuthRebase);
  Linkedit.Image.Segments.front().Name = kLinkeditName;
  Linkedit.expectFailure();
}

TEST(MachOThreadedBind, DuplicateDyldCommandsCannotHideAThreadedOwner) {
  ThreadedImage Ambiguous(kRebasesOnly);
  Ambiguous.word(0, kAuthRebase);
  Ambiguous.Info.Ambiguous = true;
  Ambiguous.expectFailure();

  for (bool EmptyFirst : {false, true}) {
    ThreadedImage F(kRebasesOnly);
    F.word(0, kAuthRebase);
    auto File = F.linkedFile();
    File[kStreamOffset + kPublicSegmentOpcodeOffset] = kPublicSegmentOpcode;
    mach_header_64 Header;
    std::memcpy(&Header, File.data(), sizeof(Header));
    const size_t FirstOffset = sizeof(Header) + 2 * sizeof(segment_command_64);
    dyld_info_command Command;
    std::memcpy(&Command, File.data() + FirstOffset, sizeof(Command));
    std::memcpy(File.data() + sizeof(Header) + Header.sizeofcmds, &Command,
                sizeof(Command));
    if (EmptyFirst) {
      Command.bind_off = Command.bind_size = 0;
      std::memcpy(File.data() + FirstOffset, &Command, sizeof(Command));
    }
    ++Header.ncmds;
    Header.sizeofcmds += sizeof(Command);
    std::memcpy(File.data(), &Header, sizeof(Header));
    auto Image = MachOLoader().loadBuffer(llvm::MemoryBufferRef(
        llvm::StringRef(reinterpret_cast<const char *>(File.data()),
                        File.size()),
        kBufferName));
    ASSERT_FALSE(bool(Image));
    // LLVM rejects duplicate load commands before the bind reader runs.
    EXPECT_NE(llvm::toString(Image.takeError()).find(kDuplicateDyldInfo),
              std::string::npos);
  }
}

TEST(MachOThreadedBind, LebOperandsCannotSelectTheThreadedFormat) {
  ThreadedImage Offset(kOrdinaryULEBPayload);
  Offset.parse(InvalidVA);
  ASSERT_TRUE(Offset.Image.MachOChainedFixupsError.empty());
  EXPECT_FALSE(Offset.Image.MachOHasChainedFixups);
  ASSERT_EQ(Offset.Image.DyldBindSlots.size(), 1);
  EXPECT_EQ(Offset.Image.DyldBindSlots.begin()->first, kData + kPayloadOffset);

  ThreadedImage Addend(kOrdinarySLEBPayload);
  Addend.parse(InvalidVA);
  ASSERT_TRUE(Addend.Image.MachOChainedFixupsError.empty());
  EXPECT_FALSE(Addend.Image.MachOHasChainedFixups);
  ASSERT_EQ(Addend.Image.DyldBindSlots.size(), 1);
  EXPECT_EQ(Addend.Image.DyldBindSlots.at(kData).Addend, kPayloadAddend);
}

TEST(MachOThreadedBind, EqualSymbolsKeepTheirDistinctLibraryBindings) {
  ThreadedImage F(kTwoLibraries);
  F.Image.MachODylibReferences.push_back({kOtherLibrary, true});
  F.word(0, kAuthBind | kNext8);
  F.word(kPointerBytes, kAuthBind | 1);
  F.parse();
  ASSERT_TRUE(F.Image.MachOChainedFixupsError.empty());
  ASSERT_EQ(F.Image.MachOAuthenticatedPointers.size(), 2);
  const auto &First = F.Image.MachOAuthenticatedPointers.at(kData);
  const auto &Second =
      F.Image.MachOAuthenticatedPointers.at(kData + kPointerBytes);
  EXPECT_EQ(First.Symbol, kSymbol);
  EXPECT_EQ(Second.Symbol, kSymbol);
  EXPECT_EQ(First.Module, kLibrary);
  EXPECT_EQ(Second.Module, kOtherLibrary);
  EXPECT_FALSE(First.WeakImport);
  EXPECT_TRUE(Second.WeakImport);
}
