//===- HighCDeadStoreAnalysis.cpp - Dead store elimination ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dead store analysis and variable liveness for the HighIR C emitter.
///
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/backend/c/pass/HighC/HighCPasses.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

namespace {

struct ReducedAddr {
  MedVar Base;
  int64_t Offset = 0;
};

auto medVarIdentity(const MedVar &Var) {
  return std::tuple{Var.Kind,   Var.Id,        Var.SSAVer,
                    Var.RegOff, Var.RenameTag, Var.TheArch};
}

// Index the existing identity once. A second definition is ambiguous even
// when it points at the same expression; it must never become a unique alias.
// The global index does not establish dominance. The private-frame proof
// maintains a separate index containing only preceding top-level aliases.
class ExprDefinitions {
  using Key = decltype(medVarIdentity(MedVar{}));
  std::map<Key, const HighExpr *> Values;

public:
  void add(const MedVar &Var, const HighExpr *Value) {
    const auto [It, Added] = Values.try_emplace(medVarIdentity(Var), Value);
    if (!Added)
      It->second = nullptr;
  }

  const HighExpr *findUnique(const MedVar &Var) const {
    const auto It = Values.find(medVarIdentity(Var));
    return It == Values.end() ? nullptr : It->second;
  }
};

template <typename Visitor>
void walkExprNodes(const HighExpr &Root, Visitor &&Visit) {
  HighExprSet Seen;
  llvm::SmallVector<const HighExpr *> Work{&Root};
  while (!Work.empty()) {
    const HighExpr *Expr = Work.back();
    Work.pop_back();
    if (!Seen.insert(Expr).second)
      continue;
    Visit(*Expr);
    if (Expr->IndirectTarget)
      Work.push_back(Expr->IndirectTarget.get());
    for (auto It = Expr->Operands.rbegin(); It != Expr->Operands.rend(); ++It)
      if (*It)
        Work.push_back(It->get());
  }
}

std::optional<int64_t> addOffset(int64_t A, int64_t B) {
  if ((B > 0 && A > std::numeric_limits<int64_t>::max() - B) ||
      (B < 0 && A < std::numeric_limits<int64_t>::min() - B))
    return std::nullopt;
  return A + B;
}

int64_t signedConstant(const HighExpr &Expr) {
  unsigned Bits = Expr.Type ? unsigned(Expr.Type->Size) * 8u : 64u;
  Bits = std::min(Bits, 64u);
  if (Bits == 0 || Bits == 64)
    return static_cast<int64_t>(Expr.ConstVal);
  uint64_t Mask = (uint64_t(1) << Bits) - 1;
  uint64_t Value = Expr.ConstVal & Mask;
  if (Value & (uint64_t(1) << (Bits - 1)))
    Value |= ~Mask;
  return static_cast<int64_t>(Value);
}

std::optional<ReducedAddr> reduceAddr(const HighExpr &Expr,
                                      const ExprDefinitions &Defs,
                                      int64_t Offset = 0, int Depth = 0) {
  if (Depth > limits::kMaxHighCMemoryWalkDepth)
    return std::nullopt;
  if (Expr.Kind == ExprKind::Var) {
    if (const HighExpr *Def = Defs.findUnique(Expr.Var))
      return reduceAddr(*Def, Defs, Offset, Depth + 1);
    return ReducedAddr{Expr.Var, Offset};
  }
  if (Expr.Kind == ExprKind::Cast && Expr.Operands.size() == 1)
    return reduceAddr(*Expr.Operands[0], Defs, Offset, Depth + 1);
  if (Expr.Kind != ExprKind::BinOp || Expr.Operands.size() != 2)
    return std::nullopt;

  const HighExpr &Left = *Expr.Operands[0];
  const HighExpr &Right = *Expr.Operands[1];
  const HighExpr *Base = nullptr;
  int64_t Delta = 0;
  if (Expr.Op == NdOp::INT_ADD && Left.Kind == ExprKind::Const &&
      Right.Kind != ExprKind::Const) {
    Base = &Right;
    Delta = signedConstant(Left);
  } else if (Expr.Op == NdOp::INT_ADD && Right.Kind == ExprKind::Const &&
             Left.Kind != ExprKind::Const) {
    Base = &Left;
    Delta = signedConstant(Right);
  } else if (Expr.Op == NdOp::INT_SUB && Right.Kind == ExprKind::Const &&
             Left.Kind != ExprKind::Const) {
    Base = &Left;
    int64_t Constant = signedConstant(Right);
    if (Constant == std::numeric_limits<int64_t>::min())
      return std::nullopt;
    Delta = -Constant;
  } else {
    return std::nullopt;
  }

  auto NewOffset = addOffset(Offset, Delta);
  return NewOffset ? reduceAddr(*Base, Defs, *NewOffset, Depth + 1)
                   : std::nullopt;
}

std::string reducedAddrKey(const ReducedAddr &Addr) {
  return "reduced:" + std::to_string(static_cast<unsigned>(Addr.Base.Kind)) +
         ":" + std::to_string(Addr.Base.Id) + ":" +
         std::to_string(Addr.Base.SSAVer) + ":" +
         std::to_string(Addr.Base.RegOff) + ":" +
         std::to_string(Addr.Base.RenameTag) + ":" +
         std::to_string(static_cast<unsigned>(Addr.Base.TheArch)) + ":" +
         std::to_string(Addr.Offset);
}

/// A reduced address by its base value's identity and its offset.
using ReducedAddrKey = std::pair<decltype(medVarIdentity(MedVar{})), int64_t>;

ReducedAddrKey keyOf(const ReducedAddr &Addr) {
  return {medVarIdentity(Addr.Base), Addr.Offset};
}

bool matchesReducedAddr(const HighExpr &Expr, const ExprDefinitions &Defs,
                        const std::set<ReducedAddrKey> &Candidates) {
  auto Reduced = reduceAddr(Expr, Defs);
  return Reduced && Candidates.count(keyOf(*Reduced));
}

void collectExprVars(const HighExpr &E, std::set<std::string> &Out,
                     VarNameFn VarFn) {
  walkExprNodes(E, [&](const HighExpr &Ex) {
    if (isNamedValueExpr(Ex))
      Out.insert(VarFn(Ex.Var));
  });
}

void collectLoadAddrVars(const HighExpr &E, std::set<std::string> &Out,
                         VarNameFn VarFn) {
  walkExprNodes(E, [&](const HighExpr &Ex) {
    if (Ex.Kind == ExprKind::Load && !Ex.Operands.empty())
      collectExprVars(*Ex.Operands[0], Out, VarFn);
  });
}

// Memory is observable through a parameter, a global, or an escaped frame
// address even when this function never reloads it. Only private frame slots
// can participate in this emitter's store elimination. Reject unknown aliases,
// partial overlaps, calls, and address escapes rather than infer ownership
// from the absence of a matching load.
bool hasOnlyPrivateFrameMemory(const HighFunc &Func,
                               const ExprDefinitions &Defs) {
  if (Func.FrameSize <= 0)
    return false;
  bool HasNonlocalControl = false;
  walkStmts(Func.Body, [&](const HighStmt &Stmt) {
    HasNonlocalControl |=
        Stmt.Kind == StmtKind::Goto || Stmt.Kind == StmtKind::SEHTry ||
        Stmt.Kind == StmtKind::CxxTry || Stmt.Kind == StmtKind::ItaniumTry;
  });
  if (HasNonlocalControl)
    return false;
  std::vector<std::pair<int64_t, int64_t>> Slots;
  // A global unique definition is not a reaching definition. Only preceding
  // top-level assignments can establish a frame alias for subsequent uses.
  ExprDefinitions DominatingDefs;
  std::set<const HighStmt *> TopLevel;
  for (const auto &Stmt : Func.Body)
    TopLevel.insert(&Stmt);
  auto IsFrameBase = [](const MedVar &Var) {
    return Var.Kind == MedVar::Reg && Var.SSAVer == 0 && Var.RenameTag < 0 &&
           (Var.TheArch == Arch::X64 || Var.TheArch == Arch::X86 ||
            Var.TheArch == Arch::AArch64 || Var.TheArch == Arch::ARM) &&
           Var.Size == getTargetRegInfo(Var.TheArch).PointerSize &&
           Var.RegOff == getTargetRegInfo(Var.TheArch).StackPointer;
  };
  // A value reached again through another use or definition has the answer
  // it had: following each path anew is exponential in shared definitions.
  // Each statement starts afresh, since the addresses a statement may use
  // depend on the definitions before it.
  std::map<std::pair<const HighExpr *, uint16_t>, bool> FullWidthSeen;
  std::map<const HighExpr *, bool> ValueSeen;
  std::function<bool(const HighExpr &, uint16_t, unsigned)> FullWidth =
      [&](const HighExpr &Expr, uint16_t Width, unsigned Depth) {
        if (Depth > limits::kMaxHighCMemoryWalkDepth || !Expr.Type ||
            Expr.Type->Size != Width)
          return false;
        if (const auto Known = FullWidthSeen.find({&Expr, Width});
            Known != FullWidthSeen.end())
          return Known->second;
        bool &Result = FullWidthSeen[{&Expr, Width}];
        if (Expr.Kind == ExprKind::Var) {
          if (Expr.Var.Size != Width)
            return Result = false;
          if (const auto *Def = Defs.findUnique(Expr.Var))
            return Result = FullWidth(*Def, Width, Depth + 1);
        }
        for (const auto &Operand : Expr.Operands)
          if (!Operand || (Operand->Kind != ExprKind::Const &&
                           !FullWidth(*Operand, Width, Depth + 1)))
            return Result = false;
        return Result = true;
      };
  auto CheckAddress = [&](const HighExpr &Address, const TypeRef &Type) {
    const auto Reduced = reduceAddr(Address, DominatingDefs);
    if (!Reduced || !Type || !Type->Size || !IsFrameBase(Reduced->Base) ||
        !FullWidth(Address, Reduced->Base.Size, 0) ||
        Reduced->Offset < -Func.FrameSize ||
        Reduced->Offset > -static_cast<int64_t>(Type->Size))
      return false;
    const std::pair<int64_t, int64_t> Slot{Reduced->Offset,
                                           Reduced->Offset + Type->Size};
    for (const auto &Other : Slots)
      if (Slot != Other && Slot.first < Other.second &&
          Other.first < Slot.second)
        return false;
    Slots.push_back(Slot);
    return true;
  };
  std::function<bool(const HighExpr &, unsigned)> CheckValue =
      [&](const HighExpr &Expr, unsigned Depth) {
        if (Depth > limits::kMaxHighCMemoryWalkDepth ||
            Expr.Kind == ExprKind::Call || Expr.Kind == ExprKind::Addr ||
            Expr.Kind == ExprKind::Store ||
            Expr.MemoryOrdering != NdMemoryOrdering::None ||
            Expr.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          return false;
        if (const auto Known = ValueSeen.find(&Expr); Known != ValueSeen.end())
          return Known->second;
        bool &Result = ValueSeen[&Expr];
        if (Expr.Kind == ExprKind::Load)
          return Result = Expr.Operands.size() == 1 && Expr.Operands[0] &&
                          CheckAddress(*Expr.Operands[0], Expr.Type);
        if (Expr.Kind == ExprKind::Var) {
          if (IsFrameBase(Expr.Var))
            return Result = false;
          if (const auto *Def = Defs.findUnique(Expr.Var))
            return Result = CheckValue(*Def, Depth + 1);
        }
        for (const auto &Operand : Expr.Operands)
          if (!Operand || !CheckValue(*Operand, Depth + 1))
            return Result = false;
        return Result = true;
      };
  bool Valid = true;
  walkStmts(Func.Body, [&](const HighStmt &Stmt) {
    if (!Valid)
      return;
    FullWidthSeen.clear();
    ValueSeen.clear();
    if (Stmt.MemoryOrdering != NdMemoryOrdering::None ||
        Stmt.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
      Valid = false;
      return;
    }
    if (Stmt.Kind == StmtKind::Store) {
      Valid = Stmt.StoreAddr && Stmt.StoreVal &&
              CheckAddress(*Stmt.StoreAddr, Stmt.StoreVal->Type) &&
              CheckValue(*Stmt.StoreVal, 0);
      return;
    }
    if (Stmt.Kind == StmtKind::Assign && Stmt.Dst && Stmt.Val) {
      if (Stmt.Dst->Kind == ExprKind::Load) {
        Valid = CheckValue(*Stmt.Dst, 0) && CheckValue(*Stmt.Val, 0);
        return;
      }
      if (Stmt.Dst->Kind == ExprKind::Var &&
          (Stmt.Dst->Var.Kind == MedVar::Param ||
           (Stmt.Dst->Var.Kind == MedVar::Reg && Stmt.Dst->Var.SSAVer == 0 &&
            Stmt.Dst->Var.RenameTag < 0))) {
        Valid = false;
        return;
      }
      // A canonical frame address can be held in a local temporary. Its
      // consumers below must still use it solely as a memory address.
      const auto Address = reduceAddr(*Stmt.Val, DominatingDefs);
      if (Stmt.Dst->Kind == ExprKind::Var && Address &&
          IsFrameBase(Address->Base)) {
        if (!TopLevel.count(&Stmt) ||
            Defs.findUnique(Stmt.Dst->Var) != Stmt.Val.get()) {
          Valid = false;
          return;
        }
        DominatingDefs.add(Stmt.Dst->Var, Stmt.Val.get());
        return;
      }
    }
    forEachRhsExprAndFilter(Stmt, [&](const ExprPtr &Expr) {
      if (Expr)
        Valid &= CheckValue(*Expr, 0);
    });
  });
  return Valid && !Slots.empty();
}

} // anonymous namespace

void analyzeDeadStores(HighCAnalysisState &State, const HighFunc &Func,
                       VarNameFn VarFn, ExprStrFn ExprFn) {
  State.DeadStmts.clear();
  State.DeadVars.clear();
  State.AddressKeys.clear();
  State.CanElideFrameStores = false;

  bool HasOrderedMemory = false;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.MemoryOrdering != NdMemoryOrdering::None ||
        S.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      HasOrderedMemory = true;
    forEachExprAndFilter(S, [&](const ExprPtr &E) {
      if (!E)
        return;
      walkExprNodes(*E, [&](const HighExpr &Node) {
        if (Node.MemoryOrdering != NdMemoryOrdering::None ||
            Node.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          HasOrderedMemory = true;
      });
    });
  });

