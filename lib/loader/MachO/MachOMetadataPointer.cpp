//===- MachOMetadataPointer.cpp - Analysis-only pointer targets ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Resolve local metadata without turning a dyld signing recipe into a raw
/// pointer proof. Consumers still validate the destination record's layout.
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/MachOMetadataPointer.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DyldSharedCache.h"
#include "neverd/loader/MachO/MachOArm64e.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd::macho_loader {

const uint8_t *readMetadataBytes(const BinaryImage &Image, va_t Address,
                                 uint64_t Size) {
  if (Size > InvalidVA - Address)
    return nullptr;
  if (!Image.MachOSharedCache)
    return Image.readVA(Address, Size);
  if (!Image.isMachO() || Image.Arch != Arch::AArch64 || !Image.is64Bit())
    return nullptr;
  const auto *Owner = Image.getSegmentFor(Address);
  if (Owner) {
    if (!Owner->isReadable() || Owner->Size > InvalidVA - Owner->VA ||
        !rangeInBounds(Address - Owner->VA, Size, Owner->Size) ||
        !rangeInBounds(Address - Owner->VA, Size, Owner->FileSz))
      return nullptr;
    for (const auto &Other : Image.Segments)
      if (&Other != Owner && Other.Size &&
          (Other.VA <= Address ? Address - Other.VA < Other.Size
                               : Other.VA - Address < Size))
        return nullptr;
    return Image.readVA(Address, Size);
  }
  for (const auto &Other : Image.Segments)
    if (Other.Size && Other.VA > Address && Other.VA - Address < Size)
      return nullptr;
  const auto *Mapping = Image.MachOSharedCache->mappingFor(Address);
  if (!Mapping || !(Mapping->InitialProtection & llvm::MachO::VM_PROT_READ))
    return nullptr;
  const auto Bytes = Image.MachOSharedCache->readBytes(Address, Size);
  return Bytes ? Bytes->data() : nullptr;
}

namespace {

std::optional<va_t> readCachePointer(const BinaryImage &Image, va_t Slot) {
  const auto *Bytes = readMetadataBytes(Image, Slot, arm64e::kPointerBytes);
  if (!Bytes)
    return std::nullopt;
  auto Fixups = Image.MachOSharedCache->readSlideFixupsInRange(
      Slot, arm64e::kPointerBytes);
  if (!Fixups) {
    llvm::consumeError(Fixups.takeError());
    return std::nullopt;
  }
  const uint64_t Raw = readLE<uint64_t>(Bytes);
  const auto Auth = Image.MachOAuthenticatedPointers.find(Slot);
  if (Fixups->empty())
    return !Raw && Auth == Image.MachOAuthenticatedPointers.end()
               ? std::optional<va_t>(0)
               : std::nullopt;
  if (Fixups->size() != 1 || Fixups->front().Slot != Slot)
    return std::nullopt;
  const auto &Fixup = Fixups->front();
  if (Fixup.Authentication) {
    if (Raw != Fixup.EncodedValue)
      return std::nullopt;
    if (Auth != Image.MachOAuthenticatedPointers.end()) {
      const auto &Recipe = Auth->second;
      const auto &Expected = *Fixup.Authentication;
      if (Recipe.Target != Expected.Target ||
          Recipe.EncodedValue != Expected.EncodedValue ||
          Recipe.Key != Expected.Key ||
          Recipe.Diversity != Expected.Diversity ||
          Recipe.AddressDiversity != Expected.AddressDiversity ||
          !Recipe.Symbol.empty() || !Recipe.Module.empty() || Recipe.Addend ||
          Recipe.WeakImport || Recipe.LibraryOrdinal)
        return std::nullopt;
    }
    const va_t First = Slot >= arm64e::kPointerBytes - 1
                           ? Slot - (arm64e::kPointerBytes - 1)
                           : 0;
    for (va_t At = First; At < Slot + arm64e::kPointerBytes; ++At)
      if (Image.MachOResolvedChainedPointerSlots.count(At) ||
          Image.CodePtrRelocSlots.count(At) ||
          Image.DataPtrRelocSlots.count(At))
        return std::nullopt;
  } else {
    if (Auth != Image.MachOAuthenticatedPointers.end())
      return std::nullopt;
    const bool Resolved = Image.MachOResolvedChainedPointerSlots.count(Slot);
    if (Raw != (Resolved ? Fixup.Target : Fixup.EncodedValue))
      return std::nullopt;
  }
  if (!Fixup.Target)
    return 0;
  return readMetadataBytes(Image, Fixup.Target, 1)
             ? std::optional<va_t>(Fixup.Target)
             : std::nullopt;
}

} // namespace

