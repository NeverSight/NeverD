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
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Sequence.h"
#include "llvm/ADT/StringRef.h"

#include <functional>
#include <limits>
#include <map>
#include <memory>
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
      : Defs(countDefinitions(Body)), Labels(gotoTargets(Body)) {}

  /// Whether nothing jumps inside the body, so a statement's place in the
  /// lists tells what runs before it.
  bool straightFlow() const { return Labels.empty(); }

  void run(std::vector<HighStmt> &In, std::map<VarKey, ExprPtr> &Copies,
           bool Unconditional, std::vector<HighStmt> &Out) {
    // A statement the pass drops leaves the label a jump lands on.
    auto Anchor = [&](const HighStmt &S) {
      if (!S.Addr || !Labels.count(S.Addr))
        return;
      HighStmt Kept;
      Kept.Kind = StmtKind::Block;
      Kept.Addr = S.Addr;
      Out.push_back(std::move(Kept));
    };
    for (HighStmt &S : In) {
      rewriteUses(S, Copies);
      if (S.Kind == StmtKind::Block) {
        Anchor(S);
        run(S.Body, Copies, Unconditional, Out);
        continue;
      }
      if ((S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse) && S.Cond) {
        if (const auto Taken = foldToConstant(*S.Cond)) {
          Anchor(S);
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
      // What a loop, a switch or a nested region holds runs under conditions
      // of its own: its uses read the copies known here, and no copy goes.
      auto Nested = [&](std::vector<HighStmt> &List) {
        std::vector<HighStmt> Kept;
        std::map<VarKey, ExprPtr> Inner = Copies;
        run(List, Inner, /*Unconditional=*/false, Kept);
        List = std::move(Kept);
      };
      Nested(S.Body);
      Nested(S.ElseBody);
      for (SwitchCase &Case : S.Cases)
        Nested(Case.Body);
      Nested(S.DefaultBody);
      for (std::vector<HighStmt> &Clause : S.EHClauseBodies)
        Nested(Clause);
      Out.push_back(std::move(S));
    }
  }

private:
  std::map<VarKey, unsigned> Defs;
  std::set<va_t> Labels;

  bool copyable(const ExprPtr &V) {
    if (!V || V->IndirectTarget)
      return false;
    if (V->Kind == ExprKind::Const)
      return true;
    return V->Kind == ExprKind::Var && Defs[keyOf(V->Var)] <= 1;
  }

  void rewriteUses(HighStmt &S, const std::map<VarKey, ExprPtr> &Copies) {
    auto Rewrite = [&](ExprPtr &Root) {
      mapExpr(Root, [&](ExprPtr E) -> ExprPtr {
        if (E && E->Kind == ExprKind::Var)
          if (auto It = Copies.find(keyOf(E->Var)); It != Copies.end())
            return cloneExpr(It->second);
        return foldExpr(std::move(E));
      });
    };
    forEachExpr(S, [&](ExprPtr &Root) {
      // The variable an assignment defines is not a use.
      if (&Root == &S.Dst && Root && Root->Kind == ExprKind::Var)
        return;
      Rewrite(Root);
    });
    for (HighEHClause &Clause : S.EHClauses)
      if (Clause.FilterValue)
        Rewrite(Clause.FilterValue);
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
    Simplifier.run(Body, Copies, /*Unconditional=*/Simplifier.straightFlow(),
                   Out);
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
    case MedVar::SEHExceptionCode:
      // The code a nested `__except` arm of its own captures moves with it.
      return true;
    default:
      // Its own frame slots and return value are its own.
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
  int MaxCodeVersion = 0;
  ForEachVar(Body, [&](MedVar &V) {
    if (V.Id >= 0)
      HandlerIds.insert(V.Id);
    if (V.Kind == MedVar::SEHExceptionCode)
      MaxCodeVersion = std::max(MaxCodeVersion, V.SSAVer);
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
    if (V.Kind == MedVar::SEHExceptionCode)
      V.SSAVer += Names.ExceptionCodeVersion;
    if (V.RenameTag >= 0)
      V.RenameTag = static_cast<int16_t>(Names.RenameTag + V.RenameTag);
    else if (Unnamed(V))
      V.Id = Fresh.at(Local(V));
  });
  Moved.RenameTag += MaxTag + 1;
  Moved.ExceptionCodeVersion += MaxCodeVersion;
  Names = std::move(Moved);
  return true;
}

int takeFreeLocalId(FuncletNames &Names) {
  return takeFreeId(Names, {}).value_or(MedVar::UnspecifiedId);
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
  // The numbering of the exception codes the parent's own arms capture.
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (E->Kind == ExprKind::Var && E->Var.Kind == MedVar::SEHExceptionCode)
      Names.ExceptionCodeVersion =
          std::max(Names.ExceptionCodeVersion, E->Var.SSAVer);
    E->forEachChildExpr(Visit);
  };
  walkStmts(Stmts, [&](const HighStmt &S) { forEachExpr(S, Visit); });
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
    // Structuring may have made the jump a `break` or a `continue` of a loop
    // around the exit.
    if (J == Stmts.size() || !((Stmts[J].Kind == StmtKind::Goto &&
                                Stmts[J].GotoTarget == Exit->Target) ||
                               Stmts[J].Kind == StmtKind::Return ||
                               Stmts[J].Kind == StmtKind::Continue ||
                               Stmts[J].Kind == StmtKind::Break)) {
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

bool UnwindExits::runsInClause(const HighStmt &Jump, va_t Handler) const {
  if (!endsCopy(Jump))
    return false;
  for (const Copy &C : Copies) {
    if (&(*C.List)[C.Jump] != &Jump)
      continue;
    // It runs the handlers it still calls in order, innermost first, and a
    // clause runs after the explicit calls before it: only the last can go.
    if (C.Calls.empty())
      return false;
    const MedSEHLocalUnwindExit *Exit = nullptr;
    const auto *Last = handlerCall((*C.List)[C.Calls.back()], Exit);
    return Last && Last->Entry == Handler;
  }
  return false;
}

void UnwindExits::runInClause(const HighStmt &Try, va_t Handler) {
  std::set<const std::vector<HighStmt> *> Lists;
  std::set<va_t> Labels;
  protectedContents(Try.Body, Lists, Labels);
  std::map<std::vector<HighStmt> *, std::set<size_t>> Erase;
  for (const Copy &C : Copies) {
    if (!Lists.count(C.List) || C.Calls.empty())
      continue;
    const MedSEHLocalUnwindExit *Exit = nullptr;
    const auto *H = handlerCall((*C.List)[C.Calls.back()], Exit);
    if (!H || H->Entry != Handler || !endsCopy((*C.List)[C.Jump]) ||
        ((*C.List)[C.Jump].Kind == StmtKind::Goto &&
         Labels.count((*C.List)[C.Jump].GotoTarget)))
      continue;
    Erase[C.List].insert(C.Calls.back());
    // C runs the clause only when the goto leaves the try as a jump.
    (*C.List)[C.Jump].AbnormalExit = true;
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

namespace {

/// The target of a call that unwinds to an address, past the casts its value
/// goes through: `_local_unwind(frame, target)` or ARM's `__jump_unwind`.
std::optional<va_t> localUnwindCallTarget(const ExprPtr &Value) {
  const HighExpr *E = Value.get();
  while (E && (E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast ||
               (E->Kind == ExprKind::UnaryOp &&
                (E->Op == NdOp::CAST || E->Op == NdOp::INT_ZEXT ||
                 E->Op == NdOp::INT_SEXT))))
    E = E->Operands.empty() ? nullptr : E->Operands[0].get();
  if (!E || E->Kind != ExprKind::Call || E->Operands.size() < 2)
    return std::nullopt;
  llvm::StringRef Name(E->CallTarget);
  Name.consume_front("__imp_");
  Name = Name.ltrim('_');
  if (!Name.starts_with("local_unwind") && Name != "jump_unwind")
    return std::nullopt;
  const HighExpr *Target = E->Operands[1].get();
  while (Target &&
         (Target->Kind == ExprKind::Cast || Target->Kind == ExprKind::BitCast ||
          (Target->Kind == ExprKind::UnaryOp &&
           (Target->Op == NdOp::CAST || Target->Op == NdOp::INT_ZEXT))))
    Target = Target->Operands.empty() ? nullptr : Target->Operands[0].get();
  if (!Target || Target->Kind != ExprKind::Const)
    return std::nullopt;
  return static_cast<va_t>(Target->ConstVal);
}

/// The value of \p S that is all it does, or null.
const ExprPtr *wholeValue(const HighStmt &S) {
  switch (S.Kind) {
  case StmtKind::Call:
    return &S.CallExpr;
  case StmtKind::ExprStmt:
  case StmtKind::Assign:
    return &S.Val;
  case StmtKind::Return:
    return &S.RetVal;
  default:
    return nullptr;
  }
}

} // namespace

void jumpLocalUnwinds(std::vector<HighStmt> &Stmts,
                      const std::set<va_t> &Targets, Arch TargetArch) {
  walkStmts(Stmts, [&](HighStmt &S) {
    const ExprPtr *Value = wholeValue(S);
    if (!Value || !*Value)
      return;
    const std::optional<va_t> Named = localUnwindCallTarget(*Value);
    if (!Named)
      return;
    // Windows on ARM runs only Thumb code, whose addresses carry the low bit.
    const va_t Target = static_cast<va_t>(
        normalizeCodeAddress(*Named, TargetArch, InstructionMode::Thumb));
    if (!Targets.count(Target))
      return;
    // The call never returns; what follows it in its list prints as what
    // follows any jump does.
    HighStmt Jump;
    Jump.Kind = StmtKind::Goto;
    Jump.Addr = S.Addr;
    Jump.GotoTarget = Target;
    Jump.AbnormalExit = true;
    S = std::move(Jump);
  });
}

namespace {

/// Whether control can reach the end of \p Stmts.
bool listRunsPast(const std::vector<HighStmt> &Stmts) {
  for (const HighStmt &S : Stmts)
    switch (S.Kind) {
    case StmtKind::Return:
    case StmtKind::Goto:
    case StmtKind::Break:
    case StmtKind::Continue:
      return false;
    case StmtKind::IfElse:
      if (!listRunsPast(S.Body) && !listRunsPast(S.ElseBody))
        return false;
      break;
    case StmtKind::Block:
      if (!listRunsPast(S.Body))
        return false;
      break;
    case StmtKind::Call:
      if (isTerminatingHighCall(S.CallExpr))
        return false;
      break;
    case StmtKind::SEHTry:
      // A `__finally` runs as the jump that leaves the try goes on; control
      // goes past the statement by its protected body or an `__except` arm.
      if (S.EHIsReducible && !listRunsPast(S.Body) &&
          llvm::none_of(
              llvm::seq<size_t>(0, S.EHClauseBodies.size()), [&](size_t K) {
                return K < S.EHClauses.size() &&
                       S.EHClauses[K].Kind == HighEHClauseKind::SEHExcept &&
                       listRunsPast(S.EHClauseBodies[K]);
              }))
        return false;
      break;
    default:
      break;
    }
  return true;
}

/// The labels statements of \p Stmts may be jumped to at.
std::set<va_t> landingAddresses(const std::vector<HighStmt> &Stmts) {
  std::set<va_t> Labels = gotoTargets(Stmts);
  walkStmts(Stmts, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::While && S.LoopHeaderAddr)
      Labels.insert(S.LoopHeaderAddr);
    for (const HighEHClause &Clause : S.EHClauses)
      if (Clause.HandlerVA)
        Labels.insert(Clause.HandlerVA);
  });
  return Labels;
}

/// Settles the `return`s of a handler's body for the clause that holds it.
class ReturnSettler {
public:
  explicit ReturnSettler(std::vector<HighStmt> &Body)
      : Body(Body), Labels(landingAddresses(Body)) {}

  bool run() {
    if (!settle(Body, /*Tail=*/true, /*InTry=*/false))
      return false;
    if (EndLabel) {
      HighStmt Anchor;
      Anchor.Kind = StmtKind::Block;
      Anchor.Addr = *EndLabel;
      Body.push_back(std::move(Anchor));
    }
    return true;
  }

private:
  std::vector<HighStmt> &Body;
  std::set<va_t> Labels;
  /// Where a `return` that is not at the clause's end jumps to.
  std::optional<va_t> EndLabel;

  bool labelFollows(const std::vector<HighStmt> &Stmts, size_t I) const {
    return std::any_of(
        Stmts.begin() + static_cast<ptrdiff_t>(I) + 1, Stmts.end(),
        [&](const HighStmt &S) { return Labels.count(S.Addr) != 0; });
  }

  /// Settle \p Stmts, whose end ends the clause when \p Tail, in a nested
  /// `__try` when \p InTry.  False when a `return` is left that could only
  /// jump out of that try.
  bool settle(std::vector<HighStmt> &Stmts, bool Tail, bool InTry) {
    // A last `return` that does nothing else leaves its statement before it
    // at the end of the clause.
    while (Tail && !Stmts.empty() && Stmts.back().Kind == StmtKind::Return &&
           !Labels.count(Stmts.back().Addr) && !returnedWork(Stmts.back()))
      Stmts.pop_back();
    for (size_t I = 0; I < Stmts.size(); ++I) {
      const bool Last = I + 1 == Stmts.size();
      switch (Stmts[I].Kind) {
      case StmtKind::Return: {
        if (InTry)
          return false;
        std::optional<HighStmt> Work = returnedWork(Stmts[I]);
        const va_t Addr = Stmts[I].Addr;
        const bool AtEnd = Tail && !labelFollows(Stmts, I);
        size_t Keep = I;
        if (AtEnd)
          Stmts.resize(Keep);
        else
          Stmts.erase(Stmts.begin() + static_cast<ptrdiff_t>(Keep));
        if (Work) {
          Stmts.insert(Stmts.begin() + static_cast<ptrdiff_t>(Keep),
                       std::move(*Work));
          ++Keep;
        }
        if (AtEnd)
          return true;
        if (!EndLabel) {
          // An address no statement of the body has.
          va_t Last = 0;
          walkStmts(Body, [&](const HighStmt &S) {
            if (S.Addr != InvalidVA)
              Last = std::max(Last, S.Addr);
          });
          EndLabel = std::max(Last, Addr) + 1;
        }
        HighStmt Jump;
        Jump.Kind = StmtKind::Goto;
        Jump.Addr = Addr;
        Jump.GotoTarget = *EndLabel;
        Stmts.insert(Stmts.begin() + static_cast<ptrdiff_t>(Keep),
                     std::move(Jump));
        I = Keep;
        break;
      }
      case StmtKind::If:
        // `if (c) { A; return; } B` is `if (c) { A } else { B }`, in which each
        // arm's end is the clause's.
        if (Tail && !Last && !listRunsPast(Stmts[I].Body) &&
            !labelFollows(Stmts, I)) {
          HighStmt &If = Stmts[I];
          If.ElseBody.assign(std::make_move_iterator(Stmts.begin() + I + 1),
                             std::make_move_iterator(Stmts.end()));
          If.Kind = StmtKind::IfElse;
          Stmts.resize(I + 1);
          HighStmt &Kept = Stmts[I];
          if (!settle(Kept.Body, true, InTry) ||
              !settle(Kept.ElseBody, true, InTry))
            return false;
          if (Kept.ElseBody.empty())
            Kept.Kind = StmtKind::If;
          return true;
        }
        if (!settle(Stmts[I].Body, Tail && Last, InTry))
          return false;
        break;
      case StmtKind::IfElse:
        if (!settle(Stmts[I].Body, Tail && Last, InTry) ||
            !settle(Stmts[I].ElseBody, Tail && Last, InTry))
          return false;
        if (Stmts[I].ElseBody.empty())
          Stmts[I].Kind = StmtKind::If;
        break;
      case StmtKind::Block:
        if (!settle(Stmts[I].Body, Tail && Last, InTry))
          return false;
        break;
      case StmtKind::SEHTry:
      case StmtKind::CxxTry:
      case StmtKind::ItaniumTry:
        // A region structuring left out prints as a plain block.
        if (!Stmts[I].EHIsReducible) {
          if (!settle(Stmts[I].Body, Tail && Last, InTry))
            return false;
          break;
        }
        if (!settle(Stmts[I].Body, false, /*InTry=*/true))
          return false;
        for (size_t K = 0; K < Stmts[I].EHClauseBodies.size(); ++K) {
          const bool Except =
              K < Stmts[I].EHClauses.size() &&
              Stmts[I].EHClauses[K].Kind == HighEHClauseKind::SEHExcept;
          // A `__finally` clause's own returns were settled when it was
          // filled; an `__except` arm runs on where its try ends.
          if (Except &&
              !settle(Stmts[I].EHClauseBodies[K], Tail && Last, InTry))
            return false;
        }
        break;
      default: {
        HighStmt &S = Stmts[I];
        if (!settle(S.Body, false, InTry) ||
            !settle(S.ElseBody, false, InTry) ||
            !settle(S.DefaultBody, false, InTry))
          return false;
        for (SwitchCase &Case : S.Cases)
          if (!settle(Case.Body, false, InTry))
            return false;
        break;
      }
      }
    }
    return true;
  }
};

/// \p Stmts holds a `__try` whose empty `__except` arm jumps to a handler
/// after it, past a statement that does not run on, and only that arm enters
/// the handler's code: the code is the arm's, as its source wrote it.
void inlineExceptArms(std::vector<HighStmt> &Stmts,
                      const std::vector<HighStmt> &Whole) {
  for (size_t I = 0; I < Stmts.size(); ++I) {
    HighStmt &S = Stmts[I];
    inlineExceptArms(S.Body, Whole);
    inlineExceptArms(S.ElseBody, Whole);
    for (SwitchCase &Case : S.Cases)
      inlineExceptArms(Case.Body, Whole);
    inlineExceptArms(S.DefaultBody, Whole);
    for (std::vector<HighStmt> &Clause : S.EHClauseBodies)
      inlineExceptArms(Clause, Whole);
    if (S.Kind != StmtKind::SEHTry || !S.EHIsReducible ||
        S.EHClauses.size() != 1 || S.EHClauseBodies.size() != 1 ||
        S.EHClauses.front().Kind != HighEHClauseKind::SEHExcept ||
        !S.EHClauseBodies.front().empty())
      continue;
    const va_t Handler = S.EHClauses.front().HandlerVA;
    size_t K = I + 1;
    while (K < Stmts.size() && Stmts[K].Addr != Handler)
      ++K;
    if (K == Stmts.size() || K == I + 1 ||
        listRunsPast({Stmts.begin() + static_cast<ptrdiff_t>(K) - 1,
                      Stmts.begin() + static_cast<ptrdiff_t>(K)}))
      continue;
    // The handler's code: up to what does not run on, or to the list's end.
    size_t End = K;
    while (End < Stmts.size()) {
      const bool RunsOn =
          listRunsPast({Stmts.begin() + static_cast<ptrdiff_t>(End),
                        Stmts.begin() + static_cast<ptrdiff_t>(End) + 1});
      ++End;
      if (!RunsOn)
        break;
    }
    // Running off the list must be what falling out of the try does.
    const bool Falls =
        End == Stmts.size() &&
        listRunsPast({Stmts.begin() + static_cast<ptrdiff_t>(K),
                      Stmts.begin() + static_cast<ptrdiff_t>(End)});
    if (Falls && Stmts[I + 1].Kind != StmtKind::Return)
      continue;
    // Nothing else enters it.
    std::set<va_t> Inside;
    for (size_t J = K; J < End; ++J)
      walkStmts(std::vector<HighStmt>{Stmts[J]}, [&](const HighStmt &Stmt) {
        if (Stmt.Addr)
          Inside.insert(Stmt.Addr);
      });
    bool Entered = false;
    walkStmts(Whole, [&](const HighStmt &Stmt) {
      if (Stmt.Kind == StmtKind::Goto && Inside.count(Stmt.GotoTarget))
        Entered = true;
      for (const HighEHClause &Clause : Stmt.EHClauses)
        if (&Clause != &S.EHClauses.front() && Inside.count(Clause.HandlerVA))
          Entered = true;
    });
    if (Entered)
      continue;
    S.EHClauseBodies.front().assign(
        std::make_move_iterator(Stmts.begin() + static_cast<ptrdiff_t>(K)),
        std::make_move_iterator(Stmts.begin() + static_cast<ptrdiff_t>(End)));
    Stmts.erase(Stmts.begin() + static_cast<ptrdiff_t>(K),
                Stmts.begin() + static_cast<ptrdiff_t>(End));
  }
}

} // namespace

bool settleHandlerReturns(std::vector<HighStmt> &Body) {
  inlineExceptArms(Body, Body);
  return ReturnSettler(Body).run();
}

namespace {

/// Whether evaluating \p E only computes a value: no memory, call or unknown.
bool computesOnly(const HighExpr &E) {
  if (E.Kind == ExprKind::Call || E.Kind == ExprKind::Load ||
      E.Kind == ExprKind::Store || E.Kind == ExprKind::Undef ||
      E.MemoryOrdering != NdMemoryOrdering::None ||
      E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  return llvm::all_of(E.Operands, [](const ExprPtr &Operand) {
    return !Operand || computesOnly(*Operand);
  });
}

/// Remove from \p Stmts the assignments of values nothing in \p Reads reads.
/// A variable as the source spells it: renaming gives an out-of-SSA name its
/// own tag on a number that a handler's other variables may share.
using PrintedName = std::pair<VarKey, int>;
PrintedName printedName(const MedVar &V) { return {keyOf(V), V.RenameTag}; }

bool dropUnreadIn(std::vector<HighStmt> &Stmts,
                  const std::map<PrintedName, unsigned> &Reads,
                  const std::set<va_t> &Labels) {
  bool Changed = false;
  for (size_t I = 0; I < Stmts.size();) {
    HighStmt &S = Stmts[I];
    Changed |= dropUnreadIn(S.Body, Reads, Labels);
    Changed |= dropUnreadIn(S.ElseBody, Reads, Labels);
    for (SwitchCase &Case : S.Cases)
      Changed |= dropUnreadIn(Case.Body, Reads, Labels);
    Changed |= dropUnreadIn(S.DefaultBody, Reads, Labels);
    for (std::vector<HighStmt> &Clause : S.EHClauseBodies)
      Changed |= dropUnreadIn(Clause, Reads, Labels);
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var &&
        S.Val && computesOnly(*S.Val) &&
        !Reads.count(printedName(S.Dst->Var)) && !Labels.count(S.Addr)) {
      Stmts.erase(Stmts.begin() + static_cast<ptrdiff_t>(I));
      Changed = true;
      continue;
    }
    ++I;
  }
  return Changed;
}

/// Where \p E addresses \p Handler's own frame, as a displacement from its
/// entry stack pointer.
std::optional<int64_t>
ownFrameOffset(const HighExpr *E, uint64_t StackPointer,
               const std::map<PrintedName, std::optional<int64_t>> &Moved,
               unsigned Depth = 0) {
  if (!E || Depth > limits::kMaxCExprPrintDepth)
    return std::nullopt;
  if (E->Kind == ExprKind::Var) {
    if (const auto It = Moved.find(printedName(E->Var)); It != Moved.end())
      return It->second;
    return E->Var.Kind == MedVar::Reg && E->Var.RegOff == StackPointer &&
                   E->Var.SSAVer == 0 && E->Var.RenameTag < 0
               ? std::optional<int64_t>(0)
               : std::nullopt;
  }
  if (E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast)
    return E->Operands.size() == 1
               ? ownFrameOffset(E->Operands[0].get(), StackPointer, Moved,
                                Depth + 1)
               : std::nullopt;
  if (E->Kind != ExprKind::BinOp || E->Operands.size() != 2 ||
      (E->Op != NdOp::INT_ADD && E->Op != NdOp::INT_SUB) || !E->Operands[1] ||
      E->Operands[1]->Kind != ExprKind::Const)
    return std::nullopt;
  const std::optional<int64_t> Base =
      ownFrameOffset(E->Operands[0].get(), StackPointer, Moved, Depth + 1);
  if (!Base)
    return std::nullopt;
  const int64_t Step = static_cast<int64_t>(E->Operands[1]->ConstVal);
  return E->Op == NdOp::INT_ADD ? *Base + Step : *Base - Step;
}

} // namespace

void dropUnreadValues(std::vector<HighStmt> &Body) {
  const std::set<va_t> Labels = landingAddresses(Body);
  for (bool Changed = true; Changed;) {
    std::map<PrintedName, unsigned> Reads;
    std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
      if (!E)
        return;
      if (E->Kind == ExprKind::Var)
        ++Reads[printedName(E->Var)];
      E->forEachChildExpr(Visit);
    };
    walkStmts(Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &Root) {
        if (&Root == &S.Dst && Root->Kind == ExprKind::Var)
          return;
        Visit(Root);
      });
      for (const HighEHClause &Clause : S.EHClauses)
        Visit(Clause.FilterValue);
    });
    Changed = dropUnreadIn(Body, Reads, Labels);
  }
}

