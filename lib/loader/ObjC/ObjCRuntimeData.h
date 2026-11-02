//===- ObjCRuntimeData.h - Bounded Objective-C runtime records --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Separate analysis-only metadata traversal from plain runtime value proofs.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_OBJC_OBJCRUNTIMEDATA_H
#define NEVERD_LOADER_OBJC_OBJCRUNTIMEDATA_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/MachO/MachOMetadataPointer.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"

#include <optional>
#include <string>

namespace neverd::objc {

enum class PointerView { PlainRuntimeValue, MetadataTarget };

/// A bounded view of file-backed runtime records after Mach-O fixups.
class RuntimeData {
  const BinaryImage &Image;
  PointerView View;

public:
  explicit RuntimeData(const BinaryImage &Image,
                       PointerView View = PointerView::PlainRuntimeValue)
      : Image(Image), View(View) {}

  bool supportsPlainObjectPointers() const {
    if (View != PointerView::PlainRuntimeValue ||
        Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
        Image.Bits != Bitness::Bits64 ||
        (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
        Image.MachOChainedFixupsAmbiguous)
      return false;
    // Plain C object storage cannot reproduce authenticated arm64e isa fields.
    if (Image.MachOSharedCache || Image.isArm64e() ||
        !Image.MachOAuthenticatedPointers.empty())
      return false;
    return Image.Arch != Arch::AArch64 || Image.Raw.size() < 32 ||
           llvm::support::endian::read32le(Image.Raw.data()) !=
               llvm::MachO::MH_MAGIC_64 ||
           !macho_loader::arm64e::isSubtype(
               llvm::support::endian::read32le(Image.Raw.data() + 8));
  }

  const uint8_t *bytes(va_t VA, uint64_t Size) const {
    if (View == PointerView::MetadataTarget && Image.MachOSharedCache)
      return macho_loader::readMetadataBytes(Image, VA, Size);
    const auto *Section = Image.getSectionFor(VA);
    const auto *Segment = Image.getSegmentFor(VA);
    if (!Section || !Segment || !Section->isReadable() ||
        !Segment->isReadable() || Size > InvalidVA - VA ||
        Section->Size > InvalidVA - Section->VA ||
        Segment->Size > InvalidVA - Segment->VA || Section->VA < Segment->VA ||
        !rangeInBounds(VA - Section->VA, Size, Section->Size) ||
        !rangeInBounds(VA - Segment->VA, Size, Segment->Size) ||
        !rangeInBounds(VA - Section->VA, Size, Section->FileSz) ||
        !rangeInBounds(VA - Segment->VA, Size, Segment->FileSz) ||
        Section->FileOff < Segment->FileOff ||
        Section->FileOff - Segment->FileOff != Section->VA - Segment->VA)
      return nullptr;
    const auto Type = Section->Type & llvm::MachO::SECTION_TYPE;
    if (Type == llvm::MachO::S_ZEROFILL || Type == llvm::MachO::S_GB_ZEROFILL ||
        Type == llvm::MachO::S_THREAD_LOCAL_ZEROFILL)
      return nullptr;
    return Image.readVA(VA, Size);
  }

  /// Identify a runtime-written offset slot without inventing its value.
  /// This is storage ownership evidence, never a readable byte snapshot.
  bool runtimeOffsetSlot(va_t VA) const {
    constexpr uint64_t Width = 8;
    if (!VA || VA % Width || Width > InvalidVA - VA ||
        !supportsPlainObjectPointers())
      return false;
    const auto *Section = Image.getSectionFor(VA);
    const auto *Segment = Image.getSegmentFor(VA);
    if (!Section || !Segment || !Section->isReadable() ||
        !Section->isWritable() || Section->isExecutable() ||
        !Segment->isReadable() || !Segment->isWritable() ||
        Segment->isExecutable() || Section->FileSz ||
        (Section->Type & llvm::MachO::SECTION_TYPE) !=
            llvm::MachO::S_ZEROFILL ||
        Section->VA < Segment->VA || Section->Size > InvalidVA - Section->VA ||
        Segment->Size > InvalidVA - Segment->VA ||
        !rangeInBounds(Section->VA - Segment->VA, Section->Size,
                       Segment->Size) ||
        !rangeInBounds(VA - Section->VA, Width, Section->Size) ||
        !rangeInBounds(VA - Segment->VA, Width, Segment->Size))
      return false;
    auto Overlaps = [&](va_t Start, uint64_t Size) {
      return Size && (Start <= VA ? VA - Start < Size : Start - VA < Width);
    };
    for (const auto &Other : Image.Sections)
      if (&Other != Section && Overlaps(Other.VA, Other.Size))
        return false;
    for (const auto &Other : Image.Segments)
      if (&Other != Segment && Overlaps(Other.VA, Other.Size))
        return false;
    for (va_t Slot = VA >= 7 ? VA - 7 : 0; Slot < VA + Width; ++Slot)
      if (Image.hasPointerStorageProvenanceAt(Slot) ||
          Image.MachOResolvedChainedPointerSlots.count(Slot) ||
          Image.ImportPtrSlots.count(Slot) ||
          Image.ImportStorageSlots.count(Slot) ||
          Image.DyldBindSlots.count(Slot))
        return false;
    return true;
  }

  std::optional<uint32_t> u32(va_t VA) const {
    const auto *Data = bytes(VA, 4);
    return Data ? std::optional(llvm::support::endian::read32le(Data))
                : std::nullopt;
  }

  std::optional<va_t> pointer(va_t VA) const {
    const auto *Data = bytes(VA, 8);
    if (!Data)
      return std::nullopt;
    if (View == PointerView::MetadataTarget)
      return macho_loader::readMetadataPointerTarget(Image, VA);
    if (Image.MachOChainedFixupsAmbiguous ||
        Image.hasMachOAuthenticatedPointerIn(VA, 8))
      return std::nullopt;
    const auto Value = llvm::support::endian::read64le(Data);
    if (Value && Image.MachOHasChainedFixups &&
        !Image.MachOResolvedChainedPointerSlots.count(VA))
      return std::nullopt;
    return Value;
  }

  /// An imported slot never denotes a local runtime record, even when its
  /// file bytes happen to resemble a resolved address or a null pointer.
  std::optional<va_t> localPointer(va_t VA) const {
    if (Image.ImportStorageSlots.count(VA) || Image.DyldBindSlots.count(VA) ||
        Image.ImportPtrSlots.count(VA) ||
        Image.ConflictingImportStorageSlots.count(VA))
      return std::nullopt;
    return pointer(VA);
  }

  std::optional<std::string> string(va_t VA, bool AllowEmpty = false) const {
    std::string Value;
    for (size_t Index = 0;
         Index < ::neverd::limits::loader::kObjCRuntimeDataStringIndexLimit &&
         VA <= InvalidVA - Index;
         ++Index) {
      const auto *Data = bytes(VA + Index, 1);
      if (!Data)
        return std::nullopt;
      if (!*Data)
        return Value.empty() && !AllowEmpty ? std::nullopt
                                            : std::optional(Value);
      if (*Data < 0x20 || *Data > 0x7e)
        return std::nullopt;
      Value.push_back(static_cast<char>(*Data));
    }
    return std::nullopt;
  }

  std::optional<va_t> classRO(va_t Address) const {
    if (!bytes(Address, 40))
      return std::nullopt;
    auto Bits = pointer(Address + 32);
    if (!Bits || !*Bits)
      return std::nullopt;
    auto RO = *Bits & ~uint64_t(7);
    return bytes(RO, 72) ? std::optional(RO) : std::nullopt;
  }

  std::optional<std::string> className(va_t Address) const {
    auto RO = classRO(Address);
    auto Name = RO ? pointer(*RO + 24) : std::nullopt;
    return Name ? string(*Name) : std::nullopt;
  }
};

} // namespace neverd::objc

#endif // NEVERD_LOADER_OBJC_OBJCRUNTIMEDATA_H