std::optional<va_t> readMetadataPointerTarget(const BinaryImage &Image,
                                              va_t Slot) {
  const unsigned Width = Image.getPointerSize();
  if (Image.Format != BinaryFormat::MachO ||
      Image.MachOChainedFixupsAmbiguous ||
      (Width != sizeof(uint32_t) && Width != arm64e::kPointerBytes) ||
      Width > InvalidVA - Slot)
    return std::nullopt;
  if (Image.MachOSharedCache) {
    if (Image.Arch != Arch::AArch64 || Width != arm64e::kPointerBytes)
      return std::nullopt;
    const va_t First = Slot >= Width - 1 ? Slot - (Width - 1) : 0;
    for (va_t At = First; At < Slot + Width; ++At)
      if (Image.ImportPtrSlots.count(At) ||
          Image.ImportStorageSlots.count(At) || Image.DyldBindSlots.count(At) ||
          Image.ConflictingImportStorageSlots.count(At) ||
          (At != Slot && Image.MachOAuthenticatedPointers.count(At)))
        return std::nullopt;
    return readCachePointer(Image, Slot);
  }
  const auto *Owner = Image.getSegmentFor(Slot);
  if (!Owner || !Owner->isReadable() || Owner->Size > InvalidVA - Owner->VA ||
      !rangeInBounds(Slot - Owner->VA, Width, Owner->Size) ||
      !rangeInBounds(Slot - Owner->VA, Width, Owner->FileSz))
    return std::nullopt;
  const auto *Bytes = Image.readVA(Slot, Width);
  if (!Bytes)
    return std::nullopt;
  for (const auto &Other : Image.Segments)
    if (&Other != Owner && Other.Size &&
        (Other.VA <= Slot ? Slot - Other.VA < Other.Size
                          : Other.VA - Slot < Width))
      return std::nullopt;
  const auto Auth = Image.MachOAuthenticatedPointers.find(Slot);
  const bool HasAuth = Auth != Image.MachOAuthenticatedPointers.end();
  if (!HasAuth && Image.hasMachOAuthenticatedPointerIn(Slot, Width))
    return std::nullopt;
  const va_t First = Slot >= Width - 1 ? Slot - (Width - 1) : 0;
  for (va_t At = First; At < Slot + Width; ++At) {
    if (Image.ImportPtrSlots.count(At) || Image.ImportStorageSlots.count(At) ||
        Image.DyldBindSlots.count(At) ||
        Image.ConflictingImportStorageSlots.count(At) ||
        (At != Slot && Image.MachOAuthenticatedPointers.count(At)))
      return std::nullopt;
    if (HasAuth && (Image.MachOResolvedChainedPointerSlots.count(At) ||
                    Image.CodePtrRelocSlots.count(At) ||
                    Image.DataPtrRelocSlots.count(At)))
      return std::nullopt;
  }
  const uint64_t Raw = Width == arm64e::kPointerBytes ? readLE<uint64_t>(Bytes)
                                                      : readLE<uint32_t>(Bytes);
  if (!HasAuth) {
    if (Raw && Image.MachOHasChainedFixups &&
        !Image.MachOResolvedChainedPointerSlots.count(Slot))
      return std::nullopt;
    return Raw;
  }
  const auto &Recipe = Auth->second;
  if (Image.Arch != Arch::AArch64 || Width != arm64e::kPointerBytes ||
      !Recipe.EncodedValue || *Recipe.EncodedValue != Raw ||
      !arm64e::kAuthentication.extract(Raw) ||
      Recipe.Key != arm64e::kKey.extract(Raw) ||
      Recipe.Diversity != arm64e::kDiversity.extract(Raw) ||
      Recipe.AddressDiversity != bool(arm64e::kAddressDiversity.extract(Raw)) ||
      Recipe.Target == InvalidVA || Recipe.LibraryOrdinal)
    return std::nullopt;
  const auto *Target = Image.getSegmentFor(Recipe.Target);
  if (!Target || !Target->isReadable() ||
      Target->Size > InvalidVA - Target->VA ||
      !rangeInBounds(Recipe.Target - Target->VA, 1, Target->FileSz) ||
      !Image.readVA(Recipe.Target, 1))
    return std::nullopt;
  for (const auto &Other : Image.Segments)
    if (&Other != Target && Other.contains(Recipe.Target))
      return std::nullopt;
  return Recipe.Target;
}

} // namespace neverd::macho_loader
