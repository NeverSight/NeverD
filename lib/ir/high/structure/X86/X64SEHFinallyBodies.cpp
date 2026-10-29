//===- X64SEHFinallyBodies.cpp - __finally termination bodies ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../pass/HighFrameAddress.h"
#include "../SEHFinallyBodiesDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighSourceFlow.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"

#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {
namespace seh_finally {
namespace {

/// The incoming parameter a function homes at entry-SP displacement \p Offset,
/// as a Win64 prologue spills `[rsp + k] = arg`.  Only the statements before
/// the first structured region are consulted, and a displacement written more
/// than once, or by a non-parameter, yields nothing there.
std::map<int64_t, MedVar> homedParameters(const HighFunc &Func,
                                          Arch TargetArch) {
  std::map<int64_t, MedVar> Homed;
  std::set<int64_t> Ambiguous;
  size_t Budget = limits::kMaxCxxObjectAddressWork;
  for (const HighStmt &S : Func.Body) {
    if (S.Kind == StmtKind::SEHTry || S.Kind == StmtKind::CxxTry ||
        S.Kind == StmtKind::ItaniumTry)
      break;
    if (S.Kind != StmtKind::Store || !S.StoreAddr || !S.StoreVal)
      continue;
    const auto Offset =
        high_detail::frameAddressOffset(S.StoreAddr, Func, TargetArch, Budget);
    if (!Offset)
      continue;
    if (!Ambiguous.insert(*Offset).second) {
      Homed.erase(*Offset);
      continue;
    }
    const ExprPtr &V = S.StoreVal;
    if (V->Kind == ExprKind::Var && V->Var.Kind == MedVar::Param &&
        S.StoreAddr->Type &&
        S.StoreAddr->Type->Size == getTargetRegInfo(TargetArch).PointerSize)
      Homed.emplace(*Offset, V->Var);
  }
  return Homed;
}

struct FuncletShape {
  std::vector<HighStmt> Body;
  bool NoReturnTail = false;
};

/// The funclet's straight-line body, with its trailing `return` dropped (a
/// `__finally` yields no value; a call in it stays), when its shape is
/// supported; nullopt keeps the address comment.  The body must be straight
/// line, with no nested region, no control transfer, no reference to its own
/// stack pointer or a third parameter, and no exception metadata of its own.
std::optional<FuncletShape> funcletShape(const HighFunc &Funclet,
                                         Arch TargetArch) {
  if (Funclet.Body.empty() ||
      (Funclet.ExceptionMetadata && Funclet.ExceptionMetadata->SEH &&
       !Funclet.ExceptionMetadata->SEH->Scopes.empty()) ||
      (Funclet.ExceptionMetadata && Funclet.ExceptionMetadata->Cxx) ||
      (Funclet.ExceptionMetadata && Funclet.ExceptionMetadata->Itanium) ||
      (Funclet.ExceptionMetadata && Funclet.ExceptionMetadata->Registration))
    return std::nullopt;
  // A finally callback takes only (flag, frame), and reads no register of
  // the parent: the dispatcher calls it with its own.
  if (!funcletVariablesMovable(Funclet.Body, TargetArch, /*MaxParam=*/1,
                               [](const MedVar &) { return false; }) ||
      callsLocalUnwind(Funclet.Body))
    return std::nullopt;
  bool Bad = false;
  bool NoReturnTail = false;
  for (size_t I = 0; I < Funclet.Body.size(); ++I) {
    const HighStmt &S = Funclet.Body[I];
    const bool Last = I + 1 == Funclet.Body.size();
    switch (S.Kind) {
    case StmtKind::Assign:
    case StmtKind::Store:
    case StmtKind::ExprStmt:
    case StmtKind::Call:
    case StmtKind::Nop:
    case StmtKind::Block:
      break;
    case StmtKind::Return:
      if (!Last)
        Bad = true;
      break;
    default:
      Bad = true;
      break;
    }
    if (!S.Body.empty() || !S.ElseBody.empty() || !S.Cases.empty() ||
        !S.DefaultBody.empty() || !S.EHClauseBodies.empty())
      Bad = true;
    if (Last && S.Kind == StmtKind::Call && isTerminatingHighCall(S.CallExpr))
      NoReturnTail = true;
  }
  if (Bad)
    return std::nullopt;
  FuncletShape Shape;
  Shape.Body = cloneStmts(Funclet.Body);
  Shape.NoReturnTail = NoReturnTail;
  if (!Shape.Body.empty() && Shape.Body.back().Kind == StmtKind::Return) {
    std::optional<HighStmt> Work = returnedWork(Shape.Body.back());
    Shape.Body.pop_back();
    if (Work)
      Shape.Body.push_back(std::move(*Work));
  }
  return Shape;
}

/// The funclet body projected into the parent frame: the establisher-frame
/// parameter rebased onto the parent's entry stack pointer, each home-slot load
/// turned back into the parameter it holds, and the abnormal-termination
/// parameter replaced by \p AbnormalFlag (a call to `AbnormalTermination()`, or
/// a folded constant for the dedup check), with the funclet's names moved
/// past \p Names.  Nullopt when they cannot move.
std::optional<std::vector<HighStmt>>
projectFuncletBody(const std::vector<HighStmt> &FuncletBody,
                   const HighFunc &Parent, Arch TargetArch, uint64_t FrameBytes,
                   const ExprPtr &AbnormalFlag,
                   const std::map<int64_t, MedVar> &Homed,
                   const MedVar &EntrySP, FuncletNames &Names) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  std::vector<HighStmt> Body = cloneStmts(FuncletBody);
  // Rename first: the parent values substituted below keep their names.
  if (!renameFuncletLocals(Body, Names))
    return std::nullopt;

