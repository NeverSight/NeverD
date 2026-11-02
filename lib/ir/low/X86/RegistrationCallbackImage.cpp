//===- RegistrationCallbackImage.cpp - SEH callback image storage --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Proves mapped PE32 data ownership without inventing runtime contents.

#include "RegistrationABIPrivate.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include <utility>

namespace neverd::registration_abi {

std::optional<std::vector<uint8_t>>
readImmutableRegistrationImageBytes(const BinaryImage &Image, va_t Address,
                                    uint16_t Width, size_t &Work) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Image.IsRelocatable || !Width ||
      Address > UINT32_MAX - (Width - 1))
    return std::nullopt;
  if (!chargeCalleeWork(Work, Width))
    return std::nullopt;
  for (const size_t Count : {Image.Sections.size(),
                             Image.Segments.size(),
                             Image.CodePtrRelocSlots.size(),
                             Image.DataPtrRelocSlots.size(),
                             Image.DataPtrRelocTargetOwners.size(),
                             Image.RelCodeRelocSlots.size(),
                             Image.RelDataPtrRelocSlots.size(),
                             Image.MachOResolvedChainedPointerSlots.size(),
                             Image.ConflictingImportStorageSlots.size(),
                             Image.ImportPtrSlots.size(),
                             Image.ImportStorageSlots.size(),
                             Image.DyldBindSlots.size(),
                             Image.ObjCSourceReferences.size(),
                             Image.DataAddressRelocOperands.size(),
                             Image.CodeAddressRelocOperands.size(),
                             Image.Relocations.size(),
                             Image.BaseRelocations.size(),
                             Image.RuntimeCallablePointerSlots.size(),
                             Image.RelocDataAddrs.size(),
                             Image.WritableRelocDataAddrs.size(),
                             Image.RodataAnchorSeg.size(),
                             Image.CodeRefTargets.size()})
    if (!chargeCalleeWork(Work, Count + 1))
      return std::nullopt;
  // Account for the mapping owner's nested section and segment lookups.
  for (unsigned I = 0; I != 8; ++I)
    if (!chargeCalleeWork(Work, Image.Sections.size()) ||
        !chargeCalleeWork(Work, Image.Segments.size()))
      return std::nullopt;
  return readImmutableImageBytes(Image, Address, Width);
}

std::optional<ImmutableRegistrationScalar>
readImmutableRegistrationScalar(const BinaryImage &Image, va_t Address,
                                uint16_t Width, size_t &Work) {
  if (Width > sizeof(uint32_t) || !chargeCalleeWork(Work, 1))
    return std::nullopt;
  // A runtime cookie remains opaque even in read-only source storage. Share
  // this exclusion with ordinary bound helpers as well as layout-scoped
  // proofs; neither narrow reads nor an initial zero may freeze its value.
  const auto RVA = Image.DynInfo.SecurityCookieRVA;
  constexpr va_t CookieBytes = sizeof(uint32_t);
  if (RVA && Image.Base <= UINT32_MAX - (CookieBytes - 1) &&
      RVA <= UINT32_MAX - (CookieBytes - 1) - Image.Base) {
    const va_t Cookie = Image.Base + RVA;
    if (Width && (Address <= Cookie ? Cookie - Address < Width
                                    : Address - Cookie < CookieBytes))
      return std::nullopt;
  }
  auto Bytes = readImmutableRegistrationImageBytes(Image, Address, Width, Work);
  if (!Bytes)
    return std::nullopt;
  // Potential one-past addresses inspect each segment's two extents and
  // their data owners, including nested segment/section lookup.
  for (size_t I = 0; I != Image.Segments.size(); ++I)
    for (unsigned Extent = 0; Extent != 2; ++Extent)
      if (!chargeCalleeWork(Work, Image.Sections.size()) ||
          !chargeCalleeWork(Work, Image.Segments.size()) ||
          !chargeCalleeWork(Work, Image.Segments.size()))
        return std::nullopt;
  uint32_t Value = 0;
  for (unsigned I = 0; I != Width; ++I)
    Value |= uint32_t((*Bytes)[I]) << (I * 8);
  ImmutableRegistrationScalar Read{std::move(*Bytes), std::nullopt};
  if (Width != 4 || !Image.isPotentiallyRelocatableAddress(Value))
    Read.Value = Value;
  return Read;
}

bool hasSEHCallbackImageMemory(const BinaryImage &Image, va_t Address,
                               uint16_t Width, bool Write, size_t &Work) {
  if (Image.Format != BinaryFormat::COFF || Image.IsRelocatable ||
      Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 || !Width ||
      !Address || Address > UINT32_MAX ||
      uint64_t(Width) > uint64_t(UINT32_MAX) + 1 - Address ||
      !chargeCalleeWork(Work, Image.Imports.size() + Image.Sections.size() +
                                  Image.Segments.size() + Width + 1))
    return false;
  const auto *Section = Image.getSectionFor(Address);
  const auto *Segment = Image.getSegmentFor(Address);
  const auto End = Image.mappedObjectOwnerEnd(Address);
  if (!Section || !Segment || !End || *End < Address ||
      Width > *End - Address || !Section->isReadable() ||
      !Segment->isReadable() || Section->isExecutable() ||
      Segment->isExecutable() || Section->Size > InvalidVA - Section->VA ||
      Segment->Size > InvalidVA - Segment->VA || Section->VA < Segment->VA ||
      !rangeInBounds(Address - Segment->VA, Width, Segment->Size) ||
      (Write && (!Section->isWritable() || !Segment->isWritable() ||
                 Segment->ReadOnlyAfterRelocations)))
    return false;
  const auto Overlaps = [&](va_t Base, uint64_t Size) {
    return Size &&
           (Base <= Address ? Address - Base < Size : Base - Address < Width);
  };
  for (const auto &Other : Image.Sections)
    if (&Other != Section && Overlaps(Other.VA, Other.Size))
      return false;
  for (const auto &Other : Image.Segments)
    if (&Other != Segment && Overlaps(Other.VA, Other.Size))
      return false;
  for (const auto &Import : Image.Imports)
    if (Overlaps(Import.IATAddr, 4))
      return !Write && Address == Import.IATAddr && Width == 4 &&
             isFileBackedPEImportSlot(Image, Address);
  // The loader may materialize PE zero-fill storage. Its runtime address is
  // still disjoint from a private stack; neither initial nor future values
  // are copied into the frame domain by this permission check.
  return true;
}

} // namespace neverd::registration_abi
