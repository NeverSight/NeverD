//===- PEHeaderMappingTests.cpp - PE header mapping tests ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// File ownership, initialization and immutable-read boundaries for PE headers.
//===----------------------------------------------------------------------===//

#include "PEHeaderFixture.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/COFF/PEHeaderMapping.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/object/PELayout.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

namespace neverd {
namespace {
using namespace llvm::COFF;
using namespace llvm::object;

llvm::Expected<BinaryImage> load(const std::string &Bytes, bool Restricted) {
  COFFLoader Loader;
  if (Restricted)
    Loader.restrictFunctions({0x401000});
  return Loader.loadBuffer(llvm::MemoryBufferRef(Bytes, "header-fixture.exe"));
}

TEST(PEHeaderMapping, OriginalHeadersAreReadableInFullAndRestrictedImages) {
  for (const auto [Is64, Machine] :
       {std::pair{false, uint16_t(IMAGE_FILE_MACHINE_I386)},
        {true, uint16_t(IMAGE_FILE_MACHINE_AMD64)},
        {false, uint16_t(IMAGE_FILE_MACHINE_ARMNT)},
        {true, uint16_t(IMAGE_FILE_MACHINE_ARM64)}})
    for (bool Restricted : {false, true})
      for (bool LongStub : {false, true})
        for (bool ZeroVirtualSize : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << Machine << ':' << Restricted << ':' << LongStub << ':'
                       << ZeroVirtualSize);
          auto Bytes = test::peHeaderImage(Is64, Machine, LongStub);
          auto Headers = locatePEHeaders(
              reinterpret_cast<uint8_t *>(Bytes.data()), Bytes.size());
          if (ZeroVirtualSize)
            reinterpret_cast<coff_section *>(Headers.SectionTable)
                ->VirtualSize = 0;
          const auto Size = getPESizeOfHeaders(Headers);
          const auto Base = getPEImageBase(Headers);
          auto Image = load(Bytes, Restricted);
          ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
          EXPECT_EQ(Image->Raw.empty(), Restricted);
          EXPECT_TRUE(coff_loader::hasCheckedPEHeaderMapping(
              *Image, Image->Sections.size() - 1));
          EXPECT_EQ(COFFLoader::hasExpectedPEMappingInventory(
                        *Image, Headers.NumSections),
                    !Restricted);
          const auto *Data = Image->readVA(Base, Size);
          ASSERT_NE(Data, nullptr);
          EXPECT_EQ(std::memcmp(Data, Bytes.data(), Size), 0);
          EXPECT_EQ(Image->readVA(Base + Size, 1), nullptr);
          const auto *Owner = Image->getSectionFor(Base);
          ASSERT_NE(Owner, nullptr);
          EXPECT_TRUE(Owner->IsImageHeader);
          EXPECT_FALSE(Image->Sections.front().IsImageHeader);
          EXPECT_TRUE(Owner->isReadable());
          EXPECT_FALSE(Owner->isWritable());
          EXPECT_FALSE(Owner->isExecutable());
          EXPECT_FALSE(Image->isCodeAddress(Base));
          const size_t BaseOffset = Headers.OptionalHeader -
                                    reinterpret_cast<uint8_t *>(Bytes.data()) +
                                    (Is64 ? offsetof(pe32plus_header, ImageBase)
                                          : offsetof(pe32_header, ImageBase));
          const unsigned Width = Is64 ? 8 : 4;
          for (const auto [Begin, End] :
               {std::pair<size_t, size_t>{0, BaseOffset},
                {BaseOffset + Width, Size}}) {
            const auto Read =
                readImmutableImageBytes(*Image, Base + Begin, End - Begin);
            ASSERT_TRUE(Read);
            EXPECT_EQ(*Read, std::vector<uint8_t>(Bytes.begin() + Begin,
                                                  Bytes.begin() + End));
          }
          EXPECT_FALSE(readImmutableImageBytes(*Image, Base, Size));
          EXPECT_FALSE(
              readImmutableImageBytes(*Image, Base + BaseOffset, Width));
          EXPECT_FALSE(readImmutableImagePointer(*Image, Base + BaseOffset));
          EXPECT_TRUE(Image->DataPtrRelocSlots.count(Base + BaseOffset));
          EXPECT_EQ(Image->DataPtrRelocTargetOwners.at(Base + BaseOffset),
                    Base);
          EXPECT_FALSE(readImmutableImageBytes(*Image, Base + Size - 1, 2));
          // Mapping a header never bypasses an overlapping runtime fixup.
          Image->BaseRelocations.push_back(
              {Base + 0x3c, IMAGE_REL_BASED_HIGHLOW});
          EXPECT_FALSE(readImmutableImageBytes(*Image, Base + 0x3c, 4));
        }
}

TEST(PEHeaderMapping, HeaderIdentityRequiresBothRegionsAndNativeCount) {
  for (bool Restricted : {false, true})
    for (unsigned Mutation = 0; Mutation != 20; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Restricted << ':' << Mutation);
      auto Image =
          load(test::peHeaderImage(true, IMAGE_FILE_MACHINE_AMD64), Restricted);
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      const size_t Count = Image->Sections.size() - 1;
      ASSERT_TRUE(coff_loader::hasCheckedPEHeaderMapping(*Image, Count));
      auto &Section = Image->Sections.back();
      auto &Segment = Image->Segments.back();
      switch (Mutation) {
      case 0:
        Section.Name += "changed";
        break;
      case 1:
        Segment.Name += "changed";
        break;
      case 2:
        ++Section.VA;
        break;
      case 3:
        ++Segment.VA;
        break;
      case 4:
        ++Section.FileOff;
        break;
      case 5:
        ++Segment.FileSz;
        break;
      case 6:
        Section.Flags = Section.Flags | SegmentFlags::Writable;
        break;
      case 7:
        Segment.Flags = Segment.Flags | SegmentFlags::Writable;
        break;
      case 8:
        Section.Type = IMAGE_SCN_CNT_INITIALIZED_DATA;
        break;
      case 9:
        ++Section.Alignment;
        break;
      case 10:
        Segment.Data[0x3c] ^= 1;
        break;
      case 11:
        Section.Data.assign(1, 0);
        break;
      case 12:
        if (Image->Raw.empty())
          Segment.Data[0] = 0;
        else
          Image->Raw[0] = 0;
        break;
      case 13:
        Image->LoadOnlyFunctionEntries.clear();
        Section.Data.clear();
        break;
      case 14:
        Image->Segments.pop_back();
        break;
      case 15:
        Image->Segments.push_back(Segment);
        Image->Sections.push_back(Section);
        break;
      case 16:
        Image->Format = BinaryFormat::ELF;
        break;
      case 17:
        ++Image->Base;
        break;
      case 18:
        if (Image->Raw.empty())
          Segment.Data[0x86] = 2;
        else
          Image->Raw[0x86] = 2;
        break;
      case 19:
        Segment.ReadOnlyAfterRelocations = true;
        break;
      }
      EXPECT_FALSE(coff_loader::hasCheckedPEHeaderMapping(*Image, Count));
    }
}

