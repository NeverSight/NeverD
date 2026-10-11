//===- HighDemandedBytes.cpp - Bytes no observable result reads -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A value assembled from known bytes and the bytes of a register the function
// never set, such as a lane written into an undefined NEON register or the
// upper half an SSE conversion keeps, prints its unknown bytes as a trap even
// where no store, call, return, branch or trap ever depends on them.
//
// This pass finds which bytes of each value an observable use depends on,
// walking back from those uses through truncations, extensions, byte merges,
// constant masks, constant shifts and carries, and through each variable to
// its definitions.  An unknown leaf none of whose bytes any use depends on
// becomes zero.  A leaf with one demanded byte keeps its trap: the pass never
// supplies a value a result can observe.
//
//===----------------------------------------------------------------------===//

#include "HighDCEDetail.h"

#include "neverd/ir/high/HighIR.h"

#include "llvm/ADT/bit.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neverd {
namespace {

/// One bit per byte of a value; a value wider than 64 bytes demands all.
using ByteMask = uint64_t;

constexpr size_t kVisitBudget = 2000000;

ByteMask allBytes(uint16_t Bytes) {
  return Bytes == 0 || Bytes >= 64 ? ~ByteMask{0} : (ByteMask{1} << Bytes) - 1;
}

uint16_t exprBytes(const HighExpr &E) {
  if (E.Type && E.Type->Size)
    return E.Type->Size;
  return E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi ? E.Var.Size : 0;
}

/// The bytes at or below the highest demanded one: a sum, difference,
/// product or negation carries only upward.
ByteMask carryFill(ByteMask Demand) {
  return Demand
             ? allBytes(static_cast<uint16_t>(64 - llvm::countl_zero(Demand)))
             : 0;
}

/// The bytes of \p Value that are not zero, in a \p Bytes wide operand.
ByteMask nonzeroBytes(uint64_t Value, uint16_t Bytes) {
  ByteMask Mask = 0;
  for (unsigned I = 0; I < 8 && I < Bytes; ++I)
    if ((Value >> (8 * I)) & 0xFF)
      Mask |= ByteMask{1} << I;
  // A constant wider than 8 bytes zero-extends.
  return Mask;
}

/// The operand bytes a left shift by \p Bits reads for the result bytes in
/// \p Demand: result bit b is operand bit b - Bits.
ByteMask leftShiftSource(ByteMask Demand, uint64_t Bits, uint16_t Bytes) {
  ByteMask Source = 0;
  for (unsigned I = 0; I < 64 && I < Bytes; ++I) {
    if (!(Demand >> I & 1))
      continue;
    const int64_t Low = int64_t(8 * I) - int64_t(Bits);
    const int64_t High = Low + 7;
    for (int64_t Bit = std::max<int64_t>(Low, 0); Bit <= High; Bit += 8)
      Source |= ByteMask{1} << (Bit / 8);
    if (High >= 0)
      Source |= ByteMask{1} << (High / 8);
  }
  return Source & allBytes(Bytes);
}

/// The operand bytes a right shift by \p Bits reads for the result bytes in
/// \p Demand: result bit b is operand bit b + Bits, or the sign for an
/// arithmetic shift past the top.
ByteMask rightShiftSource(ByteMask Demand, uint64_t Bits, uint16_t Bytes,
                          bool Arithmetic) {
  ByteMask Source = 0;
  const uint64_t Top = uint64_t(Bytes) * 8;
  for (unsigned I = 0; I < 64 && I < Bytes; ++I) {
    if (!(Demand >> I & 1))
      continue;
    const uint64_t Low = uint64_t(8 * I) + Bits;
    const uint64_t High = Low + 7;
    if (Low < Top)
      Source |= ByteMask{1} << (Low / 8);
    if (High < Top)
      Source |= ByteMask{1} << (High / 8);
    if (Arithmetic && High >= Top)
      Source |= ByteMask{1} << (Bytes - 1);
  }
  return Source & allBytes(Bytes);
}

class DemandedBytes {
public:
  DemandedBytes(HighFunc &Func, Arch Architecture)
      : Func(Func), Architecture(Architecture) {}

