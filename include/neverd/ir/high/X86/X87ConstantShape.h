//===- X87ConstantShape.h - HighIR x87 control values ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapt typed HighIR operands to the shared numerical/control contract.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_HIGH_X86_X87CONSTANTSHAPE_H
#define NEVERD_IR_HIGH_X86_X87CONSTANTSHAPE_H
#include "neverd/ir/X86/X87Constant.h"
#include "neverd/ir/high/HighIR.h"
namespace neverd {
inline X87ConstantControlShape x87ConstantControlHighShape(const HighExpr &Call,
                                                           Arch A) {
  LowOp Op;
  Op.NumInputs = Call.Operands.size() + 1;
  Op.Inputs[0] = NdVar::cst(unsigned(Call.IntrinsicId), 2);
  Op.Output = NdVar::tmp(0, Call.Type ? Call.Type->Size : 0);
  Op.MemoryOrdering = Call.MemoryOrdering;
  Op.MemoryAddressSpace = Call.MemoryAddressSpace;
  if (Call.Operands.size() > 5 || Call.Kind != ExprKind::Call ||
      Call.IsIndirectCall || Call.IndirectTarget || Call.SourceCallHint ||
      Call.DoesNotReturn ||
      (!Call.Type && Call.IntrinsicId != Intrinsic::X87WriteControl) ||
      (Call.Type && Call.Type->Kind != NdTypeKind::Int &&
       Call.Type->Kind != NdTypeKind::Void))
    return {};
  for (unsigned I = 0; I < Call.Operands.size(); ++I) {
    const auto &V = Call.Operands[I];
    if (!V || !V->Type || V->Type->Kind != NdTypeKind::Int)
      return {};
    Op.Inputs[I + 1] = V->Kind == ExprKind::Const
                           ? NdVar::cst(V->ConstVal, V->Type->Size)
                           : NdVar::tmp(0, V->Type->Size);
    if (V->Kind == ExprKind::Var && V->Var.TheArch != Arch::Unknown &&
        V->Var.TheArch != A)
      return {};
  }
  auto Shape = x87ConstantControlLowShape(Op, A);
  Shape.HasAuxiliaryOutputs = !Call.IntrinsicOutputs.empty();
  return Shape;
}
} // namespace neverd
#endif
