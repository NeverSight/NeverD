//===- DyldSharedCache.cpp - Validated cache files and slide chains ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read Apple's cache address space without modifying its signing recipes.
/// Layout references: apple-oss-distributions/dyld, include/mach-o/
/// dyld_cache_format.h and fixup-chains.h; v3/v5 page starts are byte offsets,
/// while each encoded next field counts eight-byte pointer slots.
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/DyldSharedCache.h"

#include "neverd/loader/BinaryImageFlags.h"
#include "neverd/loader/MachO/MachOArm64e.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/FilePath.h"
#include "neverd/support/SHA256.h"

#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iterator>
#include <set>
#include <utility>

namespace neverd::macho_loader {
namespace {

#define NEVERD_DYLD_CACHE_VALUE(Type, Name, Value)                             \
  inline constexpr Type Name = Value;
#define NEVERD_DYLD_CACHE_LIMIT(Type, Name, Value)                             \
  inline constexpr Type Name = Value;
#define NEVERD_DYLD_CACHE_FIELD(Name, Low, Width)                              \
  inline constexpr arm64e::PointerField Name{Low, Width};
#define NEVERD_DYLD_CACHE_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "neverd/loader/MachO/DyldSharedCache.def"

using Bytes = llvm::ArrayRef<uint8_t>;
using UUID = std::array<uint8_t, kUUIDBytes>;

llvm::Error fail(const char *Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

Bytes fileBytes(const llvm::MemoryBuffer &File) {
  return llvm::arrayRefFromStringRef(File.getBuffer());
}

struct Subcache {
  UUID Identity{};
  uint64_t VMOffset = 0;
  std::string Suffix;
};

struct Header {
  uint32_t MappingOffset = 0;
  uint32_t MappingCount = 0;
  uint32_t SlideMappingOffset = 0;
  uint32_t SlideMappingCount = 0;
  uint32_t ImagesOffset = 0;
  uint32_t ImagesCount = 0;
  uint64_t RegionStart = 0;
  uint64_t RegionSize = 0;
  UUID Identity{};
  std::vector<Subcache> Subcaches;
};

llvm::Expected<Header> readHeader(Bytes Data) {
  static_assert(sizeof(kMagic) == kMagicBytes);
  if (Data.size() < kMinimumHeaderBytes ||
      std::memcmp(Data.data(), kMagic, kMagicBytes))
    return fail(kMalformedHeader);
  Header H;
  const auto read32 = [&](size_t Offset) {
    return readLE<uint32_t>(Data.data() + Offset);
  };
  const auto read64 = [&](size_t Offset) {
    return readLE<uint64_t>(Data.data() + Offset);
  };
  H.MappingOffset = read32(kMappingOffset);
  H.MappingCount = read32(kMappingCount);
  if (H.MappingOffset < kMinimumHeaderBytes || !H.MappingCount ||
      !rangeInBounds(H.MappingOffset, uint64_t(H.MappingCount) * kMappingBytes,
                     Data.size()))
    return fail(kMalformedHeader);
  if (H.MappingCount > kMaxMappings)
    return fail(kLimitExceeded);
  const auto has = [&](size_t Offset, size_t Size) {
    return rangeInBounds(Offset, Size, H.MappingOffset);
  };
  std::copy_n(Data.data() + kUUIDOffset, H.Identity.size(), H.Identity.begin());
  if (has(kRegionSize, sizeof(uint64_t))) {
    H.RegionStart = read64(kRegionStart);
    H.RegionSize = read64(kRegionSize);
    if (H.RegionSize > InvalidVA - H.RegionStart)
      return fail(kMalformedHeader);
  }
  H.ImagesOffset = read32(kOldImagesOffset);
  H.ImagesCount = read32(kOldImagesCount);
  if (has(kImagesCount, sizeof(uint32_t))) {
    H.ImagesOffset = read32(kImagesOffset);
    H.ImagesCount = read32(kImagesCount);
  }
  if (H.ImagesCount > kMaxImages)
    return fail(kLimitExceeded);
  if (H.ImagesCount &&
      (H.ImagesOffset < H.MappingOffset ||
       !rangeInBounds(H.ImagesOffset, uint64_t(H.ImagesCount) * kImageBytes,
                      Data.size())))
    return fail(kMalformedImages);
  if (has(kSlideMappingCount, sizeof(uint32_t))) {
    H.SlideMappingOffset = read32(kSlideMappingOffset);
    H.SlideMappingCount = read32(kSlideMappingCount);
    if (H.SlideMappingCount &&
        (H.SlideMappingCount != H.MappingCount ||
         H.SlideMappingOffset < H.MappingOffset ||
         !rangeInBounds(H.SlideMappingOffset,
                        uint64_t(H.SlideMappingCount) * kSlideMappingBytes,
                        Data.size())))
      return fail(kMalformedMappings);
  }
  if (has(kSubcacheCount, sizeof(uint32_t))) {
    const uint32_t Offset = read32(kSubcacheOffset);
    const uint32_t Count = read32(kSubcacheCount);
    const bool HasSuffix = has(kCacheSubtype, sizeof(uint32_t));
    const size_t RowSize = HasSuffix ? kSubcacheBytes : kOldSubcacheBytes;
    if (Count >= kMaxFiles)
      return fail(kLimitExceeded);
    if (Count &&
        (Offset < H.MappingOffset ||
         !rangeInBounds(Offset, uint64_t(Count) * RowSize, Data.size())))
      return fail(kMalformedSubcaches);
    std::set<std::string> Suffixes;
    std::set<UUID> Identities{H.Identity};
    for (uint32_t I = 0; I < Count; ++I) {
      const uint8_t *Row = Data.data() + Offset + size_t(I) * RowSize;
      Subcache Entry;
      std::copy_n(Row, Entry.Identity.size(), Entry.Identity.begin());
      Entry.VMOffset = readLE<uint64_t>(Row + kSubcacheVMOffset);
      if (HasSuffix) {
        const char *Start =
            reinterpret_cast<const char *>(Row + kSubcacheSuffix);
        const auto *End = static_cast<const char *>(
            std::memchr(Start, 0, kSubcacheSuffixBytes));
        if (!End || End == Start)
          return fail(kMalformedSubcaches);
        Entry.Suffix.assign(Start, End);
        if (Entry.Suffix.front() != '.' ||
            Entry.Suffix.find_first_of(kForbiddenSuffixBytes) !=
                std::string::npos)
          return fail(kMalformedSubcaches);
      } else {
        Entry.Suffix = llvm::formatv(kLegacySuffix, I + 1).str();
      }
      if (!Entry.VMOffset || !Suffixes.insert(Entry.Suffix).second ||
          !Identities.insert(Entry.Identity).second)
        return fail(kMalformedSubcaches);
      H.Subcaches.push_back(std::move(Entry));
    }
  }
  return H;
}

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
readFile(llvm::StringRef Path) {
  auto Buffer = llvm::MemoryBuffer::getFile(Path, /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (!Buffer)
    return llvm::createStringError(Buffer.getError(),
                                   llvm::Twine(kMissingFile) + Path);
  return std::move(*Buffer);
}

std::pair<uint32_t, uint32_t> slidePageRange(va_t Address, uint32_t PageSize,
                                             uint32_t PageCount, va_t First,
                                             va_t End) {
  const uint64_t Bytes = uint64_t(PageCount) * PageSize;
  const uint64_t Begin = std::min(std::max(First, Address) - Address, Bytes);
  const uint64_t Limit = std::min(std::max(End, Address) - Address, Bytes);
  return {uint32_t(Begin / PageSize),
          uint32_t((Limit + PageSize - 1) / PageSize)};
}

bool overlapsFixup(va_t Slot, va_t First, va_t End) {
  return Slot < End && (Slot >= First || First - Slot < arm64e::kPointerBytes);
}

llvm::Expected<std::vector<DyldCacheFixup>>
readV2Slides(Bytes Data, Bytes Info, va_t Address, va_t First, va_t End,
             const LoadProgress &Progress) {
  if (Info.size() < kV2HeaderBytes)
    return fail(kMalformedSlides);
  const auto read32 = [&](size_t Offset) {
    return readLE<uint32_t>(Info.data() + Offset);
  };
  const uint32_t PageSize = read32(kSlidePageSize);
  const uint32_t PageCount = read32(kV2StartsCount);
  const uint32_t StartsOffset = read32(kV2StartsOffset);
  const uint32_t ExtrasOffset = read32(kV2ExtrasOffset);
  const uint32_t ExtrasCount = read32(kV2ExtrasCount);
  const uint64_t DeltaMask = readLE<uint64_t>(Info.data() + kV2DeltaMask);
  const uint64_t ValueAdd = readLE<uint64_t>(Info.data() + kV2ValueAdd);
  const unsigned LowBit = std::countr_zero(DeltaMask);
  if ((PageSize != arm64e::kSmallPageBytes &&
       PageSize != arm64e::kLargePageBytes) ||
      uint64_t(PageCount) * PageSize > Data.size() ||
      StartsOffset < kV2HeaderBytes ||
      !rangeInBounds(StartsOffset, uint64_t(PageCount) * sizeof(uint16_t),
                     Info.size()) ||
      (ExtrasCount &&
       (ExtrasOffset < kV2HeaderBytes ||
        !rangeInBounds(ExtrasOffset, uint64_t(ExtrasCount) * sizeof(uint16_t),
                       Info.size()))) ||
      !DeltaMask || LowBit < kV2StrideLog2 ||
      !std::has_single_bit((DeltaMask >> LowBit) + 1))
    return fail(kMalformedSlides);
  if (PageCount > kMaxSlidePages)
    return fail(kLimitExceeded);
  std::vector<DyldCacheFixup> Fixups;
  const auto [FirstPage, EndPage] =
      slidePageRange(Address, PageSize, PageCount, First, End);
  for (uint32_t Page = FirstPage; Page < EndPage; ++Page) {
    Progress.check();
    const uint16_t Start = readLE<uint16_t>(Info.data() + StartsOffset +
                                            size_t(Page) * sizeof(uint16_t));
    if (Start == kV2NoRebase)
      continue;
    if (Start & kV2NoRebase)
      return fail(kMalformedSlides);
    const uint64_t PageOffset = uint64_t(Page) * PageSize;
    std::vector<bool> Seen(PageSize / kV2Stride);
    const auto appendChain = [&](uint64_t Offset) -> llvm::Error {
      do {
        if (Offset % kV2Stride ||
            !rangeInBounds(Offset, sizeof(uint64_t), PageSize))
          return fail(kMalformedSlides);
        for (size_t I = Offset / kV2Stride;
             I < (Offset + sizeof(uint64_t)) / kV2Stride; ++I) {
          if (Seen[I])
            return fail(kMalformedSlides);
          Seen[I] = true;
        }
        if (Fixups.size() >= kMaxFixups)
          return fail(kLimitExceeded);
        const uint64_t Word =
            readLE<uint64_t>(Data.data() + PageOffset + Offset);
        const uint64_t Next = (Word & DeltaMask) >> (LowBit - kV2StrideLog2);
        DyldCacheFixup Fixup;
        Fixup.Slot = Address + PageOffset + Offset;
        Fixup.Target = Word & ~DeltaMask;
        Fixup.EncodedValue = Word;
        Fixup.SlideVersion = kSlideV2;
        if (Fixup.Target) {
          if (ValueAdd > InvalidVA - Fixup.Target)
            return fail(kMalformedSlides);
          Fixup.Target += ValueAdd;
        }
        if (overlapsFixup(Fixup.Slot, First, End))
          Fixups.push_back(std::move(Fixup));
        if (!Next)
          return llvm::Error::success();
        if (Next > PageSize - Offset)
          return fail(kMalformedSlides);
        Offset += Next;
      } while (true);
    };
    if (!(Start & kV2Extra)) {
      if (auto Error = appendChain(uint64_t(Start) * kV2Stride))
        return std::move(Error);
      continue;
    }
    uint32_t Index = Start & kV2IndexMask;
    while (true) {
      if (Index >= ExtrasCount)
        return fail(kMalformedSlides);
      const uint16_t Extra = readLE<uint16_t>(Info.data() + ExtrasOffset +
                                              size_t(Index) * sizeof(uint16_t));
      if (Extra & kV2NoRebase)
        return fail(kMalformedSlides);
      if (auto Error = appendChain(uint64_t(Extra & kV2IndexMask) * kV2Stride))
        return std::move(Error);
      if (Extra & kV2End)
        break;
      ++Index;
    }
  }
  return Fixups;
}

} // namespace

llvm::Expected<DyldSharedCache>
DyldSharedCache::open(const std::filesystem::path &Path,
                      const LoadProgress &Progress) {
  Progress.check();
  const std::string Name = pathToUTF8(Path);
  auto Main = readFile(Name);
  if (!Main)
    return Main.takeError();
  auto H = readHeader(fileBytes(**Main));
  if (!H)
    return H.takeError();
  std::vector<std::unique_ptr<llvm::MemoryBuffer>> Inputs;
  Inputs.push_back(std::move(*Main));
  for (const auto &Entry : H->Subcaches) {
    Progress.check();
    auto File = readFile(Name + Entry.Suffix);
    if (!File)
      return File.takeError();
    Inputs.push_back(std::move(*File));
  }
  return parse(std::move(Inputs), Progress);
}

llvm::Expected<DyldSharedCache>
DyldSharedCache::parse(std::vector<std::unique_ptr<llvm::MemoryBuffer>> Inputs,
                       const LoadProgress &Progress) {
  Progress.check();
  if (Inputs.empty() || Inputs.size() > kMaxFiles)
    return fail(kMalformedSubcaches);
  std::vector<Header> Headers;
  for (const auto &File : Inputs) {
    if (!File)
      return fail(kMalformedHeader);
    auto H = readHeader(fileBytes(*File));
    if (!H)
      return H.takeError();
    Headers.push_back(std::move(*H));
  }
  const Header &Main = Headers.front();
  if (Main.Subcaches.size() != Inputs.size() - 1)
    return fail(kMalformedSubcaches);
  DyldSharedCache Cache;
  for (size_t FileIndex = 0; FileIndex < Inputs.size(); ++FileIndex) {
    Progress.check();
    const Header &H = Headers[FileIndex];
    const Bytes Data = fileBytes(*Inputs[FileIndex]);
    if (H.MappingCount > kMaxMappings - Cache.Mappings.size())
      return fail(kLimitExceeded);
    const va_t FirstAddress = readLE<uint64_t>(Data.data() + H.MappingOffset);
    if (FileIndex == 0) {
      Cache.Base = FirstAddress;
      if (Main.RegionStart && Main.RegionStart != Cache.Base)
        return fail(kMalformedMappings);
    } else {
      const auto &Expected = Main.Subcaches[FileIndex - 1];
      if (!H.Subcaches.empty() || H.Identity != Expected.Identity ||
          Expected.VMOffset > InvalidVA - Cache.Base ||
          FirstAddress != Cache.Base + Expected.VMOffset)
        return fail(kMalformedSubcaches);
    }
    std::vector<std::pair<uint64_t, uint64_t>> FileRanges;
    for (size_t I = 0; I < H.MappingCount; ++I) {
      const uint8_t *Row = Data.data() + H.MappingOffset + I * kMappingBytes;
      DyldCacheMapping M;
      M.Address = readLE<uint64_t>(Row + kMapAddress);
      M.Size = readLE<uint64_t>(Row + kMapSize);
      M.FileOffset = readLE<uint64_t>(Row + kMapFileOffset);
      M.MaxProtection = readLE<uint32_t>(Row + kMapMaxProtection);
      M.InitialProtection = readLE<uint32_t>(Row + kMapInitialProtection);
      M.FileIndex = FileIndex;
      if (!M.Size || M.Size > InvalidVA - M.Address ||
          M.Address < FirstAddress ||
          (I == 0 && (M.FileOffset != 0 || M.Size < H.MappingOffset)) ||
          !rangeInBounds(M.FileOffset, M.Size, Data.size()) ||
          M.Address < Cache.Base ||
          (Main.RegionSize &&
           !rangeInBounds(M.Address - Cache.Base, M.Size, Main.RegionSize)))
        return fail(kMalformedMappings);
      FileRanges.emplace_back(M.FileOffset, M.FileOffset + M.Size);
      if (H.SlideMappingCount) {
        Row = Data.data() + H.SlideMappingOffset + I * kSlideMappingBytes;
        if (M.Address != readLE<uint64_t>(Row + kMapAddress) ||
            M.Size != readLE<uint64_t>(Row + kMapSize) ||
            M.FileOffset != readLE<uint64_t>(Row + kMapFileOffset) ||
            M.MaxProtection != readLE<uint32_t>(Row + kSlideMapMaxProtection) ||
            M.InitialProtection !=
                readLE<uint32_t>(Row + kSlideMapInitialProtection))
          return fail(kMalformedMappings);
        M.SlideOffset = readLE<uint64_t>(Row + kMapSlideOffset);
        M.SlideSize = readLE<uint64_t>(Row + kMapSlideSize);
        M.Flags = readLE<uint64_t>(Row + kMapFlags);
      } else if (I == kLegacySlideMapping) {
        M.SlideOffset = readLE<uint64_t>(Data.data() + kOldSlideOffset);
        M.SlideSize = readLE<uint64_t>(Data.data() + kOldSlideSize);
      }
      if (bool(M.SlideOffset) != bool(M.SlideSize) ||
          (M.SlideSize &&
           !rangeInBounds(M.SlideOffset, M.SlideSize, Data.size())))
        return fail(kMalformedSlides);
      Cache.Mappings.push_back(M);
    }
    std::sort(FileRanges.begin(), FileRanges.end());
    for (size_t I = 1; I < FileRanges.size(); ++I)
      if (FileRanges[I].first < FileRanges[I - 1].second)
        return fail(kMalformedMappings);
  }
  std::sort(Cache.Mappings.begin(), Cache.Mappings.end(),
            [](const auto &A, const auto &B) { return A.Address < B.Address; });
  for (size_t I = 1; I < Cache.Mappings.size(); ++I)
    if (Cache.Mappings[I].Address <
        Cache.Mappings[I - 1].Address + Cache.Mappings[I - 1].Size)
      return fail(kMalformedMappings);
  Cache.Files = std::move(Inputs);
  const Bytes Data = fileBytes(*Cache.Files.front());
  std::set<std::string> Paths;
  for (size_t I = 0; I < Main.ImagesCount; ++I) {
    Progress.check();
    const uint8_t *Row = Data.data() + Main.ImagesOffset + I * kImageBytes;
    DyldCacheImage Image;
    Image.Address = readLE<uint64_t>(Row + kImageAddress);
    const uint32_t Offset = readLE<uint32_t>(Row + kImagePathOffset);
    const auto ImageHeader =
        Cache.readBytes(Image.Address, sizeof(llvm::MachO::mach_header_64));
    if (!ImageHeader || Offset >= Data.size() ||
        readLE<uint32_t>(ImageHeader->data()) != llvm::MachO::MH_MAGIC_64 ||
        readLE<uint32_t>(ImageHeader->data() +
                         offsetof(llvm::MachO::mach_header_64, cputype)) !=
            llvm::MachO::CPU_TYPE_ARM64)
      return fail(kMalformedImages);
    const char *Start = reinterpret_cast<const char *>(Data.data() + Offset);
    const auto *End = static_cast<const char *>(
        std::memchr(Start, 0, std::min(kMaxPathBytes, Data.size() - Offset)));
    if (!End || End == Start)
      return fail(kMalformedImages);
    Image.Path.assign(Start, End);
    if (!Paths.insert(Image.Path).second)
      return fail(kMalformedImages);
    Cache.Images.push_back(std::move(Image));
  }
  return Cache;
}

const DyldCacheMapping *DyldSharedCache::mappingFor(va_t Address) const {
  auto It =
      std::upper_bound(Mappings.begin(), Mappings.end(), Address,
                       [](va_t VA, const auto &M) { return VA < M.Address; });
  if (It == Mappings.begin())
    return nullptr;
  --It;
  return Address - It->Address < It->Size ? &*It : nullptr;
}

std::optional<llvm::ArrayRef<uint8_t>>
DyldSharedCache::readBytes(va_t Address, uint64_t Size) const {
  const auto *M = mappingFor(Address);
  if (!M || !rangeInBounds(Address - M->Address, Size, M->Size))
    return std::nullopt;
  return fileBytes(*Files[M->FileIndex])
      .slice(M->FileOffset + (Address - M->Address), Size);
}

llvm::Expected<DyldCacheImageLayout>
DyldSharedCache::readImageLayout(size_t ImageIndex) const {
  using namespace llvm::MachO;
  if (ImageIndex >= Images.size())
    return fail(kMalformedImages);
  const va_t Address = Images[ImageIndex].Address;
  auto Bytes = readBytes(Address, sizeof(mach_header_64));
  if (!Bytes)
    return fail(kMalformedImageLayout);
  mach_header_64 Header{};
  std::memcpy(&Header, Bytes->data(), sizeof(Header));
  if (Header.magic != MH_MAGIC_64 || Header.cputype != CPU_TYPE_ARM64)
    return fail(kMalformedImageLayout);
  if (Header.ncmds > kMaxImageCommands ||
      Header.sizeofcmds > kMaxImageCommandBytes)
    return fail(kLimitExceeded);
  Bytes = readBytes(Address, sizeof(Header) + uint64_t(Header.sizeofcmds));
  if (!Bytes || Header.ncmds > Header.sizeofcmds / sizeof(load_command))
    return fail(kMalformedImageLayout);
  DyldCacheImageLayout Layout;
  Layout.HeaderBytes = *Bytes;
  size_t Offset = sizeof(Header);
  for (uint32_t I = 0; I < Header.ncmds; ++I) {
    if (!rangeInBounds(Offset, sizeof(load_command), Bytes->size()))
      return fail(kMalformedImageLayout);
    load_command Command{};
    std::memcpy(&Command, Bytes->data() + Offset, sizeof(Command));
    if (Command.cmdsize < sizeof(Command) ||
        Command.cmdsize % sizeof(uint64_t) ||
        !rangeInBounds(Offset, Command.cmdsize, Bytes->size()))
      return fail(kMalformedImageLayout);
    if (Command.cmd == LC_SEGMENT_64) {
      if (Command.cmdsize < sizeof(segment_command_64))
        return fail(kMalformedImageLayout);
      segment_command_64 Segment{};
      std::memcpy(&Segment, Bytes->data() + Offset, sizeof(Segment));
      if (Segment.nsects >
              (Command.cmdsize - sizeof(Segment)) / sizeof(section_64) ||
          Segment.vmsize > InvalidVA - Segment.vmaddr ||
          Segment.filesize > Segment.vmsize ||
          Segment.filesize > InvalidVA - Segment.fileoff ||
          (Segment.filesize && !readBytes(Segment.vmaddr, Segment.filesize)))
        return fail(kMalformedImageLayout);
      const auto Name = [](const char *Value, size_t Length) {
        const auto *End =
            static_cast<const char *>(std::memchr(Value, 0, Length));
        return std::string(Value, End ? size_t(End - Value) : Length);
      };
      DyldCacheImageSegment Mapping;
      Mapping.Name = Name(Segment.segname, sizeof(Segment.segname));
      Mapping.Address = Segment.vmaddr;
      Mapping.Size = Segment.vmsize;
      Mapping.FileOffset = Segment.fileoff;
      Mapping.FileSize = Segment.filesize;
      Mapping.MaxProtection = Segment.maxprot;
      Mapping.InitialProtection = Segment.initprot;
      Layout.Segments.push_back(std::move(Mapping));
      for (uint32_t Index = 0; Index < Segment.nsects; ++Index) {
        section_64 Entry{};
        std::memcpy(&Entry,
                    Bytes->data() + Offset + sizeof(Segment) +
                        size_t(Index) * sizeof(Entry),
                    sizeof(Entry));
        if (Entry.addr < Segment.vmaddr ||
            !rangeInBounds(Entry.addr - Segment.vmaddr, Entry.size,
                           Segment.vmsize) ||
            Entry.align > kMaxSectionAlignmentExponent)
          return fail(kMalformedImageLayout);
        Section Sec;
        Sec.Name = Name(Entry.sectname, sizeof(Entry.sectname));
        Sec.SegmentName = Name(Entry.segname, sizeof(Entry.segname));
        Sec.VA = Entry.addr;
        Sec.Size = Entry.size;
        Sec.Type = Entry.flags;
        Sec.Alignment = uint32_t(1) << Entry.align;
        Sec.Flags = machoProtToNd(Segment.initprot);
        const auto Kind = Entry.flags & SECTION_TYPE;
        const bool ZeroFill = Kind == S_ZEROFILL || Kind == S_GB_ZEROFILL ||
                              Kind == S_THREAD_LOCAL_ZEROFILL;
        Sec.FileSz = ZeroFill ? 0 : Entry.size;
        // Some cached images retain original section offsets (notably the
        // signed __TEXT of libcorecrypto). The VM layout and segment file
        // range own the cache view; the unmodified header retains that field.
        if (Sec.FileSz && (!rangeInBounds(Sec.VA - Segment.vmaddr, Sec.FileSz,
                                          Segment.filesize) ||
                           !readBytes(Sec.VA, Sec.FileSz)))
          return fail(kMalformedImageLayout);
        Sec.FileOff =
            Sec.FileSz ? Segment.fileoff + (Sec.VA - Segment.vmaddr) : 0;
        Layout.Sections.push_back(std::move(Sec));
      }
    }
    Offset += Command.cmdsize;
  }
  if (Offset != Bytes->size() || Layout.Segments.empty())
    return fail(kMalformedImageLayout);
  const auto validateRanges = [](const auto &Ranges, auto Start, auto Size) {
    std::vector<std::pair<va_t, va_t>> Sorted;
    for (const auto &Range : Ranges)
      if (Size(Range))
        Sorted.emplace_back(Start(Range), Start(Range) + Size(Range));
    std::sort(Sorted.begin(), Sorted.end());
    for (size_t I = 1; I < Sorted.size(); ++I)
      if (Sorted[I].first < Sorted[I - 1].second)
        return false;
    return true;
  };
  if (!validateRanges(
          Layout.Segments, [](const auto &S) { return S.Address; },
          [](const auto &S) { return S.Size; }) ||
      !validateRanges(
          Layout.Sections, [](const auto &S) { return S.VA; },
          [](const auto &S) { return S.Size; }))
    return fail(kMalformedImageLayout);
  const bool OwnsHeader = std::any_of(
      Layout.Segments.begin(), Layout.Segments.end(), [&](const auto &S) {
        return Address >= S.Address &&
               rangeInBounds(Address - S.Address, Bytes->size(), S.FileSize);
      });
  if (!OwnsHeader)
    return fail(kMalformedImageLayout);
  return Layout;
}

llvm::Expected<llvm::ArrayRef<uint8_t>>
DyldSharedCache::readImageFileBytes(size_t ImageIndex, uint64_t Offset,
                                    uint64_t Size) const {
  auto Layout = readImageLayout(ImageIndex);
  if (!Layout)
    return Layout.takeError();
  std::optional<llvm::ArrayRef<uint8_t>> Result;
  for (const auto &Segment : Layout->Segments) {
    if (Offset < Segment.FileOffset ||
        !rangeInBounds(Offset - Segment.FileOffset, Size, Segment.FileSize))
      continue;
    const auto Bytes =
        readBytes(Segment.Address + (Offset - Segment.FileOffset), Size);
    if (!Bytes || Result)
      return fail(kAmbiguousImageFileRange);
    Result = *Bytes;
  }
  if (!Result)
    return fail(kAmbiguousImageFileRange);
  return *Result;
}

llvm::Expected<std::optional<DyldSharedCache::ObjCOptimization>>
DyldSharedCache::readObjCOptimization() const {
  if (Files.empty())
    return fail(kMalformedHeader);
  const Bytes Header = fileBytes(*Files.front());
  const uint32_t HeaderSize = readLE<uint32_t>(Header.data() + kMappingOffset);
  if (rangeInBounds(kObjCOptsSize, sizeof(uint64_t), HeaderSize)) {
    const uint64_t Offset = readLE<uint64_t>(Header.data() + kObjCOptsOffset);
    const uint64_t Size = readLE<uint64_t>(Header.data() + kObjCOptsSize);
    if (bool(Offset) != bool(Size) || Offset > InvalidVA - Base)
      return fail(kMalformedObjCOpts);
    if (Size) {
      const auto Data = readBytes(Base + Offset, Size);
      if (!Data || Size < kObjCOptsV1Bytes)
        return fail(kMalformedObjCOpts);
      const uint32_t Version = readLE<uint32_t>(Data->data());
      if (Version != kObjCOptsV1 && Version != kObjCOptsV2)
        return fail(kUnsupportedObjCOpts);
      if (Version == kObjCOptsV2 && Size < kObjCOptsV2Bytes)
        return fail(kMalformedObjCOpts);
      return ObjCOptimization{Base + Offset, Version, false, *Data};
    }
  }
  for (size_t I = 0; I < Images.size(); ++I) {
    if (Images[I].Path != kObjCLibraryPath)
      continue;
    auto Layout = readImageLayout(I);
    if (!Layout)
      return Layout.takeError();
    for (const auto &Section : Layout->Sections) {
      if (Section.Name != kObjCLegacyOptSection ||
          Section.SegmentName != section_names::macho::TextSeg)
        continue;
      const auto Data = readBytes(Section.VA, Section.FileSz);
      if (!Data || Data->size() < sizeof(uint32_t))
        return fail(kMalformedObjCOpts);
      const uint32_t Version = readLE<uint32_t>(Data->data());
      if (Version != kObjCLegacyV15 && Version != kObjCLegacyV16)
        return fail(kUnsupportedObjCOpts);
      const size_t Minimum =
          Version == kObjCLegacyV15 ? kObjCLegacyV15Bytes : kObjCLegacyV16Bytes;
      if (Data->size() < Minimum)
        return fail(kMalformedObjCOpts);
      return ObjCOptimization{Section.VA, Version, true, *Data};
    }
  }
  return std::optional<ObjCOptimization>();
}

llvm::Expected<std::optional<DyldCacheSelectorLayout>>
DyldSharedCache::objcSelectorLayout() const {
  auto Parsed = readObjCOptimization();
  if (!Parsed)
    return Parsed.takeError();
  if (!*Parsed)
    return std::optional<DyldCacheSelectorLayout>();
  const auto &Info = **Parsed;
  if (Info.Legacy && Info.Version == kObjCLegacyV15)
    return DyldCacheSelectorLayout{DyldCacheSelectorEncoding::RecordRelative,
                                   0};
  va_t Address = 0;
  if (Info.Legacy) {
    const int64_t Relative =
        readLE<int64_t>(Info.Data.data() + kObjCLegacySelectorBaseOffset);
    if (!Relative)
      return std::optional<DyldCacheSelectorLayout>();
    const uint64_t Magnitude =
        Relative < 0 ? 0 - uint64_t(Relative) : uint64_t(Relative);
    if (Relative < 0 ? Magnitude > Info.Address
                     : Magnitude > InvalidVA - Info.Address)
      return fail(kMalformedObjCOpts);
    Address =
        Relative < 0 ? Info.Address - Magnitude : Info.Address + Magnitude;
  } else {
    const uint64_t Relative =
        readLE<uint64_t>(Info.Data.data() + kObjCSelectorBaseOffset);
    if (!Relative)
      return std::optional<DyldCacheSelectorLayout>();
    if (Relative > InvalidVA - Base)
      return fail(kMalformedObjCOpts);
    Address = Base + Relative;
  }
  if (!readBytes(Address, 1))
    return fail(kMalformedObjCOpts);
  return DyldCacheSelectorLayout{DyldCacheSelectorEncoding::BaseRelative,
                                 Address};
}

llvm::Expected<va_t> DyldSharedCache::objcImageAddress(uint32_t Index) const {
  auto Parsed = readObjCOptimization();
  if (!Parsed)
    return Parsed.takeError();
  if (!*Parsed)
    return fail(kMalformedObjCOpts);
  const auto &Info = **Parsed;
  va_t Address = 0;
  if (Info.Legacy) {
    const int64_t Relative =
        readLE<int32_t>(Info.Data.data() + kObjCLegacyHeaderInfoOffset);
    if (!Relative ||
        (Relative < 0 ? uint64_t(-Relative) > Info.Address
                      : uint64_t(Relative) > InvalidVA - Info.Address))
      return fail(kMalformedObjCOpts);
    Address = Relative < 0 ? Info.Address - uint64_t(-Relative)
                           : Info.Address + uint64_t(Relative);
  } else {
    const uint64_t Offset =
        readLE<uint64_t>(Info.Data.data() + kObjCHeaderInfoOffset);
    if (!Offset || Offset > InvalidVA - Base)
      return fail(kMalformedObjCOpts);
    Address = Base + Offset;
  }
  const auto Header = readBytes(Address, kObjCHeaderInfoHeaderBytes);
  if (!Header)
    return fail(kMalformedObjCOpts);
  const uint32_t Count = readLE<uint32_t>(Header->data());
  const uint32_t Stride =
      readLE<uint32_t>(Header->data() + kObjCHeaderInfoStrideOffset);
  if (Count > kMaxImages || Index >= Count ||
      (Stride != kObjCHeaderInfoRowBytes &&
       Stride != kObjCHeaderInfoDyldRowBytes) ||
      !readBytes(Address,
                 kObjCHeaderInfoHeaderBytes + uint64_t(Count) * Stride))
    return fail(kMalformedObjCOpts);
  const va_t Row =
      Address + kObjCHeaderInfoHeaderBytes + uint64_t(Index) * Stride;
  const int64_t Relative = readLE<int64_t>(readBytes(Row, Stride)->data());
  const uint64_t Magnitude =
      Relative < 0 ? 0 - uint64_t(Relative) : uint64_t(Relative);
  if (Relative < 0 ? Magnitude > Row : Magnitude > InvalidVA - Row)
    return fail(kMalformedObjCOpts);
  const va_t Target = Relative < 0 ? Row - Magnitude : Row + Magnitude;
  if (std::none_of(Images.begin(), Images.end(),
                   [&](const auto &Image) { return Image.Address == Target; }))
    return fail(kMalformedObjCOpts);
  return Target;
}

llvm::Expected<std::vector<DyldCacheFixup>>
DyldSharedCache::readSlideFixups(size_t MappingIndex,
                                 const LoadProgress &Progress) const {
  return readMappingSlides(MappingIndex, 0, InvalidVA, Progress);
}

llvm::Expected<std::vector<DyldCacheFixup>>
DyldSharedCache::readSlideFixupsInRange(va_t Address, uint64_t Size,
                                        const LoadProgress &Progress) const {
  Progress.check();
  if (Size > InvalidVA - Address)
    return fail(kMalformedSlides);
  std::vector<DyldCacheFixup> Fixups;
  if (!Size)
    return Fixups;
  for (size_t Index = 0; Index < Mappings.size(); ++Index) {
    const auto &M = Mappings[Index];
    if (Address >= M.Address + M.Size || Address + Size <= M.Address)
      continue;
    auto Part = readMappingSlides(Index, Address, Address + Size, Progress);
    if (!Part)
      return Part.takeError();
    if (Part->size() > kMaxFixups - Fixups.size())
      return fail(kLimitExceeded);
    Fixups.insert(Fixups.end(), std::make_move_iterator(Part->begin()),
                  std::make_move_iterator(Part->end()));
  }
  std::sort(Fixups.begin(), Fixups.end(),
            [](const auto &A, const auto &B) { return A.Slot < B.Slot; });
  return Fixups;
}

llvm::Expected<std::vector<DyldCacheFixup>>
DyldSharedCache::readMappingSlides(size_t MappingIndex, va_t First, va_t End,
                                   const LoadProgress &Progress) const {
  Progress.check();
  if (MappingIndex >= Mappings.size())
    return fail(kMalformedMappings);
  const auto &M = Mappings[MappingIndex];
  std::vector<DyldCacheFixup> Fixups;
  if (!M.SlideSize)
    return Fixups;
  if (M.SlideSize < kSlideHeaderBytes)
    return fail(kMalformedSlides);
  const Bytes File = fileBytes(*Files[M.FileIndex]);
  const Bytes Info = File.slice(M.SlideOffset, M.SlideSize);
  const uint32_t Version = readLE<uint32_t>(Info.data() + kSlideVersion);
  if (Version == kSlideV2)
    return readV2Slides(File.slice(M.FileOffset, M.Size), Info, M.Address,
                        First, End, Progress);
  if (Version != kSlideV3 && Version != kSlideV5)
    return fail(kUnsupportedSlides);
  const uint32_t PageSize = readLE<uint32_t>(Info.data() + kSlidePageSize);
  const uint32_t PageCount = readLE<uint32_t>(Info.data() + kSlidePageCount);
  const uint64_t ValueAdd = readLE<uint64_t>(Info.data() + kSlideValueAdd);
  if ((PageSize != arm64e::kSmallPageBytes &&
       PageSize != arm64e::kLargePageBytes) ||
      uint64_t(PageCount) * PageSize > M.Size ||
      !rangeInBounds(kSlideHeaderBytes, uint64_t(PageCount) * sizeof(uint16_t),
                     Info.size()))
    return fail(kMalformedSlides);
  if (PageCount > kMaxSlidePages)
    return fail(kLimitExceeded);
  const auto [FirstPage, EndPage] =
      slidePageRange(M.Address, PageSize, PageCount, First, End);
  for (uint32_t Page = FirstPage; Page < EndPage; ++Page) {
    Progress.check();
    uint64_t Offset = readLE<uint16_t>(Info.data() + kSlideHeaderBytes +
                                       size_t(Page) * sizeof(uint16_t));
    if (Offset == kNoRebase)
      continue;
    const uint64_t PageOffset = uint64_t(Page) * PageSize;
    do {
      if (Offset % kChainStride ||
          !rangeInBounds(Offset, sizeof(uint64_t), PageSize))
        return fail(kMalformedSlides);
      if (Fixups.size() >= kMaxFixups)
        return fail(kLimitExceeded);
      const uint64_t Word =
          readLE<uint64_t>(File.data() + M.FileOffset + PageOffset + Offset);
      DyldCacheFixup Fixup;
      Fixup.Slot = M.Address + PageOffset + Offset;
      Fixup.EncodedValue = Word;
      Fixup.SlideVersion = Version;
      const bool Auth = arm64e::kAuthentication.extract(Word);
      uint64_t Next;
      if (Version == kSlideV3) {
        if (kV3Unused.extract(Word))
          return fail(kMalformedSlides);
        Next = arm64e::kNext.extract(Word);
        Fixup.Target =
            Auth ? arm64e::kAuthenticatedTarget.extract(Word)
                 : arm64e::kTarget.extract(Word) |
                       (arm64e::kHigh8.extract(Word) << arm64e::kHigh8Shift);
      } else {
        if (!Auth && kV5Unused.extract(Word))
          return fail(kMalformedSlides);
        Next = kV5Next.extract(Word);
        Fixup.Target = kV5Target.extract(Word);
      }
      if (Auth || Version == kSlideV5) {
        if (ValueAdd > InvalidVA - Fixup.Target)
          return fail(kMalformedSlides);
        Fixup.Target += ValueAdd;
      }
      if (Auth) {
        MachOAuthenticatedPointer Recipe;
        Recipe.Target = Fixup.Target;
        Recipe.EncodedValue = Word;
        Recipe.Key = Version == kSlideV3
                         ? arm64e::kKey.extract(Word)
                         : kV5KeyIsData.extract(Word) * kDataKey;
        Recipe.Diversity =
            (Version == kSlideV3 ? arm64e::kDiversity : kV5Diversity)
                .extract(Word);
        Recipe.AddressDiversity =
            (Version == kSlideV3 ? arm64e::kAddressDiversity
                                 : kV5AddressDiversity)
                .extract(Word);
        Fixup.Authentication = std::move(Recipe);
      } else if (Version == kSlideV5) {
        Fixup.Target |= kV5High8.extract(Word) << arm64e::kHigh8Shift;
      }
      if (overlapsFixup(Fixup.Slot, First, End))
        Fixups.push_back(std::move(Fixup));
      if (!Next)
        break;
      Offset += Next * kChainStride;
    } while (true);
  }
  return Fixups;
}

bool DyldSharedCache::hasMagic(llvm::StringRef Bytes) {
  return Bytes.size() >= kMagicBytes &&
         std::memcmp(Bytes.data(), kMagic, kMagicBytes) == 0;
}

std::vector<std::array<uint8_t, 32>>
DyldSharedCache::fileDigests(const LoadProgress &Progress) const {
  std::vector<std::array<uint8_t, 32>> Result;
  Result.reserve(Files.size());
  for (const auto &File : Files) {
    Progress.check();
    Result.push_back(sha256(fileBytes(*File), Progress));
  }
  return Result;
}

} // namespace neverd::macho_loader
