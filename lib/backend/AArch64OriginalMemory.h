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
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"
#include "neverd/ir/low/LowIR.h"
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

/// Bounds the preserved architectural read for a terminal-exclusive table.
/// The caller must already have proved the read and its complete use closure.
inline std::optional<AArch64OriginalMemoryStorage>
aarch64OriginalJumpTableMemory(const BinaryImage &Image,
                              const AArch64AddressMemoryInstruction &Instruction,
                              const JumpTable &Table) {
  if (!Table.HasBaseAddr || !Table.HasDispatchSlotMap ||
      Table.SlotIndices.empty())
    return std::nullopt;
  const uint32_t Last =
      *std::max_element(Table.SlotIndices.begin(), Table.SlotIndices.end());
  const auto Span = Instruction.controlIndependentSpan(Table.BaseAddr, Last);
  const auto Storage =
      Span ? aarch64OriginalMemoryStorage(Image, Span->Begin) : std::nullopt;
  if (!Storage || Span->Bytes > Storage->Bytes.size())
    return std::nullopt;
  return Storage;
}

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_AARCH64ORIGINALMEMORY_H
