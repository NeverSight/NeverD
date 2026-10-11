//===- HighCFSimplify.cpp - Control-flow simplification for HighIR ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Simplifies the flat HighIR statement list into structured control
/// flow: the simplifyControlFlow driver, goto elimination, and proven
/// return-block inlining.
///
/// If/else structuring, loop detection and switch recovery are in separate
/// files:
///   HighCFSimplifyIfElse.cpp — if(cond){goto} → if/else tree folding
///   HighLoopRecovery.cpp     — backward-goto → while-loop conversion
///   HighIfChainToSwitch.cpp  — if-chain → switch recovery
///
//===----------------------------------------------------------------------===//

#include "HighCFSimplifyDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/support/Diagnostic.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>

namespace neverd {

//===----------------------------------------------------------------------===//
// Trivial goto removal (goto to next statement)
//===----------------------------------------------------------------------===//

namespace {
/// Every address a jump or handler enters, and how many statements begin an
/// address group at each address. C labels the first statement printed at an
/// address, so a jump lands on a given statement only when that statement is
/// the one start there. A first child sharing its parent's address prints
/// under the parent's label.
class LabelStarts {
  std::set<va_t> Entered;
  std::map<va_t, unsigned> Starts;

  void count(const std::vector<HighStmt> &L, va_t Parent) {
    for (size_t I = 0; I < L.size(); ++I) {
      const HighStmt &S = L[I];
      if (beginsGroup(L, I, Parent))
        ++Starts[S.Addr];
      if (S.Kind == StmtKind::Goto)
        Entered.insert(S.GotoTarget);
      for (const HighEHClause &Clause : S.EHClauses) {
        Entered.insert(Clause.HandlerVA);
        Entered.insert(Clause.ContinuationVAs.begin(),
                       Clause.ContinuationVAs.end());
      }
      count(S.Body, S.Addr);
      count(S.ElseBody, 0);
      for (const auto &C : S.Cases)
        count(C.Body, 0);
      count(S.DefaultBody, 0);
      for (const auto &ClauseBody : S.EHClauseBodies)
        count(ClauseBody, 0);
    }
  }

public:
  explicit LabelStarts(const std::vector<HighStmt> &Body) { count(Body, 0); }

  static bool beginsGroup(const std::vector<HighStmt> &L, size_t I,
                          va_t Parent) {
    const va_t Addr = L[I].Addr;
    return Addr != 0 && Addr != InvalidVA &&
           Addr != (I == 0 ? Parent : L[I - 1].Addr);
  }

  const std::set<va_t> &entered() const { return Entered; }

  /// True when a jump to \p Addr reaches the one statement starting there.
  bool unique(va_t Addr) const {
    auto It = Starts.find(Addr);
    return It != Starts.end() && It->second == 1;
  }

  /// True when a statement starts at \p Addr.
  bool starts(va_t Addr) const { return Starts.count(Addr) != 0; }
};
} // namespace

/// Remove each goto whose target is the statement right after it in the same
/// list. \p Entered holds every address a jump or handler reaches, and
/// \p Starts the statements of the whole function. With \p Late, statements
/// may have moved, so the target must be the next statement's own address and
/// no other statement may start there. The early form also accepts padding
/// between the jump and the next statement: a target short of the next
/// statement where no statement starts, since blocks need not follow their
/// addresses.
static bool removeTrivialGotos(std::vector<HighStmt> &Stmts,
                               const std::set<va_t> &Entered,
                               const LabelStarts &Starts, bool Late) {
  bool Changed = false;
  for (int I = static_cast<int>(Stmts.size()) - 2; I >= 0; --I) {
    if (Stmts[I].Kind != StmtKind::Goto)
      continue;
    va_t Target = Stmts[I].GotoTarget;
    if (Target == 0 || Target == InvalidVA)
      continue;
    // A removed statement without an address prints nothing.
    size_t Next = static_cast<size_t>(I) + 1;
    while (Next < Stmts.size() && Stmts[Next].Kind == StmtKind::Nop &&
           (Stmts[Next].Addr == 0 || Stmts[Next].Addr == InvalidVA))
      ++Next;
    if (Next == Stmts.size())
      continue;
    va_t NextAddr = Stmts[Next].Addr;
    // The next statement starts a label only when it begins its address
    // group; otherwise a goto to that address lands at or before this one.
    if (NextAddr == 0 || NextAddr == Stmts[I].Addr)
      continue;
    // A target short of the next statement is fall-through only when it lies
    // past the goto itself (padding between them); a target at or before the
    // goto, such as the `jmp $` self-loop, is a real backward jump.
    const va_t Own = Stmts[I].Addr;
    const bool OwnKnown = Own != 0 && Own != InvalidVA;
    if (Late ? Target != NextAddr || !Starts.unique(Target)
             : !(Target == NextAddr ||
                 (OwnKnown && Target > Own && Target < NextAddr &&
                  NextAddr - Target <= 16 && !Starts.starts(Target))))
      continue;
    // A goto that is itself a branch target (a lone `jmp` block) keeps its
    // address as an empty anchor, so gotos to it still have a label.
    if (OwnKnown && Entered.count(Own)) {
      HighStmt Anchor;
      Anchor.Kind = StmtKind::Block;
      Anchor.Addr = Own;
      Stmts[I] = std::move(Anchor);
    } else {
      Stmts.erase(Stmts.begin() + I);
    }
    Changed = true;
  }
  for (auto &S : Stmts) {
    Changed |= removeTrivialGotos(S.Body, Entered, Starts, Late);
    Changed |= removeTrivialGotos(S.ElseBody, Entered, Starts, Late);
    for (auto &C : S.Cases)
      Changed |= removeTrivialGotos(C.Body, Entered, Starts, Late);
    Changed |= removeTrivialGotos(S.DefaultBody, Entered, Starts, Late);
    for (auto &ClauseBody : S.EHClauseBodies)
      Changed |= removeTrivialGotos(ClauseBody, Entered, Starts, Late);
  }
  return Changed;
}

bool dropJumpsToTheNextStatement(std::vector<HighStmt> &Body) {
  const LabelStarts Labels(Body);
  return removeTrivialGotos(Body, Labels.entered(), Labels, /*Late=*/true);
}

//===----------------------------------------------------------------------===//
// Nested goto simplification inside structured blocks
//===----------------------------------------------------------------------===//

static void simplifyNestedGotos(std::vector<HighStmt> &Stmts) {
  for (auto &S : Stmts) {
    if (S.Kind == StmtKind::While || S.Kind == StmtKind::If ||
        S.Kind == StmtKind::IfElse) {
      if (!S.Body.empty()) {
        for (int J = static_cast<int>(S.Body.size()) - 1; J >= 0; --J) {
          auto &InnerStmt = S.Body[J];
          if (InnerStmt.Kind != StmtKind::If)
            continue;
          if (InnerStmt.Body.size() != 1 ||
              InnerStmt.Body[0].Kind != StmtKind::Goto)
            continue;

          va_t IfGotoTarget = InnerStmt.Body[0].GotoTarget;
          size_t NextJ = static_cast<size_t>(J) + 1;
          if (NextJ >= S.Body.size())
            continue;

          std::vector<HighStmt> ElseStmts;
          size_t EndJ = NextJ;
          for (size_t K = NextJ; K < S.Body.size(); ++K) {
            ElseStmts.push_back(S.Body[K]);
            EndJ = K + 1;
            if (S.Body[K].Kind == StmtKind::Goto ||
                S.Body[K].Kind == StmtKind::Return ||
                S.Body[K].Kind == StmtKind::Continue)
              break;
          }

          if (ElseStmts.empty())
            continue;

          va_t ElseMergeAddr = 0;
          if (!ElseStmts.empty() && ElseStmts.back().Kind == StmtKind::Goto)
            ElseMergeAddr = ElseStmts.back().GotoTarget;

          // The statements after the else part replace the goto only when
          // they are its target; otherwise the jump stays in the then arm.
          std::vector<HighStmt> IfStmts;
          const bool TargetFollows =
              IfGotoTarget != 0 && IfGotoTarget != InvalidVA &&
              EndJ < S.Body.size() &&
              (S.Body[EndJ].Addr == IfGotoTarget ||
               (S.Body[EndJ].Kind == StmtKind::While &&
                S.Body[EndJ].LoopHeaderAddr == IfGotoTarget));
          if (TargetFollows) {
            size_t IfEnd = EndJ;
            for (size_t K = EndJ; K < S.Body.size(); ++K) {
              if (ElseMergeAddr != 0 && S.Body[K].Addr == ElseMergeAddr)
                break;
              IfStmts.push_back(S.Body[K]);
              IfEnd = K + 1;
              if (S.Body[K].Kind == StmtKind::Goto ||
                  S.Body[K].Kind == StmtKind::Return ||
                  S.Body[K].Kind == StmtKind::Continue)
                break;
            }
            if (!IfStmts.empty())
              EndJ = IfEnd;
          }

          InnerStmt.Kind = StmtKind::IfElse;
          if (!IfStmts.empty())
            InnerStmt.Body = std::move(IfStmts);
          InnerStmt.ElseBody = std::move(ElseStmts);

          S.Body.erase(S.Body.begin() + static_cast<long>(NextJ),
                       S.Body.begin() + static_cast<long>(EndJ));
        }
        simplifyNestedGotos(S.Body);
      }
      if (!S.ElseBody.empty())
        simplifyNestedGotos(S.ElseBody);
    }
  }
}

//===----------------------------------------------------------------------===//
// Guard-before-switch cleanup
//===----------------------------------------------------------------------===//

/// Whether the guard \p Cond holds when the switch key is \p Value, for a
/// guard that compares the key with a constant (possibly negated); nullopt
/// for any other guard.
static std::optional<bool>
guardHoldsForKey(const HighExpr *Cond, const HighExpr &Key, uint64_t Value) {
  bool Negated = false;
  while (Cond && Cond->Kind == ExprKind::UnaryOp &&
         Cond->Op == NdOp::BOOL_NOT && Cond->Operands.size() == 1) {
    Negated = !Negated;
    Cond = Cond->Operands[0].get();
  }
  if (!Cond || Cond->Kind != ExprKind::BinOp || Cond->Operands.size() != 2 ||
      !Cond->Operands[0] || !Cond->Operands[1])
    return std::nullopt;
  auto Peel = [](const HighExpr *E) {
    while (E && !E->Operands.empty() &&
           (E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast ||
            (E->Kind == ExprKind::UnaryOp &&
             (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT))))
      E = E->Operands[0].get();
    return E;
  };
  const HighExpr *Left = Cond->Operands[0].get();
  const HighExpr *Right = Cond->Operands[1].get();
  const std::string KeyText = Peel(&Key)->str();
  const bool KeyLeft =
      Right->Kind == ExprKind::Const && Peel(Left)->str() == KeyText;
  const bool KeyRight =
      Left->Kind == ExprKind::Const && Peel(Right)->str() == KeyText;
  if (KeyLeft == KeyRight)
    return std::nullopt;
  const HighExpr *KeySide = KeyLeft ? Left : Right;
  if (!KeySide->Type || !KeySide->Type->Size || KeySide->Type->Size > 8)
    return std::nullopt;
  const unsigned Bits = 8u * KeySide->Type->Size;
  const llvm::APInt KeyValue(Bits, Value);
  const llvm::APInt Constant(Bits, (KeyLeft ? Right : Left)->ConstVal);
  const llvm::APInt &A = KeyLeft ? KeyValue : Constant;
  const llvm::APInt &B = KeyLeft ? Constant : KeyValue;
  bool Holds;
  switch (Cond->Op) {
  case NdOp::INT_EQUAL:
    Holds = A == B;
    break;
  case NdOp::INT_NOTEQUAL:
    Holds = A != B;
    break;
  case NdOp::INT_LESS:
    Holds = A.ult(B);
    break;
  case NdOp::INT_LESSEQUAL:
    Holds = A.ule(B);
    break;
  case NdOp::INT_SLESS:
    Holds = A.slt(B);
    break;
  case NdOp::INT_SLESSEQUAL:
    Holds = A.sle(B);
    break;
  default:
    return std::nullopt;
  }
  return Holds != Negated;
}

void cleanupGuardBeforeSwitch(HighFunc &Func) {
  for (size_t I = 0; I < Func.Body.size(); ++I) {
    if (Func.Body[I].Kind != StmtKind::If)
      continue;
    if (Func.Body[I].Body.size() != 1)
      continue;

    size_t SwIdx = I + 1;
    while (SwIdx < Func.Body.size() &&
           (Func.Body[SwIdx].Kind == StmtKind::Assign ||
            Func.Body[SwIdx].Kind == StmtKind::Nop))
      ++SwIdx;
    if (SwIdx >= Func.Body.size() || Func.Body[SwIdx].Kind != StmtKind::Switch)
      continue;
    if (Func.Body[SwIdx].DefaultBody.empty())
      continue;

    auto &IfBody = Func.Body[I].Body[0];
    auto &DefBody = Func.Body[SwIdx].DefaultBody;
    bool BodiesMatch = false;

    auto GetConstVal = [](const ExprPtr &E) -> std::optional<uint64_t> {
      if (!E)
        return std::nullopt;
      if (E->Kind == ExprKind::Const)
        return E->ConstVal;
      if (!E->Operands.empty() && E->Operands[0]->Kind == ExprKind::Const) {
        if (E->Kind == ExprKind::Cast)
          return E->Operands[0]->ConstVal;
        if (E->Kind == ExprKind::UnaryOp &&
            (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT))
          return E->Operands[0]->ConstVal;
      }
      return std::nullopt;
    };

    // The guard's path takes the default instead, so the default must be
    // exactly the guard's one statement.
    if (IfBody.Kind == StmtKind::Return && DefBody.size() == 1 &&
        DefBody[0].Kind == StmtKind::Return) {
      auto LV = GetConstVal(IfBody.RetVal);
      auto RV = GetConstVal(DefBody[0].RetVal);
      if (LV && RV) {
        const unsigned Bytes = Func.ReturnType && Func.ReturnType->Size &&
                                       Func.ReturnType->Size < 8
                                   ? Func.ReturnType->Size
                                   : 8;
        const uint64_t Mask =
            Bytes >= 8 ? ~uint64_t(0) : (uint64_t(1) << (8 * Bytes)) - 1;
        BodiesMatch = (*LV & Mask) == (*RV & Mask);
      } else if (!IfBody.RetVal && !DefBody[0].RetVal)
        BodiesMatch = true;
    }
    // A jump's target may read the assignments between the guard and the
    // switch, which the guard's path now runs.
    if (IfBody.Kind == StmtKind::Goto && DefBody.size() == 1 &&
        DefBody[0].Kind == StmtKind::Goto &&
        std::all_of(Func.Body.begin() + static_cast<long>(I) + 1,
                    Func.Body.begin() + static_cast<long>(SwIdx),
                    [](const HighStmt &S) { return S.Kind == StmtKind::Nop; }))
      BodiesMatch = (IfBody.GotoTarget == DefBody[0].GotoTarget);
    // Every value the guard catches must reach that default: no case value
    // may satisfy it.
    const HighStmt &Switch = Func.Body[SwIdx];
    BodiesMatch =
        BodiesMatch && Switch.SwitchExpr &&
        std::all_of(Switch.Cases.begin(), Switch.Cases.end(),
                    [&](const SwitchCase &Case) {
                      const std::optional<bool> Holds =
                          guardHoldsForKey(Func.Body[I].Cond.get(),
                                           *Switch.SwitchExpr, Case.Value);
                      return Holds && !*Holds;
                    });

    if (BodiesMatch) {
      Func.Body.erase(Func.Body.begin() + static_cast<long>(I));
      --I;
    }
  }
}

//===----------------------------------------------------------------------===//
// inlineGotoReturns — replace goto→return-block with inline return
//===----------------------------------------------------------------------===//

void MedToHighConverter::inlineGotoReturns(HighFunc &Func, const MedFunc &Med) {
  std::map<va_t, ExprPtr> ReturnBlocks;
  for (auto &MedBlock : Med.Blocks) {
    if (MedBlock.Ops.size() < 2 || MedBlock.Ops.size() > 4)
      continue;
    auto &LastOp = MedBlock.Ops.back();
    if (LastOp.Opcode != NdOp::RETURN)
      continue;
    // Only register bookkeeping (epilogue restores, stack adjustment) may
    // sit beside the return value: a goto replaced by `return v` must not
    // skip a store or call, or a load that computes v itself.
    bool OnlyBookkeeping = true;
    for (size_t J = 0; J + 1 < MedBlock.Ops.size(); ++J) {
      const MedOp &Op = MedBlock.Ops[J];
      OnlyBookkeeping &=
          Op.Output.Kind == MedVar::Reg && Op.Opcode != NdOp::STORE &&
          Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL &&
          Op.Opcode != NdOp::INTRINSIC && Op.Opcode != NdOp::ATOMIC_XCHG &&
          Op.Opcode != NdOp::ATOMIC_ADD && Op.Opcode != NdOp::ATOMIC_CMPXCHG;
    }
    if (!OnlyBookkeeping)
      continue;
    for (int J = static_cast<int>(MedBlock.Ops.size()) - 2; J >= 0; --J) {
      auto &CurrOp = MedBlock.Ops[J];
      if (CurrOp.Output.Kind == MedVar::Reg && CurrOp.Output.RegOff == 0) {
        const bool DefinedHere = std::any_of(
            MedBlock.Ops.begin(), MedBlock.Ops.end(), [&](const MedOp &Op) {
              return CurrOp.NumInputs >= 1 && !Op.Output.isConst() &&
                     Op.Output == CurrOp.Inputs[0];
            });
        if ((CurrOp.Opcode == NdOp::COPY || CurrOp.Opcode == NdOp::INT_ZEXT) &&
            CurrOp.NumInputs >= 1 && !DefinedHere)
          ReturnBlocks[MedBlock.Ops.front().Addr] =
              medvarToExpr(CurrOp.Inputs[0]);
        break;
      }
    }
  }

  std::function<void(std::vector<HighStmt> &)> Rewrite;
  Rewrite = [&](std::vector<HighStmt> &Stmts) {
    for (auto &S : Stmts) {
      if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
          S.GotoTarget != InvalidVA) {
        auto It = ReturnBlocks.find(S.GotoTarget);
        if (It != ReturnBlocks.end()) {
          S.Kind = StmtKind::Return;
          S.RetVal = It->second;
        }
      }
      Rewrite(S.Body);
      Rewrite(S.ElseBody);
      for (auto &C : S.Cases)
        Rewrite(C.Body);
      Rewrite(S.DefaultBody);
    }
  };
  Rewrite(Func.Body);
}

//===----------------------------------------------------------------------===//
// Merge consecutive if-blocks with the same condition
//===----------------------------------------------------------------------===//

static bool exprEquivalent(const ExprPtr &A, const ExprPtr &B) {
  if (!A || !B)
    return false;
  return A->structuralEq(*B);
}

static void mergeConsecutiveCondBlocks(std::vector<HighStmt> &Stmts) {
  for (size_t I = 0; I + 1 < Stmts.size();) {
    auto &Cur = Stmts[I];
    auto &Next = Stmts[I + 1];

    if (Cur.Kind == StmtKind::If && Next.Kind == StmtKind::If && Cur.Cond &&
        Next.Cond && !Cur.Cond->hasOrderedMemoryAccess() &&
        !Next.Cond->hasOrderedMemoryAccess() &&
        exprEquivalent(Cur.Cond, Next.Cond)) {
      for (auto &S : Next.Body)
        Cur.Body.push_back(std::move(S));
      Stmts.erase(Stmts.begin() + static_cast<long>(I + 1));
      continue;
    }

    if (Cur.Kind == StmtKind::IfElse && Next.Kind == StmtKind::IfElse &&
        Cur.Cond && Next.Cond && !Cur.Cond->hasOrderedMemoryAccess() &&
        !Next.Cond->hasOrderedMemoryAccess() &&
        exprEquivalent(Cur.Cond, Next.Cond)) {
      for (auto &S : Next.Body)
        Cur.Body.push_back(std::move(S));
      for (auto &S : Next.ElseBody)
        Cur.ElseBody.push_back(std::move(S));
      Stmts.erase(Stmts.begin() + static_cast<long>(I + 1));
      continue;
    }

    ++I;
  }

  for (auto &S : Stmts) {
    if (!S.Body.empty())
      mergeConsecutiveCondBlocks(S.Body);
    if (!S.ElseBody.empty())
      mergeConsecutiveCondBlocks(S.ElseBody);
  }
}

//===----------------------------------------------------------------------===//
// simplifyControlFlow -- the main entry point
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Duplicate small return tails into the gotos that reach them
//===----------------------------------------------------------------------===//

static bool isPureValue(const HighExpr &E, unsigned Depth = 0) {
  if (Depth > 16)
    return false;
  switch (E.Kind) {
  case ExprKind::Var:
  case ExprKind::Const:
  case ExprKind::Phi:
    return true;
  case ExprKind::BinOp:
  case ExprKind::UnaryOp:
  case ExprKind::Cast:
  case ExprKind::BitCast:
    if (E.Op == NdOp::ATOMIC_ADD || E.Op == NdOp::ATOMIC_XCHG ||
        E.Op == NdOp::ATOMIC_CMPXCHG)
      return false;
    for (const ExprPtr &Op : E.Operands)
      if (!Op || !isPureValue(*Op, Depth + 1))
        return false;
    return true;
  default:
    return false;
  }
}

/// A value a return tail may copy: pure, or reading memory, with no call,
/// store or atomic.  Each path executes exactly one copy, so duplicating a
/// load is as exact as duplicating arithmetic.
static bool isTailValue(const HighExpr &E, unsigned Depth = 0) {
  if (Depth > 32)
    return false;
  switch (E.Kind) {
  case ExprKind::Var:
  case ExprKind::Const:
  case ExprKind::Phi:
    return true;
  case ExprKind::Load:
    if (E.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        E.MemoryOrdering != NdMemoryOrdering::None)
      return false;
    [[fallthrough]];
  case ExprKind::BinOp:
  case ExprKind::UnaryOp:
  case ExprKind::Cast:
  case ExprKind::BitCast:
    if (E.Op == NdOp::ATOMIC_ADD || E.Op == NdOp::ATOMIC_XCHG ||
        E.Op == NdOp::ATOMIC_CMPXCHG)
      return false;
    for (const ExprPtr &Op : E.Operands)
      if (!Op || !isTailValue(*Op, Depth + 1))
        return false;
    return true;
  default:
    return false;
  }
}

static bool hasUniqueCallOccurrence(const ExprPtr &Expression) {
  if (!Expression)
    return false;
  if (Expression->SourceCallHint &&
      Expression->SourceCallHint->requiresUniqueSourceOccurrence())
    return true;
  bool Found = false;
  Expression->forEachChildExpr(
      [&](const ExprPtr &Child) { Found |= hasUniqueCallOccurrence(Child); });
  return Found;
}

// A call declaration can be copied along exclusive paths. Evidence naming
// one native occurrence cannot: its publication owner still requires one
// source evaluation, including after both return-tail and jump-tail copying.
static bool isDuplicableTailCall(const HighExpr &Call) {
  return Call.Kind == ExprKind::Call && Call.IntrinsicOutputs.empty() &&
         (!Call.SourceCallHint ||
          !Call.SourceCallHint->requiresUniqueSourceOccurrence()) &&
         std::all_of(Call.Operands.begin(), Call.Operands.end(),
                     [](const ExprPtr &Op) { return Op && isTailValue(*Op); });
}

/// Number of statements in \p S that a tail may copy to each jump into it:
/// assignments of tail values, and calls and plain stores of them, which each
/// path still runs exactly once (or a block of only such statements and
/// removed ones); nullopt otherwise.  A block member may carry its own
/// address only when no jump targets it.
static std::optional<size_t> pureAssignCount(const HighStmt &S,
                                             const std::set<va_t> &Targets) {
  if (S.Kind == StmtKind::Nop)
    return 0;
  if (S.Kind == StmtKind::Store)
    return S.StoreAddr && S.StoreVal &&
                   S.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
                   S.MemoryOrdering == NdMemoryOrdering::None &&
                   isTailValue(*S.StoreAddr) && isTailValue(*S.StoreVal)
               ? std::optional<size_t>(1)
               : std::nullopt;
  if (S.Kind == StmtKind::Assign) {
    if (!S.Dst || S.Dst->Kind != ExprKind::Var || !S.Val)
      return std::nullopt;
    if (isTailValue(*S.Val))
      return 1;
    // One call (or single-result intrinsic) whose arguments are tail
    // values: each path still runs exactly one copy of it.
    if (isDuplicableTailCall(*S.Val))
      return 1;
    return std::nullopt;
  }
  if (S.Kind == StmtKind::Call) {
    const HighExpr *Call = S.CallExpr.get();
    if (Call && isDuplicableTailCall(*Call))
      return 1;
    return std::nullopt;
  }
  if (S.Kind != StmtKind::Block)
    return std::nullopt;
  size_t Count = 0;
  for (const HighStmt &Child : S.Body) {
    if (Child.Addr != 0 && Child.Addr != S.Addr && Targets.count(Child.Addr))
      return std::nullopt;
    std::optional<size_t> ChildCount = pureAssignCount(Child, Targets);
    if (!ChildCount || (Child.Kind == StmtKind::Block && !Child.Body.empty()))
      return std::nullopt;
    Count += *ChildCount;
  }
  return Count;
}

static bool endsItsBlock(const HighStmt &S);
static bool isEmptyAnchor(const HighStmt &S);

/// The try statements whose protected bodies enclose a statement list,
/// outermost first, each named by its kind and native range.  A copy that
/// replaces a jump must stay in the same protection: copied into a protected
/// body its faults would reach that handler, copied out of one they would
/// escape it.  A handler body runs in the protection around its try.
using TryContext = std::vector<std::tuple<StmtKind, va_t, va_t>>;

