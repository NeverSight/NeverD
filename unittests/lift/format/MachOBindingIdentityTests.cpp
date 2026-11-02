//===- MachOBindingIdentityTests.cpp - Exact dyld lookup scope -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Distinguishes symbol equality from the exact dependency/lookup identity
/// retained by plain bindings and arm64e authentication recipes.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/loader/MachO/MachOMetadataPointer.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/BinaryFormat/MachO.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

using namespace neverd;

namespace {
#define NEVERD_BINDING_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define NEVERD_BINDING_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_BINDING_BYTES(Name, ...)                                        \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "MachOBindingIdentityFixtures.def"

struct Scope {
  const char *Name;
  int64_t Ordinal;
  uint8_t Opcode;
};
constexpr Scope Scopes[] = {
#define NEVERD_BINDING_SCOPE(Name, Ordinal, Opcode) {Name, Ordinal, Opcode},
#include "MachOBindingIdentityFixtures.def"
};
constexpr int64_t PositiveOrdinals[] = {
#define NEVERD_BINDING_ORDINAL(Value) Value,
#include "MachOBindingIdentityFixtures.def"
};

BinaryImage makeImage() {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.MachOCPUSubtype = kArm64eSubtype;
  Image.MachODylibReferences = {{kLibrary, false}, {kLibrary, false}};
  Segment S;
  S.VA = kStorage;
  S.Size = S.FileSz = kStorageBytes;
  S.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  S.Data.resize(kStorageBytes);
  Image.Segments.push_back(std::move(S));
  return Image;
}

void parseStream(BinaryImage &Image, llvm::ArrayRef<uint8_t> Stream,
                 bool Lazy = false) {
  std::vector<uint8_t> Bytes(kStreamOffset);
  Bytes.insert(Bytes.end(), Stream.begin(), Stream.end());
  macho_loader::DyldInfoOffsets Info;
  if (Lazy) {
    Info.LazyBindOff = kStreamOffset;
    Info.LazyBindSize = Stream.size();
  } else {
    Info.BindOff = kStreamOffset;
    Info.BindSize = Stream.size();
  }
  macho_loader::parseBindStreams(Bytes.data(), Bytes.size(), Info, Image, 0);
}

void parseClassic(BinaryImage &Image, uint8_t Opcode, bool Lazy = false) {
  const llvm::ArrayRef<uint8_t> Bytes =
      Lazy ? llvm::ArrayRef<uint8_t>(kLazyStream)
           : llvm::ArrayRef<uint8_t>(kClassicStream);
  std::vector<uint8_t> Stream(Bytes.begin(), Bytes.end());
  Stream.front() = Opcode;
  parseStream(Image, Stream, Lazy);
}

void parseThreaded(BinaryImage &Image, uint8_t Opcode, uint64_t Word) {
  std::vector<uint8_t> Stream(std::begin(kThreadedStream),
                              std::end(kThreadedStream));
  Stream[kThreadedOrdinalOffset] = Opcode;
  writeLE<uint64_t>(Image.Segments.front().Data.data(), Word);
  parseStream(Image, Stream);
}

template <typename T>
void put(std::vector<uint8_t> &Bytes, size_t Offset, const T &Value) {
  ASSERT_LE(Offset, Bytes.size());
  ASSERT_LE(sizeof(T), Bytes.size() - Offset);
  std::memcpy(Bytes.data() + Offset, &Value, sizeof(T));
}

void parseChained(BinaryImage &Image, uint16_t PointerFormat,
                  uint32_t ImportFormat, int64_t LibraryOrdinal,
                  uint64_t Word) {
  using namespace llvm::MachO;
  macho_loader::ChainedFixupsInfo Info{kStreamOffset, kBlobBytes};
  std::vector<uint8_t> Bytes(kStreamOffset + kBlobBytes);
  dyld_chained_fixups_header Header{};
  Header.starts_offset = kStartsOffset;
  Header.imports_offset = kImportsOffset;
  Header.symbols_offset = kSymbolsOffset;
  Header.imports_count = 1;
  Header.imports_format = ImportFormat;
  put(Bytes, Info.DataOff, Header);
  const size_t Starts = Info.DataOff + Header.starts_offset;
  put(Bytes, Starts, uint32_t(1));
  put(Bytes, Starts + sizeof(uint32_t), kSegmentStartOffset);
  dyld_chained_starts_in_segment Segment{};
  Segment.size =
      offsetof(dyld_chained_starts_in_segment, page_start) + sizeof(uint16_t);
  Segment.page_size = kStorageBytes;
  Segment.pointer_format = PointerFormat;
  Segment.segment_offset = kStorage;
  Segment.page_count = 1;
  put(Bytes, Starts + kSegmentStartOffset, Segment);
  const auto writeImport = [&](auto Entry) {
    Entry.lib_ordinal =
        uint64_t(LibraryOrdinal) &
        (ImportFormat == DYLD_CHAINED_IMPORT_ADDEND64 ? kLibrary16Mask
                                                      : kLibrary8Mask);
    put(Bytes, Info.DataOff + Header.imports_offset, Entry);
  };
  switch (ImportFormat) {
  case DYLD_CHAINED_IMPORT:
    writeImport(dyld_chained_import{});
    break;
  case DYLD_CHAINED_IMPORT_ADDEND:
    writeImport(dyld_chained_import_addend{});
    break;
  case DYLD_CHAINED_IMPORT_ADDEND64:
    writeImport(dyld_chained_import_addend64{});
    break;
  default:
    FAIL();
  }
  std::memcpy(Bytes.data() + Info.DataOff + Header.symbols_offset, kSymbol,
              sizeof(kSymbol));
  writeLE<uint64_t>(Image.Segments.front().Data.data(), Word);
  Image.MachOHasChainedFixups = true;
  macho_loader::parseChainedFixupsImports(Bytes.data(), Bytes.size(), Info,
                                          Image);
  macho_loader::parseChainedFixupsRebases(Bytes.data(), Bytes.size(), Info, 0,
                                          Image);
}
} // namespace

