//===- HighCCallSetup.cpp - x86 outgoing call stack projection ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Project outgoing argument setup without hiding live stack addresses.
//===----------------------------------------------------------------------===//

#include "../HighCWriter.h"

#include "neverd/backend/Limits.h"

#include <functional>

namespace neverd {

void HighCWriter::hideX86CallPushSetup(const HighFunc &Func) {
  if (Opts.TheArch != Arch::X86)
    return;

  auto Peel = [this](const HighExpr *E) { return unwrapIntegerView(E); };
  auto IsImm = [&](const HighExpr *E, uint64_t Val) {
    E = Peel(E);
    return E && E->Kind == ExprKind::Const && E->ConstVal == Val;
  };
  auto IsSubImm = [&](const HighExpr *E, uint64_t Imm) {
    E = Peel(E);
    return E && E->Kind == ExprKind::BinOp && E->Op == NdOp::INT_SUB &&
           E->Operands.size() == 2 && E->Operands[0] && E->Operands[1] &&
           IsImm(E->Operands[1].get(), Imm);
  };
  auto SameScalar = [&](const HighExpr *A, const HighExpr *B) {
    A = Peel(A);
    B = Peel(B);
    if (!A || !B)
      return false;
    if (A->Kind == ExprKind::Const && B->Kind == ExprKind::Const)
      return A->ConstVal == B->ConstVal;
    if ((A->Kind == ExprKind::Var || A->Kind == ExprKind::Phi) &&
        A->Kind == B->Kind)
      return A->Var.Kind == B->Var.Kind && A->Var.Id == B->Var.Id &&
             A->Var.SSAVer == B->Var.SSAVer;
    return false;
  };
  auto MatchesCallOperand = [&](const HighExpr *Val, const HighExpr &Call) {
    if (!Val)
      return false;
    for (const auto &Op : Call.Operands)
      if (SameScalar(Val, Op.get()))
        return true;
    return false;
  };
  auto IsPushStore = [&](const HighStmt &S, const HighExpr &Call) {
    if (S.Kind != StmtKind::Store || !S.StoreAddr || !S.StoreVal)
      return false;
    if (S.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
    if (namedFrameSlot(*S.StoreAddr) || typedMemberAccess(*S.StoreAddr))
      return false;
    if (!IsSubImm(S.StoreAddr.get(), 4))
      return false;
    const HighExpr *Val = Peel(S.StoreVal.get());
    if (!Val)
      return false;
    if (Val->Kind == ExprKind::Const || Val->Kind == ExprKind::Undef)
      return true;
    return MatchesCallOperand(S.StoreVal.get(), Call);
  };
  auto IsEspSubAssign = [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return false;
    if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
      return false;
    if (S.Dst->Var.Kind == MedVar::Param)
      return false;
    if (namedFrameSlot(*S.Dst))
      return false;
    return IsSubImm(S.Val.get(), 4);
  };
  auto IsImageLoadAssign = [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return false;
    if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
      return false;
    const HighExpr *Val = Peel(S.Val.get());
    if (!Val || Val->Kind != ExprKind::Load || Val->Operands.empty() ||
        !Val->Operands[0])
      return false;
    const HighExpr *Addr = Peel(Val->Operands[0].get());
    return Addr && Addr->Kind == ExprKind::Const;
  };

  // A callee-pop call can leave a stack value live in its continuation or
  // another EH clause. Record all readers before hiding outgoing argument
  // setup; inspecting just the setup run loses those definitions.
  std::map<std::string, std::set<const HighStmt *>> Readers;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    HighExprSet Seen;
    std::vector<ExprPtr> Work;
    forEachExpr(S, [&](const ExprPtr &E) {
      if (E && (E != S.Dst ||
                (E->Kind != ExprKind::Var && E->Kind != ExprKind::Phi)))
        Work.push_back(E);
    });
    while (!Work.empty()) {
      const auto E = Work.back();
      Work.pop_back();
      if (!Seen.insert(E.get()).second)
        continue;
      if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi)
        Readers[varName(E->Var)].insert(&S);
      E->forEachChildExpr([&](const ExprPtr &Child) {
        if (Child)
          Work.push_back(Child);
      });
    }
  });

  std::function<void(const std::vector<HighStmt> &)> Walk =
      [&](const std::vector<HighStmt> &Body) {
        for (const HighStmt &S : Body) {
          Walk(S.Body);
          Walk(S.ElseBody);
          Walk(S.DefaultBody);
          for (const auto &C : S.Cases)
            Walk(C.Body);
          for (const auto &Clause : S.EHClauseBodies)
            Walk(Clause);
        }
        for (size_t I = 0; I < Body.size(); ++I) {
          const auto &S = Body[I];
          const HighExpr *Call =
              S.Kind == StmtKind::Call ? S.CallExpr.get()
              : (S.Kind == StmtKind::Assign || S.Kind == StmtKind::ExprStmt)
                  ? S.Val.get()
                  : nullptr;
          if (!Call || Call->Kind != ExprKind::Call || Call->Operands.empty())
            continue;
          std::vector<const HighStmt *> Run;
          bool Matched = false;
          for (size_t J = I;
               J > 0 && Run.size() < ::neverd::limits::backend::
                                         kHighCCallSetupWalkRunSizeLimit;) {
            --J;
            const HighStmt &P = Body[J];
            if (P.Kind == StmtKind::Nop ||
                (P.Kind == StmtKind::Block && P.Body.empty()))
              continue;
            if (IsImageLoadAssign(P)) {
              if (MatchesCallOperand(P.Dst.get(), *Call))
                Run.push_back(&P);
              continue;
            }
            if (IsPushStore(P, *Call)) {
              if (MatchesCallOperand(P.StoreVal.get(), *Call))
                Matched = true;
              Run.push_back(&P);
              continue;
            }
            if (IsEspSubAssign(P)) {
              Run.push_back(&P);
              continue;
            }
            break;
          }
          if (!Matched)
            continue;
          std::set<const HighStmt *> Hidden(Run.begin(), Run.end());
          // Every retained reader needs its definitions: the call reads an
          // image-loaded argument just as its continuation reads a stack
          // address. Retaining either can make further inputs live.
          bool Changed;
          do {
            Changed = false;
            for (const HighStmt *P : Run) {
              if (!Hidden.count(P) || P->Kind != StmtKind::Assign)
                continue;
              for (const HighStmt *Reader : Readers[varName(P->Dst->Var)])
                if (!Hidden.count(Reader)) {
                  Hidden.erase(P);
                  Changed = true;
                  break;
                }
            }
          } while (Changed);
          Analysis.DeadStmts.insert(Hidden.begin(), Hidden.end());
        }
      };
  Walk(Func.Body);
}

} // namespace neverd
