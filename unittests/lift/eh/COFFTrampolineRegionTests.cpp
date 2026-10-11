//===- COFFTrampolineRegionTests.cpp - Checked PE source storage ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check independent executable sections and malformed virtual/raw aliases
/// before any source trampoline can be installed.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/codegen/COFF/COFFTrampolineRegions.h"
#include "gtest/gtest.h"

#include "neverd/object/PELayout.h"
#include "neverd/support/TargetCodegenInfo.h"

#include <algorithm>
#include <cstring>

namespace {
using namespace neverd;

struct SourceSections {
  std::vector<uint8_t> Bytes = std::vector<uint8_t>(0x800);
  BinaryImage Image;
  std::map<std::string, uint64_t> Entries;

  explicit SourceSections(Arch Architecture) {
    Image.Arch = Architecture;
    Image.Format = BinaryFormat::COFF;
    Image.Base = 0x400000;
    Image.Entry = Image.Base + 0x1000;
    Image.Mode = Architecture == Arch::ARM ? InstructionMode::Thumb
                                           : InstructionMode::Default;
    const bool Wide =
        Architecture == Arch::X64 || Architecture == Arch::AArch64;
    Image.Bits = Wide ? Bitness::Bits64 : Bitness::Bits32;
    auto *DOS = reinterpret_cast<llvm::object::dos_header *>(Bytes.data());
    DOS->AddressOfNewExeHeader = 0x80;
    std::memcpy(Bytes.data() + 0x80, llvm::COFF::PEMagic, 4);
    auto *File =
        reinterpret_cast<llvm::object::coff_file_header *>(Bytes.data() + 0x84);
    File->NumberOfSections = 2;
    File->SizeOfOptionalHeader = Wide ? sizeof(llvm::object::pe32plus_header)
                                      : sizeof(llvm::object::pe32_header);
    if (Wide) {
      auto *Optional =
          reinterpret_cast<llvm::object::pe32plus_header *>(File + 1);
      Optional->Magic = llvm::COFF::PE32Header::PE32_PLUS;
      Optional->ImageBase = Image.Base;
      Optional->SizeOfHeaders = 0x400;
    } else {
      auto *Optional = reinterpret_cast<llvm::object::pe32_header *>(File + 1);
      Optional->Magic = llvm::COFF::PE32Header::PE32;
      Optional->ImageBase = Image.Base;
      Optional->SizeOfHeaders = 0x400;
    }
    for (unsigned I = 0; I != 2; ++I) {
      auto &Header = section(I);
      std::memcpy(Header.Name, I ? ".ndtext" : ".text", I ? 7 : 5);
      Header.VirtualAddress = 0x1000 + I * 0x2000;
      Header.VirtualSize = 0x200;
      Header.SizeOfRawData = 0x200;
      Header.PointerToRawData = 0x400 + I * 0x200;
      Header.Characteristics =
          llvm::COFF::IMAGE_SCN_MEM_EXECUTE | llvm::COFF::IMAGE_SCN_MEM_READ;
      Segment Code;
      Code.VA = Image.Base + Header.VirtualAddress;
      Code.FileOff = Header.PointerToRawData;
      Code.Size = Code.FileSz = 0x200;
      Code.Flags = SegmentFlags::Executable | SegmentFlags::Readable;
      Code.Data.assign(0x200, 0xcc);
      std::copy(Code.Data.begin(), Code.Data.end(),
                Bytes.begin() + Code.FileOff);
      Entries.emplace(I ? "second" : "first", Code.VA);
      Image.KnownCodeRanges.emplace_back(Code.VA, Code.VA + Code.Size);
      Image.Segments.push_back(std::move(Code));
    }
  }

  llvm::object::coff_section &section(unsigned Index) {
    auto PE = locatePEHeaders(Bytes.data(), Bytes.size());
    return reinterpret_cast<llvm::object::coff_section *>(
        PE.SectionTable)[Index];
  }
};

TEST(COFFTrampolineRegions, ResolveEveryExecutableOwnerAcrossArchitectures) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    SourceSections F(Architecture);
    auto Regions =
        collectCOFFSourceTrampolineRegions(F.Bytes, F.Image, F.Entries);
    ASSERT_TRUE(bool(Regions)) << llvm::toString(Regions.takeError());
    ASSERT_EQ(Regions->size(), 2u);
    for (unsigned I = 0; I != 2; ++I) {
      EXPECT_EQ((*Regions)[I].SectionVA, 0x1000u + I * 0x2000u);
      EXPECT_EQ((*Regions)[I].SectionFileoff, 0x400u + I * 0x200u);
      EXPECT_EQ(
          (*Regions)[I].SectionSize,
          getTargetCodegenInfo(Architecture, F.Image.Mode).trampolineSize());
    }
    // A zero VirtualSize uses its raw extent, independent of section spelling.
    F.section(1).VirtualSize = 0;
    std::memcpy(F.section(1).Name, ".other\0\0", 8);
    auto Renamed =
        collectCOFFSourceTrampolineRegions(F.Bytes, F.Image, F.Entries);
    EXPECT_TRUE(bool(Renamed)) << llvm::toString(Renamed.takeError());
  }
}

TEST(COFFTrampolineRegions, RejectAmbiguousTruncatedOrChangedSourceBytes) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    SourceSections F(Arch::X86);
    auto &Section = F.section(1);
    switch (Mutation) {
    case 0:
      Section.Characteristics = llvm::COFF::IMAGE_SCN_MEM_READ;
      break;
    case 1:
      F.section(0).VirtualAddress = Section.VirtualAddress;
      break;
    case 2:
      F.section(0).PointerToRawData = Section.PointerToRawData;
      break;
    case 3:
      Section.SizeOfRawData = 0x400;
      break;
    case 4:
      Section.VirtualSize = 2;
      break;
    case 5:
      Section.SizeOfRawData = 2;
      break;
    case 6:
      Section.PointerToRawData = 0;
      break;
    case 7:
      F.Bytes[0x600] ^= 1;
      break;
    case 8:
      F.Image.KnownCodeRanges.pop_back();
      break;
    case 9:
      F.Image.Base += 0x10000;
      break;
    }
    auto Regions =
        collectCOFFSourceTrampolineRegions(F.Bytes, F.Image, F.Entries);
    EXPECT_FALSE(bool(Regions));
    if (!Regions)
      llvm::consumeError(Regions.takeError());
  }
}
} // namespace
