//===- MachOChainedFixups.cpp - Mach-O chained fixups --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MachODyldFixups.h"

#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <vector>

#define DEBUG_TYPE "neverd-macho-loader"

namespace neverd {
namespace macho_loader {

using namespace llvm::MachO;

namespace {

struct ChainedImportRecord {
  std::string Name;
  std::string Module;
  int64_t Addend = 0;
  bool Valid = false;
};

/// Decode the import table without deduplicating it: chained pointer records
/// refer to this table by ordinal, so even two equal names must retain distinct
/// indices and addends.  A malformed entry remains an invalid element rather
/// than shifting every ordinal after it.
std::optional<std::vector<ChainedImportRecord>>
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
    return std::vector<ChainedImportRecord>{};
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

  std::vector<ChainedImportRecord> Records(Hdr.imports_count);

  for (uint32_t I = 0; I < Hdr.imports_count; ++I) {
    uint32_t NameOff = 0;
    int32_t LibOrdinal = 0;
    int64_t Addend = 0;
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
      LibOrdinal = static_cast<int8_t>(E.lib_ordinal);
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
      LibOrdinal = static_cast<int8_t>(E.lib_ordinal);
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
      LibOrdinal = static_cast<int16_t>(E.lib_ordinal);
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
        static_cast<size_t>(LibOrdinal) <= Img.DynInfo.NeededLibs.size())
      DylibName = Img.DynInfo.NeededLibs[static_cast<size_t>(LibOrdinal - 1)];
    Records[I].Name = std::move(SymName);
    Records[I].Module =
        DylibName.empty() ? kExternModule.str() : std::move(DylibName);
    Records[I].Addend = Addend;
    Records[I].Valid = true;
  }
  return Records;
}

void joinImportSlot(BinaryImage &Img, llvm::StringRef Name,
                    llvm::StringRef Module, va_t SlotVA) {
  for (Import &Imp : Img.Imports)
    if (Imp.Name == Name) {
      if (Imp.Module.empty() && !Module.empty())
        Imp.Module = Module.str();
      if (Imp.IATAddr == 0)
        Imp.IATAddr = SlotVA;
      return;
    }
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

  for (const ChainedImportRecord &Record : *Records) {
    if (!Record.Valid || !SeenNames.insert(Record.Name).second)
      continue;

    Import Imp;
    Imp.Name = Record.Name;
    Imp.Module = Record.Module;
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
  if (!BasePtr)
    return;
  uint32_t DataOff = Info.DataOff;
  uint32_t DataSize = Info.DataSize;
  if (DataOff == 0 || DataSize < sizeof(dyld_chained_fixups_header) ||
      !rangeInBounds(DataOff, DataSize, FileSize))
    return;
  // The chained-fixups blob is self-contained within [DataOff, DataEnd); every
  // sub-structure offset below is validated against this bound before deref.
  const uint64_t DataEnd = static_cast<uint64_t>(DataOff) + DataSize;

  const auto *Hdr =
      reinterpret_cast<const dyld_chained_fixups_header *>(BasePtr + DataOff);
  if (Hdr->starts_offset == 0)
    return;
  uint64_t StartsAbs = static_cast<uint64_t>(DataOff) + Hdr->starts_offset;
  if (!rangeInBounds(StartsAbs, sizeof(uint32_t), DataEnd))
    return;
  const auto *Starts = reinterpret_cast<const dyld_chained_starts_in_image *>(
      BasePtr + StartsAbs);
  uint32_t SegCount = Starts->seg_count;
  if (StartsAbs + sizeof(uint32_t) +
          static_cast<uint64_t>(SegCount) * sizeof(uint32_t) >
      DataEnd)
    return;

  auto ImportRecords = decodeChainedImports(BasePtr, FileSize, Info, Img);

  size_t NumRecorded = 0;
  for (uint32_t S = 0; S < SegCount; ++S) {
    uint32_t SegInfoOff = Starts->seg_info_offset[S];
    if (SegInfoOff == 0)
      continue;
    uint64_t SegAbs = StartsAbs + SegInfoOff;
    if (!rangeInBounds(SegAbs, sizeof(dyld_chained_starts_in_segment), DataEnd))
      continue;
    const auto *Seg = reinterpret_cast<const dyld_chained_starts_in_segment *>(
        BasePtr + SegAbs);
    if (Seg->size < offsetof(dyld_chained_starts_in_segment, page_start) ||
        !rangeInBounds(SegAbs, Seg->size, DataEnd))
      continue;
    uint64_t SegEnd = SegAbs + Seg->size;
    uint16_t PtrFormat = Seg->pointer_format;
    // Only the 64-bit pointer formats carry the rebase/bind bitfields decoded
    // below; arm64e (authenticated) and the 32-bit formats are skipped (they
    // never appear for the plain data-pointer tables we care about here).
    if (PtrFormat != DYLD_CHAINED_PTR_64 &&
        PtrFormat != DYLD_CHAINED_PTR_64_OFFSET)
      continue;
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
      uint16_t Start = Seg->page_start[P];
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
      for (uint32_t Guard = 0; Guard < (1u << 22); ++Guard) {
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
            const ChainedImportRecord &Record = (*ImportRecords)[B.ordinal];
            int64_t EffectiveAddend = 0;
            const int64_t PointerAddend =
                static_cast<int64_t>(static_cast<int8_t>(B.addend));
            if (Record.Valid && !llvm::AddOverflow(Record.Addend, PointerAddend,
                                                   EffectiveAddend)) {
              detail::clearLocalPointerClassification(Img, ChainVA);
              Img.DyldBindSlots[ChainVA] =
                  ImportBindSlot{Record.Name, EffectiveAddend};
              joinImportSlot(Img, Record.Name, Record.Module, ChainVA);
              ++NumRecorded;
            }
          }
        } else {
          dyld_chained_ptr_64_rebase R;
          std::memcpy(&R, &Raw, sizeof(R));
          Next = R.next;
          va_t TargetVA;
          if (PtrFormat == DYLD_CHAINED_PTR_64_OFFSET) {
            if (R.target > InvalidVA - TextVMAddr)
              break;
            TargetVA = TextVMAddr + R.target;
          } else {
            TargetVA = (static_cast<uint64_t>(R.high8) << 56) | R.target;
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
