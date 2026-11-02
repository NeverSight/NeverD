//===- X87ValueShape.h - HighIR scalar x87 value contracts ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapt HighIR raw scalar arguments to the shared x87 value contract.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_X86_X87VALUESHAPE_H
#define NEVERD_IR_HIGH_X86_X87VALUESHAPE_H

#include "neverd/ir/X86/X87Value.h"
#include "neverd/ir/high/HighIR.h"

namespace neverd {

inline X87ValueShape x87ValueHighShape(const HighExpr &Call, Arch A) {
  if (Call.Kind != ExprKind::Call || Call.Operands.size() > 2 ||
      Call.IsIndirectCall || Call.IndirectTarget || Call.SourceCallHint ||
      Call.DoesNotReturn || !Call.Type || Call.Type->Kind != NdTypeKind::Int)
    return {};
  LowOp Low;
  Low.Opcode = NdOp::INTRINSIC;
  Low.Output = NdVar::tmp(0, Call.Type->Size);
  Low.MemoryOrdering = Call.MemoryOrdering;
  Low.MemoryAddressSpace = Call.MemoryAddressSpace;
  Low.addInput(NdVar::cst(unsigned(Call.IntrinsicId), 2));
  for (const auto &V : Call.Operands) {
    if (!V || !V->Type || V->Type->Kind != NdTypeKind::Int)
      return {};
    if (V->Kind == ExprKind::Var && V->Var.TheArch != Arch::Unknown) {
      if (A == Arch::Unknown)
        A = V->Var.TheArch;
      else if (V->Var.TheArch != A)
        return {};
    }
    Low.addInput(V->Kind == ExprKind::Const
                     ? NdVar::cst(V->ConstVal, V->Type->Size)
                     : NdVar::tmp(0, V->Type->Size));
  }
  auto Shape = x87ValueLowShape(Low, A);
  Shape.HasAuxiliaryOutputs = !Call.IntrinsicOutputs.empty();
  return Shape;
}

} // namespace neverd

#endif // NEVERD_IR_HIGH_X86_X87VALUESHAPE_H
