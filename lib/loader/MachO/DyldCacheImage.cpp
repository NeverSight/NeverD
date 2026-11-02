//===- DyldCacheImage.cpp - Bounded cached Mach-O analysis views ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Pack selected segment bytes and referenced linkedit data for the common
/// Mach-O readers. Virtual addresses and symbol indices retain their original
/// meaning; file offsets and string indices belong to the resulting view.
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/DyldSharedCache.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/Limits.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <tuple>
#include <utility>

namespace neverd::macho_loader {
namespace {

using namespace llvm::MachO;
using Bytes = llvm::ArrayRef<uint8_t>;

#define NEVERD_DYLD_CACHE_LIMIT(Type, Name, Value)                             \
  inline constexpr Type Name = Value;
#include "neverd/loader/MachO/DyldSharedCache.def"
#define NEVERD_CACHE_IMAGE_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "DyldCacheImage.def"

llvm::Error fail(const char *Message = kProjectionMalformed) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

struct Command {
  size_t Offset = 0;
  uint32_t Kind = 0;
  std::vector<uint8_t> Data;

  template <typename T> T get(size_t At) const {
    return readLE<T>(Data.data() + At);
  }
  template <typename T> void set(size_t At, T Value) {
    writeLE<T>(Data.data() + At, Value);
  }
};

class ImageBuilder {
  const DyldSharedCache &Cache;
  const DyldCacheImageLayout &Layout;
  va_t HeaderAddress;
  const LoadProgress &Progress;
  std::vector<uint8_t> Output;
  std::vector<Command> Commands;
  std::vector<uint64_t> SegmentOffsets;
  std::optional<size_t> LinkeditIndex;
  Bytes OriginalStrings;
  std::vector<uint8_t> Strings{0};
  llvm::StringMap<uint32_t> StringIndices;

  llvm::Expected<Bytes> fileRange(uint64_t Offset, uint64_t Size) const {
    if (!Size)
      return Bytes();
    std::optional<Bytes> Result;
    for (const auto &Segment : Layout.Segments) {
      if (Offset < Segment.FileOffset ||
          !rangeInBounds(Offset - Segment.FileOffset, Size, Segment.FileSize))
        continue;
      if (Result)
        return fail();
      Result =
          Cache.readBytes(Segment.Address + Offset - Segment.FileOffset, Size);
      if (!Result)
        return fail();
    }
    return Result ? llvm::Expected<Bytes>(*Result) : fail();
  }

  llvm::Expected<uint32_t> append(Bytes Data) {
    const size_t Offset = llvm::alignTo(Output.size(), sizeof(uint64_t));
    if (Offset > kMaxPreparedImageBytes ||
        Data.size() > kMaxPreparedImageBytes - Offset)
      return fail(kProjectionLimit);
    Output.resize(Offset);
    for (size_t Done = 0; Done < Data.size();) {
      Progress.check();
      const size_t Count = std::min(limits::support::kLoadByteCheckpointBytes,
                                    Data.size() - Done);
      Output.insert(Output.end(), Data.begin() + Done,
                    Data.begin() + Done + Count);
      Done += Count;
    }
    return static_cast<uint32_t>(Offset);
  }

  llvm::Expected<uint32_t> copyRange(uint64_t Offset, uint64_t Size) {
    if (!Size)
      return 0;
    auto Data = fileRange(Offset, Size);
    if (!Data)
      return Data.takeError();
    return append(*Data);
  }

  llvm::Expected<uint32_t> stringIndex(uint64_t Index) {
    if (!Index)
      return 0;
    if (Index >= OriginalStrings.size())
      return fail();
    const auto *First = OriginalStrings.data() + Index;
    const auto *End = static_cast<const uint8_t *>(std::memchr(
        First, 0,
        std::min(kMaxImageSymbolBytes,
                 OriginalStrings.size() - static_cast<size_t>(Index))));
    if (!End)
      return fail();
    llvm::StringRef Text(reinterpret_cast<const char *>(First), End - First);
    if (auto Found = StringIndices.find(Text); Found != StringIndices.end())
      return Found->second;
    if (Text.size() >= kMaxPreparedImageBytes - Strings.size())
      return fail(kProjectionLimit);
    const auto Offset = static_cast<uint32_t>(Strings.size());
    Strings.insert(Strings.end(), First, End + 1);
    StringIndices.try_emplace(Text, Offset);
    return Offset;
  }

