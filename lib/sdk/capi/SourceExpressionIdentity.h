#ifndef NEVERD_SDK_CAPI_SOURCEEXPRESSIONIDENTITY_H
#define NEVERD_SDK_CAPI_SOURCEEXPRESSIONIDENTITY_H

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/high/HighIR.h"

namespace neverd::sdk {
// Bounded identity comparison for canonical source replays. This grants no
// semantics to a call binding; callers authenticate bindings separately.
inline bool sameSourceExpressionIdentity(const HighExpr &A, const HighExpr &B,
                                         size_t &Budget, unsigned Depth = 0) {
  if (!Budget || Depth == 128)
    return false;
  --Budget;
  if (A.Kind != B.Kind || A.Op != B.Op || !equalSourceTypes(A.Type, B.Type) ||
      A.MemoryOrdering != B.MemoryOrdering ||
      A.MemoryAddressSpace != B.MemoryAddressSpace ||
      A.Operands.size() != B.Operands.size() ||
      A.IntrinsicId != B.IntrinsicId ||
      A.IntrinsicOutputs != B.IntrinsicOutputs ||
      (A.Kind != ExprKind::Call && A.SourceCallHint != B.SourceCallHint) ||
      bool(A.IndirectTarget) != bool(B.IndirectTarget))
    return false;
  switch (A.Kind) {
  case ExprKind::Var:
    if (A.Var != B.Var || A.Var.Size != B.Var.Size ||
        A.Var.TheArch != B.Var.TheArch)
      return false;
    break;
  case ExprKind::Const:
    if (A.ConstVal != B.ConstVal || A.ConstProvenance != B.ConstProvenance ||
        A.AddressOwnerVA != B.AddressOwnerVA)
      return false;
    break;
  case ExprKind::Call:
    if (A.CallAddr != B.CallAddr || A.CallTarget != B.CallTarget ||
        A.IsIndirectCall != B.IsIndirectCall ||
        A.IndirectParamIdx != B.IndirectParamIdx ||
        A.IntrinsicId != B.IntrinsicId ||
        A.IntrinsicOutputs != B.IntrinsicOutputs ||
        A.SourceCallHint != B.SourceCallHint)
      return false;
    break;
  case ExprKind::Cast:
    if (!equalSourceTypes(A.CastTo, B.CastTo))
      return false;
    break;
  case ExprKind::Field:
    if (A.ConstVal != B.ConstVal)
      return false;
    break;
  case ExprKind::Undef:
    return false;
  default:
    break;
  }
  for (size_t I = 0; I < A.Operands.size(); ++I)
    if (!A.Operands[I] || !B.Operands[I] ||
        !sameSourceExpressionIdentity(*A.Operands[I], *B.Operands[I], Budget,
                                      Depth + 1))
      return false;
  return !A.IndirectTarget ||
         sameSourceExpressionIdentity(*A.IndirectTarget, *B.IndirectTarget,
                                      Budget, Depth + 1);
}

} // namespace neverd::sdk
#endif
