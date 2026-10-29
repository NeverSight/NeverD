//===- SEHFinallyBodies.cpp - Windows __finally termination bodies -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What projecting a termination handler into its parent's `__finally` needs
/// on every architecture, and the dispatch to each architecture's projection
/// of the handler's view of the parent.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/SEHFinallyBodies.h"

#include "SEHFinallyBodiesDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"

#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace neverd {
namespace seh_finally {

/// A deep copy of \p E with fresh nodes, so mutations do not reach the
/// original funclet body (which other parents may also attach).
ExprPtr cloneExpr(const ExprPtr &E) {
  if (!E)
    return nullptr;
  auto Copy = std::make_shared<HighExpr>(*E);
  for (ExprPtr &Operand : Copy->Operands)
    Operand = cloneExpr(Operand);
  Copy->IndirectTarget = cloneExpr(Copy->IndirectTarget);
  return Copy;
}

HighStmt cloneStmt(const HighStmt &S) {
  HighStmt Copy = S;
  Copy.Dst = cloneExpr(S.Dst);
  Copy.Val = cloneExpr(S.Val);
  Copy.Cond = cloneExpr(S.Cond);
  Copy.RetVal = cloneExpr(S.RetVal);
  Copy.StoreAddr = cloneExpr(S.StoreAddr);
  Copy.StoreVal = cloneExpr(S.StoreVal);
  Copy.CallExpr = cloneExpr(S.CallExpr);
  Copy.SwitchExpr = cloneExpr(S.SwitchExpr);
  Copy.Body = cloneStmts(S.Body);
  Copy.ElseBody = cloneStmts(S.ElseBody);
  Copy.DefaultBody = cloneStmts(S.DefaultBody);
  for (SwitchCase &C : Copy.Cases)
    C.Body = cloneStmts(C.Body);
  for (HighEHClause &Clause : Copy.EHClauses)
    Clause.FilterValue = cloneExpr(Clause.FilterValue);
  for (auto &ClauseBody : Copy.EHClauseBodies)
    ClauseBody = cloneStmts(ClauseBody);
  return Copy;
}

std::vector<HighStmt> cloneStmts(const std::vector<HighStmt> &Stmts) {
  std::vector<HighStmt> Result;
  Result.reserve(Stmts.size());
  for (const HighStmt &S : Stmts)
    Result.push_back(cloneStmt(S));
  return Result;
}

/// Rewrite every expression of \p Stmts through \p Fn, children first, so a
/// substitution of a leaf is itself rewritten by its new parent's visit.
void mapExprs(std::vector<HighStmt> &Stmts,
              const std::function<ExprPtr(ExprPtr)> &Fn) {
  std::function<ExprPtr(ExprPtr)> Visit = [&](ExprPtr E) -> ExprPtr {
    if (!E)
      return nullptr;
    for (ExprPtr &Operand : E->Operands)
      Operand = Visit(Operand);
    E->IndirectTarget = Visit(E->IndirectTarget);
    return Fn(std::move(E));
  };
  walkStmts(Stmts, [&](HighStmt &S) {
    forEachExpr(S, [&](ExprPtr &E) { E = Visit(E); });
    for (HighEHClause &Clause : S.EHClauses)
      Clause.FilterValue = Visit(Clause.FilterValue);
  });
}

namespace {

/// Whether a constant may take part in a fold. An address is not a number:
/// folded, it would print as one.
bool scalarConstant(const HighExpr &E) {
  return E.ConstProvenance == ConstantAddressProvenance::Unknown ||
         E.ConstProvenance == ConstantAddressProvenance::Scalar;
}

uint64_t signExtend(uint64_t V, unsigned Bytes) {
  if (!Bytes || Bytes >= 8)
    return V;
  const uint64_t Low = (uint64_t(1) << (Bytes * 8)) - 1;
  V &= Low;
  if (V & (uint64_t(1) << (Bytes * 8 - 1)))
    V |= ~Low;
  return V;
}

/// Whether \p E computes a value rather than only restating one constant at
/// another width.
bool computes(const HighExpr &E) {
  if (E.Kind == ExprKind::BinOp)
    return true;
  if (E.Kind == ExprKind::UnaryOp && E.Op != NdOp::INT_ZEXT &&
      E.Op != NdOp::INT_SEXT && E.Op != NdOp::CAST)
    return true;
  return llvm::any_of(E.Operands,
                      [](const ExprPtr &Op) { return Op && computes(*Op); });
}

/// Whether every constant under \p E is explicitly a number.
bool explicitlyScalar(const HighExpr &E) {
  if (E.Kind == ExprKind::Const)
    return E.ConstProvenance == ConstantAddressProvenance::Scalar;
  return llvm::all_of(E.Operands, [](const ExprPtr &Op) {
    return Op && explicitlyScalar(*Op);
  });
}

/// \p E replaced by the constant it evaluates to, when it computes one.
ExprPtr foldExpr(ExprPtr E) {
  if (!E || E->Kind == ExprKind::Const || !E->Type ||
      E->Type->Kind != NdTypeKind::Int)
    return E;
  const auto V = foldToConstant(*E);
  // A width change of a constant whose provenance is not stated stays: the
  // writer may still print those bits as the address they name.
  if (!V || (!computes(*E) && !explicitlyScalar(*E)))
    return E;
  auto Const =
      HighExpr::makeConst(*V, E->Type->Size, ConstantAddressProvenance::Scalar);
  Const->Type = E->Type;
  return Const;
}

/// Rewrite \p Root, children first, through \p Fn.
void mapExpr(ExprPtr &Root, const std::function<ExprPtr(ExprPtr)> &Fn) {
  std::function<ExprPtr(ExprPtr)> Visit = [&](ExprPtr E) -> ExprPtr {
    if (!E)
      return nullptr;
    for (ExprPtr &Operand : E->Operands)
      Operand = Visit(Operand);
    E->IndirectTarget = Visit(E->IndirectTarget);
    return Fn(std::move(E));
  };
  Root = Visit(Root);
}

// HighIR's identity of a value, shared by every kind but parameters.
VarKey keyOf(const MedVar &V) { return varKey(V); }

/// Each variable \p Stmts define, with its number of definitions.
std::map<VarKey, unsigned>
countDefinitions(const std::vector<HighStmt> &Stmts) {
  std::map<VarKey, unsigned> Defs;
  walkStmts(Stmts, [&](const HighStmt &S) {
    if (S.Dst && S.Dst->Kind == ExprKind::Var)
      ++Defs[keyOf(S.Dst->Var)];
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
        if (!E)
          return;
        for (const MedVar &Out : E->IntrinsicOutputs)
          ++Defs[keyOf(Out)];
        E->forEachChildExpr(Visit);
      };
      Visit(Root);
    });
  });
  return Defs;
}

