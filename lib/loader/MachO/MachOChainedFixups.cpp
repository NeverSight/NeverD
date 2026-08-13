//===- MachOChainedFixups.cpp - Mach-O chained fixups --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/MachOLoaderUtils.h"

#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <cstring>
#include <set>
#include <string>

#define DEBUG_TYPE "neverd-macho-loader"

namespace neverd {
namespace macho_loader {

using namespace llvm::MachO;

void parseChainedFixupsImports(const uint8_t *BasePtr, size_t FileSize,
                               const ChainedFixupsInfo &Info,
                               BinaryImage &Img) {
  uint32_t DataOff = Info.DataOff;
  uint32_t DataSize = Info.DataSize;
  if (DataOff == 0 || DataSize < sizeof(dyld_chained_fixups_header) ||
      !rangeInBounds(DataOff, DataSize, FileSize))
    return;

  const auto *Hdr =
      reinterpret_cast<const dyld_chained_fixups_header *>(BasePtr + DataOff);
  if (Hdr->imports_count == 0 || Hdr->imports_offset == 0 ||
      Hdr->symbols_offset == 0)
    return;

  // imports_offset/symbols_offset are untrusted: validate them against DataSize
  // in 64-bit so the derived absolute offsets cannot wrap and StrSize (the gap
  // to the blob end) cannot underflow into a huge length.
  if (Hdr->imports_offset >= DataSize || Hdr->symbols_offset >= DataSize)
    return;
  uint64_t ImportsAbs = static_cast<uint64_t>(DataOff) + Hdr->imports_offset;
  uint64_t SymbolsAbs = static_cast<uint64_t>(DataOff) + Hdr->symbols_offset;
  uint64_t SymbolsEnd = static_cast<uint64_t>(DataOff) + DataSize;

  if (ImportsAbs >= FileSize || SymbolsAbs >= FileSize)
    return;

  const char *StrTab = reinterpret_cast<const char *>(BasePtr + SymbolsAbs);
  size_t StrSize = static_cast<size_t>(SymbolsEnd - SymbolsAbs);

  std::set<std::string> SeenNames;
  for (const auto &Imp : Img.Imports)
    SeenNames.insert(Imp.Name);

  for (uint32_t I = 0; I < Hdr->imports_count; ++I) {
    uint32_t NameOff = 0;
    int32_t LibOrdinal = 0;

    switch (Hdr->imports_format) {
    case DYLD_CHAINED_IMPORT: {
      uint64_t EntOff =
          ImportsAbs + static_cast<uint64_t>(I) * sizeof(dyld_chained_import);
      if (!rangeInBounds(EntOff, sizeof(dyld_chained_import), SymbolsEnd))
        return;
      dyld_chained_import E;
      std::memcpy(&E, BasePtr + static_cast<size_t>(EntOff), sizeof(E));
      NameOff = E.name_offset;
      LibOrdinal = static_cast<int8_t>(E.lib_ordinal);
      break;
    }
    case DYLD_CHAINED_IMPORT_ADDEND: {
      uint64_t EntOff = ImportsAbs + static_cast<uint64_t>(I) *
                                         sizeof(dyld_chained_import_addend);
      if (!rangeInBounds(EntOff, sizeof(dyld_chained_import_addend),
                         SymbolsEnd))
        return;
      dyld_chained_import_addend E;
      std::memcpy(&E, BasePtr + static_cast<size_t>(EntOff), sizeof(E));
      NameOff = E.name_offset;
      LibOrdinal = static_cast<int8_t>(E.lib_ordinal);
      break;
    }
    case DYLD_CHAINED_IMPORT_ADDEND64: {
      uint64_t EntOff = ImportsAbs + static_cast<uint64_t>(I) *
                                         sizeof(dyld_chained_import_addend64);
      if (!rangeInBounds(EntOff, sizeof(dyld_chained_import_addend64),
                         SymbolsEnd))
        return;
      dyld_chained_import_addend64 E;
      std::memcpy(&E, BasePtr + static_cast<size_t>(EntOff), sizeof(E));
      NameOff = E.name_offset;
      LibOrdinal = static_cast<int16_t>(E.lib_ordinal);
      break;
    }
    default:
      return;
    }

    if (NameOff >= StrSize)
      continue;
    std::string SymName = readFixedName(StrTab + NameOff, StrSize - NameOff);
    if (SymName.empty() || !SeenNames.insert(SymName).second)
      continue;

    std::string DylibName;
    if (LibOrdinal > 0 &&
        static_cast<size_t>(LibOrdinal) <= Img.DynInfo.NeededLibs.size())
      DylibName = Img.DynInfo.NeededLibs[static_cast<size_t>(LibOrdinal - 1)];

    Import Imp;
    Imp.Name = SymName;
    Imp.Module = DylibName.empty() ? kExternModule.str() : DylibName;
    // Chained-import records identify symbols by ordinal but do not carry the
    // address of the pointer that dyld fixes up.  The indirect symbol table
    // parsed above does: join the two views so data-only imports such as
    // ___gxx_personality_v0 resolve to their concrete __got slot rather than
    // being exposed as address zero.
    for (const auto &[SlotVA, SlotName] : Img.ImportPtrSlots)
      if (SlotName == SymName) {
        Imp.IATAddr = SlotVA;
        break;
      }
    Img.Imports.push_back(std::move(Imp));
  }
  LLVM_DEBUG(llvm::dbgs() << "macho: parsed " << Hdr->imports_count
                          << " chained fixups imports\n");
}

void parseChainedFixupsRebases(const uint8_t *BasePtr, size_t FileSize,
                               const ChainedFixupsInfo &Info, va_t TextVMAddr,
                               BinaryImage &Img) {
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

  // A rebase slot at \p SlotVA points at \p TargetVA; classify the target
  // segment exactly as the ELF loader does (executable → code-pointer table
  // entry; read-only non-exec data → data-pointer table entry).
  auto recordSlot = [&](va_t SlotVA, va_t TargetVA) {
    const Segment *TSeg = Img.getSegmentFor(TargetVA);
    if (!TSeg)
      return;
    if (TSeg->isExecutable())
      Img.CodePtrRelocSlots.insert(SlotVA);
    else if (TSeg->isReadable() && !TSeg->Data.empty() &&
             (!TSeg->isWritable() ||
              TSeg->Name == section_names::macho::DataConstSeg))
      Img.DataPtrRelocSlots.insert(SlotVA);
  };

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
          // bind: an imported symbol, not a rebase — only carries the chain
          // link.
          dyld_chained_ptr_64_bind B;
          std::memcpy(&B, &Raw, sizeof(B));
          Next = B.next;
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
          size_t Before =
              Img.CodePtrRelocSlots.size() + Img.DataPtrRelocSlots.size();
          recordSlot(ChainVA, TargetVA);
          NumRecorded +=
              (Img.CodePtrRelocSlots.size() + Img.DataPtrRelocSlots.size()) -
              Before;
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
                          << NumRecorded << " code/data pointer slots\n");
  (void)NumRecorded;
}

} // namespace macho_loader
} // namespace neverd
