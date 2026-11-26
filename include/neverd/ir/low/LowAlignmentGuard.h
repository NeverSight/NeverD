//===- LowAlignmentGuard.h - LowIR alignment preconditions ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_LOWALIGNMENTGUARD_H
#define NEVERD_IR_LOW_LOWALIGNMENTGUARD_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowIR.h"

namespace neverd {

/// An unconditional or enabled alignment check in the default address space.
/// Its normal continuation preserves every register and memory byte.
inline bool isDefaultAddressAlignmentGuard(const LowOp &Operation) {
  if (Operation.Opcode != NdOp::INTRINSIC ||
      Operation.MemoryOrdering != NdMemoryOrdering::None ||
      Operation.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      (Operation.NumInputs != 3 && Operation.NumInputs != 4) ||
      !Operation.Inputs[0].isConst() || Operation.Inputs[0].Size != 2 ||
      Operation.Inputs[0].Offset != uint64_t(Intrinsic::RequireAligned) ||
      !intrinsicMemoryAddressSpaceShapeIsValid(
          Intrinsic::RequireAligned, Operation.NumInputs, Operation.Output.Size,
          Operation.Inputs[1].Size, Operation.Inputs[2].Size,
          Operation.NumInputs == 4 ? Operation.Inputs[3].Size : 0) ||
      !Operation.Inputs[2].isConst() || !Operation.Inputs[2].Offset ||
      (Operation.Inputs[2].Offset & (Operation.Inputs[2].Offset - 1)))
    return false;
  for (unsigned I : {1U, 3U}) {
    if (I >= Operation.NumInputs)
      continue;
    const auto &Value = Operation.Inputs[I];
    if ((!Value.isReg() && !Value.isTemp() && !Value.isConst()) ||
        (!Value.isConst() && Value.Offset > UINT64_MAX - Value.Size))
      return false;
  }
  return true;
}

} // namespace neverd

#endif