/// A forward pass over a projected handler body: each use of a copy reads
/// the copied value, each expression is folded, and an `if` whose condition
/// folds keeps only the branch it takes.  A copy is dropped only where it
/// runs unconditionally and is its variable's one definition, and only when
/// it copies a constant or a value that is not defined again.
class ProjectedBodySimplifier {
public:
  explicit ProjectedBodySimplifier(const std::vector<HighStmt> &Body)
      : Defs(countDefinitions(Body)) {}

  void run(std::vector<HighStmt> &In, std::map<VarKey, ExprPtr> &Copies,
           bool Unconditional, std::vector<HighStmt> &Out) {
    for (HighStmt &S : In) {
      rewriteUses(S, Copies);
      if (S.Kind == StmtKind::Block) {
        run(S.Body, Copies, Unconditional, Out);
        continue;
      }
      if ((S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse) && S.Cond) {
        if (const auto Taken = foldToConstant(*S.Cond)) {
          run(*Taken ? S.Body : S.ElseBody, Copies, Unconditional, Out);
          continue;
        }
        std::vector<HighStmt> Then, Else;
        std::map<VarKey, ExprPtr> Inner = Copies;
        run(S.Body, Inner, /*Unconditional=*/false, Then);
        Inner = Copies;
        run(S.ElseBody, Inner, /*Unconditional=*/false, Else);
        S.Body = std::move(Then);
        S.ElseBody = std::move(Else);
        if (S.Kind == StmtKind::IfElse && S.ElseBody.empty())
          S.Kind = StmtKind::If;
        Out.push_back(std::move(S));
        continue;
      }
      if (Unconditional && S.Kind == StmtKind::Assign && S.Dst &&
          S.Dst->Kind == ExprKind::Var && copyable(S.Val) &&
          Defs[keyOf(S.Dst->Var)] == 1) {
        Copies[keyOf(S.Dst->Var)] = S.Val;
        continue;
      }
      Out.push_back(std::move(S));
    }
  }

private:
  std::map<VarKey, unsigned> Defs;

  bool copyable(const ExprPtr &V) {
    if (!V || V->IndirectTarget)
      return false;
    if (V->Kind == ExprKind::Const)
      return true;
    return V->Kind == ExprKind::Var && Defs[keyOf(V->Var)] <= 1;
  }

  void rewriteUses(HighStmt &S, const std::map<VarKey, ExprPtr> &Copies) {
    forEachExpr(S, [&](ExprPtr &Root) {
      // The variable an assignment defines is not a use.
      if (&Root == &S.Dst && Root && Root->Kind == ExprKind::Var)
        return;
      mapExpr(Root, [&](ExprPtr E) -> ExprPtr {
        if (E && E->Kind == ExprKind::Var)
          if (auto It = Copies.find(keyOf(E->Var)); It != Copies.end())
            return cloneExpr(It->second);
        return foldExpr(std::move(E));
      });
    });
  }
};

} // namespace