TEST(MachOBindingIdentity, DifferentSpecialLookupsCannotMergeBySymbolAlone) {
  for (const auto &First : Scopes)
    for (const auto &Other : Scopes) {
      if (First.Ordinal == Other.Ordinal)
        continue;
      SCOPED_TRACE(First.Name);
      SCOPED_TRACE(Other.Name);
      auto Image = makeImage();
      parseClassic(Image, First.Opcode);
      ASSERT_EQ(Image.DyldBindSlots.size(), 1);
      parseClassic(Image, Other.Opcode);
      parseClassic(Image, First.Opcode);
      EXPECT_TRUE(Image.DyldBindSlots.empty());
      EXPECT_TRUE(Image.collectImportStorageSlots().Slots.empty());
      EXPECT_EQ(Image.ConflictingImportStorageSlots.count(kStorage), 1);
    }
}

TEST(MachOBindingIdentity, ClassicAndLazyBindingsRetainEverySpecialScope) {
  for (const auto &Scope : Scopes)
    for (bool Lazy : {false, true}) {
      SCOPED_TRACE(Scope.Name);
      SCOPED_TRACE(Lazy);
      auto Image = makeImage();
      parseClassic(Image, Scope.Opcode, Lazy);
      ASSERT_EQ(Image.DyldBindSlots.size(), 1);
      EXPECT_EQ(Image.DyldBindSlots.at(kStorage).LibraryOrdinal, Scope.Ordinal);
      EXPECT_TRUE(Image.DyldBindSlots.at(kStorage).Module.empty());
    }
}

TEST(MachOBindingIdentity, DuplicateInstallNamesKeepTheirExactOrdinals) {
  for (bool Reverse : {false, true}) {
    auto Image = makeImage();
    parseClassic(Image, Reverse ? kSecondLibraryOpcode : kFirstLibraryOpcode);
    ASSERT_EQ(Image.DyldBindSlots.size(), 1);
    parseClassic(Image, Reverse ? kFirstLibraryOpcode : kSecondLibraryOpcode);
    EXPECT_TRUE(Image.DyldBindSlots.empty());
    EXPECT_TRUE(Image.collectImportStorageSlots().Slots.empty());
    EXPECT_EQ(Image.ConflictingImportStorageSlots.count(kStorage), 1);
  }
}