  llvm::Error symbols(Command &C) {
    if (C.Data.size() < sizeof(symtab_command))
      return fail();
    const uint32_t Count = C.get<uint32_t>(offsetof(symtab_command, nsyms));
    if (Count > kMaxImageSymbols)
      return fail(kProjectionLimit);
    auto Data = fileRange(C.get<uint32_t>(offsetof(symtab_command, symoff)),
                          uint64_t(Count) * sizeof(nlist_64));
    if (!Data)
      return Data.takeError();
    auto Names = fileRange(C.get<uint32_t>(offsetof(symtab_command, stroff)),
                           C.get<uint32_t>(offsetof(symtab_command, strsize)));
    if (!Names)
      return Names.takeError();
    OriginalStrings = *Names;
    auto Offset = append(*Data);
    if (!Offset)
      return Offset.takeError();
    C.set(offsetof(symtab_command, symoff), Count ? *Offset : 0u);
    for (uint32_t I = 0; I < Count; ++I) {
      Progress.check();
      uint8_t *Entry = Output.data() + *Offset + size_t(I) * sizeof(nlist_64);
      auto Name =
          stringIndex(readLE<uint32_t>(Entry + offsetof(nlist_64, n_strx)));
      if (!Name)
        return Name.takeError();
      writeLE<uint32_t>(Entry + offsetof(nlist_64, n_strx), *Name);
      if ((Entry[offsetof(nlist_64, n_type)] & N_TYPE) == N_INDR) {
        auto Target =
            stringIndex(readLE<uint64_t>(Entry + offsetof(nlist_64, n_value)));
        if (!Target)
          return Target.takeError();
        writeLE<uint64_t>(Entry + offsetof(nlist_64, n_value), *Target);
      }
    }
    return llvm::Error::success();
  }

  llvm::Error dynamicSymbols(Command &C) {
    if (C.Data.size() < sizeof(dysymtab_command))
      return fail();
    const auto Table = [&](size_t OffsetAt, size_t CountAt,
                           size_t EntryBytes) -> llvm::Expected<uint32_t> {
      auto New = copyRange(C.get<uint32_t>(OffsetAt),
                           uint64_t(C.get<uint32_t>(CountAt)) * EntryBytes);
      if (!New)
        return New.takeError();
      C.set(OffsetAt, *New);
      return *New;
    };
    for (const auto &[OffsetAt, CountAt, Width] :
         {std::tuple{offsetof(dysymtab_command, tocoff),
                     offsetof(dysymtab_command, ntoc),
                     sizeof(dylib_table_of_contents)},
          std::tuple{offsetof(dysymtab_command, extrefsymoff),
                     offsetof(dysymtab_command, nextrefsyms),
                     sizeof(dylib_reference)},
          std::tuple{offsetof(dysymtab_command, indirectsymoff),
                     offsetof(dysymtab_command, nindirectsyms),
                     sizeof(uint32_t)},
          std::tuple{offsetof(dysymtab_command, extreloff),
                     offsetof(dysymtab_command, nextrel),
                     sizeof(relocation_info)},
          std::tuple{offsetof(dysymtab_command, locreloff),
                     offsetof(dysymtab_command, nlocrel),
                     sizeof(relocation_info)}}) {
      auto New = Table(OffsetAt, CountAt, Width);
      if (!New)
        return New.takeError();
    }
    auto Modules =
        Table(offsetof(dysymtab_command, modtaboff),
              offsetof(dysymtab_command, nmodtab), sizeof(dylib_module_64));
    if (!Modules)
      return Modules.takeError();
    const uint32_t Count = C.get<uint32_t>(offsetof(dysymtab_command, nmodtab));
    for (uint32_t I = 0; I < Count; ++I) {
      Progress.check();
      uint8_t *Entry =
          Output.data() + *Modules + size_t(I) * sizeof(dylib_module_64);
      auto Name = stringIndex(
          readLE<uint32_t>(Entry + offsetof(dylib_module_64, module_name)));
      if (!Name)
        return Name.takeError();
      writeLE<uint32_t>(Entry + offsetof(dylib_module_64, module_name), *Name);
    }
    return llvm::Error::success();
  }