/// The context of \p S's body, given \p Outer, that of the list holding it.
static TryContext bodyTryContext(const TryContext &Outer, const HighStmt &S) {
  if (S.Kind != StmtKind::SEHTry && S.Kind != StmtKind::CxxTry &&
      S.Kind != StmtKind::ItaniumTry)
    return Outer;
  TryContext Inner = Outer;
  Inner.emplace_back(S.Kind, S.EHRange.Begin, S.EHRange.End);
  return Inner;
}

/// Whether evaluating \p E may raise an exception.  Only integer and boolean
/// arithmetic on registers and constants cannot: a load may fault, a
/// division may trap, a call may do anything, and floating point stays out
/// of the claim.
static bool exprMayFault(const HighExpr *E) {
  if (!E)
    return false;
  if (E->MemoryOrdering != NdMemoryOrdering::None ||
      E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return true;
  switch (E->Kind) {
  case ExprKind::Var:
  case ExprKind::Phi:
  case ExprKind::Const:
  case ExprKind::Undef:
  case ExprKind::EntryRegister:
  case ExprKind::Addr:
    break;
  case ExprKind::BinOp:
  case ExprKind::UnaryOp:
  case ExprKind::Cast:
  case ExprKind::BitCast:
    switch (E->Op) {
    case NdOp::NOP:
    case NdOp::COPY:
    case NdOp::CAST:
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
    case NdOp::INT_LEFT:
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR:
    case NdOp::INT_MULT:
    case NdOp::INT_EQUAL:
    case NdOp::INT_NOTEQUAL:
    case NdOp::INT_LESS:
    case NdOp::INT_SLESS:
    case NdOp::INT_LESSEQUAL:
    case NdOp::INT_SLESSEQUAL:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::INT_NEGATE:
    case NdOp::INT_NOT:
    case NdOp::INT_NEG2:
    case NdOp::BOOL_AND:
    case NdOp::BOOL_OR:
    case NdOp::BOOL_XOR:
    case NdOp::BOOL_NOT:
    case NdOp::CONCAT:
    case NdOp::SUBBYTES:
    case NdOp::SELECT:
      break;
    default:
      return true;
    }
    break;
  default:
    return true;
  }
  for (const auto &Operand : E->Operands)
    if (exprMayFault(Operand.get()))
      return true;
  return false;
}

bool highStmtMayFault(const HighStmt &S) {
  switch (S.Kind) {
  case StmtKind::Nop:
  case StmtKind::Block:
  case StmtKind::Goto:
  case StmtKind::Break:
  case StmtKind::Continue:
  case StmtKind::SEHTry:
  case StmtKind::CxxTry:
  case StmtKind::ItaniumTry:
    return false;
  case StmtKind::Assign:
    return !S.Dst ||
           (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi) ||
           exprMayFault(S.Val.get());
  case StmtKind::Return:
    return exprMayFault(S.RetVal.get());
  case StmtKind::If:
  case StmtKind::IfElse:
  case StmtKind::While:
  case StmtKind::DoWhile:
  case StmtKind::For:
    return exprMayFault(S.Cond.get());
  case StmtKind::Switch:
    return exprMayFault(S.SwitchExpr.get());
  default:
    return true;
  }
}

/// Whether running \p Tail may raise an exception.
static bool tailMayFault(const std::vector<HighStmt> &Tail) {
  for (const HighStmt &S : Tail) {
    switch (S.Kind) {
    case StmtKind::Nop:
      continue;
    case StmtKind::Block:
      if (!S.Body.empty())
        return true;
      continue;
    case StmtKind::Assign:
      if (!S.Dst ||
          (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi) ||
          exprMayFault(S.Val.get()))
        return true;
      continue;
    case StmtKind::Return:
      if (exprMayFault(S.RetVal.get()))
        return true;
      continue;
    default:
      return true;
    }
  }
  return false;
}

/// `goto L` where L starts a few pure assignments and a return (typically
/// `result = 1; return result;` shared through an epilogue), or a call that
/// never returns such as the fail-fast trap, becomes a copy of those
/// statements.  The labelled original stays for any other path, so no code
/// is removed; only the jump is.
bool duplicateSmallJumpTails(std::vector<HighStmt> &Body) {
  std::map<va_t, unsigned> Uses;
  std::set<va_t> Pinned;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      ++Uses[S.GotoTarget];
    for (const HighEHClause &Clause : S.EHClauses)
      if (Clause.HandlerVA != 0 && Clause.HandlerVA != InvalidVA)
        Pinned.insert(Clause.HandlerVA);
  });
  auto UsesOf = [&](va_t A) -> unsigned {
    auto It = Uses.find(A);
    return It == Uses.end() ? 0 : It->second;
  };
  std::set<va_t> Targets;
  for (const auto &[Addr, Count] : Uses)
    Targets.insert(Addr);
  auto Addressed = [](va_t A) { return A != 0 && A != InvalidVA; };
  const LabelStarts Starts(Body);
  // The tail at a label nothing falls into: a few pure assignments, then a
  // forward jump, the next label, or the end of a list whose follow
  // \p After is known; the copy then jumps there.
  std::map<va_t, std::vector<HighStmt>> Tails;
  std::map<va_t, TryContext> TailContexts;
  std::function<void(const std::vector<HighStmt> &, va_t, va_t,
                     const TryContext &)>
      Collect = [&](const std::vector<HighStmt> &L, va_t After, va_t Parent,
                    const TryContext &Ctx) {
        for (size_t I = 0; I < L.size(); ++I) {
          const va_t X = L[I].Addr;
          if (!Addressed(X) || Pinned.count(X) || UsesOf(X) < 2 ||
              !Starts.unique(X) || !LabelStarts::beginsGroup(L, I, Parent) ||
              Tails.count(X))
            continue;
          size_t Before = I;
          while (Before > 0 && isEmptyAnchor(L[Before - 1]) &&
                 !UsesOf(L[Before - 1].Addr))
            --Before;
          if (Before == 0 || !endsItsBlock(L[Before - 1]))
            continue;
          size_t J = I;
          size_t Assigns = 0;
          bool Pure = true;
          for (; J < L.size(); ++J) {
            if (J > I && Addressed(L[J].Addr) && L[J].Addr != L[J - 1].Addr &&
                UsesOf(L[J].Addr))
              break;
            if (L[J].Kind == StmtKind::Goto)
              break;
            std::optional<size_t> Count = pureAssignCount(L[J], Targets);
            if (!Count || Assigns + *Count > limits::kMaxJumpTailStatements) {
              Pure = false;
              break;
            }
            Assigns += *Count;
          }
          if (!Pure || (J == L.size() && !After))
            continue;
          std::vector<HighStmt> Tail(L.begin() + I, L.begin() + J);
          HighStmt Jump;
          Jump.Kind = StmtKind::Goto;
          Jump.GotoTarget = J == L.size()                 ? After
                            : L[J].Kind == StmtKind::Goto ? L[J].GotoTarget
                                                          : L[J].Addr;
          // Forward only, so copies never chase each other round a cycle.
          if (!Addressed(Jump.GotoTarget) || Jump.GotoTarget <= X)
            continue;
          Tail.push_back(std::move(Jump));
          Tails.emplace(X, std::move(Tail));
          TailContexts.emplace(X, Ctx);
        }
        for (size_t I = 0; I < L.size(); ++I) {
          const HighStmt &S = L[I];
          // Falling off an if/else arm, block or case continues after the
          // statement owning it; a loop or try body continues elsewhere.
          va_t Next = After;
          if (I + 1 < L.size()) {
            const va_t A = L[I + 1].Addr;
            Next = L[I + 1].Kind != StmtKind::Nop && Addressed(A) &&
                           A != S.Addr && Starts.unique(A)
                       ? A
                       : 0;
          }
          const bool Arms =
              S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse ||
              S.Kind == StmtKind::Block || S.Kind == StmtKind::Switch;
          const va_t Inner = Arms ? Next : 0;
          Collect(S.Body, Inner, S.Addr, bodyTryContext(Ctx, S));
          Collect(S.ElseBody, Inner, 0, Ctx);
          for (const auto &C : S.Cases)
            Collect(C.Body, Inner, 0, Ctx);
          Collect(S.DefaultBody, Inner, 0, Ctx);
          for (const auto &ClauseBody : S.EHClauseBodies)
            Collect(ClauseBody, 0, 0, Ctx);
        }
      };
  Collect(Body, 0, 0, TryContext());
  if (Tails.empty())
    return false;
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &, const TryContext &)> Rewrite =
      [&](std::vector<HighStmt> &L, const TryContext &Ctx) {
        for (size_t I = 0; I < L.size(); ++I) {
          if (L[I].Kind == StmtKind::Goto) {
            auto It = Tails.find(L[I].GotoTarget);
            if (It != Tails.end() && TailContexts.at(It->first) == Ctx) {
              L.erase(L.begin() + I);
              L.insert(L.begin() + I, It->second.begin(), It->second.end());
              I += It->second.size() - 1;
              Changed = true;
              continue;
            }
          }
          const TryContext Inner = bodyTryContext(Ctx, L[I]);
          Rewrite(L[I].Body, Inner);
          Rewrite(L[I].ElseBody, Ctx);
          for (auto &C : L[I].Cases)
            Rewrite(C.Body, Ctx);
          Rewrite(L[I].DefaultBody, Ctx);
          for (auto &ClauseBody : L[I].EHClauseBodies)
            Rewrite(ClauseBody, Ctx);
        }
      };
  Rewrite(Body, TryContext());
  return Changed;
}

/// `__try { ...; goto L; } __except (...) { ...; return; }` leaves the
/// protected body through its last jump just as falling off its end would
/// reach a `goto L` after the statement.  The jump moves there, where the
/// late rewrites can copy or splice L into the try's own protection; they
/// never move code into a protected body.  An `__except` body that falls
/// through, or that has no statements of its own, keeps the jump in place,
/// and so does a target inside the try statement.
bool hoistTryExitJumps(std::vector<HighStmt> &Body) {
  std::set<va_t> Targets;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto)
      Targets.insert(S.GotoTarget);
  });
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &L) {
        for (size_t I = 0; I < L.size(); ++I) {
          HighStmt &S = L[I];
          Visit(S.Body);
          Visit(S.ElseBody);
          for (SwitchCase &C : S.Cases)
            Visit(C.Body);
          Visit(S.DefaultBody);
          for (std::vector<HighStmt> &ClauseBody : S.EHClauseBodies)
            Visit(ClauseBody);
          if (S.Kind != StmtKind::SEHTry || S.Body.empty() ||
              S.Body.back().Kind != StmtKind::Goto ||
              S.EHClauses.size() != S.EHClauseBodies.size())
            continue;
          bool Exits = true;
          for (size_t C = 0; C < S.EHClauses.size(); ++C)
            Exits &= S.EHClauses[C].Kind == HighEHClauseKind::SEHExcept &&
                     !S.EHClauseBodies[C].empty() &&
                     endsItsBlock(S.EHClauseBodies[C].back());
          const va_t Target = S.Body.back().GotoTarget;
          bool Inside = false;
          auto Find = [&](const HighStmt &N) { Inside |= N.Addr == Target; };
          walkStmts(S.Body, Find);
          for (const std::vector<HighStmt> &ClauseBody : S.EHClauseBodies)
            walkStmts(ClauseBody, Find);
          if (!Exits || Inside || Target == 0 || Target == InvalidVA)
            continue;
          HighStmt Jump = std::move(S.Body.back());
          // A jump that is itself entered leaves its label behind.
          if (Jump.Addr != 0 && Jump.Addr != InvalidVA &&
              Targets.count(Jump.Addr) &&
              (S.Body.size() == 1 ||
               S.Body[S.Body.size() - 2].Addr != Jump.Addr)) {
            HighStmt Anchor;
            Anchor.Kind = StmtKind::Block;
            Anchor.Addr = Jump.Addr;
            S.Body.back() = std::move(Anchor);
            Jump.Addr = 0;
          } else {
            S.Body.pop_back();
          }
          L.insert(L.begin() + I + 1, std::move(Jump));
          ++I;
          Changed = true;
        }
      };
  Visit(Body);
  return Changed;
}

/// How often \p E reads the variable \p K.
static unsigned readsOf(const HighExpr *E, const VarKey &K) {
  if (!E)
    return 0;
  unsigned N = E->Kind == ExprKind::Var && varKey(E->Var) == K;
  E->forEachChildExpr([&](const ExprPtr &Op) { N += readsOf(Op.get(), K); });
  return N;
}

bool duplicateSmallReturnTails(std::vector<HighStmt> &Body, bool PrintedSize) {
  constexpr size_t kMaxTailAssigns = limits::kMaxReturnTailStatements;
  constexpr size_t kMaxComposedTail = 2 * kMaxTailAssigns + 1;
  std::set<va_t> Targets;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto)
      Targets.insert(S.GotoTarget);
  });
  const LabelStarts Starts(Body);
  std::map<va_t, std::vector<HighStmt>> Tails;
  std::map<va_t, TryContext> TailContexts;
  // The tail that runs from Stmts[I]: a few pure assignments ending in a
  // return, in a jump to a known tail in the same protection \p Ctx, or at
  // the end of the list followed by \p Cont (what runs after the construct
  // owning this list).
  auto TailAt =
      [&](const std::vector<HighStmt> &Stmts, size_t I,
          const std::vector<HighStmt> *Cont,
          const TryContext &Ctx) -> std::optional<std::vector<HighStmt>> {
    // An empty block can anchor the label ahead of the tail.
    // Removed statements (Nop) print nothing and are skipped.
    size_t First = I;
    while (First < Stmts.size() && ((Stmts[First].Kind == StmtKind::Block &&
                                     Stmts[First].Body.empty()) ||
                                    Stmts[First].Kind == StmtKind::Nop))
      ++First;
    // A temporary read once later in the tail prints inside that read, and
    // foldTempsInReturnTails folds it in each copy, so by printed size it
    // does not count toward the limit; the tail still holds at most
    // kMaxComposedTail statements.
    auto Weight = [&](size_t N, size_t Count) -> size_t {
      const HighStmt &S = Stmts[N];
      if (S.Kind != StmtKind::Assign || !S.Dst ||
          S.Dst->Kind != ExprKind::Var || S.Dst->Var.Kind != MedVar::Temp)
        return Count;
      const VarKey K = varKey(S.Dst->Var);
      unsigned Uses = 0;
      for (size_t M = N + 1; M < Stmts.size(); ++M) {
        forEachExpr(Stmts[M],
                    [&](const ExprPtr &E) { Uses += readsOf(E.get(), K); });
        if (Stmts[M].Kind == StmtKind::Return ||
            Stmts[M].Kind == StmtKind::Goto)
          break;
      }
      return Uses == 1 ? 0 : Count;
    };
    size_t J = First;
    size_t Assigns = 0, Statements = 0;
    while (J < Stmts.size()) {
      std::optional<size_t> Count = pureAssignCount(Stmts[J], Targets);
      if (!Count)
        break;
      const size_t Cost = PrintedSize ? Weight(J, *Count) : *Count;
      if (Assigns + Cost > kMaxTailAssigns ||
          Statements + *Count > kMaxComposedTail)
        break;
      Assigns += Cost;
      Statements += *Count;
      // Nothing runs after a call that never returns.
      if (endsItsBlock(Stmts[J]))
        return std::vector<HighStmt>(Stmts.begin() + First,
                                     Stmts.begin() + J + 1);
      ++J;
    }
    if (J < Stmts.size() && Stmts[J].Kind == StmtKind::Return &&
        (!Stmts[J].RetVal || isTailValue(*Stmts[J].RetVal)))
      return std::vector<HighStmt>(Stmts.begin() + First,
                                   Stmts.begin() + J + 1);
    const std::vector<HighStmt> *Rest = nullptr;
    if (J < Stmts.size() && Stmts[J].Kind == StmtKind::Goto &&
        Stmts[J].GotoTarget != Stmts[I].Addr) {
      // Edge copies ahead of a jump to a shared return epilogue: the
      // tail is those copies followed by the epilogue's own tail.
      auto Target = Tails.find(Stmts[J].GotoTarget);
      if (Target != Tails.end() && TailContexts.at(Target->first) == Ctx)
        Rest = &Target->second;
    } else if (J == Stmts.size()) {
      Rest = Cont;
    }
    if (!Rest || Assigns + Rest->size() > kMaxComposedTail)
      return std::nullopt;
    std::vector<HighStmt> Tail(Stmts.begin() + First, Stmts.begin() + J);
    Tail.insert(Tail.end(), Rest->begin(), Rest->end());
    return Tail;
  };
  std::function<void(std::vector<HighStmt> &, const std::vector<HighStmt> *,
                     va_t, const TryContext &)>
      Collect = [&](std::vector<HighStmt> &Stmts,
                    const std::vector<HighStmt> *Cont, va_t Parent,
                    const TryContext &Ctx) {
        for (size_t I = 0; I < Stmts.size(); ++I) {
          const va_t Label = Stmts[I].Addr;
          if (Starts.unique(Label) && !Tails.count(Label) &&
              LabelStarts::beginsGroup(Stmts, I, Parent))
            if (auto Tail = TailAt(Stmts, I, Cont, Ctx)) {
              Tails.emplace(Label, std::move(*Tail));
              TailContexts.emplace(Label, Ctx);
            }
          // Falling off an if/else arm or block continues after it, and so
          // does falling off the protected body or a handler of a try with
          // only __except clauses.  Copied back into the protected body, what
          // runs after the try would run under its handlers, so the body
          // continues into it only when it cannot fault.
          std::optional<std::vector<HighStmt>> After;
          const StmtKind K = Stmts[I].Kind;
          const bool ExceptOnly =
              K == StmtKind::SEHTry && !Stmts[I].EHClauses.empty() &&
              llvm::all_of(Stmts[I].EHClauses, [](const HighEHClause &Clause) {
                return Clause.Kind == HighEHClauseKind::SEHExcept;
              });
          if (K == StmtKind::If || K == StmtKind::IfElse ||
              K == StmtKind::Block || ExceptOnly)
            After = I + 1 < Stmts.size()
                        ? TailAt(Stmts, I + 1, Cont, Ctx)
                        : (Cont ? std::optional(*Cont) : std::nullopt);
          const std::vector<HighStmt> *ChildCont = After ? &*After : nullptr;
          const std::vector<HighStmt> *BodyCont =
              ExceptOnly && ChildCont && tailMayFault(*ChildCont) ? nullptr
                                                                  : ChildCont;
          Collect(Stmts[I].Body, BodyCont, Stmts[I].Addr,
                  bodyTryContext(Ctx, Stmts[I]));
          Collect(Stmts[I].ElseBody, ChildCont, 0, Ctx);
          for (auto &C : Stmts[I].Cases)
            Collect(C.Body, nullptr, 0, Ctx);
          Collect(Stmts[I].DefaultBody, nullptr, 0, Ctx);
          for (auto &ClauseBody : Stmts[I].EHClauseBodies)
            Collect(ClauseBody, ExceptOnly ? ChildCont : nullptr, 0, Ctx);
        }
      };
  // A composed tail needs its epilogue's tail first; the epilogue usually
  // follows the jumps to it, so repeat until no new tail appears.
  for (size_t Round = 0; Round < 4; ++Round) {
    const size_t Before = Tails.size();
    Collect(Body, nullptr, 0, TryContext());
    if (Tails.size() == Before)
      break;
  }
  if (Tails.empty())
    return false;
  // A tail that cannot raise an exception runs the same in any protection
  // made only of __except clauses: no handler can catch it, and none runs
  // on the way out of the try the way a __finally would.
  std::set<TryContext::value_type> ExceptOnly;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::SEHTry && !S.EHClauses.empty() &&
        std::all_of(S.EHClauses.begin(), S.EHClauses.end(),
                    [](const HighEHClause &Clause) {
                      return Clause.Kind == HighEHClauseKind::SEHExcept;
                    }))
      ExceptOnly.emplace(S.Kind, S.EHRange.Begin, S.EHRange.End);
  });
  auto SameProtection = [&](const TryContext &A, const TryContext &B,
                            const std::vector<HighStmt> &Tail) {
    if (A == B)
      return true;
    size_t Shared = 0;
    while (Shared < A.size() && Shared < B.size() && A[Shared] == B[Shared])
      ++Shared;
    for (const TryContext *Ctx : {&A, &B})
      for (size_t K = Shared; K < Ctx->size(); ++K)
        if (!ExceptOnly.count((*Ctx)[K]))
          return false;
    return !tailMayFault(Tail);
  };
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &, const TryContext &)> Rewrite =
      [&](std::vector<HighStmt> &Stmts, const TryContext &Ctx) {
        for (size_t I = 0; I < Stmts.size(); ++I) {
          if (Stmts[I].Kind == StmtKind::Goto) {
            auto It = Tails.find(Stmts[I].GotoTarget);
            if (It != Tails.end() &&
                SameProtection(TailContexts.at(It->first), Ctx, It->second)) {
              std::vector<HighStmt> Copy = It->second;
              // The copies are not jump targets; keep the label unique.
              const va_t Site = Stmts[I].Addr;
              for (HighStmt &C : Copy)
                C.Addr = Site;
              walkStmts(Copy, [&](HighStmt &C) {
                if (C.Addr != 0)
                  C.Addr = Site;
              });
              Stmts.erase(Stmts.begin() + I);
              Stmts.insert(Stmts.begin() + I, Copy.begin(), Copy.end());
              I += Copy.size() - 1;
              Changed = true;
              continue;
            }
          }
          const TryContext Inner = bodyTryContext(Ctx, Stmts[I]);
          Rewrite(Stmts[I].Body, Inner);
          Rewrite(Stmts[I].ElseBody, Ctx);
          for (auto &C : Stmts[I].Cases)
            Rewrite(C.Body, Ctx);
          Rewrite(Stmts[I].DefaultBody, Ctx);
          // A handler that jumps to a return tail returns as the copy does.
          for (auto &ClauseBody : Stmts[I].EHClauseBodies)
            Rewrite(ClauseBody, Ctx);
        }
      };
  Rewrite(Body, TryContext());
  return Changed;
}

/// Late goto reduction on the final statement tree.  Every rewrite moves or
/// merges statements within one statement list and keeps their order of
/// execution; nothing is removed except a jump made redundant by the move.
///
///  * `if (a) goto L; if (b) goto L;`  ->  `if (a || b) goto L;`
///  * a block entered only by one forward `if (c) goto X;`, placed after a
///    terminator and ending in `return`/`goto`, moves into that `if`.
///  * `if (c) { ...; goto Y; } S...; Y:`  ->  `if (c) { ... } else { S... }`.
/// True when \p Stmts contains a break or continue that would bind to a loop
/// wrapped around it.  A nested loop owns both; a switch owns only break.
static bool hasLooseBreakOrContinue(const std::vector<HighStmt> &Stmts,
                                    bool InSwitch = false) {
  for (const HighStmt &S : Stmts) {
    if (S.Kind == StmtKind::Continue ||
        (S.Kind == StmtKind::Break && !InSwitch))
      return true;
    switch (S.Kind) {
    case StmtKind::While:
    case StmtKind::DoWhile:
    case StmtKind::For:
      break;
    case StmtKind::Switch:
      for (const auto &C : S.Cases)
        if (hasLooseBreakOrContinue(C.Body, /*InSwitch=*/true))
          return true;
      if (hasLooseBreakOrContinue(S.DefaultBody, /*InSwitch=*/true))
        return true;
      break;
    default:
      if (hasLooseBreakOrContinue(S.Body, InSwitch) ||
          hasLooseBreakOrContinue(S.ElseBody, InSwitch))
        return true;
      for (const auto &ClauseBody : S.EHClauseBodies)
        if (hasLooseBreakOrContinue(ClauseBody, InSwitch))
          return true;
      break;
    }
  }
  return false;
}

bool loopsForArmsJumpingBack(std::vector<HighStmt> &Body) {
  std::map<va_t, unsigned> Uses, Starts;
  std::function<void(const std::vector<HighStmt> &)> Count =
      [&](const std::vector<HighStmt> &L) {
        for (size_t I = 0; I < L.size(); ++I) {
          const HighStmt &S = L[I];
          if (S.Kind == StmtKind::Goto)
            ++Uses[S.GotoTarget];
          if (S.Addr != 0 && S.Addr != InvalidVA &&
              (I == 0 || L[I - 1].Addr != S.Addr))
            ++Starts[S.Addr];
          Count(S.Body);
          Count(S.ElseBody);
          for (const auto &C : S.Cases)
            Count(C.Body);
          Count(S.DefaultBody);
          for (const auto &ClauseBody : S.EHClauseBodies)
            Count(ClauseBody);
        }
      };
  Count(Body);
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &L) {
        for (HighStmt &S : L) {
          Visit(S.Body);
          Visit(S.ElseBody);
          for (auto &C : S.Cases)
            Visit(C.Body);
          Visit(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            Visit(ClauseBody);
        }
        for (size_t I = 0; I < L.size(); ++I) {
          const HighStmt &Arm = L[I];
          if (Arm.Kind != StmtKind::If || !Arm.Cond || !Arm.ElseBody.empty() ||
              Arm.Body.size() < 2 || Arm.Body.back().Kind != StmtKind::Goto)
            continue;
          const va_t X = Arm.Body.back().GotoTarget;
          if (Starts[X] != 1)
            continue;
          size_t K = I;
          while (K > 0 &&
                 !(L[K - 1].Addr == X && (K == 1 || L[K - 2].Addr != X)))
            --K;
          if (K == 0)
            continue;
          --K;
          std::vector<HighStmt> LoopBody(L.begin() + K, L.begin() + I + 1);
          if (hasLooseBreakOrContinue(LoopBody))
            continue;
          // A jump carrying an entered label keeps it as an empty block.
          std::vector<HighStmt> &Tail = LoopBody.back().Body;
          HighStmt &Jump = Tail.back();
          if (Jump.Addr != 0 && Jump.Addr != InvalidVA && Uses[Jump.Addr] &&
              Tail[Tail.size() - 2].Addr != Jump.Addr) {
            HighStmt Anchor;
            Anchor.Kind = StmtKind::Block;
            Anchor.Addr = Jump.Addr;
            Jump = std::move(Anchor);
          } else {
            Tail.pop_back();
          }
          HighStmt Next;
          Next.Kind = StmtKind::Continue;
          Tail.push_back(std::move(Next));
          HighStmt Leave;
          Leave.Kind = StmtKind::Break;
          LoopBody.push_back(std::move(Leave));
          HighStmt Loop;
          Loop.Kind = StmtKind::While;
          Loop.Cond = HighExpr::makeConst(1, 1);
          Loop.LoopHeaderAddr = X;
          Loop.Body = std::move(LoopBody);
          --Uses[X];
          L.erase(L.begin() + K, L.begin() + I + 1);
          L.insert(L.begin() + K, std::move(Loop));
          I = K;
          Changed = true;
        }
      };
  Visit(Body);
  return Changed;
}

