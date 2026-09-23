//===- HighCleanup.cpp - HighIR cleanup passes -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// HighIR cleanup passes: prologue/epilogue removal, branch entry
/// coalescing, and trailing return insertion. The main DCE logic lives
/// in HighDCE.cpp.
///
//===----------------------------------------------------------------------===//

#include "HighDCEDetail.h"
#include "HighFrameAddress.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"

#include "llvm/Support/Debug.h"

#include <algorithm>
#include <functional>
#include <set>

#define DEBUG_TYPE "neverd-high-cleanup"

namespace neverd {

void coalesceBranchEntryStatements(std::vector<HighStmt> &Stmts) {
  std::set<va_t> Targets;
  walkStmts(Stmts, [&](const HighStmt &Statement) {
    if (Statement.Kind == StmtKind::Goto && Statement.GotoTarget &&
        Statement.GotoTarget != InvalidVA)
      Targets.insert(Statement.GotoTarget);
  });
  if (Targets.empty())
    return;
  std::function<void(std::vector<HighStmt> &)> Group = [&](auto &Body) {
    for (auto &Statement : Body) {
      if ((Statement.Kind == StmtKind::If ||
           Statement.Kind == StmtKind::IfElse) &&
          Statement.Cond && Targets.count(Statement.Addr)) {
        // Edge copies carry their predecessor branch's instruction address,
        // but entry at that address must first evaluate the branch. They are
        // not independent native labels inside the chosen arm. Only clear
        // the direct prefix belonging to this exact conditional; a later or
        // non-PHI occurrence remains a distinct, possibly ambiguous entry.
        auto ClearEdgeEntries = [&](auto &Arm) {
          for (auto &Copy : Arm) {
            if (Copy.Kind != StmtKind::Assign || !Copy.IsPhiCopy ||
                Copy.Addr != Statement.Addr || !Copy.Dst || !Copy.Val ||
                Copy.Dst->Kind != ExprKind::Var || !Copy.Body.empty() ||
                !Copy.ElseBody.empty() || !Copy.Cases.empty() ||
                !Copy.DefaultBody.empty() || !Copy.EHClauseBodies.empty())
              break;
            Copy.Addr = 0;
          }
        };
        ClearEdgeEntries(Statement.Body);
        ClearEdgeEntries(Statement.ElseBody);
      }
      Group(Statement.Body);
      Group(Statement.ElseBody);
      for (auto &Case : Statement.Cases)
        Group(Case.Body);
      Group(Statement.DefaultBody);
      for (auto &Clause : Statement.EHClauseBodies)
        Group(Clause);
    }
    std::vector<HighStmt> Result;
    Result.reserve(Body.size());
    for (size_t I = 0; I < Body.size();) {
      size_t End = I + 1;
      const va_t Address = Body[I].Addr;
      if (Targets.count(Address))
        while (End < Body.size() && Body[End].Addr == Address)
          ++End;
      if (End == I + 1) {
        Result.push_back(std::move(Body[I++]));
        continue;
      }
      // A native instruction and its edge copies share one source entry.
      // Group only adjacent statements in this scope, after all structuring
      // has consumed their original addresses. Entering the label executes
      // the complete sequence in order; separated duplicates stay ambiguous.
      HighStmt Entry;
      Entry.Kind = StmtKind::Block;
      Entry.Addr = Address;
      Entry.Body.reserve(End - I);
      for (; I < End; ++I) {
        Body[I].Addr = 0;
        Entry.Body.push_back(std::move(Body[I]));
      }
      Result.push_back(std::move(Entry));
    }
    Body = std::move(Result);
  };
  Group(Stmts);
}

void coalesceBranchEntryStatements(HighFunc &Func) {
  coalesceBranchEntryStatements(Func.Body);
}

//===----------------------------------------------------------------------===//
// Prologue / epilogue stripping
//===----------------------------------------------------------------------===//

void MedToHighConverter::stripPrologueEpilogue(HighFunc &Func) {
  const auto &TRI = getTargetRegInfo(TargetArch);

  auto IsSPReg = [&TRI](const MedVar &V) -> bool {
    return V.Kind == MedVar::Reg && TRI.isStackPointer(V.RegOff);
  };
  auto IsFPReg = [&TRI](const MedVar &V) -> bool {
    return V.Kind == MedVar::Reg && TRI.isFramePointer(V.RegOff);
  };
  auto IsLRReg = [&TRI](const MedVar &V) -> bool {
    return V.Kind == MedVar::Reg && TRI.isLinkRegister(V.RegOff);
  };
  size_t FrameBudget = 100000;
  auto IsCalleeSaveStore = [&](const HighStmt &S) -> bool {
    if (S.Kind != StmtKind::Store || !S.StoreVal)
      return false;
    if (S.MemoryOrdering != NdMemoryOrdering::None ||
        S.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        S.StoreVal->hasOrderedMemoryAccess())
      return false;
    if (S.StoreVal->Kind != ExprKind::Var)
      return false;
    const auto &V = S.StoreVal->Var;
    // A register's ABI role does not make its computed values disposable.
    // Only the unchanged entry value saved into this function's own frame is
    // prologue storage; writes through arguments/globals remain observable.
    if (V.SSAVer != 0 || V.RenameTag >= 0 || Func.FrameSize <= 0 || !V.Size)
      return false;
    const auto Offset = high_detail::frameAddressOffset(
        forceInlineExpr(S.StoreAddr), Func, TargetArch, FrameBudget);
    if (!Offset || *Offset < -Func.FrameSize || *Offset >= 0 ||
        uint64_t(V.Size) > uint64_t(-*Offset))
      return false;
    if (IsFPReg(V) || IsLRReg(V))
      return true;
    // Preservation is a byte-range property: AArch64 saves D8-D15, while
    // their Q-register upper halves remain volatile. Share the ABI predicate
    // used by call liveness instead of the integer-only register list.
    const BinaryFormat Convention = CurMed && CurMed->CC == CallingConv::Win64
                                        ? BinaryFormat::COFF
                                        : BinaryFormat::ELF;
    return V.Kind == MedVar::Reg &&
           TRI.isCallPreserved(V.RegOff, V.Size, Convention);
  };

  Func.Body.erase(
      std::remove_if(Func.Body.begin(), Func.Body.end(), IsCalleeSaveStore),
      Func.Body.end());

  std::set<std::string> UsedFrameVars;
  std::function<void(const ExprPtr &)> CollectVarRefs;
  CollectVarRefs = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (E->Kind == ExprKind::Var && (IsFPReg(E->Var) || IsSPReg(E->Var)))
      UsedFrameVars.insert(E->str());
    for (auto &Op : E->Operands)
      CollectVarRefs(Op);
  };
  std::function<void(const HighStmt &)> CollectStmtRefsPE;
  CollectStmtRefsPE = [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Val)
      CollectVarRefs(S.Val);
    if (S.Kind == StmtKind::Store) {
      CollectVarRefs(S.StoreAddr);
      CollectVarRefs(S.StoreVal);
    }
    if (S.Cond)
      CollectVarRefs(S.Cond);
    if (S.RetVal)
      CollectVarRefs(S.RetVal);
    if (S.CallExpr)
      CollectVarRefs(S.CallExpr);
    for (auto &Inner : S.Body)
      CollectStmtRefsPE(Inner);
    for (auto &Inner : S.ElseBody)
      CollectStmtRefsPE(Inner);
  };
  for (auto &S : Func.Body)
    CollectStmtRefsPE(S);

  auto IsPrologueEpilogue = [&](const HighStmt &S) -> bool {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val) {
      if (S.Val->hasOrderedMemoryAccess())
        return false;
      if (S.Dst->Kind == ExprKind::Var && IsSPReg(S.Dst->Var) &&
          S.Val->Kind == ExprKind::BinOp &&
          (S.Val->Op == NdOp::INT_SUB || S.Val->Op == NdOp::INT_ADD) &&
          S.Val->Operands.size() == 2 &&
          S.Val->Operands[0]->Kind == ExprKind::Var &&
          IsSPReg(S.Val->Operands[0]->Var) &&
          S.Val->Operands[1]->Kind == ExprKind::Const) {
        return UsedFrameVars.count(S.Dst->str()) == 0;
      }
      if (S.Dst->Kind == ExprKind::Var && IsFPReg(S.Dst->Var))
        return UsedFrameVars.count(S.Dst->str()) == 0;
      // A temporary derived from SP may address live local storage, including
      // block captures. Its origin does not make its definition a prologue.
      // The following liveness-based DCE removes unused address computations.
    }
    return IsCalleeSaveStore(S);
  };

  auto IsEpilogueLoad = [&](const HighStmt &S) -> bool {
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return false;
    if (S.Dst->Kind != ExprKind::Var)
      return false;
    if (S.Val->hasOrderedMemoryAccess())
      return false;
    if (IsLRReg(S.Dst->Var))
      return true;
    if (IsFPReg(S.Dst->Var))
      return UsedFrameVars.count(S.Dst->str()) == 0;
    return false;
  };

  std::function<void(std::vector<HighStmt> &)> StripRecursive;
  StripRecursive = [&](std::vector<HighStmt> &Stmts) {
    Stmts.erase(std::remove_if(Stmts.begin(), Stmts.end(),
                               [&](const HighStmt &S) {
                                 return IsPrologueEpilogue(S) ||
                                        IsEpilogueLoad(S);
                               }),
                Stmts.end());
    for (auto &S : Stmts) {
      StripRecursive(S.Body);
      StripRecursive(S.ElseBody);
      for (auto &C : S.Cases)
        StripRecursive(C.Body);
      StripRecursive(S.DefaultBody);
    }
  };
  StripRecursive(Func.Body);

  // Prologue cleanup cannot remove a return based on a lexical predecessor.
  // A later block may have its own incoming edge and a different return value;
  // reachability cleanup owns elimination after considering those edges.
}

