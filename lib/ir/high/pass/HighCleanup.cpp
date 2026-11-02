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

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <set>
#include <vector>

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

namespace {

/// True when dropping an assignment of \p E loses no effect: no call, store,
/// ordered access or segmented memory access (the rule DCE applies).
bool valueIsDisposable(const ExprPtr &E) {
  if (!E)
    return true;
  if (E->Kind == ExprKind::Call || E->Kind == ExprKind::Store ||
      E->MemoryOrdering != NdMemoryOrdering::None ||
      E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  return std::all_of(E->Operands.begin(), E->Operands.end(), valueIsDisposable);
}

} // namespace

void MedToHighConverter::stripPrologueEpilogue(HighFunc &Func) {
  const auto &TRI = getTargetRegInfo(TargetArch);
  const std::set<va_t> BranchTargets = gotoTargets(Func.Body);

  auto IsSPReg = [&TRI](const MedVar &V) -> bool {
    return V.Kind == MedVar::Reg && TRI.isStackPointer(V.RegOff);
  };
  auto IsFPReg = [&TRI](const MedVar &V) -> bool {
    return V.Kind == MedVar::Reg && TRI.isFramePointer(V.RegOff);
  };
  auto IsLRReg = [&TRI](const MedVar &V) -> bool {
    return V.Kind == MedVar::Reg && TRI.isLinkRegister(V.RegOff);
  };
  size_t FrameBudget = ::neverd::limits::ir::kHighCleanupFrameBudget;
  // A register's entry value is a version no statement assigns.  A view's
  // first definition is version 0 as well (`BX = (uint16_t)EBX` before a
  // `movw %bx` store into a local), and that value is the function's own.
  VarKeyMap<bool> Assigned;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst &&
        (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi) &&
        !(S.Val && S.Val->Kind == ExprKind::Var && S.Val->Var == S.Dst->Var))
      Assigned[varKey(S.Dst->Var)] = true;
  });
  // Only an unchanged entry value stored to memory can be prologue storage
  // (IsCalleeSaveStore below); without one, no read needs collecting.
  bool SavesEntryValue = false;
  if (Func.FrameSize > 0)
    walkStmts(Func.Body, [&](const HighStmt &S) {
      SavesEntryValue |=
          S.Kind == StmtKind::Store && S.StoreVal &&
          S.StoreVal->Kind == ExprKind::Var && S.StoreVal->Var.SSAVer == 0 &&
          S.StoreVal->Var.RenameTag < 0 && S.StoreVal->Var.Size &&
          !Assigned.count(varKey(S.StoreVal->Var));
    });
  // Preservation is a byte-range property: AArch64 saves D8-D15, while
  // their Q-register upper halves remain volatile. Share the ABI predicate
  // used by call liveness instead of the integer-only register list.
  const BinaryFormat Convention = CurMed && CurMed->CC == CallingConv::Win64
                                      ? BinaryFormat::COFF
                                      : BinaryFormat::ELF;
  // Win64 also hands the callee its register arguments' home slots, above the
  // return address: MSVC saves nonvolatile registers there (`mov [rsp+8],
  // rbx`), and the caller reads nothing back from them.
  const IntegerArgumentLayout Layout =
      TRI.integerArgumentLayout(Convention == BinaryFormat::COFF);
  const int64_t HomeBegin = Layout.EntryStackBase - Layout.CallStackBase;
  // The frame bytes each read takes, and the register a restore puts them
  // back into: a save slot read for anything else keeps its store.
  struct FrameRead {
    int64_t Offset = 0;
    uint16_t Size = 0;
    std::optional<MedVar> Restores;
  };
  std::vector<FrameRead> FrameReads;
  VarKeySet ReadValues;
  HighExprSet ReadExpressions;
  if (SavesEntryValue)
    walkStmts(Func.Body, [&](const HighStmt &S) {
      const auto Read = [&](const ExprPtr &Root) {
        std::vector<ExprPtr> Pending{Root};
        while (!Pending.empty() && FrameBudget) {
          auto E = Pending.back();
          Pending.pop_back();
          --FrameBudget;
          if (!E || !ReadExpressions.insert(E.get()).second)
            continue;
          if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi)
            ReadValues.insert(varKey(E->Var));
          E->forEachChildExpr(
              [&](const ExprPtr &Child) { Pending.push_back(Child); });
        }
      };
      forEachRhsExpr(S, Read);
      // A store-form assignment reads its destination's address.
      if (S.Dst && S.Dst->Kind == ExprKind::Load)
        for (const ExprPtr &Operand : S.Dst->Operands)
          Read(Operand);
    });
  if (SavesEntryValue)
    walkStmts(Func.Body, [&](const HighStmt &S) {
      const HighExpr *Restore = S.Kind == StmtKind::Assign && S.Dst &&
                                        S.Dst->Kind == ExprKind::Var &&
                                        S.Dst->Var.Kind == MedVar::Reg &&
                                        S.Val && S.Val->Kind == ExprKind::Load
                                    ? S.Val.get()
                                    : nullptr;
      std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
        if (!E)
          return;
        if (E->Kind == ExprKind::Load && !E->Operands.empty() && E->Type &&
            E->Type->Size)
          if (const auto At = high_detail::frameAddressOffset(
                  forceInlineExpr(E->Operands[0]), Func, TargetArch,
                  FrameBudget)) {
            FrameRead Read{*At, E->Type->Size, std::nullopt};
            if (E.get() == Restore && !ReadValues.count(varKey(S.Dst->Var)))
              Read.Restores = S.Dst->Var;
            FrameReads.push_back(std::move(Read));
          }
        E->forEachChildExpr(Visit);
      };
      forEachExpr(S, Visit);
    });
  auto OnlyRestored = [&](int64_t Offset, const MedVar &V) {
    if (!FrameBudget)
      return false;
    return llvm::all_of(FrameReads, [&](const FrameRead &Read) {
      if (Read.Offset >= Offset + V.Size || Offset >= Read.Offset + Read.Size)
        return true;
      return Read.Restores && Read.Restores->RegOff == V.RegOff &&
             Read.Offset == Offset && Read.Size == V.Size;
    });
  };
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
    if (V.SSAVer != 0 || V.RenameTag >= 0 || Func.FrameSize <= 0 || !V.Size ||
        Assigned.count(varKey(V)))
      return false;
    const auto Offset = high_detail::frameAddressOffset(
        forceInlineExpr(S.StoreAddr), Func, TargetArch, FrameBudget);
    if (!Offset)
      return false;
    const int64_t End = *Offset + V.Size;
    const bool OwnFrame = *Offset >= -Func.FrameSize && End <= 0;
    const bool Home = *Offset >= HomeBegin && End <= Layout.EntryStackBase;
    // An ABI-preserved register can also carry ordinary data. A private
    // spill is disposable only when every read is an unobserved restore,
    // exactly as for a home slot above the entry SP. Otherwise deleting its
    // initializer leaves a later source load undefined.
    if ((!OwnFrame && !Home) || !OnlyRestored(*Offset, V))
      return false;
    if (OwnFrame && (IsFPReg(V) || IsLRReg(V)))
      return true;
    return V.Kind == MedVar::Reg &&
           TRI.isCallPreserved(V.RegOff, V.Size, Convention);
  };

  if (SavesEntryValue)
    eraseKeepingBranchEntries(Func.Body, BranchTargets, IsCalleeSaveStore);

  std::set<std::string> UsedFrameVars;
  std::function<void(const ExprPtr &)> CollectVarRefs;
  CollectVarRefs = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (E->Kind == ExprKind::Var &&
        (IsFPReg(E->Var) || IsSPReg(E->Var) || IsLRReg(E->Var)))
      UsedFrameVars.insert(E->str());
    E->forEachChildExpr(CollectVarRefs);
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
    if (S.SwitchExpr)
      CollectVarRefs(S.SwitchExpr);
    // Every nested body the strip below visits: a read inside a switch case
    // keeps its value as much as one inside an if arm.
    for (auto &Inner : S.Body)
      CollectStmtRefsPE(Inner);
    for (auto &Inner : S.ElseBody)
      CollectStmtRefsPE(Inner);
    for (auto &C : S.Cases)
      for (auto &Inner : C.Body)
        CollectStmtRefsPE(Inner);
    for (auto &Inner : S.DefaultBody)
      CollectStmtRefsPE(Inner);
    for (auto &Clause : S.EHClauseBodies)
      for (auto &Inner : Clause)
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
      // RBP is also a general callee-saved register (MSVC x64): keep a
      // definition whose value still has an effect, such as a call.
      if (S.Dst->Kind == ExprKind::Var && IsFPReg(S.Dst->Var))
        return UsedFrameVars.count(S.Dst->str()) == 0 &&
               valueIsDisposable(S.Val);
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
    // Once the prologue saved them, the link and frame registers are general
    // registers too (`mla lr, r12, r2, r1`): only a value no statement reads
    // is the epilogue's.
    if (IsLRReg(S.Dst->Var) || IsFPReg(S.Dst->Var))
      return UsedFrameVars.count(S.Dst->str()) == 0 && valueIsDisposable(S.Val);
    return false;
  };

  std::function<void(std::vector<HighStmt> &)> StripRecursive;
  StripRecursive = [&](std::vector<HighStmt> &Stmts) {
    eraseKeepingBranchEntries(Stmts, BranchTargets, [&](const HighStmt &S) {
      return IsPrologueEpilogue(S) || IsEpilogueLoad(S);
    });
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
  if ((Last.Kind == StmtKind::Call && isTerminatingHighCall(Last.CallExpr)) ||
      ((Last.Kind == StmtKind::Assign || Last.Kind == StmtKind::ExprStmt) &&
       isTerminatingHighCall(Last.Val)))
    return;

  // A verified void source ABI owns the return contract even when imperfect
  // structuring leaves a fallthrough after an earlier native RET.  Mirror
  // lowerReturn: append a bare source return instead of searching X0/RAX and
  // reviving an undefined machine carrier as a synthetic result.
  if (Med.SourceParametersBound && Func.SourceTypeHint &&
      Func.SourceTypeHint->HasExplicitABI && Func.SourceTypeHint->ReturnType &&
      Func.SourceTypeHint->ReturnType->Kind == NdTypeKind::Void &&
      Func.ReturnType && Func.ReturnType->Kind == NdTypeKind::Void) {
    HighStmt RetStmt;
    RetStmt.Kind = StmtKind::Return;
    Func.Body.push_back(std::move(RetStmt));
    return;
  }

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
