//===- AArch64WritebackMemory.h - LowIR writeback bindings -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Auxiliary results require complete, independently stored temporary carriers.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_AARCH64WRITEBACKMEMORY_H
#define NEVERD_IR_LOW_AARCH64WRITEBACKMEMORY_H

#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.h"
#include "neverd/ir/low/LowIntrinsicOutputs.h"

#include <optional>
#include <vector>

namespace neverd {

/// Query a validated auxiliary-load operation before conversion or tracing.
inline std::optional<std::vector<NdVar>>
lowAArch64WritebackOutputs(const LowOp &Op, llvm::ArrayRef<LowOp> Following) {
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  auto Outputs = collectLowIntrinsicOutputBindings(
      Op, Following, aarch64WritebackMemoryOutputCount(Id));
  if (!aarch64WritebackMemoryOutputsAreValid(
          Id, Outputs, [](const NdVar &V) { return V.Size; }))
    return std::nullopt;
  for (size_t I = 0; I != Outputs.size(); ++I)
    for (size_t J = 0; J != I; ++J)
      if (Outputs[I].Offset == Outputs[J].Offset)
        return std::nullopt;
  return Outputs;
}

} // namespace neverd

#endif // NEVERD_IR_LOW_AARCH64WRITEBACKMEMORY_H
