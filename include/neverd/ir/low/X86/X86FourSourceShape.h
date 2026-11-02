//===- X86FourSourceShape.h - Four-source LowIR ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapt LowIR operands to the shared x86 four-source memory contract.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_LOW_X86_X86FOURSOURCESHAPE_H
#define NEVERD_IR_LOW_X86_X86FOURSOURCESHAPE_H
#include "neverd/ir/intrinsics/X86/X86FourSource.h"
#include "neverd/ir/low/LowIR.h"

#include <iterator>

namespace neverd {
inline X86FourSourceShape x86FourSourceLowShape(const LowOp &Op,
                                                Arch A = Arch::Unknown) {
  X86FourSourceShape S;
  S.TargetArch = A;
  S.MemoryOrdering = Op.MemoryOrdering;
  S.MemoryAddressSpace = Op.MemoryAddressSpace;
  S.ValidId = Op.NumInputs && Op.NumInputs <= std::size(Op.Inputs) &&
              Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2 &&
              Op.Inputs[0].Offset ==
                  static_cast<uint16_t>(Intrinsic::X86FourSourceMemory);
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
  if (Op.NumInputs > 5 && Op.Inputs[5].isConst() &&
      !isAddressProvenance(Op.Inputs[5].Provenance))
    S.Control = Op.Inputs[5].Offset;
  return S;
}

} // namespace neverd
#endif // NEVERD_IR_LOW_X86_X86FOURSOURCESHAPE_H