TEST(PEHeaderMapping, InventoryRejectsMissingDuplicateOrChangedHeaderIdentity) {
  for (const auto [Is64, Machine] :
       {std::pair{false, uint16_t(IMAGE_FILE_MACHINE_I386)},
        {true, uint16_t(IMAGE_FILE_MACHINE_AMD64)},
        {true, uint16_t(IMAGE_FILE_MACHINE_ARM64)}})
    for (unsigned Mutation = 0; Mutation != 24; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Machine << ':' << Mutation);
      auto Bytes = test::peHeaderImage(Is64, Machine);
      auto Image = load(Bytes, false);
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      const size_t Count = Image->Sections.size() - 1;
      ASSERT_TRUE(COFFLoader::hasExpectedPEMappingInventory(*Image, Count));
      EXPECT_FALSE(
          COFFLoader::hasExpectedPEMappingInventory(*Image, Count + 1));
      EXPECT_FALSE(COFFLoader::hasExpectedPEMappingInventory(*Image, 0));
      auto &Segment = Image->Segments.back();
      auto &Section = Image->Sections.back();
      switch (Mutation) {
      case 0:
        Image->Segments.pop_back();
        break;
      case 1:
        Image->Sections.pop_back();
        break;
      case 2:
        Image->Segments.pop_back();
        Image->Sections.pop_back();
        break;
      case 3:
        Image->Segments.push_back(Segment);
        Image->Sections.push_back(Section);
        break;
      case 4:
        Segment.Data.back() ^= 1;
        break;
      case 5:
        Section.Data.back() ^= 1;
        break;
      case 6:
        Segment.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
        break;
      case 7:
        Section.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
        break;
      case 8:
        Section.IsImageHeader = false;
        break;
      case 9:
        Image->Sections.front().IsImageHeader = true;
        break;
      case 10:
        std::swap(Image->Segments.front(), Segment);
        std::swap(Image->Sections.front(), Section);
        break;
      case 11:
        Segment.FileOff = 1;
        break;
      case 12:
        ++Section.VA;
        break;
      case 13:
        Section.Type = IMAGE_SCN_CNT_INITIALIZED_DATA;
        break;
      case 14:
        Segment.ReadOnlyAfterRelocations = true;
        break;
      case 15:
        Section.Alignment = 1;
        break;
      case 16:
        Image->Raw[sizeof(dos_header)] ^= 1;
        break;
      case 17:
        Section.SegmentName = "other";
        break;
      case 18:
        Segment.Name = ".data";
        break;
      case 19:
        --Section.FileSz;
        break;
      case 20:
        ++Segment.Size;
        break;
      case 21:
        Image->Raw.clear();
        break;
      case 22:
        Image->Format = BinaryFormat::ELF;
        break;
      case 23:
        Image->IsRelocatable = true;
        break;
      }
      EXPECT_FALSE(COFFLoader::hasExpectedPEMappingInventory(*Image, Count));
    }
}