bool localizeHandlerFrame(std::vector<HighStmt> &Body, Arch TargetArch) {
  const uint64_t SP = getTargetRegInfo(TargetArch).StackPointer;
  // What the names of the moved stack pointer hold, where one assignment gives
  // each: a handler that passes stack arguments addresses them through one.
  std::map<PrintedName, std::optional<int64_t>> Moved;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Assign || !S.Dst || S.Dst->Kind != ExprKind::Var ||
        S.Dst->Var.Kind != MedVar::Reg || S.Dst->Var.RegOff != SP ||
        S.Dst->Var.RenameTag < 0)
      return;
    const auto Offset = ownFrameOffset(S.Val.get(), SP, Moved);
    const auto [It, First] = Moved.try_emplace(printedName(S.Dst->Var), Offset);
    if (!First)
      It->second = std::nullopt;
  });
  auto SlotOffset = [&](const HighExpr *Address) {
    return ownFrameOffset(Address, SP, Moved);
  };
  // Each slot's accesses, which must agree on its width.
  struct Slot {
    uint16_t Size = 0;
    TypeRef Type;
    bool Read = false;
    MedVar Local;
  };
  std::map<int64_t, Slot> Slots;
  int NextId = 0;
  bool Consistent = true;
  auto Note = [&](int64_t Offset, uint16_t Size, const TypeRef &Type,
                  bool Read) {
    Slot &Found = Slots[Offset];
    if (Found.Size && Found.Size != Size)
      Consistent = false;
    Found.Size = Size;
    if (!Found.Type)
      Found.Type = Type;
    Found.Read |= Read;
  };
  std::function<void(const ExprPtr &)> NoteReads = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (E->Kind == ExprKind::Var && E->Var.Id >= NextId)
      NextId = E->Var.Id + 1;
    if (E->Kind == ExprKind::Load && E->Operands.size() == 1 && E->Type &&
        E->Type->Size)
      if (const auto Offset = SlotOffset(E->Operands[0].get())) {
        Note(*Offset, E->Type->Size, E->Type, /*Read=*/true);
        return;
      }
    E->forEachChildExpr(NoteReads);
  };
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Store && S.StoreAddr && S.StoreVal &&
        S.StoreVal->Type && S.StoreVal->Type->Size) {
      if (const auto Offset = SlotOffset(S.StoreAddr.get())) {
        Note(*Offset, S.StoreVal->Type->Size, S.StoreVal->Type, /*Read=*/false);
        NoteReads(S.StoreVal);
        return;
      }
    }
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Load &&
        S.Dst->Operands.size() == 1 && S.Dst->Type && S.Dst->Type->Size) {
      if (const auto Offset = SlotOffset(S.Dst->Operands[0].get())) {
        Note(*Offset, S.Dst->Type->Size, S.Dst->Type, /*Read=*/false);
        NoteReads(S.Val);
        return;
      }
    }
    forEachExpr(S, NoteReads);
  });
  if (!Consistent)
    return false;
  if (Slots.empty())
    return true;
  // Slots that overlap are one object at two widths.
  int64_t End = std::numeric_limits<int64_t>::min();
  for (auto &[Offset, Found] : Slots) {
    if (Offset < End)
      return false;
    End = Offset + Found.Size;
    Found.Local.Kind = MedVar::Temp;
    Found.Local.TheArch = TargetArch;
    Found.Local.Id = NextId++;
    Found.Local.SSAVer = 0;
    Found.Local.RenameTag = -1;
    Found.Local.Size = Found.Size;
  }
  std::function<ExprPtr(ExprPtr)> Rewrite = [&](ExprPtr E) -> ExprPtr {
    if (!E)
      return E;
    if (E->Kind == ExprKind::Load && E->Operands.size() == 1)
      if (const auto Offset = SlotOffset(E->Operands[0].get()))
        return HighExpr::makeVar(Slots.at(*Offset).Local, E->Type);
    for (ExprPtr &Operand : E->Operands)
      Operand = Rewrite(Operand);
    E->IndirectTarget = Rewrite(E->IndirectTarget);
    return E;
  };
  std::function<void(std::vector<HighStmt> &)> Walk =
      [&](std::vector<HighStmt> &Stmts) {
        for (size_t I = 0; I < Stmts.size();) {
          HighStmt &S = Stmts[I];
          std::optional<int64_t> Written;
          if (S.Kind == StmtKind::Store && S.StoreAddr)
            Written = SlotOffset(S.StoreAddr.get());
          else if (S.Kind == StmtKind::Assign && S.Dst &&
                   S.Dst->Kind == ExprKind::Load && S.Dst->Operands.size() == 1)
            Written = SlotOffset(S.Dst->Operands[0].get());
          if (Written) {
            const Slot &Found = Slots.at(*Written);
            ExprPtr Value = S.Kind == StmtKind::Store ? S.StoreVal : S.Val;
            // A slot nothing reads holds nothing the clause wants.
            if (!Found.Read) {
              Stmts.erase(Stmts.begin() + static_cast<ptrdiff_t>(I));
              continue;
            }
            HighStmt Copy;
            Copy.Kind = StmtKind::Assign;
            Copy.Addr = S.Addr;
            Copy.Dst = HighExpr::makeVar(Found.Local, Found.Type);
            Copy.Val = Rewrite(Value);
            S = std::move(Copy);
            ++I;
            continue;
          }
          forEachExpr(S, [&](ExprPtr &Root) { Root = Rewrite(Root); });
          Walk(S.Body);
          Walk(S.ElseBody);
          for (SwitchCase &Case : S.Cases)
            Walk(Case.Body);
          Walk(S.DefaultBody);
          for (std::vector<HighStmt> &Clause : S.EHClauseBodies)
            Walk(Clause);
          ++I;
        }
      };
  Walk(Body);
  // The moved stack pointer may have nothing left to address.
  dropUnreadValues(Body);
  return true;
}