bool loopsForNestedJumpsBack(std::vector<HighStmt> &Body) {
  std::map<va_t, unsigned> Uses, Starts;
  std::function<void(const std::vector<HighStmt> &)> Count =
      [&](const std::vector<HighStmt> &L) {
        for (size_t I = 0; I < L.size(); ++I) {
          const HighStmt &S = L[I];
          if (S.Kind == StmtKind::Goto)
            ++Uses[S.GotoTarget];
          for (const HighEHClause &Clause : S.EHClauses)
            ++Uses[Clause.HandlerVA];
          if (S.Addr != 0 && S.Addr != InvalidVA &&
              (I == 0 || L[I - 1].Addr != S.Addr))
            ++Starts[S.Addr];
          Count(S.Body);
          Count(S.ElseBody);
          for (const auto &C : S.Cases)
            Count(C.Body);
          Count(S.DefaultBody);
          for (const auto &ClauseBody : S.EHClauseBodies)
            Count(ClauseBody);
        }
      };
  Count(Body);
  // Turn the jumps to \p X in \p L into `continue` of a loop around \p L:
  // those reached through if/else arms, blocks and switch cases, since a
  // switch owns no continue.  A jump carrying an entered label leaves an
  // empty block for it.  Returns how many were turned.
  std::function<unsigned(std::vector<HighStmt> &, va_t)> Jumps =
      [&](std::vector<HighStmt> &L, va_t X) {
        unsigned N = 0;
        for (size_t I = 0; I < L.size(); ++I) {
          HighStmt &S = L[I];
          if (S.Kind == StmtKind::Goto && S.GotoTarget == X) {
            ++N;
            HighStmt Next;
            Next.Kind = StmtKind::Continue;
            if (S.Addr != 0 && S.Addr != InvalidVA && Uses.count(S.Addr) &&
                (I == 0 || L[I - 1].Addr != S.Addr)) {
              HighStmt Anchor;
              Anchor.Kind = StmtKind::Block;
              Anchor.Addr = S.Addr;
              S = std::move(Anchor);
              L.insert(L.begin() + static_cast<ptrdiff_t>(++I),
                       std::move(Next));
            } else {
              S = std::move(Next);
            }
            continue;
          }
          switch (S.Kind) {
          case StmtKind::If:
          case StmtKind::IfElse:
          case StmtKind::Block:
            N += Jumps(S.Body, X);
            N += Jumps(S.ElseBody, X);
            break;
          case StmtKind::Switch:
            for (auto &C : S.Cases)
              N += Jumps(C.Body, X);
            N += Jumps(S.DefaultBody, X);
            break;
          default:
            break;
          }
        }
        return N;
      };
  // The targets of the jumps Jumps turns in \p S.
  std::function<void(const HighStmt &, std::set<va_t> &)> Targets =
      [&](const HighStmt &S, std::set<va_t> &Out) {
        switch (S.Kind) {
        case StmtKind::Goto:
          Out.insert(S.GotoTarget);
          break;
        case StmtKind::If:
        case StmtKind::IfElse:
        case StmtKind::Block:
          for (const auto *List : {&S.Body, &S.ElseBody})
            for (const HighStmt &Child : *List)
              Targets(Child, Out);
          break;
        case StmtKind::Switch:
          for (const auto &C : S.Cases)
            for (const HighStmt &Child : C.Body)
              Targets(Child, Out);
          for (const HighStmt &Child : S.DefaultBody)
            Targets(Child, Out);
          break;
        default:
          break;
        }
      };
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &L) {
        for (HighStmt &S : L) {
          Visit(S.Body);
          Visit(S.ElseBody);
          for (auto &C : S.Cases)
            Visit(C.Body);
          Visit(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            Visit(ClauseBody);
        }
        std::vector<std::set<va_t>> Reach(L.size());
        for (size_t I = 0; I < L.size(); ++I)
          Targets(L[I], Reach[I]);
        for (size_t K = 0; K < L.size(); ++K) {
          const va_t X = L[K].Addr;
          if (X == 0 || X == InvalidVA || (K != 0 && L[K - 1].Addr == X) ||
              Starts[X] != 1 || !Uses.count(X))
            continue;
          // The last statement after the label holding such a jump.
          size_t Last = K;
          for (size_t I = K + 1; I < L.size(); ++I)
            if (Reach[I].count(X))
              Last = I;
          if (Last == K)
            continue;
          std::vector<HighStmt> LoopBody(L.begin() + K, L.begin() + Last + 1);
          if (hasLooseBreakOrContinue(LoopBody))
            continue;
          // Jumps from the label's own statement onward restart the loop.
          const unsigned Replaced = Jumps(LoopBody, X);
          HighStmt Leave;
          Leave.Kind = StmtKind::Break;
          LoopBody.push_back(std::move(Leave));
          HighStmt Loop;
          Loop.Kind = StmtKind::While;
          Loop.Cond = HighExpr::makeConst(1, 1);
          Loop.LoopHeaderAddr = X;
          Loop.Body = std::move(LoopBody);
          Uses[X] -= Replaced;
          if (!Uses[X])
            Uses.erase(X);
          L.erase(L.begin() + static_cast<ptrdiff_t>(K),
                  L.begin() + static_cast<ptrdiff_t>(Last) + 1);
          L.insert(L.begin() + static_cast<ptrdiff_t>(K), std::move(Loop));
          Reach.erase(Reach.begin() + static_cast<ptrdiff_t>(K),
                      Reach.begin() + static_cast<ptrdiff_t>(Last) + 1);
          Reach.insert(Reach.begin() + static_cast<ptrdiff_t>(K),
                       std::set<va_t>());
          Targets(L[K], Reach[K]);
          Changed = true;
        }
      };
  Visit(Body);
  return Changed;
}

bool loopJumpsAsBreakAndContinue(std::vector<HighStmt> &Body) {
  std::map<va_t, unsigned> Uses, Starts;
  std::function<void(const std::vector<HighStmt> &, va_t)> Count =
      [&](const std::vector<HighStmt> &L, va_t Parent) {
        for (size_t I = 0; I < L.size(); ++I) {
          const HighStmt &S = L[I];
          if (S.Kind == StmtKind::Goto)
            ++Uses[S.GotoTarget];
          for (const HighEHClause &Clause : S.EHClauses)
            ++Uses[Clause.HandlerVA];
          // A first child sharing its parent's address prints under the
          // parent's label.
          if (S.Addr != 0 && S.Addr != InvalidVA &&
              S.Addr != (I == 0 ? Parent : L[I - 1].Addr))
            ++Starts[S.Addr];
          if (S.Kind == StmtKind::While && S.LoopHeaderAddr != 0 &&
              S.LoopHeaderAddr != InvalidVA && S.LoopHeaderAddr != S.Addr)
            ++Starts[S.LoopHeaderAddr];
          Count(S.Body, S.Addr);
          Count(S.ElseBody, 0);
          for (const auto &C : S.Cases)
            Count(C.Body, 0);
          Count(S.DefaultBody, 0);
          for (const auto &ClauseBody : S.EHClauseBodies)
            Count(ClauseBody, 0);
        }
      };
  Count(Body, 0);
  auto Unique = [&](va_t Addr) {
    return Addr != 0 && Addr != InvalidVA && Starts[Addr] == 1;
  };
  // Turn the jumps to \p Targets in \p L into \p Kind.  A loop owns both
  // break and continue, and a switch owns break, so neither is entered for
  // the kind it owns; a try is not entered.  A jump carrying an entered
  // label leaves an empty block for it.
  std::function<unsigned(std::vector<HighStmt> &, const std::set<va_t> &,
                         StmtKind)>
      Turn = [&](std::vector<HighStmt> &L, const std::set<va_t> &Targets,
                 StmtKind Kind) {
        unsigned N = 0;
        for (size_t I = 0; I < L.size(); ++I) {
          HighStmt &S = L[I];
          if (S.Kind == StmtKind::Goto && Targets.count(S.GotoTarget)) {
            if (--Uses[S.GotoTarget] == 0)
              Uses.erase(S.GotoTarget);
            ++N;
            HighStmt Jump;
            Jump.Kind = Kind;
            if (S.Addr != 0 && S.Addr != InvalidVA && Uses.count(S.Addr) &&
                (I == 0 || L[I - 1].Addr != S.Addr)) {
              HighStmt Anchor;
              Anchor.Kind = StmtKind::Block;
              Anchor.Addr = S.Addr;
              S = std::move(Anchor);
              L.insert(L.begin() + static_cast<ptrdiff_t>(++I),
                       std::move(Jump));
            } else {
              S = std::move(Jump);
            }
            continue;
          }
          switch (S.Kind) {
          case StmtKind::If:
          case StmtKind::IfElse:
          case StmtKind::Block:
            N += Turn(S.Body, Targets, Kind);
            N += Turn(S.ElseBody, Targets, Kind);
            break;
          case StmtKind::Switch:
            if (Kind == StmtKind::Continue) {
              for (auto &C : S.Cases)
                N += Turn(C.Body, Targets, Kind);
              N += Turn(S.DefaultBody, Targets, Kind);
            }
            break;
          default:
            break;
          }
        }
        return N;
      };
  auto IsAnchor = [](const HighStmt &S) {
    return S.Kind == StmtKind::Nop ||
           (S.Kind == StmtKind::Block && S.Body.empty());
  };
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &L) {
        for (size_t I = 0; I < L.size(); ++I) {
          HighStmt &S = L[I];
          const bool Loop = S.Kind == StmtKind::While ||
                            S.Kind == StmtKind::DoWhile ||
                            S.Kind == StmtKind::For;
          if (Loop) {
            // What runs after the loop: the anchors after it and the
            // statement they lead to.
            std::set<va_t> After;
            for (size_t J = I + 1; J < L.size(); ++J) {
              if (Unique(L[J].Addr) && L[J].Addr != L[J - 1].Addr)
                After.insert(L[J].Addr);
              if (!IsAnchor(L[J]))
                break;
            }
            if (!After.empty())
              Changed |= Turn(S.Body, After, StmtKind::Break) != 0;
            // A while loop's test, and for an always-true one the top of
            // its body.
            std::set<va_t> Top;
            if (S.Kind == StmtKind::While) {
              // The header's label prints before the loop only as the loop
              // statement's own address.
              if (S.LoopHeaderAddr == S.Addr && Unique(S.Addr) &&
                  (I == 0 || L[I - 1].Addr != S.Addr))
                Top.insert(S.Addr);
              const bool Always = S.Cond && S.Cond->Kind == ExprKind::Const &&
                                  S.Cond->ConstVal != 0;
              for (size_t J = 0; Always && J < S.Body.size(); ++J) {
                const va_t Addr = S.Body[J].Addr;
                if (Addr != (J == 0 ? S.Addr : S.Body[J - 1].Addr) &&
                    Unique(Addr))
                  Top.insert(Addr);
                if (!IsAnchor(S.Body[J]))
                  break;
              }
            }
            if (!Top.empty())
              Changed |= Turn(S.Body, Top, StmtKind::Continue) != 0;
          }
          Visit(S.Body);
          Visit(S.ElseBody);
          for (auto &C : S.Cases)
            Visit(C.Body);
          Visit(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            Visit(ClauseBody);
        }
      };
  Visit(Body);
  return Changed;
}

bool foldTempsInReturnTails(std::vector<HighStmt> &Body) {
  std::set<va_t> Entered = gotoTargets(Body);
  walkStmts(Body, [&](const HighStmt &S) {
    for (const HighEHClause &Clause : S.EHClauses)
      Entered.insert(Clause.HandlerVA);
  });
  // How often \p E reads \p K, and which variables it reads.
  auto Reads = [](const HighExpr *E, const VarKey &K) { return readsOf(E, K); };
  std::function<void(const HighExpr *, std::set<VarKey> &)> ReadSet =
      [&](const HighExpr *E, std::set<VarKey> &Out) {
        if (!E)
          return;
        if (E->Kind == ExprKind::Var)
          Out.insert(varKey(E->Var));
        E->forEachChildExpr([&](const ExprPtr &Op) { ReadSet(Op.get(), Out); });
      };
  // \p E with its reads of \p K replaced by \p With; nodes off that path are
  // shared, the ones on it copied.
  std::function<ExprPtr(const ExprPtr &, const VarKey &, const ExprPtr &)>
      Replace = [&](const ExprPtr &E, const VarKey &K,
                    const ExprPtr &With) -> ExprPtr {
    if (!E || !Reads(E.get(), K))
      return E;
    if (E->Kind == ExprKind::Var)
      return With;
    auto Copy = std::make_shared<HighExpr>(*E);
    for (ExprPtr &Op : Copy->Operands)
      Op = Replace(Op, K, With);
    return Copy;
  };
  auto ReadOf = [](HighStmt &S) -> ExprPtr * {
    if (S.Kind == StmtKind::Return)
      return &S.RetVal;
    if (S.Kind == StmtKind::Assign)
      return &S.Val;
    return nullptr;
  };
  auto WrittenVar = [](const HighStmt &S) -> std::optional<VarKey> {
    if (S.Kind == StmtKind::Assign && S.Dst &&
        (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi))
      return varKey(S.Dst->Var);
    return std::nullopt;
  };
  // Whether \p E calls, stores or orders memory (and so must stay put), or
  // reads memory (and so may move only past statements that write none).
  std::function<bool(const HighExpr *)> Pinned = [&](const HighExpr *E) {
    if (!E)
      return false;
    if (E->Kind == ExprKind::Call || E->Kind == ExprKind::Store ||
        E->MemoryOrdering != NdMemoryOrdering::None)
      return true;
    bool Found = false;
    E->forEachChildExpr([&](const ExprPtr &Op) { Found |= Pinned(Op.get()); });
    return Found;
  };
  std::function<bool(const HighExpr *)> ReadsMemory = [&](const HighExpr *E) {
    if (!E)
      return false;
    if (E->Kind == ExprKind::Load)
      return true;
    bool Found = false;
    E->forEachChildExpr(
        [&](const ExprPtr &Op) { Found |= ReadsMemory(Op.get()); });
    return Found;
  };
  // Only temporaries a copied tail assigns again are left for this pass:
  // one assignment and one read the general inlining already takes.  Each of
  // their reads must also follow an assignment in its own run, as in copies
  // of a tail; a temporary read elsewhere may hold a frame slot another path
  // or a handler sees.
  std::map<VarKey, unsigned> Defs, Reads_, Covered;
  walkStmts(Body, [&](HighStmt &S) {
    if (std::optional<VarKey> W = WrittenVar(S))
      ++Defs[*W];
    forEachExpr(S, [&](ExprPtr &E) {
      // An assigned variable is written, not read.
      if (&E == &S.Dst && S.Kind == StmtKind::Assign)
        return;
      std::set<VarKey> Seen;
      ReadSet(E.get(), Seen);
      for (const VarKey &K : Seen)
        Reads_[K] += Reads(E.get(), K);
    });
  });
  std::function<void(std::vector<HighStmt> &)> CountCovered =
      [&](std::vector<HighStmt> &L) {
        for (HighStmt &S : L) {
          CountCovered(S.Body);
          CountCovered(S.ElseBody);
          for (auto &C : S.Cases)
            CountCovered(C.Body);
          CountCovered(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            CountCovered(ClauseBody);
        }
        for (size_t R = 0; R < L.size(); ++R) {
          if (L[R].Kind != StmtKind::Return)
            continue;
          size_t Start = R;
          while (Start > 0 && (L[Start - 1].Kind == StmtKind::Nop ||
                               (L[Start - 1].Kind == StmtKind::Block &&
                                L[Start - 1].Body.empty()) ||
                               WrittenVar(L[Start - 1])))
            --Start;
          std::set<VarKey> Assigned;
          for (size_t J = Start; J <= R; ++J) {
            if (ExprPtr *Read = ReadOf(L[J])) {
              std::set<VarKey> Seen;
              ReadSet(Read->get(), Seen);
              for (const VarKey &K : Seen)
                if (Assigned.count(K))
                  Covered[K] += Reads(Read->get(), K);
            }
            if (std::optional<VarKey> W = WrittenVar(L[J]))
              Assigned.insert(*W);
          }
        }
      };
  CountCovered(Body);
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &L) {
        for (HighStmt &S : L) {
          Visit(S.Body);
          Visit(S.ElseBody);
          for (auto &C : S.Cases)
            Visit(C.Body);
          Visit(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            Visit(ClauseBody);
        }
        for (size_t R = 0; R < L.size(); ++R) {
          if (L[R].Kind != StmtKind::Return)
            continue;
          size_t Start = R;
          while (Start > 0) {
            const HighStmt &P = L[Start - 1];
            if (P.Kind != StmtKind::Nop &&
                !(P.Kind == StmtKind::Block && P.Body.empty()) &&
                !WrittenVar(P))
              break;
            --Start;
          }
          // Whether control can enter at \p J without running what precedes.
          auto EnteredAt = [&](size_t J) {
            return L[J].Addr && L[J].Addr != InvalidVA &&
                   Entered.count(L[J].Addr) && L[J - 1].Addr != L[J].Addr;
          };
          for (size_t I = Start; I < R;) {
            HighStmt &Def = L[I];
            if (Def.Kind != StmtKind::Assign || !Def.Dst || !Def.Val ||
                Def.Dst->Kind != ExprKind::Var ||
                Def.Dst->Var.Kind != MedVar::Temp ||
                Defs[varKey(Def.Dst->Var)] < 2 ||
                Covered[varKey(Def.Dst->Var)] != Reads_[varKey(Def.Dst->Var)] ||
                Pinned(Def.Val.get()) ||
                (Def.Addr && Def.Addr != InvalidVA && Entered.count(Def.Addr) &&
                 (I == 0 || L[I - 1].Addr != Def.Addr))) {
              ++I;
              continue;
            }
            const VarKey K = varKey(Def.Dst->Var);
            // `v = v + x` updates a variable, typically a frame slot; that
            // write stays visible.
            std::set<VarKey> Sources;
            ReadSet(Def.Val.get(), Sources);
            if (llvm::any_of(Sources, [&](const VarKey &S) {
                  return S.first == K.first;
                })) {
              ++I;
              continue;
            }
            unsigned Uses = 0;
            size_t UseAt = 0;
            bool Rewritten = false;
            for (size_t J = I + 1; J <= R && !Rewritten; ++J) {
              if (ExprPtr *Read = ReadOf(L[J]))
                if (const unsigned N = Reads(Read->get(), K)) {
                  Uses += N;
                  UseAt = J;
                }
              Rewritten = WrittenVar(L[J]) == K;
            }
            bool Fold = false;
            if (!Rewritten && Uses == 1) {
              std::set<VarKey> Operands;
              ReadSet(Def.Val.get(), Operands);
              const bool Memory = ReadsMemory(Def.Val.get());
              Fold = true;
              for (size_t J = I + 1; J <= UseAt && Fold; ++J) {
                if (EnteredAt(J))
                  Fold = false;
                else if (J < UseAt)
                  if (std::optional<VarKey> W = WrittenVar(L[J]))
                    // A variable other than a temporary may live in memory
                    // the moved read sees.
                    Fold = !Operands.count(*W) &&
                           (!Memory || L[J].Dst->Var.Kind == MedVar::Temp);
              }
              if (Fold)
                *ReadOf(L[UseAt]) = Replace(*ReadOf(L[UseAt]), K, Def.Val);
            }
            if (Fold ||
                (!Rewritten && Uses == 0 && !exprMayFault(Def.Val.get()))) {
              L.erase(L.begin() + static_cast<ptrdiff_t>(I));
              --R;
              Changed = true;
              continue;
            }
            ++I;
          }
        }
      };
  Visit(Body);
  return Changed;
}

/// True when \p Stmts contains a continue that would restart a loop wrapped
/// around it. Only a nested loop owns a continue.
static bool hasLooseContinue(const std::vector<HighStmt> &Stmts) {
  for (const HighStmt &S : Stmts) {
    switch (S.Kind) {
    case StmtKind::Continue:
      return true;
    case StmtKind::While:
    case StmtKind::DoWhile:
    case StmtKind::For:
      break;
    default:
      if (hasLooseContinue(S.Body) || hasLooseContinue(S.ElseBody) ||
          hasLooseContinue(S.DefaultBody))
        return true;
      for (const auto &C : S.Cases)
        if (hasLooseContinue(C.Body))
          return true;
      for (const auto &ClauseBody : S.EHClauseBodies)
        if (hasLooseContinue(ClauseBody))
          return true;
      break;
    }
  }
  return false;
}

/// A statement that does nothing: a Nop, or an empty block kept as the anchor
/// of a label.
static bool isEmptyAnchor(const HighStmt &S) {
  return S.Kind == StmtKind::Nop ||
         (S.Kind == StmtKind::Block &&
          std::all_of(S.Body.begin(), S.Body.end(), isEmptyAnchor));
}

/// True when \p Entered holds the address of \p S or of a statement nested
/// in it.
template <typename Pred>
static bool anyAddressEntered(const HighStmt &S, const Pred &Entered) {
  if (Entered(S.Addr))
    return true;
  for (const auto *List : {&S.Body, &S.ElseBody, &S.DefaultBody})
    for (const HighStmt &T : *List)
      if (anyAddressEntered(T, Entered))
        return true;
  for (const auto &C : S.Cases)
    for (const HighStmt &T : C.Body)
      if (anyAddressEntered(T, Entered))
        return true;
  for (const auto &ClauseBody : S.EHClauseBodies)
    for (const HighStmt &T : ClauseBody)
      if (anyAddressEntered(T, Entered))
        return true;
  return false;
}

/// A statement after which control never reaches the next one: a return or
/// jump, a call to a known noreturn function (KeBugCheckEx, ExRaiseStatus...),
/// a terminating trap such as the fail-fast, or an endless loop.
static bool endsItsBlock(const HighStmt &S) {
  if (S.Kind == StmtKind::Return || S.Kind == StmtKind::Goto ||
      isEndlessLoop(S))
    return true;
  if (S.Kind == StmtKind::IfElse) {
    // An empty statement with an address may be a label a jump reaches, and
    // then the arm runs past its end.
    auto ArmEnds = [](const std::vector<HighStmt> &Arm) {
      auto Last = std::find_if(Arm.rbegin(), Arm.rend(), [](const HighStmt &T) {
        return !isEmptyAnchor(T) || (T.Addr != 0 && T.Addr != InvalidVA);
      });
      return Last != Arm.rend() && endsItsBlock(*Last);
    };
    return ArmEnds(S.Body) && ArmEnds(S.ElseBody);
  }
  const ExprPtr &Call = S.Kind == StmtKind::Call ? S.CallExpr : S.Val;
  if ((S.Kind != StmtKind::Call && S.Kind != StmtKind::Assign &&
       S.Kind != StmtKind::ExprStmt) ||
      !Call || Call->Kind != ExprKind::Call)
    return false;
  return isTerminatingHighCall(Call) || Call->DoesNotReturn ||
         (Call->IntrinsicId == Intrinsic::None && !Call->CallTarget.empty() &&
          libc::isNoReturnFunction(Call->CallTarget));
}

bool highStmtEndsItsBlock(const HighStmt &S) { return endsItsBlock(S); }

/// True when \p Stmts holds a `break` that leaves the switch around them: one
/// outside every nested loop and switch.
static bool hasSwitchBreak(const std::vector<HighStmt> &Stmts) {
  for (const HighStmt &S : Stmts) {
    if (S.Kind == StmtKind::Break)
      return true;
    if (S.Kind == StmtKind::While || S.Kind == StmtKind::DoWhile ||
        S.Kind == StmtKind::For || S.Kind == StmtKind::Switch)
      continue;
    if (hasSwitchBreak(S.Body) || hasSwitchBreak(S.ElseBody))
      return true;
    for (const auto &ClauseBody : S.EHClauseBodies)
      if (hasSwitchBreak(ClauseBody))
        return true;
  }
  return false;
}

static bool armNeverFallsOut(const std::vector<HighStmt> &Arm);

/// True when control never runs from \p S into the statement after it: it
/// ends its block, leaves for an enclosing loop or switch, or is an if/else,
/// block or switch whose every way out does.  A switch also needs a default
/// and no case that breaks out of it.
static bool stmtNeverFallsOut(const HighStmt &S) {
  if (S.Kind == StmtKind::Break || S.Kind == StmtKind::Continue ||
      endsItsBlock(S))
    return true;
  if (S.Kind == StmtKind::IfElse)
    return armNeverFallsOut(S.Body) && armNeverFallsOut(S.ElseBody);
  if (S.Kind == StmtKind::Block)
    return armNeverFallsOut(S.Body);
  if (S.Kind != StmtKind::Switch || S.DefaultBody.empty() ||
      !armNeverFallsOut(S.DefaultBody) || hasSwitchBreak(S.DefaultBody))
    return false;
  return std::all_of(S.Cases.begin(), S.Cases.end(), [](const SwitchCase &C) {
    return C.FallsThrough ||
           (armNeverFallsOut(C.Body) && !hasSwitchBreak(C.Body));
  });
}

static bool armNeverFallsOut(const std::vector<HighStmt> &Arm) {
  // An empty statement with an address may be a label a jump reaches, and
  // then the arm runs past its end.
  auto Last = std::find_if(Arm.rbegin(), Arm.rend(), [](const HighStmt &T) {
    return !isEmptyAnchor(T) || (T.Addr != 0 && T.Addr != InvalidVA);
  });
  return Last != Arm.rend() && stmtNeverFallsOut(*Last);
}

/// Two statement lists of assignments, stores, returns and gotos that do the
/// same thing and contain no entered label.
static bool sameStraightLineBody(const std::vector<HighStmt> &A,
                                 const std::vector<HighStmt> &B) {
  if (A.size() != B.size())
    return false;
  for (size_t I = 0; I < A.size(); ++I) {
    const HighStmt &X = A[I], &Y = B[I];
    if (X.Kind != Y.Kind)
      return false;
    switch (X.Kind) {
    case StmtKind::Assign:
      if (!exprEquivalent(X.Dst, Y.Dst) || !exprEquivalent(X.Val, Y.Val))
        return false;
      break;
    case StmtKind::Return:
      if ((X.RetVal || Y.RetVal) && !exprEquivalent(X.RetVal, Y.RetVal))
        return false;
      break;
    case StmtKind::Goto:
      if (X.GotoTarget != Y.GotoTarget)
        return false;
      break;
    case StmtKind::Nop:
      break;
    default:
      return false;
    }
  }
  return true;
}

/// True when a jump to \p X in \p Stmts sits in a __finally or cleanup body.
/// Turning it into `continue` would leave a termination handler abnormally.
static bool jumpsOutOfATerminationHandler(const std::vector<HighStmt> &Stmts,
                                          va_t X, bool InHandler = false) {
  for (const HighStmt &S : Stmts) {
    if (InHandler && S.Kind == StmtKind::Goto && S.GotoTarget == X)
      return true;
    if (jumpsOutOfATerminationHandler(S.Body, X, InHandler) ||
        jumpsOutOfATerminationHandler(S.ElseBody, X, InHandler) ||
        jumpsOutOfATerminationHandler(S.DefaultBody, X, InHandler))
      return true;
    for (const auto &C : S.Cases)
      if (jumpsOutOfATerminationHandler(C.Body, X, InHandler))
        return true;
    for (size_t I = 0; I < S.EHClauseBodies.size(); ++I) {
      const bool Termination =
          I < S.EHClauses.size() &&
          (S.EHClauses[I].Kind == HighEHClauseKind::SEHFinally ||
           S.EHClauses[I].Kind == HighEHClauseKind::CxxCleanup);
      if (jumpsOutOfATerminationHandler(S.EHClauseBodies[I], X,
                                        InHandler || Termination))
        return true;
    }
  }
  return false;
}

bool loopifyBackwardGotos(std::vector<HighStmt> &Body) {
  std::map<va_t, unsigned> Uses;
  std::set<va_t> Pinned;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      ++Uses[S.GotoTarget];
    for (const HighEHClause &Clause : S.EHClauses)
      if (Clause.HandlerVA != 0 && Clause.HandlerVA != InvalidVA)
        Pinned.insert(Clause.HandlerVA);
  });
  // Gotos to \p X inside \p S, or -1 when one sits in a nested loop (where
  // `continue` would bind to that loop).
  std::function<int(const HighStmt &, va_t, bool)> Count =
      [&](const HighStmt &S, va_t X, bool InLoop) -> int {
    if (S.Kind == StmtKind::Goto && S.GotoTarget == X)
      return InLoop ? -1 : 1;
    const bool Loop = S.Kind == StmtKind::While ||
                      S.Kind == StmtKind::DoWhile || S.Kind == StmtKind::For;
    int N = 0;
    auto Add = [&](const std::vector<HighStmt> &L) {
      for (const HighStmt &C : L) {
        if (N < 0)
          return;
        const int M = Count(C, X, InLoop || Loop);
        N = M < 0 ? -1 : N + M;
      }
    };
    Add(S.Body);
    Add(S.ElseBody);
    for (const auto &C : S.Cases)
      Add(C.Body);
    Add(S.DefaultBody);
    for (const auto &ClauseBody : S.EHClauseBodies)
      Add(ClauseBody);
    return N;
  };
  std::function<void(std::vector<HighStmt> &, va_t)> ToContinue =
      [&](std::vector<HighStmt> &L, va_t X) {
        for (HighStmt &S : L) {
          if (S.Kind == StmtKind::Goto && S.GotoTarget == X) {
            S.Kind = StmtKind::Continue;
            S.GotoTarget = 0;
            continue;
          }
          ToContinue(S.Body, X);
          ToContinue(S.ElseBody, X);
          for (auto &C : S.Cases)
            ToContinue(C.Body, X);
          ToContinue(S.DefaultBody, X);
          for (auto &ClauseBody : S.EHClauseBodies)
            ToContinue(ClauseBody, X);
        }
      };
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &L) {
        for (size_t K = 0; K < L.size(); ++K) {
          const va_t X = L[K].Addr;
          if (X == 0 || X == InvalidVA || Pinned.count(X) ||
              (K > 0 && L[K - 1].Addr == X) || !Uses.count(X))
            continue;
          // The jumps to X from K onward in this list close the loop. A jump
          // from elsewhere still lands on X, the first statement of the loop
          // body, which enters the loop exactly as falling into it does.
          size_t M = K;
          int Inside = 0;
          bool Bad = false;
          for (size_t J = K; J < L.size() && !Bad; ++J) {
            const int N = Count(L[J], X, false);
            if (N < 0)
              Bad = true;
            else if (N > 0) {
              Inside += N;
              M = J;
            }
          }
          if (Bad || Inside == 0)
            continue;
          std::vector<HighStmt> Region(
              std::make_move_iterator(L.begin() + K),
              std::make_move_iterator(L.begin() + M + 1));
          // A jump out of a try body or an __except handler back to X leaves
          // it the way `continue` does, running any termination handler on
          // the way. A jump out of a termination handler itself stays.
          if (hasLooseBreakOrContinue(Region) ||
              jumpsOutOfATerminationHandler(Region, X)) {
            std::move(Region.begin(), Region.end(), L.begin() + K);
            continue;
          }
          ToContinue(Region, X);
          const StmtKind LastKind = Region.back().Kind;
          if (LastKind != StmtKind::Return && LastKind != StmtKind::Goto &&
              LastKind != StmtKind::Continue) {
            HighStmt Break;
            Break.Kind = StmtKind::Break;
            Region.push_back(std::move(Break));
          }
          HighStmt Loop;
          Loop.Kind = StmtKind::While;
          Loop.Body = std::move(Region);
          L.erase(L.begin() + K + 1, L.begin() + M + 1);
          L[K] = std::move(Loop);
          if ((Uses[X] -= static_cast<unsigned>(Inside)) == 0)
            Uses.erase(X);
          Changed = true;
        }
        for (HighStmt &S : L) {
          Visit(S.Body);
          Visit(S.ElseBody);
          for (auto &C : S.Cases)
            Visit(C.Body);
          Visit(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            Visit(ClauseBody);
        }
      };
  Visit(Body);
  return Changed;
}

