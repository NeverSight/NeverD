//===- X86RegistrationFilter.cpp - Native x86 filter values --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recover only a bounded, pure return expression in the native parent frame.
//===----------------------------------------------------------------------===//

#include "X86RegistrationFilter.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedStackAlignment.h"

#include <functional>
#include <map>

namespace neverd {

/// The value an x86 filter thunk returns when that is all it computes:
/// `return E;`, or `t = E; return t;`.  E may read only memory, constants and
/// the frame, the one register the personality enters a filter with; each
/// frame register becomes the entry stack pointer plus its proven offset, the
/// way the function's own frame accesses are written.
ExprPtr registrationFilterValue(const std::vector<HighStmt> &Body,
                                const MedFunc &Med) {
  std::vector<const HighStmt *> Live;
  for (const HighStmt &S : Body)
    if (S.Kind != StmtKind::Nop &&
        !(S.Kind == StmtKind::Block && S.Body.empty()))
      Live.push_back(&S);
  if (Live.empty() || Live.size() > 2 ||
      Live.back()->Kind != StmtKind::Return || !Live.back()->RetVal)
    return nullptr;
  ExprPtr Value = Live.back()->RetVal;
  if (Live.size() == 2) {
    const HighStmt &Def = *Live.front();
    if (Def.Kind != StmtKind::Assign || !Def.Dst || !Def.Val ||
        Def.Dst->Kind != ExprKind::Var || Value->Kind != ExprKind::Var ||
        varKey(Def.Dst->Var) != varKey(Value->Var))
      return nullptr;
    Value = Def.Val;
  }
  size_t Budget =
      ::neverd::limits::ir::kX86RegistrationFilterRegistrationFilterValueBudget;
  std::map<const HighExpr *, ExprPtr> Rebuilt;
  std::function<ExprPtr(const ExprPtr &)> Rebuild =
      [&](const ExprPtr &E) -> ExprPtr {
    if (!E || !Budget)
      return nullptr;
    --Budget;
    if (auto It = Rebuilt.find(E.get()); It != Rebuilt.end())
      return It->second;
    ExprPtr Result;
    switch (E->Kind) {
    case ExprKind::Const:
      Result = E;
      break;
    case ExprKind::Var: {
      const TargetRegInfo &TRI = getTargetRegInfo(E->Var.TheArch);
      if (E->Var.Kind != MedVar::Reg || E->Var.Size != TRI.PointerSize ||
          (E->Var.RegOff != TRI.StackPointer &&
           E->Var.RegOff != TRI.FramePointer))
        return nullptr;
      const std::optional<int64_t> Offset =
          entryStackOffset(Med, E->Var, E->Var.TheArch, BinaryFormat::COFF);
      if (!Offset)
        return nullptr;
      MedVar EntrySP;
      EntrySP.Kind = MedVar::Reg;
      EntrySP.TheArch = E->Var.TheArch;
      EntrySP.RegOff = TRI.StackPointer;
      EntrySP.Size = TRI.PointerSize;
      Result = HighExpr::makeVar(EntrySP, E->Type);
      if (*Offset != 0) {
        const bool Down = *Offset < 0;
        Result = HighExpr::makeBinop(
            Down ? NdOp::INT_SUB : NdOp::INT_ADD, Result,
            HighExpr::makeConst(
                static_cast<uint64_t>(Down ? -*Offset : *Offset),
                TRI.PointerSize, ConstantAddressProvenance::Scalar));
        Result->Type = E->Type;
      }
      break;
    }
    case ExprKind::Load:
      if (E->MemoryOrdering != NdMemoryOrdering::None ||
          E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return nullptr;
      [[fallthrough]];
    case ExprKind::BinOp:
    case ExprKind::UnaryOp:
    case ExprKind::Cast:
    case ExprKind::BitCast: {
      auto Copy = std::make_shared<HighExpr>(*E);
      for (ExprPtr &Operand : Copy->Operands)
        if (!(Operand = Rebuild(Operand)))
          return nullptr;
      Result = Copy;
      break;
    }
    default:
      return nullptr;
    }
    Rebuilt.emplace(E.get(), Result);
    return Result;
  };
  return Rebuild(Value);
}

} // namespace neverd