void relabelHandlerCopy(std::vector<HighStmt> &Body, uint64_t Copy) {
  // The labels the copy prints: its own statements' a jump lands on.
  std::set<va_t> Own;
  walkStmts(Body, [&](const HighStmt &S) {
    Own.insert(S.Addr);
    if (S.Kind == StmtKind::While)
      Own.insert(S.LoopHeaderAddr);
  });
  Own.erase(0);
  const std::set<va_t> Landing = landingAddresses(Body);
  const auto Moved = [&](va_t Addr) -> va_t {
    return Own.count(Addr) && Landing.count(Addr) ? Addr | (Copy << 56) : Addr;
  };
  walkStmts(Body, [&](HighStmt &S) {
    S.Addr = Moved(S.Addr);
    if (S.Kind == StmtKind::While)
      S.LoopHeaderAddr = Moved(S.LoopHeaderAddr);
    if (S.Kind == StmtKind::Goto)
      S.GotoTarget = Moved(S.GotoTarget);
    for (HighEHClause &Clause : S.EHClauses) {
      Clause.HandlerVA = Moved(Clause.HandlerVA);
      for (va_t &Continuation : Clause.ContinuationVAs)
        Continuation = Moved(Continuation);
    }
  });
}

ProtectedExits scanProtectedExits(HighStmt &Try, va_t Handler,
                                  const std::set<va_t> &Continuation,
                                  const UnwindExits &Exits) {
  ProtectedExits Result;
  std::set<const std::vector<HighStmt> *> Lists;
  std::set<va_t> Labels;
  protectedContents(Try.Body, Lists, Labels);
  std::function<void(std::vector<HighStmt> &, unsigned, unsigned, unsigned)>
      Scan = [&](std::vector<HighStmt> &Stmts, unsigned InnerTries,
                 unsigned Loops, unsigned Switches) {
        for (HighStmt &S : Stmts) {
          switch (S.Kind) {
          case StmtKind::Goto:
            if (S.GotoTarget == InvalidVA || S.AbnormalExit || S.LeavesTry ||
                Labels.count(S.GotoTarget) || Exits.runsInClause(S, Handler))
              break;
            if (!InnerTries && Continuation.count(S.GotoTarget))
              Result.ToContinuation.push_back(&S);
            else
              Result.Stranded.push_back(&S);
            break;
          case StmtKind::Return:
            if (!S.AbnormalExit && !Exits.runsInClause(S, Handler))
              Result.Stranded.push_back(&S);
            break;
          case StmtKind::Break:
            if (!Loops && !Switches && !S.AbnormalExit &&
                !Exits.runsInClause(S, Handler))
              Result.Stranded.push_back(&S);
            break;
          case StmtKind::Continue:
            if (!Loops && !S.AbnormalExit && !Exits.runsInClause(S, Handler))
              Result.Stranded.push_back(&S);
            break;
          case StmtKind::While:
          case StmtKind::DoWhile:
          case StmtKind::For:
            Scan(S.Body, InnerTries, Loops + 1, Switches);
            break;
          case StmtKind::Switch:
            for (SwitchCase &Case : S.Cases)
              Scan(Case.Body, InnerTries, Loops, Switches + 1);
            Scan(S.DefaultBody, InnerTries, Loops, Switches + 1);
            break;
          case StmtKind::SEHTry:
            if (!S.EHIsReducible)
              Result.UnstructuredRegion = true;
            Scan(S.Body, InnerTries + 1, Loops, Switches);
            // An arm is outside its own `__try`, in the enclosing one.  An
            // empty arm jumps to its handler: say so, where that leaves the
            // try and a jump has to be told from a normal exit.
            for (size_t K = 0; K < S.EHClauseBodies.size(); ++K) {
              if (K < S.EHClauses.size() && S.EHClauseBodies[K].empty() &&
                  S.EHClauses[K].Kind == HighEHClauseKind::SEHExcept &&
                  S.EHClauses[K].HandlerVA &&
                  !Labels.count(S.EHClauses[K].HandlerVA)) {
                HighStmt Jump;
                Jump.Kind = StmtKind::Goto;
                Jump.Addr = S.Addr;
                Jump.GotoTarget = S.EHClauses[K].HandlerVA;
                S.EHClauseBodies[K].push_back(std::move(Jump));
              }
              Scan(S.EHClauseBodies[K], InnerTries, Loops, Switches);
            }
            break;
          case StmtKind::CxxTry:
          case StmtKind::ItaniumTry:
            Result.UnstructuredRegion = true;
            break;
          default:
            Scan(S.Body, InnerTries, Loops, Switches);
            Scan(S.ElseBody, InnerTries, Loops, Switches);
            break;
          }
        }
      };
  Scan(Try.Body, 0, 0, 0);
  return Result;
}

