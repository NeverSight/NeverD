//===- HighFrameStoreForwarding.cpp - Safe frame load forwarding ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// Forward exact private frame reads before semantic expression simplification.
/// Facts describe source locals after renaming, and only remain valid while
/// their inputs and every byte of the stored value remain unchanged.
///
//===----------------------------------------------------------------------===//

#include "HighDCEDetail.h"
#include "HighFrameAddress.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighSourceFlow.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neverd {
namespace {

constexpr size_t kMaxForwardValueNodes = 1024;
constexpr size_t kMaxForwardExpansionNodes = 4096;
constexpr size_t kFrameWalkBudget = 100000;
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

bool sameIntegerType(const TypeRef &Left, const TypeRef &Right) {
  return isForwardableInteger(Left) && isForwardableInteger(Right) &&
         Left->Size == Right->Size && Left->IsSigned == Right->IsSigned;
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
  if (!isForwardableInteger(E.Type) || E.IndirectTarget ||
      E.IntrinsicId != Intrinsic::None || !E.IntrinsicOutputs.empty() ||
      E.MemoryOrdering != NdMemoryOrdering::None ||
      E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  for (const auto &Child : E.Operands)
    if (!Child || !isForwardableInteger(Child->Type))
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
    return E.Operands.size() == 1 && sameIntegerType(E.Type, E.CastTo);
  case ExprKind::UnaryOp:
    if (E.Operands.size() != 1)
      return false;
    if (E.Op == NdOp::INT_ZEXT || E.Op == NdOp::INT_SEXT)
      return E.Type->Size >= E.Operands[0]->Type->Size;
    return (E.Op == NdOp::INT_NOT || E.Op == NdOp::INT_NEGATE ||
            E.Op == NdOp::INT_NEG2) &&
           E.Type->Size == E.Operands[0]->Type->Size;
  case ExprKind::BinOp:
    if (E.Operands.size() != 2 || E.Type->Size != E.Operands[0]->Type->Size)
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
  std::unordered_set<const HighExpr *> Active;
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
      if (ChildCost > kMaxForwardValueNodes - Cost)
        return false;
      Cost += ChildCost;
    }
    Costs.emplace(E.get(), Cost);
  }
  NodeCount = Costs.at(Root.get());
  return true;
}

// Prove the effect boundary once, with an acyclic, bounded graph walk. A call,
// atomic operation or address escape can invalidate private-memory facts even
// when it occurs inside an otherwise ordinary assignment.
bool isStraightLinePrivateFrameFunction(const HighFunc &Func, Arch Architecture,
                                        size_t &Budget) {
  if (Func.FrameSize <= 0 || Func.StructuredExceptionRegions ||
      Func.UnstructuredExceptionRegions)
    return false;
  std::unordered_set<const HighExpr *> Seen, Active;
  for (const HighStmt &Stmt : Func.Body) {
    if (!Budget || !Stmt.Body.empty() || !Stmt.ElseBody.empty() ||
        !Stmt.Cases.empty() || !Stmt.DefaultBody.empty() ||
        !Stmt.EHClauseBodies.empty() || !Stmt.EHClauses.empty() ||
        Stmt.IsPhiCopy || Stmt.MemoryOrdering != NdMemoryOrdering::None ||
        Stmt.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
    --Budget;
    if (Stmt.Kind != StmtKind::Assign && Stmt.Kind != StmtKind::Store &&
        Stmt.Kind != StmtKind::ExprStmt && Stmt.Kind != StmtKind::Return &&
        Stmt.Kind != StmtKind::Nop)
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
    bool Safe = true;
    forEachExpr(Stmt, [&](const ExprPtr &Root) {
      std::vector<std::pair<ExprPtr, bool>> Work{{Root, false}};
      while (Safe && !Work.empty()) {
        const auto [E, Ready] = Work.back();
        Work.pop_back();
        if (!Budget || !E) {
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
            E->IndirectTarget || E->Kind == ExprKind::Call ||
            E->Kind == ExprKind::Addr || E->Kind == ExprKind::Store ||
            E->Op == NdOp::ATOMIC_ADD || E->Op == NdOp::ATOMIC_XCHG ||
            E->Op == NdOp::ATOMIC_CMPXCHG ||
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
            sameIntegerType(It->second.Type, E->Type)) {
          // A store/load pair is also an integer truncation boundary. Preserve
          // it explicitly when C's integer promotions would widen an operator.
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
      if (R.Cost > kMaxForwardExpansionNodes - Result.Cost ||
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

void recordFrameStore(const ExprPtr &Address, const ExprPtr &Value,
                      const TypeRef &AccessType,
                      std::map<int64_t, StoredFrameValue> &Stores,
                      const HighFunc &Func, Arch Architecture, size_t &Budget,
                      const FrameAliases &Aliases) {
  const uint16_t Bytes = AccessType ? AccessType->Size : uint16_t(0);
  const auto Offset =
      privateFrameOffset(Address, Bytes, Func, Architecture, Budget, Aliases);
  if (!Offset) {
    Stores.clear();
    return;
  }
  for (auto It = Stores.begin(); It != Stores.end();) {
    if (rangesOverlap(It->second.Offset, It->second.Bytes, *Offset, Bytes))
      It = Stores.erase(It);
    else
      ++It;
  }
  if (!Value || !sameIntegerType(AccessType, Value->Type))
    return;
  StoredFrameValue Stored;
  Stored.Offset = *Offset;
  Stored.Bytes = Bytes;
  Stored.Type = AccessType;
  Stored.Value = Value;
  if (safeFrameValue(Value, Stored.Dependencies, Stored.Nodes, Budget))
    Stores.emplace(*Offset, std::move(Stored));
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

void forwardPrivateFrameLoads(HighFunc &Func, Arch Architecture) {
  if (Architecture == Arch::Unknown)
    return;
  const uint16_t PointerBytes = getTargetRegInfo(Architecture).PointerSize;
  size_t Budget = kFrameWalkBudget;
  if ((PointerBytes != 4 && PointerBytes != 8) ||
      !isStraightLinePrivateFrameFunction(Func, Architecture, Budget))
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

  size_t ExpansionBudget = kFrameWalkBudget;
  std::map<int64_t, StoredFrameValue> Stores;
  for (HighStmt &Stmt : Func.Body) {
    if (!Budget)
      break;
    forEachRhsExpr(Stmt, [&](ExprPtr &E) {
      E = replaceFrameLoads(E, Stores, Func, Architecture, Budget,
                            ExpansionBudget, Aliases);
    });
    if (Stmt.Kind == StmtKind::Store) {
      recordFrameStore(Stmt.StoreAddr, Stmt.StoreVal,
                       Stmt.StoreVal ? Stmt.StoreVal->Type : nullptr, Stores,
                       Func, Architecture, Budget, Aliases);
    } else if (Stmt.Kind == StmtKind::Assign &&
               Stmt.Dst->Kind == ExprKind::Load) {
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
      invalidateWrittenVariable(Stores, Stmt.Dst->Var);
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
} // namespace neverd