  // arg0 -> abnormal flag, arg1 -> entry SP - FrameBytes.
  mapExprs(Body, [&](ExprPtr E) -> ExprPtr {
    if (!E || E->Kind != ExprKind::Var)
      return E;
    if (E->Var.Kind == MedVar::Param && E->Var.Id == 0 && AbnormalFlag) {
      ExprPtr Flag = cloneExpr(AbnormalFlag);
      if (E->Type)
        Flag->Type = E->Type;
      return Flag;
    }
    if (E->Var.Kind == MedVar::Param && E->Var.Id == 1) {
      ExprPtr Base = HighExpr::makeVar(EntrySP, NdType::makeInt(8, false));
      ExprPtr Rebased = HighExpr::makeBinop(
          NdOp::INT_SUB, Base,
          HighExpr::makeConst(FrameBytes, 8,
                              ConstantAddressProvenance::Scalar));
      Rebased->Type = E->Type ? E->Type : NdType::makeInt(8, false);
      return Rebased;
    }
    return E;
  });

  // A pointer-width load of a homed slot is the homed parameter.
  if (!Homed.empty()) {
    size_t Budget = limits::kMaxCxxObjectAddressWork;
    mapExprs(Body, [&](ExprPtr E) -> ExprPtr {
      if (!E || E->Kind != ExprKind::Load || E->Operands.size() != 1 ||
          !E->Type || E->Type->Size != TRI.PointerSize ||
          E->MemoryOrdering != NdMemoryOrdering::None ||
          E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return E;
      const auto Offset = high_detail::frameAddressOffset(
          E->Operands[0], Parent, TargetArch, Budget);
      if (!Offset)
        return E;
      auto It = Homed.find(*Offset);
      if (It == Homed.end())
        return E;
      return HighExpr::makeVar(It->second, E->Type);
    });
  }

  simplifyProjectedBody(Body);
  return Body;
}

/// Whether any expression of \p Stmts reads the abnormal-termination
/// parameter (the funclet's first argument).
bool referencesAbnormalFlag(const std::vector<HighStmt> &Stmts) {
  bool Found = false;
  walkStmts(Stmts, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
        if (!E)
          return;
        if (E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Param &&
            E->Var.Id == 0)
          Found = true;
        E->forEachChildExpr(Visit);
      };
      Visit(Root);
    });
  });
  return Found;
}