namespace {

/// Where control goes past \p List[\p I], past the list's end to
/// \p Continuation.
std::set<va_t> nextAddresses(const std::vector<HighStmt> &List, size_t I,
                             const std::set<va_t> &Continuation) {
  std::set<va_t> Next = fallthroughAddresses(List, I);
  for (size_t J = I + 1; J < List.size(); ++J) {
    const HighStmt &S = List[J];
    if (S.Kind != StmtKind::Nop &&
        !(S.Kind == StmtKind::Block && S.Body.empty()))
      return Next;
  }
  Next.insert(Continuation.begin(), Continuation.end());
  return Next;
}

/// The arm of an `__except` region in \p Stmts that does nothing but enter
/// \p Handler, empty or holding the jump.
HighStmt *findExceptEntering(std::vector<HighStmt> &Stmts, va_t Handler) {
  for (HighStmt &S : Stmts) {
    if (S.Kind == StmtKind::SEHTry && S.EHIsReducible &&
        S.EHClauses.size() == 1 && S.EHClauseBodies.size() == 1 &&
        S.EHClauses.front().Kind == HighEHClauseKind::SEHExcept &&
        S.EHClauses.front().HandlerVA == Handler) {
      const std::vector<HighStmt> &Arm = S.EHClauseBodies.front();
      if (Arm.empty() ||
          (Arm.size() == 1 && Arm.front().Kind == StmtKind::Goto &&
           Arm.front().GotoTarget == Handler))
        return &S;
    }
    for (std::vector<HighStmt> *List : {&S.Body, &S.ElseBody, &S.DefaultBody})
      if (HighStmt *Found = findExceptEntering(*List, Handler))
        return Found;
    for (SwitchCase &Case : S.Cases)
      if (HighStmt *Found = findExceptEntering(Case.Body, Handler))
        return Found;
    for (std::vector<HighStmt> &Clause : S.EHClauseBodies)
      if (HighStmt *Found = findExceptEntering(Clause, Handler))
        return Found;
  }
  return nullptr;
}

} // namespace