/// A pure integer value re-evaluated to one constant, when every leaf is a
/// numeric constant.  Covers the two's-complement idiom a compiler emits for
/// `AbnormalTermination()` (`(0 - (int)(flag != 0)) + k`), the byte a handler
/// stores its flag as, and the tests a handler makes of the flag.
std::optional<uint64_t> foldToConstant(const HighExpr &E, unsigned Depth) {
  if (Depth > limits::kMaxCExprPrintDepth)
    return std::nullopt;
  const uint64_t Mask = E.Type && E.Type->Size && E.Type->Size < 8
                            ? (uint64_t(1) << (E.Type->Size * 8)) - 1
                            : ~uint64_t(0);
  auto Trunc = [&](uint64_t V) { return V & Mask; };
  if (E.Kind == ExprKind::Const) {
    if (!scalarConstant(E))
      return std::nullopt;
    return Trunc(E.ConstVal);
  }
  auto Operand = [&](size_t I) -> std::optional<uint64_t> {
    return I < E.Operands.size() && E.Operands[I]
               ? foldToConstant(*E.Operands[I], Depth + 1)
               : std::nullopt;
  };
  auto OperandBytes = [&](size_t I) -> unsigned {
    return I < E.Operands.size() && E.Operands[I] && E.Operands[I]->Type
               ? E.Operands[I]->Type->Size
               : 0;
  };
  if (E.Kind == ExprKind::Cast || E.Kind == ExprKind::BitCast) {
    if (auto V = Operand(0))
      return Trunc(*V);
    return std::nullopt;
  }
  if (E.Kind == ExprKind::UnaryOp) {
    const auto V = Operand(0);
    if (!V)
      return std::nullopt;
    switch (E.Op) {
    case NdOp::INT_ZEXT:
    case NdOp::CAST:
      return Trunc(*V);
    case NdOp::INT_SEXT: {
      const unsigned Bytes = OperandBytes(0);
      if (!Bytes)
        return std::nullopt;
      return Trunc(signExtend(*V, Bytes));
    }
    case NdOp::INT_NEG2:
      return Trunc(0 - *V);
    // Both spellings of the complement; INT_NEG2 is the negation.
    case NdOp::INT_NEGATE:
    case NdOp::INT_NOT:
      return Trunc(~*V);
    case NdOp::BOOL_NOT:
      return *V == 0 ? 1 : 0;
    default:
      return std::nullopt;
    }
  }
  if (E.Kind != ExprKind::BinOp || E.Operands.size() != 2)
    return std::nullopt;
  const auto A = Operand(0);
  const auto B = Operand(1);
  if (!A || !B)
    return std::nullopt;
  auto Signed = [&](uint64_t V) -> std::optional<int64_t> {
    const unsigned Bytes = OperandBytes(0);
    if (!Bytes || Bytes != OperandBytes(1))
      return std::nullopt;
    return static_cast<int64_t>(signExtend(V, Bytes));
  };
  switch (E.Op) {
  case NdOp::INT_ADD:
    return Trunc(*A + *B);
  case NdOp::INT_SUB:
    return Trunc(*A - *B);
  case NdOp::INT_MULT:
    return Trunc(*A * *B);
  case NdOp::INT_AND:
    return Trunc(*A & *B);
  case NdOp::INT_OR:
    return Trunc(*A | *B);
  case NdOp::INT_XOR:
    return Trunc(*A ^ *B);
  case NdOp::INT_LEFT:
    return *B >= 64 ? 0 : Trunc(*A << *B);
  case NdOp::INT_RIGHT:
    return *B >= 64 ? 0 : Trunc(*A >> *B);
  case NdOp::SUBBYTES:
    // The value's bytes from the byte offset on, at the result's width.
    return *B >= 8 ? 0 : Trunc(*A >> (*B * 8));
  case NdOp::INT_EQUAL:
    return *A == *B ? 1 : 0;
  case NdOp::INT_NOTEQUAL:
    return *A != *B ? 1 : 0;
  case NdOp::INT_LESS:
    return *A < *B ? 1 : 0;
  case NdOp::INT_LESSEQUAL:
    return *A <= *B ? 1 : 0;
  case NdOp::INT_SLESS:
  case NdOp::INT_SLESSEQUAL: {
    const auto SA = Signed(*A);
    const auto SB = Signed(*B);
    if (!SA || !SB)
      return std::nullopt;
    return (E.Op == NdOp::INT_SLESS ? *SA < *SB : *SA <= *SB) ? 1 : 0;
  }
  case NdOp::BOOL_AND:
    return *A != 0 && *B != 0 ? 1 : 0;
  case NdOp::BOOL_OR:
    return *A != 0 || *B != 0 ? 1 : 0;
  case NdOp::BOOL_XOR:
    return (*A != 0) != (*B != 0) ? 1 : 0;
  default:
    return std::nullopt;
  }
}

void simplifyProjectedBody(std::vector<HighStmt> &Body) {
  // The second pass counts definitions without the branches the first one
  // dropped, so a value an `if` chose on both arms is one copy by then.
  for (int Pass = 0; Pass < 2; ++Pass) {
    ProjectedBodySimplifier Simplifier(Body);
    std::map<VarKey, ExprPtr> Copies;
    std::vector<HighStmt> Out;
    Simplifier.run(Body, Copies, /*Unconditional=*/true, Out);
    Body = std::move(Out);
  }
}

std::set<VarKey> definedVariables(const std::vector<HighStmt> &Body) {
  std::set<VarKey> Defined;
  for (const auto &[Key, Count] : countDefinitions(Body))
    Defined.insert(Key);
  return Defined;
}

bool isEntryValue(const MedVar &V, const std::set<VarKey> &Defined) {
  return (V.Kind == MedVar::Reg || V.Kind == MedVar::Flag) && V.SSAVer == 0 &&
         !Defined.count(varKey(V));
}

