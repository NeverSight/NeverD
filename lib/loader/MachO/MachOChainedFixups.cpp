//===- MachOChainedFixups.cpp - Mach-O chained fixups --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MachOArm64eFixups.h"
#include "MachODyldFixups.h"

#include "neverd/loader/Limits.h"
#include "neverd/loader/MachO/MachOArm64e.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <cstring>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

#define DEBUG_TYPE "neverd-macho-loader"

namespace neverd {
namespace macho_loader {

using namespace llvm::MachO;

namespace {

using detail::Arm64eImportRecord;

/// Decode the import table without deduplicating it: chained pointer records
/// refer to this table by ordinal, so even two equal names must retain distinct
/// indices and addends.  A malformed entry remains an invalid element rather
/// than shifting every ordinal after it.
std::optional<std::vector<Arm64eImportRecord>>
decodeChainedImports(const uint8_t *BasePtr, size_t FileSize,
                     const ChainedFixupsInfo &Info, const BinaryImage &Img) {
  const uint64_t DataOff = Info.DataOff;
  const uint64_t DataSize = Info.DataSize;
  if (!BasePtr || DataOff == 0 ||
      DataSize < sizeof(dyld_chained_fixups_header) ||
      !rangeInBounds(DataOff, DataSize, FileSize))
    return std::nullopt;

  dyld_chained_fixups_header Hdr{};
  std::memcpy(&Hdr, BasePtr + DataOff, sizeof(Hdr));
  if (Hdr.imports_count == 0)
    return std::vector<Arm64eImportRecord>{};
  if (Hdr.imports_offset == 0 || Hdr.symbols_offset == 0 ||
      Hdr.symbols_format != 0 || Hdr.imports_offset >= DataSize ||
      Hdr.symbols_offset >= DataSize)
    return std::nullopt;

  const uint64_t DataEnd = DataOff + DataSize;
  const uint64_t ImportsAbs = DataOff + Hdr.imports_offset;
  const uint64_t SymbolsAbs = DataOff + Hdr.symbols_offset;
  const char *StrTab = reinterpret_cast<const char *>(BasePtr + SymbolsAbs);
  const size_t StrSize = static_cast<size_t>(DataEnd - SymbolsAbs);

  uint64_t ImportEntrySize = 0;
  switch (Hdr.imports_format) {
  case DYLD_CHAINED_IMPORT:
    ImportEntrySize = sizeof(dyld_chained_import);
    break;
  case DYLD_CHAINED_IMPORT_ADDEND:
    ImportEntrySize = sizeof(dyld_chained_import_addend);
    break;
  case DYLD_CHAINED_IMPORT_ADDEND64:
    ImportEntrySize = sizeof(dyld_chained_import_addend64);
    break;
  default:
    return std::nullopt;
  }
  if (Hdr.imports_count > (InvalidVA - ImportsAbs) / ImportEntrySize)
    return std::nullopt;
  const uint64_t ImportsSize =
      static_cast<uint64_t>(Hdr.imports_count) * ImportEntrySize;
  if (!rangeInBounds(ImportsAbs, ImportsSize, DataEnd))
    return std::nullopt;

  std::vector<Arm64eImportRecord> Records(Hdr.imports_count);

  for (uint32_t I = 0; I < Hdr.imports_count; ++I) {
    uint32_t NameOff = 0;
    int32_t LibOrdinal = 0;
    int64_t Addend = 0;
    bool WeakImport = false;
    uint64_t EntOff = ImportsAbs;
    uint64_t EntSize = 0;

    switch (Hdr.imports_format) {
    case DYLD_CHAINED_IMPORT: {
      EntSize = sizeof(dyld_chained_import);
      if (I > (InvalidVA - EntOff) / EntSize)
        return std::nullopt;
      EntOff += static_cast<uint64_t>(I) * EntSize;
      if (!rangeInBounds(EntOff, EntSize, DataEnd))
        return std::nullopt;
      dyld_chained_import E{};
      std::memcpy(&E, BasePtr + EntOff, sizeof(E));
      NameOff = E.name_offset;
      LibOrdinal = chainedLibraryOrdinal(E.lib_ordinal, 256);
      WeakImport = E.weak_import != 0;
      break;
    }
    case DYLD_CHAINED_IMPORT_ADDEND: {
      EntSize = sizeof(dyld_chained_import_addend);
      if (I > (InvalidVA - EntOff) / EntSize)
        return std::nullopt;
      EntOff += static_cast<uint64_t>(I) * EntSize;
      if (!rangeInBounds(EntOff, EntSize, DataEnd))
        return std::nullopt;
      dyld_chained_import_addend E{};
      std::memcpy(&E, BasePtr + EntOff, sizeof(E));
      NameOff = E.name_offset;
      LibOrdinal = chainedLibraryOrdinal(E.lib_ordinal, 256);
      WeakImport = E.weak_import != 0;
      Addend = E.addend;
      break;
    }
    case DYLD_CHAINED_IMPORT_ADDEND64: {
      EntSize = sizeof(dyld_chained_import_addend64);
      if (I > (InvalidVA - EntOff) / EntSize)
        return std::nullopt;
      EntOff += static_cast<uint64_t>(I) * EntSize;
      if (!rangeInBounds(EntOff, EntSize, DataEnd))
        return std::nullopt;
      dyld_chained_import_addend64 E{};
      std::memcpy(&E, BasePtr + EntOff, sizeof(E));
      NameOff = E.name_offset;
      LibOrdinal = chainedLibraryOrdinal(E.lib_ordinal, 65536);
      WeakImport = E.weak_import != 0;
      std::memcpy(&Addend, &E.addend, sizeof(Addend));
      break;
    }
    default:
      return std::nullopt;
    }

    if (NameOff >= StrSize)
      continue;
    const char *Name = StrTab + NameOff;
    const size_t Remaining = StrSize - NameOff;
    const void *Term = std::memchr(Name, 0, Remaining);
    if (!Term)
      continue;
    std::string SymName(Name, static_cast<const char *>(Term) -
                                  static_cast<const char *>(Name));
    if (SymName.empty())
      continue;

    std::string DylibName;
    if (LibOrdinal > 0 &&
        static_cast<uint64_t>(LibOrdinal) <= Img.DynInfo.NeededLibs.size())
      DylibName = Img.DynInfo.NeededLibs[static_cast<size_t>(LibOrdinal - 1)];
    std::string BindingModule;
    if (LibOrdinal > 0 &&
        static_cast<uint64_t>(LibOrdinal) <= Img.MachODylibReferences.size()) {
      const MachODylibReference &Reference =
          Img.MachODylibReferences[static_cast<size_t>(LibOrdinal - 1)];
      BindingModule = Reference.Name;
      WeakImport = WeakImport || Reference.Weak;
    }
    Records[I].Name = std::move(SymName);
    Records[I].Module = std::move(BindingModule);
    Records[I].DisplayModule =
        DylibName.empty() ? kExternModule.str() : std::move(DylibName);
    Records[I].Addend = Addend;
    Records[I].WeakImport = WeakImport;
    Records[I].LibraryOrdinal = LibOrdinal;
    Records[I].Valid = true;
  }
  return Records;
}

/// Decode a whole segment before publishing any of its fixups. Authenticated
/// slots keep a signing recipe and their original bytes; only unauthenticated
/// slots are materialized as preferred-base runtime pointer values.
bool parseArm64eSegment(
    const dyld_chained_starts_in_segment &Seg,
    const arm64e::ChainFormat &Format, const uint8_t *Bytes, va_t Base,
    uint32_t SegmentIndex,
    const std::optional<std::vector<Arm64eImportRecord>> &Imports,
    BinaryImage &Img) {
  const auto fail = [&]() {
    Img.MachOChainedFixupsError = arm64e::kMalformedChainedFixups;
    return false;
  };
  const size_t StartOffset =
      offsetof(dyld_chained_starts_in_segment, page_start);
  if (!Imports || (Format.Segmented && Img.IsRelocatable) ||
      (Seg.page_size != arm64e::kSmallPageBytes &&
       Seg.page_size != arm64e::kLargePageBytes) ||
      Seg.size < StartOffset ||
      uint64_t(Seg.page_count) * sizeof(uint16_t) > Seg.size - StartOffset ||
      Seg.segment_offset > InvalidVA - Base)
    return fail();
  const va_t SegmentVA = Base + Seg.segment_offset;
  const Segment *Owner = Img.getSegmentFor(SegmentVA);
  if (!Owner || Owner->VA != SegmentVA ||
      (!Img.IsRelocatable && (SegmentIndex >= Img.Segments.size() ||
                              &Img.Segments[SegmentIndex] != Owner)) ||
      (Seg.page_count &&
       uint64_t(Seg.page_count - 1) * Seg.page_size >= Owner->Size))
    return fail();
  const size_t StartCount = (Seg.size - StartOffset) / sizeof(uint16_t);
  const auto startAt = [&](size_t Index) {
    return readLE<uint16_t>(Bytes + StartOffset + Index * sizeof(uint16_t));
  };
  std::vector<detail::Arm64eFixup> Pending;
  std::set<va_t> Slots;
  for (unsigned Page = 0; Page < Seg.page_count; ++Page) {
    const uint16_t Start = startAt(Page);
    if (Start == DYLD_CHAINED_PTR_START_NONE)
      continue;
    const uint64_t PageOffset = uint64_t(Page) * Seg.page_size;
    if (PageOffset > InvalidVA - Base - Seg.segment_offset)
      return fail();
    const va_t PageVA = Base + Seg.segment_offset + PageOffset;
    std::vector<uint16_t> Starts;
    if (Start & DYLD_CHAINED_PTR_START_MULTI) {
      size_t Index = Start & ~DYLD_CHAINED_PTR_START_MULTI;
      if (Index < Seg.page_count)
        return fail();
      for (;;) {
        if (Index >= StartCount)
          return fail();
        const uint16_t Value = startAt(Index++);
        Starts.push_back(Value & ~DYLD_CHAINED_PTR_START_LAST);
        if (Value & DYLD_CHAINED_PTR_START_LAST)
          break;
      }
    } else {
      Starts.push_back(Start);
    }
    for (uint32_t At : Starts) {
      for (;;) {
        if (At % Format.Stride || At > Seg.page_size - sizeof(uint64_t) ||
            At > InvalidVA - PageVA ||
            Pending.size() >=
                limits::loader::
                    kMachOChainedFixupsParseChainedFixupsRebasesGuardLimit)
          return fail();
        const va_t Slot = PageVA + At;
        if (!rangeInBounds(Slot - SegmentVA, sizeof(uint64_t), Owner->Size))
          return fail();
        const uint8_t *Storage = Img.readVA(Slot, sizeof(uint64_t));
        const auto NextSlot = Slots.lower_bound(Slot);
        if ((NextSlot != Slots.end() && *NextSlot - Slot < sizeof(uint64_t)) ||
            (NextSlot != Slots.begin() &&
             Slot - *std::prev(NextSlot) < sizeof(uint64_t)))
          return fail();
        if (!Storage || !Slots.insert(Slot).second ||
            Img.MachOAuthenticatedPointers.count(Slot) ||
            Img.MachOResolvedChainedPointerSlots.count(Slot))
          return fail();
        const uint64_t Raw = readLE<uint64_t>(Storage);
        auto F =
            detail::decodeArm64eFixup(Raw, Format, Slot, Base, *Imports, Img);
        if (!F)
          return fail();
        const uint32_t Next = F->Next;
        Pending.push_back(std::move(*F));
        if (!Next)
          break;
        const unsigned Delta = Next * Format.Stride;
        if (Delta > Seg.page_size - At)
          return fail();
        At += Delta;
      }
    }
  }
  detail::publishArm64eFixups(Pending, Img);
  return true;
}

} // namespace

void parseChainedFixupsImports(const uint8_t *BasePtr, size_t FileSize,
                               const ChainedFixupsInfo &Info,
                               BinaryImage &Img) {
  auto Records = decodeChainedImports(BasePtr, FileSize, Info, Img);
  if (!Records)
    return;

  std::set<std::string> SeenNames;
  for (const auto &Imp : Img.Imports)
    SeenNames.insert(Imp.Name);

  for (const Arm64eImportRecord &Record : *Records) {
    if (!Record.Valid || !SeenNames.insert(Record.Name).second)
      continue;

    Import Imp;
    Imp.Name = Record.Name;
    Imp.Module = Record.DisplayModule;
    // Chained-import records identify symbols by ordinal but do not carry the
    // address of the pointer that dyld fixes up.  The indirect symbol table
    // parsed above does: join the two views so data-only imports such as
    // ___gxx_personality_v0 resolve to their concrete __got slot rather than
    // being exposed as address zero.
    for (const auto &[SlotVA, SlotName] : Img.ImportPtrSlots)
      if (SlotName == Record.Name) {
        Imp.IATAddr = SlotVA;
        break;
      }
    Img.Imports.push_back(std::move(Imp));
  }
  LLVM_DEBUG(llvm::dbgs() << "macho: parsed " << Records->size()
                          << " chained fixups imports\n");
}

void parseChainedFixupsRebases(const uint8_t *BasePtr, size_t FileSize,
                               const ChainedFixupsInfo &Info, va_t TextVMAddr,
                               BinaryImage &Img) {
  const auto malformedArm64e = [&]() {
    if (Img.isArm64e())
      Img.MachOChainedFixupsError = arm64e::kMalformedChainedFixups;
  };
  uint32_t DataOff = Info.DataOff;
  uint32_t DataSize = Info.DataSize;
  if (!DataOff && !DataSize && !Img.MachOHasChainedFixups)
    return;
  if (!BasePtr || DataOff == 0 ||
      DataSize < sizeof(dyld_chained_fixups_header) ||
      !rangeInBounds(DataOff, DataSize, FileSize) ||
      (Img.isArm64e() && Img.MachOChainedFixupsAmbiguous)) {
    malformedArm64e();
    return;
  }
  // The chained-fixups blob is self-contained within [DataOff, DataEnd); every
  // sub-structure offset below is validated against this bound before deref.
  const uint64_t DataEnd = static_cast<uint64_t>(DataOff) + DataSize;

  dyld_chained_fixups_header Header{};
  std::memcpy(&Header, BasePtr + DataOff, sizeof(Header));
  const auto *Hdr = &Header;
  if (Hdr->starts_offset == 0 || Hdr->fixups_version != 0) {
    malformedArm64e();
    return;
  }
  uint64_t StartsAbs = static_cast<uint64_t>(DataOff) + Hdr->starts_offset;
  if (!rangeInBounds(StartsAbs, sizeof(uint32_t), DataEnd)) {
    malformedArm64e();
    return;
  }
  uint32_t SegCount = readLE<uint32_t>(BasePtr + StartsAbs);
  if (StartsAbs + sizeof(uint32_t) +
          static_cast<uint64_t>(SegCount) * sizeof(uint32_t) >
      DataEnd) {
    malformedArm64e();
    return;
  }

  auto ImportRecords = decodeChainedImports(BasePtr, FileSize, Info, Img);

  size_t NumRecorded = 0;
  for (uint32_t S = 0; S < SegCount; ++S) {
    uint32_t SegInfoOff = readLE<uint32_t>(
        BasePtr + StartsAbs + sizeof(uint32_t) * (uint64_t(S) + 1));
    if (SegInfoOff == 0)
      continue;
    uint64_t SegAbs = StartsAbs + SegInfoOff;
    constexpr size_t SegmentHeaderSize =
        offsetof(dyld_chained_starts_in_segment, page_start);
    if (!rangeInBounds(SegAbs, SegmentHeaderSize, DataEnd)) {
      malformedArm64e();
      continue;
    }
    dyld_chained_starts_in_segment SegmentHeader{};
    std::memcpy(&SegmentHeader, BasePtr + SegAbs, SegmentHeaderSize);
    const auto *Seg = &SegmentHeader;
    if (Seg->size < offsetof(dyld_chained_starts_in_segment, page_start) ||
        !rangeInBounds(SegAbs, Seg->size, DataEnd)) {
      malformedArm64e();
      continue;
    }
    uint64_t SegEnd = SegAbs + Seg->size;
    uint16_t PtrFormat = Seg->pointer_format;
    if (const auto Format = arm64e::chainFormat(PtrFormat)) {
      if (!parseArm64eSegment(*Seg, *Format, BasePtr + SegAbs, TextVMAddr, S,
                              ImportRecords, Img))
        return;
      continue;
    }
    // Other layouts require their own bitfield interpretation.
    if (PtrFormat != DYLD_CHAINED_PTR_64 &&
        PtrFormat != DYLD_CHAINED_PTR_64_OFFSET) {
      if (Img.isArm64e()) {
        Img.MachOChainedFixupsError =
            std::string(arm64e::kUnsupportedChainFormatPrefix) +
            std::to_string(PtrFormat);
        return;
      }
      continue;
    }
    uint32_t PageSize = Seg->page_size ? Seg->page_size : 0x1000;
    uint16_t PageCount = Seg->page_count;
    uint64_t PageStartArr =
        SegAbs + offsetof(dyld_chained_starts_in_segment, page_start);
    if (!rangeInBounds(PageStartArr,
                       static_cast<uint64_t>(PageCount) * sizeof(uint16_t),
                       SegEnd))
      continue;
    uint64_t SegVMOff = Seg->segment_offset;
    for (uint16_t P = 0; P < PageCount; ++P) {
      uint16_t Start = readLE<uint16_t>(BasePtr + PageStartArr +
                                        uint64_t(P) * sizeof(uint16_t));
      if (Start == DYLD_CHAINED_PTR_START_NONE)
        continue;
      // Pages with multiple chain starts (overlapping fixups) are rare for the
      // __DATA tables of interest; skip rather than mis-walk the overflow list.
      if (Start & DYLD_CHAINED_PTR_START_MULTI)
        continue;
      uint64_t PageOffset = static_cast<uint64_t>(P) * PageSize + Start;
      if (PageOffset > InvalidVA - SegVMOff)
        continue;
      uint64_t ChainOffset = SegVMOff + PageOffset;
      if (ChainOffset > InvalidVA - TextVMAddr)
        continue;
      va_t ChainVA = TextVMAddr + ChainOffset;
      // Bounded chain walk; a malformed `next` cannot loop forever.
      for (uint32_t Guard = 0;
           Guard < ::neverd::limits::loader::
                       kMachOChainedFixupsParseChainedFixupsRebasesGuardLimit;
           ++Guard) {
        const uint8_t *Loc = Img.readVA(ChainVA, sizeof(uint64_t));
        if (!Loc)
          break;
        uint64_t Raw;
        std::memcpy(&Raw, Loc, sizeof(uint64_t));
        uint32_t Next;
        if ((Raw >> 63) & 1) {
          dyld_chained_ptr_64_bind B;
          std::memcpy(&B, &Raw, sizeof(B));
          Next = B.next;
          if (ImportRecords && B.ordinal < ImportRecords->size()) {
            const Arm64eImportRecord &Record = (*ImportRecords)[B.ordinal];
            int64_t EffectiveAddend = 0;
            const int64_t PointerAddend = static_cast<int64_t>(B.addend);
            if (Record.Valid && !llvm::AddOverflow(Record.Addend, PointerAddend,
                                                   EffectiveAddend)) {
              if (Img.isValidImportStorageSlot(ChainVA, Record.Name)) {
                detail::clearLocalPointerClassification(Img, ChainVA);
                if (Img.recordDyldBindSlot(
                        ChainVA, Record.Name, EffectiveAddend, Record.Module,
                        Record.WeakImport, Record.LibraryOrdinal)) {
                  detail::joinImportSlot(Img, Record.Name, Record.DisplayModule,
                                         ChainVA);
                  ++NumRecorded;
                }
              }
            }
          }
        } else {
          dyld_chained_ptr_64_rebase R;
          std::memcpy(&R, &Raw, sizeof(R));
          Next = R.next;
          // Both generic64 formats carry high8. dyld adds the load address
          // to the fully unpacked word for OFFSET, including those high bits.
          // Keep that runtime value intact; ordinary pointer provenance must
          // classify the complete address rather than strip an embedded tag.
          va_t TargetVA = (static_cast<uint64_t>(R.high8) << 56) | R.target;
          if (PtrFormat == DYLD_CHAINED_PTR_64_OFFSET) {
            if (TargetVA > InvalidVA - TextVMAddr)
              break;
            TargetVA += TextVMAddr;
          }
          if (detail::recordAbsolutePointerSlot(Img, ChainVA, TargetVA))
            ++NumRecorded;
          // Apply the rebase in-place: the on-disk slot holds the *encoded*
          // chained-pointer bitfield (target/next/bind), but every consumer of
          // the loaded image (jump-table resolver reading absolute table
          // entries, pointer-table symbolization, data reads) expects the
          // resolved preferred-base VA — exactly what dyld writes at load time
          // and what the ELF loader does when it applies relocations.  Without
          // this the table entries read back as garbage (`target | next<<51`).
          Img.patchPtr(ChainVA, static_cast<uint64_t>(TargetVA));
          Img.MachOResolvedChainedPointerSlots.insert(ChainVA);
        }
        if (Next == 0)
          break;
        // For 64-bit chained pointers, `next` counts 4-byte strides.
        va_t Delta = static_cast<va_t>(Next) * 4;
        if (Delta > InvalidVA - ChainVA)
          break;
        ChainVA += Delta;
      }
    }
  }
  LLVM_DEBUG(llvm::dbgs() << "macho: parsed chained-fixup rebases, recorded "
                          << NumRecorded << " pointer slots\n");
  (void)NumRecorded;
}

} // namespace macho_loader
} // namespace neverd
