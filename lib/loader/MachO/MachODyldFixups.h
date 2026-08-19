//===- MachODyldFixups.h - Shared Mach-O dyld fixup helpers ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_MACHO_MACHODYLDFIXUPS_H
#define NEVERD_LIB_LOADER_MACHO_MACHODYLDFIXUPS_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/PointerRelocation.h"

namespace neverd {
namespace macho_loader {
namespace detail {

inline void clearLocalPointerClassification(BinaryImage &Img, va_t SlotVA) {
  Img.CodePtrRelocSlots.erase(SlotVA);
  Img.DataPtrRelocSlots.erase(SlotVA);
}

inline bool recordAbsolutePointerSlot(BinaryImage &Img, va_t SlotVA,
                                      va_t TargetVA) {
  return recordAbsolutePointerRelocation(Img, SlotVA, TargetVA);
}

} // namespace detail
} // namespace macho_loader
} // namespace neverd

#endif // NEVERD_LIB_LOADER_MACHO_MACHODYLDFIXUPS_H
