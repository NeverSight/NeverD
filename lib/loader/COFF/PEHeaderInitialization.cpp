//===- PEHeaderInitialization.cpp - Header initialization boundary -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify supported loader writes cannot replace PE header storage.
//===----------------------------------------------------------------------===//

#include "PEHeaderInitialization.h"

#include "PELoaderWriteRanges.h"

#include "neverd/Limits.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/object/PELayout.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace neverd::coff_loader {
namespace {
using namespace llvm::COFF;
using namespace llvm::object;

class HeaderInitialization {
  llvm::ArrayRef<uint8_t> Bytes;
  const PEHeaderPtrs &PE;
  PEHeaderInitializationWork &Work;
  size_t Remaining = limits::kMaxPEHeaderInitializationWork;
  uint64_t HeaderPages;
  std::vector<LoaderRange> Writes, Relocations;
  std::vector<LoaderProtectedRead> Reads, RelocationReads;
  llvm::SmallVector<uint64_t, 3> WriterFields;
  bool HasRelocations = false;

  bool charge(size_t Bytes, size_t Records = 0) {
    if (Bytes > Remaining || Records > Remaining - Bytes) {
      Remaining = 0;
      return false;
    }
    Remaining -= Bytes + Records;
    Work.Bytes += Bytes;
    Work.Records += Records;
    return true;
  }

  void protect(LoaderRange Range,
               std::optional<uint64_t> InPlaceWriter = std::nullopt) {
    Reads.push_back({Range, InPlaceWriter});
    RelocationReads.push_back({Range, std::nullopt});
  }

  bool checkWrites() {
    for (auto *Ranges : {&Writes, &Relocations}) {
      // Charge the comparison bound before sorting; allocation and visits
      // were already charged while parsing each source record.
      const size_t Count = Ranges->size();
      if (!charge(0, Count * llvm::bit_width(Count)))
        return false;
      llvm::sort(*Ranges, [](LoaderRange A, LoaderRange B) {
        return A.Begin < B.Begin;
      });
      for (size_t I = 1; I < Count; ++I)
        if ((*Ranges)[I].Begin < (*Ranges)[I - 1].End)
          return false;
    }
    const auto Preserved = [&](const auto &Destinations, const auto &Metadata) {
      for (const auto &Read : Metadata)
        if (!charge(0, llvm::bit_width(Destinations.size()) + 1) ||
            !loaderWritesPreserveRead(Destinations, Read))
          return false;
      return true;
    };
    if (!Preserved(Writes, Reads) || !Preserved(Relocations, RelocationReads))
      return false;
    // An absolute destination must move with a rebased image. A partial or
    // missing relocation cannot bound the loader write to an ordinary section.
    if (HasRelocations) {
      for (uint64_t Field : WriterFields) {
        if (!charge(0, llvm::bit_width(Relocations.size()) + 1))
          return false;
        const auto It = std::lower_bound(
            Relocations.begin(), Relocations.end(), Field,
            [](LoaderRange R, uint64_t Begin) { return R.Begin < Begin; });
        if (It == Relocations.end() || It->Begin != Field ||
            It->End != Field + (PE.Is64 ? 8 : 4))
          return false;
      }
    }
    return true;
  }

  // An ordinary section's complete file-backed suffix. No header alias,
  // overlapping virtual owner, zero-fill, or wrapped RVA may supply metadata.
  std::optional<llvm::ArrayRef<uint8_t>> span(uint64_t RVA) {
    if (RVA < HeaderPages || !charge(0, 2 * size_t(PE.NumSections)))
      return std::nullopt;
    std::optional<llvm::ArrayRef<uint8_t>> Result;
    uint64_t End = 0;
    unsigned Owners = 0;
    forEachPESection(PE, [&](const PESectionFields &S, uint16_t) {
      const uint64_t Size =
          std::min(getPESectionContentSize(S.VirtualSize, S.SizeOfRawData),
                   S.SizeOfRawData);
      if (RVA < S.VirtualAddress || RVA - S.VirtualAddress >= Size)
        return;
      ++Owners;
      const uint64_t Offset = RVA - S.VirtualAddress;
      const uint64_t File = uint64_t(S.PointerToRawData) + Offset;
      if (File <= Bytes.size() && Size - Offset <= Bytes.size() - File) {
        Result = Bytes.slice(File, Size - Offset);
        End = uint64_t(S.VirtualAddress) + Size;
      }
    });
    if (Owners != 1 || !Result)
      return std::nullopt;
    unsigned Intersections = 0;
    forEachPESection(PE, [&](const PESectionFields &S, uint16_t) {
      const uint64_t Size =
          getPESectionContentSize(S.VirtualSize, S.SizeOfRawData);
      if (Size && S.VirtualAddress < End &&
          RVA < uint64_t(S.VirtualAddress) + Size)
        ++Intersections;
    });
    return Intersections == 1 ? Result : std::nullopt;
  }

