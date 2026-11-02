#ifndef NEVERD_IR_HIGH_FRAMEADDRESS_H
#define NEVERD_IR_HIGH_FRAMEADDRESS_H
#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/MathExtras.h"
namespace neverd::high_detail {
/// Dead-store cleanup retains empty blocks as address/label anchors. They do
/// not end an entry address prefix, but must remain in the statement stream.
inline bool isEmptyBlockAnchor(const HighStmt &S) {
  if (S.Kind != StmtKind::Block || !S.Body.empty() || !S.ElseBody.empty() ||
      !S.Cases.empty() || !S.DefaultBody.empty() || !S.EHClauseBodies.empty() ||
      !S.EHClauses.empty() || S.IsPhiCopy ||
      S.MemoryOrdering != NdMemoryOrdering::None ||
      S.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  bool Empty = true;
  forEachExpr(S, [&](const ExprPtr &) { Empty = false; });
  return Empty;
}

/// A bounded arithmetic offset from the synthetic entry stack pointer.
/// The caller must separately check the access width and private frame bounds,
/// and prove that each supplied alias is defined and evaluates to the same
/// immutable frame address at every use.
inline std::optional<int64_t>
frameAddressOffset(const ExprPtr &E, const HighFunc &Func, Arch Architecture,
                   size_t &Budget, unsigned Depth = 0,
                   llvm::function_ref<ExprPtr(const MedVar &)> Alias = {}) {
  const auto &TRI = getTargetRegInfo(Architecture);
  if (!E || !E->Type || E->Type->Size != TRI.PointerSize ||
      E->MemoryOrdering != NdMemoryOrdering::None ||
      E->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Depth > ::neverd::limits::ir::kHighFrameAddressOffsetDepthLimit ||
      !Budget)
    return std::nullopt;
  --Budget;
  if (E->Kind == ExprKind::Var && E->Var.Size == TRI.PointerSize &&
      isSyntheticEntryStackPointer(E->Var, Func, Architecture))
    return 0;
  if (Alias && E->Kind == ExprKind::Var && E->Var.Size == TRI.PointerSize) {
    if (ExprPtr Value = Alias(E->Var))
      return frameAddressOffset(Value, Func, Architecture, Budget, Depth + 1,
                                Alias);
  }
  // A signedness view of the target pointer bits does not alter its address.
  // Keep widening and truncating conversions outside this proof.
  if ((E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast) &&
      E->IntrinsicId == Intrinsic::None && E->IntrinsicOutputs.empty() &&
      !E->IndirectTarget && E->Type->Kind == NdTypeKind::Int &&
      E->Operands.size() == 1 && E->Operands[0] && E->Operands[0]->Type &&
      E->Operands[0]->Type->Kind == NdTypeKind::Int &&
      E->Operands[0]->Type->Size == TRI.PointerSize &&
      (E->Kind == ExprKind::BitCast ||
       (E->CastTo && E->CastTo->Kind == NdTypeKind::Int &&
        E->CastTo->Size == TRI.PointerSize &&
        E->CastTo->IsSigned == E->Type->IsSigned)))
    return frameAddressOffset(E->Operands[0], Func, Architecture, Budget,
                              Depth + 1, Alias);
  if (E->Kind != ExprKind::BinOp || E->Operands.size() != 2 ||
      (E->Op != NdOp::INT_ADD && E->Op != NdOp::INT_SUB))
    return std::nullopt;
  const bool Swapped = E->Op == NdOp::INT_ADD && E->Operands[0] &&
                       E->Operands[0]->Kind == ExprKind::Const;
  const auto &BaseExpr = E->Operands[Swapped ? 1 : 0];
  const auto &OffsetExpr = E->Operands[Swapped ? 0 : 1];
  if (!OffsetExpr || OffsetExpr->Kind != ExprKind::Const || !OffsetExpr->Type ||
      !OffsetExpr->Type->Size || OffsetExpr->Type->Size > TRI.PointerSize ||
      (OffsetExpr->Type->Size < 8 &&
       OffsetExpr->ConstVal >= (UINT64_C(1) << (OffsetExpr->Type->Size * 8))))
    return std::nullopt;
  const auto Base = frameAddressOffset(BaseExpr, Func, Architecture, Budget,
                                       Depth + 1, Alias);
  if (!Base)
    return std::nullopt;
  const int64_t Delta = TRI.PointerSize == 4
                            ? int64_t(int32_t(OffsetExpr->ConstVal))
                            : int64_t(OffsetExpr->ConstVal);
  int64_t Result;
  if (E->Op == NdOp::INT_ADD ? llvm::AddOverflow(*Base, Delta, Result)
                             : llvm::SubOverflow(*Base, Delta, Result))
    return std::nullopt;
  if (TRI.PointerSize == 4 && Result != int64_t(int32_t(Result)))
    return std::nullopt;
  return Result;
}

// Earlier pipeline consumers still operate on MedIR SSA identities. Source
// passes supply their own resolver after renaming, using source-local identity.
inline std::optional<int64_t>
frameAddressOffset(const ExprPtr &E, const HighFunc &Func, Arch Architecture,
                   size_t &Budget, unsigned Depth,
                   const VarKeyMap<ExprPtr> *Aliases) {
  const auto Alias = [&](const MedVar &V) -> ExprPtr {
    if (!Aliases)
      return nullptr;
    const auto It = Aliases->find(varKey(V));
    return It == Aliases->end() ? nullptr : It->second;
  };
  return frameAddressOffset(E, Func, Architecture, Budget, Depth, Alias);
}
} // namespace neverd::high_detail
#endif