bool funcletVariablesMovable(
    const std::vector<HighStmt> &Body, Arch TargetArch, int MaxParam,
    const std::function<bool(const MedVar &)> &EntryRegisterKnown) {
  const uint64_t SP = getTargetRegInfo(TargetArch).StackPointer;
  // Version 0 of a register the handler writes before reading is that
  // write, one of its own values; only an undefined one is read on entry.
  const std::set<VarKey> Defined = definedVariables(Body);
  auto Movable = [&](const MedVar &V) {
    switch (V.Kind) {
    case MedVar::Const:
    case MedVar::Temp:
      return true;
    case MedVar::Param:
      return V.Id >= 0 && V.Id <= MaxParam;
    case MedVar::Reg:
      // Its own stack pointer names its own frame, not the parent's; a
      // register it reads on entry is the dispatcher's unless the parent's
      // value is known.
      if (V.RegOff == SP)
        return false;
      return !isEntryValue(V, Defined) || EntryRegisterKnown(V);
    case MedVar::Flag:
      return !isEntryValue(V, Defined);
    default:
      // Its own frame slots, return value and exception state are its own.
      return false;
    }
  };
  bool OK = true;
  walkStmts(Body, [&](const HighStmt &S) {
    // A definition of a parameter would make the substitution wrong.
    if (S.Dst && S.Dst->Kind == ExprKind::Var &&
        S.Dst->Var.Kind == MedVar::Param)
      OK = false;
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
        if (!E || !OK)
          return;
        if (E->Kind == ExprKind::EntryRegister ||
            (E->Kind == ExprKind::Var && !Movable(E->Var)))
          OK = false;
        for (const MedVar &Out : E->IntrinsicOutputs)
          if (Out.Kind == MedVar::Param || !Movable(Out))
            OK = false;
        E->forEachChildExpr(Visit);
      };
      Visit(Root);
    });
    for (const HighEHClause &Clause : S.EHClauses)
      if (Clause.FilterValue)
        OK = false;
  });
  return OK;
}

namespace {

/// The variable numbers \p Stmts use.
std::set<int> usedIds(const std::vector<HighStmt> &Stmts) {
  std::set<int> Used;
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (E->Kind == ExprKind::Var && E->Var.Id >= 0)
      Used.insert(E->Var.Id);
    for (const MedVar &Out : E->IntrinsicOutputs)
      if (Out.Id >= 0)
        Used.insert(Out.Id);
    E->forEachChildExpr(Visit);
  };
  walkStmts(Stmts, [&](const HighStmt &S) {
    forEachExpr(S, Visit);
    for (const HighEHClause &Clause : S.EHClauses)
      Visit(Clause.FilterValue);
  });
  return Used;
}

/// The smallest variable number neither the parent, an attached body, nor
/// \p Avoid uses; nullopt past the int range.
std::optional<int> takeFreeId(FuncletNames &Names, const std::set<int> &Avoid) {
  auto Used = [&](int Id) {
    return Names.ParentIds->count(Id) || Names.Taken.count(Id);
  };
  // Every number below the cursor is used for good.
  while (Used(Names.Cursor)) {
    if (Names.Cursor == std::numeric_limits<int>::max())
      return std::nullopt;
    ++Names.Cursor;
  }
  int Id = Names.Cursor;
  while (Used(Id) || Avoid.count(Id)) {
    if (Id == std::numeric_limits<int>::max())
      return std::nullopt;
    ++Id;
  }
  Names.Taken.insert(Id);
  return Id;
}

} // namespace

bool renameFuncletLocals(std::vector<HighStmt> &Body, FuncletNames &Names) {
  // A name the handler's out-of-SSA pass gave keeps its sharing, past the
  // parent's names. An unnamed local keeps its kind, and so the way the
  // writer prints and inlines it, but takes a number of its own: its
  // spelling comes from the handler's numbering, which the parent's reuses.
  int MaxTag = -1;
  std::map<int, int> Fresh;
  const std::set<VarKey> Defined = definedVariables(Body);
  auto Unnamed = [&](const MedVar &V) {
    return V.RenameTag < 0 &&
           (V.Kind == MedVar::Temp || V.Kind == MedVar::Flag ||
            V.Kind == MedVar::Reg) &&
           !isEntryValue(V, Defined);
  };
  // Kinds share HighIR's numbering, and versions share a number's name.
  auto Local = [](const MedVar &V) { return V.Id; };
  // Each node once, even one the body shares.
  auto ForEachVar = [&](std::vector<HighStmt> &Stmts, auto &&Fn) {
    std::set<const HighExpr *> Seen;
    walkStmts(Stmts, [&](HighStmt &S) {
      forEachExpr(S, [&](ExprPtr &Root) {
        std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
          if (!E || !Seen.insert(E.get()).second)
            return;
          if (E->Kind == ExprKind::Var)
            Fn(E->Var);
          for (MedVar &Out : E->IntrinsicOutputs)
            Fn(Out);
          E->forEachChildExpr(Visit);
        };
        Visit(Root);
      });
    });
  };
  // A fresh number is none the handler uses either, so none is a number an
  // entry value it keeps still has.
  std::set<int> HandlerIds;
  ForEachVar(Body, [&](MedVar &V) {
    if (V.Id >= 0)
      HandlerIds.insert(V.Id);
    if (V.RenameTag >= 0)
      MaxTag = std::max<int>(MaxTag, V.RenameTag);
    else if (Unnamed(V))
      Fresh.emplace(Local(V), 0);
  });
  if (Names.RenameTag < 0 ||
      Names.RenameTag + MaxTag > std::numeric_limits<int16_t>::max())
    return false;
  FuncletNames Moved = Names;
  for (auto &[Key, Id] : Fresh) {
    const std::optional<int> Free = takeFreeId(Moved, HandlerIds);
    if (!Free)
      return false;
    Id = *Free;
  }
  ForEachVar(Body, [&](MedVar &V) {
    if (V.RenameTag >= 0)
      V.RenameTag = static_cast<int16_t>(Names.RenameTag + V.RenameTag);
    else if (Unnamed(V))
      V.Id = Fresh.at(Local(V));
  });
  Moved.RenameTag += MaxTag + 1;
  Names = std::move(Moved);
  return true;
}

