#include "neverd/loader/ReadOnlyBytes.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Endian.h"

namespace neverd {
namespace {
bool supportedImage(const BinaryImage &Image) {
  return Image.Format == BinaryFormat::MachO && !Image.IsRelocatable &&
         Image.Bits == Bitness::Bits64 && !Image.MachOChainedFixupsAmbiguous &&
         (Image.Arch == Arch::AArch64 || Image.Arch == Arch::X64);
}

bool overlaps(va_t Address, uint64_t Extent, va_t Base, uint64_t Width) {
  return Width && Extent &&
         (Base <= Address ? Address - Base < Width : Base - Address < Extent);
}

const uint8_t *mappedBytes(const BinaryImage &Image, va_t Address,
                           uint64_t Extent, bool Immutable) {
  if (!Address || Address > InvalidVA - Extent)
    return nullptr;
  const auto *Section = Image.getSectionFor(Address);
  const auto *Segment = Image.getSegmentFor(Address);
  if (!Section || !Segment || !Section->isReadable() ||
      !Segment->isReadable() || Image.isCodeAddress(Address) ||
      (Immutable && !Segment->ReadOnlyAfterRelocations &&
       (Section->isWritable() || Segment->isWritable())) ||
      Section->Size > InvalidVA - Section->VA ||
      Segment->Size > InvalidVA - Segment->VA || Section->VA < Segment->VA ||
      !rangeInBounds(Address - Section->VA, Extent, Section->Size) ||
      !rangeInBounds(Address - Section->VA, Extent, Section->FileSz) ||
      !rangeInBounds(Address - Segment->VA, Extent, Segment->Size) ||
      !rangeInBounds(Address - Segment->VA, Extent, Segment->FileSz) ||
      Section->FileOff < Segment->FileOff ||
      Section->FileOff - Segment->FileOff != Section->VA - Segment->VA)
    return nullptr;
  const auto Type = Section->Type & llvm::MachO::SECTION_TYPE;
  if (Type == llvm::MachO::S_ZEROFILL || Type == llvm::MachO::S_GB_ZEROFILL ||
      Type == llvm::MachO::S_THREAD_LOCAL_ZEROFILL)
    return nullptr;
  for (const auto &Other : Image.Sections)
    if (&Other != Section && overlaps(Address, Extent, Other.VA, Other.Size))
      return nullptr;
  for (const auto &Other : Image.Segments)
    if (&Other != Segment && overlaps(Address, Extent, Other.VA, Other.Size))
      return nullptr;
  return Image.readVA(Address, Extent);
}

// Byte copies admit no fixup. Pointer reads admit only the exact normalized
// absolute data slot, with independently checked resolution and target owner.
bool hasConflictingFixups(const BinaryImage &Image, va_t Address,
                          uint64_t Extent, bool Pointer) {
  auto Touches = [&](const auto &Slots, auto Key, bool AllowExact = false) {
    auto It = Slots.lower_bound(Address >= 7 ? Address - 7 : 0);
    for (; It != Slots.end() &&
           (Key(*It) < Address || Key(*It) - Address < Extent);
         ++It)
      if (overlaps(Address, Extent, Key(*It), 8) &&
          !(AllowExact && Key(*It) == Address))
        return true;
    return false;
  };
  auto Set = [](va_t Slot) { return Slot; };
  auto Map = [](const auto &Slot) { return Slot.first; };
  if (Touches(Image.CodePtrRelocSlots, Set) ||
      Touches(Image.DataPtrRelocSlots, Set, Pointer) ||
      Touches(Image.DataPtrRelocTargetOwners, Map, Pointer) ||
      Touches(Image.RelCodeRelocSlots, Set) ||
      Touches(Image.RelDataPtrRelocSlots, Set) ||
      Touches(Image.MachOResolvedChainedPointerSlots, Set, Pointer) ||
      Touches(Image.ConflictingImportStorageSlots, Set) ||
      Touches(Image.ImportPtrSlots, Map) ||
      Touches(Image.ImportStorageSlots, Map) ||
      Touches(Image.DyldBindSlots, Map) ||
      Touches(Image.ObjCSourceReferences, Map) ||
      Touches(Image.DataAddressRelocOperands, Map) ||
      Touches(Image.CodeAddressRelocOperands, Map))
    return true;
  for (const auto &Relocation : Image.Relocations)
    if (overlaps(Address, Extent, Relocation.Address, 8))
      return true;
  for (const auto &Relocation : Image.BaseRelocations)
    if (overlaps(Address, Extent, Relocation.Address, 8))
      return true;
  for (const auto &Slot : Image.RuntimeCallablePointerSlots)
    if (overlaps(Address, Extent, Slot.SlotVA, 8))
      return true;
  return false;
}
} // namespace

std::optional<std::vector<uint8_t>>
readImmutableImageBytes(const BinaryImage &Image, va_t Address, uint32_t Size) {
  if (!supportedImage(Image) || Size > 1024 * 1024)
    return std::nullopt;
  // A zero-length borrow still requires a valid nonnull object address.
  const uint64_t Extent = Size ? Size : 1;
  const auto *Bytes = mappedBytes(Image, Address, Extent, true);
  if (!Bytes || hasConflictingFixups(Image, Address, Extent, false))
    return std::nullopt;
  return std::vector<uint8_t>(Bytes, Bytes + Size);
}

bool isImagePointerBitPattern(const BinaryImage &Image, uint64_t Bits,
                              uint16_t Width) {
  return Bits && Width == (Image.is64Bit() ? 8 : 4) &&
         Image.getSectionFor(Bits);
}

bool isFileBackedWritableImageRange(const BinaryImage &Image, va_t Address,
                                    uint32_t Size) {
  if (!supportedImage(Image) || !Size || Size > 1024 * 1024 ||
      !mappedBytes(Image, Address, Size, false))
    return false;
  const auto *Section = Image.getSectionFor(Address);
  const auto *Segment = Image.getSegmentFor(Address);
  return Section->isWritable() && !Section->isExecutable() &&
         Segment->isWritable() && !Segment->isExecutable() &&
         !Segment->ReadOnlyAfterRelocations;
}

namespace {
std::optional<va_t> readResolvedPointer(const BinaryImage &Image, va_t Address,
                                        bool Immutable) {
  if (!supportedImage(Image) || !Image.DataPtrRelocSlots.count(Address) ||
      (Image.MachOHasChainedFixups &&
       !Image.MachOResolvedChainedPointerSlots.count(Address)))
    return std::nullopt;
  const auto Owner = Image.DataPtrRelocTargetOwners.find(Address);
  const auto *Bytes = mappedBytes(Image, Address, 8, Immutable);
  if (!Bytes || Owner == Image.DataPtrRelocTargetOwners.end() ||
      hasConflictingFixups(Image, Address, 8, true))
    return std::nullopt;
  const auto Target = llvm::support::endian::read64le(Bytes);
  if (!mappedBytes(Image, Target, 1, false) ||
      Image.getSectionFor(Target)->VA != Owner->second)
    return std::nullopt;
  return Target;
}
} // namespace

std::optional<va_t> readImmutableImagePointer(const BinaryImage &Image,
                                              va_t Address) {
  return readResolvedPointer(Image, Address, true);
}

std::optional<va_t> readInitialImagePointer(const BinaryImage &Image,
                                            va_t Address) {
  return readResolvedPointer(Image, Address, false);
}
} // namespace neverd