  bool imports(const data_directory &D) {
    auto Data = span(D.RelativeVirtualAddress);
    if (!Data || D.Size > Data->size())
      return false;
    const uint32_t Width = PE.Is64 ? 8 : 4;
    for (uint64_t Offset = 0; Offset + 20 <= D.Size; Offset += 20) {
      if (!charge(20, 1))
        return false;
      const uint64_t At = uint64_t(D.RelativeVirtualAddress) + Offset;
      protect({At, At + 20});
      const auto *Descriptor = Data->data() + Offset;
      if (std::all_of(Descriptor, Descriptor + 20,
                      [](uint8_t Byte) { return Byte == 0; }))
        return true;
      const uint32_t Lookup = readLE<uint32_t>(Descriptor);
      const uint32_t IAT = readLE<uint32_t>(Descriptor + 16);
      if (!detail::validImportLookupSource(
              Lookup, readLE<uint32_t>(Descriptor + 4), false) ||
          !readLE<uint32_t>(Descriptor + 12) || !IAT || IAT % Width ||
          (Lookup && Lookup % Width))
        return false;
      auto Source = span(Lookup ? Lookup : IAT);
      auto Destination = span(IAT);
      if (!Source || !Destination)
        return false;
      bool Terminated = false;
      for (uint64_t At = 0; At + Width <= Source->size(); At += Width) {
        if (!charge(Width, 1) || At + Width > Destination->size())
          return false;
        const uint64_t Value = PE.Is64 ? readLE<uint64_t>(Source->data() + At)
                                       : readLE<uint32_t>(Source->data() + At);
        const uint64_t SourceRVA = uint64_t(Lookup ? Lookup : IAT) + At;
        const uint64_t DestinationRVA = uint64_t(IAT) + At;
        protect({SourceRVA, SourceRVA + Width},
                !Lookup && Value ? std::optional<uint64_t>(DestinationRVA)
                                 : std::nullopt);
        if (!Value) {
          Terminated = true;
          break;
        }
        Writes.push_back({DestinationRVA, DestinationRVA + Width});
      }
      if (!Terminated)
        return false;
    }
    return false;
  }

  bool relocations(const data_directory &D) {
    HasRelocations = true;
    auto Data = span(D.RelativeVirtualAddress);
    if (!Data || D.Size > Data->size())
      return false;
    protect({D.RelativeVirtualAddress,
             uint64_t(D.RelativeVirtualAddress) + D.Size});
    uint64_t At = 0;
    while (At < D.Size) {
      if (!charge(8, 1) || D.Size - At < 8)
        return false;
      const uint64_t Page = readLE<uint32_t>(Data->data() + At);
      const uint64_t Size = readLE<uint32_t>(Data->data() + At + 4);
      if (Page % 4096 || Size < 8 || Size % 2 || Size > D.Size - At)
        return false;
      for (uint64_t I = 8; I < Size; I += 2) {
        if (!charge(2, 1))
          return false;
        const uint16_t Field = readLE<uint16_t>(Data->data() + At + I);
        const unsigned Type = Field >> 12;
        if (Type == IMAGE_REL_BASED_ABSOLUTE)
          continue;
        const unsigned Width = PE.Is64 ? 8 : 4;
        if (Type != (PE.Is64 ? IMAGE_REL_BASED_DIR64 : IMAGE_REL_BASED_HIGHLOW))
          return false;
        const uint64_t RVA = Page + (Field & 0xfff);
        if (RVA < HeaderPages || RVA > getPESizeOfImage(PE) ||
            Width > getPESizeOfImage(PE) - RVA)
          return false;
        Relocations.push_back({RVA, RVA + Width});
      }
      At += Size;
    }
    return true;
  }