MedVar entryStackPointer(const HighFunc &Parent, Arch TargetArch,
                         FuncletNames &Names) {
  std::optional<MedVar> Found;
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
    if (!E || Found)
      return;
    if (E->Kind == ExprKind::Var &&
        isSyntheticEntryStackPointer(E->Var, Parent, TargetArch) &&
        E->Var.Size == getTargetRegInfo(TargetArch).PointerSize) {
      Found = E->Var;
      return;
    }
    E->forEachChildExpr(Visit);
  };
  walkStmts(Parent.Body, [&](const HighStmt &S) { forEachExpr(S, Visit); });
  if (Found)
    return *Found;
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.TheArch = TargetArch;
  SP.RenameTag = -1;
  SP.SSAVer = 0;
  SP.Id = takeFreeId(Names, {}).value_or(MedVar::UnspecifiedId);
  SP.RegOff = TRI.StackPointer;
  SP.Size = TRI.PointerSize;
  return SP;
}

FuncletNames firstFreeNames(const std::vector<HighStmt> &Stmts) {
  FuncletNames Names;
  Names.RenameTag = maxRenameTag(Stmts) + 1;
  Names.ParentIds = std::make_shared<const std::set<int>>(usedIds(Stmts));
  return Names;
}

ExprPtr makeAbnormalTermination() {
  // `AbnormalTermination()` is a compiler intrinsic declared by <excpt.h>,
  // like `GetExceptionCode()`; it takes no arguments and needs no declaration.
  ExprPtr Call = HighExpr::makeCall("AbnormalTermination", 0, {});
  Call->Type = NdType::makeInt(4);
  return Call;
}

int maxRenameTag(const std::vector<HighStmt> &Stmts) {
  int Max = -1;
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
    if (!E)
      return;
    Max = std::max<int>(Max, E->Var.RenameTag);
    for (const MedVar &Out : E->IntrinsicOutputs)
      Max = std::max<int>(Max, Out.RenameTag);
    E->forEachChildExpr(Visit);
  };
  walkStmts(Stmts, [&](const HighStmt &S) {
    forEachExpr(S, Visit);
    for (const HighEHClause &Clause : S.EHClauses)
      Visit(Clause.FilterValue);
  });
  return Max;
}

/// Whether \p Stmts call a routine that unwinds this frame to a target in the
/// function (`_local_unwind`, ARM's `__jump_unwind`, `RtlUnwind[Ex]`).  The
/// machine then runs the termination handler with the abnormal flag set, but C
/// reads the call as an ordinary one followed by a normal exit, so a filled
/// `__finally` would not run there.  LowToMed replaces each call it proves
/// unwinds this frame by the handler calls and a jump
/// (MedSEHLocalUnwindExit); a region holding one it did not keeps its
/// out-of-line form.  A handler body that makes such a call leaves its
/// `__finally` by a jump C would not take either.
bool callsLocalUnwind(const std::vector<HighStmt> &Stmts) {
  bool Found = false;
  auto IsUnwind = [](llvm::StringRef Name) {
    Name = Name.ltrim('_');
    return Name.starts_with("local_unwind") || Name == "jump_unwind" ||
           Name == "RtlUnwind" || Name == "RtlUnwindEx";
  };
  walkStmts(Stmts, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
        if (!E || Found)
          return;
        if (E->Kind == ExprKind::Call && IsUnwind(E->CallTarget))
          Found = true;
        E->forEachChildExpr(Visit);
      };
      Visit(Root);
    });
  });
  return Found;
}

UnwindExits::UnwindExits(HighFunc &Parent) : Parent(Parent) { rebuild(); }

void UnwindExits::rebuild() {
  Copies.clear();
  Unstructured.clear();
  Jumps.clear();
  if (Parent.SEHLocalUnwindExits.empty())
    return;
  collect(Parent.Body);
  for (const Copy &C : Copies)
    Jumps.emplace(&(*C.List)[C.Jump], C.Exit->CallAddr);
  // A handler call no copy holds, such as one whose result the parent
  // reads, leaves its exit unstructured.
  std::map<va_t, size_t> Held;
  for (const Copy &C : Copies)
    Held[C.Exit->CallAddr] += C.Calls.size();
  std::map<va_t, size_t> Made;
  walkStmts(Parent.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
        if (!E)
          return;
        if (E->Kind == ExprKind::Call)
          for (const MedSEHLocalUnwindExit &Exit : Parent.SEHLocalUnwindExits)
            if (Exit.CallAddr == E->CallSiteAddr &&
                llvm::any_of(Exit.Handlers, [&](const auto &H) {
                  return H.Entry == E->CallAddr;
                }))
              ++Made[Exit.CallAddr];
        E->forEachChildExpr(Visit);
      };
      Visit(Root);
    });
  });
  for (const auto &[Site, Count] : Made)
    if (Held[Site] != Count)
      Unstructured.insert(Site);
}

