//===- PEHeaderMapping.cpp - Checked PE header storage ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Map independently owned original PE headers as non-executable data.
//===----------------------------------------------------------------------===//

#include "PEHeaderMapping.h"

#include "PEHeaderInitialization.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/object/PELayout.h"

#include <algorithm>
#include <cstring>

namespace neverd::coff_loader {
namespace {
constexpr uint64_t PageSize = 4096;
/// A PE image's preferred base is a multiple of 64 KiB.
constexpr uint64_t ImageBaseAlignment = 65536;
constexpr char HeaderName[] = "<PE headers>";

PEHeaderPtrs locateHeaders(llvm::ArrayRef<uint8_t> Bytes) {
  // locatePEHeaders is read-only; its mutable pointer is also used by writers.
  return Bytes.size() >= 2 && Bytes[0] == 'M' && Bytes[1] == 'Z'
             ? locatePEHeaders(const_cast<uint8_t *>(Bytes.data()),
                               Bytes.size())
             : PEHeaderPtrs{};
}
} // namespace

uint32_t getPEHeaderBytes(llvm::ArrayRef<uint8_t> Bytes) {
  const auto PE = locateHeaders(Bytes);
  if (!PE.valid() || !PE.SectionTable)
    return 0;
  const uint64_t Base = getPEImageBase(PE);
  uint64_t Size =
      std::min<uint64_t>(getPESizeOfHeaders(PE), getPESizeOfImage(PE));
  if (!Base || Base % ImageBaseAlignment || Size > InvalidVA - Base ||
      (!PE.Is64 && Base + Size > (uint64_t{1} << 32)))
    return 0;
  // A section's mapping owns every address it covers.
  forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
    if (getPESectionContentSize(Section.VirtualSize, Section.SizeOfRawData))
      Size = std::min<uint64_t>(Size, Section.VirtualAddress);
  });
  return static_cast<uint32_t>(Size);
}

llvm::Expected<PEHeaderMapping>
getPEHeaderMapping(llvm::ArrayRef<uint8_t> Bytes,
                   PEHeaderInitializationWork *Work) {
  const auto PE = locateHeaders(Bytes);
  if (!PE.valid() || !PE.SectionTable)
    return llvm::createStringError("missing complete PE header inventory");
  const uint64_t Base = getPEImageBase(PE);
  const uint64_t Size = getPESizeOfHeaders(PE);
  const uint64_t ImageSize = getPESizeOfImage(PE);
  const uint64_t FileAlignment = getPEFileAlignment(PE);
  const uint64_t SectionAlignment = getPESectionAlignment(PE);
  const uint64_t TableEnd =
      PE.SectionTable - Bytes.data() +
      uint64_t(PE.NumSections) * sizeof(llvm::object::coff_section);
  if (!Size || Size < TableEnd || Size > Bytes.size() || Size > ImageSize ||
      ImageSize > InvalidVA - Base ||
      (!PE.Is64 && Base + ImageSize > (uint64_t{1} << 32)))
    return llvm::createStringError("invalid PE header extent");
  if (!llvm::isPowerOf2_64(FileAlignment) || FileAlignment < 512 ||
      FileAlignment > 65536 || !llvm::isPowerOf2_64(SectionAlignment) ||
      SectionAlignment < PageSize || SectionAlignment < FileAlignment ||
      Base % ImageBaseAlignment || Size % FileAlignment ||
      ImageSize % SectionAlignment)
    return llvm::createStringError(
        "PE headers require an aligned, separate page mapping");

  const uint64_t HeaderPages = llvm::alignTo(Size, PageSize);
  bool Valid = HeaderPages <= ImageSize;
  forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
    const uint64_t RVA = Section.VirtualAddress;
    const uint64_t File = Section.PointerToRawData;
    const uint64_t Content =
        getPESectionContentSize(Section.VirtualSize, Section.SizeOfRawData);
    const uint64_t Raw = Section.SizeOfRawData;
    // A section sharing a header page can change its protection. Raw aliases
    // can expose the same metadata through a separately writable mapping.
    if (Content)
      Valid &= RVA >= HeaderPages && RVA % SectionAlignment == 0 &&
               RVA <= ImageSize && Content <= ImageSize - RVA;
    if (Raw)
      Valid &=
          File >= Size && File <= Bytes.size() && Raw <= Bytes.size() - File;
  });
  if (!Valid)
    return llvm::createStringError(
        "PE headers overlap a section or have invalid section extents");
  PEHeaderInitializationWork LocalWork;
  if (!hasBoundedPEHeaderInitialization(Bytes, PE, Work ? *Work : LocalWork))
    return llvm::createStringError(
        "PE header initialization has an unproven loader write footprint");
  const uint64_t ImageBaseOffset =
      PE.OptionalHeader - Bytes.data() +
      (PE.Is64 ? offsetof(llvm::object::pe32plus_header, ImageBase)
               : offsetof(llvm::object::pe32_header, ImageBase));
  return PEHeaderMapping{Base, uint32_t(Size), uint32_t(FileAlignment),
                         uint32_t(ImageBaseOffset)};
}

void appendPEHeaderMapping(BinaryImage &Image, const PEHeaderMapping &Mapping,
                           llvm::ArrayRef<uint8_t> Bytes) {
  Segment Header;
  Header.Name = HeaderName;
  Header.VA = Mapping.Base;
  Header.Size = Header.FileSz = Mapping.Size;
  Header.Flags = SegmentFlags::Readable;
  Header.Data.assign(Bytes.begin(), Bytes.begin() + Mapping.Size);
  Section Region;
  Region.Name = HeaderName;
  Region.VA = Header.VA;
  Region.Size = Region.FileSz = Header.Size;
  Region.Flags = Header.Flags;
  Region.Alignment = Mapping.FileAlignment;
  Region.IsImageHeader = true;
  if (Image.LoadOnlyFunctionEntries.empty())
    Region.Data = Header.Data;
  // Retain every real COFF section's existing index.
  Image.Segments.push_back(std::move(Header));
  Image.Sections.push_back(std::move(Region));
  // A loaded header's ImageBase is an address, including on loaders that
  // replace it when rebasing. Preserve that identity when mirroring headers;
  // never publish a fictitious entry in the file's base-relocation inventory.
  const va_t Slot = Mapping.Base + Mapping.ImageBaseOffset;
  Image.DataPtrRelocSlots.insert(Slot);
  Image.DataPtrRelocTargetOwners.emplace(Slot, Mapping.Base);
}

bool matchesPEHeaderMapping(const BinaryImage &Image, size_t Index,
                            const PEHeaderMapping &Mapping,
                            llvm::ArrayRef<uint8_t> Bytes) {
  if (Index >= Image.Segments.size() || Index >= Image.Sections.size() ||
      Mapping.Size > Bytes.size())
    return false;
  const auto Matches = [&](const auto &Region) {
    return Region.Name == HeaderName && Region.VA == Mapping.Base &&
           Region.Size == Mapping.Size && Region.FileOff == 0 &&
           Region.FileSz == Mapping.Size &&
           Region.Flags == SegmentFlags::Readable &&
           Region.Data.size() == Mapping.Size &&
           std::memcmp(Region.Data.data(), Bytes.data(), Mapping.Size) == 0;
  };
  const auto &Segment = Image.Segments[Index];
  const auto &Section = Image.Sections[Index];
  return Matches(Segment) && Matches(Section) &&
         !Segment.ReadOnlyAfterRelocations && Section.IsImageHeader &&
         Section.Type == 0 && Section.SegmentName.empty() &&
         Section.Alignment == Mapping.FileAlignment;
}
} // namespace neverd::coff_loader