void mergeSplitFinallyRanges(std::vector<HighStmt> &List, size_t I,
                             const std::vector<HighStmt> &Whole,
                             const std::set<va_t> &Continuation,
                             UnwindExits &Exits) {
  if (!isEmptyFinallyRegion(List[I]))
    return;
  const va_t Handler = List[I].EHClauses.front().FilterOrActionVA;
  for (size_t K = I + 1; K < List.size();) {
    HighStmt &Later = List[K];
    if (!isEmptyFinallyRegion(Later) ||
        Later.EHClauses.front().FilterOrActionVA != Handler ||
        Later.Body.empty()) {
      ++K;
      continue;
    }
    // Structuring may have folded the range's last jump into falling out.
    restoreFallingUnwindJumps(Later, nextAddresses(List, K, Continuation),
                              Exits);
    Exits.rebuild();
    if (listRunsPast(Later.Body)) {
      ++K;
      continue;
    }
    // The arm that runs this range's code, which only it enters.
    va_t Entry = Later.Addr;
    HighStmt *Inner = findExceptEntering(List[I].Body, Entry);
    for (size_t F = 0; !Inner && F < Later.Body.size(); ++F) {
      Entry = Later.Body[F].Addr;
      Inner = findExceptEntering(List[I].Body, Entry);
      if (Later.Body[F].Kind != StmtKind::Block &&
          Later.Body[F].Kind != StmtKind::Nop)
        break;
    }
    unsigned Named = 0;
    walkStmts(Whole, [&](const HighStmt &S) {
      Named += S.Kind == StmtKind::Goto && S.GotoTarget == Entry;
    });
    if (!Inner || Named > Inner->EHClauseBodies.front().size()) {
      ++K;
      continue;
    }
    Inner->EHClauseBodies.front() = std::move(Later.Body);
    ExceptionAddressRange &Range = List[I].EHRange;
    if (Later.EHRange.isValid()) {
      Range.Begin = std::min(Range.Begin, Later.EHRange.Begin);
      Range.End = std::max(Range.End, Later.EHRange.End);
    }
    // What jumped over this range now falls where it landed.
    const std::set<va_t> After = fallthroughAddresses(List, K);
    List.erase(List.begin() + static_cast<ptrdiff_t>(K));
    if (K > I + 1 && List[K - 1].Kind == StmtKind::Goto &&
        After.count(List[K - 1].GotoTarget) && !List[K - 1].AbnormalExit)
      List.erase(List.begin() + static_cast<ptrdiff_t>(K - 1));
    return;
  }
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
         S.EHClauses.front().FinallyKind ==
             HighEHClause::FinallyBody::OutOfLine &&
         S.EHClauseBodies.front().empty();
}