  llvm::Expected<uint64_t> translatedOffset(uint64_t Offset,
                                            uint64_t Size) const {
    std::optional<uint64_t> Result;
    for (size_t I = 0; I < Layout.Segments.size(); ++I) {
      const auto &S = Layout.Segments[I];
      if (I == LinkeditIndex || Offset < S.FileOffset ||
          !rangeInBounds(Offset - S.FileOffset, Size, S.FileSize))
        continue;
      if (Result)
        return fail();
      Result = SegmentOffsets[I] + Offset - S.FileOffset;
    }
    return Result ? llvm::Expected<uint64_t>(*Result) : fail();
  }

  llvm::Error linkedit(Command &C) {
    if (C.Data.size() < sizeof(linkedit_data_command))
      return fail();
    const uint32_t Size =
        C.get<uint32_t>(offsetof(linkedit_data_command, datasize));
    auto Offset = copyRange(
        C.get<uint32_t>(offsetof(linkedit_data_command, dataoff)), Size);
    if (!Offset)
      return Offset.takeError();
    C.set(offsetof(linkedit_data_command, dataoff), *Offset);
    if (C.Kind == LC_DATA_IN_CODE) {
      if (Size % sizeof(data_in_code_entry))
        return fail();
      for (size_t I = 0; I < Size; I += sizeof(data_in_code_entry)) {
        uint8_t *Entry = Output.data() + *Offset + I;
        auto New = translatedOffset(
            readLE<uint32_t>(Entry + offsetof(data_in_code_entry, offset)),
            readLE<uint16_t>(Entry + offsetof(data_in_code_entry, length)));
        if (!New)
          return New.takeError();
        writeLE<uint32_t>(Entry + offsetof(data_in_code_entry, offset), *New);
      }
    }
    return llvm::Error::success();
  }