bool flattenBlocks(std::vector<HighStmt> &Body) {
  std::set<va_t> Targets;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      Targets.insert(S.GotoTarget);
    for (const HighEHClause &Clause : S.EHClauses)
      if (Clause.HandlerVA != 0 && Clause.HandlerVA != InvalidVA)
        Targets.insert(Clause.HandlerVA);
  });
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &L) {
        for (size_t I = 0; I < L.size(); ++I) {
          // A block groups statements and nothing else: its statements run
          // in the enclosing list just the same. An entered block address
          // keeps an empty anchor ahead of them.
          if (L[I].Kind != StmtKind::Block || L[I].Body.empty() ||
              isEmptyAnchor(L[I]))
            continue;
          std::vector<HighStmt> Inner = std::move(L[I].Body);
          const va_t Addr = L[I].Addr;
          const bool Anchor =
              Addr != 0 && Addr != InvalidVA && Targets.count(Addr) &&
              (Inner.front().Addr != Addr || (I > 0 && L[I - 1].Addr == Addr));
          if (Anchor) {
            L[I].Body.clear();
            L.insert(L.begin() + I + 1, std::make_move_iterator(Inner.begin()),
                     std::make_move_iterator(Inner.end()));
          } else {
            L.erase(L.begin() + I);
            L.insert(L.begin() + I, std::make_move_iterator(Inner.begin()),
                     std::make_move_iterator(Inner.end()));
            --I;
          }
          Changed = true;
        }
        for (HighStmt &S : L) {
          Visit(S.Body);
          Visit(S.ElseBody);
          for (auto &C : S.Cases)
            Visit(C.Body);
          Visit(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            Visit(ClauseBody);
        }
      };
  Visit(Body);
  return Changed;
}

bool hoistLoopEntryLabels(std::vector<HighStmt> &Body) {
  std::map<va_t, unsigned> Uses;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      ++Uses[S.GotoTarget];
    for (const HighEHClause &Clause : S.EHClauses)
      if (Clause.HandlerVA != 0 && Clause.HandlerVA != InvalidVA)
        Uses[Clause.HandlerVA] = ~0u;
  });
  bool Changed = false;
  walkStmts(Body, [&](HighStmt &S) {
    // Entering `while (1)` or a do-while at the top of its body is
    // entering the loop.
    const bool Forever =
        !S.Cond || (S.Cond->Kind == ExprKind::Const && S.Cond->ConstVal != 0);
    const bool TopIsEntry =
        S.Kind == StmtKind::DoWhile || (S.Kind == StmtKind::While && Forever);
    if (!TopIsEntry || S.Body.empty() || (S.Addr != 0 && S.Addr != InvalidVA))
      return;
    const va_t X = S.Body.front().Addr;
    auto It = Uses.find(X);
    if (X == 0 || X == InvalidVA || It == Uses.end() || It->second == ~0u)
      return;
    unsigned Inside = 0;
    walkStmts(S.Body, [&](const HighStmt &C) {
      Inside += C.Kind == StmtKind::Goto && C.GotoTarget == X;
    });
    if (Inside != 0)
      return;
    for (HighStmt &C : S.Body) {
      if (C.Addr != X)
        break;
      C.Addr = 0;
    }
    S.Addr = X;
    Changed = true;
  });
  return Changed;
}

/// True when a jump reaches the start of \p Loop's body: its first real
/// statement, or an anchor ahead of it.
template <typename Pred>
static bool loopTopEntered(const HighStmt &Loop, const Pred &Entered) {
  if (Entered(Loop.Addr))
    return true;
  for (const HighStmt &T : Loop.Body) {
    if (!isEmptyAnchor(T))
      return Entered(T.Addr);
    if (anyAddressEntered(T, Entered))
      return true;
  }
  return false;
}

bool rotateLoopsToTheirEntry(std::vector<HighStmt> &Body) {
  std::map<va_t, unsigned> Uses;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      ++Uses[S.GotoTarget];
    for (const HighEHClause &Clause : S.EHClauses)
      if (Clause.HandlerVA != 0 && Clause.HandlerVA != InvalidVA)
        Uses[Clause.HandlerVA] = ~0u;
  });
  auto Entered = [&](va_t Addr) {
    return Addr != 0 && Addr != InvalidVA && Uses.count(Addr);
  };
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &L) {
        for (size_t K = 1; K < L.size(); ++K) {
          // `jump; while (1) { S1; X: S2 }` with X entered from outside.
          // Nothing reaches S1 before X, so every run is S2 S1 S2 S1...
          HighStmt &Loop = L[K];
          const bool Forever =
              !Loop.Cond ||
              (Loop.Cond->Kind == ExprKind::Const && Loop.Cond->ConstVal != 0);
          if (Loop.Kind != StmtKind::While || !Forever ||
              Loop.Body.size() < 2 || loopTopEntered(Loop, Entered))
            continue;
          size_t P = K;
          bool TopEntered = false;
          while (P > 0 && isEmptyAnchor(L[P - 1]))
            TopEntered |= anyAddressEntered(L[--P], Entered);
          if (TopEntered || P == 0 || !endsItsBlock(L[P - 1]))
            continue;
          // A continue would restart at S2 instead of S1.
          if (hasLooseContinue(Loop.Body))
            continue;
          size_t J = 1;
          while (J < Loop.Body.size() &&
                 !(Entered(Loop.Body[J].Addr) &&
                   Loop.Body[J - 1].Addr != Loop.Body[J].Addr))
            ++J;
          if (J >= Loop.Body.size())
            continue;
          std::rotate(Loop.Body.begin(), Loop.Body.begin() + J,
                      Loop.Body.end());
          Changed = true;
        }
        for (size_t K = 1; K < L.size(); ++K) {
          // `jump; do { S; X: } while (c);` entered only at X, the test:
          // every run is c S c S..., so it is `X: while (c) { S }`. A
          // continue goes to the test in both forms.
          HighStmt &Loop = L[K];
          if (Loop.Kind != StmtKind::DoWhile || !Loop.Cond ||
              Loop.Body.size() < 2 || loopTopEntered(Loop, Entered))
            continue;
          size_t P = K;
          bool TopEntered = false;
          while (P > 0 && isEmptyAnchor(L[P - 1]))
            TopEntered |= anyAddressEntered(L[--P], Entered);
          if (TopEntered || P == 0 || !endsItsBlock(L[P - 1]))
            continue;
          size_t J = Loop.Body.size();
          while (J > 1 && isEmptyAnchor(Loop.Body[J - 1]))
            --J;
          if (J == Loop.Body.size() || J == 0)
            continue;
          const va_t X = Loop.Body[J].Addr;
          if (!Entered(X) || Loop.Body[J - 1].Addr == X)
            continue;
          bool OnlyX = true;
          for (size_t Q = J; Q < Loop.Body.size(); ++Q)
            OnlyX &= !anyAddressEntered(
                Loop.Body[Q], [&](va_t A) { return A != X && Entered(A); });
          if (!OnlyX)
            continue;
          Loop.Body.resize(J);
          Loop.Kind = StmtKind::While;
          Loop.Addr = X;
          Changed = true;
        }
        for (HighStmt &S : L) {
          Visit(S.Body);
          Visit(S.ElseBody);
          for (auto &C : S.Cases)
            Visit(C.Body);
          Visit(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            Visit(ClauseBody);
        }
      };
  Visit(Body);
  return Changed;
}

bool hoistLoopExitTests(std::vector<HighStmt> &Body) {
  std::map<va_t, unsigned> Uses;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      ++Uses[S.GotoTarget];
    for (const HighEHClause &Clause : S.EHClauses)
      if (Clause.HandlerVA != 0 && Clause.HandlerVA != InvalidVA)
        Uses[Clause.HandlerVA] = ~0u;
  });
  auto Entered = [&](va_t Addr) {
    return Addr != 0 && Addr != InvalidVA && Uses.count(Addr);
  };
  auto ExitTest = [&](const HighStmt &S) {
    return S.Kind == StmtKind::If && S.Cond && S.ElseBody.empty() &&
           S.Body.size() == 1 && S.Body[0].Kind == StmtKind::Goto &&
           S.Body[0].GotoTarget != 0 && S.Body[0].GotoTarget != InvalidVA &&
           !Entered(S.Addr);
  };
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &L) {
        for (size_t K = 0; K < L.size(); ++K) {
          HighStmt &Loop = L[K];
          if (Loop.Kind != StmtKind::While || !isEndlessLoop(Loop))
            continue;
          auto &LoopBody = Loop.Body;
          size_t First = 0;
          bool AnchorEntered = false;
          while (First < LoopBody.size() && isEmptyAnchor(LoopBody[First]))
            AnchorEntered |= anyAddressEntered(LoopBody[First++], Entered);
          size_t Last = LoopBody.size();
          while (Last > First && isEmptyAnchor(LoopBody[Last - 1]))
            --Last;
          if (Last - First < 2)
            continue;
          HighStmt Exit;
          if (!AnchorEntered && ExitTest(LoopBody[First])) {
            // `while (1) { if (c) goto X; S }`: the test runs before every
            // body, so it is `while (!c) { S } goto X;`. A continue still
            // goes to the test; the loop has no break for the goto to catch.
            Exit = std::move(LoopBody[First].Body[0]);
            Loop.Cond =
                HighExpr::makeUnary(NdOp::BOOL_NOT, LoopBody[First].Cond);
            LoopBody.erase(LoopBody.begin() + First);
          } else if (ExitTest(LoopBody[Last - 1]) &&
                     !hasLooseContinue(LoopBody)) {
            // `while (1) { S; if (c) goto X; }` is `do { S } while (!c);
            // goto X;`. A continue would reach the test only in the do.
            bool TailEntered = false;
            for (size_t Q = Last; Q < LoopBody.size(); ++Q)
              TailEntered |= anyAddressEntered(LoopBody[Q], Entered);
            if (TailEntered)
              continue;
            Exit = std::move(LoopBody[Last - 1].Body[0]);
            Loop.Cond =
                HighExpr::makeUnary(NdOp::BOOL_NOT, LoopBody[Last - 1].Cond);
            Loop.Kind = StmtKind::DoWhile;
            LoopBody.erase(LoopBody.begin() + Last - 1, LoopBody.end());
          } else {
            continue;
          }
          // A jump to the child goto bypasses the condition and leaves the
          // loop. Keep that exact entry on the moved goto; only the enclosing
          // test's entered address prevents this rewrite.
          L.insert(L.begin() + K + 1, std::move(Exit));
          Changed = true;
        }
        for (HighStmt &S : L) {
          Visit(S.Body);
          Visit(S.ElseBody);
          for (auto &C : S.Cases)
            Visit(C.Body);
          Visit(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            Visit(ClauseBody);
        }
      };
  Visit(Body);
  return Changed;
}

/// The one break of a loop body that code can move to, or null when there
/// are none or several. \p Count accumulates the breaks seen.
static HighStmt *findOnlyLoopBreak(std::vector<HighStmt> &Stmts,
                                   unsigned &Count) {
  HighStmt *Found = nullptr;
  for (HighStmt &S : Stmts) {
    switch (S.Kind) {
    case StmtKind::Break:
      ++Count;
      Found = &S;
      break;
    case StmtKind::While:
    case StmtKind::DoWhile:
    case StmtKind::For:
    case StmtKind::Switch:
      break;
    case StmtKind::SEHTry:
    case StmtKind::CxxTry:
    case StmtKind::ItaniumTry: {
      // Code moved to a break inside a try would become protected.
      bool Inside = hasLooseBreak(S.Body);
      for (const auto &ClauseBody : S.EHClauseBodies)
        Inside |= hasLooseBreak(ClauseBody);
      if (Inside)
        Count += 2;
      break;
    }
    default:
      for (auto *Arm : {&S.Body, &S.ElseBody})
        if (HighStmt *B = findOnlyLoopBreak(*Arm, Count))
          Found = B;
      for (auto &ClauseBody : S.EHClauseBodies)
        if (HighStmt *B = findOnlyLoopBreak(ClauseBody, Count))
          Found = B;
      break;
    }
  }
  return Count == 1 ? Found : nullptr;
}

bool moveLoopTailsToTheirBreak(std::vector<HighStmt> &Body) {
  std::set<va_t> Targets;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      Targets.insert(S.GotoTarget);
  });
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &L) {
        for (size_t K = 0; K + 1 < L.size(); ++K) {
          // `while (1) { ..break..; X: .. } T...` where T never falls through
          // and jumps back to X: the break is T's only way in, so T can run
          // at the break, next to the label it jumps to.
          HighStmt &Loop = L[K];
          const bool Forever =
              !Loop.Cond ||
              (Loop.Cond->Kind == ExprKind::Const && Loop.Cond->ConstVal != 0);
          if (Loop.Kind != StmtKind::While || !Forever)
            continue;
          auto Last = std::find_if(
              L.rbegin(), L.rend() - (K + 1), [&](const HighStmt &T) {
                return !isEmptyAnchor(T) || Targets.count(T.Addr);
              });
          if (Last == L.rend() - (K + 1) || !endsItsBlock(*Last))
            continue;
          std::vector<HighStmt> Tail(L.begin() + K + 1, L.end());
          if (hasLooseBreakOrContinue(Tail))
            continue;
          std::set<va_t> LoopLabels;
          walkStmts(Loop.Body, [&](const HighStmt &S) {
            if (Targets.count(S.Addr))
              LoopLabels.insert(S.Addr);
          });
          bool JumpsBack = false;
          walkStmts(Tail, [&](const HighStmt &S) {
            JumpsBack |=
                S.Kind == StmtKind::Goto && LoopLabels.count(S.GotoTarget);
          });
          if (!JumpsBack)
            continue;
          unsigned Breaks = 0;
          HighStmt *Break = findOnlyLoopBreak(Loop.Body, Breaks);
          if (!Break)
            continue;
          // Replace the break with a block of the tail; it never falls through.
          HighStmt Moved;
          Moved.Kind = StmtKind::Block;
          // Entering the break originally continued at the tail. The block
          // now owns that same entry, including jumps from outside the loop.
          Moved.Addr = Break->Addr;
          Moved.Body = std::move(Tail);
          *Break = std::move(Moved);
          L.erase(L.begin() + K + 1, L.end());
          Changed = true;
          break;
        }
        for (HighStmt &S : L) {
          Visit(S.Body);
          Visit(S.ElseBody);
          for (auto &C : S.Cases)
            Visit(C.Body);
          Visit(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            Visit(ClauseBody);
        }
      };
  Visit(Body);
  return Changed;
}

/// The addresses a jump or an exception handler entry reaches.
static std::set<va_t> collectEntryTargets(const std::vector<HighStmt> &Body) {
  std::set<va_t> Targets;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      Targets.insert(S.GotoTarget);
    for (const HighEHClause &Clause : S.EHClauses)
      Targets.insert(Clause.HandlerVA);
  });
  return Targets;
}

/// The statement that ends \p Body, looking into a trailing block; a label
/// there, which a jump may reach, is what ends it.  Null for an empty body.
static const HighStmt *lastLeafStatement(const std::vector<HighStmt> &Body) {
  for (auto It = Body.rbegin(); It != Body.rend(); ++It) {
    if (isEmptyAnchor(*It)) {
      if (It->Addr != 0 && It->Addr != InvalidVA)
        return &*It;
      continue;
    }
    if (It->Kind == StmtKind::Block)
      return lastLeafStatement(It->Body);
    return &*It;
  }
  return nullptr;
}

bool moveSwitchTailsToTheirExit(std::vector<HighStmt> &Body) {
  const std::set<va_t> Targets = collectEntryTargets(Body);
  auto lastStatement = [](std::vector<HighStmt> &L) -> HighStmt * {
    for (auto It = L.rbegin(); It != L.rend(); ++It)
      if (!isEmptyAnchor(*It) || (It->Addr != 0 && It->Addr != InvalidVA))
        return &*It;
    return nullptr;
  };
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit = [&](std::vector<HighStmt>
                                                               &L) {
    for (size_t K = 0; K + 1 < L.size(); ++K) {
      HighStmt &Switch = L[K];
      // Without a default, an unmatched selector leaves the switch too.
      if (Switch.Kind != StmtKind::Switch || Switch.DefaultBody.empty())
        continue;
      std::vector<HighStmt> Tail(L.begin() + K + 1, L.end());
      HighStmt *TailLast = lastStatement(Tail);
      if (!TailLast || !endsItsBlock(*TailLast) ||
          hasLooseBreakOrContinue(Tail))
        continue;
      // Only straight-line code moves.  A tail that jumps on is
      // busiestExitFollowsTheSwitch's to place: moved into a case, its
      // jump would only join the switch's others.  Nested control flow
      // stays where later rewrites still restructure it.
      const bool StraightLine =
          std::all_of(Tail.begin(), Tail.end(), [](const HighStmt &T) {
            return isEmptyAnchor(T) ||
                   ((T.Kind == StmtKind::Assign || T.Kind == StmtKind::Store ||
                     T.Kind == StmtKind::Call || T.Kind == StmtKind::ExprStmt ||
                     T.Kind == StmtKind::Return) &&
                    T.Body.empty() && T.ElseBody.empty());
          });
      if (!StraightLine)
        continue;
      bool Entered = false;
      for (const HighStmt &T : Tail)
        Entered |=
            anyAddressEntered(T, [&](va_t A) { return Targets.count(A) != 0; });
      if (Entered)
        continue;
      // The switch's ways out: its breaks, and each body whose end falls
      // out of the switch.  T moves to the only one.
      unsigned Exits = 0;
      HighStmt *Break = nullptr;
      std::vector<HighStmt> *OpenBody = nullptr;
      auto Scan = [&](std::vector<HighStmt> &CaseBody) {
        unsigned Breaks = 0;
        if (HighStmt *B = findOnlyLoopBreak(CaseBody, Breaks))
          Break = B;
        Exits += Breaks;
        const HighStmt *Last = lastLeafStatement(CaseBody);
        if (!Last || (!endsItsBlock(*Last) && Last->Kind != StmtKind::Break)) {
          ++Exits;
          OpenBody = &CaseBody;
        }
      };
      for (SwitchCase &C : Switch.Cases)
        if (!C.FallsThrough)
          Scan(C.Body);
      Scan(Switch.DefaultBody);
      if (Exits != 1)
        continue;
      if (Break) {
        HighStmt Moved;
        Moved.Kind = StmtKind::Block;
        Moved.Addr = Break->Addr;
        Moved.Body = std::move(Tail);
        *Break = std::move(Moved);
      } else if (OpenBody) {
        OpenBody->insert(OpenBody->end(), std::make_move_iterator(Tail.begin()),
                         std::make_move_iterator(Tail.end()));
      } else {
        continue;
      }
      L.erase(L.begin() + K + 1, L.end());
      Changed = true;
      break;
    }
    for (HighStmt &S : L) {
      Visit(S.Body);
      Visit(S.ElseBody);
      for (auto &C : S.Cases)
        Visit(C.Body);
      Visit(S.DefaultBody);
      for (auto &ClauseBody : S.EHClauseBodies)
        Visit(ClauseBody);
    }
  };
  Visit(Body);
  return Changed;
}

