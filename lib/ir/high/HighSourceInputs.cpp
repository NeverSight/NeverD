//===- HighSourceInputs.cpp - Entry-only scalar read proofs --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove single entry reads without moving them across competing effects.
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/HighSourceInputs.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/high/AArch64AddressMemory.h"

namespace neverd {
namespace {
bool plain(const HighExpr &Expression) {
  return Expression.IntrinsicId == Intrinsic::None &&
         Expression.IntrinsicOutputs.empty() &&
         Expression.MemoryOrdering == NdMemoryOrdering::None &&
         Expression.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         !Expression.IsIndirectCall && Expression.IndirectParamIdx < 0 &&
         !Expression.IndirectTarget;
}

bool scalarEntryType(const TypeRef &Type) {
  return Type &&
         ((Type->Kind == NdTypeKind::Int && Type->Size && Type->Size <= 8) ||
          (Type->Kind == NdTypeKind::Ptr && Type->Size == 8));
}

bool entryConversion(const HighExpr &Expression) {
  if (Expression.Operands.size() != 1 || !Expression.Operands[0])
    return false;
  if (Expression.Kind == ExprKind::Cast)
    return scalarEntryType(Expression.Type);
  const auto &Input = Expression.Operands[0]->Type;
  return Expression.Kind == ExprKind::UnaryOp &&
         (Expression.Op == NdOp::INT_ZEXT || Expression.Op == NdOp::INT_SEXT) &&
         scalarEntryType(Expression.Type) && scalarEntryType(Input) &&
         Expression.Type->Kind == NdTypeKind::Int &&
         Input->Kind == NdTypeKind::Int && Expression.Type->Size >= Input->Size;
}

bool pureEntryValue(const ExprPtr &Expression, size_t &Budget,
                    unsigned Depth = 0) {
  if (!Expression || !Budget ||
      Depth > ::neverd::limits::ir::kHighSourceInputsPureEntryValueDepthLimit ||
      !plain(*Expression) || !scalarEntryType(Expression->Type))
    return false;
  --Budget;
  if (Expression->Kind == ExprKind::Var || Expression->Kind == ExprKind::Const)
    return Expression->Operands.empty();
  if (entryConversion(*Expression))
    return pureEntryValue(Expression->Operands[0], Budget, Depth + 1);
  return Expression->Kind == ExprKind::BinOp &&
         (Expression->Op == NdOp::INT_ADD || Expression->Op == NdOp::INT_SUB) &&
         Expression->Operands.size() == 2 &&
         pureEntryValue(Expression->Operands[0], Budget, Depth + 1) &&
         pureEntryValue(Expression->Operands[1], Budget, Depth + 1);
}

const HighExpr *directEntryLoad(const ExprPtr &Value, unsigned Parameter) {
  if (Value->Kind != ExprKind::Load || !Value->Type ||
      Value->Type->Kind != NdTypeKind::Int ||
      (Value->Type->Size != 4 && Value->Type->Size != 8) ||
      Value->Operands.size() != 1)
    return nullptr;
  auto Address = Value->Operands[0];
  while (Address && Address->Kind == ExprKind::Cast && plain(*Address) &&
         scalarEntryType(Address->Type) && Address->Type->Size == 8 &&
         Address->Operands.size() == 1)
    Address = Address->Operands[0];
  if (!Address || Address->Kind != ExprKind::Var || !plain(*Address) ||
      !Address->Operands.empty() || Address->Var.Kind != MedVar::Param ||
      Address->Var.Id != static_cast<int>(Parameter) || Address->Var.SSAVer ||
      Address->Var.RenameTag >= 0 || !scalarEntryType(Address->Type) ||
      Address->Type->Size != 8 || Address->Var.Size != 8)
    return nullptr;
  return Value.get();
}

/// Follow only unconditional dependencies. A surrounding read or direct call
/// takes its address/arguments before performing its own observable operation;
/// every sibling must be free of memory accesses and other effects.
const HighExpr *entryLoad(const ExprPtr &Expression, unsigned Parameter,
                          Arch Architecture, size_t &Budget,
                          unsigned Depth = 0) {
  if (!Expression || !Budget ||
      Depth > ::neverd::limits::ir::kHighSourceInputsEntryLoadDepthLimit)
    return nullptr;
  if (!plain(*Expression)) {
    if (Architecture != Arch::AArch64 || Expression->SourceCallHint ||
        Expression->IndirectParamIdx >= 0 ||
        !isAArch64AddressMemoryIntrinsic(Expression->IntrinsicId) ||
        !aarch64AddressMemoryHighShapeIsValid(*Expression))
      return nullptr;
    const auto Instruction = *aarch64AddressMemoryInstruction(
        Expression->Operands[kAddressMemoryWordOperand]->ConstVal);
    if (!Instruction.isLoad() || Instruction.stackBase())
      return nullptr;
    // The original read evaluates its inputs before performing the access.
    // Only a single dependency with pure siblings can expose an entry read;
    // the surrounding instruction remains effectful and is never replaced.
  }
  --Budget;
  if (Expression->Kind == ExprKind::Load) {
    if (!scalarEntryType(Expression->Type) || Expression->Operands.size() != 1)
      return nullptr;
    if (const auto *Load = directEntryLoad(Expression, Parameter))
      return Load;
    return entryLoad(Expression->Operands[0], Parameter, Architecture, Budget,
                     Depth + 1);
  }
  if (entryConversion(*Expression))
    return entryLoad(Expression->Operands[0], Parameter, Architecture, Budget,
                     Depth + 1);
  if ((Expression->Kind != ExprKind::BinOp ||
       !scalarEntryType(Expression->Type) ||
       (Expression->Op != NdOp::INT_ADD && Expression->Op != NdOp::INT_SUB) ||
       Expression->Operands.size() != 2) &&
      Expression->Kind != ExprKind::Call)
    return nullptr;
  for (size_t I = 0; I < Expression->Operands.size() && Budget; ++I) {
    bool PureSiblings = true;
    for (size_t J = 0; J < Expression->Operands.size(); ++J)
      if (I != J &&
          !pureEntryValue(Expression->Operands[J], Budget, Depth + 1)) {
        PureSiblings = false;
        break;
      }
    if (PureSiblings)
      if (const auto *Load = entryLoad(Expression->Operands[I], Parameter,
                                       Architecture, Budget, Depth + 1))
        return Load;
  }
  return nullptr;
}
} // namespace

TypeRef entryScalarLoadInput(const HighFunc &Function, unsigned Parameter) {
  if (Parameter >= Function.Params.size() || !Function.Params[Parameter].Type ||
      Function.Params[Parameter].Type->Kind != NdTypeKind::Ptr ||
      Function.Params[Parameter].Type->Size != 8 ||
      Function.Body.size() >
          ::neverd::limits::ir::
              kHighSourceInputsEntryScalarLoadInputFunctionBodySizeLimit ||
      Function.StructuredExceptionRegions ||
      Function.UnstructuredExceptionRegions)
    return {};
  const HighExpr *Load = nullptr;
  bool AtEntry = true;
  size_t Budget =
      ::neverd::limits::ir::kHighSourceInputsEntryScalarLoadInputBudget;
  unsigned Uses = 0;
  const auto Scan = [&](auto &&Self, const ExprPtr &Expression,
                        unsigned Depth) -> bool {
    if (!Expression || !Budget ||
        Depth > ::neverd::limits::ir::
                    kHighSourceInputsEntryScalarLoadInputScanScanDepthLimit)
      return false;
    --Budget;
    if (Expression->Kind == ExprKind::Var || Expression->Kind == ExprKind::Phi)
      if (Expression->Var.Kind == MedVar::Param &&
          Expression->Var.Id == static_cast<int>(Parameter) && ++Uses > 1)
        return false;
    if (Expression->IsIndirectCall &&
        Expression->IndirectParamIdx == static_cast<int>(Parameter) &&
        ++Uses > 1)
      return false;
    for (const auto &Output : Expression->IntrinsicOutputs)
      if (Output.Kind == MedVar::Param &&
          Output.Id == static_cast<int>(Parameter))
        return false;
    for (const auto &Operand : Expression->Operands)
      if (!Self(Self, Operand, Depth + 1))
        return false;
    if (Expression->IndirectTarget &&
        !Self(Self, Expression->IndirectTarget, Depth + 1))
      return false;
    return true;
  };
  for (const auto &Statement : Function.Body) {
    // There are no alternate entries or reexecuted loads in this subset.
    // Broader control flow needs a proof over HighSourceFlow, not lexical
    // order.
    if (!Statement.Body.empty() || !Statement.ElseBody.empty() ||
        !Statement.DefaultBody.empty() || !Statement.Cases.empty() ||
        !Statement.EHClauseBodies.empty() ||
        (Statement.Kind != StmtKind::Assign &&
         Statement.Kind != StmtKind::Store &&
         Statement.Kind != StmtKind::Call &&
         Statement.Kind != StmtKind::ExprStmt &&
         Statement.Kind != StmtKind::Return && Statement.Kind != StmtKind::Nop))
      return {};
    bool ValidUses = true;
    forEachExpr(Statement, [&](const ExprPtr &Expression) {
      ValidUses &= Scan(Scan, Expression, 0);
    });
    if (!ValidUses)
      return {};
    if (!AtEntry)
      continue;
    if (Statement.MemoryOrdering != NdMemoryOrdering::None ||
        Statement.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return {};
    if (Statement.Kind == StmtKind::Nop) {
      bool Empty = true;
      forEachExpr(Statement, [&](const ExprPtr &) { Empty = false; });
      if (!Empty)
        return {};
      continue;
    }
    if (Statement.Cond || Statement.StoreAddr || Statement.StoreVal ||
        Statement.SwitchExpr)
      return {};
    ExprPtr Value;
    if (Statement.Kind == StmtKind::Assign) {
      if (!Statement.Dst || Statement.Dst->Kind != ExprKind::Var ||
          !plain(*Statement.Dst) ||
          (Statement.Dst->Var.Kind != MedVar::Temp &&
           Statement.Dst->Var.Kind != MedVar::Reg) ||
          !Statement.Dst->Operands.empty() || Statement.RetVal ||
          Statement.CallExpr)
        return {};
      Value = Statement.Val;
    } else if (!Statement.Dst && Statement.Kind == StmtKind::Return &&
               !Statement.Val && !Statement.CallExpr) {
      Value = Statement.RetVal;
    } else if (!Statement.Dst && Statement.Kind == StmtKind::Call &&
               !Statement.Val && !Statement.RetVal) {
      Value = Statement.CallExpr;
    } else if (!Statement.Dst && Statement.Kind == StmtKind::ExprStmt &&
               !Statement.CallExpr && !Statement.RetVal) {
      Value = Statement.Val;
    } else {
      return {};
    }
    const auto Architecture = Function.SourceTypeHint
                                  ? Function.SourceTypeHint->Architecture
                                  : Arch::Unknown;
    if (const auto *Entry = entryLoad(Value, Parameter, Architecture, Budget)) {
      Load = Entry;
      AtEntry = false;
      continue;
    }
    if (Statement.Kind != StmtKind::Assign || !pureEntryValue(Value, Budget))
      return {};
  }
  return Load && Uses == 1 ? Load->Type : TypeRef{};
}
} // namespace neverd