  void run() {
    collectDefinitions();
    seed();
    while (!Pending.empty() && Budget) {
      const HighExpr *E = Pending.back();
      Pending.pop_back();
      propagate(*E);
    }
    if (!Budget)
      return;
    replaceUndemandedLeaves();
  }

private:
  HighFunc &Func;
  Arch Architecture;
  size_t Budget = kVisitBudget;
  std::unordered_map<const HighExpr *, ByteMask> NodeDemand;
  VarKeyMap<ByteMask> VarDemand;
  VarKeyMap<std::vector<const HighExpr *>> Definitions;
  VarKeySet Assigned;
  std::vector<const HighExpr *> Pending;

  /// A register, flag or temporary, which only its uses read.  A stack slot
  /// or parameter may also be read through memory, which no Var names.
  static bool readOnlyByUses(const MedVar &V) {
    return V.Kind == MedVar::Reg || V.Kind == MedVar::Flag ||
           V.Kind == MedVar::Temp;
  }

  /// Each variable's assigned values, which its uses' demand reaches.
  void collectDefinitions() {
    walkStmts(Func.Body, [&](HighStmt &S) {
      if (S.Kind != StmtKind::Assign || !S.Dst || S.Dst->Kind != ExprKind::Var)
        return;
      Assigned.insert(varKey(S.Dst->Var));
      if (S.Val && readOnlyByUses(S.Dst->Var))
        Definitions[varKey(S.Dst->Var)].push_back(S.Val.get());
    });
  }

  void demand(const HighExpr *E, ByteMask Mask) {
    if (!E || !Mask || !Budget)
      return;
    --Budget;
    ByteMask &Current = NodeDemand[E];
    if ((Current | Mask) == Current)
      return;
    Current |= Mask;
    Pending.push_back(E);
  }

  void demandAll(const ExprPtr &E) {
    if (E)
      demand(E.get(), allBytes(exprBytes(*E)));
  }

  /// A node whose evaluation is observable whether or not its value is: a
  /// call, a memory access, an intrinsic, or an operation that can trap.
  static bool observableEvaluation(const HighExpr &E) {
    if (E.MemoryOrdering != NdMemoryOrdering::None ||
        E.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        E.IntrinsicId != Intrinsic::None || !E.IntrinsicOutputs.empty() ||
        E.IndirectTarget)
      return true;
    switch (E.Kind) {
    case ExprKind::Call:
    case ExprKind::Load:
    case ExprKind::Store:
      return true;
    case ExprKind::BinOp:
      return E.Op == NdOp::INT_DIV || E.Op == NdOp::INT_SDIV ||
             E.Op == NdOp::INT_REM || E.Op == NdOp::INT_SREM;
    default:
      return false;
    }
  }

  /// Every operand of every observable evaluation, and every root a
  /// statement reads for anything but a variable's value, is demanded whole.
  void seed() {
    walkStmts(Func.Body, [&](HighStmt &S) {
      const bool DefinesVar =
          S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var;
      forEachExpr(S, [&](const ExprPtr &Root) {
        seedObservableEvaluations(Root);
        if (DefinesVar && &Root == &S.Dst)
          return;
        if (DefinesVar && &Root == &S.Val && readOnlyByUses(S.Dst->Var))
          return;
        if (S.Kind == StmtKind::Assign && &Root == &S.Dst) {
          // An assignment through memory reads its address.
          Root->forEachChildExpr([&](const ExprPtr &C) { demandAll(C); });
          return;
        }
        demandAll(Root);
      });
    });
  }

  void seedObservableEvaluations(const ExprPtr &Root) {
    std::vector<const HighExpr *> Stack{Root.get()};
    std::unordered_set<const HighExpr *> Seen;
    while (!Stack.empty() && Budget) {
      const HighExpr *E = Stack.back();
      Stack.pop_back();
      if (!E || !Seen.insert(E).second)
        continue;
      --Budget;
      const bool Observable = observableEvaluation(*E);
      E->forEachChildExpr([&](const ExprPtr &C) {
        if (Observable)
          demandAll(C);
        Stack.push_back(C.get());
      });
    }
  }