const MedSEHLocalUnwindExit::Handler *
UnwindExits::handlerCall(const HighStmt &S,
                         const MedSEHLocalUnwindExit *&Exit) const {
  const HighExpr *Call = nullptr;
  if (S.Kind == StmtKind::Call)
    Call = S.CallExpr.get();
  else if (S.Kind == StmtKind::ExprStmt && S.Val &&
           S.Val->Kind == ExprKind::Call)
    Call = S.Val.get();
  if (!Call || Call->Kind != ExprKind::Call || Call->IndirectTarget)
    return nullptr;
  for (const MedSEHLocalUnwindExit &Candidate : Parent.SEHLocalUnwindExits) {
    if (Candidate.CallAddr != Call->CallSiteAddr)
      continue;
    for (const MedSEHLocalUnwindExit::Handler &H : Candidate.Handlers)
      if (H.Entry == Call->CallAddr) {
        Exit = &Candidate;
        return &H;
      }
  }
  return nullptr;
}

namespace {

/// Whether \p S only moves values between variables: what a jump's block
/// sets for its target.  Run before or after a handler, it reads and writes
/// nothing the handler does.
bool movesValuesOnly(const HighStmt &S) {
  if (S.Kind == StmtKind::Nop)
    return true;
  if (S.Kind != StmtKind::Assign || !S.Dst || S.Dst->Kind != ExprKind::Var ||
      !S.Val)
    return false;
  bool Pure = true;
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
    if (!E || !Pure)
      return;
    if (E->Kind == ExprKind::Call || E->Kind == ExprKind::Load ||
        E->Kind == ExprKind::Store || E->Kind == ExprKind::Undef ||
        E->MemoryOrdering != NdMemoryOrdering::None ||
        E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      Pure = false;
    E->forEachChildExpr(Visit);
  };
  Visit(S.Val);
  return Pure;
}

} // namespace

void UnwindExits::collect(std::vector<HighStmt> &Stmts) {
  for (size_t I = 0; I < Stmts.size();) {
    const MedSEHLocalUnwindExit *Exit = nullptr;
    if (!handlerCall(Stmts[I], Exit)) {
      ++I;
      continue;
    }
    // The handlers in the exit's order, innermost first, and the values the
    // jump's block moves for its target.
    Copy C;
    C.List = &Stmts;
    C.Exit = Exit;
    const MedSEHLocalUnwindExit::Handler *Last = nullptr;
    size_t J = I;
    for (; J < Stmts.size(); ++J) {
      const MedSEHLocalUnwindExit *Next = nullptr;
      const MedSEHLocalUnwindExit::Handler *H = handlerCall(Stmts[J], Next);
      if (!H && movesValuesOnly(Stmts[J]))
        continue;
      if (!H || Next != Exit || (Last && H <= Last))
        break;
      C.Calls.push_back(J);
      Last = H;
    }
    if (J == Stmts.size() || !((Stmts[J].Kind == StmtKind::Goto &&
                                Stmts[J].GotoTarget == Exit->Target) ||
                               Stmts[J].Kind == StmtKind::Return)) {
      Unstructured.insert(Exit->CallAddr);
      I = J;
      continue;
    }
    C.Jump = J;
    Copies.push_back(std::move(C));
    I = J + 1;
  }
  for (HighStmt &S : Stmts) {
    collect(S.Body);
    collect(S.ElseBody);
    for (SwitchCase &Case : S.Cases)
      collect(Case.Body);
    collect(S.DefaultBody);
    for (auto &ClauseBody : S.EHClauseBodies)
      collect(ClauseBody);
  }
}

const MedSEHLocalUnwindExit *
UnwindExits::fallingCopy(const std::vector<HighStmt> &Stmts, size_t End) const {
  const MedSEHLocalUnwindExit *Found = nullptr;
  for (size_t J = std::min(End, Stmts.size()); J-- > 0;) {
    const MedSEHLocalUnwindExit *Exit = nullptr;
    if (handlerCall(Stmts[J], Exit)) {
      if (Found && Exit != Found)
        break;
      Found = Exit;
      continue;
    }
    if (!movesValuesOnly(Stmts[J]))
      break;
  }
  return Found;
}

bool UnwindExits::endsCopy(const HighStmt &S) const {
  const auto It = Jumps.find(&S);
  return It != Jumps.end() && !Unstructured.count(It->second);
}

namespace {

/// The statement lists of \p Body and of everything nested in it, and the
/// addresses its statements print labels at.
void protectedContents(const std::vector<HighStmt> &Body,
                       std::set<const std::vector<HighStmt> *> &Lists,
                       std::set<va_t> &Labels) {
  Lists.insert(&Body);
  for (const HighStmt &S : Body) {
    if (S.Addr && S.Addr != InvalidVA)
      Labels.insert(S.Addr);
    if (S.Kind == StmtKind::While && S.LoopHeaderAddr)
      Labels.insert(S.LoopHeaderAddr);
    protectedContents(S.Body, Lists, Labels);
    protectedContents(S.ElseBody, Lists, Labels);
    for (const SwitchCase &Case : S.Cases)
      protectedContents(Case.Body, Lists, Labels);
    protectedContents(S.DefaultBody, Lists, Labels);
    for (const auto &ClauseBody : S.EHClauseBodies)
      protectedContents(ClauseBody, Lists, Labels);
  }
}

} // namespace