HighStmt guardAbnormalWork(std::vector<HighStmt> Abnormal) {
  HighStmt Guard;
  Guard.Kind = StmtKind::If;
  Guard.Cond = makeAbnormalTermination();
  Guard.Body = std::move(Abnormal);
  return Guard;
}

namespace {

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
  if (ADeref || BDeref || A.Kind != B.Kind)
    return false;
  auto listsEqual = [](const std::vector<HighStmt> &X,
                       const std::vector<HighStmt> &Y) {
    return llvm::equal(X, Y, stmtEqual);
  };
  switch (A.Kind) {
  case StmtKind::Assign:
  case StmtKind::ExprStmt:
  case StmtKind::Call:
    return eq(A.Dst, B.Dst) && eq(A.Val, B.Val) && eq(A.CallExpr, B.CallExpr);
  case StmtKind::If:
  case StmtKind::IfElse:
    return eq(A.Cond, B.Cond) && listsEqual(A.Body, B.Body) &&
           listsEqual(A.ElseBody, B.ElseBody);
  case StmtKind::Block:
    return listsEqual(A.Body, B.Body);
  case StmtKind::Goto:
    return A.GotoTarget == B.GotoTarget;
  case StmtKind::Nop:
    return true;
  default:
    // Nothing else is known to be one statement's copy of another.
    return false;
  }
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

/// Whether \p Stmts end in a call that does not return.
bool endsInTermination(const std::vector<HighStmt> &Stmts) {
  return !Stmts.empty() && Stmts.back().Kind == StmtKind::Call &&
         isTerminatingHighCall(Stmts.back().CallExpr);
}

/// The statement that sets the exit a stranded jump takes.
HighStmt setExitCode(const MedVar &Flag, uint64_t Code, va_t Addr) {
  HighStmt Set;
  Set.Kind = StmtKind::Assign;
  Set.Addr = Addr;
  Set.Dst = HighExpr::makeVar(Flag, NdType::makeInt(4));
  Set.Val = HighExpr::makeConst(Code, 4, ConstantAddressProvenance::Scalar);
  Set.Val->Type = NdType::makeInt(4);
  return Set;
}

/// `if (flag == code) { Then }`.
HighStmt ifExitCode(const MedVar &Flag, uint64_t Code, HighStmt Then) {
  HighStmt If;
  If.Kind = StmtKind::If;
  If.Addr = Then.Addr;
  ExprPtr Value =
      HighExpr::makeConst(Code, 4, ConstantAddressProvenance::Scalar);
  Value->Type = NdType::makeInt(4);
  If.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL,
                                HighExpr::makeVar(Flag, NdType::makeInt(4)),
                                std::move(Value));
  If.Cond->Type = NdType::makeInt(1);
  If.Body.push_back(std::move(Then));
  return If;
}

/// `__leave`, ending the innermost `__try` normally.
HighStmt leaveTry(va_t Addr, va_t Continuation) {
  HighStmt Leave;
  Leave.Kind = StmtKind::Goto;
  Leave.Addr = Addr;
  Leave.GotoTarget = Continuation;
  Leave.LeavesTry = true;
  return Leave;
}

/// Routes the jumps \p Codes holds out of the protected body: each sets its
/// code and leaves the innermost `__try`, and each `__try` a jump left this way
/// passes it on.  Whether a statement of \p Stmts left one.
bool routeStrandedExits(std::vector<HighStmt> &Stmts,
                        const std::map<const HighStmt *, uint64_t> &Codes,
                        const MedVar &Flag, va_t Continuation) {
  bool Routed = false;
  for (size_t I = 0; I < Stmts.size(); ++I) {
    if (const auto It = Codes.find(&Stmts[I]); It != Codes.end()) {
      const va_t Addr = Stmts[I].Addr;
      const uint64_t Code = It->second;
      Stmts[I] = leaveTry(Addr, Continuation);
      Stmts.insert(Stmts.begin() + static_cast<ptrdiff_t>(I),
                   setExitCode(Flag, Code, Addr));
      ++I;
      Routed = true;
      continue;
    }
    bool Inner = false;
    {
      HighStmt &S = Stmts[I];
      if (S.Kind == StmtKind::SEHTry) {
        Inner = routeStrandedExits(S.Body, Codes, Flag, Continuation);
        // An arm stands in the enclosing `__try`.
        for (size_t K = 0; K < S.EHClauseBodies.size(); ++K)
          if (K < S.EHClauses.size() &&
              S.EHClauses[K].Kind == HighEHClauseKind::SEHExcept)
            Routed |= routeStrandedExits(S.EHClauseBodies[K], Codes, Flag,
                                         Continuation);
      } else {
        Routed |= routeStrandedExits(S.Body, Codes, Flag, Continuation);
        Routed |= routeStrandedExits(S.ElseBody, Codes, Flag, Continuation);
        for (SwitchCase &Case : S.Cases)
          Routed |= routeStrandedExits(Case.Body, Codes, Flag, Continuation);
        Routed |= routeStrandedExits(S.DefaultBody, Codes, Flag, Continuation);
      }
    }
    if (Inner) {
      const va_t Addr = Stmts[I].Addr;
      HighStmt Pass;
      Pass.Kind = StmtKind::If;
      Pass.Addr = Addr;
      ExprPtr Zero =
          HighExpr::makeConst(0, 4, ConstantAddressProvenance::Scalar);
      Zero->Type = NdType::makeInt(4);
      Pass.Cond = HighExpr::makeBinop(
          NdOp::INT_NOTEQUAL, HighExpr::makeVar(Flag, NdType::makeInt(4)),
          std::move(Zero));
      Pass.Cond->Type = NdType::makeInt(1);
      Pass.Body.push_back(leaveTry(Addr, Continuation));
      Stmts.insert(Stmts.begin() + static_cast<ptrdiff_t>(I) + 1,
                   std::move(Pass));
      ++I;
      Routed = true;
    }
  }
  return Routed;
}