  llvm::Error command(Command &C) {
    switch (C.Kind) {
    case LC_SEGMENT_64: {
      const uint32_t Sections =
          C.get<uint32_t>(offsetof(segment_command_64, nsects));
      for (uint32_t I = 0; I < Sections; ++I) {
        const size_t At =
            sizeof(segment_command_64) + size_t(I) * sizeof(section_64);
        const uint32_t Count =
            C.get<uint32_t>(At + offsetof(section_64, nreloc));
        auto Relocations =
            copyRange(C.get<uint32_t>(At + offsetof(section_64, reloff)),
                      uint64_t(Count) * sizeof(relocation_info));
        if (!Relocations)
          return Relocations.takeError();
        C.set(At + offsetof(section_64, reloff), *Relocations);
      }
      return llvm::Error::success();
    }
    case LC_SYMTAB:
      return llvm::Error::success();
    case LC_DYSYMTAB:
      return dynamicSymbols(C);
#define NEVERD_CACHE_IMAGE_EMPTY(Value) case Value:
#include "DyldCacheImage.def"
      return llvm::Error::success();
#define NEVERD_CACHE_IMAGE_LINKEDIT(Value) case Value:
#include "DyldCacheImage.def"
      return linkedit(C);
    case LC_DYLD_INFO:
    case LC_DYLD_INFO_ONLY: {
      if (C.Data.size() < sizeof(dyld_info_command))
        return fail();
      for (const auto &[OffsetAt, SizeAt] :
           {std::pair{offsetof(dyld_info_command, rebase_off),
                      offsetof(dyld_info_command, rebase_size)},
            std::pair{offsetof(dyld_info_command, bind_off),
                      offsetof(dyld_info_command, bind_size)},
            std::pair{offsetof(dyld_info_command, weak_bind_off),
                      offsetof(dyld_info_command, weak_bind_size)},
            std::pair{offsetof(dyld_info_command, lazy_bind_off),
                      offsetof(dyld_info_command, lazy_bind_size)},
            std::pair{offsetof(dyld_info_command, export_off),
                      offsetof(dyld_info_command, export_size)}}) {
        auto New =
            copyRange(C.get<uint32_t>(OffsetAt), C.get<uint32_t>(SizeAt));
        if (!New)
          return New.takeError();
        C.set(OffsetAt, *New);
      }
      return llvm::Error::success();
    }
    case LC_NOTE: {
      if (C.Data.size() < sizeof(note_command))
        return fail();
      auto New = copyRange(C.get<uint64_t>(offsetof(note_command, offset)),
                           C.get<uint64_t>(offsetof(note_command, size)));
      if (!New)
        return New.takeError();
      C.set<uint64_t>(offsetof(note_command, offset), *New);
      return llvm::Error::success();
    }
    case LC_MAIN: {
      if (C.Data.size() < sizeof(entry_point_command))
        return fail();
      auto New = translatedOffset(
          C.get<uint64_t>(offsetof(entry_point_command, entryoff)), 1);
      if (!New)
        return New.takeError();
      C.set(offsetof(entry_point_command, entryoff), *New);
      return llvm::Error::success();
    }
    case LC_ENCRYPTION_INFO_64: {
      if (C.Data.size() < sizeof(encryption_info_command_64))
        return fail();
      if (C.get<uint32_t>(offsetof(encryption_info_command_64, cryptid)))
        return fail(kProjectionEncrypted);
      const uint32_t Size =
          C.get<uint32_t>(offsetof(encryption_info_command_64, cryptsize));
      if (!Size) {
        C.set<uint32_t>(offsetof(encryption_info_command_64, cryptoff), 0);
        return llvm::Error::success();
      }
      auto New = translatedOffset(
          C.get<uint32_t>(offsetof(encryption_info_command_64, cryptoff)),
          Size);
      if (!New)
        return New.takeError();
      C.set<uint32_t>(offsetof(encryption_info_command_64, cryptoff), *New);
      return llvm::Error::success();
    }
    default:
      return fail(kProjectionCommand);
    }
  }

public:
  ImageBuilder(const DyldSharedCache &Cache, const DyldCacheImageLayout &Layout,
               va_t HeaderAddress, const LoadProgress &Progress)
      : Cache(Cache), Layout(Layout), HeaderAddress(HeaderAddress),
        Progress(Progress) {}

