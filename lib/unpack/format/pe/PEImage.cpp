//===- PEImage.cpp - Validated PE32+ header facts -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "PEImage.h"

#include "neverd/object/PELayout.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/MathExtras.h"

#include <cstring>

namespace neverd::unpack::pe {
using namespace llvm::object;
namespace {
template <typename Record>
bool load(llvm::ArrayRef<uint8_t> File, uint64_t Offset, Record &Out) {
  if (Offset > File.size() || File.size() - Offset < sizeof(Record))
    return false;
  std::memcpy(&Out, File.data() + Offset, sizeof(Record));
  return true;
}

std::optional<emulation::GuestArchitecture> architectureOf(uint16_t Machine) {
  switch (Machine) {
#define NEVERD_UNPACK_PE_MACHINE(COFFName, Architecture)                       \
  case llvm::COFF::COFFName:                                                   \
    return emulation::GuestArchitecture::Architecture;
#include "PE.def"
#undef NEVERD_UNPACK_PE_MACHINE
  default:
    return std::nullopt;
  }
}
} // namespace

data_directory Image::directory(unsigned Index) const {
  return Index < H.Directories.size() ? H.Directories[Index] : data_directory{};
}

llvm::Expected<std::unique_ptr<Image>>
Image::read(llvm::ArrayRef<uint8_t> File) {
  dos_header DOS;
  if (!load(File, 0, DOS) ||
      std::memcmp(DOS.Magic, value::DOSMagic, sizeof(value::DOSMagic)))
    return failure(text::NotPE);
  const uint64_t Signature = DOS.AddressOfNewExeHeader;
  if (Signature > File.size() ||
      File.size() - Signature < sizeof(llvm::COFF::PEMagic) ||
      std::memcmp(File.data() + Signature, llvm::COFF::PEMagic,
                  sizeof(llvm::COFF::PEMagic)))
    return failure(text::NotPE);
  coff_file_header COFF;
  pe32plus_header PE;
  const uint64_t FileHeader = Signature + sizeof(llvm::COFF::PEMagic);
  const uint64_t Optional = FileHeader + sizeof(COFF);
  if (!load(File, FileHeader, COFF) || !load(File, Optional, PE))
    return failure(text::Truncated);
  const auto Architecture = architectureOf(COFF.Machine);
  if (!Architecture || PE.Magic != llvm::COFF::PE32Header::PE32_PLUS)
    return failure(text::Machine);
  const uint64_t Directories = PE.NumberOfRvaAndSize;
  if (Directories > value::MaxDirectories ||
      COFF.SizeOfOptionalHeader <
          sizeof(PE) + Directories * sizeof(data_directory) ||
      !COFF.NumberOfSections || COFF.NumberOfSections > value::MaxSections)
    return failure(text::Truncated);
  const uint64_t Table = Optional + COFF.SizeOfOptionalHeader;
  if (Table > File.size() ||
      (File.size() - Table) / sizeof(coff_section) < COFF.NumberOfSections)
    return failure(text::Truncated);
  if (!llvm::isPowerOf2_32(PE.SectionAlignment) ||
      !llvm::isPowerOf2_32(PE.FileAlignment) ||
      PE.SectionAlignment < unpack::value::PageSize ||
      PE.FileAlignment < value::MinFileAlignment ||
      PE.FileAlignment > value::MaxFileAlignment ||
      PE.FileAlignment > PE.SectionAlignment)
    return failure(text::Alignment);
  const uint64_t TableEnd =
      Table + uint64_t(COFF.NumberOfSections) * sizeof(coff_section);
  if (!PE.SizeOfImage || PE.SizeOfImage % PE.SectionAlignment ||
      PE.SizeOfHeaders < TableEnd || PE.SizeOfHeaders > File.size() ||
      PE.SizeOfHeaders > PE.SizeOfImage ||
      PE.AddressOfEntryPoint >= PE.SizeOfImage)
    return failure(text::Truncated);
  std::unique_ptr<Image> Out(new Image(File));
  Out->Architecture = *Architecture;
  const bool Kernel = PE.Subsystem == llvm::COFF::IMAGE_SUBSYSTEM_NATIVE;
  Out->Domain = Kernel ? ExecutionDomain::Kernel : ExecutionDomain::User;
  Out->Base = PE.ImageBase;
  Out->Extent = PE.SizeOfImage;
  Out->EntryRVA = PE.AddressOfEntryPoint;
  Headers &H = Out->H;
  H.SizeOfHeaders = PE.SizeOfHeaders;
  H.SectionAlignment = PE.SectionAlignment;
  H.FileAlignment = PE.FileAlignment;
  H.FileHeaderOffset = FileHeader;
  H.OptionalHeaderOffset = Optional;
  H.SectionTableOffset = Table;
  H.Directories.resize(Directories);
  for (uint64_t I = 0; I < Directories; ++I)
    load(File, Optional + sizeof(PE) + I * sizeof(data_directory),
         H.Directories[I]);
  uint64_t Next =
      llvm::alignTo(uint64_t(PE.SizeOfHeaders), uint64_t(PE.SectionAlignment));
  for (unsigned I = 0; I < COFF.NumberOfSections; ++I) {
    coff_section Raw;
    load(File, Table + I * sizeof(Raw), Raw);
    ImageRegion R;
    R.Name = llvm::StringRef(Raw.Name, value::SectionNameBytes)
                 .take_until([](char C) { return !C; })
                 .str();
    R.RVA = Raw.VirtualAddress;
    R.FileSize = Raw.SizeOfRawData;
    // The Windows loader reads section data from a sector boundary.
    R.FileOffset = R.FileSize ? uint32_t(Raw.PointerToRawData) : 0;
    if (!Kernel)
      R.FileOffset &= ~uint64_t(value::MinFileAlignment - 1);
    R.MemorySize =
        Kernel
            ? getPEDriverSectionMappedSize(Raw.VirtualSize, Raw.SizeOfRawData,
                                           unpack::value::PageSize)
            : getPEUserSectionMappedSize(Raw.VirtualSize, Raw.SizeOfRawData,
                                         unpack::value::PageSize);
    if (!R.MemorySize || R.RVA % PE.SectionAlignment || R.RVA < Next ||
        R.RVA >= PE.SizeOfImage || R.MemorySize > PE.SizeOfImage - R.RVA)
      return failure(text::Sections);
    if (R.FileSize &&
        (R.FileOffset > File.size() || R.FileSize > File.size() - R.FileOffset))
      return failure(text::SectionData);
    Next = R.RVA + R.MemorySize;
    H.Sections.push_back({Raw.VirtualSize, Raw.Characteristics});
    Out->Regions.push_back(std::move(R));
  }
  return Out;
}
} // namespace neverd::unpack::pe