/// The value of the pure integer expression \p E when the variable \p Key
/// holds \p Value; nullopt when \p E reads anything else.
static std::optional<llvm::APInt> evaluateWith(const HighExpr &E, VarKey Key,
                                               const llvm::APInt &Value,
                                               unsigned Depth = 0) {
  if (Depth > 32 || !E.Type || E.Type->Kind != NdTypeKind::Int ||
      E.Type->Size == 0 || E.Type->Size > 8 ||
      E.MemoryOrdering != NdMemoryOrdering::None ||
      E.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      E.IntrinsicId != Intrinsic::None || !E.IntrinsicOutputs.empty())
    return std::nullopt;
  const unsigned Bits = E.Type->Size * 8;
  auto Operand = [&](size_t I) -> std::optional<llvm::APInt> {
    if (I >= E.Operands.size() || !E.Operands[I])
      return std::nullopt;
    return evaluateWith(*E.Operands[I], Key, Value, Depth + 1);
  };
  auto Boolean = [&](bool B) { return llvm::APInt(Bits, B ? 1 : 0); };
  switch (E.Kind) {
  case ExprKind::Const:
    return llvm::APInt(64, E.ConstVal).zextOrTrunc(Bits);
  case ExprKind::Var:
    if (varKey(E.Var) != Key || Value.getBitWidth() != Bits)
      return std::nullopt;
    return Value;
  case ExprKind::Cast:
  case ExprKind::BitCast: {
    auto In = Operand(0);
    if (!In || E.Operands.size() != 1 ||
        (E.Kind == ExprKind::BitCast && In->getBitWidth() != Bits))
      return std::nullopt;
    // evaluateWith returned a value, so the operand has an integer type.
    return E.Operands[0]->Type->IsSigned ? In->sextOrTrunc(Bits)
                                         : In->zextOrTrunc(Bits);
  }
  case ExprKind::UnaryOp: {
    auto In = Operand(0);
    if (!In)
      return std::nullopt;
    switch (E.Op) {
    case NdOp::INT_ZEXT:
      return In->zextOrTrunc(Bits);
    case NdOp::INT_SEXT:
      return In->sextOrTrunc(Bits);
    case NdOp::BOOL_NOT:
      return Boolean(In->isZero());
    default:
      return std::nullopt;
    }
  }
  case ExprKind::BinOp: {
    auto A = Operand(0), B = Operand(1);
    if (!A || !B)
      return std::nullopt;
    if (E.Op == NdOp::SUBBYTES) {
      const uint64_t Shift = B->getZExtValue() * 8;
      if (Shift >= A->getBitWidth())
        return std::nullopt;
      return A->lshr(static_cast<unsigned>(Shift)).zextOrTrunc(Bits);
    }
    if (E.Op == NdOp::BOOL_AND)
      return Boolean(!A->isZero() && !B->isZero());
    if (E.Op == NdOp::BOOL_OR)
      return Boolean(!A->isZero() || !B->isZero());
    if (A->getBitWidth() != B->getBitWidth())
      return std::nullopt;
    const bool SameWidth = A->getBitWidth() == Bits;
    switch (E.Op) {
    case NdOp::INT_ADD:
      return SameWidth ? std::optional(*A + *B) : std::nullopt;
    case NdOp::INT_SUB:
      return SameWidth ? std::optional(*A - *B) : std::nullopt;
    case NdOp::INT_AND:
      return SameWidth ? std::optional(*A & *B) : std::nullopt;
    case NdOp::INT_OR:
      return SameWidth ? std::optional(*A | *B) : std::nullopt;
    case NdOp::INT_XOR:
      return SameWidth ? std::optional(*A ^ *B) : std::nullopt;
    case NdOp::INT_EQUAL:
      return Boolean(*A == *B);
    case NdOp::INT_NOTEQUAL:
      return Boolean(*A != *B);
    case NdOp::INT_LESS:
      return Boolean(A->ult(*B));
    case NdOp::INT_LESSEQUAL:
      return Boolean(A->ule(*B));
    case NdOp::INT_SLESS:
      return Boolean(A->slt(*B));
    case NdOp::INT_SLESSEQUAL:
      return Boolean(A->sle(*B));
    default:
      return std::nullopt;
    }
  }
  default:
    return std::nullopt;
  }
}

/// \p A and \p B do the same thing: copies of one statement list, as a
/// duplicated return tail is.  Empty anchors do nothing; the caller has
/// checked that no jump enters them.
static bool sameStatements(const std::vector<HighStmt> &AllA,
                           const std::vector<HighStmt> &AllB) {
  std::vector<const HighStmt *> A, B;
  for (const HighStmt &S : AllA)
    if (!isEmptyAnchor(S))
      A.push_back(&S);
  for (const HighStmt &S : AllB)
    if (!isEmptyAnchor(S))
      B.push_back(&S);
  if (A.size() != B.size())
    return false;
  auto SameExpr = [](const ExprPtr &X, const ExprPtr &Y) {
    return X == Y || (X && Y && X->structuralEq(*Y));
  };
  for (size_t I = 0; I < A.size(); ++I) {
    const HighStmt &X = *A[I], &Y = *B[I];
    if (X.Kind != Y.Kind || X.GotoTarget != Y.GotoTarget ||
        !SameExpr(X.Dst, Y.Dst) || !SameExpr(X.Val, Y.Val) ||
        !SameExpr(X.RetVal, Y.RetVal) || !SameExpr(X.CallExpr, Y.CallExpr) ||
        !SameExpr(X.StoreAddr, Y.StoreAddr) ||
        !SameExpr(X.StoreVal, Y.StoreVal) || !SameExpr(X.Cond, Y.Cond) ||
        !X.Cases.empty() || !Y.Cases.empty() || !X.EHClauses.empty() ||
        !Y.EHClauses.empty() || !sameStatements(X.Body, Y.Body) ||
        !sameStatements(X.ElseBody, Y.ElseBody) ||
        !sameStatements(X.DefaultBody, Y.DefaultBody))
      return false;
  }
  return true;
}

bool absorbSwitchRangeGuards(std::vector<HighStmt> &Body) {
  const std::set<va_t> Targets = collectEntryTargets(Body);
  auto Entered = [&](const std::vector<HighStmt> &L) {
    return std::any_of(L.begin(), L.end(), [&](const HighStmt &T) {
      return anyAddressEntered(T,
                               [&](va_t A) { return Targets.count(A) != 0; });
    });
  };
  // A case that breaks or runs off its end continues after the switch.
  auto LeavesTheSwitch = [&](const std::vector<HighStmt> &CaseBody) {
    const HighStmt *Last = lastLeafStatement(CaseBody);
    return !Last || !endsItsBlock(*Last) || hasLooseBreak(CaseBody);
  };
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &L) {
        for (size_t K = 0; K + 1 < L.size(); ++K) {
          HighStmt &Guard = L[K];
          if (Guard.Kind != StmtKind::If || !Guard.Cond ||
              !Guard.ElseBody.empty() || Guard.Body.size() != 1 ||
              Guard.Body[0].Kind != StmtKind::Switch ||
              Targets.count(Guard.Body[0].Addr))
            continue;
          HighStmt &Switch = Guard.Body[0];
          if (!Switch.SwitchExpr || Switch.SwitchExpr->Kind != ExprKind::Var ||
              !Switch.SwitchExpr->Type || Switch.DefaultBody.empty())
            continue;
          // Every case label must pass the guard: a value that fails it then
          // matches no case and takes the default.
          const unsigned Bits = Switch.SwitchExpr->Type->Size * 8;
          if (Bits == 0 || Bits > 64)
            continue;
          const VarKey Key = varKey(Switch.SwitchExpr->Var);
          bool Covered = !Switch.Cases.empty();
          for (const SwitchCase &C : Switch.Cases) {
            auto Passes = evaluateWith(
                *Guard.Cond, Key, llvm::APInt(64, C.Value).zextOrTrunc(Bits));
            Covered &= Passes && !Passes->isZero();
            if (!Covered)
              break;
          }
          if (!Covered)
            continue;
          // A failing value continues after the guard at D; the default must
          // run D too: by jumping to it, or by a copy of all of it.  A copy
          // stands for D only when D never falls out (the default would
          // then run D twice) and has no break (one in the default leaves
          // the switch instead).
          std::vector<HighStmt> &Default = Switch.DefaultBody;
          const va_t RestLabel = L[K + 1].Addr;
          const bool JumpsToRest = Default.size() == 1 &&
                                   Default[0].Kind == StmtKind::Goto &&
                                   RestLabel != 0 && RestLabel != InvalidVA &&
                                   Default[0].GotoTarget == RestLabel;
          bool KeepDefault = false;
          if (!JumpsToRest) {
            std::vector<HighStmt> Rest(L.begin() + K + 1, L.end());
            const HighStmt *RestLast = lastLeafStatement(Rest);
            if (!RestLast || !endsItsBlock(*RestLast) || hasLooseBreak(Rest) ||
                !sameStatements(Default, Rest))
              continue;
            // With no case leaving the switch, D after it is dead once the
            // guard goes; otherwise the cases still need D after the switch,
            // and the default's copy goes instead.
            KeepDefault = std::none_of(Switch.Cases.begin(), Switch.Cases.end(),
                                       [&](const SwitchCase &C) {
                                         return !C.FallsThrough &&
                                                LeavesTheSwitch(C.Body);
                                       });
            if (KeepDefault ? Entered(Rest) : Entered(Default))
              continue;
          } else if (Entered(Default)) {
            continue;
          }
          HighStmt Absorbed = std::move(Switch);
          // Falling out of the switch reaches D, as a failing value did.
          if (!KeepDefault)
            Absorbed.DefaultBody.clear();
          size_t After = K + 1;
          if (Guard.Addr != 0 && Guard.Addr != InvalidVA &&
              Absorbed.Addr != Guard.Addr) {
            // Keep the guard's label: a jump may still name it.
            HighStmt Anchor;
            Anchor.Kind = StmtKind::Block;
            Anchor.Addr = Guard.Addr;
            L[K] = std::move(Anchor);
            L.insert(L.begin() + K + 1, std::move(Absorbed));
            ++After;
          } else {
            L[K] = std::move(Absorbed);
          }
          if (KeepDefault)
            L.erase(L.begin() + After, L.end());
          Changed = true;
          break;
        }
        for (HighStmt &S : L) {
          Visit(S.Body);
          Visit(S.ElseBody);
          for (auto &C : S.Cases)
            Visit(C.Body);
          Visit(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            Visit(ClauseBody);
        }
      };
  Visit(Body);
  return Changed;
}

bool busiestExitFollowsTheSwitch(std::vector<HighStmt> &Body) {
  constexpr unsigned kMaxRounds = 64;
  auto IsTry = [](const HighStmt &S) {
    return S.Kind == StmtKind::SEHTry || S.Kind == StmtKind::CxxTry ||
           S.Kind == StmtKind::ItaniumTry;
  };
  auto Transparent = [](const HighStmt &S) {
    return S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse ||
           S.Kind == StmtKind::Block;
  };
  auto OwnsBreaks = [](const HighStmt &S) {
    return S.Kind == StmtKind::While || S.Kind == StmtKind::DoWhile ||
           S.Kind == StmtKind::For || S.Kind == StmtKind::Switch;
  };
  // The jumps breakToTheLoopFollow turns into breaks once their target
  // follows the switch: those reached through ifs and blocks only.
  std::function<void(const std::vector<HighStmt> &, std::map<va_t, unsigned> &)>
      Convertible =
          [&](const std::vector<HighStmt> &L, std::map<va_t, unsigned> &Count) {
            for (const HighStmt &S : L) {
              if (S.Kind == StmtKind::Goto) {
                ++Count[S.GotoTarget];
              } else if (Transparent(S)) {
                Convertible(S.Body, Count);
                Convertible(S.ElseBody, Count);
              }
            }
          };
  // Every jump and handler entry in \p L, at any depth.
  auto CountEntries = [](const std::vector<HighStmt> &L,
                         std::map<va_t, unsigned> &Jumps,
                         std::map<va_t, unsigned> &Handlers) {
    walkStmts(L, [&](const HighStmt &S) {
      if (S.Kind == StmtKind::Goto)
        ++Jumps[S.GotoTarget];
      for (const HighEHClause &Clause : S.EHClauses)
        ++Handlers[Clause.HandlerVA];
    });
  };
  std::set<va_t> Refused;
  bool Changed = false;
  for (unsigned Round = 0; Round < kMaxRounds; ++Round) {
    const LabelStarts Labels(Body);
    std::map<va_t, unsigned> AllJumps, AllHandlers;
    CountEntries(Body, AllJumps, AllHandlers);
    // The addresses a jump can name to reach L[N]: those of the empty
    // anchors before the first statement, of that statement, and of a
    // block's first statements.  False when L ends first.
    std::function<bool(const std::vector<HighStmt> &, size_t, std::set<va_t> &)>
        Collect =
            [&](const std::vector<HighStmt> &L, size_t N, std::set<va_t> &Out) {
              auto Name = [&](va_t A) {
                if (A != 0 && A != InvalidVA && Labels.unique(A))
                  Out.insert(A);
              };
              for (; N < L.size(); ++N) {
                const HighStmt &T = L[N];
                Name(T.Addr);
                if (isEmptyAnchor(T)) {
                  walkStmts(T.Body, [&](const HighStmt &C) { Name(C.Addr); });
                  continue;
                }
                if (T.Kind == StmtKind::Block)
                  Collect(T.Body, 0, Out);
                return true;
              }
              return false;
            };
    struct Frame {
      std::vector<HighStmt> *List;
      size_t Index;
      // Falling off the child list runs List[Index + 1], and nothing else
      // falls into it: the child is a block's body, or an if/else arm whose
      // other arm never falls out.
      bool Open;
    };
    std::vector<Frame> Frames;
    enum class Outcome { Kept, Moved, Reverted };
    auto TryMove = [&](std::vector<HighStmt> &L, size_t K) {
      HighStmt &Switch = L[K];
      if (!Switch.SwitchExpr || Switch.Cases.empty() ||
          Refused.count(Switch.Addr))
        return Outcome::Kept;
      // C: what runs once the switch falls out, up a list end only where
      // nothing else falls into what follows.
      struct Segment {
        std::vector<HighStmt> *List;
        size_t Begin;
      };
      std::vector<Segment> Chain{{&L, K + 1}};
      for (size_t F = Frames.size(); F > 0 && Frames[F - 1].Open; --F)
        Chain.push_back({Frames[F - 1].List, Frames[F - 1].Index + 1});
      auto Reach = [&](size_t I, size_t N, std::set<va_t> &Out) {
        while (I < Chain.size()) {
          if (Collect(*Chain[I].List, N, Out))
            return true;
          if (++I < Chain.size())
            N = Chain[I].Begin;
        }
        return false;
      };
      // The ways the switch falls out: a break leaving it from any depth
      // short of a nested loop or switch (under a try or in a clause it
      // stays a jump), a body running off its end, and no case matching.
      struct Way {
        std::vector<HighStmt> *List;
        size_t Index;
        bool Guarded;
      };
      std::vector<Way> Breaks;
      std::function<void(std::vector<HighStmt> &, bool)> FindBreaks =
          [&](std::vector<HighStmt> &B, bool Guarded) {
            for (size_t I = 0; I < B.size(); ++I) {
              HighStmt &T = B[I];
              if (T.Kind == StmtKind::Break) {
                Breaks.push_back({&B, I, Guarded});
                continue;
              }
              if (OwnsBreaks(T))
                continue;
              FindBreaks(T.Body, Guarded || IsTry(T));
              FindBreaks(T.ElseBody, Guarded || IsTry(T));
              for (auto &ClauseBody : T.EHClauseBodies)
                FindBreaks(ClauseBody, true);
            }
          };
      std::vector<std::vector<HighStmt> *> Arms, FallOffs;
      for (SwitchCase &C : Switch.Cases)
        if (!C.FallsThrough)
          Arms.push_back(&C.Body);
      if (!Switch.DefaultBody.empty())
        Arms.push_back(&Switch.DefaultBody);
      for (std::vector<HighStmt> *Arm : Arms) {
        FindBreaks(*Arm, false);
        if (!armNeverFallsOut(*Arm))
          FallOffs.push_back(Arm);
      }
      const bool NoDefault = Switch.DefaultBody.empty();
      const unsigned Ways = Breaks.size() + FallOffs.size() + NoDefault;
      const Way *InlineBreak = nullptr;
      for (const Way &W : Breaks)
        if (!W.Guarded) {
          InlineBreak = &W;
          break;
        }
      // One way out runs C in place; each other one jumps to it.
      const bool InlineAtWay =
          !FallOffs.empty() || NoDefault || InlineBreak != nullptr;
      const unsigned AddedJumps = InlineAtWay ? Ways - 1 : Ways;
      std::map<va_t, unsigned> Conv;
      for (SwitchCase &C : Switch.Cases)
        Convertible(C.Body, Conv);
      Convertible(Switch.DefaultBody, Conv);
      auto Sum = [&](const std::set<va_t> &Addrs) {
        unsigned N = 0;
        for (va_t A : Addrs)
          if (auto It = Conv.find(A); It != Conv.end())
            N += It->second;
        return N;
      };
      size_t StartSeg = 0;
      while (StartSeg < Chain.size() &&
             Chain[StartSeg].Begin >= Chain[StartSeg].List->size())
        ++StartSeg;
      if (StartSeg == Chain.size())
        return Outcome::Kept;
      std::set<va_t> StartAddrs;
      Reach(StartSeg, Chain[StartSeg].Begin, StartAddrs);
      const unsigned StartJumps = Sum(StartAddrs);
      const va_t Start = (*Chain[StartSeg].List)[Chain[StartSeg].Begin].Addr;
      const bool StartNamed =
          Start != 0 && Start != InvalidVA && Labels.unique(Start);
      if (AddedJumps && !StartNamed)
        return Outcome::Kept;
      // Candidates for J, best first: a statement C runs into that the
      // switch jumps to.
      struct Candidate {
        size_t Seg, Index;
        std::set<va_t> Addrs;
        int Gain;
      };
      std::vector<Candidate> Candidates;
      for (size_t I = StartSeg; I < Chain.size(); ++I) {
        std::vector<HighStmt> &List = *Chain[I].List;
        for (size_t N = Chain[I].Begin; N < List.size(); ++N) {
          if (I == StartSeg && N == Chain[I].Begin)
            continue;
          if (N > Chain[I].Begin && isEmptyAnchor(List[N - 1]))
            continue;
          std::set<va_t> Addrs;
          if (!Reach(I, N, Addrs) || Addrs.empty())
            continue;
          const int Gain = static_cast<int>(Sum(Addrs)) -
                           static_cast<int>(StartJumps) -
                           static_cast<int>(AddedJumps);
          if (Gain > 0)
            Candidates.push_back({I, N, std::move(Addrs), Gain});
        }
      }
      std::stable_sort(Candidates.begin(), Candidates.end(),
                       [](const Candidate &A, const Candidate &B) {
                         return A.Gain > B.Gain;
                       });
      auto ForEachMoved = [&](const Candidate &J, auto &&Fn) {
        for (size_t I = StartSeg; I <= J.Seg; ++I) {
          std::vector<HighStmt> &List = *Chain[I].List;
          const size_t End = I == J.Seg ? J.Index : List.size();
          for (size_t N = Chain[I].Begin; N < End; ++N)
            Fn(List[N]);
        }
      };
      const Candidate *Chosen = nullptr;
      for (const Candidate &J : Candidates) {
        std::vector<HighStmt> C;
        ForEachMoved(J, [&](const HighStmt &S) { C.push_back(S); });
        // A loose break or continue would bind to the switch once inside.
        if (hasLooseBreakOrContinue(C))
          continue;
        // Only the switch and C itself may enter C.
        std::map<va_t, unsigned> InJumps, InHandlers;
        for (SwitchCase &Case : Switch.Cases)
          CountEntries(Case.Body, InJumps, InHandlers);
        CountEntries(Switch.DefaultBody, InJumps, InHandlers);
        CountEntries(C, InJumps, InHandlers);
        bool Outside = false;
        walkStmts(C, [&](const HighStmt &S) {
          if (S.Addr == 0 || S.Addr == InvalidVA ||
              !Labels.entered().count(S.Addr))
            return;
          Outside |= AllJumps[S.Addr] != InJumps[S.Addr] ||
                     AllHandlers[S.Addr] != InHandlers[S.Addr];
        });
        if (!Outside) {
          Chosen = &J;
          break;
        }
      }
      if (!Chosen)
        return Outcome::Kept;
      std::vector<HighStmt> Saved = Body;
      const va_t SwitchAddr = Switch.Addr;
      std::vector<HighStmt> C;
      for (size_t I = StartSeg; I <= Chosen->Seg; ++I) {
        std::vector<HighStmt> &List = *Chain[I].List;
        const size_t End = I == Chosen->Seg ? Chosen->Index : List.size();
        const auto First =
            List.begin() + static_cast<ptrdiff_t>(Chain[I].Begin);
        const auto Last = List.begin() + static_cast<ptrdiff_t>(End);
        C.insert(C.end(), std::make_move_iterator(First),
                 std::make_move_iterator(Last));
        List.erase(First, Last);
      }
      auto JumpToC = [&](va_t Addr) {
        HighStmt Jump;
        Jump.Kind = StmtKind::Goto;
        Jump.Addr = Addr;
        Jump.GotoTarget = Start;
        return Jump;
      };
      // A break keeps its address: a jump to it still reaches C.
      for (const Way &W : Breaks) {
        HighStmt &Break = (*W.List)[W.Index];
        if (&W == InlineBreak && FallOffs.empty() && !NoDefault) {
          HighStmt Moved;
          Moved.Kind = StmtKind::Block;
          Moved.Addr = Break.Addr;
          Moved.Body = std::move(C);
          HighStmt Leave;
          Leave.Kind = StmtKind::Break;
          Moved.Body.push_back(std::move(Leave));
          Break = std::move(Moved);
        } else {
          Break = JumpToC(Break.Addr);
        }
      }
      for (std::vector<HighStmt> *Arm : FallOffs)
        if (Arm != FallOffs.back())
          Arm->push_back(JumpToC(0));
      if (!FallOffs.empty()) {
        FallOffs.back()->insert(FallOffs.back()->end(),
                                std::make_move_iterator(C.begin()),
                                std::make_move_iterator(C.end()));
        if (NoDefault)
          Switch.DefaultBody.push_back(JumpToC(0));
      } else if (NoDefault) {
        Switch.DefaultBody = std::move(C);
      } else if (!InlineBreak) {
        // Every way out sits under a try, or there is none: C follows the
        // last body, which never falls out, and only jumps reach it.
        Arms.back()->insert(Arms.back()->end(),
                            std::make_move_iterator(C.begin()),
                            std::make_move_iterator(C.end()));
      }
      // Moving statements next to others can change which statement a
      // label lands on; keep the move only if no entered label changed.
      const LabelStarts After(Body);
      bool Same = !AddedJumps || After.unique(Start);
      for (va_t A : Labels.entered())
        Same &= A == 0 || A == InvalidVA ||
                (After.unique(A) == Labels.unique(A) &&
                 After.starts(A) == Labels.starts(A));
      if (!Same) {
        Body = std::move(Saved);
        Refused.insert(SwitchAddr);
        return Outcome::Reverted;
      }
      return Outcome::Moved;
    };
    Outcome Result = Outcome::Kept;
    std::function<void(std::vector<HighStmt> &)> Visit =
        [&](std::vector<HighStmt> &L) {
          for (size_t K = 0; K < L.size() && Result == Outcome::Kept; ++K) {
            if (L[K].Kind == StmtKind::Switch) {
              Result = TryMove(L, K);
              if (Result != Outcome::Kept)
                return;
            }
            // A change may free every statement; nothing below may touch
            // one once a child reports it.
            HighStmt &S = L[K];
            auto Into = [&](std::vector<HighStmt> &Child, bool Open) {
              Frames.push_back({&L, K, Open});
              Visit(Child);
              Frames.pop_back();
              return Result == Outcome::Kept;
            };
            const bool Both = S.Kind == StmtKind::IfElse;
            const bool BodyOpen = S.Kind == StmtKind::Block ||
                                  (Both && armNeverFallsOut(S.ElseBody));
            const bool ElseOpen = Both && armNeverFallsOut(S.Body);
            if (!Into(S.Body, BodyOpen) || !Into(S.ElseBody, ElseOpen))
              return;
            for (auto &Case : S.Cases)
              if (!Into(Case.Body, false))
                return;
            if (!Into(S.DefaultBody, false))
              return;
            for (auto &ClauseBody : S.EHClauseBodies)
              if (!Into(ClauseBody, false))
                return;
          }
        };
    Visit(Body);
    if (Result == Outcome::Kept)
      break;
    Changed |= Result == Outcome::Moved;
  }
  return Changed;
}

