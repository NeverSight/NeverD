//===- DyldCacheLoading.cpp - Cache slide ownership ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve authentication recipes and resolve unsigned cache pointers once,
/// before metadata and function discovery consume the selected image.
//===----------------------------------------------------------------------===//

#include "DyldCacheLoading.h"

#include "neverd/loader/MachO/DyldCacheLoad.h"
#include "neverd/loader/MachO/DyldSharedCache.h"
#include "neverd/loader/MachO/MachOArm64e.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/support/BinaryEncoding.h"

#include <map>
#include <optional>

namespace neverd::macho_loader {

llvm::Error applyCacheSlideFixups(BinaryImage &Image,
                                  const LoadProgress &Progress) {
  const auto Fail = [] {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   cache_load::kInvalidSlideSlot);
  };
  if (!Image.MachOSharedCache || Image.Arch != Arch::AArch64 ||
      !Image.is64Bit() || Image.MachOChainedFixupsAmbiguous)
    return Fail();
  std::map<va_t, DyldCacheFixup> Pending;
  for (const auto &Segment : Image.Segments) {
    Progress.check();
    auto Fixups = Image.MachOSharedCache->readSlideFixupsInRange(
        Segment.VA, Segment.FileSz, Progress);
    if (!Fixups)
      return Fixups.takeError();
    for (auto &Fixup : *Fixups) {
      if (Fixup.Slot < Segment.VA ||
          !rangeInBounds(Fixup.Slot - Segment.VA, arm64e::kPointerBytes,
                         Segment.FileSz) ||
          !rangeInBounds(Fixup.Slot - Segment.VA, arm64e::kPointerBytes,
                         Segment.Data.size()) ||
          readLE<uint64_t>(Segment.Data.data() + Fixup.Slot - Segment.VA) !=
              Fixup.EncodedValue ||
          Image.ImportPtrSlots.count(Fixup.Slot) ||
          Image.ImportStorageSlots.count(Fixup.Slot) ||
          Image.DyldBindSlots.count(Fixup.Slot) ||
          !Pending.emplace(Fixup.Slot, std::move(Fixup)).second)
        return Fail();
    }
  }
  std::optional<va_t> Previous;
  for (const auto &[Slot, Fixup] : Pending) {
    if (Previous && Slot - *Previous < arm64e::kPointerBytes)
      return Fail();
    Previous = Slot;
  }
  for (const auto &[Slot, Name] : Image.MachOSharedCacheSymbolPointers) {
    Progress.check();
    const auto *Bytes = Image.readVA(Slot, arm64e::kPointerBytes);
    if (!Bytes || Name.empty() ||
        (!Pending.count(Slot) && readLE<uint64_t>(Bytes)))
      return Fail();
  }
  for (const auto &[Slot, Fixup] : Pending) {
    Progress.check();
    if (Fixup.Authentication) {
      Image.MachOAuthenticatedPointers.emplace(Slot, *Fixup.Authentication);
    } else {
      if (!Image.patchPtr(Slot, Fixup.Target))
        return Fail();
      Image.MachOResolvedChainedPointerSlots.insert(Slot);
      // Classification only grants proof for targets owned by this image.
      // Cross-image addresses remain cache metadata, not native pointer proof.
      recordAbsolutePointerRelocation(Image, Slot, Fixup.Target);
    }
  }
  Image.MachOHasChainedFixups = true;
  return llvm::Error::success();
}

} // namespace neverd::macho_loader