TEST(MachOBindingIdentity, AbsentScopeRefinesWithoutBecomingSelf) {
  for (const auto &Scope : Scopes)
    for (bool KnownFirst : {false, true}) {
      auto Image = makeImage();
      const auto record = [&](bool Known) {
        return Image.recordDyldBindSlot(
            kStorage, kSymbol, 0, {}, false,
            Known ? std::optional<int64_t>(Scope.Ordinal) : std::nullopt);
      };
      ASSERT_TRUE(record(KnownFirst));
      EXPECT_EQ(Image.DyldBindSlots.at(kStorage).LibraryOrdinal.has_value(),
                KnownFirst);
      ASSERT_TRUE(record(!KnownFirst));
      ASSERT_TRUE(record(false));
      EXPECT_EQ(Image.DyldBindSlots.at(kStorage).LibraryOrdinal, Scope.Ordinal);
    }
}

TEST(MachOBindingIdentity, LegacyLazyEntryWithoutOrdinalKeepsScopeUnknown) {
  auto Image = makeImage();
  Image.MachOCPUSubtype = kArm64Subtype;
  std::vector<uint8_t> Stream(std::begin(kLazyStream), std::end(kLazyStream));
  const auto &Scope = *std::rbegin(Scopes);
  Stream.front() = Scope.Opcode;
  Stream.insert(Stream.end(), std::begin(kLazyStream) + kClassicSymbolOffset,
                std::end(kLazyStream));
  Stream[std::size(kLazyStream) + kLazyOffsetOperand - kClassicSymbolOffset] =
      kPointerBytes;
  parseStream(Image, Stream, true);
  ASSERT_EQ(Image.DyldBindSlots.size(), 2);
  EXPECT_EQ(Image.DyldBindSlots.at(kStorage).LibraryOrdinal, Scope.Ordinal);
  EXPECT_FALSE(Image.DyldBindSlots.at(kStorage + kPointerBytes).LibraryOrdinal);
}

TEST(MachOBindingIdentity, ClassicOrdinalsRetainTheirFullDecodedWidth) {
  auto Image = makeImage();
  std::vector<uint8_t> Stream{kOrdinalULEBOpcode};
  Stream.insert(Stream.end(), std::begin(kLargeClassicOrdinalULEB),
                std::end(kLargeClassicOrdinalULEB));
  Stream.insert(Stream.end(), std::begin(kClassicStream) + kClassicSymbolOffset,
                std::end(kClassicStream));
  parseStream(Image, Stream);
  ASSERT_EQ(Image.DyldBindSlots.size(), 1);
  EXPECT_EQ(Image.DyldBindSlots.at(kStorage).LibraryOrdinal,
            kLargeClassicOrdinal);
  EXPECT_TRUE(Image.DyldBindSlots.at(kStorage).Module.empty());
}

TEST(MachOBindingIdentity, AllChainedImportLayoutsRetainSpecialScopes) {
  for (uint8_t Format : kArm64ePointerFormats)
    for (uint8_t ImportFormat : kImportFormats)
      for (const auto &Scope : Scopes)
        for (bool Authenticated : {false, true}) {
          SCOPED_TRACE(unsigned(Format));
          SCOPED_TRACE(unsigned(ImportFormat));
          SCOPED_TRACE(Scope.Name);
          SCOPED_TRACE(Authenticated);
          auto Image = makeImage();
          parseChained(Image, Format, ImportFormat, Scope.Ordinal,
                       Authenticated ? kAuthenticatedBind : kArm64eBind);
          ASSERT_TRUE(Image.MachOChainedFixupsError.empty());
          if (Authenticated) {
            ASSERT_EQ(Image.MachOAuthenticatedPointers.size(), 1);
            const auto &Recipe = Image.MachOAuthenticatedPointers.at(kStorage);
            EXPECT_EQ(Recipe.LibraryOrdinal, Scope.Ordinal);
            EXPECT_TRUE(Recipe.Module.empty());
            EXPECT_EQ(Recipe.Symbol, kSymbol);
            EXPECT_EQ(Recipe.EncodedValue, kAuthenticatedBind);
          } else {
            ASSERT_EQ(Image.DyldBindSlots.size(), 1);
            EXPECT_EQ(Image.DyldBindSlots.at(kStorage).LibraryOrdinal,
                      Scope.Ordinal);
          }
        }
}

