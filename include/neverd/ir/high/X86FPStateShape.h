//===- X86FPStateShape.h - Typed HighIR state transports -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_X86FPSTATESHAPE_H
#define NEVERD_IR_HIGH_X86FPSTATESHAPE_H

#include "neverd/ir/X86FPState.h"
#include "neverd/ir/high/HighIR.h"

#include <algorithm>

namespace neverd {
inline X86FPStateShape x86FPStateHighShape(const HighExpr &Call,
                                           Arch TargetArch) {
  const auto Size = [&](unsigned Index) -> unsigned {
    return Index < Call.Operands.size() && Call.Operands[Index] &&
                   Call.Operands[Index]->Type
               ? Call.Operands[Index]->Type->Size
               : 0;
  };
  const bool Conversion = isX86FPConversionStateIntrinsic(Call.IntrinsicId);
  const bool Round = isX86FPRoundStateIntrinsic(Call.IntrinsicId);
  const bool Memory = Call.IntrinsicId == Intrinsic::X86FPRoundMemoryState;
  const unsigned ControlIndex = Memory ? 1 : 0;
  const bool HasControl =
      Call.Operands.size() > ControlIndex && Call.Operands[ControlIndex];
  bool ScalarOperands = true;
  for (unsigned Index = 0; Index < Call.Operands.size(); ++Index) {
    const auto &Operand = Call.Operands[Index];
    ScalarOperands &=
        Operand && Operand->Type &&
        (Operand->Type->Kind == NdTypeKind::Int ||
         Operand->Type->Kind == NdTypeKind::Float ||
         (Memory && Index == 0 && Operand->Type->Kind == NdTypeKind::Ptr));
  }
  const bool PointerAddress = Memory && !Call.Operands.empty() &&
                              Call.Operands[0] && Call.Operands[0]->Type &&
                              Call.Operands[0]->Type->Kind == NdTypeKind::Ptr &&
                              Size(0) == (TargetArch == Arch::X86 ? 4 : 8);
  const bool HasSelector = Call.Operands.size() > 2 && Call.Operands[2];
  return {.TargetArch = TargetArch,
          .MemoryOrdering = Call.MemoryOrdering,
          .MemoryAddressSpace = Call.MemoryAddressSpace,
          .NumInputs = static_cast<unsigned>(Call.Operands.size() + 1),
          .IdIsConst = true,
          .IdSize = 2,
          .OutputIsWritable = Call.Type && Call.Type->Kind == NdTypeKind::Int &&
                              Call.Type->Size > 0,
          .OutputSize = Call.Type ? Call.Type->Size : 0U,
          .OperandsAreScalar = ScalarOperands,
          .LeftSize = PointerAddress ? 8 : Size(0),
          .RightSize = Size(1),
          .StateSize = Size(Round        ? 3
                            : Conversion ? 1
                                         : 2),
          .HasAuxiliaryOutputs = !Call.IntrinsicOutputs.empty(),
          .DestinationIsConst = HasSelector &&
                                Call.Operands[2]->Kind == ExprKind::Const &&
                                Call.Operands[2]->Type &&
                                Call.Operands[2]->Type->Kind == NdTypeKind::Int,
          .DestinationSelectorSize = Size(2),
          .DestinationBytes = HasSelector ? Call.Operands[2]->ConstVal : 0,
          .ControlIsConst =
              HasControl &&
              Call.Operands[ControlIndex]->Kind == ExprKind::Const &&
              Call.Operands[ControlIndex]->Type &&
              Call.Operands[ControlIndex]->Type->Kind == NdTypeKind::Int,
          .ControlSize = Size(ControlIndex),
          .Control = HasControl ? Call.Operands[ControlIndex]->ConstVal : 0,
          .ImmediateIsConst = HasSelector &&
                              Call.Operands[2]->Kind == ExprKind::Const &&
                              Call.Operands[2]->Type &&
                              Call.Operands[2]->Type->Kind == NdTypeKind::Int,
          .ImmediateSize = Size(2),
          .Immediate = HasSelector ? Call.Operands[2]->ConstVal : 0,
          .AddressIsScalar = PointerAddress ||
                             (!Call.Operands.empty() && Call.Operands[0] &&
                              Call.Operands[0]->Type &&
                              Call.Operands[0]->Type->Kind == NdTypeKind::Int &&
                              Size(0) == 8)};
}
} // namespace neverd
#endif