bool UnwindExits::canRunInClause(const HighStmt &Try, va_t Handler) const {
  std::set<const std::vector<HighStmt> *> Lists;
  std::set<va_t> Labels;
  protectedContents(Try.Body, Lists, Labels);
  // An exit leaves the try when its call is guarded by it and it runs the
  // try's handler.  Every copy of such an exit, and only of such an exit,
  // must call the handler in the try, as its last explicit call: an outer
  // handler still called explicitly would run before the clause does.
  for (const MedSEHLocalUnwindExit &Exit : Parent.SEHLocalUnwindExits) {
    const bool Leaves = Try.EHRange.contains(Exit.CallAddr) &&
                        llvm::any_of(Exit.Handlers, [&](const auto &H) {
                          return H.Entry == Handler;
                        });
    if (Leaves && Unstructured.count(Exit.CallAddr))
      return false;
  }
  for (const Copy &C : Copies) {
    std::optional<size_t> Position;
    for (size_t K = 0; K < C.Calls.size(); ++K) {
      const MedSEHLocalUnwindExit *Exit = nullptr;
      const auto *H = handlerCall((*C.List)[C.Calls[K]], Exit);
      if (H && H->Entry == Handler)
        Position = K;
    }
    if (!Position)
      continue;
    const bool Inside = Lists.count(C.List);
    const bool Leaves = Try.EHRange.contains(C.Exit->CallAddr);
    if (!Inside && !Leaves)
      continue;
    if (!Inside || !Leaves || *Position + 1 != C.Calls.size())
      return false;
    const HighStmt &Jump = (*C.List)[C.Jump];
    if (Jump.Kind == StmtKind::Goto && Labels.count(Jump.GotoTarget))
      return false;
  }
  return true;
}

void UnwindExits::runInClause(const HighStmt &Try, va_t Handler) {
  std::set<const std::vector<HighStmt> *> Lists;
  std::set<va_t> Labels;
  protectedContents(Try.Body, Lists, Labels);
  std::map<std::vector<HighStmt> *, std::set<size_t>> Erase;
  for (const Copy &C : Copies) {
    if (!Lists.count(C.List) || !Try.EHRange.contains(C.Exit->CallAddr))
      continue;
    for (size_t Position : C.Calls) {
      const MedSEHLocalUnwindExit *Exit = nullptr;
      const auto *H = handlerCall((*C.List)[Position], Exit);
      if (!H || H->Entry != Handler)
        continue;
      Erase[C.List].insert(Position);
      // C runs the clause only when the goto leaves the try as a jump.
      (*C.List)[C.Jump].AbnormalExit = true;
    }
  }
  for (auto &[List, Positions] : Erase)
    for (auto It = Positions.rbegin(); It != Positions.rend(); ++It)
      List->erase(List->begin() + static_cast<ptrdiff_t>(*It));
  rebuild();
}

std::set<va_t> fallthroughAddresses(const std::vector<HighStmt> &Stmts,
                                    size_t Index) {
  // Empty anchors print nothing: each of their addresses is the point the
  // first statement that runs starts at.
  std::set<va_t> Addresses;
  for (size_t J = Index + 1; J < Stmts.size(); ++J) {
    const HighStmt &Next = Stmts[J];
    if (Next.Addr && Next.Addr != InvalidVA)
      Addresses.insert(Next.Addr);
    if (Next.Kind == StmtKind::While && Next.LoopHeaderAddr)
      Addresses.insert(Next.LoopHeaderAddr);
    if (Next.Kind != StmtKind::Nop &&
        !(Next.Kind == StmtKind::Block && Next.Body.empty()))
      break;
  }
  return Addresses;
}

void dropFallthroughExits(HighStmt &Try, const std::set<va_t> &Continuation,
                          const std::set<va_t> &Labels,
                          const UnwindExits &Exits) {
  auto Hidden = [](const HighStmt &S) {
    return S.Kind == StmtKind::Nop ||
           (S.Kind == StmtKind::Block && S.Body.empty());
  };
  // Only what runs last can fall through: the end of a branch, of a nested
  // try's body, or of an `__except` arm that resumes after its try.
  std::function<void(std::vector<HighStmt> &)> Tail =
      [&](std::vector<HighStmt> &Stmts) {
        size_t End = Stmts.size();
        while (End && Hidden(Stmts[End - 1]))
          --End;
        if (!End)
          return;
        HighStmt &Last = Stmts[End - 1];
        switch (Last.Kind) {
        case StmtKind::Goto:
          if (Continuation.count(Last.GotoTarget) && !Labels.count(Last.Addr) &&
              !Last.AbnormalExit && !Exits.endsCopy(Last)) {
            Stmts.erase(Stmts.begin() + static_cast<ptrdiff_t>(End - 1));
            Tail(Stmts);
          }
          return;
        case StmtKind::If:
        case StmtKind::IfElse:
          Tail(Last.Body);
          Tail(Last.ElseBody);
          return;
        case StmtKind::Block:
          Tail(Last.Body);
          return;
        case StmtKind::SEHTry:
          if (!Last.EHIsReducible)
            return;
          Tail(Last.Body);
          for (size_t I = 0;
               I < Last.EHClauses.size() && I < Last.EHClauseBodies.size(); ++I)
            if (Last.EHClauses[I].Kind == HighEHClauseKind::SEHExcept)
              Tail(Last.EHClauseBodies[I]);
          return;
        default:
          return;
        }
      };
  Tail(Try.Body);
}

