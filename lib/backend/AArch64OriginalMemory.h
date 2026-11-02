//===- AArch64OriginalMemory.h - Original memory storage -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A finite original byte representation, independent of callable relocation
/// identities. Callers must prove the read's result cannot be observed. This
/// descriptor grants no effective address, mapping or control-flow authority.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_AARCH64ORIGINALMEMORY_H
#define NEVERD_LIB_BACKEND_AARCH64ORIGINALMEMORY_H

#include "neverd/Limits.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace neverd {

#define NEVERD_A64_ORIGINAL_MEMORY_TEXT(Name, Text)                            \
  inline constexpr char Name[] = Text;
#include "AArch64OriginalMemory.def"

struct AArch64OriginalMemoryStorage {
  va_t Base;
  llvm::ArrayRef<uint8_t> Bytes;
  uint64_t Pad;
};

inline std::optional<AArch64OriginalMemoryStorage>
aarch64OriginalMemoryStorage(const BinaryImage &Image, va_t Base) {
  if (Image.Arch != Arch::AArch64 || !Image.IsRelocatable)
    return std::nullopt;
  const Segment *Segment = Image.getSegmentFor(Base);
  if (!Segment || !Segment->isReadable() || Base < Segment->VA ||
      Base - Segment->VA >= Segment->Data.size() ||
      (Segment->isExecutable() && !Image.DataInCode.count(Base)))
    return std::nullopt;
  uint64_t Bytes = Segment->Data.size() - (Base - Segment->VA);
  if (const auto *Section = Image.getSectionFor(Base)) {
    if (Base < Section->VA || Base - Section->VA >= Section->Size)
      return std::nullopt;
    Bytes = std::min(Bytes, Section->Size - (Base - Section->VA));
  }
  if (const auto ObjectBytes = Image.dataObjectSizeAt(Base))
    Bytes = std::min(Bytes, ObjectBytes);
  const uint64_t Pad = Base % limits::kMaxEmbeddedDataAlignment;
  const auto Pointer = Image.CodePtrRelocSlots.lower_bound(Base);
  if (!Bytes || Bytes > InvalidVA - Base ||
      Bytes > limits::kMaxSingleGlobalEmbedLen - Pad ||
      Pointer == Image.CodePtrRelocSlots.end() || *Pointer - Base >= Bytes ||
      Image.hasMachOAuthenticatedPointerIn(Base, Bytes))
    return std::nullopt;
  return AArch64OriginalMemoryStorage{
      Base,
      llvm::ArrayRef<uint8_t>(Segment->Data).slice(Base - Segment->VA, Bytes),
      Pad};
}

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_AARCH64ORIGINALMEMORY_H