TEST(PEHeaderMapping, AmbiguousOrMalformedHeadersPublishNoHeaderRegion) {
  for (bool Is64 : {false, true})
    for (bool Restricted : {false, true})
      for (unsigned Mutation = 0; Mutation != 19; ++Mutation) {
        SCOPED_TRACE(::testing::Message()
                     << Is64 << ':' << Restricted << ':' << Mutation);
        auto Bytes = test::peHeaderImage(Is64, Is64 ? IMAGE_FILE_MACHINE_AMD64
                                                    : IMAGE_FILE_MACHINE_I386);
        auto Headers = locatePEHeaders(
            reinterpret_cast<uint8_t *>(Bytes.data()), Bytes.size());
        auto *Section = reinterpret_cast<coff_section *>(Headers.SectionTable);
        const auto Set = [&](auto *PE) {
          switch (Mutation) {
          case 0:
            PE->SizeOfHeaders = 0;
            break;
          case 1:
            PE->SizeOfHeaders = 0x180; // Section table is outside the extent.
            break;
          case 2:
            PE->SizeOfHeaders = 0x600; // Missing file-backed bytes.
            break;
          case 3:
            PE->FileAlignment = 0;
            break;
          case 4:
            PE->FileAlignment = 768;
            break;
          case 5:
            PE->SectionAlignment = 512;
            break;
          case 6:
            PE->SizeOfHeaders = 0x201;
            break;
          case 7:
            Section->VirtualAddress = 0x200; // Separate bytes, shared page.
            Section->Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
            break;
          case 8:
            Section->VirtualAddress = 0;
            break;
          case 9:
            Section->PointerToRawData = 0x100;
            break;
          case 10:
            Section->SizeOfRawData = 0x400;
            break;
          case 11:
            PE->SizeOfImage = 0x1000;
            break;
          case 12:
            PE->SizeOfImage = 0x2001;
            break;
          case 13:
            if constexpr (sizeof(PE->ImageBase) == 8)
              PE->ImageBase = UINT64_MAX - 0xffff;
            else
              PE->ImageBase = UINT32_MAX - 0xffff;
            PE->SizeOfImage = 0x20000;
            break;
          case 14:
            Section->VirtualAddress = UINT32_MAX;
            break;
          case 15:
            Section->VirtualSize = UINT32_MAX;
            break;
          case 16:
            PE->ImageBase = 0x400001;
            break;
          case 17:
            PE->SectionAlignment = 0;
            break;
          case 18:
            PE->FileAlignment = 0x2000; // Larger than section alignment.
            break;
          }
        };
        if (Is64)
          Set(reinterpret_cast<pe32plus_header *>(Headers.OptionalHeader));
        else
          Set(reinterpret_cast<pe32_header *>(Headers.OptionalHeader));
        auto Image = load(Bytes, Restricted);
        if (!Image) {
          llvm::consumeError(Image.takeError());
          continue;
        }
        EXPECT_TRUE(llvm::none_of(Image->Segments, [](const auto &Segment) {
          return Segment.Name == "<PE headers>";
        }));
        EXPECT_TRUE(llvm::none_of(Image->Sections, [](const auto &Section) {
          return Section.Name == "<PE headers>";
        }));
        if (!Restricted)
          EXPECT_TRUE(COFFLoader::hasExpectedPEMappingInventory(
              *Image, Headers.NumSections));
        EXPECT_TRUE(llvm::any_of(Image->LoadDiagnostics, [](const auto &D) {
          return D.Code == "pe.headers_unmapped";
        }));
      }
}

