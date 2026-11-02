//===- X64ArchitecturalValues.h - Scalar choice lowering --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Shared bit updates for checked continuation and region sources.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64ARCHITECTURALVALUES_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64ARCHITECTURALVALUES_H

#include "neverd/ir/low/LowIR.h"

#include <cstdint>

namespace neverd::analysis::detail {

/// Callers validate a nonempty scalar range, equally sized Old/Witness, and a
/// Boolean guard. This only lowers the bit update; it supplies no architecture
/// facts, storage aliases, runtime choices or instruction retry semantics.
template <typename Writer>
NdVar lowerX64ArchitecturalBits(Writer &W, NdVar Old, NdVar When, NdVar Witness,
                                uint16_t BitOffset, uint16_t BitCount) {
  const uint64_t Bits = UINT64_MAX >> (64 - BitCount);
  const uint64_t Mask = Bits << BitOffset;
  const auto K = [&](uint64_t Value) { return NdVar::scalar(Value, Old.Size); };
  const auto Limited = W.temporary(Old.Size);
  const auto Shifted = W.temporary(Old.Size);
  const auto Kept = W.temporary(Old.Size);
  const auto Updated = W.temporary(Old.Size);
  W.emit(NdOp::INT_AND, Limited, {Witness, K(Bits)});
  W.emit(NdOp::INT_LEFT, Shifted, {Limited, K(BitOffset)});
  W.emit(NdOp::INT_AND, Kept, {Old, K(~Mask)});
  W.emit(NdOp::INT_OR, Updated, {Kept, Shifted});
  if (When.isConst() && When.Offset == 1)
    return Updated;
  const auto Result = W.temporary(Old.Size);
  W.emit(NdOp::SELECT, Result, {When, Updated, Old});
  return Result;
}

} // namespace neverd::analysis::detail

#endif // NEVERD_ANALYSIS_ARCH_X86_64_X64ARCHITECTURALVALUES_H