bool stmtEqual(const HighStmt &A, const HighStmt &B) {
  // `*p = v` reaches HighIR as a Store or as an assignment to a load; compare
  // the address and value either way.
  auto deref = [](const HighStmt &S, ExprPtr &Addr, ExprPtr &Val) {
    if (S.Kind == StmtKind::Store) {
      Addr = S.StoreAddr;
      Val = S.StoreVal;
      return true;
    }
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Load &&
        !S.Dst->Operands.empty()) {
      Addr = S.Dst->Operands[0];
      Val = S.Val;
      return true;
    }
    return false;
  };
  auto eq = [](const ExprPtr &X, const ExprPtr &Y) {
    if (!X || !Y)
      return X == Y;
    return X->structuralEq(*Y);
  };
  ExprPtr AAddr, AVal, BAddr, BVal;
  const bool ADeref = deref(A, AAddr, AVal);
  const bool BDeref = deref(B, BAddr, BVal);
  if (ADeref && BDeref)
    return eq(AAddr, BAddr) && eq(AVal, BVal);
  if (ADeref || BDeref)
    return false;
  return A.Kind == B.Kind && eq(A.Dst, B.Dst) && eq(A.Val, B.Val) &&
         eq(A.CallExpr, B.CallExpr);
}

/// The dereferenced address of a `*p = v` statement, either spelling, else
/// null.
const HighExpr *storeAddress(const HighStmt &S) {
  if (S.Kind == StmtKind::Store)
    return S.StoreAddr.get();
  if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Load &&
      !S.Dst->Operands.empty())
    return S.Dst->Operands[0].get();
  return nullptr;
}

/// Where the parent's statements after \p TryIndex begin the inline normal-path
/// copy of the finally: the leading run equal to the funclet body with the
/// abnormal flag folded false.  Returns the count to remove, or nullopt when no
/// such run is proved.  A deleted statement must not be a branch target, and
/// the run must not be an ambiguous prefix of a longer repetition, so removing
/// it cannot drop the parent's real continuation or a label a goto needs.
std::optional<size_t> inlineCopyLength(const std::vector<HighStmt> &Body,
                                       size_t TryIndex,
                                       const std::vector<HighStmt> &NormalBody,
                                       const std::set<va_t> &Targets) {
  if (NormalBody.empty() || TryIndex + 1 + NormalBody.size() > Body.size())
    return std::nullopt;
  for (size_t I = 0; I < NormalBody.size(); ++I) {
    const HighStmt &S = Body[TryIndex + 1 + I];
    if (!stmtEqual(S, NormalBody[I]))
      return std::nullopt;
    if (S.Addr && S.Addr != InvalidVA && Targets.count(S.Addr))
      return std::nullopt;
  }
  // If the statement right after the run repeats the last store's address, the
  // run could be a coincidental prefix rather than exactly the copy.
  const size_t After = TryIndex + 1 + NormalBody.size();
  if (const HighExpr *Last = storeAddress(Body[After - 1]);
      Last && After < Body.size())
    if (const HighExpr *Next = storeAddress(Body[After]);
        Next && Next->structuralEq(*Last))
      return std::nullopt;
  return NormalBody.size();
}

} // namespace