  ExprDefinitions Defs;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        S.Dst->Kind == ExprKind::Var)
      Defs.add(S.Dst->Var, S.Val.get());
  });
  State.CanElideFrameStores =
      !HasOrderedMemory && hasOnlyPrivateFrameMemory(Func, Defs);

  auto RecordAddress = [&](const HighExpr &Addr) {
    auto Reduced = reduceAddr(Addr, Defs);
    State.AddressKeys[&Addr] =
        Reduced ? reducedAddrKey(*Reduced) : ExprFn(Addr);
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Store && S.StoreAddr)
      RecordAddress(*S.StoreAddr);
    forEachExprAndFilter(S, [&](const ExprPtr &Expr) {
      if (!Expr)
        return;
      walkExprNodes(*Expr, [&](const HighExpr &Node) {
        if ((Node.Kind == ExprKind::Load || Node.Kind == ExprKind::Store) &&
            !Node.Operands.empty())
          RecordAddress(*Node.Operands[0]);
      });
    });
  });

  std::set<ReducedAddrKey> ReducedLoads;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    forEachExprAndFilter(S, [&](const ExprPtr &E) {
      if (!E)
        return;
      walkExprNodes(*E, [&](const HighExpr &Expr) {
        if (Expr.Kind == ExprKind::Load && !Expr.Operands.empty())
          if (auto Addr = reduceAddr(*Expr.Operands[0], Defs))
            ReducedLoads.insert(keyOf(*Addr));
      });
    });
  });

  std::set<std::string> LoadVars;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    forEachExprAndFilter(S, [&](const ExprPtr &E) {
      if (E)
        collectLoadAddrVars(*E, LoadVars, VarFn);
    });
  });

  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Store || !S.StoreAddr)
      return;
    if (!State.CanElideFrameStores)
      return;
    if (S.MemoryOrdering != NdMemoryOrdering::None)
      return;
    std::set<std::string> AddrVars;
    collectExprVars(*S.StoreAddr, AddrVars, VarFn);
    bool AnyLoaded = false;
    for (auto &V : AddrVars) {
      if (LoadVars.count(V)) {
        AnyLoaded = true;
        break;
      }
    }
    if (!AnyLoaded && !AddrVars.empty() &&
        !matchesReducedAddr(*S.StoreAddr, Defs, ReducedLoads))
      State.DeadStmts.insert(&S);
  });

  std::set<std::string> LoadedAddrs;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S))
      return;
    forEachExprAndFilter(S, [&](const ExprPtr &E) {
      if (E)
        walkExprNodes(*E, [&](const HighExpr &Expr) {
          if (Expr.Kind == ExprKind::Load && !Expr.Operands.empty())
            LoadedAddrs.insert(ExprFn(*Expr.Operands[0]));
        });
    });
  });
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S))
      return;
    if (S.Kind != StmtKind::Store || !S.StoreAddr)
      return;
    if (!State.CanElideFrameStores)
      return;
    if (S.MemoryOrdering != NdMemoryOrdering::None)
      return;
    std::string Addr = ExprFn(*S.StoreAddr);
    if (!LoadedAddrs.count(Addr) &&
        !matchesReducedAddr(*S.StoreAddr, Defs, ReducedLoads))
      State.DeadStmts.insert(&S);
  });

  // A plain assignment none of whose values a live statement reads is dead,
  // and what it reads is read once less. Removing one only lowers the counts,
  // so every order removes the same assignments.
  std::map<std::string, unsigned> NameIds;
  const auto IdOf = [&](const std::string &Name) {
    return NameIds.try_emplace(Name, unsigned(NameIds.size())).first->second;
  };
  std::set<unsigned> ParamIds;
  for (auto &P : Func.Params)
    ParamIds.insert(IdOf(P.Name));
  std::vector<size_t> Uses;
  std::map<const HighStmt *, std::vector<unsigned>> Reads;
  std::vector<const HighStmt *> Live;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S))
      return;
    Live.push_back(&S);
    std::set<std::string> Names;
    forEachRhsExprAndFilter(S, [&](const ExprPtr &E) {
      if (E)
        collectExprVars(*E, Names, VarFn);
    });
    std::vector<unsigned> &Read = Reads[&S];
    for (const std::string &Name : Names) {
      const unsigned Id = IdOf(Name);
      if (Id >= Uses.size())
        Uses.resize(Id + 1);
      ++Uses[Id];
      Read.push_back(Id);
    }
  });
  const auto Alive = [&](unsigned Id) {
    return ParamIds.count(Id) || (Id < Uses.size() && Uses[Id]);
  };
  // The values each removable assignment defines, by the statement.
  std::map<const HighStmt *, std::vector<unsigned>> Defines;
  std::map<unsigned, std::vector<const HighStmt *>> DefinedBy;
  for (const HighStmt *S : Live) {
    if (S->Kind != StmtKind::Assign || !S->Dst || S->Dst->Kind != ExprKind::Var)
      continue;
    if (S->Val &&
        (S->Val->Kind == ExprKind::Call || S->Val->hasOrderedMemoryAccess()))
      continue;
    std::vector<unsigned> &Defined = Defines[S];
    Defined.push_back(IdOf(VarFn(S->Dst->Var)));
    if (S->Val)
      for (const auto &Output : S->Val->IntrinsicOutputs)
        Defined.push_back(IdOf(VarFn(Output)));
    for (const unsigned Id : Defined)
      DefinedBy[Id].push_back(S);
  }
  std::vector<const HighStmt *> Work;
  for (const auto &[S, Defined] : Defines)
    Work.push_back(S);
  while (!Work.empty()) {
    const HighStmt *S = Work.back();
    Work.pop_back();
    const std::vector<unsigned> &Defined = Defines.at(S);
    if (State.DeadStmts.count(S) ||
        std::any_of(Defined.begin(), Defined.end(), Alive))
      continue;
    State.DeadStmts.insert(S);
    State.DeadVars.insert(VarFn(S->Dst->Var));
    for (const unsigned Id : Reads.at(S))
      if (--Uses[Id] == 0 && !ParamIds.count(Id))
        if (const auto It = DefinedBy.find(Id); It != DefinedBy.end())
          Work.insert(Work.end(), It->second.begin(), It->second.end());
  }
  // An intrinsic's statement stays; a value of it nothing reads is dead.
  for (const HighStmt *S : Live) {
    if (State.DeadStmts.count(S))
      continue;
    const auto *Call = S->Kind == StmtKind::Call     ? S->CallExpr.get()
                       : S->Kind == StmtKind::Assign ? S->Val.get()
                                                     : nullptr;
    if (!Call || Call->Kind != ExprKind::Call ||
        Call->IntrinsicId == Intrinsic::None)
      continue;
    for (const auto &Output : Call->IntrinsicOutputs)
      if (std::string Name = VarFn(Output); !Alive(IdOf(Name)))
        State.DeadVars.insert(std::move(Name));
    if (S->Kind == StmtKind::Assign && S->Dst && S->Dst->Kind == ExprKind::Var)
      if (std::string Name = VarFn(S->Dst->Var); !Alive(IdOf(Name)))
        State.DeadVars.insert(std::move(Name));
  }

  std::set<std::string> AssignedVars;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S))
      return;
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var)
      AssignedVars.insert(VarFn(S.Dst->Var));
    if (S.Kind == StmtKind::Call && S.CallExpr &&
        !S.CallExpr->IntrinsicOutputs.empty())
      for (auto &CO : S.CallExpr->IntrinsicOutputs)
        AssignedVars.insert(VarFn(CO));
    if (S.Kind == StmtKind::Assign && S.Val && !S.Val->IntrinsicOutputs.empty())
      for (auto &CO : S.Val->IntrinsicOutputs)
        AssignedVars.insert(VarFn(CO));
  });

  std::set<std::string> AliveRhs;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S))
      return;
    forEachRhsExprAndFilter(S, [&](const ExprPtr &E) {
      if (E)
        collectExprVars(*E, AliveRhs, VarFn);
    });
  });

  std::set<std::string> ParamNames;
  for (auto &P : Func.Params)
    ParamNames.insert(P.Name);

  std::map<std::string, TypeRef> LiveUsed;
  collectUsedVars(Func.Body, LiveUsed, VarFn);
  for (auto &[Name, Ty] : LiveUsed) {
    if (ParamNames.count(Name))
      continue;
    if (AliveRhs.count(Name))
      continue;
    if (!AssignedVars.count(Name))
      State.DeadVars.insert(Name);
  }
}

} // namespace neverd
