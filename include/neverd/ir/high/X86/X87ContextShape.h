//===- X87ContextShape.h - HighIR complete x87 contexts -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapt typed HighIR operands to the shared complete-context contract.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_HIGH_X86_X87CONTEXTSHAPE_H
#define NEVERD_IR_HIGH_X86_X87CONTEXTSHAPE_H
#include "neverd/ir/X86/X87Context.h"
#include "neverd/ir/high/HighIR.h"
namespace neverd {
inline bool x87ContextHighShapeIsValid(const HighExpr &Call, Arch A) {
  if (Call.Operands.size() > 5 || Call.Kind != ExprKind::Call ||
      Call.IsIndirectCall || Call.IndirectTarget || Call.SourceCallHint ||
      Call.DoesNotReturn || !Call.IntrinsicOutputs.empty())
    return false;
  LowOp Op;
  Op.NumInputs = Call.Operands.size() + 1;
  Op.Inputs[0] = NdVar::cst(unsigned(Call.IntrinsicId), 2);
  Op.Output = NdVar::tmp(0, Call.Type ? Call.Type->Size : 0);
  Op.MemoryOrdering = Call.MemoryOrdering;
  Op.MemoryAddressSpace = Call.MemoryAddressSpace;
  if (Call.Type && Call.Type->Kind != NdTypeKind::Void &&
      Call.Type->Kind != NdTypeKind::Int && Call.Type->Kind != NdTypeKind::Ptr)
    return false;
  for (unsigned I = 0; I < Call.Operands.size(); ++I) {
    const auto &V = Call.Operands[I];
    if (!V || !V->Type ||
        (V->Type->Kind != NdTypeKind::Int && V->Type->Kind != NdTypeKind::Ptr))
      return false;
    Op.Inputs[I + 1] = V->Kind == ExprKind::Const
                           ? NdVar::cst(V->ConstVal, V->Type->Size)
                           : NdVar::tmp(0, V->Type->Size);
    if (V->Kind == ExprKind::Var && V->Var.TheArch != Arch::Unknown &&
        V->Var.TheArch != A)
      return false;
  }
  return x87ContextLowShapeIsValid(Op, A);
}
} // namespace neverd
#endif