  void propagate(const HighExpr &E) {
    const ByteMask D = NodeDemand[&E];
    if (observableEvaluation(E)) {
      E.forEachChildExpr([&](const ExprPtr &C) { demandAll(C); });
      return;
    }
    switch (E.Kind) {
    case ExprKind::Phi:
      // A join of incoming values, read as its variable too.
      E.forEachChildExpr([&](const ExprPtr &C) { demand(C.get(), D); });
      [[fallthrough]];
    case ExprKind::Var: {
      ByteMask &Current = VarDemand[varKey(E.Var)];
      if ((Current | D) == Current)
        return;
      Current |= D;
      const auto It = Definitions.find(varKey(E.Var));
      if (It != Definitions.end())
        for (const HighExpr *Value : It->second)
          demand(Value, Current);
      return;
    }
    case ExprKind::Const:
    case ExprKind::Undef:
    case ExprKind::EntryRegister:
      return;
    case ExprKind::UnaryOp:
      propagateUnary(E, D);
      return;
    case ExprKind::BinOp:
      propagateBinary(E, D);
      return;
    case ExprKind::Cast:
    case ExprKind::BitCast:
      propagateCast(E, D);
      return;
    default:
      E.forEachChildExpr([&](const ExprPtr &C) { demandAll(C); });
      return;
    }
  }

  static bool integer(const ExprPtr &E) {
    return E && E->Type && E->Type->Kind == NdTypeKind::Int;
  }

  void propagateUnary(const HighExpr &E, ByteMask D) {
    const ExprPtr Only = E.Operands.size() == 1 ? E.Operands[0] : nullptr;
    if (!integer(Only) || !E.Type || E.Type->Kind != NdTypeKind::Int) {
      E.forEachChildExpr([&](const ExprPtr &C) { demandAll(C); });
      return;
    }
    const uint16_t Bytes = exprBytes(*Only);
    switch (E.Op) {
    case NdOp::INT_ZEXT:
      demand(Only.get(), D & allBytes(Bytes));
      return;
    case NdOp::INT_SEXT:
      demand(
          Only.get(),
          (D & allBytes(Bytes)) |
              (D & ~allBytes(Bytes) && Bytes ? ByteMask{1} << (Bytes - 1) : 0));
      return;
    case NdOp::INT_NOT:
      demand(Only.get(), D & allBytes(Bytes));
      return;
    case NdOp::INT_NEGATE:
      demand(Only.get(), carryFill(D) & allBytes(Bytes));
      return;
    default:
      demandAll(Only);
      return;
    }
  }

  void propagateCast(const HighExpr &E, ByteMask D) {
    const ExprPtr Only = E.Operands.size() == 1 ? E.Operands[0] : nullptr;
    if (!integer(Only) || !E.Type || E.Type->Kind != NdTypeKind::Int) {
      E.forEachChildExpr([&](const ExprPtr &C) { demandAll(C); });
      return;
    }
    const uint16_t From = exprBytes(*Only);
    const uint16_t To = exprBytes(E);
    if (!From || !To) {
      demandAll(Only);
      return;
    }
    // A truncation keeps the low bytes; an extension adds bytes made of the
    // operand's sign (or of zero), which its top byte decides.
    ByteMask Source = D & allBytes(std::min(From, To));
    if (To > From && (D & ~allBytes(From)))
      Source |= ByteMask{1} << (From - 1);
    demand(Only.get(), Source);
  }

