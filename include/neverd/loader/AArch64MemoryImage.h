//===- AArch64MemoryImage.h - Immediate image bases ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared byte ownership for rebasing an original immediate memory base.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_AARCH64MEMORYIMAGE_H
#define NEVERD_LOADER_AARCH64MEMORYIMAGE_H

#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"
#include "neverd/loader/BinaryImage.h"

#include <optional>

namespace neverd {

/// Provenance belongs to the base occurrence, never to a matching image VA.
/// Include the raw base as well as every accessed byte, so negative offsets
/// and the maximum unsigned Q offset retain one contiguous relocation owner.
template <typename InstructionT>
inline std::optional<AArch64AddressMemorySpan> aarch64ImmediateMemoryImageSpan(
    const BinaryImage &Image, const InstructionT &Instruction, va_t Base,
    ConstantAddressProvenance Provenance, va_t Owner) {
  if (Instruction.hasRegisterIndex() || Instruction.stackBase() ||
      !isExactAddressProvenance(Provenance) ||
      isCodeAddressProvenance(Provenance) ||
      (Provenance != ConstantAddressProvenance::DataAddress &&
       Image.isCodeAddress(Base)))
    return std::nullopt;
  if (Owner == InvalidVA)
    Owner = Image.layoutAddressOwner(Base);
  const auto Span = Instruction.controlIndependentSpan(Base, 0);
  if (!Span || !Span->Bytes || Owner == InvalidVA ||
      !Image.relocatedTargetBelongsToOwner(Base, Owner) ||
      Image.layoutAddressOwner(Span->Begin) != Owner ||
      Image.layoutAddressOwner(Span->Begin + Span->Bytes - 1) != Owner ||
      !Image.hasObjectDataProvenance(Span->Begin) ||
      !Image.hasObjectDataProvenance(Span->Begin + Span->Bytes - 1) ||
      !Image.readVA(Span->Begin, Span->Bytes))
    return std::nullopt;
  return Span;
}

} // namespace neverd

#endif // NEVERD_LOADER_AARCH64MEMORYIMAGE_H
