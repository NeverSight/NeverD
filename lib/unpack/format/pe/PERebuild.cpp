//===- PERebuild.cpp - Observed memory as a PE32+ file --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/X64Imports.h"
#include "PEImage.h"

#include "neverd/emulation/GuestMemory.h"
#include "neverd/object/PEChecksum.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <cstring>

namespace neverd::unpack::pe {
using namespace llvm::object;
using namespace llvm::support;
namespace {
struct Slot {
  uint64_t RVA;
  const ExportBinding *Target;
  ImportOrigin Origin;
};
/// Adjacent cells naming one module: one import descriptor.
struct Group {
  size_t First, Count;
};
/// Where one section's bytes live in the rebuilt file.
struct Placement {
  uint32_t Offset, Size, VirtualSize;
};

template <typename Record>
void store(std::vector<uint8_t> &Bytes, uint64_t Offset, const Record &R) {
  std::memcpy(Bytes.data() + Offset, &R, sizeof(Record));
}
template <typename Record>
Record fetch(llvm::ArrayRef<uint8_t> Bytes, uint64_t Offset) {
  Record R;
  std::memcpy(&R, Bytes.data() + Offset, sizeof(Record));
  return R;
}

std::vector<Group> groupSlots(llvm::ArrayRef<Slot> Slots);

/// Recover complete, terminated arrays of bindable export pointers. A pointer
/// match alone cannot grant ownership of the following program bytes to the
/// loader: each descriptor needs its own intact null terminator.
llvm::Expected<std::vector<Slot>>
findSlots(const Image &In, const Capture &C, llvm::ArrayRef<uint8_t> Memory,
          llvm::ArrayRef<DelayImportRange> Owned) {
  std::vector<Slot> Slots;
  for (const auto &R : In.regions()) {
    const uint64_t End = R.RVA + R.MemorySize;
    // Thunk arrays are usually aligned, but neither the loader nor a stub
    // requires it. A cell is consumed whole, so matches cannot overlap.
    for (uint64_t RVA = R.RVA; RVA + value::PointerSize <= End; ++RVA) {
      const auto Reserved =
          llvm::lower_bound(Owned, RVA, [](const auto &Range, uint64_t At) {
            return Range.End <= At;
          });
      if (Reserved != Owned.end() &&
          Reserved->Begin < RVA + value::PointerSize) {
        RVA = Reserved->End - 1;
        continue;
      }
      const uint64_t Pointer = endian::read64le(Memory.data() + RVA);
      // An address inside the image remains an internal pointer even when
      // that function or datum is exported. Binding it as an import would
      // create a new self-dependency and change the loader's initialization.
      if (Pointer >= C.Base && Pointer - C.Base < In.extent())
        continue;
      auto Export = C.Exports.find(Pointer);
      if (Export == C.Exports.end())
        continue;
      if (Slots.size() == defaults::Imports)
        return failure(unpack::text::ImportLimit);
      Slots.push_back({RVA, &Export->second,
                       endian::read64le(C.Baseline.data() + RVA) == Pointer
                           ? ImportOrigin::Static
                           : ImportOrigin::Runtime});
      RVA += value::PointerSize - 1;
    }
  }
  std::vector<Slot> Terminated;
  for (const auto &G : groupSlots(Slots)) {
    const uint64_t End = Slots[G.First + G.Count - 1].RVA + value::PointerSize;
    const bool InSection = llvm::any_of(In.regions(), [&](const auto &R) {
      return Slots[G.First].RVA >= R.RVA && End >= R.RVA &&
             End <= R.RVA + R.MemorySize &&
             value::PointerSize <= R.RVA + R.MemorySize - End;
    });
    if (InSection && !endian::read64le(Memory.data() + End))
      Terminated.insert(Terminated.end(), Slots.begin() + G.First,
                        Slots.begin() + G.First + G.Count);
  }
  return Terminated;
}

std::vector<Group> groupSlots(llvm::ArrayRef<Slot> Slots) {
  std::vector<Group> Groups;
  for (size_t I = 0; I < Slots.size(); ++I) {
    if (I && Slots[I].RVA - Slots[I - 1].RVA == value::PointerSize &&
        Slots[I].Target->Module == Slots[I - 1].Target->Module)
      ++Groups.back().Count;
    else
      Groups.push_back({I, 1});
  }
  return Groups;
}

/// The import directory, lookup tables and strings of the rebuilt image, laid
/// out for \p RVA. Each cell's file value becomes its lookup-table entry.
std::vector<uint8_t> buildImports(llvm::ArrayRef<Slot> Slots,
                                  llvm::ArrayRef<Group> Groups, uint64_t RVA,
                                  std::vector<uint64_t> &Thunks,
                                  uint32_t &DirectoryBytes) {
  DirectoryBytes =
      (Groups.size() + 1) * sizeof(coff_import_directory_table_entry);
  // Lookup tables are arrays of pointer-sized entries and aligned as such.
  uint64_t Cursor = llvm::alignTo(DirectoryBytes, value::PointerSize);
  std::vector<uint64_t> Lookup(Groups.size());
  for (size_t G = 0; G < Groups.size(); ++G) {
    Lookup[G] = Cursor;
    Cursor += (Groups[G].Count + 1) * value::PointerSize;
  }
  Thunks.assign(Slots.size(), 0);
  std::vector<uint8_t> Strings;
  auto Text = [&](llvm::StringRef S) {
    const uint64_t Offset = Cursor + Strings.size();
    Strings.insert(Strings.end(), S.begin(), S.end());
    Strings.push_back(0);
    return Offset;
  };
  for (size_t I = 0; I < Slots.size(); ++I) {
    const auto &Target = *Slots[I].Target;
    if (Target.Name.empty()) {
      Thunks[I] = value::OrdinalFlag | *Target.Ordinal;
      continue;
    }
    // Hint/name records start on an even offset; the hint is advisory.
    if (Strings.size() % value::HintBytes)
      Strings.push_back(0);
    Thunks[I] = RVA + Cursor + Strings.size();
    Strings.insert(Strings.end(), value::HintBytes, 0);
    Text(Target.Name);
  }
  std::vector<uint64_t> Names(Groups.size());
  for (size_t G = 0; G < Groups.size(); ++G)
    Names[G] = Text(Slots[Groups[G].First].Target->Module);
  std::vector<uint8_t> Out(Cursor + Strings.size());
  std::copy(Strings.begin(), Strings.end(), Out.begin() + Cursor);
  for (size_t G = 0; G < Groups.size(); ++G) {
    coff_import_directory_table_entry Entry{};
    Entry.ImportLookupTableRVA = RVA + Lookup[G];
    Entry.NameRVA = RVA + Names[G];
    Entry.ImportAddressTableRVA = Slots[Groups[G].First].RVA;
    store(Out, G * sizeof(Entry), Entry);
    for (size_t I = 0; I < Groups[G].Count; ++I)
      endian::write64le(Out.data() + Lookup[G] + I * value::PointerSize,
                        Thunks[Groups[G].First + I]);
  }
  return Out;
}

struct TailRepairs {
  struct Site {
    // Conflicting starts share an end, so their union begins at the earliest
    // start. Keep that coverage even after this continuation is rejected.
    uint64_t BeginRVA, ReturnRVA;
    const ExportBinding *Target;
    x64::ImportSite Patch;
  };
  std::vector<Site> Sites;
  uint64_t CellBytes = 0, Conflicts = 0;
};

/// Use existing export cells when possible; new cells belong to the appended
/// metadata section. Zero bytes in an existing region may be live BSS and
/// cannot authorize borrowing that storage for an import table.
llvm::Expected<TailRepairs> planTailImports(const Image &In, const Capture &C,
                                            llvm::ArrayRef<TailImport> Calls,
                                            llvm::ArrayRef<uint8_t> Memory,
                                            std::vector<Slot> &Slots) {
  TailRepairs Repairs;
  if (In.architecture() != emulation::GuestArchitecture::X64)
    return Repairs;
  for (const auto &Call : Calls) {
    if (Call.ReturnAddress < C.Base ||
        Call.ReturnAddress - C.Base >= Memory.size() ||
        Call.Target.Module.empty() ||
        (Call.Target.Name.empty() && !Call.Target.Ordinal))
      continue;
    const uint64_t ReturnRVA = Call.ReturnAddress - C.Base;
    if (Call.InstructionAddress < C.Base)
      continue;
    auto Patch =
        x64::importSite(Memory, ReturnRVA, Call.AddressLoad,
                        Call.InstructionAddress - C.Base, Call.ResultRegister);
    if (!Patch)
      continue;
    auto Existing = llvm::find_if(
        Repairs.Sites, [&](const auto &S) { return S.ReturnRVA == ReturnRVA; });
    if (Existing != Repairs.Sites.end()) {
      Existing->BeginRVA = std::min(Existing->BeginRVA, Patch->RVA);
      if (Existing->Target && (*Existing->Target != Call.Target ||
                               Existing->Patch.RVA != Patch->RVA ||
                               Existing->Patch.Register != Patch->Register)) {
        Existing->Target = nullptr;
        ++Repairs.Conflicts;
      }
      continue;
    }
    Repairs.Sites.push_back({Patch->RVA, ReturnRVA, &Call.Target, *Patch});
  }
  // A byte sequence can describe overlapping sites. No observation gives one
  // permission to overwrite another, even when both reach the same export.
  llvm::sort(Repairs.Sites, [](const auto &A, const auto &B) {
    return A.BeginRVA < B.BeginRVA;
  });
  for (size_t First = 0; First < Repairs.Sites.size();) {
    size_t End = First + 1;
    uint64_t Limit = Repairs.Sites[First].ReturnRVA;
    while (End < Repairs.Sites.size() && Repairs.Sites[End].BeginRVA < Limit) {
      Limit = std::max(Limit, Repairs.Sites[End].ReturnRVA);
      ++End;
    }
    if (End - First > 1)
      for (size_t At = First; At < End; ++At)
        if (Repairs.Sites[At].Target) {
          Repairs.Sites[At].Target = nullptr;
          ++Repairs.Conflicts;
        }
    First = End;
  }
  llvm::erase_if(Repairs.Sites, [](const auto &S) { return !S.Target; });
  for (const auto &Site : Repairs.Sites) {
    const auto *Binding = Site.Target;
    if (llvm::any_of(Slots,
                     [&](const Slot &S) { return *S.Target == *Binding; }))
      continue;
    if (Slots.size() == defaults::Imports)
      return failure(unpack::text::ImportLimit);
    // Each fresh cell has its own zero terminator. Its lookup table controls
    // the loader's count, so no existing program byte needs to become zero.
    const uint64_t RVA = In.extent() + Repairs.CellBytes;
    if (RVA > UINT32_MAX - 2 * value::PointerSize)
      return failure(text::ImageSize);
    Slots.push_back({RVA, Binding, ImportOrigin::Runtime});
    Repairs.CellBytes += 2 * value::PointerSize;
  }
  for (const auto &Site : Repairs.Sites) {
    const auto *Binding = Site.Target;
    const auto Cell = llvm::find_if(
        Slots, [&](const Slot &S) { return *S.Target == *Binding; });
    const int64_t Displacement =
        int64_t(Cell->RVA) - int64_t(Site.Patch.instructionEnd());
    if (Displacement != int32_t(Displacement))
      return failure(text::ImportRange);
  }
  return Repairs;
}

void redirectTailImports(const TailRepairs &Repairs, llvm::ArrayRef<Slot> Slots,
                         llvm::MutableArrayRef<uint8_t> Memory) {
  for (const auto &Site : Repairs.Sites) {
    const auto *Binding = Site.Target;
    const auto Cell = llvm::find_if(
        Slots, [&](const Slot &S) { return *S.Target == *Binding; });
    x64::writeImport(Memory, Site.Patch, Cell->RVA);
  }
}

/// Section access flags for the permissions the region's pages had.
uint32_t observedAccess(const Capture &C, const ImageRegion &R) {
  unsigned Access = 0;
  const uint64_t First = R.RVA / unpack::value::PageSize;
  const uint64_t Pages = R.MemorySize / unpack::value::PageSize;
  for (uint64_t Page = First; Page < First + Pages; ++Page)
    Access |= C.PageAccess[Page];
  uint32_t Flags = 0;
  if (Access & emulation::Read)
    Flags |= llvm::COFF::IMAGE_SCN_MEM_READ;
  if (Access & emulation::Write)
    Flags |= llvm::COFF::IMAGE_SCN_MEM_WRITE;
  if (Access & emulation::Execute)
    Flags |= llvm::COFF::IMAGE_SCN_MEM_EXECUTE | llvm::COFF::IMAGE_SCN_CNT_CODE;
  return Flags;
}

/// Import descriptors name each address array independently. The optional
/// IAT directory also tells the native loader which pages it may protect.
/// Writable cells, including appended cells, need no such protection range.
/// Including their intervening data or code would change program semantics.
llvm::Expected<data_directory>
iatProtection(const Image &In, llvm::ArrayRef<uint32_t> Permissions,
              llvm::ArrayRef<Slot> Slots) {
  data_directory Directory{};
  const auto Regions = In.regions();
  uint64_t Begin = In.extent(), End = 0;
  for (const auto &S : Slots) {
    const auto *Region = In.regionAt(S.RVA);
    if (!Region || (Permissions[Region - Regions.data()] &
                    llvm::COFF::IMAGE_SCN_MEM_WRITE))
      continue;
    Begin = std::min(Begin, S.RVA);
    End = std::max(End, S.RVA + 2 * value::PointerSize);
  }
  if (!End)
    return Directory;
  uint64_t Covered = Begin;
  for (size_t I = 0; I < Regions.size(); ++I) {
    const auto &Region = Regions[I];
    if (Region.RVA + Region.MemorySize <= Covered)
      continue;
    const uint32_t Access = Permissions[I];
    if (Region.RVA > Covered || !(Access & llvm::COFF::IMAGE_SCN_MEM_READ) ||
        (Access &
         (llvm::COFF::IMAGE_SCN_MEM_WRITE | llvm::COFF::IMAGE_SCN_MEM_EXECUTE)))
      return failure(text::IATProtection);
    Covered = Region.RVA + Region.MemorySize;
    if (Covered >= End) {
      Directory.RelativeVirtualAddress = Begin;
      Directory.Size = End - Begin;
      return Directory;
    }
  }
  return failure(text::IATProtection);
}

/// Keep complete debug records and mapped payloads in the file. A zero suffix
/// still belongs to the record or payload; trimming it before rewriting file
/// offsets could discard the rewritten field or truncate an all-zero payload.
void retainDebugStorage(const Image &In,
                        std::vector<data_directory> &Directories,
                        llvm::ArrayRef<uint8_t> Memory,
                        std::vector<uint64_t> &Required) {
  if (llvm::COFF::DEBUG_DIRECTORY >= Directories.size())
    return;
  auto &Debug = Directories[llvm::COFF::DEBUG_DIRECTORY];
  const uint64_t RVA = Debug.RelativeVirtualAddress, Size = Debug.Size;
  const uint64_t Records = Size / sizeof(debug_directory);
  if (!Size)
    return;
  if (Size % sizeof(debug_directory) || Records > value::MaxDebugRecords ||
      RVA > In.extent() || Size > In.extent() - RVA) {
    Debug = data_directory{};
    return;
  }
  const auto Regions = In.regions();
  uint64_t End = RVA;
  while (End < RVA + Size) {
    const auto *Region = In.regionAt(End);
    if (!Region) {
      Debug = data_directory{};
      return;
    }
    End = std::min(RVA + Size, Region->RVA + Region->MemorySize);
  }
  for (const auto &Region : Regions) {
    const uint64_t Begin = std::max(RVA, Region.RVA);
    End = std::min(RVA + Size, Region.RVA + Region.MemorySize);
    if (Begin < End)
      Required[&Region - Regions.data()] = End - Region.RVA;
  }
  for (uint64_t I = 0; I < Records; ++I) {
    const auto Record =
        fetch<debug_directory>(Memory, RVA + I * sizeof(debug_directory));
    const uint64_t Payload = Record.AddressOfRawData;
    const auto *Region = In.regionAt(Payload);
    if (!Payload || !Region ||
        Record.SizeOfData > Region->MemorySize - (Payload - Region->RVA))
      continue;
    auto &Minimum = Required[Region - Regions.data()];
    Minimum = std::max(Minimum, Payload - Region->RVA + Record.SizeOfData);
  }
}

/// Debug records name their payload by file offset as well as by RVA. Their
/// validated storage requirements have already participated in placement.
void relocateDebugRecords(const Image &In, llvm::ArrayRef<Placement> Placed,
                          std::vector<data_directory> &Directories,
                          std::vector<uint8_t> &Memory,
                          uint64_t InputOverlayOffset,
                          uint64_t OutputOverlayOffset) {
  if (llvm::COFF::DEBUG_DIRECTORY >= Directories.size())
    return;
  const auto Regions = In.regions();
  auto FileOffset = [&](uint64_t RVA, uint64_t Size) -> uint32_t {
    for (size_t I = 0; I < Regions.size(); ++I) {
      const auto &R = Regions[I];
      if (RVA >= R.RVA && RVA - R.RVA < Placed[I].Size &&
          Size <= Placed[I].Size - (RVA - R.RVA))
        return Placed[I].Offset + uint32_t(RVA - R.RVA);
    }
    return 0;
  };
  auto &Debug = Directories[llvm::COFF::DEBUG_DIRECTORY];
  const uint64_t RVA = Debug.RelativeVirtualAddress, Size = Debug.Size;
  const uint64_t Records = Size / sizeof(debug_directory);
  for (uint64_t I = 0; I < Records; ++I) {
    const uint64_t At = RVA + I * sizeof(debug_directory);
    auto Record = fetch<debug_directory>(Memory, At);
    if (Record.AddressOfRawData) {
      Record.PointerToRawData =
          FileOffset(Record.AddressOfRawData, Record.SizeOfData);
    } else {
      // File-only payloads can live in the retained overlay. Its bytes move
      // together, so preserve their relative file position without inventing
      // an RVA or treating an invalid mapped payload as file-only data.
      const uint64_t Source = Record.PointerToRawData;
      Record.PointerToRawData =
          Source >= InputOverlayOffset && Source <= In.file().size() &&
                  Record.SizeOfData <= In.file().size() - Source
              ? OutputOverlayOffset + (Source - InputOverlayOffset)
              : 0;
    }
    store(Memory, At, Record);
  }
}
} // namespace

llvm::Expected<RebuiltImage> rebuild(const Image &In, const Capture &C,
                                     const RebuildPlan &Plan) {
  const Headers &H = In.headers();
  const auto Regions = In.regions();
  const auto File = In.file();
  const uint64_t Extent = In.extent();
  if (C.Memory.size() != Extent || C.Baseline.size() != Extent ||
      C.PageAccess.size() != Extent / unpack::value::PageSize)
    return failure(unpack::text::ImageChanged);
  if (!In.regionAt(C.EntryRVA))
    return failure(unpack::text::EntryOutside);
  std::vector<uint8_t> Memory(C.Memory.begin(), C.Memory.end());
  auto Delay = restoreDelayImports(In, C, Memory);
  if (!Delay)
    return Delay.takeError();
  auto Slots = findSlots(In, C, Memory, Delay->Metadata);
  if (!Slots)
    return Slots.takeError();
  if (Delay->Resolved.size() > defaults::Imports - Slots->size())
    return failure(unpack::text::ImportLimit);
  for (const auto &Binding : Delay->Resolved)
    Slots->push_back({Binding.RVA, Binding.Target, ImportOrigin::Runtime});
  llvm::sort(*Slots,
             [](const auto &A, const auto &B) { return A.RVA < B.RVA; });
  auto Repairs = planTailImports(In, C, Plan.TailImports, Memory, *Slots);
  if (!Repairs)
    return Repairs.takeError();
  std::vector<uint32_t> Permissions;
  Permissions.reserve(Regions.size());
  for (const auto &Region : Regions)
    Permissions.push_back(observedAccess(C, Region));
  auto IAT = iatProtection(In, Permissions, *Slots);
  if (!IAT)
    return IAT.takeError();
  redirectTailImports(*Repairs, *Slots, Memory);
  const auto Groups = groupSlots(*Slots);

  // The metadata section follows the image, so every recovered RVA is kept.
  const uint64_t MetadataRVA = Extent;
  std::vector<uint64_t> Thunks;
  uint32_t DirectoryBytes = 0;
  std::vector<uint8_t> Metadata;
  if (!Slots->empty())
    Metadata = buildImports(*Slots, Groups, MetadataRVA + Repairs->CellBytes,
                            Thunks, DirectoryBytes);
  Metadata.insert(Metadata.begin(), Repairs->CellBytes, 0);
  auto RebuiltTLS = rebuildTLS(In, C, MetadataRVA, Metadata);
  if (!RebuiltTLS)
    return RebuiltTLS.takeError();
  auto Entry = rebuildEntry(In, C, MetadataRVA, Metadata);
  if (!Entry)
    return Entry.takeError();
  const uint64_t NewImageSize =
      MetadataRVA +
      llvm::alignTo(Metadata.size(), uint64_t(H.SectionAlignment));
  if (NewImageSize > UINT32_MAX)
    return failure(text::ImageSize);

  for (size_t I = 0; I < Slots->size(); ++I) {
    const uint64_t RVA = (*Slots)[I].RVA;
    if (RVA < Extent)
      endian::write64le(Memory.data() + RVA, Thunks[I]);
    else
      endian::write64le(Metadata.data() + RVA - MetadataRVA, Thunks[I]);
  }

  const bool AddSection = !Metadata.empty();
  const uint64_t Count = Regions.size() + AddSection;
  const uint64_t TableEnd = H.SectionTableOffset + Count * sizeof(coff_section);
  const uint64_t HeaderBytes = std::max<uint64_t>(
      H.SizeOfHeaders, llvm::alignTo(TableEnd, uint64_t(H.FileAlignment)));
  if (Count > value::MaxSections || HeaderBytes > Regions.front().RVA)
    return failure(text::HeaderRoom);

  auto Directories = H.Directories;
  std::vector<uint64_t> Required(Regions.size());
  for (const auto &Range : Delay->Metadata)
    for (size_t I = 0; I < Regions.size(); ++I) {
      const auto &R = Regions[I];
      const uint64_t End = std::min(Range.End, R.RVA + R.MemorySize);
      if (Range.Begin < End && R.RVA < Range.End)
        Required[I] = std::max(Required[I], End - R.RVA);
    }
  retainDebugStorage(In, Directories, Memory, Required);

  // Section bytes are the observed memory without its trailing zero fill.
  std::vector<Placement> Placed;
  uint64_t Cursor = HeaderBytes;
  auto Place = [&](llvm::ArrayRef<uint8_t> Bytes, uint64_t VirtualSize,
                   uint64_t Minimum) {
    uint64_t Used = Bytes.size();
    while (Used > Minimum && !Bytes[Used - 1])
      --Used;
    const uint64_t Size = llvm::alignTo(Used, uint64_t(H.FileAlignment));
    Placed.push_back({Size ? uint32_t(Cursor) : 0, uint32_t(Size),
                      uint32_t(std::max(VirtualSize, Used))});
    Cursor += Size;
  };
  for (size_t I = 0; I < Regions.size(); ++I) {
    const auto &R = Regions[I];
    // A loader reads the zero qword after each import run. That terminator is
    // itself zero, so trimming trailing zeros would leave it outside the
    // section and the run would not be file backed.
    uint64_t ExtentInSection =
        H.Sections[I].VirtualSize ? H.Sections[I].VirtualSize : R.MemorySize;
    for (const auto &S : *Slots) {
      if (S.RVA < R.RVA || S.RVA - R.RVA >= R.MemorySize)
        continue;
      const uint64_t Terminator = S.RVA + 2 * value::PointerSize;
      if (Terminator < R.RVA || Terminator - R.RVA > R.MemorySize)
        return failure(text::ImageSize);
      ExtentInSection = std::max(ExtentInSection, Terminator - R.RVA);
    }
    Place(llvm::ArrayRef(Memory).slice(R.RVA, R.MemorySize), ExtentInSection,
          Required[I]);
  }
  if (AddSection)
    Place(Metadata, Metadata.size(), 0);
  uint64_t InputEnd = H.SizeOfHeaders;
  for (const auto &R : Regions)
    InputEnd = std::max(InputEnd, R.FileOffset + R.FileSize);
  const auto Overlay =
      File.drop_front(std::min<uint64_t>(InputEnd, File.size()));
  if (Cursor + Overlay.size() > UINT32_MAX)
    return failure(text::ImageSize);

  if (IAT->Size && Directories.size() <= llvm::COFF::IAT) {
    // An input can leave trailing directory entries undeclared while still
    // reserving them in the optional header. Use that space without moving
    // the section table or overwriting an original section header.
    const uint64_t End = H.OptionalHeaderOffset + sizeof(pe32plus_header) +
                         (llvm::COFF::IAT + 1) * sizeof(data_directory);
    if (End > H.SectionTableOffset)
      return failure(text::HeaderRoom);
    Directories.resize(llvm::COFF::IAT + 1);
  }
  relocateDebugRecords(In, Placed, Directories, Memory, InputEnd, Cursor);
  if (RebuiltTLS->DirectoryRVA) {
    Directories[llvm::COFF::TLS_TABLE].RelativeVirtualAddress =
        RebuiltTLS->DirectoryRVA;
    Directories[llvm::COFF::TLS_TABLE].Size = sizeof(coff_tls_directory64);
  }
#define NEVERD_UNPACK_PE_STALE_DIRECTORY(Name)                                 \
  if (llvm::COFF::Name < Directories.size())                                   \
    Directories[llvm::COFF::Name] = data_directory{};
#include "PE.def"
#undef NEVERD_UNPACK_PE_STALE_DIRECTORY
  if (llvm::COFF::IMPORT_TABLE < Directories.size())
    Directories[llvm::COFF::IMPORT_TABLE] = data_directory{};
  if (!Slots->empty()) {
    if (llvm::COFF::IMPORT_TABLE >= Directories.size())
      return failure(text::HeaderRoom);
    Directories[llvm::COFF::IMPORT_TABLE].RelativeVirtualAddress =
        MetadataRVA + Repairs->CellBytes;
    Directories[llvm::COFF::IMPORT_TABLE].Size = DirectoryBytes;
  }
  if (IAT->Size)
    Directories[llvm::COFF::IAT] = *IAT;

  RebuiltImage Out;
  for (const auto &Site : Repairs->Sites)
    if (Site.Patch.Register)
      ++Out.RepairedImportLoads;
    else
      ++Out.RepairedTailCalls;
  Out.ConflictingTailCalls = Repairs->Conflicts;
  Out.MaterializedTLSCallbacks = RebuiltTLS->MaterializedCallbacks;
  Out.LoaderEntryRVA = *Entry;
  Out.File.assign(Cursor + Overlay.size(), 0);
  std::copy_n(File.begin(), H.SizeOfHeaders, Out.File.begin());
  auto COFF = fetch<coff_file_header>(Out.File, H.FileHeaderOffset);
  COFF.NumberOfSections = Count;
  COFF.Characteristics =
      COFF.Characteristics | llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED;
  store(Out.File, H.FileHeaderOffset, COFF);
  auto PE = fetch<pe32plus_header>(Out.File, H.OptionalHeaderOffset);
  PE.AddressOfEntryPoint = *Entry;
  PE.ImageBase = C.Base;
  PE.SizeOfImage = NewImageSize;
  PE.SizeOfHeaders = HeaderBytes;
  PE.NumberOfRvaAndSize = Directories.size();
  PE.CheckSum = 0;
  PE.DLLCharacteristics =
      PE.DLLCharacteristics &
      ~(llvm::COFF::IMAGE_DLL_CHARACTERISTICS_DYNAMIC_BASE |
        llvm::COFF::IMAGE_DLL_CHARACTERISTICS_HIGH_ENTROPY_VA);
  store(Out.File, H.OptionalHeaderOffset, PE);
  for (size_t I = 0; I < Directories.size(); ++I)
    store(Out.File,
          H.OptionalHeaderOffset + sizeof(PE) + I * sizeof(data_directory),
          Directories[I]);
  for (size_t I = 0; I < Count; ++I) {
    const bool Added = I == Regions.size();
    const uint64_t At = H.SectionTableOffset + I * sizeof(coff_section);
    coff_section Header{};
    if (!Added) {
      const auto &R = Regions[I];
      Header = fetch<coff_section>(File, At);
      Header.VirtualSize = Placed[I].VirtualSize;
      // A section has the access the program actually had at the transfer.
      const uint32_t Access = llvm::COFF::IMAGE_SCN_MEM_READ |
                              llvm::COFF::IMAGE_SCN_MEM_WRITE |
                              llvm::COFF::IMAGE_SCN_MEM_EXECUTE;
      uint32_t Flags =
          (H.Sections[I].Characteristics & ~Access) | Permissions[I];
      if (Placed[I].Size) {
        Flags &= ~uint32_t(llvm::COFF::IMAGE_SCN_CNT_UNINITIALIZED_DATA);
        if (!(Flags & llvm::COFF::IMAGE_SCN_CNT_CODE))
          Flags |= llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA;
      }
      Header.Characteristics = Flags;
      uint64_t Generated = 0;
      for (uint64_t RVA = R.RVA; RVA < R.RVA + R.MemorySize; ++RVA)
        Generated += C.Memory[RVA] != C.Baseline[RVA];
      Out.Sections.push_back(
          {R.Name, R.RVA, Placed[I].VirtualSize, Placed[I].Size, Generated});
      // File alignment may exceed the committed page range. Its padding is
      // already zero; bytes from an image gap do not belong to this section.
      std::copy_n(Memory.begin() + R.RVA,
                  std::min<uint64_t>(Placed[I].Size, R.MemorySize),
                  Out.File.begin() + Placed[I].Offset);
    } else {
      const llvm::StringRef Name = unpack::text::MetadataSection;
      std::copy(Name.begin(), Name.end(), Header.Name);
      Header.VirtualSize = Placed[I].VirtualSize;
      Header.VirtualAddress = MetadataRVA;
      Header.Characteristics = llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
                               llvm::COFF::IMAGE_SCN_MEM_READ;
      if (Repairs->CellBytes)
        Header.Characteristics |= llvm::COFF::IMAGE_SCN_MEM_WRITE;
      if (RebuiltTLS->HasCode || *Entry != C.EntryRVA)
        Header.Characteristics |=
            llvm::COFF::IMAGE_SCN_MEM_EXECUTE | llvm::COFF::IMAGE_SCN_CNT_CODE;
      Out.Sections.push_back(
          {Name.str(), MetadataRVA, Placed[I].VirtualSize, Placed[I].Size, 0});
      std::copy_n(Metadata.begin(),
                  std::min<uint64_t>(Metadata.size(), Placed[I].Size),
                  Out.File.begin() + Placed[I].Offset);
    }
    Header.SizeOfRawData = Placed[I].Size;
    Header.PointerToRawData = Placed[I].Offset;
    store(Out.File, At, Header);
  }
  std::copy(Overlay.begin(), Overlay.end(), Out.File.begin() + Cursor);
  for (const auto &S : *Slots)
    Out.Imports.push_back(
        {S.Target->Module, S.Target->Name,
         S.Target->Name.empty() ? S.Target->Ordinal : std::nullopt, S.RVA,
         S.Origin});
  // The emitted headers must satisfy the same contract as an input.
  if (In.domain() == ExecutionDomain::Kernel) {
    // A kernel loader chooses the runtime address. Keeping the observed base
    // while stripping generated-content relocations cannot bind a fresh load.
    // Original relocation entries alone do not cover generated pointers.
    Out.RuntimeDependency =
        "kernel image relocation state requires reconstruction";
    const uint64_t Offset =
        H.OptionalHeaderOffset + offsetof(pe32plus_header, CheckSum);
    auto Checksum = computePEChecksum(Out.File, Offset);
    if (!Checksum)
      return failure("driver image exceeds the PE checksum contract");
    store(Out.File, Offset, llvm::support::ulittle32_t(*Checksum));
  }
  if (auto Check = Image::read(Out.File); !Check)
    return failure(unpack::text::Rebuilt + llvm::toString(Check.takeError()));
  return Out;
}
} // namespace neverd::unpack::pe
