//===- X86FourSourceShape.h - Four-source MedIR ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapt MedIR operands to the shared x86 four-source memory contract.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_MED_X86_X86FOURSOURCESHAPE_H
#define NEVERD_IR_MED_X86_X86FOURSOURCESHAPE_H
#include "neverd/ir/intrinsics/X86/X86FourSource.h"
#include "neverd/ir/med/IntrinsicShapes.h"
namespace neverd {
inline X86FourSourceShape x86FourSourceMedShape(const MedOp &Op,
                                                Arch A = Arch::Unknown) {
  X86FourSourceShape S;
  S.TargetArch = A;
  S.MemoryOrdering = Op.MemoryOrdering;
  S.MemoryAddressSpace = Op.MemoryAddressSpace;
  S.ValidId = Op.NumInputs && Op.NumInputs <= Op.Inputs.size() &&
              Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2 &&
              Op.Inputs[0].ConstVal ==
                  static_cast<uint16_t>(Intrinsic::X86FourSourceMemory);
  S.OutputSize = Op.Output.Size;
  S.WritableOutput = isMedIntrinsicWritableScalar(Op.Output);
  S.Arguments = Op.NumInputs ? Op.NumInputs - 1 : 0;
  for (unsigned I = 0;
       I < S.Arguments && I < S.Sizes.size() && I + 1 < Op.Inputs.size(); ++I) {
    const auto &V = Op.Inputs[I + 1];
    S.Sizes[I] = V.Size;
    S.Scalars[I] = isMedIntrinsicScalarInput(V) || V.Kind == MedVar::Param;
  }
  if (Op.NumInputs > 5 && Op.Inputs.size() > 5 && Op.Inputs[5].isConst() &&
      !isAddressProvenance(Op.Inputs[5].Provenance))
    S.Control = Op.Inputs[5].ConstVal;
  S.ValidId &= Op.IntrinsicOutputs.empty();
  const auto Observe = [&](const MedVar &V) {
    if (V.Size && V.Kind == MedVar::Reg) {
      if (S.TargetArch == Arch::Unknown)
        S.TargetArch = V.TheArch;
      else if (V.TheArch != Arch::Unknown && S.TargetArch != V.TheArch)
        S.ValidId = false;
    }
  };
  Observe(Op.Output);
  for (unsigned I = 1; I < Op.NumInputs && I < Op.Inputs.size(); ++I)
    Observe(Op.Inputs[I]);
  return S;
}

} // namespace neverd
#endif // NEVERD_IR_MED_X86_X86FOURSOURCESHAPE_H
