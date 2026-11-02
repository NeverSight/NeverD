//===- X86CompactShape.h - Compact memory LowIR adapter -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapt LowIR operands to the shared x86 compact-memory contract.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_LOW_X86_X86COMPACTSHAPE_H
#define NEVERD_IR_LOW_X86_X86COMPACTSHAPE_H
#include "neverd/ir/intrinsics/X86/X86CompressExpand.h"
#include "neverd/ir/low/LowIR.h"

#include <iterator>

namespace neverd {
inline X86CompactShape x86CompactLowShape(const LowOp &Op,
                                          Arch A = Arch::Unknown) {
  X86CompactShape S;
  S.TargetArch = A;
  S.MemoryOrdering = Op.MemoryOrdering;
  S.MemoryAddressSpace = Op.MemoryAddressSpace;
  S.ValidId = Op.NumInputs && Op.NumInputs <= std::size(Op.Inputs) &&
              Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2 &&
              Op.Inputs[0].Offset <= UINT16_MAX;
  S.OutputSize = Op.Output.Size;
  S.WritableOutput = Op.Output.isReg() || Op.Output.isTemp();
  S.Arguments = Op.NumInputs ? Op.NumInputs - 1 : 0;
  for (unsigned I = 0;
       I < S.Arguments && I < S.Sizes.size() && I + 1 < std::size(Op.Inputs);
       ++I) {
    const auto &V = Op.Inputs[I + 1];
    S.Sizes[I] = V.Size;
    S.Scalars[I] = V.isConst() || V.isReg() || V.isTemp();
  }
  if (Op.NumInputs > 4 && Op.Inputs[4].isConst() &&
      !isAddressProvenance(Op.Inputs[4].Provenance))
    S.Control = Op.Inputs[4].Offset;
  return S;
}

} // namespace neverd
#endif // NEVERD_IR_LOW_X86_X86COMPACTSHAPE_H