/// A loop whose head the `__finally` scope guards runs inside the `__try`, as
/// it does in the machine, where structuring may have put it around the try:
/// `continue` then stays in the protected body.  False when \p S is no such
/// `while (1) { __try { ... } __finally { } break; }`.
bool hoistTryOverLoop(HighStmt &S, const ExceptionFunction &EH,
                      Arch TargetArch) {
  if (S.Kind != StmtKind::While || S.Body.empty() ||
      (S.Cond && !(S.Cond->Kind == ExprKind::Const && S.Cond->ConstVal != 0)) ||
      !isEmptyFinallyRegion(S.Body.front()) || S.Body.front().Body.empty() ||
      !EH.SEH)
    return false;
  const va_t Head =
      S.LoopHeaderAddr ? S.LoopHeaderAddr : S.Body.front().Body.front().Addr;
  const va_t Handler = S.Body.front().EHClauses.front().FilterOrActionVA;
  if (llvm::none_of(EH.SEH->Scopes, [&](const SEHScopeRecord &Scope) {
        const auto Range = getSemanticSEHGuardedRange(Scope, TargetArch, EH);
        return Scope.Kind == SEHScopeKind::Finally &&
               Scope.FilterOrFinallyVA == Handler && Range &&
               Range->contains(Head);
      }))
    return false;
  for (size_t I = 1; I < S.Body.size(); ++I)
    if (S.Body[I].Kind != StmtKind::Break && S.Body[I].Kind != StmtKind::Nop)
      return false;
  HighStmt Try = std::move(S.Body.front());
  std::vector<HighStmt> LoopBody = std::move(Try.Body);
  LoopBody.insert(LoopBody.end(), std::make_move_iterator(S.Body.begin() + 1),
                  std::make_move_iterator(S.Body.end()));
  S.Body = std::move(LoopBody);
  Try.Addr = S.Addr ? S.Addr : Try.Addr;
  Try.Body.clear();
  Try.Body.push_back(std::move(S));
  S = std::move(Try);
  return true;
}

/// Fills the `__finally` clauses of one function, outermost first: a local
/// unwind's handler calls leave the exit in the order of the clauses that take
/// them.
class ParentFill {
public:
  ParentFill(HighFunc &Parent, const std::map<va_t, HighFunc *> &ByEntry,
             Arch TargetArch, std::set<va_t> *Merged,
             std::unique_ptr<HandlerProjection> Projection)
      : Parent(Parent), ByEntry(ByEntry), TargetArch(TargetArch),
        Merged(Merged), Projection(std::move(Projection)), Exits(Parent) {}

  void run() {
    // The targets a handler's certified local unwinds may jump to, where the
    // parent has code to land on.  Another handler's runtime call stays one.
    std::set<va_t> Landing;
    walkStmts(Parent.Body, [&](const HighStmt &S) {
      if (S.Addr && S.Addr != InvalidVA)
        Landing.insert(S.Addr);
      if (S.Kind == StmtKind::While && S.LoopHeaderAddr)
        Landing.insert(S.LoopHeaderAddr);
    });
    for (va_t Target : Parent.SEHHandlerUnwindTargets)
      if (Landing.count(Target))
        UnwindTargets.insert(Target);
    fillList(Parent.Body, {});
    // Nothing has left a try before the first one is entered.
    if (ExitFlag)
      Parent.Body.insert(Parent.Body.begin(),
                         setExitCode(*ExitFlag, 0, Parent.Entry));
  }

private:
  HighFunc &Parent;
  const std::map<va_t, HighFunc *> &ByEntry;
  Arch TargetArch;
  std::set<va_t> *Merged;
  std::unique_ptr<HandlerProjection> Projection;
  UnwindExits Exits;
  std::set<va_t> UnwindTargets;
  bool Prepared = false;
  FuncletNames Names;
  /// The copies of each handler this function's clauses hold.
  std::map<va_t, uint64_t> Attached;
  /// What says which jump left a try (routeExits), and the codes it took.
  std::optional<MedVar> ExitFlag;
  uint64_t ExitCodes = 0;

  void fillList(std::vector<HighStmt> &List,
                const std::set<va_t> &Continuation) {
    for (size_t I = 0; I < List.size(); ++I) {
      if (Parent.ExceptionMetadata &&
          hoistTryOverLoop(List[I], *Parent.ExceptionMetadata, TargetArch))
        Exits.rebuild();
      if (isEmptyFinallyRegion(List[I])) {
        mergeSplitFinallyRanges(List, I, Parent.Body, Continuation, Exits);
        Exits.rebuild();
        fillRegion(List, I, nextAddresses(List, I, Continuation));
      }
      const std::set<va_t> Next = nextAddresses(List, I, Continuation);
      HighStmt &S = List[I];
      std::set<va_t> BodyEnd = Next;
      if (S.Kind == StmtKind::DoWhile || S.Kind == StmtKind::For)
        BodyEnd.clear();
      else if (S.Kind == StmtKind::While) {
        BodyEnd.clear();
        if (S.LoopHeaderAddr)
          BodyEnd.insert(S.LoopHeaderAddr);
      }
      fillList(S.Body, BodyEnd);
      fillList(S.ElseBody, Next);
      for (SwitchCase &Case : S.Cases)
        fillList(Case.Body, Next);
      fillList(S.DefaultBody, Next);
      // A `__finally` clause holds a handler filled on its own.
      for (size_t K = 0; K < S.EHClauseBodies.size(); ++K)
        if (K < S.EHClauses.size() &&
            S.EHClauses[K].Kind == HighEHClauseKind::SEHExcept)
          fillList(S.EHClauseBodies[K], Next);
    }
  }