bool breakToTheLoopFollow(std::vector<HighStmt> &Body) {
  const LabelStarts Labels(Body);
  const std::set<va_t> None;
  // The addresses a jump can name to reach what runs after L[N - 1]: the
  // next statement, through removed statements, empty anchors and the start
  // of a block, or \p After past the end of the list.
  std::function<bool(const std::vector<HighStmt> &, size_t, std::set<va_t> &)>
      Collect = [&](const std::vector<HighStmt> &L, size_t N,
                    std::set<va_t> &Follow) {
        for (; N < L.size(); ++N) {
          const HighStmt &T = L[N];
          if (T.Addr != 0 && T.Addr != InvalidVA && Labels.unique(T.Addr))
            Follow.insert(T.Addr);
          if (isEmptyAnchor(T)) {
            walkStmts(T.Body, [&](const HighStmt &C) {
              if (C.Addr != 0 && C.Addr != InvalidVA && Labels.unique(C.Addr))
                Follow.insert(C.Addr);
            });
            continue;
          }
          if (T.Kind == StmtKind::Block)
            Collect(T.Body, 0, Follow);
          return true;
        }
        return false;
      };
  auto FollowAt = [&](const std::vector<HighStmt> &L, size_t N,
                      const std::set<va_t> &After) {
    std::set<va_t> Follow;
    if (!Collect(L, N, Follow))
      Follow.insert(After.begin(), After.end());
    return Follow;
  };
  bool Changed = false;
  // A break at loop level leaves the loop for its follow. A nested loop or
  // switch owns its breaks, and a jump out of a try stays a jump.
  std::function<void(std::vector<HighStmt> &, const std::set<va_t> &)> ToBreak =
      [&](std::vector<HighStmt> &L, const std::set<va_t> &Follow) {
        for (HighStmt &S : L) {
          if (S.Kind == StmtKind::Goto && Follow.count(S.GotoTarget)) {
            S.Kind = StmtKind::Break;
            S.GotoTarget = 0;
            Changed = true;
          } else if (S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse ||
                     S.Kind == StmtKind::Block) {
            ToBreak(S.Body, Follow);
            ToBreak(S.ElseBody, Follow);
          }
        }
      };
  std::function<void(const std::vector<HighStmt> &, std::set<va_t> &)> Jumps =
      [&](const std::vector<HighStmt> &L, std::set<va_t> &Targets) {
        for (const HighStmt &S : L) {
          if (S.Kind == StmtKind::Goto)
            Targets.insert(S.GotoTarget);
          else if (S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse ||
                   S.Kind == StmtKind::Block) {
            Jumps(S.Body, Targets);
            Jumps(S.ElseBody, Targets);
          }
        }
      };
  auto Entered = [&](va_t Addr) { return Labels.entered().count(Addr) != 0; };
  // `if (c) { A } S...` ending a list, where A never falls out and jumps to
  // what follows the list, \p After: S runs only when c fails, so it becomes
  // the else.  A's jumps to After then leave the if/else for what follows it,
  // and a switch case among them breaks.  Nothing may enter S.
  auto MoveRestToElse = [&](std::vector<HighStmt> &L, size_t K,
                            const std::set<va_t> &After) {
    HighStmt &If = L[K];
    if (If.Kind != StmtKind::If || !If.Cond || !If.ElseBody.empty() ||
        After.empty() || K + 1 >= L.size() ||
        std::all_of(L.begin() + K + 1, L.end(), isEmptyAnchor) ||
        !armNeverFallsOut(If.Body))
      return;
    bool LeavesForAfter = false;
    walkStmts(If.Body, [&](const HighStmt &S) {
      LeavesForAfter |= S.Kind == StmtKind::Goto && After.count(S.GotoTarget);
    });
    if (!LeavesForAfter)
      return;
    for (size_t M = K + 1; M < L.size(); ++M)
      if (anyAddressEntered(L[M], Entered))
        return;
    If.Kind = StmtKind::IfElse;
    If.ElseBody.assign(std::make_move_iterator(L.begin() + K + 1),
                       std::make_move_iterator(L.end()));
    L.erase(L.begin() + K + 1, L.end());
    // A trailing jump of the then arm now names what follows the if/else.
    std::function<void(std::vector<HighStmt> &)> TrimTail =
        [&](std::vector<HighStmt> &Arm) {
          if (Arm.empty())
            return;
          HighStmt &Last = Arm.back();
          if (Last.Kind == StmtKind::Goto && After.count(Last.GotoTarget) &&
              !Entered(Last.Addr))
            Arm.pop_back();
          else if (Last.Kind == StmtKind::IfElse) {
            TrimTail(Last.Body);
            TrimTail(Last.ElseBody);
          } else if (Last.Kind == StmtKind::Block) {
            TrimTail(Last.Body);
          }
        };
    TrimTail(If.Body);
    Changed = true;
  };
  // `while (1) { ..break..; goto X; } P...; X:` where nothing enters P: P
  // runs only after the break, so it can run at the break instead, leaving X
  // as what follows the loop. X may also be what follows the list, \p After,
  // when P runs to its end.
  auto MoveFollowToBreak = [&](std::vector<HighStmt> &L, size_t K,
                               const std::set<va_t> &After) {
    HighStmt &Loop = L[K];
    const bool Forever = !Loop.Cond || (Loop.Cond->Kind == ExprKind::Const &&
                                        Loop.Cond->ConstVal != 0);
    if (Loop.Kind != StmtKind::While || !Forever)
      return;
    std::set<va_t> Targets;
    Jumps(Loop.Body, Targets);
    size_t M = K + 1;
    for (; M < L.size(); ++M) {
      const va_t X = L[M].Addr;
      if (M > K + 1 && Targets.count(X) && Labels.unique(X))
        break;
      if (anyAddressEntered(L[M], Entered))
        return;
    }
    if (M == L.size() && std::none_of(After.begin(), After.end(), [&](va_t X) {
          return Targets.count(X) != 0;
        }))
      return;
    // Empty anchors alone already let a jump past them count as the follow.
    if (std::all_of(L.begin() + K + 1, L.begin() + M, isEmptyAnchor))
      return;
    std::vector<HighStmt> Follow(std::make_move_iterator(L.begin() + K + 1),
                                 std::make_move_iterator(L.begin() + M));
    unsigned Breaks = 0;
    HighStmt *Break = hasLooseBreakOrContinue(Follow)
                          ? nullptr
                          : findOnlyLoopBreak(Loop.Body, Breaks);
    if (!Break) {
      std::move(Follow.begin(), Follow.end(), L.begin() + K + 1);
      return;
    }
    HighStmt Exit;
    Exit.Kind = StmtKind::Break;
    Follow.push_back(std::move(Exit));
    HighStmt Moved;
    Moved.Kind = StmtKind::Block;
    // A jump to the break still runs the moved code first.
    Moved.Addr = Break->Addr;
    Moved.Body = std::move(Follow);
    *Break = std::move(Moved);
    L.erase(L.begin() + K + 1, L.begin() + M);
    Changed = true;
  };
  // The one place a switch falls out of: a case body that ends without a
  // jump or with `break`, or the default (an absent one falls out too).
  // Nothing when two places fall out or one breaks before its end.
  std::function<bool(const std::vector<HighStmt> &, bool)> BreaksInside =
      [&](const std::vector<HighStmt> &B, bool SkipLast) {
        for (size_t I = 0; I < B.size(); ++I) {
          const HighStmt &S = B[I];
          if (S.Kind == StmtKind::Break && !(SkipLast && I + 1 == B.size()))
            return true;
          if ((S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse ||
               S.Kind == StmtKind::Block) &&
              (BreaksInside(S.Body, false) || BreaksInside(S.ElseBody, false)))
            return true;
        }
        return false;
      };
  auto OnlyFallOut = [&](HighStmt &Switch) -> std::vector<HighStmt> * {
    std::vector<HighStmt> *Out = nullptr;
    unsigned Count = 0;
    auto Consider = [&](std::vector<HighStmt> &B) {
      if (BreaksInside(B, true)) {
        Count = 2;
        return;
      }
      auto Last = std::find_if(B.rbegin(), B.rend(), [](const HighStmt &T) {
        return !isEmptyAnchor(T) || (T.Addr != 0 && T.Addr != InvalidVA);
      });
      if (Last == B.rend() || Last->Kind == StmtKind::Break ||
          (!endsItsBlock(*Last) && Last->Kind != StmtKind::Continue)) {
        Out = &B;
        ++Count;
      }
    };
    for (SwitchCase &C : Switch.Cases)
      if (!C.FallsThrough)
        Consider(C.Body);
    Consider(Switch.DefaultBody);
    return Count == 1 ? Out : nullptr;
  };
  // `switch (x) { case A: ..goto X; default: ..break; } P...; X:` where
  // nothing enters P: P runs only after the switch falls out of its one
  // falling place, so it can run there instead, leaving X as what follows
  // the switch. X may also be what follows the list, \p After, when P runs
  // to its end.
  auto MoveFollowToFallOut = [&](std::vector<HighStmt> &L, size_t K,
                                 const std::set<va_t> &After) {
    HighStmt &Switch = L[K];
    if (Switch.Kind != StmtKind::Switch)
      return;
    std::set<va_t> Targets;
    for (const auto &C : Switch.Cases)
      Jumps(C.Body, Targets);
    Jumps(Switch.DefaultBody, Targets);
    size_t M = K + 1;
    for (; M < L.size(); ++M) {
      const va_t X = L[M].Addr;
      if (M > K + 1 && Targets.count(X) && Labels.unique(X))
        break;
      if (anyAddressEntered(L[M], Entered))
        return;
    }
    if (M == L.size() && std::none_of(After.begin(), After.end(), [&](va_t X) {
          return Targets.count(X) != 0;
        }))
      return;
    if (std::all_of(L.begin() + K + 1, L.begin() + M, isEmptyAnchor))
      return;
    std::vector<HighStmt> *Out = OnlyFallOut(Switch);
    if (!Out)
      return;
    std::vector<HighStmt> Follow(std::make_move_iterator(L.begin() + K + 1),
                                 std::make_move_iterator(L.begin() + M));
    // A loose break or continue would bind to the switch once inside it.
    if (hasLooseBreakOrContinue(Follow)) {
      std::move(Follow.begin(), Follow.end(), L.begin() + K + 1);
      return;
    }
    if (!Out->empty() && Out->back().Kind == StmtKind::Break)
      Out->pop_back();
    Out->insert(Out->end(), std::make_move_iterator(Follow.begin()),
                std::make_move_iterator(Follow.end()));
    L.erase(L.begin() + K + 1, L.begin() + M);
    Changed = true;
  };
  std::function<void(std::vector<HighStmt> &, const std::set<va_t> &)> Visit =
      [&](std::vector<HighStmt> &L, const std::set<va_t> &After) {
        for (size_t K = 0; K < L.size(); ++K) {
          MoveRestToElse(L, K, After);
          MoveFollowToBreak(L, K, After);
          MoveFollowToFallOut(L, K, After);
          HighStmt &S = L[K];
          const bool Loop = S.Kind == StmtKind::While ||
                            S.Kind == StmtKind::DoWhile ||
                            S.Kind == StmtKind::For;
          // Falling off an if/else arm or a block continues after it.
          const bool Arms = S.Kind == StmtKind::If ||
                            S.Kind == StmtKind::IfElse ||
                            S.Kind == StmtKind::Block;
          const bool Switch = S.Kind == StmtKind::Switch;
          std::set<va_t> Follow;
          if (Loop || Arms || Switch)
            Follow = FollowAt(L, K + 1, After);
          if (Loop && !Follow.empty())
            ToBreak(S.Body, Follow);
          // A case leaves its switch for the same follow with `break`.
          if (Switch && !Follow.empty()) {
            for (auto &C : S.Cases)
              ToBreak(C.Body, Follow);
            ToBreak(S.DefaultBody, Follow);
          }
          const std::set<va_t> &Inner = Arms ? Follow : None;
          Visit(S.Body, Inner);
          Visit(S.ElseBody, Inner);
          for (auto &C : S.Cases)
            Visit(C.Body, None);
          Visit(S.DefaultBody, None);
          for (auto &ClauseBody : S.EHClauseBodies)
            Visit(ClauseBody, None);
        }
      };
  Visit(Body, None);
  return Changed;
}

bool unwrapLoopsThatNeverRepeat(std::vector<HighStmt> &Body) {
  std::set<va_t> Targets;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      Targets.insert(S.GotoTarget);
  });
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &L) {
        for (HighStmt &S : L) {
          Visit(S.Body);
          Visit(S.ElseBody);
          for (auto &C : S.Cases)
            Visit(C.Body);
          Visit(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            Visit(ClauseBody);
        }
        for (size_t K = 0; K < L.size(); ++K) {
          // `while (1) { S }` whose S never reaches its end, with no break
          // or continue of its own, runs S once: jumps inside S go to the
          // same statements with or without the loop around them.
          HighStmt &Loop = L[K];
          const bool Forever =
              !Loop.Cond ||
              (Loop.Cond->Kind == ExprKind::Const && Loop.Cond->ConstVal != 0);
          if ((Loop.Kind != StmtKind::While &&
               Loop.Kind != StmtKind::DoWhile) ||
              (Loop.Kind == StmtKind::While && !Forever) || Loop.Body.empty())
            continue;
          // A label after the last jump still reaches the end of the body.
          auto Last = std::find_if(
              Loop.Body.rbegin(), Loop.Body.rend(), [&](const HighStmt &T) {
                return !isEmptyAnchor(T) || Targets.count(T.Addr);
              });
          if (Last == Loop.Body.rend() || !endsItsBlock(*Last) ||
              hasLooseBreak(Loop.Body) || hasLooseContinue(Loop.Body))
            continue;
          std::vector<HighStmt> Inner = std::move(Loop.Body);
          if (Loop.Addr != 0 && Loop.Addr != InvalidVA) {
            HighStmt Anchor;
            Anchor.Kind = StmtKind::Block;
            Anchor.Addr = Loop.Addr;
            Inner.insert(Inner.begin(), std::move(Anchor));
          }
          L.erase(L.begin() + K);
          L.insert(L.begin() + K, std::make_move_iterator(Inner.begin()),
                   std::make_move_iterator(Inner.end()));
          Changed = true;
        }
      };
  Visit(Body);
  return Changed;
}

bool hoistSharedArmTails(std::vector<HighStmt> &Body) {
  bool Changed = false;
  // Addresses a jump enters: a removed jump that carries one leaves an empty
  // anchor so its label still exists.
  std::set<va_t> Entered;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto)
      Entered.insert(S.GotoTarget);
    for (const HighEHClause &Clause : S.EHClauses)
      Entered.insert(Clause.HandlerVA);
  });
  // The index of the statement that starts label \p X at the top of \p Arm,
  // past its first statement; 0 when there is none.
  auto SuffixAt = [](const std::vector<HighStmt> &Arm, va_t X) -> size_t {
    if (X == 0 || X == InvalidVA)
      return 0;
    for (size_t K = 1; K < Arm.size(); ++K)
      if (Arm[K].Addr == X && Arm[K - 1].Addr != X)
        return K;
    return 0;
  };
  std::function<void(std::vector<HighStmt> &)> Visit = [&](std::vector<HighStmt>
                                                               &L) {
    for (size_t I = 0; I < L.size(); ++I) {
      HighStmt &S = L[I];
      Visit(S.Body);
      Visit(S.ElseBody);
      for (SwitchCase &C : S.Cases)
        Visit(C.Body);
      Visit(S.DefaultBody);
      for (auto &ClauseBody : S.EHClauseBodies)
        Visit(ClauseBody);
      if (S.Kind != StmtKind::IfElse || S.Body.empty() || S.ElseBody.empty())
        continue;
      // `if (c) { A; X: B } else { C; goto X; }`: both arms finish with
      // B, which then runs after the if/else instead.
      for (bool ThenOwns : {true, false}) {
        std::vector<HighStmt> &Owner = ThenOwns ? S.Body : S.ElseBody;
        std::vector<HighStmt> &Other = ThenOwns ? S.ElseBody : S.Body;
        if (Other.empty() || Other.back().Kind != StmtKind::Goto)
          continue;
        const size_t K = SuffixAt(Owner, Other.back().GotoTarget);
        if (K == 0)
          continue;
        std::vector<HighStmt> Suffix(std::make_move_iterator(Owner.begin() + K),
                                     std::make_move_iterator(Owner.end()));
        Owner.erase(Owner.begin() + K, Owner.end());
        HighStmt &Jump = Other.back();
        if (Jump.Addr != 0 && Jump.Addr != InvalidVA &&
            Entered.count(Jump.Addr) &&
            (Other.size() == 1 || Other[Other.size() - 2].Addr != Jump.Addr)) {
          HighStmt Anchor;
          Anchor.Kind = StmtKind::Block;
          Anchor.Addr = Jump.Addr;
          Jump = std::move(Anchor);
        } else {
          Other.pop_back();
        }
        L.insert(L.begin() + I + 1, std::make_move_iterator(Suffix.begin()),
                 std::make_move_iterator(Suffix.end()));
        Changed = true;
        break;
      }
      HighStmt &T = L[I];
      if (T.Kind == StmtKind::IfElse && T.ElseBody.empty())
        T.Kind = StmtKind::If;
    }
  };
  Visit(Body);
  return Changed;
}

bool loopifyTrailingArmBodies(std::vector<HighStmt> &Body) {
  std::map<va_t, unsigned> Uses;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      ++Uses[S.GotoTarget];
    for (const HighEHClause &Clause : S.EHClauses)
      if (Clause.HandlerVA != 0 && Clause.HandlerVA != InvalidVA)
        Uses[Clause.HandlerVA] = ~0u;
  });
  auto isJump = [](const HighStmt &S) {
    return S.Kind == StmtKind::Return || S.Kind == StmtKind::Goto ||
           S.Kind == StmtKind::Break || S.Kind == StmtKind::Continue;
  };
  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &P) {
        for (size_t I = 0; I + 1 < P.size(); ++I) {
          // `if (a) {..} else { ..; jump; X: S.. }  if (c) goto X;`
          //   ->  `if (a) {..} else { ..; jump; }  while (c) { S.. }`.
          HighStmt &Arm = P[I];
          HighStmt &Test = P[I + 1];
          if ((Arm.Kind != StmtKind::If && Arm.Kind != StmtKind::IfElse) ||
              Test.Kind != StmtKind::If || !Test.Cond ||
              !Test.ElseBody.empty() || Test.Body.size() != 1 ||
              Test.Body[0].Kind != StmtKind::Goto ||
              (Test.Addr != 0 && Test.Addr != InvalidVA &&
               Uses.count(Test.Addr)))
            continue;
          const va_t X = Test.Body[0].GotoTarget;
          if (Uses[X] != 1)
            continue;
          for (std::vector<HighStmt> *LL : {&Arm.Body, &Arm.ElseBody}) {
            size_t K = 1;
            while (K < LL->size() &&
                   !((*LL)[K].Addr == X && (*LL)[K - 1].Addr != X))
              ++K;
            if (K >= LL->size() || !isJump((*LL)[K - 1]))
              continue;
            std::vector<HighStmt> Loop(std::make_move_iterator(LL->begin() + K),
                                       std::make_move_iterator(LL->end()));
            bool Bad = hasLooseBreakOrContinue(Loop);
            walkStmts(Loop, [&](const HighStmt &S) {
              Bad |= S.Kind == StmtKind::SEHTry || S.Kind == StmtKind::CxxTry ||
                     S.Kind == StmtKind::ItaniumTry;
            });
            if (Bad) {
              std::move(Loop.begin(), Loop.end(), LL->begin() + K);
              continue;
            }
            LL->erase(LL->begin() + K, LL->end());
            Test.Kind = StmtKind::While;
            Test.Body = std::move(Loop);
            Uses.erase(X);
            Changed = true;
            break;
          }
        }
        for (HighStmt &S : P) {
          Visit(S.Body);
          Visit(S.ElseBody);
          for (auto &C : S.Cases)
            Visit(C.Body);
          Visit(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            Visit(ClauseBody);
        }
      };
  Visit(Body);
  return Changed;
}

bool groupSwitchCases(std::vector<HighStmt> &Body) {
  std::map<va_t, unsigned> Uses;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      ++Uses[S.GotoTarget];
    for (const HighEHClause &Clause : S.EHClauses)
      if (Clause.HandlerVA != 0 && Clause.HandlerVA != InvalidVA)
        Uses[Clause.HandlerVA] = ~0u;
  });
  auto labeled = [&](const HighStmt &S) {
    return S.Addr != 0 && S.Addr != InvalidVA && Uses.count(S.Addr) != 0;
  };
  // `goto Next` that leaves a case for the statement right after the switch
  // is a `break`.  Loops and nested switches own their own `break`.
  std::function<bool(std::vector<HighStmt> &, va_t)> GotoNextToBreak =
      [&](std::vector<HighStmt> &L, va_t Next) {
        bool Changed = false;
        for (HighStmt &S : L) {
          if (S.Kind == StmtKind::Goto && S.GotoTarget == Next) {
            if (--Uses[Next] == 0)
              Uses.erase(Next);
            S.Kind = StmtKind::Break;
            S.GotoTarget = 0;
            Changed = true;
          } else if (S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse ||
                     S.Kind == StmtKind::Block) {
            Changed |= GotoNextToBreak(S.Body, Next);
            Changed |= GotoNextToBreak(S.ElseBody, Next);
          }
        }
        return Changed;
      };
  // Where a case body goes: a lone goto's target, or 0 for leaving the
  // switch.  Bodies with any other content, or whose statement is itself a
  // label, have no exit key.
  constexpr va_t kLeave = 0;
  auto exitOf = [&](const std::vector<HighStmt> &L) -> std::optional<va_t> {
    if (L.empty())
      return kLeave;
    if (L.size() != 1 || labeled(L[0]))
      return std::nullopt;
    if (L[0].Kind == StmtKind::Break)
      return kLeave;
    if (L[0].Kind == StmtKind::Goto && L[0].GotoTarget != 0 &&
        L[0].GotoTarget != InvalidVA)
      return L[0].GotoTarget;
    return std::nullopt;
  };

  bool Changed = false;
  std::function<void(std::vector<HighStmt> &)> Visit =
      [&](std::vector<HighStmt> &L) {
        for (size_t I = 0; I < L.size(); ++I) {
          HighStmt &S = L[I];
          Visit(S.Body);
          Visit(S.ElseBody);
          for (SwitchCase &C : S.Cases)
            Visit(C.Body);
          Visit(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            Visit(ClauseBody);
          if (S.Kind != StmtKind::Switch || S.Cases.empty())
            continue;
          if (I + 1 < L.size() && L[I + 1].Addr != 0 &&
              L[I + 1].Addr != InvalidVA && L[I + 1].Addr != S.Addr) {
            const va_t Next = L[I + 1].Addr;
            for (SwitchCase &C : S.Cases)
              Changed |= GotoNextToBreak(C.Body, Next);
            Changed |= GotoNextToBreak(S.DefaultBody, Next);
          }
          // Units: a run of fall-through cases and the case owning the body.
          std::vector<std::vector<SwitchCase>> Units;
          std::vector<SwitchCase> Cur;
          for (SwitchCase &C : S.Cases) {
            Cur.push_back(std::move(C));
            if (!Cur.back().FallsThrough) {
              Units.push_back(std::move(Cur));
              Cur.clear();
            }
          }
          if (!Cur.empty())
            Units.push_back(std::move(Cur));
          // Two case bodies are interchangeable when they leave for the
          // same place or are the same short label-free statement list.
          auto sameBody = [&](const std::vector<HighStmt> &A,
                              const std::vector<HighStmt> &B) {
            const std::optional<va_t> EA = exitOf(A), EB = exitOf(B);
            if (EA || EB)
              return EA == EB;
            if (A.size() > 8 || !sameStraightLineBody(A, B))
              return false;
            for (const HighStmt &X : A)
              if (labeled(X))
                return false;
            for (const HighStmt &X : B)
              if (labeled(X))
                return false;
            return true;
          };
          auto hasBody = [&](size_t U) {
            return !Units[U].back().FallsThrough;
          };
          auto dropBody = [&](std::vector<HighStmt> &L) {
            for (const HighStmt &X : L)
              if (X.Kind == StmtKind::Goto && X.GotoTarget != 0 &&
                  X.GotoTarget != InvalidVA && --Uses[X.GotoTarget] == 0)
                Uses.erase(X.GotoTarget);
            L.clear();
          };
          // A case that does what an explicit `default` does adds nothing.
          // Without one, a case that just leaves keeps its recovered label.
          std::vector<bool> Dropped(Units.size(), false);
          for (size_t U = 0; U < Units.size(); ++U)
            if (!S.DefaultBody.empty() && hasBody(U) &&
                sameBody(Units[U].back().Body, S.DefaultBody)) {
              Dropped[U] = true;
              dropBody(Units[U].back().Body);
              Changed = true;
            }
          // Cases with the same destination share one body.
          std::vector<SwitchCase> NewCases;
          std::vector<bool> Emitted(Units.size(), false);
          for (size_t U = 0; U < Units.size(); ++U) {
            if (Dropped[U] || Emitted[U])
              continue;
            std::vector<size_t> Group{U};
            if (hasBody(U))
              for (size_t V = U + 1; V < Units.size(); ++V)
                if (!Dropped[V] && !Emitted[V] && hasBody(V) &&
                    sameBody(Units[V].back().Body, Units[U].back().Body))
                  Group.push_back(V);
            for (size_t G = 0; G < Group.size(); ++G) {
              Emitted[Group[G]] = true;
              auto &Unit = Units[Group[G]];
              if (G + 1 < Group.size()) {
                dropBody(Unit.back().Body);
                Unit.back().FallsThrough = true;
                Changed = true;
              }
              for (SwitchCase &C : Unit)
                NewCases.push_back(std::move(C));
            }
          }
          S.Cases = std::move(NewCases);
        }
      };
  Visit(Body);
  return Changed;
}