TEST(MachOBindingIdentity, GenericChainsRejectConflictingSpecialScopes) {
  for (uint8_t Format : kGenericPointerFormats)
    for (uint8_t ImportFormat : kImportFormats)
      for (const auto &First : Scopes)
        for (const auto &Other : Scopes) {
          if (First.Ordinal == Other.Ordinal)
            continue;
          auto Image = makeImage();
          Image.MachOCPUSubtype = kArm64Subtype;
          parseChained(Image, Format, ImportFormat, First.Ordinal,
                       kGenericBind);
          ASSERT_EQ(Image.DyldBindSlots.size(), 1);
          EXPECT_EQ(Image.DyldBindSlots.at(kStorage).LibraryOrdinal,
                    First.Ordinal);
          parseChained(Image, Format, ImportFormat, Other.Ordinal,
                       kGenericBind);
          EXPECT_TRUE(Image.DyldBindSlots.empty());
          EXPECT_TRUE(Image.collectImportStorageSlots().Slots.empty());
          EXPECT_EQ(Image.ConflictingImportStorageSlots.count(kStorage), 1);
        }
}

TEST(MachOBindingIdentity, ChainedHighPositiveOrdinalsRemainUnsigned) {
  for (uint8_t ImportFormat : kImportFormats)
    for (int64_t Ordinal : PositiveOrdinals) {
      if (ImportFormat != llvm::MachO::DYLD_CHAINED_IMPORT_ADDEND64 &&
          Ordinal > kLast8BitPositive)
        continue;
      for (bool Authenticated : {false, true}) {
        SCOPED_TRACE(unsigned(ImportFormat));
        SCOPED_TRACE(Ordinal);
        auto Image = makeImage();
        Image.MachODylibReferences.assign(Ordinal, {kLibrary, false});
        parseChained(Image, kArm64ePointerFormat, ImportFormat, Ordinal,
                     Authenticated ? kAuthenticatedBind : kArm64eBind);
        ASSERT_TRUE(Image.MachOChainedFixupsError.empty());
        if (Authenticated) {
          ASSERT_EQ(Image.MachOAuthenticatedPointers.size(), 1);
          const auto &Recipe = Image.MachOAuthenticatedPointers.at(kStorage);
          EXPECT_EQ(Recipe.LibraryOrdinal, Ordinal);
          EXPECT_EQ(Recipe.Module, kLibrary);
        } else {
          ASSERT_EQ(Image.DyldBindSlots.size(), 1);
          EXPECT_EQ(Image.DyldBindSlots.at(kStorage).LibraryOrdinal, Ordinal);
          EXPECT_EQ(Image.DyldBindSlots.at(kStorage).Module, kLibrary);
        }
      }
    }
}

