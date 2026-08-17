//===- MachODyldFixups.h - Shared Mach-O dyld fixup helpers ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_MACHO_MACHODYLDFIXUPS_H
#define NEVERD_LIB_LOADER_MACHO_MACHODYLDFIXUPS_H

#include "neverd/loader/BinaryImage.h"

namespace neverd {
namespace macho_loader {
namespace detail {

inline void clearLocalPointerClassification(BinaryImage &Img, va_t SlotVA) {
  Img.CodePtrRelocSlots.erase(SlotVA);
  Img.DataPtrRelocSlots.erase(SlotVA);
}

inline bool recordAbsolutePointerSlot(BinaryImage &Img, va_t SlotVA,
                                      va_t TargetVA) {
  if (Img.DyldBindSlots.count(SlotVA) || Img.ImportPtrSlots.count(SlotVA))
    return false;

  const Segment *TargetSeg = Img.getSegmentFor(TargetVA);
  if (!TargetSeg)
    return false;
  if (Img.isCodeAddress(TargetVA)) {
    const bool Changed = Img.DataPtrRelocSlots.erase(SlotVA) != 0;
    return Img.CodePtrRelocSlots.insert(SlotVA).second || Changed;
  }
  if (Img.isDataAddress(TargetVA) && !TargetSeg->Data.empty()) {
    const bool Changed = Img.CodePtrRelocSlots.erase(SlotVA) != 0;
    return Img.DataPtrRelocSlots.insert(SlotVA).second || Changed;
  }
  return false;
}

} // namespace detail
} // namespace macho_loader
} // namespace neverd

#endif // NEVERD_LIB_LOADER_MACHO_MACHODYLDFIXUPS_H