  void fillRegion(std::vector<HighStmt> &List, size_t I,
                  const std::set<va_t> &Continuation) {
    HighStmt &Stmt = List[I];
    HighEHClause &Clause = Stmt.EHClauses.front();
    const va_t FuncletVA = Clause.FilterOrActionVA;
    const auto Found = ByEntry.find(FuncletVA);
    if (Found == ByEntry.end() || !Found->second || Found->second == &Parent) {
      return;
    }
    const HighFunc &Handler = *Found->second;
    if (Handler.ExceptionMetadata &&
        (Handler.ExceptionMetadata->Cxx || Handler.ExceptionMetadata->Itanium ||
         Handler.ExceptionMetadata->Registration)) {
      return;
    }
    if (!Prepared) {
      Names = firstFreeNames(Parent.Body);
      Projection->prepare(Names);
      Prepared = true;
    }

    // What runs while unwinding, with the flag set.
    FuncletNames AbnormalNames = Names;
    std::optional<std::vector<HighStmt>> Abnormal = Projection->project(
        Handler, UnwindTargets,
        HighExpr::makeConst(1, 4, ConstantAddressProvenance::Scalar),
        AbnormalNames);
    if (!Abnormal) {
      return;
    }
    // A handler with nothing to do while unwinding leaves C's clause empty:
    // no exit of the try can tell it from an out-of-line one.
    if (Abnormal->empty()) {
      Exits.runInClause(Stmt, FuncletVA);
      Clause.FinallyKind = HighEHClause::FinallyBody::AbnormalWork;
      if (Merged)
        Merged->insert(FuncletVA);
      return;
    }
    // The protected body's normal exits fall through to what follows it, the
    // inline normal-path copy; only a local unwind leaves it as a jump, which
    // now runs the handler as the clause.
    dropFallthroughExits(Stmt, Continuation, gotoTargets(Parent.Body), Exits);
    restoreFallingUnwindJumps(Stmt, Continuation, Exits);
    Exits.rebuild();
    ProtectedExits Leaving =
        scanProtectedExits(Stmt, FuncletVA, Continuation, Exits);
    if (Leaving.UnstructuredRegion) {
      return;
    }
    for (HighStmt *Exit : Leaving.ToContinuation)
      Exit->LeavesTry = true;
    const bool Routing = !Leaving.Stranded.empty();

    // Prove the statements after the `__try` are the inline normal-path copy,
    // the body with its flag folded false, and if so run the whole finally in
    // the clause (`__finally { *Counter += 1; }`).  Otherwise that copy stays
    // and the clause holds what runs while unwinding, so the finally never
    // runs twice (`__finally { if (AbnormalTermination()) *Counter = 99; }`).
    std::optional<size_t> CopyLength;
    std::optional<std::vector<HighStmt>> Whole;
    FuncletNames WholeNames = Names;
    if (!Routing && !endsInTermination(*Abnormal)) {
      FuncletNames NormalNames = Names;
      std::optional<std::vector<HighStmt>> Normal = Projection->project(
          Handler, UnwindTargets,
          HighExpr::makeConst(0, 4, ConstantAddressProvenance::Scalar),
          NormalNames);
      if (Normal)
        CopyLength =
            inlineCopyLength(List, I, *Normal, gotoTargets(Parent.Body));
      if (CopyLength)
        Whole = Projection->project(Handler, UnwindTargets,
                                    makeAbnormalTermination(), WholeNames);
    }

    // A later attach takes fresh names past the ones this one takes.
    Exits.runInClause(Stmt, FuncletVA);
    const uint64_t Copy = Attached[FuncletVA]++;
    if (Whole) {
      if (Copy)
        relabelHandlerCopy(*Whole, Copy);
      Stmt.EHClauseBodies.front() = std::move(*Whole);
      Clause.FinallyKind = HighEHClause::FinallyBody::WholeBody;
      List.erase(List.begin() + I + 1, List.begin() + I + 1 + *CopyLength);
      Names = WholeNames;
    } else {
      if (Copy)
        relabelHandlerCopy(*Abnormal, Copy);
      Stmt.EHClauseBodies.front().push_back(
          guardAbnormalWork(std::move(*Abnormal)));
      Clause.FinallyKind = HighEHClause::FinallyBody::AbnormalWork;
      Names = AbnormalNames;
    }
    if (Merged)
      Merged->insert(FuncletVA);
    if (Routing)
      routeExits(List, I, Continuation, Leaving.Stranded);
    Exits.rebuild();
  }

  /// C runs a filled clause for a jump out of its `__try`, and reads it as an
  /// abnormal termination, where the machine leaves without running the
  /// handler: each such jump says where it goes, leaves the try normally, and
  /// is made after it.
  void routeExits(std::vector<HighStmt> &List, size_t I,
                  const std::set<va_t> &Continuation,
                  const std::vector<HighStmt *> &Stranded) {
    if (!ExitFlag) {
      MedVar Flag;
      Flag.Kind = MedVar::Temp;
      Flag.TheArch = TargetArch;
      Flag.Id = takeFreeLocalId(Names);
      Flag.SSAVer = 0;
      Flag.RenameTag = -1;
      Flag.Size = 4;
      ExitFlag = Flag;
    }
    // The jumps, in order, each to be made after the try unless it lands where
    // the try falls through anyway.
    std::map<const HighStmt *, uint64_t> Codes;
    std::vector<HighStmt> Made;
    for (HighStmt *Exit : Stranded) {
      Codes.emplace(Exit, ++ExitCodes);
      if (!(Exit->Kind == StmtKind::Goto &&
            Continuation.count(Exit->GotoTarget)))
        Made.push_back(ifExitCode(*ExitFlag, ExitCodes, cloneStmt(*Exit)));
    }
    const va_t Resume = Continuation.empty() ? 0 : *Continuation.begin();
    routeStrandedExits(List[I].Body, Codes, *ExitFlag, Resume);
    List.insert(List.begin() + static_cast<ptrdiff_t>(I) + 1,
                std::make_move_iterator(Made.begin()),
                std::make_move_iterator(Made.end()));
  }
};

} // namespace

} // namespace seh_finally

void attachSEHFinallyBodies(std::vector<HighFunc> &Funcs, Arch TargetArch,
                            std::set<va_t> *Merged) {
  using namespace seh_finally;
  if (TargetArch != Arch::X64 && TargetArch != Arch::ARM &&
      TargetArch != Arch::AArch64)
    return;
  // Index just the handlers some function's scope table names, so a unit
  // without one pays nothing.
  std::map<va_t, HighFunc *> ByEntry;
  auto OutOfLineFinally = [](const HighFunc &Func, const SEHScopeRecord &S) {
    return S.Kind == SEHScopeKind::Finally && S.FilterOrFinallyVA &&
           !Func.ExceptionMetadata->ownsCode(S.FilterOrFinallyVA);
  };
  for (const HighFunc &Func : Funcs)
    if (Func.ExceptionMetadata && Func.ExceptionMetadata->SEH)
      for (const SEHScopeRecord &Scope : Func.ExceptionMetadata->SEH->Scopes)
        if (OutOfLineFinally(Func, Scope))
          ByEntry.emplace(Scope.FilterOrFinallyVA, nullptr);
  if (ByEntry.empty())
    return;
  for (HighFunc &Func : Funcs)
    if (auto It = ByEntry.find(Func.Entry); It != ByEntry.end() && !It->second)
      It->second = &Func;

  // A handler's own `__finally` clauses fill before a parent takes its body,
  // whichever order the unit holds them in.
  std::set<const HighFunc *> Visited;
  std::function<void(HighFunc &)> Fill = [&](HighFunc &Func) {
    if (!Visited.insert(&Func).second || !Func.ExceptionMetadata ||
        !Func.ExceptionMetadata->SEH)
      return;
    for (const SEHScopeRecord &Scope : Func.ExceptionMetadata->SEH->Scopes)
      if (OutOfLineFinally(Func, Scope))
        if (const auto It = ByEntry.find(Scope.FilterOrFinallyVA);
            It != ByEntry.end() && It->second)
          Fill(*It->second);
    ParentFill(Func, ByEntry, TargetArch, Merged,
               TargetArch == Arch::X64 ? makeX64Projection(Func)
                                       : makeARMProjection(Func, TargetArch))
        .run();
  };
  for (HighFunc &Func : Funcs)
    Fill(Func);
}

} // namespace neverd