void attachX64(std::vector<HighFunc> &Funcs, std::set<va_t> *Merged) {
  const Arch TargetArch = Arch::X64;
  // Only a parent with a certified termination frame can gain a body. Index
  // just the funclets those parents name, so a unit without one pays nothing.
  std::map<va_t, const HighFunc *> ByEntry;
  for (const HighFunc &Func : Funcs)
    for (const auto &Entry : Func.SEHTerminationFrameBytes)
      ByEntry.emplace(Entry.first, nullptr);
  if (ByEntry.empty())
    return;
  for (const HighFunc &Func : Funcs)
    if (auto It = ByEntry.find(Func.Entry); It != ByEntry.end() && !It->second)
      It->second = &Func;

  for (HighFunc &Parent : Funcs) {
    if (!Parent.ExceptionMetadata || !Parent.ExceptionMetadata->SEH ||
        Parent.SEHTerminationFrameBytes.empty() || Parent.FrameSize <= 0)
      continue;
    // Whole-body facts, computed once a region of this parent qualifies.
    std::optional<std::map<int64_t, MedVar>> Homed;
    std::optional<std::set<va_t>> Targets;
    FuncletNames Names;
    MedVar EntrySP;
    UnwindExits Exits(Parent);

    // Top-level only: a termination region the structurer kept is a direct
    // statement of the body, and the inline normal-path copy follows it there.
    for (size_t I = 0; I < Parent.Body.size(); ++I) {
      HighStmt &Stmt = Parent.Body[I];
      if (!isEmptyFinallyRegion(Stmt))
        continue;
      const va_t FuncletVA = Stmt.EHClauses.front().FilterOrActionVA;
      auto Frame = Parent.SEHTerminationFrameBytes.find(FuncletVA);
      auto Funclet = ByEntry.find(FuncletVA);
      if (Frame == Parent.SEHTerminationFrameBytes.end() ||
          Funclet == ByEntry.end() || !Funclet->second ||
          Funclet->second == &Parent)
        continue;
      const std::optional<FuncletShape> Shape =
          funcletShape(*Funclet->second, TargetArch);
      if (!Shape)
        continue;
      // The protected body's normal exits fall through to what follows it,
      // the inline normal-path copy; only a local unwind leaves it as a jump,
      // which now runs the handler as the clause.
      const std::set<va_t> Continuation = fallthroughAddresses(Parent.Body, I);
      dropFallthroughExits(Stmt, Continuation, gotoTargets(Parent.Body), Exits);
      restoreFallingUnwindJumps(Stmt, Continuation, Exits);
      Exits.rebuild();
      Targets.reset();
      if (!protectedBodyQualifies(Stmt, Exits) ||
          !Exits.canRunInClause(Stmt, FuncletVA))
        continue;
      if (!Homed) {
        Homed = homedParameters(Parent, TargetArch);
        Names = firstFreeNames(Parent.Body);
        EntrySP = entryStackPointer(Parent, TargetArch, Names);
      }

      // When the body does not depend on the abnormal flag, it runs the same
      // on both exits: prove the statements after the `__try` are the inline
      // normal-path copy and, if so, drop them and run the body itself in the
      // clause (`__finally { *Counter += 1; }`).  Otherwise, keep that copy
      // and run only the abnormal work while unwinding, so the finally never
      // runs twice (`__finally { if (AbnormalTermination()) *Counter = 99; }`).
      const bool DependsOnFlag = referencesAbnormalFlag(Shape->Body);
      std::optional<size_t> CopyLength;
      std::optional<std::vector<HighStmt>> Normal;
      FuncletNames NormalNames = Names;
      if (!DependsOnFlag && !Shape->NoReturnTail) {
        Normal = projectFuncletBody(
            Shape->Body, Parent, TargetArch, Frame->second,
            HighExpr::makeConst(0, 4, ConstantAddressProvenance::Scalar),
            *Homed, EntrySP, NormalNames);
        if (!Targets)
          Targets = gotoTargets(Parent.Body);
        if (Normal)
          CopyLength = inlineCopyLength(Parent.Body, I, *Normal, *Targets);
      }

      // A later attach takes fresh names past the ones this one takes.
      if (CopyLength) {
        Exits.runInClause(Stmt, FuncletVA);
        Stmt.EHClauseBodies.front() = std::move(*Normal);
        Parent.Body.erase(Parent.Body.begin() + I + 1,
                          Parent.Body.begin() + I + 1 + *CopyLength);
        Names = NormalNames;
      } else {
        FuncletNames AbnormalNames = Names;
        std::optional<std::vector<HighStmt>> Abnormal = projectFuncletBody(
            Shape->Body, Parent, TargetArch, Frame->second,
            HighExpr::makeConst(1, 4, ConstantAddressProvenance::Scalar),
            *Homed, EntrySP, AbnormalNames);
        if (!Abnormal)
          continue;
        Exits.runInClause(Stmt, FuncletVA);
        Stmt.EHClauseBodies.front().push_back(
            guardAbnormalWork(std::move(*Abnormal)));
        Names = AbnormalNames;
      }
      Exits.rebuild();
      if (Merged)
        Merged->insert(FuncletVA);
    }
  }
}

} // namespace seh_finally
} // namespace neverd