void restoreFallingUnwindJumps(HighStmt &Try,
                               const std::set<va_t> &Continuation,
                               const UnwindExits &Exits) {
  auto Hidden = [](const HighStmt &S) {
    return S.Kind == StmtKind::Nop ||
           (S.Kind == StmtKind::Block && S.Body.empty());
  };
  std::function<void(std::vector<HighStmt> &)> Tail =
      [&](std::vector<HighStmt> &Stmts) {
        size_t End = Stmts.size();
        while (End && Hidden(Stmts[End - 1]))
          --End;
        if (!End)
          return;
        if (const MedSEHLocalUnwindExit *Exit = Exits.fallingCopy(Stmts, End)) {
          // Before any anchor that ends the list: a jump landing there still
          // falls out normally.
          if (Continuation.count(Exit->Target)) {
            HighStmt Jump;
            Jump.Kind = StmtKind::Goto;
            Jump.Addr = Stmts[End - 1].Addr;
            Jump.GotoTarget = Exit->Target;
            Stmts.insert(Stmts.begin() + static_cast<ptrdiff_t>(End),
                         std::move(Jump));
          }
          return;
        }
        HighStmt &Last = Stmts[End - 1];
        switch (Last.Kind) {
        case StmtKind::If:
        case StmtKind::IfElse:
          Tail(Last.Body);
          Tail(Last.ElseBody);
          return;
        case StmtKind::Block:
          Tail(Last.Body);
          return;
        case StmtKind::SEHTry:
          if (!Last.EHIsReducible)
            return;
          Tail(Last.Body);
          for (size_t I = 0;
               I < Last.EHClauses.size() && I < Last.EHClauseBodies.size(); ++I)
            if (Last.EHClauses[I].Kind == HighEHClauseKind::SEHExcept)
              Tail(Last.EHClauseBodies[I]);
          return;
        default:
          return;
        }
      };
  Tail(Try.Body);
}

bool protectedBodyQualifies(const HighStmt &Try, const UnwindExits &Exits) {
  // A protected body that itself transfers control cannot gain a non-empty
  // finally: the writer no longer redirects its `__leave` to the finally,
  // and C would run the clause on a jump the machine leaves by normally.
  // The jump that ends a local unwind is the exception: the machine runs
  // the handler there, as an abnormal termination.  A nested exception
  // region with a handler outside its clause is also left alone: a
  // same-range inner `__except` then reaches its handler by a `goto` that
  // leaves this try, and a filled `__finally` would run on that edge,
  // before the handler.  An inner `__except` whose handler runs in its
  // clause stays in the try, as it does in the machine, and an inner
  // `__try/__finally` resumes nowhere, so it runs its own finally and leaves
  // by falling through, as C does.
  bool TransfersOut = false;
  walkStmts(Try.Body, [&](const HighStmt &S) {
    // A region the writer renders with fallback jumps is not one.
    const bool FinallyOnly =
        S.Kind == StmtKind::SEHTry && S.EHIsReducible && !S.EHClauses.empty() &&
        llvm::all_of(S.EHClauses, [](const HighEHClause &Clause) {
          return Clause.Kind == HighEHClauseKind::SEHFinally;
        });
    const bool InlineExcept =
        S.Kind == StmtKind::SEHTry && S.EHIsReducible && !S.EHClauses.empty() &&
        S.EHClauseBodies.size() == S.EHClauses.size() &&
        llvm::all_of(S.EHClauses,
                     [](const HighEHClause &Clause) {
                       return Clause.Kind == HighEHClauseKind::SEHExcept;
                     }) &&
        llvm::none_of(
            S.EHClauseBodies,
            [](const std::vector<HighStmt> &Clause) { return Clause.empty(); });
    if (((S.Kind == StmtKind::Return || S.Kind == StmtKind::Goto) &&
         !Exits.endsCopy(S)) ||
        S.Kind == StmtKind::Break || S.Kind == StmtKind::Continue ||
        (S.Kind == StmtKind::SEHTry && !FinallyOnly && !InlineExcept) ||
        S.Kind == StmtKind::CxxTry || S.Kind == StmtKind::ItaniumTry)
      TransfersOut = true;
  });
  return !TransfersOut && !callsLocalUnwind(Try.Body);
}

std::optional<HighStmt> returnedWork(const HighStmt &Return) {
  bool Calls = false;
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
    if (!E || Calls)
      return;
    Calls = E->Kind == ExprKind::Call;
    E->forEachChildExpr(Visit);
  };
  Visit(Return.RetVal);
  if (!Calls)
    return std::nullopt;
  HighStmt Work;
  Work.Kind = StmtKind::ExprStmt;
  Work.Addr = Return.Addr;
  Work.Val = Return.RetVal;
  return Work;
}

bool isEmptyFinallyRegion(const HighStmt &S) {
  return S.Kind == StmtKind::SEHTry && S.EHIsReducible &&
         S.EHClauses.size() == 1 && S.EHClauseBodies.size() == 1 &&
         S.EHClauses.front().Kind == HighEHClauseKind::SEHFinally &&
         S.EHClauseBodies.front().empty();
}

HighStmt guardAbnormalWork(std::vector<HighStmt> Abnormal) {
  HighStmt Guard;
  Guard.Kind = StmtKind::If;
  Guard.Cond = makeAbnormalTermination();
  Guard.Body = std::move(Abnormal);
  return Guard;
}

} // namespace seh_finally

void attachSEHFinallyBodies(std::vector<HighFunc> &Funcs, Arch TargetArch,
                            std::set<va_t> *Merged) {
  switch (TargetArch) {
  case Arch::X64:
    seh_finally::attachX64(Funcs, Merged);
    return;
  case Arch::ARM:
  case Arch::AArch64:
    seh_finally::attachARM(Funcs, TargetArch, Merged);
    return;
  default:
    return;
  }
}

} // namespace neverd