bool reduceSingleUseGotos(std::vector<HighStmt> &Body, bool SpliceRegions) {
  // Every way a statement address is entered: gotos and __except handlers.
  std::map<va_t, unsigned> Uses;
  std::set<va_t> Pinned;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      ++Uses[S.GotoTarget];
    for (const HighEHClause &Clause : S.EHClauses)
      if (Clause.HandlerVA != 0 && Clause.HandlerVA != InvalidVA)
        Pinned.insert(Clause.HandlerVA);
  });
  auto usesOf = [&](va_t Addr) -> unsigned {
    if (Addr == 0 || Addr == InvalidVA)
      return 0;
    if (Pinned.count(Addr))
      return ~0u;
    auto It = Uses.find(Addr);
    return It == Uses.end() ? 0 : It->second;
  };
  auto isTerminator = [](const HighStmt &S) { return endsItsBlock(S); };
  auto isCondGoto = [](const HighStmt &S) {
    return S.Kind == StmtKind::If && S.Cond && S.ElseBody.empty() &&
           S.Body.size() == 1 && S.Body[0].Kind == StmtKind::Goto &&
           S.Body[0].GotoTarget != 0 && S.Body[0].GotoTarget != InvalidVA;
  };
  // A statement starts an entered label when its address is referenced and
  // the previous statement belongs to another address.
  auto labelStart = [&](const std::vector<HighStmt> &L, size_t I) {
    return L[I].Addr != 0 && L[I].Addr != InvalidVA &&
           (I == 0 || L[I - 1].Addr != L[I].Addr) && usesOf(L[I].Addr) != 0;
  };
  // Statements starting each address anywhere in the tree.  A copied tail
  // keeps its addresses, so a jump names one statement only when exactly one
  // starts its target.
  std::map<va_t, unsigned> AddressStarts;
  std::function<void(const std::vector<HighStmt> &)> CountStarts =
      [&](const std::vector<HighStmt> &L) {
        for (size_t I = 0; I < L.size(); ++I) {
          const HighStmt &S = L[I];
          if (S.Addr != 0 && S.Addr != InvalidVA &&
              (I == 0 || L[I - 1].Addr != S.Addr))
            ++AddressStarts[S.Addr];
          CountStarts(S.Body);
          CountStarts(S.ElseBody);
          for (const auto &C : S.Cases)
            CountStarts(C.Body);
          CountStarts(S.DefaultBody);
          for (const auto &ClauseBody : S.EHClauseBodies)
            CountStarts(ClauseBody);
        }
      };
  CountStarts(Body);

  // Reads of each variable anywhere in the function (assignment targets are
  // writes, not reads). An indirect call reads its target like an operand.
  std::unordered_map<VarKey, unsigned, VarKeyHash> Reads;
  std::function<void(const ExprPtr &)> CountReads = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (E->Kind == ExprKind::Var)
      ++Reads[varKey(E->Var)];
    E->forEachChildExpr(CountReads);
  };
  walkStmts(Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      if (&E == &S.Dst && S.Kind == StmtKind::Assign && E &&
          E->Kind == ExprKind::Var)
        return;
      CountReads(E);
    });
  });
  // Replace the single read of \p Key in \p E with \p Value.
  std::function<bool(ExprPtr &, const VarKey &, const ExprPtr &)> Substitute =
      [&](ExprPtr &E, const VarKey &Key, const ExprPtr &Value) {
        if (!E)
          return false;
        if (E->Kind == ExprKind::Var && varKey(E->Var) == Key) {
          E = Value;
          return true;
        }
        for (ExprPtr &Op : E->Operands)
          if (Substitute(Op, Key, Value))
            return true;
        return Substitute(E->IndirectTarget, Key, Value);
      };
  // A load or pure value with no call inside it.
  std::function<bool(const HighExpr &, unsigned)> Foldable =
      [&](const HighExpr &E, unsigned Depth) -> bool {
    // An atomic is still evaluated exactly once, at the same point.
    if (Depth > 32 || E.Kind == ExprKind::Call || E.Kind == ExprKind::Store ||
        E.Kind == ExprKind::Phi || E.Kind == ExprKind::Undef)
      return false;
    for (const ExprPtr &Op : E.Operands)
      if (!Op || !Foldable(*Op, Depth + 1))
        return false;
    return true;
  };

  // Remove the trailing goto of \p L.  A goto that itself carries an entered
  // label leaves an empty block behind so that label still exists.
  auto popGoto = [&](std::vector<HighStmt> &L) {
    HighStmt &G = L.back();
    if (G.Addr != 0 && G.Addr != InvalidVA && usesOf(G.Addr) != 0 &&
        (L.size() == 1 || L[L.size() - 2].Addr != G.Addr)) {
      HighStmt Anchor;
      Anchor.Kind = StmtKind::Block;
      Anchor.Addr = G.Addr;
      G = std::move(Anchor);
      return;
    }
    L.pop_back();
  };

  // Placeholders left by an earlier block splice would hide the statement
  // a rewrite inspects (the label opening an else, the next statement).
  std::function<void(std::vector<HighStmt> &)> DropPlaceholders =
      [&](std::vector<HighStmt> &L) {
        L.erase(std::remove_if(L.begin(), L.end(),
                               [&](const HighStmt &S) {
                                 return S.Kind == StmtKind::Nop &&
                                        usesOf(S.Addr) == 0;
                               }),
                L.end());
        for (HighStmt &S : L) {
          DropPlaceholders(S.Body);
          DropPlaceholders(S.ElseBody);
          for (auto &C : S.Cases)
            DropPlaceholders(C.Body);
          DropPlaceholders(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            DropPlaceholders(ClauseBody);
        }
      };
  DropPlaceholders(Body);

  bool Changed = false;
  // Statements in \p Stmts, counting nested ones.
  std::function<size_t(const std::vector<HighStmt> &)> Size =
      [&](const std::vector<HighStmt> &Stmts) {
        size_t N = 0;
        for (const HighStmt &S : Stmts) {
          ++N;
          N += Size(S.Body) + Size(S.ElseBody) + Size(S.DefaultBody);
          for (const auto &C : S.Cases)
            N += Size(C.Body);
          for (const auto &ClauseBody : S.EHClauseBodies)
            N += Size(ClauseBody);
        }
        return N;
      };
  // Whether a jump enters \p S or a statement nested in it, or \p S holds
  // a loop, switch or try a copy would duplicate.
  std::function<bool(const HighStmt &)> Uncopyable = [&](const HighStmt &S) {
    if ((S.Addr != 0 && S.Addr != InvalidVA && usesOf(S.Addr) != 0) ||
        S.Kind == StmtKind::While || S.Kind == StmtKind::DoWhile ||
        S.Kind == StmtKind::For || S.Kind == StmtKind::Switch ||
        S.Kind == StmtKind::SEHTry || S.Kind == StmtKind::CxxTry ||
        S.Kind == StmtKind::ItaniumTry || S.Kind == StmtKind::Break ||
        S.Kind == StmtKind::Continue)
      return true;
    bool UniqueCall = false;
    forEachExpr(S, [&](const ExprPtr &Expression) {
      UniqueCall |= hasUniqueCallOccurrence(Expression);
    });
    if (UniqueCall)
      return true;
    for (const auto *List : {&S.Body, &S.ElseBody})
      for (const HighStmt &T : *List)
        if (Uncopyable(T))
          return true;
    return false;
  };
  // T16 on L[I]; see its use below. The exit may sit several ifs deep:
  // the rest of each arm on its way down then runs only off the exit.
  struct Level {
    HighStmt *If = nullptr;
    bool Then = true;
    size_t Pos = 0;
  };
  // Paths of ifs from \p If down to an arm that ends in a jump; each level
  // records the arm taken and where the next if (or the jump) sits in it.
  // \p Try sees each path in turn and ends the walk by returning true.
  std::function<bool(HighStmt &, std::vector<Level> &,
                     const std::function<bool(std::vector<Level> &)> &)>
      FindExits =
          [&](HighStmt &If, std::vector<Level> &Path,
              const std::function<bool(std::vector<Level> &)> &Try) -> bool {
    if (Path.size() >= limits::kMaxSkippedCopyDepth)
      return false;
    for (const bool Then : {true, false}) {
      std::vector<HighStmt> &A = Then ? If.Body : If.ElseBody;
      if (!Path.empty() && !A.empty() && A.back().Kind == StmtKind::Goto) {
        Path.push_back({&If, Then, A.size() - 1});
        if (Try(Path))
          return true;
        Path.pop_back();
      }
      for (size_t P = 0; P < A.size(); ++P)
        if ((A[P].Kind == StmtKind::If || A[P].Kind == StmtKind::IfElse) &&
            A[P].Cond) {
          Path.push_back({&If, Then, P});
          if (FindExits(A[P], Path, Try))
            return true;
          Path.pop_back();
        }
    }
    return false;
  };
  auto ArmOf = [](const Level &V) -> std::vector<HighStmt> & {
    return V.Then ? V.If->Body : V.If->ElseBody;
  };
  // Rewrites the exit at the end of \p Path when its label X follows L[I],
  // later in L or as \p After, what runs once L ends.
  auto copyExit = [&](std::vector<HighStmt> &L, size_t I, va_t After,
                      std::vector<Level> &Path) -> bool {
    const size_t D = Path.size();
    // Other jumps to X land after R and keep their label.
    const va_t X = ArmOf(Path[D - 1]).back().GotoTarget;
    if (usesOf(X) == 0 || usesOf(X) == ~0u)
      return false;
    size_t J = I + 1;
    while (J < L.size() && !(L[J].Addr == X && labelStart(L, J)))
      ++J;
    if (J >= L.size() && X != After)
      return false;
    // Tails[K]: the rest of level K's arm after the if one level down.
    std::vector<std::vector<HighStmt>> Tails(D - 1);
    for (size_t K = 0; K + 1 < D; ++K) {
      const std::vector<HighStmt> &A = ArmOf(Path[K]);
      Tails[K].assign(A.begin() + Path[K].Pos + 1, A.end());
    }
    const std::vector<HighStmt> R(L.begin() + I + 1, L.begin() + J);
    for (const auto &T : Tails)
      for (const HighStmt &S : T)
        if (Uncopyable(S))
          return false;
    for (const HighStmt &S : R)
      if (Uncopyable(S))
        return false;
    // Level K's other arm continues, as before, with the tails of the levels
    // above it, innermost first, then R.
    std::vector<std::vector<HighStmt>> Append(D);
    size_t Total = 0;
    for (size_t K = 0; K < D; ++K) {
      for (size_t Up = K; Up-- > 0;)
        Append[K].insert(Append[K].end(), Tails[Up].begin(), Tails[Up].end());
      Append[K].insert(Append[K].end(), R.begin(), R.end());
      Total += Size(Append[K]);
    }
    size_t Original = Size(R);
    for (const auto &T : Tails)
      Original += Size(T);
    if (Total - Original > limits::kMaxSkippedCopyStatements)
      return false;
    for (size_t K = 0; K < D; ++K) {
      const Level &V = Path[K];
      std::vector<HighStmt> &A = ArmOf(V);
      if (K + 1 == D)
        popGoto(A);
      else
        A.erase(A.begin() + V.Pos + 1, A.end());
      if (Append[K].empty())
        continue;
      std::vector<HighStmt> &Other = V.Then ? V.If->ElseBody : V.If->Body;
      V.If->Kind = StmtKind::IfElse;
      Other.insert(Other.end(), std::make_move_iterator(Append[K].begin()),
                   std::make_move_iterator(Append[K].end()));
    }
    --Uses[X];
    L.erase(L.begin() + I + 1, L.begin() + J);
    Changed = true;
    return true;
  };
  auto copySkippedTail = [&](std::vector<HighStmt> &L, size_t I,
                             va_t After) -> bool {
    std::vector<Level> Path;
    return FindExits(L[I], Path, [&](std::vector<Level> &Exit) {
      return copyExit(L, I, After, Exit);
    });
  };

  std::function<void(std::vector<HighStmt> &)> Visit = [&](std::vector<HighStmt>
                                                               &L) {
    for (size_t I = 0; I < L.size(); ++I) {
      // `if (c) {} else { A }`  ->  `if (!c) { A }`.
      if (L[I].Kind == StmtKind::IfElse && L[I].Cond && L[I].Body.empty() &&
          !L[I].ElseBody.empty()) {
        L[I].Kind = StmtKind::If;
        L[I].Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, L[I].Cond);
        L[I].Body = std::move(L[I].ElseBody);
        L[I].ElseBody.clear();
      }
      // `if (c) { A } else {}`  ->  `if (c) { A }`, which the rules below
      // take for a plain if.
      if (L[I].Kind == StmtKind::IfElse && L[I].Cond && L[I].ElseBody.empty())
        L[I].Kind = StmtKind::If;
      // T11: `X: S...; goto X;` -> `while (1) { S... }` and
      // `X: S...; if (c) goto X;` -> `do { S... } while (c);`.
      if (L[I].Kind == StmtKind::Goto || isCondGoto(L[I])) {
        const bool Conditional = L[I].Kind == StmtKind::If;
        const va_t X = Conditional ? L[I].Body[0].GotoTarget : L[I].GotoTarget;
        size_t K = I;
        while (K > 0 && !(L[K - 1].Addr == X && labelStart(L, K - 1)))
          --K;
        if (K > 0 && !labelStart(L, I) &&
            (Conditional || L[I].Addr == 0 || L[I].Addr == InvalidVA ||
             L[I].Addr != L[I - 1].Addr)) {
          --K;
          std::vector<HighStmt> LoopBody(L.begin() + K, L.begin() + I);
          if (!LoopBody.empty() && !hasLooseBreakOrContinue(LoopBody)) {
            HighStmt Loop;
            Loop.Kind = Conditional ? StmtKind::DoWhile : StmtKind::While;
            Loop.Cond = Conditional ? L[I].Cond : HighExpr::makeConst(1, 1);
            Loop.LoopHeaderAddr = X;
            Loop.Body = std::move(LoopBody);
            --Uses[X];
            L.erase(L.begin() + K, L.begin() + I + 1);
            L.insert(L.begin() + K, std::move(Loop));
            I = K;
            Changed = true;
          }
        }
      }
      // T10: `if (a) { if (b) { S } }`  ->  `if (a && b) { S }`.
      while (L[I].Kind == StmtKind::If && L[I].Cond && L[I].ElseBody.empty() &&
             L[I].Body.size() == 1 && L[I].Body[0].Kind == StmtKind::If &&
             L[I].Body[0].Cond && L[I].Body[0].ElseBody.empty() &&
             (L[I].Body[0].Addr == 0 || L[I].Body[0].Addr == InvalidVA ||
              usesOf(L[I].Body[0].Addr) == 0)) {
        HighStmt Inner = std::move(L[I].Body[0]);
        ExprPtr Merged =
            HighExpr::makeBinop(NdOp::BOOL_AND, L[I].Cond, Inner.Cond);
        Merged->Type = NdType::makeInt(1, false);
        L[I].Cond = Merged;
        L[I].Body = std::move(Inner.Body);
        Changed = true;
      }
      // T0: a temporary read only by the next condition is evaluated at
      // exactly the same point when folded into it.
      if (I + 1 < L.size() && L[I].Kind == StmtKind::Assign &&
          !L[I].IsPhiCopy && L[I].Dst && L[I].Dst->Kind == ExprKind::Var &&
          L[I].Val && Foldable(*L[I].Val, 0) && !labelStart(L, I) &&
          !labelStart(L, I + 1) &&
          (L[I + 1].Kind == StmtKind::If ||
           L[I + 1].Kind == StmtKind::IfElse) &&
          L[I + 1].Cond) {
        const VarKey Key = varKey(L[I].Dst->Var);
        auto ReadIt = Reads.find(Key);
        unsigned CondReads = 0;
        std::function<void(const ExprPtr &)> CountKey = [&](const ExprPtr &E) {
          if (!E)
            return;
          if (E->Kind == ExprKind::Var && varKey(E->Var) == Key)
            ++CondReads;
          E->forEachChildExpr(CountKey);
        };
        CountKey(L[I + 1].Cond);
        if (ReadIt != Reads.end() && ReadIt->second == 1 && CondReads == 1 &&
            Substitute(L[I + 1].Cond, Key, L[I].Val)) {
          ReadIt->second = 0;
          L.erase(L.begin() + I);
          Changed = true;
        }
      }
      // T3: merge a run of conditional jumps to one target.
      while (I + 1 < L.size() && isCondGoto(L[I]) && isCondGoto(L[I + 1]) &&
             L[I].Body[0].GotoTarget == L[I + 1].Body[0].GotoTarget &&
             !labelStart(L, I + 1)) {
        ExprPtr Merged =
            HighExpr::makeBinop(NdOp::BOOL_OR, L[I].Cond, L[I + 1].Cond);
        Merged->Type = NdType::makeInt(1, false);
        L[I].Cond = Merged;
        --Uses[L[I + 1].Body[0].GotoTarget];
        L.erase(L.begin() + I + 1);
        Changed = true;
      }
      // T4: consecutive ifs with the same straight-line body. `a || b` runs
      // the body once and skips `b` when `a` holds, which matches only a body
      // that leaves: otherwise both tests would run it, and `b` after it.
      while (I + 1 < L.size() && L[I].Kind == StmtKind::If && L[I].Cond &&
             L[I].ElseBody.empty() && L[I + 1].Kind == StmtKind::If &&
             L[I + 1].Cond && L[I + 1].ElseBody.empty() &&
             !labelStart(L, I + 1) && !L[I].Body.empty() &&
             (L[I].Body.back().Kind == StmtKind::Return ||
              L[I].Body.back().Kind == StmtKind::Goto) &&
             sameStraightLineBody(L[I].Body, L[I + 1].Body)) {
        ExprPtr Merged =
            HighExpr::makeBinop(NdOp::BOOL_OR, L[I].Cond, L[I + 1].Cond);
        Merged->Type = NdType::makeInt(1, false);
        L[I].Cond = Merged;
        walkStmts(L[I + 1].Body, [&](const HighStmt &S) {
          if (S.Kind == StmtKind::Goto)
            --Uses[S.GotoTarget];
        });
        L.erase(L.begin() + I + 1);
        Changed = true;
      }
      // T2': any `if (c) { ...; goto Y; } S...; Y:` becomes if/else.
      if (L[I].Kind == StmtKind::If && L[I].Cond && L[I].ElseBody.empty() &&
          !L[I].Body.empty() && L[I].Body.back().Kind == StmtKind::Goto) {
        const va_t Y = L[I].Body.back().GotoTarget;
        size_t J = I + 1;
        while (J < L.size() && !(L[J].Addr == Y && labelStart(L, J)))
          ++J;
        // A label after a terminator starts an out-of-line block; moving
        // that block (T1) reads better than wrapping everything before it.
        // Once the local rewrites have settled (SpliceRegions), a jump that
        // T1 could not absorb still becomes an if/else.
        if (J < L.size() && J > I + 1 &&
            (!isTerminator(L[J - 1]) || SpliceRegions)) {
          popGoto(L[I].Body);
          --Uses[Y];
          L[I].Kind = StmtKind::IfElse;
          L[I].ElseBody.assign(std::make_move_iterator(L.begin() + I + 1),
                               std::make_move_iterator(L.begin() + J));
          L.erase(L.begin() + I + 1, L.begin() + J);
          if (L[I].Body.empty()) {
            // `if (c) goto Y; S...; Y:`  ->  `if (!c) { S... }`.
            L[I].Kind = StmtKind::If;
            L[I].Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, L[I].Cond);
            L[I].Body = std::move(L[I].ElseBody);
            L[I].ElseBody.clear();
          }
          Changed = true;
          continue;
        }
      }
      // T2e: `if (c) { A } else { B; goto Y; } S...; Y:` runs S only after A,
      // so S moves to the end of A's arm.  A label in S may be entered only
      // from inside S, so the move takes its jumps along: a jump into the arm
      // would be legal C but no clearer, and a labelled tail left in place
      // can still be copied to each of its jumps.  Y and every label in S
      // must be the one statement starting its address, since a label
      // resolves to the first statement printed there and S now prints
      // before B.
      if (L[I].Kind == StmtKind::IfElse && L[I].Cond &&
          !L[I].ElseBody.empty() &&
          L[I].ElseBody.back().Kind == StmtKind::Goto &&
          (L[I].Body.empty() || !isTerminator(L[I].Body.back()))) {
        const va_t Y = L[I].ElseBody.back().GotoTarget;
        size_t J = I + 1;
        while (J < L.size() && !(L[J].Addr == Y && labelStart(L, J)))
          ++J;
        // The labels S starts and the jumps inside S to each address.
        std::vector<va_t> Labels;
        std::map<va_t, unsigned> Inside;
        std::function<void(const std::vector<HighStmt> &, size_t, size_t)>
            Scan = [&](const std::vector<HighStmt> &List, size_t From,
                       size_t To) {
              for (size_t K = From; K < To; ++K) {
                const HighStmt &S = List[K];
                if (labelStart(List, K))
                  Labels.push_back(S.Addr);
                if (S.Kind == StmtKind::Goto)
                  ++Inside[S.GotoTarget];
                for (const auto *Inner : {&S.Body, &S.ElseBody, &S.DefaultBody})
                  Scan(*Inner, 0, Inner->size());
                for (const auto &C : S.Cases)
                  Scan(C.Body, 0, C.Body.size());
                for (const auto &ClauseBody : S.EHClauseBodies)
                  Scan(ClauseBody, 0, ClauseBody.size());
              }
            };
        bool SelfContained = J < L.size();
        if (SelfContained) {
          Scan(L, I + 1, J);
          for (va_t Label : Labels)
            SelfContained &=
                AddressStarts[Label] == 1 && usesOf(Label) == Inside[Label];
        }
        if (J < L.size() && J > I + 1 && AddressStarts[Y] == 1 &&
            (!isTerminator(L[J - 1]) || SpliceRegions) && SelfContained) {
          popGoto(L[I].ElseBody);
          --Uses[Y];
          L[I].Body.insert(L[I].Body.end(),
                           std::make_move_iterator(L.begin() + I + 1),
                           std::make_move_iterator(L.begin() + J));
          L.erase(L.begin() + I + 1, L.begin() + J);
          if (L[I].ElseBody.empty())
            L[I].Kind = StmtKind::If;
          Changed = true;
          continue;
        }
      }
      // T9: `if (a) goto L; if (b) { L: S }`  ->  `if (a || b) { S }`.
      if (isCondGoto(L[I]) && I + 1 < L.size() &&
          L[I + 1].Kind == StmtKind::If && L[I + 1].Cond &&
          L[I + 1].ElseBody.empty() && !L[I + 1].Body.empty() &&
          !labelStart(L, I + 1) &&
          L[I + 1].Body.front().Addr == L[I].Body[0].GotoTarget) {
        --Uses[L[I].Body[0].GotoTarget];
        ExprPtr Merged =
            HighExpr::makeBinop(NdOp::BOOL_OR, L[I].Cond, L[I + 1].Cond);
        Merged->Type = NdType::makeInt(1, false);
        L[I + 1].Cond = Merged;
        L.erase(L.begin() + I);
        Changed = true;
        if (I > 0)
          --I;
        continue;
      }
      // T12: `goto X; do { S...; X: } while (c);`  ->  `while (c) { S... }`.
      // A jump to the loop itself enters its body before the test; the
      // rewritten loop would test first (T12) or start mid-body (T12b).
      const bool LoopEntered = I + 1 < L.size() && L[I + 1].Addr != 0 &&
                               L[I + 1].Addr != InvalidVA &&
                               usesOf(L[I + 1].Addr) != 0;
      if (L[I].Kind == StmtKind::Goto && I + 1 < L.size() && !LoopEntered &&
          L[I + 1].Kind == StmtKind::DoWhile && L[I + 1].Cond &&
          usesOf(L[I].GotoTarget) == 1 && !L[I + 1].Body.empty()) {
        auto &Loop = L[I + 1].Body;
        const HighStmt &Last = Loop.back();
        if (Last.Kind == StmtKind::Block && Last.Body.empty() &&
            Last.Addr == L[I].GotoTarget &&
            (Loop.size() == 1 || Loop[Loop.size() - 2].Addr != Last.Addr) &&
            !(L[I].Addr != 0 && L[I].Addr != InvalidVA &&
              usesOf(L[I].Addr) != 0)) {
          --Uses[L[I].GotoTarget];
          Loop.pop_back();
          L[I + 1].Kind = StmtKind::While;
          L.erase(L.begin() + I);
          Changed = true;
          continue;
        }
      }
      // T12b: `goto X; do { A...; X: B... } while (c);`  ->
      // `while (1) { B...; if (!c) break; A... }`.
      if (L[I].Kind == StmtKind::Goto && I + 1 < L.size() && !LoopEntered &&
          L[I + 1].Kind == StmtKind::DoWhile && L[I + 1].Cond &&
          usesOf(L[I].GotoTarget) == 1 &&
          !(L[I].Addr != 0 && L[I].Addr != InvalidVA &&
            usesOf(L[I].Addr) != 0) &&
          !hasLooseBreakOrContinue(L[I + 1].Body)) {
        auto &Loop = L[I + 1].Body;
        size_t K = 1;
        while (K < Loop.size() && !(Loop[K].Addr == L[I].GotoTarget &&
                                    Loop[K - 1].Addr != Loop[K].Addr))
          ++K;
        if (K < Loop.size()) {
          std::vector<HighStmt> Rotated(
              std::make_move_iterator(Loop.begin() + K),
              std::make_move_iterator(Loop.end()));
          HighStmt Exit;
          Exit.Kind = StmtKind::If;
          Exit.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, L[I + 1].Cond);
          HighStmt Break;
          Break.Kind = StmtKind::Break;
          Exit.Body.push_back(std::move(Break));
          Rotated.push_back(std::move(Exit));
          Rotated.insert(Rotated.end(), std::make_move_iterator(Loop.begin()),
                         std::make_move_iterator(Loop.begin() + K));
          Loop = std::move(Rotated);
          L[I + 1].Kind = StmtKind::While;
          L[I + 1].Cond = nullptr;
          --Uses[L[I].GotoTarget];
          L.erase(L.begin() + I);
          Changed = true;
          continue;
        }
      }
      // T13: `if (c) { ...; return; } S...; Y:` where the arm jumps to Y:
      // the arm never falls through, so S can be its else, after which the
      // jumps to Y are fall-through exits of the if.
      if (SpliceRegions && L[I].Kind == StmtKind::If && L[I].Cond &&
          L[I].ElseBody.empty() && !L[I].Body.empty() &&
          isTerminator(L[I].Body.back())) {
        std::set<va_t> ArmTargets;
        walkStmts(L[I].Body, [&](const HighStmt &S) {
          if (S.Kind == StmtKind::Goto)
            ArmTargets.insert(S.GotoTarget);
        });
        if (L[I].Body.back().Kind == StmtKind::Goto)
          ArmTargets.erase(L[I].Body.back().GotoTarget);
        size_t J = I + 1;
        while (J < L.size() &&
               !(labelStart(L, J) && ArmTargets.count(L[J].Addr)))
          ++J;
        if (J < L.size() && J > I + 1) {
          L[I].Kind = StmtKind::IfElse;
          L[I].ElseBody.assign(std::make_move_iterator(L.begin() + I + 1),
                               std::make_move_iterator(L.begin() + J));
          L.erase(L.begin() + I + 1, L.begin() + J);
          Changed = true;
        }
      }
      // T8: `if (c) { A; goto Y; } else { B; Y: C }`
      //   ->  `if (c) { A } else { B }` C.
      if (L[I].Kind == StmtKind::IfElse && L[I].Cond && !L[I].Body.empty() &&
          L[I].Body.back().Kind == StmtKind::Goto && !L[I].ElseBody.empty() &&
          L[I].Body.back().GotoTarget != 0 &&
          L[I].Body.back().GotoTarget != InvalidVA) {
        const va_t Y = L[I].Body.back().GotoTarget;
        auto &Else = L[I].ElseBody;
        size_t J = 0;
        while (J < Else.size() &&
               !(Else[J].Addr == Y && (J == 0 || Else[J - 1].Addr != Y)))
          ++J;
        if (J < Else.size()) {
          --Uses[Y];
          popGoto(L[I].Body);
          std::vector<HighStmt> Tail(std::make_move_iterator(Else.begin() + J),
                                     std::make_move_iterator(Else.end()));
          Else.erase(Else.begin() + J, Else.end());
          if (Else.empty())
            L[I].Kind = StmtKind::If;
          else if (L[I].Body.empty()) {
            L[I].Kind = StmtKind::If;
            L[I].Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, L[I].Cond);
            L[I].Body = std::move(Else);
            Else.clear();
          }
          L.insert(L.begin() + I + 1, std::make_move_iterator(Tail.begin()),
                   std::make_move_iterator(Tail.end()));
          Changed = true;
        }
      }
      // T14 (late): `if (c) { ...; jump; } else { S... }`  ->
      // `if (c) { ...; jump; }` S...: the arm never reaches S.
      if (SpliceRegions && L[I].Kind == StmtKind::IfElse && L[I].Cond &&
          !L[I].Body.empty() && !L[I].ElseBody.empty() &&
          (isTerminator(L[I].Body.back()) ||
           L[I].Body.back().Kind == StmtKind::Break ||
           L[I].Body.back().Kind == StmtKind::Continue) &&
          // T13's shape: the arm exits to the label right after the if/else,
          // which the fall-through rewrite still has to absorb.
          !(I + 1 < L.size() && labelStart(L, I + 1) && [&] {
            bool Exits = false;
            walkStmts(L[I].Body, [&](const HighStmt &S) {
              Exits |=
                  S.Kind == StmtKind::Goto && S.GotoTarget == L[I + 1].Addr;
            });
            return Exits;
          }())) {
        std::vector<HighStmt> Tail = std::move(L[I].ElseBody);
        L[I].ElseBody.clear();
        L[I].Kind = StmtKind::If;
        L.insert(L.begin() + I + 1, std::make_move_iterator(Tail.begin()),
                 std::make_move_iterator(Tail.end()));
        Changed = true;
      }
      // T15 (late): `if (c) { T } R...; goto X;` where X starts T and
      // neither T nor R falls through: `if (!c) { R... } T`, so the jump
      // back into T becomes the fall-through out of the if.
      if (SpliceRegions && L[I].Kind == StmtKind::If && L[I].Cond &&
          L[I].ElseBody.empty() && !L[I].Body.empty() && I + 1 < L.size() &&
          labelStart(L[I].Body, 0) && usesOf(L[I].Body.front().Addr) != ~0u) {
        // A label after the last jump still runs past the end.
        auto LastOf = [&](const std::vector<HighStmt> &Stmts,
                          size_t From) -> const HighStmt * {
          for (size_t K = Stmts.size(); K > From; --K)
            if (!isEmptyAnchor(Stmts[K - 1]) || usesOf(Stmts[K - 1].Addr) != 0)
              return &Stmts[K - 1];
          return nullptr;
        };
        const va_t X = L[I].Body.front().Addr;
        const HighStmt *LastT = LastOf(L[I].Body, 0);
        const HighStmt *LastR = LastOf(L, I + 1);
        bool TEntersR = false;
        if (LastT && isTerminator(*LastT) && LastR &&
            LastR->Kind == StmtKind::Goto && LastR->GotoTarget == X) {
          // Every entered label in R, at any depth.
          std::set<va_t> RLabels;
          std::function<void(const std::vector<HighStmt> &, size_t)> Collect =
              [&](const std::vector<HighStmt> &Stmts, size_t From) {
                for (size_t K = From; K < Stmts.size(); ++K) {
                  if (labelStart(Stmts, K))
                    RLabels.insert(Stmts[K].Addr);
                  Collect(Stmts[K].Body, 0);
                  Collect(Stmts[K].ElseBody, 0);
                  for (const auto &C : Stmts[K].Cases)
                    Collect(C.Body, 0);
                  Collect(Stmts[K].DefaultBody, 0);
                  for (const auto &ClauseBody : Stmts[K].EHClauseBodies)
                    Collect(ClauseBody, 0);
                }
              };
          Collect(L, I + 1);
          walkStmts(L[I].Body, [&](const HighStmt &S) {
            TEntersR |= S.Kind == StmtKind::Goto && RLabels.count(S.GotoTarget);
          });
          if (!TEntersR) {
            std::vector<HighStmt> T = std::move(L[I].Body);
            std::vector<HighStmt> R(std::make_move_iterator(L.begin() + I + 1),
                                    std::make_move_iterator(L.end()));
            L.erase(L.begin() + I + 1, L.end());
            L[I].Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, L[I].Cond);
            L[I].Body = std::move(R);
            L.insert(L.end(), std::make_move_iterator(T.begin()),
                     std::make_move_iterator(T.end()));
            Changed = true;
          }
        }
      }
      if (!isCondGoto(L[I]))
        continue;
      const va_t X = L[I].Body[0].GotoTarget;
      if (usesOf(X) != 1)
        continue;
      // T1: find X's block after this jump, entered only here.
      size_t K = I + 1;
      while (K < L.size() && !(L[K].Addr == X && labelStart(L, K)))
        ++K;
      if (K >= L.size() || K == I + 1 || !isTerminator(L[K - 1]))
        continue;
      size_t M = K;
      bool OtherEntry = false;
      while (M < L.size() && !isTerminator(L[M])) {
        if (M > K && labelStart(L, M) && L[M].Addr != X) {
          OtherEntry = true;
          break;
        }
        ++M;
      }
      if (OtherEntry || M >= L.size() ||
          (M > K && labelStart(L, M) && L[M].Addr != X))
        continue;
      std::vector<HighStmt> Block(std::make_move_iterator(L.begin() + K),
                                  std::make_move_iterator(L.begin() + M + 1));
      L.erase(L.begin() + K, L.begin() + M + 1);
      --Uses[X];
      L[I].Body = std::move(Block);
      Changed = true;

      // T2: the moved block rejoins at a label after this `if`.
      const HighStmt &Last = L[I].Body.back();
      if (Last.Kind != StmtKind::Goto)
        continue;
      const va_t Y = Last.GotoTarget;
      size_t J = I + 1;
      while (J < L.size() && !(L[J].Addr == Y && labelStart(L, J)))
        ++J;
      if (J >= L.size())
        continue;
      popGoto(L[I].Body);
      --Uses[Y];
      L[I].Kind = StmtKind::IfElse;
      L[I].ElseBody.assign(std::make_move_iterator(L.begin() + I + 1),
                           std::make_move_iterator(L.begin() + J));
      L.erase(L.begin() + I + 1, L.begin() + J);
      if (L[I].Body.empty()) {
        // `if (c) {} else { S }`  ->  `if (!c) { S }`.
        L[I].Kind = StmtKind::If;
        L[I].Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, L[I].Cond);
        L[I].Body = std::move(L[I].ElseBody);
        L[I].ElseBody.clear();
      }
    }
    for (HighStmt &S : L) {
      Visit(S.Body);
      Visit(S.ElseBody);
      for (auto &C : S.Cases)
        Visit(C.Body);
      Visit(S.DefaultBody);
      for (auto &ClauseBody : S.EHClauseBodies)
        Visit(ClauseBody);
    }
  };
  Visit(Body);

  // T1g: a block entered only by one goto anywhere in the tree, placed after
  // a terminator and ending in one, is spliced in place of that goto.  The
  // original statements become Nop.  Blocks never cross the boundary of a
  // try body or of one of its handlers, which only the exception dispatcher
  // enters, and carry no break/continue whose target the move could change.
  struct Site {
    std::vector<HighStmt> *List = nullptr;
    size_t Index = 0;
    /// The innermost try body or handler holding the site.
    const void *Try = nullptr;
    // Position in a pre-order walk of the tree: program order.
    size_t Order = 0;
    // Every (list, index) enclosing the site, outermost first.
    std::vector<std::pair<const std::vector<HighStmt> *, size_t>> Chain;
  };
  // A no-op that a jump still enters falls through like any statement.
  auto prevNonNop = [&](const std::vector<HighStmt> &L,
                        size_t K) -> const HighStmt * {
    while (K > 0) {
      --K;
      if (L[K].Kind != StmtKind::Nop || labelStart(L, K))
        return &L[K];
    }
    return nullptr;
  };
  // End of the region starting at the label at \p K: the first terminator
  // followed by a label (or the list end) such that every label starting
  // after K up to it is entered only by gotos inside the region.
  auto regionEnd = [&](const std::vector<HighStmt> &LL,
                       size_t K) -> std::optional<size_t> {
    constexpr size_t kMaxRegionStmts = 256;
    std::map<va_t, unsigned> Internal;
    std::set<va_t> Inner;
    for (size_t M = K; M < LL.size() && M - K < kMaxRegionStmts; ++M) {
      if (M > K && labelStart(LL, M)) {
        if (usesOf(LL[M].Addr) == ~0u)
          return std::nullopt;
        Inner.insert(LL[M].Addr);
      }
      std::function<void(const HighStmt &)> CountGotos =
          [&](const HighStmt &S) {
            if (S.Kind == StmtKind::Goto)
              ++Internal[S.GotoTarget];
          };
      CountGotos(LL[M]);
      walkStmts(LL[M].Body, CountGotos);
      walkStmts(LL[M].ElseBody, CountGotos);
      for (const auto &C : LL[M].Cases)
        walkStmts(C.Body, CountGotos);
      walkStmts(LL[M].DefaultBody, CountGotos);
      for (const auto &ClauseBody : LL[M].EHClauseBodies)
        walkStmts(ClauseBody, CountGotos);
      if (!isTerminator(LL[M]) ||
          !(M + 1 == LL.size() || labelStart(LL, M + 1)))
        continue;
      bool Closed = true;
      for (va_t Label : Inner)
        if (Internal[Label] != usesOf(Label)) {
          Closed = false;
          break;
        }
      if (Closed)
        return M;
    }
    return std::nullopt;
  };
  for (unsigned Applied = 0; Applied < 512; ++Applied) {
    std::map<va_t, Site> Labels;
    std::vector<std::pair<Site, va_t>> Gotos;
    std::vector<std::pair<const std::vector<HighStmt> *, size_t>> Chain;
    // Statements starting each address group: a jump can name an address
    // only when exactly one statement starts it.
    std::map<va_t, unsigned> Starts;
    size_t Order = 0;
    std::function<void(std::vector<HighStmt> &, const void *)> Collect =
        [&](std::vector<HighStmt> &L, const void *Try) {
          for (size_t I = 0; I < L.size(); ++I) {
            HighStmt &S = L[I];
            ++Order;
            if (S.Addr != 0 && S.Addr != InvalidVA &&
                (I == 0 || L[I - 1].Addr != S.Addr))
              ++Starts[S.Addr];
            if (labelStart(L, I) && S.Kind != StmtKind::Nop)
              Labels.emplace(S.Addr, Site{&L, I, Try, Order, Chain});
            if (S.Kind == StmtKind::Goto && usesOf(S.GotoTarget) == 1)
              Gotos.push_back({Site{&L, I, Try, Order, Chain}, S.GotoTarget});
            Chain.push_back({&L, I});
            const bool IsTry = S.Kind == StmtKind::SEHTry ||
                               S.Kind == StmtKind::CxxTry ||
                               S.Kind == StmtKind::ItaniumTry;
            Collect(S.Body, IsTry ? static_cast<const void *>(&S) : Try);
            Collect(S.ElseBody, Try);
            for (auto &C : S.Cases)
              Collect(C.Body, Try);
            Collect(S.DefaultBody, Try);
            for (auto &ClauseBody : S.EHClauseBodies)
              Collect(ClauseBody, &ClauseBody);
            Chain.pop_back();
          }
        };
    Collect(Body, nullptr);
    // What runs after the list holding \p At falls off its end: the next
    // statement after the if/else, block or switch around it, when exactly
    // one statement starts that address.
    auto FollowOf = [&](const Site &At) -> va_t {
      for (size_t D = At.Chain.size(); D-- > 0;) {
        const auto &[ParentList, ParentIndex] = At.Chain[D];
        const HighStmt &Parent = (*ParentList)[ParentIndex];
        if (Parent.Kind != StmtKind::If && Parent.Kind != StmtKind::IfElse &&
            Parent.Kind != StmtKind::Block && Parent.Kind != StmtKind::Switch)
          return 0;
        if (ParentIndex + 1 == ParentList->size())
          continue;
        const HighStmt &Next = (*ParentList)[ParentIndex + 1];
        if (Next.Kind == StmtKind::Nop || Next.Addr == 0 ||
            Next.Addr == InvalidVA || Next.Addr == Parent.Addr)
          return 0;
        auto It = Starts.find(Next.Addr);
        return It != Starts.end() && It->second == 1 ? Next.Addr : 0;
      }
      return 0;
    };
    bool Done = false;
    for (auto &[GotoSite, X] : Gotos) {
      auto LabelIt = Labels.find(X);
      if (LabelIt == Labels.end())
        continue;
      Site &Block = LabelIt->second;
      std::vector<HighStmt> &LL = *Block.List;
      const size_t K = Block.Index;
      const HighStmt *Prev = prevNonNop(LL, K);
      if (!Prev || !isTerminator(*Prev) || Block.Try != GotoSite.Try)
        continue;
      size_t M = K;
      bool Bad = false;
      // The first label the block runs into before any terminator.
      size_t FallInto = 0;
      while (M < LL.size() && !isTerminator(LL[M])) {
        if (M > K && labelStart(LL, M)) {
          if (!Bad)
            FallInto = M;
          Bad = true;
        }
        ++M;
      }
      va_t FallTarget = 0;
      if (Bad || M >= LL.size() || (M > K && labelStart(LL, M))) {
        // A longer region also qualifies when every label starting inside
        // it is entered only from inside it: the whole region then moves
        // as one unit and its internal jumps stay internal.
        std::optional<size_t> End =
            SpliceRegions ? regionEnd(LL, K) : std::nullopt;
        if (End) {
          M = *End;
        } else if (SpliceRegions && FallInto > K &&
                   usesOf(LL[FallInto].Addr) != ~0u) {
          // A block that runs into the next label moves too; its fall-through
          // becomes a jump to that label, which later rewrites can merge
          // with the jumps of its new neighbours.
          M = FallInto - 1;
          FallTarget = LL[FallInto].Addr;
        } else if (SpliceRegions && !Bad && M >= LL.size() && M > K) {
          // A block that runs off the end of its arm continues where the
          // arm does; it moves, with a jump there, to a jump ahead of it.
          FallTarget = FollowOf(Block);
          if (!FallTarget)
            continue;
          M = LL.size() - 1;
        } else {
          continue;
        }
        Bad = false;
      }
      // The goto must not sit inside the block it would receive.
      for (const auto &[List, Index] : GotoSite.Chain)
        if (List == &LL && Index >= K && Index <= M)
          Bad = true;
      if (GotoSite.List == &LL && GotoSite.Index >= K && GotoSite.Index <= M)
        Bad = true;
      std::vector<HighStmt> Moved(LL.begin() + K, LL.begin() + M + 1);
      Bad |= hasLooseBreakOrContinue(Moved);
      if (Bad)
        continue;
      if (FallTarget) {
        HighStmt Jump;
        Jump.Kind = StmtKind::Goto;
        Jump.GotoTarget = FallTarget;
        Moved.push_back(std::move(Jump));
        ++Uses[FallTarget];
      }
      for (size_t J = K; J <= M; ++J) {
        HighStmt Removed;
        Removed.Kind = StmtKind::Nop;
        LL[J] = std::move(Removed);
      }
      std::vector<HighStmt> &GL = *GotoSite.List;
      // A goto carrying an entered label keeps it as an empty block.
      HighStmt &G = GL[GotoSite.Index];
      size_t InsertAt = GotoSite.Index;
      if (labelStart(GL, GotoSite.Index)) {
        HighStmt Anchor;
        Anchor.Kind = StmtKind::Block;
        Anchor.Addr = G.Addr;
        G = std::move(Anchor);
        ++InsertAt;
      } else {
        GL.erase(GL.begin() + GotoSite.Index);
      }
      GL.insert(GL.begin() + InsertAt, std::make_move_iterator(Moved.begin()),
                std::make_move_iterator(Moved.end()));
      --Uses[X];
      Changed = true;
      Done = true;
      break;
    }
    if (!Done)
      break;
  }

  // T16 (late, once no block can move instead): `if (a) { A; if (b) { B;
  // goto X; } T } R; X:` with a small R that nothing enters becomes
  // `if (a) { A; if (b) { B } else { T R } } else { R }`: R runs on every
  // path but the exit, so a copy of it goes on each.
  if (SpliceRegions) {
    // \p After: the label of what runs once L ends, when it is the next
    // statement after the if/else, block or switch owning L.
    std::function<void(std::vector<HighStmt> &, va_t)> CopyTails =
        [&](std::vector<HighStmt> &L, va_t After) {
          for (size_t I = 0; I < L.size(); ++I)
            if ((L[I].Kind == StmtKind::If || L[I].Kind == StmtKind::IfElse) &&
                L[I].Cond)
              copySkippedTail(L, I, After);
          for (size_t K = 0; K < L.size(); ++K) {
            HighStmt &S = L[K];
            va_t Next = After;
            if (K + 1 < L.size())
              Next = labelStart(L, K + 1) ? L[K + 1].Addr : 0;
            const bool Arms =
                S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse ||
                S.Kind == StmtKind::Block || S.Kind == StmtKind::Switch;
            const va_t Inner = Arms ? Next : 0;
            CopyTails(S.Body, Inner);
            CopyTails(S.ElseBody, Inner);
            for (auto &C : S.Cases)
              CopyTails(C.Body, Inner);
            CopyTails(S.DefaultBody, Inner);
            for (auto &ClauseBody : S.EHClauseBodies)
              CopyTails(ClauseBody, 0);
          }
        };
    CopyTails(Body, 0);
  }

  // A list ends in `goto F` directly, or through `if (c) { A...; goto F; }
  // B...` where B never falls through and nothing enters it: that is
  // `if (!c) { B... } A...; goto F;`. With \p Apply the rewrite is made, so
  // the list then ends in the jump itself.
  auto Entered = [&](va_t Addr) { return usesOf(Addr) != 0; };
  std::function<bool(std::vector<HighStmt> &, va_t, bool)> EndsInJumpTo =
      [&](std::vector<HighStmt> &L, va_t F, bool Apply) -> bool {
    if (L.empty())
      return false;
    if (L.back().Kind == StmtKind::Goto && L.back().GotoTarget == F)
      return true;
    if (!isTerminator(L.back()))
      return false;
    for (size_t I = L.size() - 1; I-- > 0;) {
      if (anyAddressEntered(L[I + 1], Entered))
        return false;
      HighStmt &S = L[I];
      if (S.Kind != StmtKind::If || !S.Cond || !S.ElseBody.empty() ||
          !EndsInJumpTo(S.Body, F, false))
        continue;
      if (!Apply)
        return true;
      EndsInJumpTo(S.Body, F, true);
      std::vector<HighStmt> Taken = std::move(S.Body);
      S.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, S.Cond);
      S.Body.assign(std::make_move_iterator(L.begin() + I + 1),
                    std::make_move_iterator(L.end()));
      L.erase(L.begin() + I + 1, L.end());
      L.insert(L.end(), std::make_move_iterator(Taken.begin()),
               std::make_move_iterator(Taken.end()));
      return true;
    }
    return false;
  };

  // T5: a goto ending an if/else/block body whose target is exactly the
  // statement that runs next after that construct is a fall-through.
  // T6: `if (c) { ...; goto F; } rest...` where F follows the whole list
  // becomes `if (c) { ... } else { rest... }`: the rest flows to F as well.
  std::function<void(std::vector<HighStmt> &, va_t)> DropFallthrough =
      [&](std::vector<HighStmt> &L, va_t Follow) {
        if (Follow != 0 && Follow != InvalidVA)
          for (size_t I = 0; I + 1 < L.size(); ++I)
            if (L[I].Kind == StmtKind::If && L[I].Cond &&
                L[I].ElseBody.empty() && !L[I].Body.empty() &&
                EndsInJumpTo(L[I].Body, Follow, false)) {
              EndsInJumpTo(L[I].Body, Follow, true);
              popGoto(L[I].Body);
              --Uses[Follow];
              L[I].Kind = StmtKind::IfElse;
              L[I].ElseBody.assign(std::make_move_iterator(L.begin() + I + 1),
                                   std::make_move_iterator(L.end()));
              L.erase(L.begin() + I + 1, L.end());
              if (L[I].Body.empty()) {
                // `if (c) goto F; rest...`  ->  `if (!c) { rest... }`.
                L[I].Kind = StmtKind::If;
                L[I].Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, L[I].Cond);
                L[I].Body = std::move(L[I].ElseBody);
                L[I].ElseBody.clear();
              }
              Changed = true;
              break;
            }
        if (!L.empty() && L.back().Kind == StmtKind::Goto && Follow != 0 &&
            Follow != InvalidVA && L.back().GotoTarget == Follow) {
          --Uses[Follow];
          popGoto(L);
          Changed = true;
        }
        for (size_t I = 0; I < L.size(); ++I) {
          // The next statement starts a label only when it begins its
          // address group; otherwise a goto to that address lands earlier.
          va_t Next = Follow;
          if (I + 1 < L.size())
            Next = L[I + 1].Addr != L[I].Addr ? L[I + 1].Addr : InvalidVA;
          HighStmt &S = L[I];
          switch (S.Kind) {
          case StmtKind::If:
          case StmtKind::IfElse:
          case StmtKind::Block:
            DropFallthrough(S.Body, Next);
            DropFallthrough(S.ElseBody, Next);
            break;
          case StmtKind::SEHTry: {
            // A __try body or __except handler that ends continues after the
            // statement. Under a __finally a jump out is an abnormal
            // termination instead, so it stays.
            const bool ExceptOnly =
                !S.EHClauses.empty() &&
                std::all_of(S.EHClauses.begin(), S.EHClauses.end(),
                            [](const HighEHClause &Clause) {
                              return Clause.Kind == HighEHClauseKind::SEHExcept;
                            });
            const va_t After = ExceptOnly ? Next : InvalidVA;
            DropFallthrough(S.Body, After);
            for (auto &ClauseBody : S.EHClauseBodies)
              DropFallthrough(ClauseBody, After);
            break;
          }
          default:
            DropFallthrough(S.Body, InvalidVA);
            DropFallthrough(S.ElseBody, InvalidVA);
            for (auto &C : S.Cases)
              DropFallthrough(C.Body, InvalidVA);
            DropFallthrough(S.DefaultBody, InvalidVA);
            for (auto &ClauseBody : S.EHClauseBodies)
              DropFallthrough(ClauseBody, InvalidVA);
            break;
          }
        }
      };
  DropFallthrough(Body, InvalidVA);
  return Changed;
}