TEST(PEHeaderMapping, LoaderWritesCannotSupplyImmutableHeaderBytes) {
  for (bool Is64 : {false, true})
    for (unsigned Mutation = 0; Mutation != 33; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Is64 << ':' << Mutation);
      auto Bytes = test::peHeaderImage(Is64, Is64 ? IMAGE_FILE_MACHINE_AMD64
                                                  : IMAGE_FILE_MACHINE_I386);
      // Discard the source fixture's unrelated canary before laying out tables.
      std::fill(Bytes.begin() + 0x210, Bytes.end(), 0);
      auto *Data = reinterpret_cast<uint8_t *>(Bytes.data());
      auto PE = locatePEHeaders(Data, Bytes.size());
      auto *Section = reinterpret_cast<coff_section *>(PE.SectionTable);
      Section->VirtualSize = 512;
      const auto Base = getPEImageBase(PE);
      const unsigned Width = Is64 ? 8 : 4;
      const auto Put32 = [&](size_t At, uint32_t Value) {
        writeLE<uint32_t>(Data + At, Value);
      };
      const auto PutPointer = [&](size_t At, uint64_t Value) {
        if (Is64)
          writeLE<uint64_t>(Data + At, Value);
        else
          Put32(At, Value);
      };
      // One ordinary import, an independent lookup array and its IAT.
      setPEDataDirectory(PE, IMPORT_TABLE, 0x1010, 40);
      setPEDataDirectory(PE, IAT, 0x10a0, 2 * Width);
      Put32(0x210, 0x1080);
      Put32(0x21c, 0x1060);
      Put32(0x220, 0x10a0);
      std::memcpy(Data + 0x260, "example.dll", 12);
      PutPointer(0x280, (uint64_t{1} << (Width * 8 - 1)) | 1);
      PutPointer(0x2a0, (uint64_t{1} << (Width * 8 - 1)) | 1);
      // Legacy load configuration may initialize its cookie, outside headers.
      const size_t ConfigSize =
          Is64 ? offsetof(coff_load_configuration64, GuardCFCheckFunction)
               : offsetof(coff_load_configuration32, GuardCFCheckFunction);
      const size_t Cookie =
          Is64 ? offsetof(coff_load_configuration64, SecurityCookie)
               : offsetof(coff_load_configuration32, SecurityCookie);
      const size_t Lock =
          Is64 ? offsetof(coff_load_configuration64, LockPrefixTable)
               : offsetof(coff_load_configuration32, LockPrefixTable);
      const size_t Edit = Is64 ? offsetof(coff_load_configuration64, EditList)
                               : offsetof(coff_load_configuration32, EditList);
      setPEDataDirectory(PE, LOAD_CONFIG_TABLE, 0x10c0, Is64 ? ConfigSize : 64);
      Put32(0x2c0, ConfigSize);
      PutPointer(0x2c0 + Cookie, Base + 0x11c0);
      setPEDataDirectory(PE, BASE_RELOCATION_TABLE, 0x1140, 16);
      Put32(0x340, 0x1000);
      Put32(0x344, 16);
      const uint16_t Kind =
          Is64 ? IMAGE_REL_BASED_DIR64 : IMAGE_REL_BASED_HIGHLOW;
      writeLE<uint16_t>(Data + 0x348, (Kind << 12) | 0x180);
      writeLE<uint16_t>(Data + 0x34a, (Kind << 12) | (0xc0 + Cookie));
      PutPointer(0x380, Base + 0x1188);
      switch (Mutation) {
      case 1:
        Put32(0x220, 0x40); // IAT writes to headers.
        break;
      case 2:
        Put32(0x210, 0x40); // Unsupported header-resident lookup metadata.
        break;
      case 3:
        setPEDataDirectory(PE, IMPORT_TABLE, 0x40, 40);
        break;
      case 4:
        Put32(0x220, 0);
        break;
      case 5:
        setPEDataDirectory(PE, IMPORT_TABLE, 0x1010, 20);
        break;
      case 6:
        Put32(0x210, 0x11f8);
        PutPointer(0x3f8, 1);
        Put32(0x3fc, 1); // No complete null lookup before section end.
        break;
      case 7:
        Put32(0x340, 0);
        writeLE<uint16_t>(Data + 0x348, (Kind << 12) | 0x20);
        break;
      case 8:
        PutPointer(0x2c0 + Cookie, Base + 0x3c);
        break;
      case 9:
        PutPointer(0x2c0 + Cookie, Base - 2);
        break;
      case 10:
        PutPointer(0x2c0 + Lock, Base + 0x11f0);
        break;
      case 11:
        PutPointer(0x2c0 + Edit, Base + 0x11f0);
        break;
      case 12:
        Put32(0x2c0, ConfigSize + 4);
        break;
      case 13:
        Put32(0x2c0, Cookie + 2);
        break;
      case 14:
      case 15:
      case 16:
      case 17: {
        const unsigned Directories[] = {TLS_TABLE, BOUND_IMPORT,
                                        DELAY_IMPORT_DESCRIPTOR,
                                        CLR_RUNTIME_HEADER};
        setPEDataDirectory(PE, Directories[Mutation - 14], 0x1180, 32);
        break;
      }
      case 18:
        setPEDataDirectory(PE, IAT, UINT32_MAX - 15, 32);
        break;
      case 19:
        writeLE<uint16_t>(Data + 0x348,
                          (IMAGE_REL_BASED_HIGHADJ << 12) | 0x180);
        break;
      case 20:
        PE.FileHeader->NumberOfSections = 2;
        Section[1] = Section[0]; // Ambiguous metadata owner.
        break;
      case 21:
        // Ordinary imports may use the IAT itself as their lookup array.
        Put32(0x210, 0);
        break;
      case 22:
        // ABSOLUTE relocation padding has no write footprint.
        writeLE<uint16_t>(Data + 0x348, 0x20);
        break;
      case 23:
        Put32(0x220, 0x1020); // IAT replaces a later descriptor.
        break;
      case 24:
        Put32(0x220, 0x1080 + Width); // IAT replaces a lookup terminator.
        break;
      case 25:
        writeLE<uint16_t>(Data + 0x348, (Kind << 12) | 0x20);
        break;
      case 26:
        writeLE<uint16_t>(Data + 0x348, (Kind << 12) | 0xc0);
        break;
      case 27:
        writeLE<uint16_t>(Data + 0x34a, 0); // Cookie would not move on rebase.
        break;
      case 28:
        writeLE<uint16_t>(Data + 0x34a, (Kind << 12) | (0xc0 + Cookie - 1));
        break;
      case 29:
        PutPointer(0x2c0 + Cookie, Base + 0x3000);
        break;
      case 30:
        PutPointer(0x2c0 + Cookie, 0);
        writeLE<uint16_t>(Data + 0x348, 0x20);
        writeLE<uint16_t>(Data + 0x34a, 0x24);
        Put32(0x340, 0);
        break;
      case 31:
        // An independent ordinary relocation may initialize the cookie value.
        writeLE<uint16_t>(Data + 0x348, (Kind << 12) | 0x1c0);
        break;
      case 32: {
        const size_t SEH =
            Is64 ? offsetof(coff_load_configuration64, SEHandlerTable)
                 : offsetof(coff_load_configuration32, SEHandlerTable);
        PutPointer(0x2c0 + SEH, Base + 0x11a0);
        writeLE<uint16_t>(Data + 0x348, (Kind << 12) | (0xc0 + SEH));
        break;
      }
      }
      const bool Expected =
          Mutation == 0 || Mutation == 21 || Mutation == 22 || Mutation >= 30;
      auto Image = load(Bytes, false);
      if (!Image) {
        const auto Error = llvm::toString(Image.takeError());
        EXPECT_FALSE(Expected) << Error;
        continue;
      }
      EXPECT_EQ(Image->readVA(Base, 2) != nullptr, Expected);
      EXPECT_EQ(bool(readImmutableImageBytes(*Image, Base, 2)), Expected);
    }
}