TEST(MachOBindingIdentity, ClassicAndChainedEvidenceUseTheSameScopeJoin) {
  for (const auto &First : Scopes)
    for (const auto &Other : Scopes)
      for (bool ChainedFirst : {false, true}) {
        auto Image = makeImage();
        Image.MachOCPUSubtype = kArm64Subtype;
        const auto chained = [&] {
          parseChained(Image, kGenericPointerFormat, kImportFormats[0],
                       First.Ordinal, kGenericBind);
        };
        if (ChainedFirst)
          chained();
        parseClassic(Image, Other.Opcode);
        if (!ChainedFirst)
          chained();
        if (First.Ordinal == Other.Ordinal) {
          ASSERT_EQ(Image.DyldBindSlots.size(), 1);
          EXPECT_EQ(Image.DyldBindSlots.at(kStorage).LibraryOrdinal,
                    First.Ordinal);
        } else {
          EXPECT_TRUE(Image.DyldBindSlots.empty());
          EXPECT_TRUE(Image.collectImportStorageSlots().Slots.empty());
          EXPECT_EQ(Image.ConflictingImportStorageSlots.count(kStorage), 1);
        }
      }
}

TEST(MachOBindingIdentity, ThreadedRecipesRetainPlainAndAuthenticatedScopes) {
  for (uint32_t Subtype : {kArm64eSubtype, kArm64ex1Subtype})
    for (const auto &Scope : Scopes)
      for (bool Authenticated : {false, true}) {
        auto Image = makeImage();
        Image.MachOCPUSubtype = Subtype;
        parseThreaded(Image, Scope.Opcode,
                      Authenticated ? kAuthenticatedBind : kArm64eBind);
        ASSERT_TRUE(Image.MachOChainedFixupsError.empty());
        if (Authenticated) {
          ASSERT_EQ(Image.MachOAuthenticatedPointers.size(), 1);
          const auto &Recipe = Image.MachOAuthenticatedPointers.at(kStorage);
          EXPECT_EQ(Recipe.LibraryOrdinal, Scope.Ordinal);
          EXPECT_EQ(Recipe.EncodedValue, kAuthenticatedBind);
        } else {
          ASSERT_EQ(Image.DyldBindSlots.size(), 1);
          EXPECT_EQ(Image.DyldBindSlots.at(kStorage).LibraryOrdinal,
                    Scope.Ordinal);
        }
      }
}

TEST(MachOBindingIdentity, ImportScopeCannotTurnIntoLocalMetadataProvenance) {
  for (const auto &Scope : Scopes) {
    auto Image = makeImage();
    parseChained(Image, kArm64ePointerFormat, kImportFormats[0], Scope.Ordinal,
                 kAuthenticatedRebase);
    ASSERT_EQ(macho_loader::readMetadataPointerTarget(Image, kStorage),
              kLocalTarget);
    auto &Recipe = Image.MachOAuthenticatedPointers.at(kStorage);
    EXPECT_FALSE(Recipe.LibraryOrdinal);
    Recipe.LibraryOrdinal = Scope.Ordinal;
    EXPECT_FALSE(macho_loader::readMetadataPointerTarget(Image, kStorage));
    Recipe.LibraryOrdinal.reset();
    EXPECT_EQ(macho_loader::readMetadataPointerTarget(Image, kStorage),
              kLocalTarget);
  }
}

TEST(MachOBindingIdentity, ThreadedDuplicateNamesRetainPositiveOrdinals) {
  int64_t Ordinal = 0;
  for (uint8_t Opcode : {kFirstLibraryOpcode, kSecondLibraryOpcode}) {
    ++Ordinal;
    for (bool Authenticated : {false, true}) {
      auto Image = makeImage();
      parseThreaded(Image, Opcode,
                    Authenticated ? kAuthenticatedBind : kArm64eBind);
      ASSERT_TRUE(Image.MachOChainedFixupsError.empty());
      if (Authenticated) {
        ASSERT_EQ(Image.MachOAuthenticatedPointers.size(), 1);
        const auto &Recipe = Image.MachOAuthenticatedPointers.at(kStorage);
        EXPECT_EQ(Recipe.LibraryOrdinal, Ordinal);
        EXPECT_EQ(Recipe.Module, kLibrary);
      } else {
        ASSERT_EQ(Image.DyldBindSlots.size(), 1);
        EXPECT_EQ(Image.DyldBindSlots.at(kStorage).LibraryOrdinal, Ordinal);
        EXPECT_EQ(Image.DyldBindSlots.at(kStorage).Module, kLibrary);
      }
    }
  }
}