  void propagateBinary(const HighExpr &E, ByteMask D) {
    if (E.Operands.size() != 2 || !integer(E.Operands[0]) || !E.Operands[1] ||
        !E.Type || E.Type->Kind != NdTypeKind::Int) {
      E.forEachChildExpr([&](const ExprPtr &C) { demandAll(C); });
      return;
    }
    const ExprPtr &L = E.Operands[0];
    const ExprPtr &R = E.Operands[1];
    const uint16_t Bytes = exprBytes(*L);
    const auto Constant = [](const ExprPtr &X) {
      return X && X->Kind == ExprKind::Const;
    };
    switch (E.Op) {
    case NdOp::SUBBYTES:
      if (Constant(R) && R->ConstVal < 64) {
        demand(L.get(), (D << R->ConstVal) & allBytes(Bytes));
        return;
      }
      break;
    case NdOp::CONCAT: {
      // The high part above the low part.
      const uint16_t Low = exprBytes(*R);
      if (!Low || !integer(R))
        break;
      demand(R.get(), D & allBytes(Low));
      demand(L.get(), Low < 64 ? (D >> Low) & allBytes(Bytes) : 0);
      return;
    }
    case NdOp::INT_AND:
      if (Constant(R)) {
        demand(L.get(), D & nonzeroBytes(R->ConstVal, Bytes));
        return;
      }
      if (Constant(L) && integer(R)) {
        demand(R.get(), D & nonzeroBytes(L->ConstVal, exprBytes(*R)));
        return;
      }
      demand(L.get(), D & allBytes(Bytes));
      demand(R.get(), D & allBytes(exprBytes(*R)));
      return;
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
      demand(L.get(), D & allBytes(Bytes));
      demand(R.get(), D & allBytes(exprBytes(*R)));
      return;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_MULT:
      demand(L.get(), carryFill(D) & allBytes(Bytes));
      demand(R.get(), carryFill(D) & allBytes(exprBytes(*R)));
      return;
    case NdOp::INT_LEFT:
      if (Constant(R)) {
        demand(L.get(), leftShiftSource(D, R->ConstVal, Bytes));
        return;
      }
      break;
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR:
      if (Constant(R)) {
        demand(L.get(),
               rightShiftSource(D, R->ConstVal, Bytes, E.Op == NdOp::INT_ASHR));
        return;
      }
      break;
    default:
      break;
    }
    demandAll(L);
    demandAll(R);
  }

  /// An entry value of a register or flag the function neither receives as
  /// a parameter nor ever assigns, which the C prints as unknown.
  bool unknownLeaf(const HighExpr &E) const {
    if (E.Kind == ExprKind::Undef)
      return true;
    if (E.Kind != ExprKind::Var ||
        (E.Var.Kind != MedVar::Reg && E.Var.Kind != MedVar::Flag) ||
        E.Var.SSAVer != 0 || E.Var.Id < 0 || Assigned.count(varKey(E.Var)) ||
        isSyntheticEntryStackPointer(E.Var, Func, Architecture))
      return false;
    for (const HighParam &Param : Func.Params)
      if (Param.RegOff != kNoParamReg && E.Var.Kind == MedVar::Reg &&
          Param.RegOff == E.Var.RegOff)
        return false;
    return true;
  }

  void replaceUndemandedLeaves() {
    std::unordered_set<const HighExpr *> Seen;
    const auto Zero = [](const ExprPtr &Leaf) {
      ExprPtr Value = HighExpr::makeConst(0, exprBytes(*Leaf));
      if (Leaf->Type)
        Value->Type = Leaf->Type;
      return Value;
    };
    const auto Replace = [&](ExprPtr &Slot) {
      if (!Slot || !unknownLeaf(*Slot) || !exprBytes(*Slot))
        return;
      const auto It = NodeDemand.find(Slot.get());
      if (It != NodeDemand.end() && It->second)
        return;
      Slot = Zero(Slot);
    };
    std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
      if (!E || !Seen.insert(E.get()).second)
        return;
      for (ExprPtr &Operand : E->Operands) {
        Replace(Operand);
        Visit(Operand);
      }
      if (E->IndirectTarget) {
        Replace(E->IndirectTarget);
        Visit(E->IndirectTarget);
      }
    };
    walkStmts(Func.Body, [&](HighStmt &S) {
      // A variable no use reads a byte of, defined as an unknown value
      // alone, which the C would otherwise omit and trap at each use.
      if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var &&
          readOnlyByUses(S.Dst->Var) && S.Val && unknownLeaf(*S.Val) &&
          exprBytes(*S.Val)) {
        const auto It = VarDemand.find(varKey(S.Dst->Var));
        if (It == VarDemand.end() || !It->second)
          S.Val = Zero(S.Val);
      }
      forEachExpr(S, [&](const ExprPtr &Root) { Visit(Root); });
    });
  }
};

} // namespace

void zeroUndemandedUnknownBytes(HighFunc &Func, Arch Architecture) {
  if (Architecture == Arch::Unknown)
    return;
  DemandedBytes(Func, Architecture).run();
}

} // namespace neverd