TEST(PEHeaderMapping, InitializationExhaustionDoesNotPublishPartialHeaders) {
  for (bool Is64 : {false, true}) {
    auto Bytes = test::peHeaderImage(Is64, Is64 ? IMAGE_FILE_MACHINE_AMD64
                                                : IMAGE_FILE_MACHINE_I386);
    constexpr uint32_t Extent = limits::kMaxPEHeaderInitializationWork;
    Bytes.resize(Extent + 512, 0);
    auto PE = locatePEHeaders(reinterpret_cast<uint8_t *>(Bytes.data()),
                              Bytes.size());
    const auto Set = [&](auto *Header) {
      Header->SizeOfHeaders = Extent;
      Header->SizeOfImage = Extent + 4096;
      Header->AddressOfEntryPoint = Extent;
    };
    if (Is64)
      Set(reinterpret_cast<pe32plus_header *>(PE.OptionalHeader));
    else
      Set(reinterpret_cast<pe32_header *>(PE.OptionalHeader));
    auto *Section = reinterpret_cast<coff_section *>(PE.SectionTable);
    Section->VirtualAddress = Extent;
    Section->PointerToRawData = Extent;
    auto Image = load(Bytes, false);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(Image->readVA(getPEImageBase(PE), 2), nullptr);
    EXPECT_TRUE(llvm::any_of(Image->LoadDiagnostics, [](const auto &D) {
      return D.Code == "pe.headers_unmapped";
    }));
  }
}

} // namespace
} // namespace neverd