void MedToHighConverter::simplifyControlFlow(HighFunc &Func,
                                             const MedFunc &Med) {
  const bool IsMega = Func.Body.size() > limits::kMaxStructuredHighStmts;
  const char *Detail = std::getenv("NEVERD_HIGHIR_DETAIL");
  const bool WantDetail = Detail && Detail[0] == '1' && Detail[1] == '\0';
  size_t NestedStatements = 0;
  if (Func.Body.size() > 512)
    walkStmts(Func.Body, [&](const HighStmt &) { ++NestedStatements; });
  // Every if/else pass revisits nested arms; cap optional folding before a
  // shallow outer list multiplies work across thousands of descendants.
  const bool TooNestedForIfElse = NestedStatements > 1536;
  auto Now = [] { return std::chrono::steady_clock::now(); };
  auto ElapsedMs = [](auto Start, auto End) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(End - Start)
        .count();
  };

  std::unordered_map<va_t, int> AddrToBlock;
  AddrToBlock.reserve(Med.Blocks.size());
  for (auto &Block : Med.Blocks)
    if (!Block.Ops.empty())
      AddrToBlock[Block.Ops.front().Addr] = Block.Id;

  auto TLoops = Now();
  detectAndConvertLoops(Func, AddrToBlock, Med, IsMega);
  auto TSwitch = Now();
  if (!IsMega)
    recoverSwitchStatements(Func);
  if (!IsMega)
    pullCompareTreeCases(Func, Med);

  int IfElseMaxPasses =
      (IsMega || TooNestedForIfElse) ? 0
      : (Med.Blocks.size() > limits::kMaxIfElseStructuringBlocks)
          ? limits::kIfElseLargeCfgPasses
          : limits::kIfElseStructuringPasses;
  auto TIfElse = Now();
  structureIfElse(Func, IfElseMaxPasses, &Med);
  auto TPost = Now();

  removeTrivialGotos(Func.Body, gotoTargets(Func.Body), LabelStarts(Func.Body),
                     /*Late=*/false);
  simplifyNestedGotos(Func.Body);

  mergeConsecutiveCondBlocks(Func.Body);
  inlineGotoReturns(Func, Med);
  cleanupGuardBeforeSwitch(Func);

  Func.Body.erase(std::remove_if(Func.Body.begin(), Func.Body.end(),
                                 [](const HighStmt &S) {
                                   return S.Kind == StmtKind::Nop &&
                                          (!S.Addr || S.Addr == InvalidVA);
                                 }),
                  Func.Body.end());
  auto TEnd = Now();
  if (WantDetail)
    syncWarning() << "m2h-simp: " << Med.Name
                  << " [loops=" << ElapsedMs(TLoops, TSwitch)
                  << "ms switch=" << ElapsedMs(TSwitch, TIfElse)
                  << "ms ifelse=" << ElapsedMs(TIfElse, TPost)
                  << "ms post=" << ElapsedMs(TPost, TEnd) << "ms]\n";
}
} // namespace neverd