  template <class Config> bool configuration(const data_directory &D) {
    auto Data = span(D.RelativeVirtualAddress);
    if (!Data || D.Size < 4 || D.Size > Data->size())
      return false;
    const uint32_t Size = readLE<uint32_t>(Data->data());
    constexpr size_t LegacySize = offsetof(Config, GuardCFCheckFunction);
    if (Size < 4 || Size > Data->size() || !charge(Size, 1))
      return false;
    if (Size > LegacySize || D.Size > LegacySize) {
      // MSVC x86 can retain the 64-byte directory size with a longer
      // self-sized configuration. Inspect every declared extension word.
      using PE32 = coff_load_configuration32;
      if (PE.Is64 || PE.FileHeader->Machine != IMAGE_FILE_MACHINE_I386 ||
          D.Size > Size ||
          Size < offsetof(PE32, GuardFlags) + sizeof(uint32_t) ||
          Size > sizeof(PE32) || Size % sizeof(uint32_t))
        return false;
    }
    const unsigned Width = PE.Is64 ? 8 : 4;
    const auto Pointer = [&](size_t Offset) -> std::optional<uint64_t> {
      if (Size <= Offset)
        return 0;
      if (Size - Offset < Width)
        return std::nullopt;
      return PE.Is64 ? readLE<uint64_t>(Data->data() + Offset)
                     : readLE<uint32_t>(Data->data() + Offset);
    };
    const auto Lock = Pointer(offsetof(Config, LockPrefixTable));
    const auto Edit = Pointer(offsetof(Config, EditList));
    if (!Lock || *Lock || !Edit || *Edit)
      return false;
    const uint64_t RVA = D.RelativeVirtualAddress;
    const uint64_t End = RVA + std::max(uint32_t(D.Size), Size);
    Reads.push_back({{RVA, End}, std::nullopt});
    uint64_t Cursor = RVA;
    const auto RecordPointer = [&](size_t Offset, bool WritesThrough) {
      const auto Value = Pointer(Offset);
      if (!Value)
        return false;
      if (!*Value)
        return true;
      const uint64_t Base = getPEImageBase(PE);
      if (*Value < Base || *Value - Base < HeaderPages ||
          *Value - Base >= getPESizeOfImage(PE))
        return false;
      // Image-relative read tables and loader destinations retain their
      // identity when the complete pointer field is rebased.
      if (Cursor < RVA + Offset)
        RelocationReads.push_back({{Cursor, RVA + Offset}, std::nullopt});
      RelocationReads.push_back(
          {{RVA + Offset, RVA + Offset + Width}, RVA + Offset});
      Cursor = RVA + Offset + Width;
      if (WritesThrough) {
        if (*Value > InvalidVA - Width ||
            (!PE.Is64 && *Value + Width > (uint64_t{1} << 32)) ||
            Width > getPESizeOfImage(PE) - (*Value - Base))
          return false;
        WriterFields.push_back(RVA + Offset);
        Writes.push_back({*Value - Base, *Value - Base + Width});
      }
      return true;
    };
    if (!RecordPointer(offsetof(Config, SecurityCookie), true) ||
        !RecordPointer(offsetof(Config, SEHandlerTable), false))
      return false;
    for (size_t Offset = LegacySize; Offset < Size;
         Offset += sizeof(uint32_t)) {
      const uint32_t Value = readLE<uint32_t>(Data->data() + Offset);
      if (!Value)
        continue;
      switch (Offset) {
      case offsetof(coff_load_configuration32, GuardCFCheckFunction):
      case offsetof(coff_load_configuration32,
                    CastGuardOsDeterminedFailureMode):
        if (!RecordPointer(Offset, true))
          return false;
        break;
      case offsetof(coff_load_configuration32, VolatileMetadataPointer):
        // MSVC emits access-site metadata for x86 on ARM64. It describes
        // memory ordering and supplies no loader write destination.
        if (!RecordPointer(Offset, false))
          return false;
        break;
      case offsetof(coff_load_configuration32, GuardFlags):
        if (Value != uint32_t(GuardFlags::CF_INSTRUMENTED))
          return false;
        break;
      default:
        return false;
      }
    }
    if (Cursor < End)
      RelocationReads.push_back({{Cursor, End}, std::nullopt});
    return true;
  }

public:
  HeaderInitialization(llvm::ArrayRef<uint8_t> Bytes, const PEHeaderPtrs &PE,
                       PEHeaderInitializationWork &Work)
      : Bytes(Bytes), PE(PE), Work(Work),
        HeaderPages(llvm::alignTo(uint64_t(getPESizeOfHeaders(PE)), 4096)) {}

  bool check() {
    if (!charge(getPESizeOfHeaders(PE), size_t(PE.NumSections) + 1))
      return false;
    const auto Count = getPENumberOfRvaAndSizes(PE);
    if (Count > NUM_DATA_DIRECTORIES + 1)
      return false;
    for (unsigned I = 0; I < Count; ++I) {
      const auto *D = getPEDataDirectory(PE, I);
      if (!D)
        return false;
      if (!D->RelativeVirtualAddress && !D->Size)
        continue;
      if (!D->RelativeVirtualAddress || !D->Size)
        return false;
      switch (I) {
      case IMPORT_TABLE:
        if (!imports(*D))
          return false;
        break;
      case BASE_RELOCATION_TABLE:
        if (!relocations(*D))
          return false;
        break;
      case LOAD_CONFIG_TABLE:
        if (!(PE.Is64 ? configuration<coff_load_configuration64>(*D)
                      : configuration<coff_load_configuration32>(*D)))
          return false;
        break;
      case EXPORT_TABLE:
      case RESOURCE_TABLE:
      case EXCEPTION_TABLE:
      case CERTIFICATE_TABLE:
      case DEBUG_DIRECTORY:
        // These declare readers, not loader write destinations.
        break;
      case IAT:
        if (D->RelativeVirtualAddress < HeaderPages ||
            D->RelativeVirtualAddress > getPESizeOfImage(PE) ||
            D->Size > getPESizeOfImage(PE) - D->RelativeVirtualAddress)
          return false;
        break;
      default:
        // TLS, delay/bound imports, managed images and newer initialization
        // protocols need their own complete write-footprint contract.
        return false;
      }
    }
    return checkWrites();
  }
};
} // namespace

bool hasBoundedPEHeaderInitialization(llvm::ArrayRef<uint8_t> Bytes,
                                      const PEHeaderPtrs &PE,
                                      PEHeaderInitializationWork &Work) {
  return HeaderInitialization(Bytes, PE, Work).check();
}
} // namespace neverd::coff_loader
