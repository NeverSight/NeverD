//===- HighFrameStoreForwarding.cpp - Safe frame load forwarding ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Forward exact private frame reads before semantic expression simplification.
/// Facts describe source locals after renaming, and only remain valid while
/// their inputs and every byte of the stored value remain unchanged.
///
//===----------------------------------------------------------------------===//

#include "HighDCEDetail.h"
#include "HighFrameAddress.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighSourceFlow.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neverd {
namespace {

using LocalIdentity = HighSourceLocalIdentity;
using FrameAliases = std::map<LocalIdentity, ExprPtr>;

struct StoredFrameValue {
  int64_t Offset = 0;
  uint16_t Bytes = 0;
  TypeRef Type;
  ExprPtr Value;
  std::set<LocalIdentity> Dependencies;
  size_t Nodes = 0;
};

bool isForwardableInteger(const TypeRef &Type) {
  return Type && Type->Kind == NdTypeKind::Int &&
         (Type->Size == 1 || Type->Size == 2 || Type->Size == 4 ||
          Type->Size == 8);
}

bool isForwardableScalar(const TypeRef &Type) {
  return isForwardableInteger(Type) || (Type && Type->Kind == NdTypeKind::Ptr &&
                                        (Type->Size == 4 || Type->Size == 8));
}

bool sameIntegerWidth(const TypeRef &Left, const TypeRef &Right) {
  return isForwardableInteger(Left) && isForwardableInteger(Right) &&
         Left->Size == Right->Size;
}

bool sameIntegerType(const TypeRef &Left, const TypeRef &Right) {
  return sameIntegerWidth(Left, Right) && Left->IsSigned == Right->IsSigned;
}

bool rangesOverlap(int64_t Left, uint16_t LeftBytes, int64_t Right,
                   uint16_t RightBytes) {
  return Left <= Right ? uint64_t(Right) - uint64_t(Left) < LeftBytes
                       : uint64_t(Left) - uint64_t(Right) < RightBytes;
}

bool hasValidShape(const HighExpr &E) {
  switch (E.Kind) {
  case ExprKind::Var:
  case ExprKind::Const:
  case ExprKind::Undef:
    return E.Operands.empty();
  case ExprKind::Load:
    return E.Operands.size() == 1 && E.Type && E.Type->Size;
  case ExprKind::Cast:
  case ExprKind::BitCast:
  case ExprKind::UnaryOp:
    return E.Operands.size() == 1;
  case ExprKind::BinOp:
    return E.Operands.size() == (E.Op == NdOp::FLOAT_FMA ? 3u : 2u);
  default:
    return true;
  }
}

// Unlike symbolic rewriting, forwarding re-evaluates an expression later.
// Only known, total integer operations with explicit widths can be replayed.
bool replayableIntegerNode(const HighExpr &E) {
  if (!isForwardableScalar(E.Type) || E.IndirectTarget ||
      E.IntrinsicId != Intrinsic::None || !E.IntrinsicOutputs.empty() ||
      E.MemoryOrdering != NdMemoryOrdering::None ||
      E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  for (const auto &Child : E.Operands)
    if (!Child || !isForwardableScalar(Child->Type))
      return false;
  switch (E.Kind) {
  case ExprKind::Const:
    return E.Operands.empty() && E.AddressOwnerVA == InvalidVA &&
           (E.ConstProvenance == ConstantAddressProvenance::Unknown ||
            E.ConstProvenance == ConstantAddressProvenance::Scalar);
  case ExprKind::Var:
    return E.Operands.empty() && E.Var.Size == E.Type->Size &&
           (E.Var.Kind == MedVar::Reg || E.Var.Kind == MedVar::Temp ||
            E.Var.Kind == MedVar::Param || E.Var.Kind == MedVar::RetVal);
  case ExprKind::Cast:
    return E.Operands.size() == 1 && E.CastTo &&
           E.CastTo->Kind == E.Type->Kind && E.CastTo->Size == E.Type->Size &&
           ((E.Type->Kind == NdTypeKind::Int &&
             E.CastTo->IsSigned == E.Type->IsSigned) ||
            (E.Type->Kind == NdTypeKind::Ptr &&
             E.Operands[0]->Type->Size == E.Type->Size));
  case ExprKind::UnaryOp:
    if (E.Operands.size() != 1 || !isForwardableInteger(E.Type) ||
        !isForwardableInteger(E.Operands[0]->Type))
      return false;
    if (E.Op == NdOp::INT_ZEXT || E.Op == NdOp::INT_SEXT)
      return E.Type->Size >= E.Operands[0]->Type->Size;
    return (E.Op == NdOp::INT_NOT || E.Op == NdOp::INT_NEGATE ||
            E.Op == NdOp::INT_NEG2) &&
           E.Type->Size == E.Operands[0]->Type->Size;
  case ExprKind::BinOp:
    if (E.Operands.size() != 2 || !isForwardableInteger(E.Type) ||
        !isForwardableInteger(E.Operands[0]->Type) ||
        !isForwardableInteger(E.Operands[1]->Type) ||
        E.Type->Size != E.Operands[0]->Type->Size)
      return false;
    switch (E.Op) {
    case NdOp::INT_LEFT:
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR:
      return E.Operands[1]->Kind == ExprKind::Const &&
             E.Operands[1]->ConstVal < E.Type->Size * 8u;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_MULT:
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
      return E.Type->Size == E.Operands[1]->Type->Size;
    default:
      return false;
    }
  default:
    return false;
  }
}

bool safeFrameValue(const ExprPtr &Root, std::set<LocalIdentity> &Dependencies,
                    size_t &NodeCount, size_t &Budget) {
  struct Item {
    ExprPtr Expr;
    bool ChildrenReady = false;
  };
  std::vector<Item> Work{{Root, false}};
  std::unordered_map<const HighExpr *, size_t> Costs;
  HighExprSet Active;
  while (!Work.empty()) {
    const auto [E, Ready] = Work.back();
    Work.pop_back();
    if (!Budget || !E)
      return false;
    --Budget;
    if (Costs.count(E.get()))
      continue;
    if (!Ready) {
      if (!Active.insert(E.get()).second || !replayableIntegerNode(*E))
        return false;
      if (E->Kind == ExprKind::Var)
        Dependencies.insert(highSourceLocalIdentity(E->Var));
      Work.push_back({E, true});
      for (const auto &Child : E->Operands)
        Work.push_back({Child, false});
      continue;
    }
    Active.erase(E.get());
    size_t Cost = 1;
    for (const auto &Child : E->Operands) {
      const size_t ChildCost = Costs.at(Child.get());
      if (ChildCost >
          ::neverd::limits::ir::kHighFrameStoreForwardingMaxForwardValueNodes -
              Cost)
        return false;
      Cost += ChildCost;
    }
    Costs.emplace(E.get(), Cost);
  }
  NodeCount = Costs.at(Root.get());
  return true;
}

// Only a statement's outermost call has a known argument/effect boundary.
// Nested calls can change memory before another argument or operand reads it.
const HighExpr *statementCall(const HighStmt &Stmt) {
  const HighExpr *Root = nullptr;
  switch (Stmt.Kind) {
  case StmtKind::Assign:
    if (Stmt.Dst && Stmt.Dst->Kind == ExprKind::Var)
      Root = Stmt.Val.get();
    break;
  case StmtKind::ExprStmt:
    Root = Stmt.Val.get();
    break;
  case StmtKind::Call:
    Root = Stmt.CallExpr.get();
    break;
  case StmtKind::Return:
    Root = Stmt.RetVal.get();
    break;
  default:
    break;
  }
  return Root && Root->Kind == ExprKind::Call ? Root : nullptr;
}

// The source flow graph's reading of a test: a missing loop test is true,
// and only an integer constant decides one.
std::optional<bool> testTruth(const ExprPtr &Test) {
  if (!Test)
    return true;
  if (Test->Kind != ExprKind::Const || !Test->Type ||
      Test->Type->Kind != NdTypeKind::Int)
    return std::nullopt;
  return Test->ConstVal != 0;
}

// Whether \p Body has a `break` or `continue` (\p Kind) for the statement
// that encloses it. A nested loop owns both, and a nested switch its breaks.
bool hasOwnJump(const std::vector<HighStmt> &Body, StmtKind Kind) {
  for (const HighStmt &S : Body) {
    if (S.Kind == Kind)
      return true;
    if (S.Kind == StmtKind::While || S.Kind == StmtKind::DoWhile ||
        S.Kind == StmtKind::For ||
        (Kind == StmtKind::Break && S.Kind == StmtKind::Switch))
      continue;
    if (hasOwnJump(S.Body, Kind) || hasOwnJump(S.ElseBody, Kind) ||
        hasOwnJump(S.DefaultBody, Kind) ||
        llvm::any_of(S.Cases, [&](const SwitchCase &Case) {
          return hasOwnJump(Case.Body, Kind);
        }))
      return true;
  }
  return false;
}

bool mayComplete(const HighStmt &S);

// Whether control entering \p Body can leave its end, taking either way of
// every test that is not constant, as the source flow graph does.
bool mayFallThrough(const std::vector<HighStmt> &Body) {
  return llvm::all_of(Body, mayComplete);
}

// Whether control entering \p S can continue after it.
bool mayComplete(const HighStmt &S) {
  switch (S.Kind) {
  case StmtKind::Return:
  case StmtKind::Goto:
  case StmtKind::Break:
  case StmtKind::Continue:
    return false;
  case StmtKind::Assign:
  case StmtKind::ExprStmt:
    return !isTerminatingHighCall(S.Val);
  case StmtKind::Call:
    return !isTerminatingHighCall(S.CallExpr);
  case StmtKind::Block:
    return mayFallThrough(S.Body);
  case StmtKind::If:
  case StmtKind::IfElse: {
    const auto Truth = testTruth(S.Cond);
    return ((!Truth || *Truth) && mayFallThrough(S.Body)) ||
           ((!Truth || !*Truth) && mayFallThrough(S.ElseBody));
  }
  case StmtKind::While:
  case StmtKind::For: {
    const auto Truth = testTruth(S.Cond);
    return !Truth || !*Truth || hasOwnJump(S.Body, StmtKind::Break);
  }
  case StmtKind::DoWhile: {
    const auto Truth = testTruth(S.Cond);
    return ((!Truth || !*Truth) && (mayFallThrough(S.Body) ||
                                    hasOwnJump(S.Body, StmtKind::Continue))) ||
           hasOwnJump(S.Body, StmtKind::Break);
  }
  case StmtKind::Switch:
    return mayFallThrough(S.DefaultBody) ||
           hasOwnJump(S.DefaultBody, StmtKind::Break) ||
           llvm::any_of(S.Cases, [](const SwitchCase &Case) {
             return (!Case.FallsThrough && mayFallThrough(Case.Body)) ||
                    hasOwnJump(Case.Body, StmtKind::Break);
           });
  default:
    return true;
  }
}

// Whether control from the entry certainly enters a cycle, taking either way
// of every test that is not constant: a loop whose body can return to its
// test, or a goto to a label that reaches it. FrameWritebacks commits nothing
// across a cycle, and refining the graph's tests could remove one only where
// its back edge can never run. \p Labels are the function's goto targets.
bool entersCycle(const std::vector<HighStmt> &Body,
                 const std::set<va_t> &Labels) {
  // Labels that fall through to the visited statement, and the labels that a
  // goto reached from the entry jumps to.
  std::vector<va_t> Reaching;
  std::set<va_t> Entered;
  bool Cyclic = false;
  const auto Visit = [&](auto &&Self, const std::vector<HighStmt> &List,
                         size_t From, bool Live) -> void {
    const size_t Mark = Reaching.size();
    for (const HighStmt &S : List) {
      if (Cyclic)
        return;
      if (S.Addr && S.Addr != InvalidVA && Labels.count(S.Addr)) {
        Reaching.push_back(S.Addr);
        Live |= Entered.count(S.Addr) != 0;
      }
      const auto Truth = testTruth(S.Cond);
      if (Live && S.Kind == StmtKind::Goto) {
        Cyclic |= std::find(Reaching.begin() + From, Reaching.end(),
                            S.GotoTarget) != Reaching.end();
        Entered.insert(S.GotoTarget);
      }
      if (Live &&
          (S.Kind == StmtKind::While || S.Kind == StmtKind::For ||
           S.Kind == StmtKind::DoWhile) &&
          (!Truth || *Truth) &&
          (mayFallThrough(S.Body) || hasOwnJump(S.Body, StmtKind::Continue)))
        Cyclic = true;
      const bool Conditional =
          S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse ||
          S.Kind == StmtKind::While || S.Kind == StmtKind::For;
      Self(Self, S.Body, From, Live && (!Conditional || !Truth || *Truth));
      Self(Self, S.ElseBody, From, Live && (!Truth || !*Truth));
      // A constant selector enters one arm; a falling case may enter more,
      // which this conservative walk does not follow.
      const bool Selected =
          S.SwitchExpr && S.SwitchExpr->Kind == ExprKind::Const;
      bool Matched = false;
      for (const SwitchCase &Case : S.Cases) {
        const bool Taken = !Selected || Case.Value == S.SwitchExpr->ConstVal;
        Matched |= Selected && Taken;
        Self(Self, Case.Body, From, Live && Taken);
      }
      Self(Self, S.DefaultBody, From, Live && !Matched);
      if (!mayComplete(S)) {
        From = Reaching.size();
        Live = false;
      }
    }
    Reaching.resize(Mark);
  };
  Visit(Visit, Body, 0, true);
  return Cyclic;
}

// Prove the effect boundary once, with an acyclic, bounded graph walk. An
// outermost call consumes its arguments before invalidating memory facts;
// nested effects, atomic operations and address escapes remain unsupported.
// \p Body is a statement list of \p Func, which owns the frame they access.
bool isStraightLinePrivateFrameFunction(llvm::ArrayRef<HighStmt> Body,
                                        const HighFunc &Func, Arch Architecture,
                                        size_t &Budget) {
  if (Func.FrameSize <= 0 || Func.StructuredExceptionRegions ||
      Func.UnstructuredExceptionRegions)
    return false;
  HighExprSet Seen, Active;
  for (const HighStmt &Stmt : Body) {
    if (!Budget || !Stmt.Body.empty() || !Stmt.ElseBody.empty() ||
        !Stmt.Cases.empty() || !Stmt.DefaultBody.empty() ||
        !Stmt.EHClauseBodies.empty() || !Stmt.EHClauses.empty() ||
        Stmt.IsPhiCopy || Stmt.MemoryOrdering != NdMemoryOrdering::None ||
        Stmt.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
    --Budget;
    if (Stmt.Kind == StmtKind::Block) {
      // Dead-store removal leaves empty block anchors in the straight-line
      // body. They neither access memory nor change control flow. Reject
      // malformed anchors carrying expressions instead of ignoring them.
      bool Empty = true;
      forEachExpr(Stmt, [&](const ExprPtr &) { Empty = false; });
      if (Empty)
        continue;
      return false;
    }
    if (Stmt.Kind != StmtKind::Assign && Stmt.Kind != StmtKind::Store &&
        Stmt.Kind != StmtKind::ExprStmt && Stmt.Kind != StmtKind::Call &&
        Stmt.Kind != StmtKind::Return && Stmt.Kind != StmtKind::Nop)
      return false;
    if (Stmt.Kind == StmtKind::Assign &&
        (!Stmt.Dst || !Stmt.Val ||
         (Stmt.Dst->Kind != ExprKind::Var &&
          Stmt.Dst->Kind != ExprKind::Load) ||
         (Stmt.Dst->Kind == ExprKind::Var &&
          isSyntheticEntryStackPointer(Stmt.Dst->Var, Func, Architecture))))
      return false;
    if (Stmt.Kind == StmtKind::Store && (!Stmt.StoreAddr || !Stmt.StoreVal))
      return false;
    const auto *Call = statementCall(Stmt);
    if (Stmt.Kind == StmtKind::Call && !Call)
      return false;
    bool Safe = true;
    forEachExpr(Stmt, [&](const ExprPtr &Root) {
      std::vector<std::pair<ExprPtr, bool>> Work{{Root, false}};
      while (Safe && !Work.empty()) {
        const auto [E, Ready] = Work.back();
        Work.pop_back();
        if (!Budget || !E || (E->Kind == ExprKind::Call && E.get() != Call)) {
          Safe = false;
          break;
        }
        --Budget;
        if (Seen.count(E.get()))
          continue;
        if (Ready) {
          Active.erase(E.get());
          Seen.insert(E.get());
          continue;
        }
        if (!Active.insert(E.get()).second || !hasValidShape(*E) ||
            E->IndirectTarget || E->Kind == ExprKind::Addr ||
            E->Kind == ExprKind::Store || E->Op == NdOp::ATOMIC_ADD ||
            E->Op == NdOp::ATOMIC_XCHG || E->Op == NdOp::ATOMIC_CMPXCHG ||
            E->IntrinsicId != Intrinsic::None || !E->IntrinsicOutputs.empty() ||
            E->MemoryOrdering != NdMemoryOrdering::None ||
            E->MemoryAddressSpace != NdMemoryAddressSpace::Default) {
          Safe = false;
          break;
        }
        Work.push_back({E, true});
        for (const auto &Child : E->Operands)
          Work.push_back({Child, false});
      }
    });
    if (!Safe)
      return false;
  }
  return true;
}

bool isStraightLinePrivateFrameFunction(const HighFunc &Func, Arch Architecture,
                                        size_t &Budget) {
  return isStraightLinePrivateFrameFunction(Func.Body, Func, Architecture,
                                            Budget);
}

std::optional<int64_t> privateFrameOffset(const ExprPtr &Address,
                                          uint16_t Bytes, const HighFunc &Func,
                                          Arch Architecture, size_t &Budget,
                                          const FrameAliases &Aliases) {
  if (!Bytes)
    return std::nullopt;
  const auto Alias = [&](const MedVar &V) -> ExprPtr {
    auto It = Aliases.find(highSourceLocalIdentity(V));
    return It == Aliases.end() ? nullptr : It->second;
  };
  const auto Offset = high_detail::frameAddressOffset(
      Address, Func, Architecture, Budget, 0, Alias);
  if (!Offset || *Offset >= 0 || *Offset < -Func.FrameSize ||
      uint64_t(Bytes) > uint64_t(-*Offset))
    return std::nullopt;
  return Offset;
}

ExprPtr replaceFrameLoads(const ExprPtr &Root,
                          const std::map<int64_t, StoredFrameValue> &Stores,
                          const HighFunc &Func, Arch Architecture,
                          size_t &Budget, size_t &ExpansionBudget,
                          const FrameAliases &Aliases) {
  if (!Root || Stores.empty())
    return Root;
  struct Item {
    ExprPtr Expr;
    bool ChildrenReady = false;
  };
  struct Replacement {
    ExprPtr Expr;
    size_t Cost = 1;
    size_t Expansion = 0;
  };
  std::vector<Item> Work{{Root, false}};
  std::unordered_map<const HighExpr *, Replacement> Rebuilt;
  while (!Work.empty()) {
    const auto [E, Ready] = Work.back();
    Work.pop_back();
    if (!Budget)
      return Root;
    --Budget;
    if (Rebuilt.count(E.get()))
      continue;
    if (!Ready) {
      if (E->Kind == ExprKind::Load && E->Type && E->Operands.size() == 1) {
        const auto Offset = privateFrameOffset(
            E->Operands[0], E->Type->Size, Func, Architecture, Budget, Aliases);
        const auto It = Offset ? Stores.find(*Offset) : Stores.end();
        if (It != Stores.end() && It->second.Bytes == E->Type->Size &&
            isForwardableScalar(It->second.Type) &&
            isForwardableScalar(E->Type)) {
          // A store/load pair is also an integer truncation boundary. Preserve
          // it explicitly when C's integer promotions would widen an operator
          // or the read interprets the same stored bits with another sign.
          auto Value = std::make_shared<HighExpr>();
          Value->Kind = ExprKind::Cast;
          Value->Type = Value->CastTo = E->Type;
          Value->Operands = {It->second.Value};
          const size_t Cost = It->second.Nodes + 1;
          Rebuilt.emplace(E.get(), Replacement{Value, Cost, Cost});
          continue;
        }
      }
      Work.push_back({E, true});
      for (const auto &Child : E->Operands)
        Work.push_back({Child, false});
      continue;
    }
    Replacement Result{E};
    bool Changed = false;
    for (const auto &Child : E->Operands) {
      const auto &R = Rebuilt.at(Child.get());
      if (R.Cost > ::neverd::limits::ir::
                           kHighFrameStoreForwardingMaxForwardExpansionNodes -
                       Result.Cost ||
          R.Expansion > ExpansionBudget - Result.Expansion)
        return Root;
      Result.Cost += R.Cost;
      Result.Expansion += R.Expansion;
      Changed |= R.Expr != Child;
    }
    if (Changed) {
      Result.Expr = std::make_shared<HighExpr>(*E);
      for (auto &Child : Result.Expr->Operands)
        Child = Rebuilt.at(Child.get()).Expr;
    }
    Rebuilt.emplace(E.get(), std::move(Result));
  }
  const auto &Result = Rebuilt.at(Root.get());
  if (Result.Expansion > ExpansionBudget)
    return Root;
  ExpansionBudget -= Result.Expansion;
  return Result.Expr;
}

bool recordFrameStore(const ExprPtr &Address, const ExprPtr &Value,
                      const TypeRef &AccessType,
                      std::map<int64_t, StoredFrameValue> &Stores,
                      const HighFunc &Func, Arch Architecture, size_t &Budget,
                      const FrameAliases &Aliases) {
  const uint16_t Bytes = AccessType ? AccessType->Size : uint16_t(0);
  const auto Offset =
      privateFrameOffset(Address, Bytes, Func, Architecture, Budget, Aliases);
  if (!Offset) {
    const auto Alias = [&](const MedVar &V) -> ExprPtr {
      const auto I = Aliases.find(highSourceLocalIdentity(V));
      return I == Aliases.end() ? nullptr : I->second;
    };
    const auto Known = high_detail::frameAddressOffset(
        Address, Func, Architecture, Budget, 0, Alias);
    if (!Known || !Bytes)
      Stores.clear();
    else
      std::erase_if(Stores, [&](const auto &S) {
        return rangesOverlap(S.second.Offset, S.second.Bytes, *Known, Bytes);
      });
    return false;
  }
  bool Redundant = false;
  if (const auto Old = Stores.find(*Offset);
      Old != Stores.end() && Old->second.Bytes == Bytes && Value &&
      isForwardableInteger(Value->Type)) {
    const auto Bits = [](ExprPtr E) {
      while (E && E->Kind == ExprKind::Cast && E->Operands.size() == 1 &&
             E->Operands[0] && isForwardableInteger(E->Type) &&
             isForwardableInteger(E->Operands[0]->Type) &&
             E->Operands[0]->Type->Size == E->Type->Size)
        E = E->Operands[0];
      return E;
    };
    const auto Previous = Bits(Old->second.Value);
    const auto Current = Bits(Value);
    Redundant = Previous && Current && Previous->structuralEq(*Current);
  }
  for (auto It = Stores.begin(); It != Stores.end();) {
    if (rangesOverlap(It->second.Offset, It->second.Bytes, *Offset, Bytes))
      It = Stores.erase(It);
    else
      ++It;
  }
  if (!Value || !isForwardableScalar(AccessType) ||
      !isForwardableScalar(Value->Type) ||
      AccessType->Size != Value->Type->Size)
    return false;
  StoredFrameValue Stored;
  Stored.Offset = *Offset;
  Stored.Bytes = Bytes;
  Stored.Type = AccessType;
  Stored.Value = Value;
  if (safeFrameValue(Value, Stored.Dependencies, Stored.Nodes, Budget)) {
    Stores.emplace(*Offset, std::move(Stored));
    return Redundant;
  }
  return false;
}

// A Stack variable names a frame home, so reading it observes the frame
// bytes a pending store may still provide.
bool readsStackHome(const ExprPtr &Root) {
  std::vector<const HighExpr *> Pending{Root.get()};
  HighExprSet Seen;
  while (!Pending.empty()) {
    const HighExpr *E = Pending.back();
    Pending.pop_back();
    if (!E || !Seen.insert(E).second)
      continue;
    if (E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Stack)
      return true;
    E->forEachChildExpr(
        [&](const ExprPtr &Child) { Pending.push_back(Child.get()); });
  }
  return false;
}

void invalidateWrittenVariable(std::map<int64_t, StoredFrameValue> &Stores,
                               const MedVar &Variable) {
  const auto Key = highSourceLocalIdentity(Variable);
  for (auto It = Stores.begin(); It != Stores.end();) {
    if (It->second.Dependencies.count(Key))
      It = Stores.erase(It);
    else
      ++It;
  }
}
} // namespace

// \p Validated says the caller has just proven \p Func straight-line.
static void forwardLinearPrivateFrameLoads(HighFunc &Func, Arch Architecture,
                                           size_t &Budget,
                                           bool Validated = false) {
  if (Architecture == Arch::Unknown)
    return;
  const uint16_t PointerBytes = getTargetRegInfo(Architecture).PointerSize;
  if ((PointerBytes != 4 && PointerBytes != 8) ||
      (!Validated &&
       !isStraightLinePrivateFrameFunction(Func, Architecture, Budget)))
    return;
  std::map<LocalIdentity, unsigned> DefinitionCounts;
  for (const HighStmt &Stmt : Func.Body)
    if (Stmt.Kind == StmtKind::Assign && Stmt.Dst->Kind == ExprKind::Var)
      ++DefinitionCounts[highSourceLocalIdentity(Stmt.Dst->Var)];

  // Learn an immutable frame alias only after its assignment.  Prologue
  // spills can precede a frame-pointer assignment, so requiring a contiguous
  // alias-only prefix loses otherwise exact private-frame accesses.  Following
  // statement order also keeps an alias unavailable to earlier reads.
  FrameAliases Aliases;
  const auto Alias = [&](const MedVar &V) -> ExprPtr {
    auto It = Aliases.find(highSourceLocalIdentity(V));
    return It == Aliases.end() ? nullptr : It->second;
  };

  size_t ExpansionBudget =
      ::neverd::limits::ir::kHighFrameStoreForwardingFrameWalkBudget;
  std::map<int64_t, StoredFrameValue> Stores;
  struct PendingStore {
    uint16_t Bytes;
    HighStmt *Statement;
  };
  std::map<int64_t, PendingStore> PendingStores;
  const auto Anchor = [](HighStmt &Statement) {
    const auto Address = Statement.Addr;
    Statement = HighStmt{};
    Statement.Kind = StmtKind::Block;
    Statement.Addr = Address;
  };
  for (HighStmt &Stmt : Func.Body) {
    if (!Budget)
      break;
    const bool Calls = statementCall(Stmt) != nullptr;
    forEachRhsExpr(Stmt, [&](ExprPtr &E) {
      E = replaceFrameLoads(E, Stores, Func, Architecture, Budget,
                            ExpansionBudget, Aliases);
    });
    if (Calls)
      Stores.clear();
    // Within this effect-free region, a complete overwrite makes an earlier
    // unobserved write dead, even if another region lets an object escape.
    // Never carry this proof across a call, join or unknown memory read.
    forEachRhsExpr(Stmt, [&](const ExprPtr &Root) {
      if (!PendingStores.empty() &&
          (!discardableIntegerValue(Root, Budget) || readsStackHome(Root)))
        PendingStores.clear();
    });
    if (Stmt.Kind == StmtKind::Store) {
      const uint16_t Bytes =
          Stmt.StoreVal->Type ? Stmt.StoreVal->Type->Size : 0;
      const auto At = privateFrameOffset(Stmt.StoreAddr, Bytes, Func,
                                         Architecture, Budget, Aliases);
      const bool Redundant =
          recordFrameStore(Stmt.StoreAddr, Stmt.StoreVal,
                           Stmt.StoreVal ? Stmt.StoreVal->Type : nullptr,
                           Stores, Func, Architecture, Budget, Aliases);
      if (Redundant && !Stmt.Dst && !Stmt.Val && !Stmt.Cond && !Stmt.RetVal &&
          !Stmt.CallExpr && !Stmt.SwitchExpr) {
        Anchor(Stmt);
      } else {
        if (!At)
          PendingStores.clear();
        else {
          for (auto I = PendingStores.begin(); I != PendingStores.end();) {
            if (!rangesOverlap(I->first, I->second.Bytes, *At, Bytes)) {
              ++I;
              continue;
            }
            if (I->first == *At && I->second.Bytes == Bytes && Budget)
              Anchor(*I->second.Statement);
            I = PendingStores.erase(I);
          }
          if (discardableIntegerValue(Stmt.StoreVal, Budget))
            PendingStores.emplace(*At, PendingStore{Bytes, &Stmt});
        }
      }
    } else if (Stmt.Kind == StmtKind::Assign &&
               Stmt.Dst->Kind == ExprKind::Load) {
      PendingStores.clear();
      if (Stmt.Dst->Operands.size() != 1)
        Stores.clear();
      else
        recordFrameStore(Stmt.Dst->Operands[0], Stmt.Val, Stmt.Dst->Type,
                         Stores, Func, Architecture, Budget, Aliases);
    } else if (Stmt.Kind == StmtKind::Assign) {
      // A Stack variable denotes a mutable memory home, not a scalar local.
      // Its source-level write may alias any recorded private access.
      if (Stmt.Dst->Var.Kind == MedVar::Stack)
        Stores.clear();
      if (Stmt.Dst->Var.Kind == MedVar::Stack)
        PendingStores.clear();
      invalidateWrittenVariable(Stores, Stmt.Dst->Var);
      // Reading a complete cell into a scalar captures its current bits. Use
      // that scalar for later reloads and redundant writebacks, never replay
      // the original memory read. A subsequent local write invalidates this
      // fact through the ordinary dependency mechanism above.
      if (Stmt.Dst->Var.Kind != MedVar::Stack && Stmt.Val &&
          Stmt.Val->Kind == ExprKind::Load && Stmt.Val->Operands.size() == 1 &&
          isForwardableScalar(Stmt.Dst->Type) &&
          isForwardableScalar(Stmt.Val->Type) &&
          Stmt.Dst->Type->Size == Stmt.Val->Type->Size &&
          privateFrameOffset(Stmt.Val->Operands[0], Stmt.Val->Type->Size, Func,
                             Architecture, Budget, Aliases))
        recordFrameStore(Stmt.Val->Operands[0], Stmt.Dst, Stmt.Val->Type,
                         Stores, Func, Architecture, Budget, Aliases);
      if (Stmt.Dst->Var.Kind != MedVar::Stack && Stmt.Dst->Type &&
          Stmt.Dst->Type->Size == PointerBytes &&
          Stmt.Dst->Var.Size == PointerBytes &&
          DefinitionCounts[highSourceLocalIdentity(Stmt.Dst->Var)] == 1 &&
          high_detail::frameAddressOffset(Stmt.Val, Func, Architecture, Budget,
                                          0, Alias))
        Aliases.emplace(highSourceLocalIdentity(Stmt.Dst->Var), Stmt.Val);
    }
  }
}

namespace {
// Value numbers denote captured bits, never expressions to be re-evaluated.
// This extends the local writeback proof across acyclic source joins without
// assuming that an escaping pointer exposes only part of the native frame.
class FrameWritebacks {
  struct Value {
    size_t Number = 0;
    uint16_t Bytes = 0;
    bool operator==(const Value &) const = default;
  };
  struct State {
    std::map<LocalIdentity, Value> Locals;
    std::map<LocalIdentity, int64_t> Addresses;
    std::map<int64_t, Value> Memory;
    size_t size() const {
      return Locals.size() + Addresses.size() + Memory.size();
    }
  };
  HighFunc &Function;
  Arch Architecture;
  size_t &Budget;
  const std::set<va_t> &Labels;
  bool Complete = true;
  size_t NextValue = 1;
  ExprPtr EntryStack;
  std::map<int64_t, ExprPtr> OffsetExpressions;
  std::map<std::pair<uint16_t, uint64_t>, Value> Constants;
  std::map<const HighStmt *, bool> Redundant;
  std::map<LocalIdentity, ExprPtr> LocalExpressions;
  std::map<const HighStmt *, State> Before;

  bool spend(size_t Count = 1) {
    if (Count > Budget) {
      Complete = false;
      Budget = 0;
      return false;
    }
    Budget -= Count;
    return true;
  }

  std::optional<int64_t> address(const ExprPtr &E, const State &S) {
    const auto Alias = [&](const MedVar &V) -> ExprPtr {
      const auto It = S.Addresses.find(highSourceLocalIdentity(V));
      if (It == S.Addresses.end() || !EntryStack)
        return nullptr;
      auto [Cached, Fresh] = OffsetExpressions.try_emplace(It->second);
      if (Fresh) {
        const auto Bytes = getTargetRegInfo(Architecture).PointerSize;
        const uint64_t Bits =
            Bytes == 4 ? uint64_t(uint32_t(It->second)) : uint64_t(It->second);
        Cached->second = HighExpr::makeBinop(NdOp::INT_ADD, EntryStack,
                                             HighExpr::makeConst(Bits, Bytes));
      }
      return Cached->second;
    };
    auto Result = high_detail::frameAddressOffset(E, Function, Architecture,
                                                  Budget, 0, Alias);
    Complete &= Budget != 0;
    return Result;
  }

  std::optional<int64_t> slot(const ExprPtr &E, uint16_t Bytes,
                              const State &S) {
    const auto At = address(E, S);
    if (!At || !Bytes || *At < -Function.FrameSize || *At >= 0 ||
        uint64_t(Bytes) > uint64_t(-*At))
      return std::nullopt;
    return At;
  }

  Value value(const ExprPtr &E, State &S, unsigned Depth = 0) {
    if (!spend() || !E || !isForwardableScalar(E->Type) ||
        Depth > limits::ir::kMaxPrivateFrameLeafDepth ||
        E->MemoryOrdering != NdMemoryOrdering::None ||
        E->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        E->IntrinsicId != Intrinsic::None || !E->IntrinsicOutputs.empty() ||
        E->IndirectTarget)
      return {};
    const uint16_t Bytes = E->Type->Size;
    if (E->Kind == ExprKind::Var && E->Operands.empty() &&
        E->Var.Size == Bytes) {
      const auto It = S.Locals.find(highSourceLocalIdentity(E->Var));
      return It == S.Locals.end() ? Value{} : It->second;
    }
    if (E->Kind == ExprKind::Const && replayableIntegerNode(*E)) {
      auto [It, Fresh] = Constants.try_emplace({Bytes, E->ConstVal});
      if (Fresh)
        It->second = {NextValue++, Bytes};
      return It->second;
    }
    if ((E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast) &&
        E->Operands.size() == 1 && E->Operands[0] &&
        isForwardableInteger(E->Type) &&
        isForwardableInteger(E->Operands[0]->Type) &&
        E->Operands[0]->Type->Size == Bytes &&
        (E->Kind == ExprKind::BitCast || replayableIntegerNode(*E)))
      return value(E->Operands[0], S, Depth + 1);
    if (E->Kind == ExprKind::Load && E->Operands.size() == 1)
      if (const auto At = slot(E->Operands[0], Bytes, S)) {
        auto &Stored = S.Memory[*At];
        if (!Stored.Number || Stored.Bytes != Bytes)
          Stored = {NextValue++, Bytes};
        return Stored;
      }
    return {};
  }

  template <typename Key, typename Fact>
  void intersect(std::map<Key, Fact> &Into, const std::map<Key, Fact> &From) {
    for (auto It = Into.begin(); It != Into.end();) {
      if (!spend())
        return;
      const auto Other = From.find(It->first);
      if (Other == From.end() || !(Other->second == It->second))
        It = Into.erase(It);
      else
        ++It;
    }
  }

  void transfer(const HighSourceFlowNode &Node, State &S) {
    const HighStmt *T = Node.Statement;
    if (T) {
      if (!spend(1 + S.size()))
        return;
      auto [It, Fresh] = Before.try_emplace(T, S);
      if (!Fresh) {
        intersect(It->second.Locals, S.Locals);
        intersect(It->second.Addresses, S.Addresses);
        intersect(It->second.Memory, S.Memory);
      }
    }
    std::vector<ExprPtr> Pending;
    if (Node.Test)
      Pending.push_back(Node.Test);
    else if (T)
      forEachRhsExpr(*T, [&](const ExprPtr &E) { Pending.push_back(E); });
    bool Barrier =
        T && (T->MemoryOrdering != NdMemoryOrdering::None ||
              T->MemoryAddressSpace != NdMemoryAddressSpace::Default);
    HighExprSet Seen;
    std::vector<MedVar> Writes;
    while (!Pending.empty() && spend()) {
      const auto E = Pending.back();
      Pending.pop_back();
      if (!E || !Seen.insert(E.get()).second)
        continue;
      Barrier |= E->Kind == ExprKind::Call || E->Kind == ExprKind::Store ||
                 E->IntrinsicId != Intrinsic::None ||
                 E->Op == NdOp::ATOMIC_ADD || E->Op == NdOp::ATOMIC_XCHG ||
                 E->Op == NdOp::ATOMIC_CMPXCHG ||
                 E->MemoryOrdering != NdMemoryOrdering::None ||
                 E->MemoryAddressSpace != NdMemoryAddressSpace::Default;
      Writes.insert(Writes.end(), E->IntrinsicOutputs.begin(),
                    E->IntrinsicOutputs.end());
      E->forEachChildExpr([&](const ExprPtr &C) { Pending.push_back(C); });
    }
    for (const auto &V : Writes) {
      S.Locals.erase(highSourceLocalIdentity(V));
      S.Addresses.erase(highSourceLocalIdentity(V));
    }
    if (Barrier)
      S.Memory.clear();
    if (!T || Node.Test)
      return;
    if (T->Kind == StmtKind::Assign) {
      if (!T->Dst || T->Dst->Kind != ExprKind::Var ||
          T->Dst->Var.Kind == MedVar::Stack) {
        S.Memory.clear();
        return;
      }
      const auto K = highSourceLocalIdentity(T->Dst->Var);
      auto Captured = Barrier ? Value{} : value(T->Val, S);
      // A local captures one result, including a call or an ordinary load.
      // Remember those bits, never an expression that would perform the call
      // or load again. An undefined value is no stable captured observation.
      if (!Captured.Number && T->Val && T->Val->Kind != ExprKind::Undef &&
          isForwardableScalar(T->Val->Type) && T->Dst->Type &&
          T->Dst->Type->Size == T->Val->Type->Size &&
          T->Dst->Var.Size == T->Val->Type->Size)
        Captured = {NextValue++, T->Val->Type->Size};
      const auto Offset = Barrier ? std::nullopt : address(T->Val, S);
      S.Locals.erase(K);
      S.Addresses.erase(K);
      if (Captured.Number && T->Dst->Type &&
          T->Dst->Type->Size == Captured.Bytes &&
          T->Dst->Var.Size == Captured.Bytes)
        S.Locals[K] = Captured;
      if (Offset && T->Dst->Type &&
          T->Dst->Type->Size == getTargetRegInfo(Architecture).PointerSize &&
          T->Dst->Var.Size == T->Dst->Type->Size)
        S.Addresses[K] = *Offset;
    } else if (T->Kind == StmtKind::Store) {
      const uint16_t Bytes =
          T->StoreVal && T->StoreVal->Type ? T->StoreVal->Type->Size : 0;
      const auto At = slot(T->StoreAddr, Bytes, S);
      if (!At || Barrier) {
        S.Memory.clear();
        Redundant[T] = false;
        return;
      }
      const auto V = value(T->StoreVal, S);
      const auto Old = S.Memory.find(*At);
      const bool Same = V.Number && Old != S.Memory.end() && Old->second == V &&
                        V.Bytes == Bytes;
      auto [Candidate, Fresh] = Redundant.try_emplace(T, Same);
      if (!Fresh)
        Candidate->second &= Same;
      for (auto It = S.Memory.begin(); It != S.Memory.end();) {
        if (!spend())
          return;
        if (rangesOverlap(*At, Bytes, It->first, It->second.Bytes))
          It = S.Memory.erase(It);
        else
          ++It;
      }
      if (V.Number && V.Bytes == Bytes)
        S.Memory[*At] = V;
    }
  }

  ExprPtr replaceCapturedReads(const ExprPtr &Root, const State &S) {
    if (!Root || S.Memory.empty() || S.Locals.empty())
      return Root;
    // All inputs of a statement are observed before its outer call. Nested
    // effects need an evaluation-order proof and stay unchanged.
    std::vector<const HighExpr *> Pending{Root.get()};
    HighExprSet Seen;
    while (!Pending.empty()) {
      const auto *E = Pending.back();
      Pending.pop_back();
      if (!E || !Seen.insert(E).second)
        continue;
      if (!spend())
        return Root;
      if ((E->Kind == ExprKind::Call && E != Root.get()) ||
          E->Kind == ExprKind::Store || E->IntrinsicId != Intrinsic::None ||
          !E->IntrinsicOutputs.empty() ||
          E->MemoryOrdering != NdMemoryOrdering::None ||
          E->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          E->Op == NdOp::ATOMIC_ADD || E->Op == NdOp::ATOMIC_XCHG ||
          E->Op == NdOp::ATOMIC_CMPXCHG)
        return Root;
      E->forEachChildExpr(
          [&](const ExprPtr &C) { Pending.push_back(C.get()); });
    }
    std::map<const HighExpr *, ExprPtr> Rebuilt;
    const auto Visit = [&](auto &&Self, const ExprPtr &E,
                           unsigned Depth) -> ExprPtr {
      if (!E)
        return E;
      if (!spend() || Depth > limits::ir::kMaxPrivateFrameLeafDepth) {
        Complete = false;
        return E;
      }
      if (const auto It = Rebuilt.find(E.get()); It != Rebuilt.end())
        return It->second;
      if (E->Kind == ExprKind::Load && E->Operands.size() == 1 &&
          isForwardableInteger(E->Type))
        if (const auto At = slot(E->Operands[0], E->Type->Size, S)) {
          const auto Stored = S.Memory.find(*At);
          if (Stored != S.Memory.end() && Stored->second.Bytes == E->Type->Size)
            for (const auto &[Local, Bits] : S.Locals) {
              if (!spend())
                return E;
              if (Bits != Stored->second)
                continue;
              const auto Source = LocalExpressions.find(Local);
              if (Source == LocalExpressions.end() ||
                  !isForwardableInteger(Source->second->Type) ||
                  Source->second->Type->Size != E->Type->Size)
                continue;
              ExprPtr Result = Source->second;
              if (!sameIntegerType(Result->Type, E->Type)) {
                auto View = std::make_shared<HighExpr>();
                View->Kind = ExprKind::Cast;
                View->Type = View->CastTo = E->Type;
                View->Operands = {Result};
                Result = std::move(View);
              }
              Rebuilt[E.get()] = Result;
              return Result;
            }
        }
      auto Copy = std::make_shared<HighExpr>(*E);
      bool Changed = false;
      for (auto &Child : Copy->Operands) {
        auto Value = Self(Self, Child, Depth + 1);
        Changed |= Value != Child;
        Child = std::move(Value);
      }
      if (Copy->IndirectTarget) {
        auto Value = Self(Self, Copy->IndirectTarget, Depth + 1);
        Changed |= Value != Copy->IndirectTarget;
        Copy->IndirectTarget = std::move(Value);
      }
      auto Result = Changed ? Copy : E;
      Rebuilt[E.get()] = Result;
      return Result;
    };
    return Visit(Visit, Root, 0);
  }

  // Complete cells overwritten before their next possible observation.
  // Unknown calls/reads revoke this proof; an escaped frame need not disable
  // unrelated earlier overwrites. Exits start empty, so this does not assume
  // that an escaped cell becomes dead merely because the function returns.
  void
  findOverwrittenStores(const HighSourceFlowGraph &Graph,
                        const std::vector<size_t> &Order,
                        const std::map<const HighStmt *, HighStmt> &Rewritten,
                        std::map<const HighStmt *, bool> &Overwritten) {
    std::vector<std::map<int64_t, uint16_t>> Dead(Graph.Nodes.size());
    for (auto I = Order.rbegin(); I != Order.rend() && Complete; ++I) {
      const auto &Node = Graph.Nodes[*I];
      auto &Bytes = Dead[*I];
      bool First = true;
      for (size_t Next : Node.Successors) {
        if (!spend(1 + Bytes.size() + Dead[Next].size()))
          return;
        if (First) {
          Bytes = Dead[Next];
          First = false;
        } else {
          std::erase_if(Bytes, [&](const auto &Cell) {
            const auto Other = Dead[Next].find(Cell.first);
            return Other == Dead[Next].end() || Other->second != Cell.second;
          });
        }
      }
      const auto StateIt = Before.find(Node.Statement);
      const auto Rewrite = Rewritten.find(Node.Statement);
      if (Node.Statement &&
          (StateIt == Before.end() || Rewrite == Rewritten.end())) {
        Bytes.clear();
        continue;
      }
      const State EmptyState;
      const HighStmt EmptyStatement;
      const auto &S = StateIt == Before.end() ? EmptyState : StateIt->second;
      const auto &T =
          Rewrite == Rewritten.end() ? EmptyStatement : Rewrite->second;
      if (T.MemoryOrdering != NdMemoryOrdering::None ||
          T.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
        Bytes.clear();
        continue;
      }
      // A writeback already proved redundant will disappear in this same
      // commit. It cannot also justify deleting the store it repeats.
      if (const auto Removed = Redundant.find(Node.Statement);
          Removed != Redundant.end() && Removed->second)
        continue;
      if (!Node.Test && T.Kind == StmtKind::Store) {
        const uint16_t Size =
            T.StoreVal && T.StoreVal->Type ? T.StoreVal->Type->Size : 0;
        const auto At = slot(T.StoreAddr, Size, S);
        if (!At || !spend(1 + Bytes.size())) {
          Bytes.clear();
        } else {
          const auto Later = Bytes.find(*At);
          const bool Covered = Later != Bytes.end() && Later->second == Size;
          const bool Discard =
              Covered && discardableIntegerValue(T.StoreVal, Budget);
          Complete &= Budget != 0;
          auto [Found, Fresh] =
              Overwritten.try_emplace(Node.Statement, Discard);
          if (!Fresh)
            Found->second &= Discard;
          std::erase_if(Bytes, [&](const auto &Cell) {
            return rangesOverlap(*At, Size, Cell.first, Cell.second);
          });
          Bytes[*At] = Size;
        }
      } else if (!Node.Test && T.Kind == StmtKind::Assign &&
                 (!T.Dst || T.Dst->Kind != ExprKind::Var ||
                  T.Dst->Var.Kind == MedVar::Stack)) {
        Bytes.clear();
      }
      std::vector<ExprPtr> Pending;
      if (Node.Test)
        Pending.push_back(Node.Test);
      else
        forEachRhsExpr(T, [&](const ExprPtr &E) { Pending.push_back(E); });
      HighExprSet Seen;
      while (!Pending.empty() && !Bytes.empty() && spend()) {
        const auto E = Pending.back();
        Pending.pop_back();
        if (!E || !Seen.insert(E.get()).second)
          continue;
        if (E->Kind == ExprKind::Call || E->Kind == ExprKind::Store ||
            E->IntrinsicId != Intrinsic::None || !E->IntrinsicOutputs.empty() ||
            E->MemoryOrdering != NdMemoryOrdering::None ||
            E->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            E->Op == NdOp::ATOMIC_ADD || E->Op == NdOp::ATOMIC_XCHG ||
            E->Op == NdOp::ATOMIC_CMPXCHG ||
            (E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Stack)) {
          Bytes.clear();
          break;
        }
        if ((E->Kind == ExprKind::BinOp || E->Kind == ExprKind::UnaryOp ||
             E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast) &&
            !discardableIntegerValue(E, Budget)) {
          Complete &= Budget != 0;
          Bytes.clear();
          break;
        }
        if (E->Kind == ExprKind::Load) {
          const auto At = slot(E->Operands[0], E->Type->Size, S);
          if (!At) {
            Bytes.clear();
            break;
          }
          if (!spend(Bytes.size()))
            return;
          std::erase_if(Bytes, [&](const auto &Cell) {
            return rangesOverlap(*At, E->Type->Size, Cell.first, Cell.second);
          });
        }
        E->forEachChildExpr([&](const ExprPtr &C) { Pending.push_back(C); });
      }
    }
  }

public:
  /// \p L are the goto targets in \p F.
  FrameWritebacks(HighFunc &F, Arch A, size_t &B, const std::set<va_t> &L)
      : Function(F), Architecture(A), Budget(B), Labels(L) {}

  /// \returns whether a proven rewrite changed the function.
  bool run() {
    // Nothing commits without budget or a frame address; learn both before
    // building the flow graph, which none of the checks below needs. A cyclic
    // graph never commits below: decide that from statements alone first.
    if (!Budget || entersCycle(Function.Body, Labels))
      return false;
    State Entry;
    HighExprSet Seen;
    walkStmts(Function.Body, [&](const HighStmt &T) {
      // The graph owns edges, not every statement payload. Do not discard
      // extra effects or children on a malformed store while keeping its label.
      if (T.Kind == StmtKind::Store &&
          (T.Dst || T.Val || T.Cond || T.RetVal || T.CallExpr || T.SwitchExpr ||
           !T.Body.empty() || !T.ElseBody.empty() || !T.Cases.empty() ||
           !T.DefaultBody.empty() || !T.EHClauses.empty() ||
           !T.EHClauseBodies.empty()))
        Complete = false;
      if (T.Kind == StmtKind::Block &&
          (T.Dst || T.Val || T.Cond || T.RetVal || T.StoreAddr || T.StoreVal ||
           T.CallExpr || T.SwitchExpr || !T.ElseBody.empty() ||
           !T.Cases.empty() || !T.DefaultBody.empty() || !T.EHClauses.empty() ||
           !T.EHClauseBodies.empty()))
        Complete = false;
      forEachExpr(T, [&](const ExprPtr &Root) {
        std::vector<ExprPtr> Pending{Root};
        while (!Pending.empty() && spend()) {
          const auto E = Pending.back();
          Pending.pop_back();
          if (!E || !Seen.insert(E.get()).second)
            continue;
          if (!hasValidShape(*E))
            Complete = false;
          // An address-taken source local could change without an assignment.
          if (E->Kind == ExprKind::Addr)
            Complete = false;
          if (E->Kind == ExprKind::Var) {
            if (E->Var.Size && E->Type && E->Var.Size == E->Type->Size &&
                E->Var.Kind != MedVar::Stack &&
                E->Var.Kind != MedVar::Unspecified)
              LocalExpressions.try_emplace(highSourceLocalIdentity(E->Var), E);
            if (isSyntheticEntryStackPointer(E->Var, Function, Architecture))
              EntryStack = E;
            if (E->Var.Kind == MedVar::Param && E->Var.SSAVer == 0 &&
                E->Var.RenameTag < 0 && isForwardableScalar(E->Type) &&
                E->Var.Id >= 0 && size_t(E->Var.Id) < Function.Params.size() &&
                E->Var.Size == E->Type->Size)
              Entry.Locals.try_emplace(highSourceLocalIdentity(E->Var),
                                       Value{NextValue++, E->Type->Size});
          }
          E->forEachChildExpr([&](const ExprPtr &C) { Pending.push_back(C); });
        }
      });
    });
    if (!Complete || !EntryStack)
      return false;
    const auto Graph = buildHighSourceFlowGraph(Function);
    if (!Graph.Diagnostics.Complete || !Graph.Diagnostics.Items.empty() ||
        Graph.Nodes.empty())
      return false;
    std::vector<bool> Reachable(Graph.Nodes.size());
    std::vector<size_t> Degree(Graph.Nodes.size()), Work{Graph.Entry};
    size_t Total = 0;
    while (!Work.empty() && spend()) {
      const size_t I = Work.back();
      Work.pop_back();
      if (Reachable[I])
        continue;
      Reachable[I] = true;
      ++Total;
      for (size_t Next : Graph.Nodes[I].Successors) {
        ++Degree[Next];
        Work.push_back(Next);
      }
    }
    if (!Complete || Degree[Graph.Entry])
      return false;
    std::vector<std::optional<State>> Incoming(Graph.Nodes.size());
    Incoming[Graph.Entry] = std::move(Entry);
    Work = {Graph.Entry};
    size_t Visited = 0;
    std::vector<size_t> Order;
    while (!Work.empty() && Complete) {
      const size_t I = Work.back();
      Work.pop_back();
      if (!Incoming[I] || !spend(1 + Incoming[I]->size()))
        return false;
      State S = std::move(*Incoming[I]);
      Incoming[I].reset();
      ++Visited;
      Order.push_back(I);
      transfer(Graph.Nodes[I], S);
      for (size_t Next : Graph.Nodes[I].Successors) {
        if (!spend(1 + S.size()))
          return false;
        if (Incoming[Next]) {
          intersect(Incoming[Next]->Locals, S.Locals);
          intersect(Incoming[Next]->Addresses, S.Addresses);
          intersect(Incoming[Next]->Memory, S.Memory);
        } else {
          Incoming[Next] = S;
        }
        if (!--Degree[Next])
          Work.push_back(Next);
      }
    }
    // A cycle, unresolved edge or exhausted proof cannot commit partial edits.
    if (!Complete || Visited != Total)
      return false;
    std::map<const HighStmt *, HighStmt> Rewritten;
    for (const auto &[Statement, S] : Before) {
      HighStmt Copy = *Statement;
      forEachRhsExpr(Copy, [&](ExprPtr &E) {
        // A do-while test runs after the body, not in the state at its entry.
        if (Copy.Kind != StmtKind::DoWhile || &E != &Copy.Cond)
          E = replaceCapturedReads(E, S);
      });
      Rewritten.emplace(Statement, std::move(Copy));
      if (!Complete)
        return false;
    }
    std::map<const HighStmt *, bool> Overwritten;
    findOverwrittenStores(Graph, Order, Rewritten, Overwritten);
    if (!Complete)
      return false;
    bool Changed = false;
    walkStmts(Function.Body, [&](HighStmt &T) {
      if (const auto Rewrite = Rewritten.find(&T); Rewrite != Rewritten.end()) {
        // Keep child vectors in place: graph statement identities borrow them.
        auto &R = Rewrite->second;
        Changed |= T.Val != R.Val || T.Cond != R.Cond || T.RetVal != R.RetVal ||
                   T.StoreAddr != R.StoreAddr || T.StoreVal != R.StoreVal ||
                   T.CallExpr != R.CallExpr || T.SwitchExpr != R.SwitchExpr;
        T.Val = std::move(R.Val);
        T.Cond = std::move(R.Cond);
        T.RetVal = std::move(R.RetVal);
        T.StoreAddr = std::move(R.StoreAddr);
        T.StoreVal = std::move(R.StoreVal);
        T.CallExpr = std::move(R.CallExpr);
        T.SwitchExpr = std::move(R.SwitchExpr);
      }
      const auto It = Redundant.find(&T);
      const auto Dead = Overwritten.find(&T);
      if ((It == Redundant.end() || !It->second) &&
          (Dead == Overwritten.end() || !Dead->second))
        return;
      const va_t Address = T.Addr;
      T = HighStmt{};
      T.Kind = StmtKind::Block;
      T.Addr = Address;
      Changed = true;
    });
    return Changed;
  }
};
} // namespace

void forwardPrivateFrameLoads(HighFunc &Func, Arch Architecture) {
  forwardPrivateFrameLoads(Func, Architecture, /*AcrossJoins=*/true);
}

bool forwardPrivateFrameLoads(HighFunc &Func, Arch Architecture,
                              bool AcrossJoins) {
  if (Architecture == Arch::Unknown || Func.FrameSize <= 0 ||
      Func.StructuredExceptionRegions || Func.UnstructuredExceptionRegions)
    return false;
  // Keep the existing exact, straight-line proof. A call or source join ends
  // its facts; it need not disable unrelated regions of the entire function.
  // In particular, no memory fact is carried into or out of a loop.
  size_t Remaining =
      ::neverd::limits::ir::kHighFrameStoreForwardingFrameWalkBudget;
  HighFunc Context;
  Context.FrameSize = Func.FrameSize;
  Context.FrameHeadroom = Func.FrameHeadroom;
  const auto Entries = gotoTargets(Func.Body);
  // A single-definition entry-prefix frame alias dominates every later
  // region. Preserve that address proof across calls, without preserving any
  // stored bytes. This is the same entry-prefix rule used by private-frame
  // dead-store analysis; aliases discovered inside a branch remain local.
  std::map<LocalIdentity, unsigned> Definitions;
  bool UnsupportedFrame = false;
  std::set<LocalIdentity> AddressTaken;
  HighExprSet Observed;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (!Remaining)
      return;
    --Remaining;
    UnsupportedFrame |= !S.EHClauses.empty() || !S.EHClauseBodies.empty();
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var)
      ++Definitions[highSourceLocalIdentity(S.Dst->Var)];
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var &&
        isSyntheticEntryStackPointer(S.Dst->Var, Func, Architecture))
      UnsupportedFrame = true;
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Pending{Root};
      while (!Pending.empty() && Remaining) {
        auto E = Pending.back();
        Pending.pop_back();
        --Remaining;
        if (!E || !Observed.insert(E.get()).second)
          continue;
        if (E->Kind == ExprKind::Addr && E->Operands.size() == 1 &&
            E->Operands[0] && E->Operands[0]->Kind == ExprKind::Var)
          AddressTaken.insert(highSourceLocalIdentity(E->Operands[0]->Var));
        E->forEachChildExpr(
            [&](const ExprPtr &Child) { Pending.push_back(Child); });
      }
    });
  });
  if (UnsupportedFrame || !Remaining)
    return false;
  FrameAliases DominatingAliases;
  const auto Alias = [&](const MedVar &V) -> ExprPtr {
    const auto I = DominatingAliases.find(highSourceLocalIdentity(V));
    return I == DominatingAliases.end() ? nullptr : I->second;
  };
  std::vector<HighStmt> Prefix;
  for (const auto &S : Func.Body) {
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val ||
        S.Dst->Kind != ExprKind::Var || S.Dst->Var.Kind == MedVar::Stack ||
        !S.Dst->Type ||
        S.Dst->Type->Size != getTargetRegInfo(Architecture).PointerSize ||
        S.Dst->Var.Size != S.Dst->Type->Size ||
        AddressTaken.count(highSourceLocalIdentity(S.Dst->Var)) ||
        Definitions[highSourceLocalIdentity(S.Dst->Var)] != 1 ||
        !high_detail::frameAddressOffset(S.Val, Func, Architecture, Remaining,
                                         0, Alias))
      break;
    DominatingAliases.emplace(highSourceLocalIdentity(S.Dst->Var), S.Val);
    Prefix.push_back(S);
  }
  // Every region below starts with this prefix: prove it once.
  bool PrefixValid = Prefix.empty();
  if (!PrefixValid) {
    size_t Budget = Remaining;
    PrefixValid = isStraightLinePrivateFrameFunction(Prefix, Context,
                                                     Architecture, Budget);
    Remaining = Budget;
  }
  const auto Forward = [&](auto &&Self, std::vector<HighStmt> &Body,
                           unsigned Depth) -> void {
    if (Depth > ::neverd::limits::ir::kMaxPrivateFrameLeafDepth)
      return;
    size_t First = 0;
    const auto Flush = [&](size_t End, HighStmt *Call = nullptr) {
      if (End <= First || !Remaining)
        return;
      HighFunc Region = Context;
      const bool AtEntry = &Body == &Func.Body && First == 0;
      if (!AtEntry)
        Region.Body = Prefix;
      const size_t PrefixCount = Region.Body.size();
      Region.Body.insert(Region.Body.end(), Body.begin() + First,
                         Body.begin() + End);
      const size_t Count = Region.Body.size();
      if (Count > Remaining) {
        Remaining = 0;
        return;
      }
      Remaining -= Count;
      // A call's target and arguments are evaluated before the call. Expose
      // only those pure input expressions to the same proof, then discard all
      // memory facts. Nested effects cause validation to reject this region.
      ExprPtr *Root = nullptr;
      if (Call) {
        if (Call->Kind == StmtKind::Assign)
          Root = &Call->Val;
        else if (Call->Kind == StmtKind::Call)
          Root = &Call->CallExpr;
        else if (Call->Kind == StmtKind::ExprStmt)
          Root = &Call->Val;
        else if (Call->Kind == StmtKind::If || Call->Kind == StmtKind::IfElse)
          Root = &Call->Cond;
        else if (Call->Kind == StmtKind::Switch)
          Root = &Call->SwitchExpr;
      }
      std::vector<ExprPtr *> Inputs;
      if (Root && *Root && (*Root)->Kind == ExprKind::Call) {
        for (auto &Operand : (*Root)->Operands)
          Inputs.push_back(&Operand);
        if ((*Root)->IndirectTarget)
          Inputs.push_back(&(*Root)->IndirectTarget);
      } else if (Root && *Root) {
        Inputs.push_back(Root);
      }
      for (auto *Input : Inputs) {
        HighStmt Read;
        Read.Kind = StmtKind::ExprStmt;
        Read.Val = *Input;
        Region.Body.push_back(std::move(Read));
      }
      // If a call input contains an effect, still optimize the preceding
      // region, but never speculate about argument evaluation order.
      // Each statement of the region already passed this proof alone, and the
      // prefix passed it above: only the exposed call inputs remain.
      size_t Budget = Remaining;
      const bool Valid =
          (AtEntry || PrefixValid) &&
          isStraightLinePrivateFrameFunction(
              llvm::ArrayRef<HighStmt>(Region.Body).drop_front(Count), Region,
              Architecture, Budget);
      if (!Valid) {
        Region.Body.resize(Count);
        Inputs.clear();
      }
      Remaining = Budget;
      // A region the proof above accepted is not proven again.
      forwardLinearPrivateFrameLoads(Region, Architecture, Remaining, Valid);
      for (size_t I = PrefixCount; I < Count; ++I)
        Body[First + I - PrefixCount] = std::move(Region.Body[I]);
      if (Root && !Inputs.empty()) {
        if ((*Root)->Kind != ExprKind::Call) {
          *Root = Region.Body[Count].Val;
          return;
        }
        // A no-op must retain the call's instruction navigation identity.
        if (std::equal(Inputs.begin(), Inputs.end(),
                       Region.Body.begin() + Count,
                       [](const ExprPtr *Input, const HighStmt &Read) {
                         return *Input == Read.Val;
                       }))
          return;
        // Expressions can be shared by source-map observations or other
        // statements. Rebuild only changed calls instead of mutating their
        // shared node; unchanged calls retain their observed identity.
        auto Rebuilt = std::make_shared<HighExpr>(**Root);
        size_t I = Count;
        for (auto &Operand : Rebuilt->Operands)
          Operand = Region.Body[I++].Val;
        if (Rebuilt->IndirectTarget)
          Rebuilt->IndirectTarget = Region.Body[I].Val;
        *Root = std::move(Rebuilt);
      }
    };
    for (size_t I = 0; I < Body.size() && Remaining; ++I) {
      auto &S = Body[I];
      if (Entries.count(S.Addr) && I > First) {
        Flush(I);
        First = I;
      }
      // Reuse the original shape/effect validator on this individual
      // statement, in place: a compound statement fails without its children
      // being copied. A failed proof is a boundary, never permission to
      // forward.
      size_t Budget = Remaining;
      const bool Plain = isStraightLinePrivateFrameFunction(
          llvm::ArrayRef<HighStmt>(S), Context, Architecture, Budget);
      Remaining = Budget;
      if (Plain && S.Kind == StmtKind::Return) {
        Flush(I + 1);
        First = I + 1;
        continue;
      }
      if (Plain)
        continue;
      Flush(I, &S);
      First = I + 1;
      Self(Self, S.Body, Depth + 1);
      Self(Self, S.ElseBody, Depth + 1);
      Self(Self, S.DefaultBody, Depth + 1);
      for (auto &Case : S.Cases)
        Self(Self, Case.Body, Depth + 1);
    }
    Flush(Body.size());
  };
  Forward(Forward, Func.Body, 0);
  return AcrossJoins &&
         FrameWritebacks(Func, Architecture, Remaining, Entries).run();
}
} // namespace neverd