  llvm::Expected<std::vector<uint8_t>> build() {
    Progress.check();
    const auto HeaderOwner =
        std::find_if(Layout.Segments.begin(), Layout.Segments.end(),
                     [&](const auto &S) { return S.Address == HeaderAddress; });
    if (HeaderOwner == Layout.Segments.end() ||
        HeaderOwner->FileSize < Layout.HeaderBytes.size() ||
        HeaderOwner->Name == section_names::macho::LinkeditSeg)
      return fail();
    SegmentOffsets.resize(Layout.Segments.size());
    const size_t HeaderIndex = HeaderOwner - Layout.Segments.begin();
    auto HeaderOffset =
        append(*Cache.readBytes(HeaderOwner->Address, HeaderOwner->FileSize));
    if (!HeaderOffset)
      return HeaderOffset.takeError();
    for (size_t I = 0; I < Layout.Segments.size(); ++I) {
      Progress.check();
      const auto &S = Layout.Segments[I];
      if (S.Name == section_names::macho::LinkeditSeg) {
        if (LinkeditIndex)
          return fail();
        LinkeditIndex = I;
        continue;
      }
      if (I == HeaderIndex)
        continue;
      auto New = append(S.FileSize ? *Cache.readBytes(S.Address, S.FileSize)
                                   : Bytes());
      if (!New)
        return New.takeError();
      SegmentOffsets[I] = *New;
    }
    const size_t LinkeditStart = llvm::alignTo(Output.size(), sizeof(uint64_t));
    Output.resize(LinkeditStart);
    if (LinkeditIndex)
      SegmentOffsets[*LinkeditIndex] = LinkeditStart;
    std::optional<size_t> Symbols;
    for (size_t At = sizeof(mach_header_64); At < Layout.HeaderBytes.size();) {
      const uint32_t Size = readLE<uint32_t>(Layout.HeaderBytes.data() + At +
                                             offsetof(load_command, cmdsize));
      Commands.push_back(
          {At, readLE<uint32_t>(Layout.HeaderBytes.data() + At),
           std::vector<uint8_t>(Layout.HeaderBytes.begin() + At,
                                Layout.HeaderBytes.begin() + At + Size)});
      if (Commands.back().Kind == LC_SYMTAB) {
        if (Symbols)
          return fail();
        Symbols = Commands.size() - 1;
        if (auto Error = symbols(Commands.back()))
          return std::move(Error);
      }
      At += Size;
    }
    for (auto &C : Commands) {
      Progress.check();
      if (auto Error = command(C))
        return std::move(Error);
    }
    if (Symbols) {
      auto Names = append(Strings);
      if (!Names)
        return Names.takeError();
      Commands[*Symbols].set(offsetof(symtab_command, stroff), *Names);
      Commands[*Symbols].set<uint32_t>(offsetof(symtab_command, strsize),
                                       Strings.size());
    }
    if (Output.size() > LinkeditStart && !LinkeditIndex)
      return fail();
    size_t SegmentIndex = 0;
    for (auto &C : Commands) {
      if (C.Kind == LC_SEGMENT_64) {
        const auto &S = Layout.Segments[SegmentIndex];
        const uint64_t Offset = SegmentOffsets[SegmentIndex];
        C.set(offsetof(segment_command_64, fileoff),
              S.FileSize ? Offset : uint64_t(0));
        if (SegmentIndex == LinkeditIndex) {
          const uint64_t Size = Output.size() - LinkeditStart;
          if (Size > S.Size)
            return fail();
          C.set(offsetof(segment_command_64, filesize), Size);
          C.set<uint64_t>(offsetof(segment_command_64, fileoff),
                          Size ? Offset : 0);
        }
        const uint32_t Sections =
            C.get<uint32_t>(offsetof(segment_command_64, nsects));
        for (uint32_t I = 0; I < Sections; ++I) {
          const size_t At =
              sizeof(segment_command_64) + size_t(I) * sizeof(section_64);
          const uint64_t Address =
              C.get<uint64_t>(At + offsetof(section_64, addr));
          const uint32_t Type =
              C.get<uint32_t>(At + offsetof(section_64, flags)) & SECTION_TYPE;
          const bool Zero = Type == S_ZEROFILL || Type == S_GB_ZEROFILL ||
                            Type == S_THREAD_LOCAL_ZEROFILL;
          if (SegmentIndex == LinkeditIndex)
            return fail();
          C.set<uint32_t>(At + offsetof(section_64, offset),
                          Zero ? 0 : Offset + Address - S.Address);
        }
        ++SegmentIndex;
      }
      std::copy(C.Data.begin(), C.Data.end(), Output.begin() + C.Offset);
    }
    Progress.check();
    return std::move(Output);
  }
};

} // namespace

llvm::Expected<std::vector<uint8_t>>
DyldSharedCache::prepareImage(size_t ImageIndex,
                              const LoadProgress &Progress) const {
  Progress.check();
  auto Layout = readImageLayout(ImageIndex);
  if (!Layout)
    return Layout.takeError();
  return ImageBuilder(*this, *Layout, Images[ImageIndex].Address, Progress)
      .build();
}

} // namespace neverd::macho_loader
