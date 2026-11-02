//===- CodePointerSources.cpp - Relocated code coordinates ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reads only relocation-marked initial code coordinates. Entry admission and
/// current-function ownership remain the caller's separate decisions.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/CodePointerSources.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd {

std::optional<va_t> normalizedRelocatedCodeTarget(const BinaryImage &Image,
                                                  va_t Slot) {
  const bool Is64 = Image.is64Bit();
  const uint32_t Width = getPointerSize(Is64);
  if (Width != Image.getPointerSize() || !Image.CodePtrRelocSlots.count(Slot))
    return {};
  const auto *Bytes = Image.readVA(Slot, Width);
  if (!Bytes)
    return {};
  return normalizeCodeAddress(readPtr(Bytes, Is64), Image.Arch, Image.Mode);
}

} // namespace neverd