//===----------------------------------------------------------------------===//
// Ensure trailing return
//===----------------------------------------------------------------------===//

void MedToHighConverter::ensureTrailingReturn(HighFunc &Func,
                                              const MedFunc &Med) {
  if (Med.DoesNotReturn)
    return;
  if (!Func.Body.empty() && Func.Body.back().Kind == StmtKind::Return)
    return;
  if (Func.Body.empty())
    return;
  const auto &Last = Func.Body.back();
  if ((Last.Kind == StmtKind::Call &&
       isNonReturningSourceCall(Last.CallExpr)) ||
      ((Last.Kind == StmtKind::Assign || Last.Kind == StmtKind::ExprStmt) &&
       isNonReturningSourceCall(Last.Val)))
    return;

  ExprPtr RetExpr;

  for (auto &Blk : Med.Blocks) {
    bool HasRet = false;
    for (auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::RETURN) {
        HasRet = true;
        break;
      }
    if (!HasRet)
      continue;

    for (auto Rit = Blk.Ops.rbegin(); Rit != Blk.Ops.rend(); ++Rit) {
      if (Rit->Opcode == NdOp::RETURN)
        continue;
      if (Rit->Output.Kind == MedVar::Reg && Rit->Output.RegOff == 0 &&
          Rit->Output.Size > 0) {
        if (Rit->Opcode == NdOp::INT_ZEXT && Rit->NumInputs >= 1 &&
            Rit->Inputs[0].Kind == MedVar::Reg)
          RetExpr = HighExpr::makeVar(Rit->Inputs[0]);
        else
          RetExpr = HighExpr::makeVar(Rit->Output);
        break;
      }
    }
    if (!RetExpr) {
      for (auto &Phi : Blk.Phis) {
        if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == 0) {
          RetExpr = HighExpr::makeVar(Phi.Output);
          break;
        }
      }
    }
    if (RetExpr) {
      RetExpr = forceInlineExpr(RetExpr);
      break;
    }
  }

  if (!RetExpr) {
    for (int I = static_cast<int>(Func.Body.size()) - 1; I >= 0; --I) {
      auto &S = Func.Body[I];
      if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var &&
          S.Dst->Var.Kind == MedVar::Reg && S.Dst->Var.RegOff == 0) {
        RetExpr = S.Dst;
        break;
      }
    }
  }

  HighStmt RetStmt;
  RetStmt.Kind = StmtKind::Return;
  RetStmt.RetVal = RetExpr;
  Func.Body.push_back(std::move(RetStmt));
}

} // namespace neverd
