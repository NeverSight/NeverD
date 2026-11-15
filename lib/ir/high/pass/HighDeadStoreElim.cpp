//===- HighDeadStoreElim.cpp - Dead store elimination for HighIR ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dead store elimination passes for HighIR:
///   - Consecutive dead store elimination (same-destination rewrites)
///   - Private frame byte liveness (unobserved stores and integer tails)
///   - Private frame stores of undefined entry scratch registers
///   - Source-local CONCAT values with unobserved upper bytes
///
/// See also:
///   HighDCE.cpp         — main DCE orchestration, iterative liveness DCE
///   HighCopyProp.cpp    — copy propagation and alias resolution
///   HighDCEDetail.h     — shared declarations
///
//===----------------------------------------------------------------------===//

#include "HighDCEDetail.h"
#include "HighFlowGraph.h"
#include "HighFrameAddress.h"

#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/IntrinsicValueEffects.h"
#include "neverd/ir/med/MedCallConvention.h"

#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace neverd {

/// Floating-point arithmetic, comparison and conversion: with the default
/// environment's masked exceptions none traps, and a conversion to an integer
/// prints through a total helper.
static bool isTrapFreeFloatOp(NdOp Op) {
  switch (Op) {
  case NdOp::FLOAT_ADD:
  case NdOp::FLOAT_SUB:
  case NdOp::FLOAT_MULT:
  case NdOp::FLOAT_DIV:
  case NdOp::FLOAT_MIN:
  case NdOp::FLOAT_MAX:
  case NdOp::FLOAT_MINNUM:
  case NdOp::FLOAT_MAXNUM:
  case NdOp::FLOAT_EQUAL:
  case NdOp::FLOAT_NOTEQUAL:
  case NdOp::FLOAT_LESS:
  case NdOp::FLOAT_LESSEQUAL:
  case NdOp::FLOAT_NEG:
  case NdOp::FLOAT_ABS:
  case NdOp::FLOAT_SQRT:
  case NdOp::FLOAT_CEIL:
  case NdOp::FLOAT_FLOOR:
  case NdOp::FLOAT_ROUND:
  case NdOp::FLOAT_ROUNDEVEN:
  case NdOp::FLOAT_ISNAN:
  case NdOp::FLOAT_INT2FLOAT:
  case NdOp::FLOAT_UINT2FLOAT:
  case NdOp::FLOAT_FLOAT2FLOAT:
  case NdOp::FLOAT_FLOAT2INT:
  case NdOp::FLOAT_FLOAT2UINT:
  case NdOp::FLOAT_TRUNC:
    return true;
  default:
    return false;
  }
}

/// Integer arithmetic, logic and comparison of two operands: none reads
/// memory or traps.  Division and remainder trap on a zero divisor.
static bool isTrapFreeIntegerBinOp(NdOp Op) {
  switch (Op) {
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_MULT:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESS:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
    return true;
  default:
    return false;
  }
}

/// The integer operations of one operand that cannot trap.
static bool isTrapFreeIntegerUnaryOp(NdOp Op) {
  switch (Op) {
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT:
  case NdOp::INT_NEG2:
  case NdOp::BOOL_NOT:
  case NdOp::POPCOUNT:
    return true;
  default:
    return false;
  }
}

// This node's evaluation is discardable exactly when its operands are too.
// Both the bounded query and the shared cleanup analysis use this contract.
static bool discardableIntegerNode(const HighExpr *E) {
  if (!E)
    return false;
  if (!E->Type || !E->Type->Size || E->IntrinsicId != Intrinsic::None ||
      !E->IntrinsicOutputs.empty() || E->IndirectTarget ||
      E->MemoryOrdering != NdMemoryOrdering::None ||
      E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  switch (E->Kind) {
  case ExprKind::Var:
  case ExprKind::Const:
  case ExprKind::Undef:
    if (!E->Operands.empty())
      return false;
    break;
  case ExprKind::BitCast:
    // A reinterpretation of the same bytes.
    if (E->Operands.size() != 1 || !E->Operands[0] || !E->Operands[0]->Type ||
        E->Operands[0]->Type->Size != E->Type->Size)
      return false;
    break;
  case ExprKind::Cast:
  case ExprKind::UnaryOp:
    if (E->Kind == ExprKind::UnaryOp && isTrapFreeFloatOp(E->Op) &&
        E->Operands.size() == 1 && E->Operands[0])
      break;
    if (E->Type->Kind != NdTypeKind::Int || E->Operands.size() != 1 ||
        !E->Operands[0] || !E->Operands[0]->Type ||
        E->Operands[0]->Type->Kind != NdTypeKind::Int)
      return false;
    if (E->Kind == ExprKind::UnaryOp &&
        (!isTrapFreeIntegerUnaryOp(E->Op) ||
         ((E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT) &&
          E->Type->Size < E->Operands[0]->Type->Size)))
      return false;
    break;
  case ExprKind::BinOp:
    if (E->Op == NdOp::SELECT) {
      if (E->Type->Kind != NdTypeKind::Int || E->Operands.size() != 3)
        return false;
      for (unsigned I = 0; I < 3; ++I) {
        const auto &Operand = E->Operands[I];
        if (!Operand || !Operand->Type ||
            Operand->Type->Kind != NdTypeKind::Int || !Operand->Type->Size ||
            (I && Operand->Type->Size != E->Type->Size))
          return false;
      }
      break;
    }
    if (isTrapFreeFloatOp(E->Op) && E->Operands.size() == 2 && E->Operands[0] &&
        E->Operands[1])
      break;
    if (E->Type->Kind != NdTypeKind::Int || E->Operands.size() != 2 ||
        !E->Operands[0] || !E->Operands[0]->Type ||
        E->Operands[0]->Type->Kind != NdTypeKind::Int || !E->Operands[1] ||
        !E->Operands[1]->Type || E->Operands[1]->Type->Kind != NdTypeKind::Int)
      return false;
    if (E->Op == NdOp::CONCAT) {
      if (E->Type->Size !=
          E->Operands[0]->Type->Size + E->Operands[1]->Type->Size)
        return false;
    } else if (E->Op == NdOp::SUBBYTES) {
      if (E->Operands[1]->Kind != ExprKind::Const ||
          E->Operands[1]->ConstVal > E->Operands[0]->Type->Size ||
          E->Type->Size > E->Operands[0]->Type->Size - E->Operands[1]->ConstVal)
        return false;
    } else if (!isTrapFreeIntegerBinOp(E->Op)) {
      return false;
    }
    break;
  default:
    return false;
  }
  return true;
}

// Only remove or slice values whose evaluation cannot read memory, trap or
// call another function. Unknown bits may be discarded only when their exact
// bytes have no reads; this never supplies a replacement bit value.
bool discardableIntegerValue(const ExprPtr &Root, size_t &Budget) {
  std::vector<const HighExpr *> Pending{Root.get()};
  HighExprSet Seen;
  while (!Pending.empty()) {
    const auto *E = Pending.back();
    Pending.pop_back();
    if (!E || !Budget)
      return false;
    if (!Seen.insert(E).second)
      continue;
    --Budget;
    if (!discardableIntegerNode(E))
      return false;
    for (const auto &Operand : E->Operands)
      Pending.push_back(Operand.get());
  }
  return true;
}

bool DiscardableIntegerValues::canDiscard(const ExprPtr &Root) {
  if (!Root)
    return false;
  std::vector<std::pair<ExprPtr, bool>> Pending{{Root, false}};
  while (!Pending.empty()) {
    auto [Value, After] = std::move(Pending.back());
    Pending.pop_back();
    if (After) {
      bool Discardable = true;
      for (const auto &Operand : Value->Operands) {
        const auto It = States.find(Operand);
        Discardable &= It != States.end() && It->second == State::Discardable;
      }
      States.at(Value) = Discardable ? State::Discardable : State::Required;
      continue;
    }
    auto [It, Added] = States.try_emplace(Value, State::Visiting);
    if (!Added)
      continue;
    if (!discardableIntegerNode(Value.get())) {
      It->second = State::Required;
      continue;
    }
    Pending.emplace_back(Value, true);
    for (const auto &Operand : Value->Operands)
      if (!States.count(Operand))
        Pending.emplace_back(Operand, false);
  }
  return States.at(Root) == State::Discardable;
}

bool harmlessIntegerValue(const ExprPtr &Root, size_t &Budget) {
  std::vector<const HighExpr *> Pending{Root.get()};
  HighExprSet Seen;
  while (!Pending.empty()) {
    const auto *E = Pending.back();
    Pending.pop_back();
    if (!E || !Budget)
      return false;
    if (!Seen.insert(E).second)
      continue;
    --Budget;
    if (!E->Type || E->Type->Kind != NdTypeKind::Int ||
        E->IntrinsicId != Intrinsic::None || !E->IntrinsicOutputs.empty() ||
        E->IndirectTarget || E->MemoryOrdering != NdMemoryOrdering::None ||
        E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
    switch (E->Kind) {
    case ExprKind::Var:
    case ExprKind::Phi:
    case ExprKind::Const:
    case ExprKind::Undef:
      if (!E->Operands.empty())
        return false;
      break;
    case ExprKind::Cast:
    case ExprKind::BitCast:
      break;
    case ExprKind::UnaryOp:
      if (!isTrapFreeIntegerUnaryOp(E->Op))
        return false;
      break;
    case ExprKind::BinOp:
      if (E->Op == NdOp::SUBBYTES) {
        if (E->Operands.size() != 2 || !E->Operands[1] ||
            E->Operands[1]->Kind != ExprKind::Const)
          return false;
      } else if (E->Op != NdOp::CONCAT && E->Op != NdOp::SELECT &&
                 !isTrapFreeIntegerBinOp(E->Op)) {
        return false;
      }
      break;
    default:
      return false;
    }
    for (const auto &Operand : E->Operands)
      Pending.push_back(Operand.get());
  }
  return true;
}

namespace {
ExprPtr frameValuePrefix(ExprPtr Value, uint16_t Bytes) {
  // CONCAT's low operand owns the low-address bytes in supported native IR.
  // Preserve its exact expression instead of retaining unknown high padding.
  if (Value->Kind == ExprKind::BinOp && Value->Op == NdOp::CONCAT &&
      Value->Operands.size() == 2 && Value->Operands[0] &&
      Value->Operands[0]->Type && Value->Operands[1] &&
      Value->Operands[1]->Type &&
      Value->Type->Size ==
          Value->Operands[0]->Type->Size + Value->Operands[1]->Type->Size &&
      Value->Operands[1]->Type->Size >= Bytes)
    Value = Value->Operands[1];
  if (Value->Type->Size == Bytes)
    return Value;
  auto Prefix =
      HighExpr::makeBinop(NdOp::SUBBYTES, Value, HighExpr::makeConst(0, 4));
  Prefix->Type = NdType::makeInt(Bytes, false);
  return Prefix;
}
} // namespace

// A source-local merge can retain dead upper register bytes on every incoming
// edge. Narrow only when every definition constructs the same scalar prefix
// and every read explicitly selects that prefix. No CFG path or evaluation
// moves: each definition keeps its low expression and its original location.
void narrowSourceConcatLocals(HighFunc &Func) {
  std::string Error;
  if (!Func.SourceTypeHint || !validateSourceABI(*Func.SourceTypeHint, Error) ||
      Func.StructuredExceptionRegions || Func.UnstructuredExceptionRegions)
    return;
  size_t Budget =
      ::neverd::limits::ir::kHighDeadStoreElimNarrowSourceConcatLocalsBudget;
  auto Plain = [](const HighExpr &E) {
    return E.IntrinsicId == Intrinsic::None && E.IntrinsicOutputs.empty() &&
           E.MemoryOrdering == NdMemoryOrdering::None &&
           E.MemoryAddressSpace == NdMemoryAddressSpace::Default;
  };
  // Follow only low-byte-preserving integer views. Native return packing can
  // wrap a CONCAT in SUBBYTES and an extension before assigning its carrier;
  // the undefined upper half is still dead when all later reads select the
  // same low scalar. Every wrapper and discarded high operand must be pure.
  const auto ConcatLowPrefix = [&](ExprPtr Value,
                                   uint16_t CarrierBytes) -> ExprPtr {
    uint16_t Capacity = CarrierBytes;
    for (
        unsigned Depth = 0;
        Value &&
        Depth <
            ::neverd::limits::ir::
                kHighDeadStoreElimNarrowSourceConcatLocalsConcatLowPrefixDepthLimit;
        ++Depth) {
      if (!Budget-- || !Plain(*Value) || !Value->Type ||
          Value->Type->Kind != NdTypeKind::Int || !Value->Type->Size)
        return {};
      Capacity = std::min(Capacity, Value->Type->Size);
      if (Value->Kind == ExprKind::BinOp && Value->Op == NdOp::CONCAT &&
          Value->Operands.size() == 2 && Value->Operands[0] &&
          Value->Operands[1] && Value->Operands[0]->Type &&
          Value->Operands[1]->Type &&
          Value->Operands[1]->Type->Kind == NdTypeKind::Int &&
          Value->Operands[0]->Type->Size + Value->Operands[1]->Type->Size ==
              Value->Type->Size &&
          (Value->Operands[1]->Type->Size == 4 ||
           (CarrierBytes == 16 && Value->Operands[1]->Type->Size == 8)) &&
          Value->Operands[1]->Type->Size <= Capacity &&
          discardableIntegerValue(Value->Operands[0], Budget))
        return Value->Operands[1];
      if (Value->Kind == ExprKind::Cast && Value->CastTo &&
          Value->CastTo->Kind == NdTypeKind::Int &&
          Value->CastTo->Size == Value->Type->Size &&
          Value->Operands.size() == 1 && Value->Operands[0] &&
          Value->Operands[0]->Type &&
          Value->Operands[0]->Type->Kind == NdTypeKind::Int) {
        Value = Value->Operands[0];
        continue;
      }
      if (Value->Kind == ExprKind::UnaryOp &&
          (Value->Op == NdOp::INT_ZEXT || Value->Op == NdOp::INT_SEXT) &&
          Value->Operands.size() == 1 && Value->Operands[0] &&
          Value->Operands[0]->Type &&
          Value->Operands[0]->Type->Kind == NdTypeKind::Int &&
          Value->Operands[0]->Type->Size <= Value->Type->Size) {
        Value = Value->Operands[0];
        continue;
      }
      if (Value->Kind == ExprKind::BinOp && Value->Op == NdOp::SUBBYTES &&
          Value->Operands.size() == 2 && Value->Operands[0] &&
          Value->Operands[0]->Type &&
          Value->Operands[0]->Type->Kind == NdTypeKind::Int &&
          Value->Operands[0]->Type->Size >= Value->Type->Size &&
          Value->Operands[1] && Plain(*Value->Operands[1]) &&
          Value->Operands[1]->Kind == ExprKind::Const &&
          Value->Operands[1]->Type &&
          Value->Operands[1]->Type->Kind == NdTypeKind::Int &&
          Value->Operands[1]->Operands.empty() &&
          Value->Operands[1]->ConstVal == 0) {
        Value = Value->Operands[0];
        continue;
      }
      return {};
    }
    return {};
  };
  struct Candidate {
    uint16_t Bytes = 0;
    uint16_t CarrierBytes = 0;
    bool Valid = true;
    std::vector<HighStmt *> Definitions;
    std::set<HighStmt *> PrefixDefinitions;
    struct Use {
      ExprPtr *Slot;
      bool ZeroExtendForMask = false;
    };
    std::vector<Use> Uses;
  };
  VarKeyMap<Candidate> Candidates;
  std::map<HighStmt *, ExprPtr> ConcatPrefixes;
  struct CopyDefinition {
    HighStmt *Statement = nullptr;
    ExprPtr *Leaf = nullptr;
    uint16_t Capacity = 0;
  };
  std::vector<CopyDefinition> CopyDefinitions;
  std::vector<std::pair<HighStmt *, unsigned>> Pending;
  for (auto &S : Func.Body)
    Pending.emplace_back(&S, 0);
  std::vector<HighStmt *> Statements;
  for (size_t I = 0; I < Pending.size(); ++I) {
    auto [S, Depth] = Pending[I];
    if (!Budget-- ||
        Depth > ::neverd::limits::ir::
                    kHighDeadStoreElimNarrowSourceConcatLocalsDepthLimit)
      return;
    Statements.push_back(S);
    auto Add = [&](std::vector<HighStmt> &Body) {
      for (auto &Child : Body)
        Pending.emplace_back(&Child, Depth + 1);
    };
    Add(S->Body);
    Add(S->ElseBody);
    Add(S->DefaultBody);
    for (auto &C : S->Cases)
      Add(C.Body);
    for (auto &Body : S->EHClauseBodies)
      Add(Body);
    if (S->Kind != StmtKind::Assign || !S->Dst || S->Dst->Kind != ExprKind::Var)
      continue;
    auto &C = Candidates[varKey(S->Dst->Var)];
    const auto &D = S->Dst;
    const auto &V = S->Val;
    const bool DestinationShape =
        Plain(*D) && D->Operands.empty() && D->Var.Id >= 0 &&
        (D->Var.Kind == MedVar::Reg || D->Var.Kind == MedVar::Temp) &&
        D->Type && D->Type->Kind == NdTypeKind::Int &&
        (D->Type->Size == 8 || D->Type->Size == 16) &&
        D->Var.Size == D->Type->Size && D->Var.RenameTag < 0 && V &&
        Plain(*V) && S->MemoryOrdering == NdMemoryOrdering::None &&
        S->MemoryAddressSpace == NdMemoryAddressSpace::Default;
    const uint16_t CarrierBytes = DestinationShape ? D->Type->Size : 0;
    C.Valid &= !C.CarrierBytes || C.CarrierBytes == CarrierBytes;
    C.CarrierBytes = CarrierBytes;
    const auto ConcatLow = DestinationShape && V->Type &&
                                   V->Type->Kind == NdTypeKind::Int &&
                                   V->Type->Size == CarrierBytes
                               ? ConcatLowPrefix(V, CarrierBytes)
                               : ExprPtr{};
    const bool ConcatShape = bool(ConcatLow);
    const bool ExtensionShape =
        DestinationShape && V->Type && V->Type->Kind == NdTypeKind::Int &&
        V->Type->Size == CarrierBytes && V->Operands.size() == 1 &&
        V->Operands[0] && V->Operands[0]->Kind != ExprKind::Undef &&
        V->Operands[0]->Type && V->Operands[0]->Type->Kind == NdTypeKind::Int &&
        V->Operands[0]->Type->Size && V->Operands[0]->Type->Size <= 8 &&
        (CarrierBytes == 16 || V->Operands[0]->Type->Size == 4) &&
        ((V->Kind == ExprKind::Cast && V->CastTo &&
          V->CastTo->Kind == NdTypeKind::Int &&
          V->CastTo->Size == CarrierBytes) ||
         (V->Kind == ExprKind::UnaryOp &&
          (V->Op == NdOp::INT_ZEXT || V->Op == NdOp::INT_SEXT)));
    CopyDefinition Copy{S, &S->Val, CarrierBytes};
    const bool CopyShape = [&]() {
      if (!DestinationShape || !V->Type || V->Type->Kind != NdTypeKind::Int ||
          V->Type->Size != CarrierBytes)
        return false;
      // Each accepted view preserves its low Capacity bytes. Check every
      // intermediate width: re-extension cannot restore a discarded byte.
      for (unsigned Depth = 0;
           Depth <=
           ::neverd::limits::ir::
               kHighDeadStoreElimNarrowSourceConcatLocalsCopyShapeDepthLimit;
           ++Depth) {
        const auto &E = *Copy.Leaf;
        if (!E || !Plain(*E) || !E->Type || E->Type->Kind != NdTypeKind::Int ||
            !E->Type->Size || E->Type->Size > 16)
          return false;
        Copy.Capacity = std::min(Copy.Capacity, E->Type->Size);
        if (E->Kind == ExprKind::Var)
          return E->Operands.empty() && E->Var.Id >= 0 &&
                 E->Var.Size == E->Type->Size && E->Var.RenameTag < 0 &&
                 (E->Var.Size == 8 || E->Var.Size == 16) &&
                 (E->Var.Kind == MedVar::Reg || E->Var.Kind == MedVar::Temp);
        const bool Cast = E->Kind == ExprKind::Cast &&
                          E->Operands.size() == 1 && E->CastTo &&
                          E->CastTo->Kind == NdTypeKind::Int &&
                          E->CastTo->Size == E->Type->Size &&
                          E->CastTo->IsSigned == E->Type->IsSigned;
        const bool Extension =
            E->Kind == ExprKind::UnaryOp && E->Operands.size() == 1 &&
            (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT);
        const bool Slice =
            E->Kind == ExprKind::BinOp && E->Op == NdOp::SUBBYTES &&
            E->Operands.size() == 2 && E->Operands[1] &&
            Plain(*E->Operands[1]) && E->Operands[1]->Kind == ExprKind::Const &&
            E->Operands[1]->Operands.empty() && E->Operands[1]->Type &&
            E->Operands[1]->Type->Kind == NdTypeKind::Int &&
            E->Operands[1]->Type->Size && E->Operands[1]->ConstVal == 0;
        if ((!Cast && !Extension && !Slice) || !E->Operands[0] ||
            !E->Operands[0]->Type ||
            (Extension && E->Operands[0]->Type->Size > E->Type->Size) ||
            (Slice && E->Operands[0]->Type->Size < E->Type->Size))
          return false;
        Copy.Leaf = &E->Operands[0];
      }
      return false;
    }();
    // An extension keeps its entire operand at this definition site: the
    // narrowed assignment reads its low bytes through frameValuePrefix.
    // Only a CONCAT's discarded high half or a copied carrier needs the
    // side-effect-free proof. In particular, a float bitcast feeding INT_ZEXT
    // must remain evaluated even though it is not an integer-only tree.
    if ((!ConcatShape && !ExtensionShape && !CopyShape) ||
        (!ConcatShape && !ExtensionShape &&
         !discardableIntegerValue(V, Budget))) {
      C.Valid = false;
      continue;
    }
    if (ConcatShape) {
      const auto Width = ConcatLow->Type->Size;
      C.Valid &= !C.Bytes || C.Bytes == Width;
      C.Bytes = Width;
      ConcatPrefixes.emplace(S, ConcatLow);
    } else {
      if (ExtensionShape) {
        const auto Width = V->Operands[0]->Type->Size;
        C.Valid &= !C.Bytes || C.Bytes == Width;
        C.Bytes = Width;
      }
      C.PrefixDefinitions.insert(S);
    }
    C.Definitions.push_back(S);
    // An extension already establishes its own prefix width. Keep that
    // existing proof independent of the source local's narrower candidates.
    if (CopyShape && !ExtensionShape)
      CopyDefinitions.push_back(Copy);
  }
  // A whole-copy RHS may inherit a prefix proof only from its exact
  // destination. Shared expression nodes in other consumers get no permission.
  std::unordered_map<ExprPtr *, CopyDefinition> CopyTargets;
  VarKeyMap<std::vector<VarKey>> Neighbors, CopySources;
  for (const auto &Copy : CopyDefinitions) {
    auto *S = Copy.Statement;
    if (!Budget--)
      return;
    const auto From = varKey((*Copy.Leaf)->Var), To = varKey(S->Dst->Var);
    auto Source = Candidates.find(From);
    auto &Destination = Candidates.at(To);
    if (Source == Candidates.end() || !Destination.CarrierBytes ||
        Source->second.CarrierBytes != (*Copy.Leaf)->Var.Size)
      continue;
    CopyTargets.emplace(&S->Val, Copy);
    Neighbors[From].push_back(To);
    Neighbors[To].push_back(From);
  }
  // Prefix widths originate in a constructing definition, never a guessed
  // use. Copy-only components without such a seed remain unchanged.
  VarKeyMap<bool> Visited;
  for (const auto &[Key, _] : Candidates) {
    if (Visited[Key])
      continue;
    Visited[Key] = true;
    std::vector<VarKey> Component{Key};
    uint16_t Bytes = 0;
    bool Consistent = true;
    for (size_t I = 0; I < Component.size(); ++I) {
      if (!Budget--)
        return;
      const auto &C = Candidates.at(Component[I]);
      if (C.Bytes) {
        Consistent &= !Bytes || Bytes == C.Bytes;
        Bytes = C.Bytes;
      }
      for (const auto &Next : Neighbors[Component[I]]) {
        if (!Budget--)
          return;
        if (!Visited[Next]) {
          Visited[Next] = true;
          Component.push_back(Next);
        }
      }
    }
    for (const auto &Member : Component) {
      auto &C = Candidates.at(Member);
      C.Bytes = Bytes;
      C.Valid &= Consistent;
    }
  }
  // Cross-carrier copies must preserve the entire established prefix and
  // still satisfy each destination's supported narrowing range.
  for (const auto &[Root, Copy] : CopyTargets) {
    auto &C = Candidates.at(varKey(Copy.Statement->Dst->Var));
    C.Valid &= C.Bytes <= Copy.Capacity && C.Bytes < C.CarrierBytes &&
               (C.CarrierBytes == 16 || C.Bytes == 4);
  }
  struct Use {
    ExprPtr *Slot;
    ExprPtr *Root;
    const HighExpr *Parent;
    unsigned Operand, Depth;
  };
  std::vector<Use> Uses;
  // Keep scanned DAG nodes alive while definitions and shared leaf slots
  // are replaced, including uses inside another candidate's discarded tail.
  std::vector<ExprPtr> Roots;
  for (auto *S : Statements)
    forEachExpr(*S, [&](ExprPtr &E) {
      if (!(S->Kind == StmtKind::Assign && &E == &S->Dst)) {
        Roots.push_back(E);
        Uses.push_back({&E, &E, nullptr, 0, 0});
      }
    });
  for (size_t I = 0; I < Uses.size(); ++I) {
    auto [Slot, Root, Parent, Operand, Depth] = Uses[I];
    const auto &E = *Slot;
    if (!Budget-- ||
        Depth > ::neverd::limits::ir::
                    kHighDeadStoreElimNarrowSourceConcatLocalsDepthLimit ||
        !E)
      return;
    for (const auto &Output : E->IntrinsicOutputs)
      if (auto It = Candidates.find(varKey(Output)); It != Candidates.end())
        It->second.Valid = false;
    if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) {
      if (auto It = Candidates.find(varKey(E->Var)); It != Candidates.end()) {
        auto &C = It->second;
        const bool Prefix =
            Parent && Plain(*Parent) && Operand == 0 && Parent->Type &&
            Parent->Type->Kind == NdTypeKind::Int && Parent->Type->Size &&
            Parent->Type->Size <= C.Bytes &&
            ((Parent->Kind == ExprKind::Cast && Parent->Operands.size() == 1 &&
              Parent->CastTo && Parent->CastTo->Kind == NdTypeKind::Int &&
              Parent->CastTo->Size == Parent->Type->Size) ||
             (Parent->Kind == ExprKind::BinOp && Parent->Op == NdOp::SUBBYTES &&
              Parent->Operands.size() == 2 && Parent->Operands[1] &&
              Parent->Operands[1]->Kind == ExprKind::Const &&
              Plain(*Parent->Operands[1]) &&
              Parent->Operands[1]->Operands.empty() &&
              Parent->Operands[1]->ConstVal == 0));
        // A full-width AND observes only the low word when every mask bit
        // above that word is clear. Keep the AND at its original width, but
        // give it an explicit zero extension after narrowing the local.
        const bool MaskedPrefix =
            Parent && Plain(*Parent) && Operand == 0 && C.Bytes == 4 &&
            C.CarrierBytes == 8 && Parent->Kind == ExprKind::BinOp &&
            Parent->Op == NdOp::INT_AND && Parent->Type &&
            Parent->Type->Kind == NdTypeKind::Int && Parent->Type->Size == 8 &&
            Parent->Operands.size() == 2 && Parent->Operands[1] &&
            Plain(*Parent->Operands[1]) &&
            Parent->Operands[1]->Kind == ExprKind::Const &&
            Parent->Operands[1]->Operands.empty() &&
            Parent->Operands[1]->Type &&
            Parent->Operands[1]->Type->Kind == NdTypeKind::Int &&
            Parent->Operands[1]->Type->Size &&
            Parent->Operands[1]->Type->Size <= 8 &&
            (Parent->Operands[1]->Type->Size == 8 ||
             Parent->Operands[1]->ConstVal <
                 (uint64_t{1}
                  << (Parent->Operands[1]->Type->Size * 8 -
                      unsigned(Parent->Operands[1]->Type->IsSigned)))) &&
            (Parent->Operands[1]->ConstVal & ~UINT64_C(0xffffffff)) == 0;
        const auto Copy = CopyTargets.find(Root);
        const bool ProvenCopy =
            Copy != CopyTargets.end() && Copy->second.Leaf == Slot && C.Bytes &&
            C.Bytes <= Copy->second.Capacity &&
            Candidates.at(varKey(Copy->second.Statement->Dst->Var)).Bytes ==
                C.Bytes;
        // Explicit low reads already prove their source independently of
        // whether the destination can narrow. Only exemptions depend on it.
        if (ProvenCopy && !Prefix)
          CopySources[varKey(Copy->second.Statement->Dst->Var)].push_back(
              It->first);
        C.Valid &= (Prefix || MaskedPrefix || ProvenCopy) && Plain(*E) &&
                   E->Operands.empty() && E->Type &&
                   E->Type->Kind == NdTypeKind::Int &&
                   E->Type->Size == C.CarrierBytes &&
                   E->Var.Size == C.CarrierBytes && E->Var.RenameTag < 0 &&
                   (E->Var.Kind == MedVar::Reg || E->Var.Kind == MedVar::Temp);
        C.Uses.push_back({Slot, MaskedPrefix});
      }
    }
    for (unsigned J = 0; J < E->Operands.size(); ++J)
      Uses.push_back({&E->Operands[J], Root, E.get(), J, Depth + 1});
  }
  // A source cannot be narrowed if an exempted whole-copy consumer will
  // remain wide. Propagate final rewrite eligibility backwards to a fixed
  // point before changing any expression or statement.
  std::vector<VarKey> Invalid;
  for (auto &[Key, C] : Candidates) {
    C.Valid &= C.Bytes && !C.Definitions.empty() && !C.Uses.empty();
    if (!C.Valid)
      Invalid.push_back(Key);
  }
  for (size_t I = 0; I < Invalid.size(); ++I) {
    if (!Budget--)
      return;
    for (const auto &From : CopySources[Invalid[I]]) {
      if (!Budget--)
        return;
      auto &C = Candidates.at(From);
      if (C.Valid) {
        C.Valid = false;
        Invalid.push_back(From);
      }
    }
  }
  if (!Budget)
    return;
  const auto Narrow = [](ExprPtr &E, uint16_t Bytes) {
    E = std::make_shared<HighExpr>(*E);
    E->Type = NdType::makeInt(Bytes, false);
    E->Var.Size = Bytes;
  };
  // A recorded use may be another candidate's definition RHS. Rewrite all
  // uses first, before prefix extraction can replace that slot with a tree.
  std::set<ExprPtr *> RewrittenUses;
  for (auto &[Key, C] : Candidates)
    if (C.Valid)
      for (const auto &Use : C.Uses) {
        if (!RewrittenUses.insert(Use.Slot).second)
          continue;
        Narrow(*Use.Slot, C.Bytes);
        if (Use.ZeroExtendForMask) {
          auto Extended = HighExpr::makeUnary(NdOp::INT_ZEXT, *Use.Slot);
          Extended->Type = NdType::makeInt(C.CarrierBytes, false);
          *Use.Slot = std::move(Extended);
        }
      }
  for (auto &[Key, C] : Candidates) {
    if (!C.Valid)
      continue;
    for (auto *S : C.Definitions) {
      Narrow(S->Dst, C.Bytes);
      const auto Prefix = ConcatPrefixes.find(S);
      S->Val = Prefix == ConcatPrefixes.end()
                   ? frameValuePrefix(S->Val, C.Bytes)
                   : Prefix->second;
    }
  }
}

// A register value whose every read selects the same low bytes has dead upper
// bytes; a byte write into a register the function never set leaves them
// undefined, and they would print as an unknown read. Each definition becomes
// its low prefix, zero-extended to the carrier, when the bytes it drops are
// pure. No definition moves and no read changes.
//
// Reads are first counted through explicit low views alone. Counting them
// also through arithmetic whose low bytes come from its operands' low bytes
// (`(uint8_t)(v - 2)`) finds more dead bytes, but rewriting a definition
// whose upper bytes are well defined only adds casts; that wider count
// narrows only definitions that can leave bytes undefined.
void narrowUnreadRegisterBytes(HighFunc &Func, Arch Architecture) {
  size_t Budget = ::neverd::limits::ir::kHighDeadStoreElimBudget;
  bool Abort = false;
  bool ThroughArithmetic = false;
  struct Value {
    uint16_t Carrier = 0;
    uint16_t Read = 0;
    bool Valid = true;
    std::vector<HighStmt *> Definitions;
    std::vector<VarKey> CopiesTo;
  };
  VarKeyMap<Value> Values;
  auto Candidate = [](const ExprPtr &D) {
    return D && D->Kind == ExprKind::Var && D->Operands.empty() &&
           D->Var.Id >= 0 &&
           (D->Var.Kind == MedVar::Reg || D->Var.Kind == MedVar::Temp) &&
           D->Var.RenameTag < 0 && D->Type &&
           D->Type->Kind == NdTypeKind::Int && D->Type->Size >= 2 &&
           D->Type->Size <= 8 && D->Var.Size == D->Type->Size &&
           D->IntrinsicId == Intrinsic::None && D->IntrinsicOutputs.empty();
  };
  // A value no context reads: only the definitions it holds count.
  std::function<void(const ExprPtr &)> Unread = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (!Budget--) {
      Abort = true;
      return;
    }
    for (const MedVar &Output : E->IntrinsicOutputs)
      Values[varKey(Output)].Valid = false;
    E->forEachChildExpr([&](const ExprPtr &C) { Unread(C); });
  };
  // Record how many low bytes of \p E its context reads (0: all of them).
  std::function<void(const ExprPtr &, uint16_t)> Visit =
      [&](const ExprPtr &E, uint16_t Selected) {
        if (!E)
          return;
        if (!Budget--) {
          Abort = true;
          return;
        }
        for (const MedVar &Output : E->IntrinsicOutputs)
          Values[varKey(Output)].Valid = false;
        auto Narrowed = [&](uint16_t Bytes) {
          return Selected ? std::min(Selected, Bytes) : Bytes;
        };
        if (E->Kind == ExprKind::Var) {
          Value &V = Values[varKey(E->Var)];
          V.Read =
              std::max<uint16_t>(V.Read, Selected ? Selected : E->Var.Size);
          return;
        }
        const ExprPtr Only = E->Operands.size() == 1 ? E->Operands[0] : nullptr;
        if (E->Kind == ExprKind::BinOp && E->Op == NdOp::SUBBYTES &&
            E->Operands.size() == 2 && E->Operands[1] &&
            E->Operands[1]->Kind == ExprKind::Const &&
            E->Operands[1]->ConstVal == 0 && E->Type && E->Type->Size) {
          Visit(E->Operands[0], Narrowed(E->Type->Size));
          return;
        }
        if (E->Kind == ExprKind::Cast && E->CastTo &&
            E->CastTo->Kind == NdTypeKind::Int && Only && Only->Type &&
            Only->Type->Kind == NdTypeKind::Int &&
            E->CastTo->Size <= Only->Type->Size) {
          Visit(Only, Narrowed(E->CastTo->Size));
          return;
        }
        if (!ThroughArithmetic) {
          E->forEachChildExpr([&](const ExprPtr &C) { Visit(C, 0); });
          return;
        }
        // Bytes [K, K + N) of a value lie in its low K + N bytes.
        if (E->Kind == ExprKind::BinOp && E->Op == NdOp::SUBBYTES &&
            E->Operands.size() == 2 && E->Operands[0] && E->Operands[0]->Type &&
            E->Operands[0]->Type->Kind == NdTypeKind::Int && E->Operands[1] &&
            E->Operands[1]->Kind == ExprKind::Const && E->Type &&
            E->Type->Size) {
          const uint64_t End =
              E->Operands[1]->ConstVal + uint64_t{Narrowed(E->Type->Size)};
          Visit(E->Operands[0],
                End < E->Operands[0]->Type->Size ? uint16_t(End) : 0);
          return;
        }
        // The low bytes of a concatenation that its low part supplies never
        // read the high part.
        if (Selected && E->Kind == ExprKind::BinOp && E->Op == NdOp::CONCAT &&
            E->Operands.size() == 2 && E->Operands[1] && E->Operands[1]->Type &&
            E->Operands[1]->Type->Kind == NdTypeKind::Int &&
            Selected <= E->Operands[1]->Type->Size) {
          Unread(E->Operands[0]);
          Visit(E->Operands[1],
                Selected < E->Operands[1]->Type->Size ? Selected : 0);
          return;
        }
        // The low bytes of an extension are its operand's; those of a sum,
        // difference, product, complement, bitwise combination or left
        // shift come from the operands' low bytes alone (the shift amount
        // is read whole).
        const bool Integer = E->Type && E->Type->Kind == NdTypeKind::Int &&
                             Selected && Selected < E->Type->Size;
        const bool Extension =
            (E->Kind == ExprKind::UnaryOp &&
             (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT)) ||
            (E->Kind == ExprKind::Cast && E->CastTo &&
             E->CastTo->Kind == NdTypeKind::Int);
        if (Integer && Extension && Only && Only->Type &&
            Only->Type->Kind == NdTypeKind::Int) {
          Visit(Only, Selected < Only->Type->Size ? Selected : 0);
          return;
        }
        if (Integer && Only && E->Kind == ExprKind::UnaryOp &&
            (E->Op == NdOp::INT_NEGATE || E->Op == NdOp::INT_NOT)) {
          Visit(Only, Selected);
          return;
        }
        if (Integer && E->Kind == ExprKind::BinOp && E->Operands.size() == 2 &&
            (E->Op == NdOp::INT_ADD || E->Op == NdOp::INT_SUB ||
             E->Op == NdOp::INT_MULT || E->Op == NdOp::INT_AND ||
             E->Op == NdOp::INT_OR || E->Op == NdOp::INT_XOR ||
             E->Op == NdOp::INT_LEFT)) {
          Visit(E->Operands[0], Selected);
          Visit(E->Operands[1], E->Op == NdOp::INT_LEFT ? 0 : Selected);
          return;
        }
        E->forEachChildExpr([&](const ExprPtr &C) { Visit(C, 0); });
      };
  auto Collect = [&](bool Arithmetic) {
    Values.clear();
    Budget = ::neverd::limits::ir::kHighDeadStoreElimBudget;
    Abort = false;
    ThroughArithmetic = Arithmetic;
    walkStmts(Func.Body, [&](HighStmt &S) {
      if (Abort)
        return;
      const bool Defines = S.Kind == StmtKind::Assign && Candidate(S.Dst);
      if (Defines) {
        Value &V = Values[varKey(S.Dst->Var)];
        V.Valid &= !V.Carrier || V.Carrier == S.Dst->Type->Size;
        V.Carrier = S.Dst->Type->Size;
        V.Definitions.push_back(&S);
        if (S.Val && S.Val->Kind == ExprKind::Var && S.Val->Operands.empty()) {
          Values[varKey(S.Val->Var)].CopiesTo.push_back(varKey(S.Dst->Var));
          return;
        }
      } else if (S.Kind == StmtKind::Assign && S.Dst &&
                 S.Dst->Kind == ExprKind::Var) {
        Values[varKey(S.Dst->Var)].Valid = false;
      }
      forEachExpr(S, [&](const ExprPtr &E) {
        if (&E == &S.Dst && S.Kind == StmtKind::Assign && E &&
            E->Kind == ExprKind::Var)
          return;
        Visit(E, 0);
      });
    });
    if (Abort)
      return;
    // A copy reads what its destination's reads select.
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (auto &[Key, V] : Values)
        for (const VarKey &To : V.CopiesTo) {
          auto Dest = Values.find(To);
          if (Dest == Values.end())
            continue;
          const uint16_t Read =
              Dest->second.Valid ? Dest->second.Read : Dest->second.Carrier;
          if (Read > V.Read) {
            V.Read = Read;
            Changed = true;
          }
        }
    }
  };
  Collect(/*Arithmetic=*/false);
  if (Abort)
    return;
  const VarKeyMap<Value> Direct = std::move(Values);
  // Without the wider count, the view-only one still applies.
  Collect(/*Arithmetic=*/true);
  const VarKeyMap<Value> Through =
      Abort ? VarKeyMap<Value>() : std::move(Values);
  Budget = ::neverd::limits::ir::kHighDeadStoreElimBudget;
  // Whether bytes [Low, size) of \p E may be undefined: an undefined value a
  // byte merge kept there, directly or, at depth 0, through the definitions
  // of a local. A call or load result is defined whatever its operands are.
  std::function<bool(const ExprPtr &, uint16_t, unsigned)> UpperUndefined =
      [&](const ExprPtr &E, uint16_t Low, unsigned Depth) -> bool {
    if (!E || !Budget)
      return false;
    --Budget;
    const uint16_t Size = E->Type ? E->Type->Size : 0;
    if (Size && Low >= Size)
      return false;
    switch (E->Kind) {
    case ExprKind::Undef:
      return true;
    case ExprKind::Const:
    case ExprKind::Call:
    case ExprKind::Load:
    case ExprKind::Addr:
      return false;
    case ExprKind::Var: {
      const auto It = Through.find(varKey(E->Var));
      const bool Defined =
          It != Through.end() && !It->second.Definitions.empty();
      // A register the function never sets prints as an unknown read; the
      // entry stack pointer is the frame base instead.
      if (!Defined && E->Var.SSAVer == 0 && E->Var.RenameTag < 0 &&
          E->Var.Id < limits::kVarRenameIdBase &&
          (E->Var.Kind == MedVar::Reg || E->Var.Kind == MedVar::Flag) &&
          !isSyntheticEntryStackPointer(E->Var, Func, Architecture))
        return true;
      if (Depth == 0 && Defined)
        for (const HighStmt *D : It->second.Definitions)
          if (UpperUndefined(D->Val, Low, Depth + 1))
            return true;
      return false;
    }
    default:
      break;
    }
    const ExprPtr Only = E->Operands.size() == 1 ? E->Operands[0] : nullptr;
    const uint16_t OnlySize = Only && Only->Type ? Only->Type->Size : 0;
    if (E->Kind == ExprKind::BinOp && E->Op == NdOp::CONCAT &&
        E->Operands.size() == 2 && E->Operands[1] && E->Operands[1]->Type) {
      const uint16_t LowSize = E->Operands[1]->Type->Size;
      return UpperUndefined(E->Operands[0], Low > LowSize ? Low - LowSize : 0,
                            Depth) ||
             (Low < LowSize && UpperUndefined(E->Operands[1], Low, Depth));
    }
    if (E->Kind == ExprKind::BinOp && E->Op == NdOp::SUBBYTES &&
        E->Operands.size() == 2 && E->Operands[1] &&
        E->Operands[1]->Kind == ExprKind::Const && E->Operands[1]->ConstVal < 8)
      return UpperUndefined(E->Operands[0],
                            uint16_t(E->Operands[1]->ConstVal + Low), Depth);
    // A zero extension supplies its upper bytes; a sign extension copies the
    // operand's top byte into them.
    const bool Signed =
        (E->Kind == ExprKind::UnaryOp && E->Op == NdOp::INT_SEXT) ||
        ((E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast) && Only &&
         Only->Type && Only->Type->IsSigned);
    if (Only && OnlySize &&
        ((E->Kind == ExprKind::UnaryOp &&
          (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT)) ||
         E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast))
      return UpperUndefined(Only,
                            Signed && OnlySize < Size
                                ? std::min<uint16_t>(Low, OnlySize - 1)
                                : Low,
                            Depth);
    if (E->Kind == ExprKind::BinOp && E->Operands.size() == 2 &&
        (E->Op == NdOp::INT_AND || E->Op == NdOp::INT_OR ||
         E->Op == NdOp::INT_XOR)) {
      // An AND with a constant clear above Low keeps no operand byte there.
      if (E->Op == NdOp::INT_AND)
        for (const ExprPtr &C : E->Operands)
          if (C && C->Kind == ExprKind::Const &&
              (C->ConstVal >> (Low * 8)) == 0)
            return false;
      return UpperUndefined(E->Operands[0], Low, Depth) ||
             UpperUndefined(E->Operands[1], Low, Depth);
    }
    // Anything else may carry any operand byte upward.
    bool Found = false;
    E->forEachChildExpr([&](const ExprPtr &C) {
      Found = Found || UpperUndefined(C, 0, Depth);
    });
    return Found;
  };
  auto Narrowable = [](const Value &V) {
    return V.Valid && V.Carrier && V.Read && V.Read < V.Carrier &&
           !V.Definitions.empty();
  };
  // Decide every width before rewriting: the rewrites drop undefined reads.
  std::vector<std::pair<const Value *, uint16_t>> Widths;
  for (const auto &[Key, V] : Direct) {
    uint16_t Width = Narrowable(V) ? V.Read : 0;
    const auto It = Through.find(Key);
    if (It != Through.end() && Narrowable(It->second) &&
        It->second.Read < (Width ? Width : It->second.Carrier) &&
        std::any_of(
            It->second.Definitions.begin(), It->second.Definitions.end(),
            [&](const HighStmt *D) {
              return UpperUndefined(D->Val, It->second.Read, 0);
            }))
      Width = It->second.Read;
    if (Width)
      Widths.emplace_back(&V, Width);
  }
  // The low \p Bytes of \p Val, dropping only pure high operands.
  auto LowPrefix = [&](ExprPtr Val, uint16_t Bytes) -> ExprPtr {
    for (unsigned Depth = 0;
         Depth <
         ::neverd::limits::ir::
             kHighDeadStoreElimNarrowUnreadRegisterBytesLowPrefixDepthLimit;
         ++Depth) {
      if (!Val || !Val->Type || Val->Type->Kind != NdTypeKind::Int ||
          Val->Type->Size < Bytes || Val->IntrinsicId != Intrinsic::None ||
          !Val->IntrinsicOutputs.empty())
        return nullptr;
      if (Val->Type->Size == Bytes)
        return Val;
      const ExprPtr Only =
          Val->Operands.size() == 1 ? Val->Operands[0] : nullptr;
      if (Val->Kind == ExprKind::BinOp && Val->Op == NdOp::CONCAT &&
          Val->Operands.size() == 2 && Val->Operands[0] && Val->Operands[1] &&
          Val->Operands[1]->Type &&
          Val->Operands[1]->Type->Kind == NdTypeKind::Int &&
          Val->Operands[1]->Type->Size >= Bytes) {
        if (!discardableIntegerValue(Val->Operands[0], Budget))
          return nullptr;
        Val = Val->Operands[1];
        continue;
      }
      if (Only && Only->Type && Only->Type->Kind == NdTypeKind::Int &&
          Only->Type->Size >= Bytes &&
          ((Val->Kind == ExprKind::UnaryOp &&
            (Val->Op == NdOp::INT_ZEXT || Val->Op == NdOp::INT_SEXT)) ||
           (Val->Kind == ExprKind::Cast && Val->CastTo &&
            Val->CastTo->Kind == NdTypeKind::Int))) {
        Val = Only;
        continue;
      }
      break;
    }
    auto Prefix =
        HighExpr::makeBinop(NdOp::SUBBYTES, Val, HighExpr::makeConst(0, 4));
    Prefix->Type = NdType::makeInt(Bytes, false);
    return Prefix;
  };
  for (const auto &[V, Width] : Widths) {
    std::vector<ExprPtr> Prefixes;
    for (HighStmt *S : V->Definitions) {
      ExprPtr Prefix = LowPrefix(S->Val, Width);
      if (!Prefix)
        break;
      Prefixes.push_back(std::move(Prefix));
    }
    if (Prefixes.size() != V->Definitions.size())
      continue;
    for (size_t I = 0; I < Prefixes.size(); ++I) {
      HighStmt &S = *V->Definitions[I];
      auto Extended = HighExpr::makeUnary(NdOp::INT_ZEXT, Prefixes[I]);
      Extended->Type = S.Dst->Type;
      S.Val = std::move(Extended);
    }
  }
}

/// The frame addresses that a contiguous entry prefix of single-definition
/// pointer-width assignments names (`v0 = RSP - 32`). \p Statements receives
/// those assignments. With \p PastStores the prefix also runs over plain
/// stores, which assign no variable (`*(v0 - 4) = EAX; t1 = v0 - 4`).
static VarKeyMap<ExprPtr>
entryFrameAliases(const HighFunc &Func, Arch Architecture, size_t &Budget,
                  std::unordered_set<const HighStmt *> *Statements = nullptr,
                  bool PastStores = false) {
  const auto &TRI = getTargetRegInfo(Architecture);
  VarKeyMap<unsigned> Definitions;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (!Budget)
      return;
    --Budget;
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var)
      ++Definitions[varKey(S.Dst->Var)];
  });
  // Only a contiguous entry prefix establishes aliases. Its single-definition
  // addresses dominate all later uses without assuming arbitrary HighIR SSA
  // definitions dominate their uses across branches or exceptional paths.
  VarKeyMap<ExprPtr> Aliases;
  for (const auto &S : Func.Body) {
    if (high_detail::isEmptyBlockAnchor(S))
      continue;
    if (PastStores && S.Kind == StmtKind::Store && S.Body.empty() &&
        S.ElseBody.empty() && S.Cases.empty() && S.DefaultBody.empty() &&
        S.EHClauseBodies.empty())
      continue;
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val ||
        S.Dst->Kind != ExprKind::Var || !S.Dst->Type ||
        S.Dst->Type->Size != TRI.PointerSize ||
        S.Dst->Var.Size != TRI.PointerSize ||
        S.MemoryOrdering != NdMemoryOrdering::None ||
        S.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        !S.Body.empty() || !S.ElseBody.empty() || !S.Cases.empty() ||
        !S.DefaultBody.empty() || !S.EHClauseBodies.empty() ||
        Definitions[varKey(S.Dst->Var)] != 1 ||
        !high_detail::frameAddressOffset(S.Val, Func, Architecture, Budget, 0,
                                         &Aliases))
      break;
    Aliases.emplace(varKey(S.Dst->Var), S.Val);
    if (Statements)
      Statements->insert(&S);
  }
  return Aliases;
}

void elimUnreadPrivateFrameStores(HighFunc &Func, Arch Architecture) {
  if (Func.FrameSize <= 0 || Architecture == Arch::Unknown ||
      Func.StructuredExceptionRegions || Func.UnstructuredExceptionRegions)
    return;
  const auto &TRI = getTargetRegInfo(Architecture);
  if (TRI.PointerSize != 4 && TRI.PointerSize != 8)
    return;
  size_t Budget = ::neverd::limits::ir::
      kHighDeadStoreElimElimUnreadPrivateFrameStoresBudget;
  std::unordered_set<const HighStmt *> AliasStatements;
  const VarKeyMap<ExprPtr> Aliases =
      entryFrameAliases(Func, Architecture, Budget, &AliasStatements);
  const auto Address = [&](const ExprPtr &E, uint16_t Bytes) {
    auto At = high_detail::frameAddressOffset(E, Func, Architecture, Budget, 0,
                                              &Aliases);
    if (!At || !Bytes || *At < -Func.FrameSize || *At >= 0 ||
        uint64_t(Bytes) > uint64_t(-*At))
      return std::optional<int64_t>{};
    return At;
  };
  struct Candidate {
    HighStmt *Statement;
    int64_t Offset;
    uint16_t Bytes;
  };
  std::vector<Candidate> Candidates;
  std::set<int64_t> ReadBytes;
  const auto RecordReadBytes = [&](int64_t At, uint16_t Bytes) {
    if (Bytes > Budget) {
      Budget = 0;
      return false;
    }
    Budget -= Bytes;
    for (unsigned I = 0; I < Bytes; ++I)
      ReadBytes.insert(At + I);
    return true;
  };
  HighExprSet Seen;
  bool Escaped = false;
  walkStmts(Func.Body, [&](HighStmt &S) {
    if (Escaped || !Budget || AliasStatements.count(&S))
      return;
    --Budget;
    bool PrivateStore = false;
    if (S.Kind == StmtKind::Store && S.StoreVal && S.StoreVal->Type &&
        S.Body.empty() && S.ElseBody.empty() && S.Cases.empty() &&
        S.DefaultBody.empty() && S.EHClauseBodies.empty() &&
        S.MemoryOrdering == NdMemoryOrdering::None &&
        S.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      if (const auto At = Address(S.StoreAddr, S.StoreVal->Type->Size)) {
        PrivateStore = true;
        if (discardableIntegerValue(S.StoreVal, Budget))
          Candidates.push_back({&S, *At, S.StoreVal->Type->Size});
      }
    }
    forEachExpr(S, [&](const ExprPtr &Root) {
      if (PrivateStore && &Root == &S.StoreAddr)
        return;
      std::vector<const HighExpr *> Pending{Root.get()};
      while (!Pending.empty() && !Escaped && Budget) {
        const auto *E = Pending.back();
        Pending.pop_back();
        if (!E || !Seen.insert(E).second)
          continue;
        --Budget;
        if (E->IntrinsicId != Intrinsic::None)
          Escaped = true;
        HighExprSet BoundedFrameInputs;
        if (E->Kind == ExprKind::Load && E->Type && E->Operands.size() == 1 &&
            E->MemoryOrdering == NdMemoryOrdering::None &&
            E->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
          if (const auto At = Address(E->Operands[0], E->Type->Size)) {
            if (!RecordReadBytes(*At, E->Type->Size))
              return;
            continue;
          }
        }
        if (E->Kind == ExprKind::Call) {
          std::string Error;
          if (!E->SourceCallHint ||
              !validateSourceABI(E->SourceCallHint->Signature, Error) ||
              E->Operands.size() !=
                  E->SourceCallHint->Signature.Parameters.size())
            Escaped = true;
          else if (E->SourceCallHint->CallKind ==
                   SourceCallTypeHint::Kind::ObjCSuper2) {
            // objc_msgSendSuper2 consumes the two-pointer objc_super record
            // synchronously and passes only its receiver to the selected
            // method. Treat that exact record as a bounded read instead of
            // exposing every otherwise private byte in the caller's frame.
            const uint16_t Bytes = 2 * TRI.PointerSize;
            if (!E->Operands.empty())
              if (const auto At = Address(E->Operands[0], Bytes)) {
                if (!RecordReadBytes(*At, Bytes))
                  return;
                BoundedFrameInputs.insert(E->Operands[0].get());
              }
          }
        }
        if (E->Kind == ExprKind::Addr && E->Operands.size() == 1 &&
            E->Operands[0] && E->Operands[0]->Kind == ExprKind::Var &&
            E->Operands[0]->Var.Kind == MedVar::Stack) {
          Escaped = true;
        } else if (E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Stack) {
          // A stack variable is the value stored in that exact slot, not the
          // slot's address. Incoming nonnegative slots cannot expose private
          // frame bytes; local negative slots contribute only their concrete
          // overlap to byte liveness. Taking a slot's address remains a
          // conservative escape in the separate case above.
          const uint16_t Bytes = E->Type ? E->Type->Size : 0;
          if (!Bytes || E->Var.Size != Bytes ||
              E->Var.StackOff < -Func.FrameSize) {
            Escaped = true;
          } else if (E->Var.StackOff < 0) {
            const auto PrivateBytes = static_cast<uint16_t>(
                std::min<uint64_t>(Bytes, uint64_t(-E->Var.StackOff)));
            if (!RecordReadBytes(E->Var.StackOff, PrivateBytes))
              return;
          }
        } else if (E->Kind == ExprKind::Var &&
                   (Aliases.count(varKey(E->Var)) ||
                    (E->Var.Kind == MedVar::Reg &&
                     (E->Var.RegOff == TRI.StackPointer ||
                      E->Var.RegOff == TRI.FramePointer)))) {
          Escaped = true;
        }
        for (const auto &Operand : E->Operands)
          if (!BoundedFrameInputs.count(Operand.get()))
            Pending.push_back(Operand.get());
      }
    });
  });
  if (Escaped || !Budget)
    return;
  for (const auto &[S, At, Bytes] : Candidates) {
    const auto First = ReadBytes.lower_bound(At);
    const auto End = ReadBytes.lower_bound(At + Bytes);
    if (First == End) {
      // Preserve a possible branch destination at the eliminated write.
      const va_t Address = S->Addr;
      *S = HighStmt{};
      S->Kind = StmtKind::Block;
      S->Addr = Address;
    } else if (S->StoreVal->Type->Kind == NdTypeKind::Int && Bytes <= 16) {
      const auto Kept = uint16_t(*std::prev(End) - At + 1);
      if (Kept < Bytes)
        S->StoreVal = frameValuePrefix(S->StoreVal, Kept);
    }
  }
}

namespace {

/// Whether anything can observe the value a private frame store writes before
/// the function overwrites it: the control-flow form of "the function reads
/// the bytes back". A read that every path reaches only after an overwrite
/// sees the overwriting value, never the stored one.
///
/// The walk follows statement flow from the store with the bytes that still
/// hold its value. A plain store at a proven frame offset ends its bytes. A
/// load of a pending byte observes it, and so does any use that may read the
/// frame where the proof cannot say: a load through a frame address it cannot
/// place, and a call, intrinsic or atomic operation that receives one. Frame
/// addresses are the stack pointer's values and what assignments compute from
/// them. A function that stores one to memory may reload it under a name this
/// propagation never sees, and proves nothing.
class FrameStoreObservers {
public:
  FrameStoreObservers(const HighFunc &Func, Arch Architecture)
      : Func(Func), Architecture(Architecture),
        TRI(getTargetRegInfo(Architecture)),
        // Every statement continues: a path too many only adds observers.
        Graph(Func.Body, [](const HighStmt &) { return false; }) {
    // An unwinder enters a handler, and a handler its parent's continuation,
    // along no edge of the graph; a jump it cannot resolve leaves it
    // incomplete.
    Usable = Graph.Complete && Func.CxxContinuationTargets.empty() &&
             Func.SEHHandlerUnwindTargets.empty() &&
             Func.SEHLocalUnwindExits.empty() && !Func.HasUnresolvedSEHFrame;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      Usable &= S.Kind != StmtKind::SEHTry && S.Kind != StmtKind::CxxTry &&
                S.Kind != StmtKind::ItaniumTry && S.EHClauses.empty() &&
                S.EHClauseBodies.empty() &&
                S.CxxContinuationReturnTargets.empty();
    });
    if (!Usable)
      return;
    Aliases = entryFrameAliases(Func, Architecture, Budget, nullptr,
                                /*PastStores=*/true);
    collectFrameAddresses();
  }

  /// Whether nothing observes the value \p Store writes to the \p Bytes
  /// frame bytes at \p Offset. With \p NoValue the calling convention gives
  /// the stored register no value: only a read by this function, or through
  /// a frame address it hands on, counts. Any other value may be a real one.
  /// The proof then also requires that no call runs before the bytes are
  /// overwritten, nor before the store: a callee may read a stack slot no
  /// operand of its call names, and a function that has made no call has no
  /// place where a later fault or jump could resume it (setjmp).
  bool unobserved(const HighStmt &Store, int64_t Offset, uint16_t Bytes,
                  bool NoValue) {
    const auto Start = Graph.NodeOf.find(&Store);
    if (!Usable || !Bytes || Bytes > 64 || Start == Graph.NodeOf.end())
      return false;
    const auto Observes = [&](const Effects &E) {
      return E.Opaque || (!NoValue && E.Calls);
    };
    // A frame address handed on before the store could be read through while
    // the bytes are pending.
    for (const size_t Node : nodesBefore(Start->second)) {
      if (!Budget)
        return false;
      --Budget;
      if (Observes(effectsOf(Node)))
        return false;
    }
    const auto Mask = [&](const std::pair<int64_t, int64_t> &Range) {
      uint64_t Bits = 0;
      for (int64_t Byte = std::max(Range.first, Offset);
           Byte < std::min(Range.second, Offset + int64_t(Bytes)); ++Byte)
        Bits |= uint64_t{1} << (Byte - Offset);
      return Bits;
    };
    const uint64_t All =
        Bytes == 64 ? ~uint64_t{0} : (uint64_t{1} << Bytes) - 1;
    std::vector<std::pair<size_t, uint64_t>> Work;
    for (const size_t Next : Graph.Nodes[Start->second].Successors)
      Work.emplace_back(Next, All);
    std::unordered_map<size_t, uint64_t> Visited;
    while (!Work.empty()) {
      auto [Node, Pending] = Work.back();
      Work.pop_back();
      // The frame ends with the function.
      if (Node == 0)
        continue;
      uint64_t &Seen = Visited[Node];
      Pending &= ~Seen;
      if (!Pending)
        continue;
      Seen |= Pending;
      if (!Budget)
        return false;
      --Budget;
      const Effects &E = effectsOf(Node);
      if (Observes(E))
        return false;
      for (const auto &Read : E.Reads)
        if (Pending & Mask(Read))
          return false;
      if (E.Write)
        Pending &= ~Mask(*E.Write);
      if (!Pending)
        continue;
      for (const size_t Next : Graph.Nodes[Node].Successors)
        Work.emplace_back(Next, Pending);
    }
    return Budget != 0;
  }

private:
  /// What running one statement's own expressions does to the frame.
  struct Effects {
    /// The frame bytes its loads read, as entry-stack offsets.
    std::vector<std::pair<int64_t, int64_t>> Reads;
    /// The frame bytes its plain store writes, after those reads.
    std::optional<std::pair<int64_t, int64_t>> Write;
    /// It may read frame bytes the proof cannot name, or hands a frame
    /// address to code that may.
    bool Opaque = false;
    /// It makes a call, which may also come back a second time.
    bool Calls = false;
  };
  using Memo = std::unordered_map<const HighExpr *, bool>;

  const HighFunc &Func;
  Arch Architecture;
  const TargetRegInfo &TRI;
  HighFlowGraph Graph;
  size_t Budget =
      ::neverd::limits::ir::kHighDeadStoreElimFrameStoreObserversBudget;
  bool Usable = false;
  VarKeyMap<ExprPtr> Aliases;
  /// Variables some assignment gives a value computed from a frame address.
  VarKeySet Derived;
  std::unordered_map<size_t, Effects> Cache;
  std::vector<std::vector<size_t>> Predecessors;

  bool frameAddressVar(const MedVar &V) const {
    return (V.Kind == MedVar::Reg && TRI.isStackPointer(V.RegOff)) ||
           Aliases.count(varKey(V)) || Derived.count(varKey(V));
  }

  /// An operation whose result is computed from its operands' bits alone, so
  /// a frame address among them may be one again in the result.
  static bool carriesAddress(const HighExpr &E) {
    if (E.MemoryOrdering != NdMemoryOrdering::None ||
        E.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        E.IntrinsicId != Intrinsic::None || !E.IntrinsicOutputs.empty() ||
        E.IndirectTarget)
      return false;
    switch (E.Kind) {
    case ExprKind::Cast:
    case ExprKind::BitCast:
      return true;
    case ExprKind::UnaryOp:
      return isTrapFreeIntegerUnaryOp(E.Op);
    case ExprKind::BinOp:
      return isTrapFreeIntegerBinOp(E.Op) || E.Op == NdOp::SELECT ||
             E.Op == NdOp::CONCAT || E.Op == NdOp::SUBBYTES;
    default:
      return false;
    }
  }

  /// Records in \p Out what evaluating \p E reads of the frame, and returns
  /// whether its value may be a frame address.
  bool scan(const ExprPtr &E, Effects &Out, Memo &Seen, unsigned Depth = 0) {
    if (!E)
      return false;
    if (const auto Known = Seen.find(E.get()); Known != Seen.end())
      return Known->second;
    if (!Budget || Depth >
                       ::neverd::limits::ir::
                           kHighDeadStoreElimFrameStoreObserversDepthLimit) {
      Budget = 0;
      Out.Opaque = true;
      return true;
    }
    --Budget;
    const auto Operands = [&] {
      bool Any = false;
      E->forEachChildExpr(
          [&](const ExprPtr &C) { Any |= scan(C, Out, Seen, Depth + 1); });
      return Any;
    };
    bool Address = false;
    switch (E->Kind) {
    case ExprKind::Const:
    case ExprKind::Undef:
      break;
    case ExprKind::IncomingStack:
      Address = true;
      break;
    case ExprKind::EntryRegister:
      Address = E->Var.Kind == MedVar::Reg && TRI.isStackPointer(E->Var.RegOff);
      break;
    case ExprKind::Var:
      if (E->Var.Kind != MedVar::Stack) {
        Address = frameAddressVar(E->Var);
      } else if (E->Var.StackOff < 0) {
        // The slot's contents, which no store here made a frame address.
        const int64_t Size =
            std::max<int64_t>(E->Var.Size, E->Type ? E->Type->Size : 0);
        Out.Reads.emplace_back(E->Var.StackOff, E->Var.StackOff + Size);
      }
      break;
    case ExprKind::Phi:
      Address = frameAddressVar(E->Var);
      Address |= Operands();
      break;
    case ExprKind::Addr:
      Address = E->Operands.size() == 1 && E->Operands[0] &&
                E->Operands[0]->Kind == ExprKind::Var &&
                E->Operands[0]->Var.Kind == MedVar::Stack;
      if (!Address)
        Address = Operands();
      break;
    case ExprKind::Load: {
      std::optional<int64_t> At;
      if (E->Operands.size() == 1 && E->Type && E->Type->Size &&
          E->MemoryOrdering == NdMemoryOrdering::None &&
          E->MemoryAddressSpace == NdMemoryAddressSpace::Default)
        At = high_detail::frameAddressOffset(E->Operands[0], Func, Architecture,
                                             Budget, 0, &Aliases);
      if (At)
        Out.Reads.emplace_back(*At, *At + int64_t(E->Type->Size));
      else
        Out.Opaque |= Operands();
      break;
    }
    case ExprKind::Call: {
      const bool Receives = Operands();
      // A fault check tests the address itself and reads nothing through it.
      Out.Opaque |= Receives && !(E->IntrinsicId != Intrinsic::None &&
                                  onlyChecksForAFault(E->IntrinsicId));
      Out.Calls |= E->IntrinsicId == Intrinsic::None ||
                   intrinsicReturnsSystemCallResult(E->IntrinsicId) ||
                   intrinsicReturnsSmcccResults(E->IntrinsicId);
      break;
    }
    case ExprKind::Cast:
    case ExprKind::BitCast:
    case ExprKind::UnaryOp:
    case ExprKind::BinOp:
      if (Operands()) {
        Address = carriesAddress(*E);
        Out.Opaque |= !Address;
      }
      break;
    default:
      Out.Opaque |= Operands();
      break;
    }
    Seen.emplace(E.get(), Address);
    return Address;
  }

  bool holdsFrameAddress(const ExprPtr &E) {
    Effects Ignored;
    Memo Seen;
    return scan(E, Ignored, Seen);
  }

  void collectFrameAddresses() {
    // A stack slot is memory; any other variable is a name for its value.
    const auto Names = [](const HighStmt &S) {
      return S.Kind == StmtKind::Assign && S.Dst &&
             S.Dst->Kind == ExprKind::Var && S.Dst->Var.Kind != MedVar::Stack;
    };
    for (bool Changed = true; Changed && Budget;) {
      Changed = false;
      walkStmts(Func.Body, [&](const HighStmt &S) {
        if (!Names(S) || Derived.count(varKey(S.Dst->Var)) ||
            !holdsFrameAddress(S.Val))
          return;
        Derived.insert(varKey(S.Dst->Var));
        Changed = true;
      });
    }
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (!Usable)
        return;
      if (S.Kind == StmtKind::Store)
        Usable = !holdsFrameAddress(S.StoreVal);
      else if (S.Kind == StmtKind::Assign && !Names(S))
        Usable = !holdsFrameAddress(S.Val);
    });
    Usable &= Budget != 0;
  }

  const Effects &effectsOf(size_t Node) {
    const auto [It, Added] = Cache.try_emplace(Node);
    Effects &Out = It->second;
    if (!Added)
      return Out;
    const HighStmt &S = *Graph.Nodes[Node].Statement;
    Memo Seen;
    switch (S.Kind) {
    case StmtKind::Store: {
      const uint16_t Bytes =
          S.StoreVal && S.StoreVal->Type ? S.StoreVal->Type->Size : 0;
      std::optional<int64_t> At;
      if (Bytes && S.StoreAddr && S.MemoryOrdering == NdMemoryOrdering::None &&
          S.MemoryAddressSpace == NdMemoryAddressSpace::Default)
        At = high_detail::frameAddressOffset(S.StoreAddr, Func, Architecture,
                                             Budget, 0, &Aliases);
      // A write the proof cannot place reads nothing: only what computing
      // its address reads counts.
      if (At)
        Out.Write = std::pair{*At, *At + int64_t(Bytes)};
      else
        scan(S.StoreAddr, Out, Seen);
      scan(S.StoreVal, Out, Seen);
      break;
    }
    case StmtKind::Assign:
      if (S.Dst && S.Dst->Kind != ExprKind::Var) {
        bool Through = false;
        S.Dst->forEachChildExpr(
            [&](const ExprPtr &C) { Through |= scan(C, Out, Seen); });
        Out.Opaque |= Through;
      }
      scan(S.Val, Out, Seen);
      break;
    default:
      forEachExpr(S, [&](const ExprPtr &Root) { scan(Root, Out, Seen); });
      break;
    }
    Out.Opaque |= Budget == 0;
    return Out;
  }

  /// The statements that may run before \p Node does.
  std::vector<size_t> nodesBefore(size_t Node) {
    if (Predecessors.empty()) {
      Predecessors.resize(Graph.Nodes.size());
      for (size_t From = 1; From < Graph.Nodes.size(); ++From)
        for (const size_t To : Graph.Nodes[From].Successors)
          Predecessors[To].push_back(From);
    }
    std::vector<size_t> Before;
    std::vector<bool> Reached(Graph.Nodes.size());
    std::vector<size_t> Work{Node};
    while (!Work.empty()) {
      const size_t At = Work.back();
      Work.pop_back();
      for (const size_t From : Predecessors[At])
        if (!Reached[From]) {
          Reached[From] = true;
          Before.push_back(From);
          Work.push_back(From);
        }
    }
    return Before;
  }
};

} // namespace

void elimUndefinedEntryScratchStores(HighFunc &Func, Arch Architecture,
                                     BinaryFormat Format) {
  // A variadic prologue spills what its caller passed in argument registers,
  // the AL vector count's XMM0-7 and AArch64's Q0-7 among them.
  if (Func.FrameSize <= 0 || Func.IsVariadic ||
      Func.StructuredExceptionRegions || Func.UnstructuredExceptionRegions)
    return;
  const auto &TRI = getTargetRegInfo(Architecture);
  if (TRI.PointerSize != 4 && TRI.PointerSize != 8)
    return;
  // Only a store of a register's incoming value can go: most functions have
  // none, and need nothing more than this look.
  bool StoresEntryRegister = false;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    StoresEntryRegister |= S.Kind == StmtKind::Store && S.StoreVal &&
                           S.StoreVal->Kind == ExprKind::Var &&
                           S.StoreVal->Var.Kind == MedVar::Reg &&
                           S.StoreVal->Var.SSAVer == 0;
  });
  if (!StoresEntryRegister)
    return;
  const CallArgumentConvention *Convention =
      callArgumentConvention(Architecture, Format);
  const IntegerArgumentLayout Arguments = TRI.integerArgumentLayout(Format);
  size_t Budget = ::neverd::limits::ir::
      kHighDeadStoreElimElimUndefinedEntryScratchStoresBudget;
  const VarKeyMap<ExprPtr> Aliases =
      entryFrameAliases(Func, Architecture, Budget);
  // HighIR's version 0 also names values a statement defines, such as a
  // call's result: only one that no statement assigns is the incoming value.
  VarKeySet Assigned;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Dst && S.Dst->Kind == ExprKind::Var)
      Assigned.insert(varKey(S.Dst->Var));
  });
  // The incoming value of a scratch register the function takes no parameter
  // in: one C can only print as unknown, wherever the convention may still
  // give it a value (i386 EAX, which a regparm caller sets).
  const auto UnknownScratch = [&](const MedVar &V) {
    return V.Kind == MedVar::Reg && V.SSAVer == 0 && V.RenameTag < 0 &&
           TRI.isGeneralReg(V.RegOff) && !TRI.isStackPointer(V.RegOff) &&
           !TRI.isLinkRegister(V.RegOff) &&
           !TRI.isPlatformRegister(V.RegOff, Format) &&
           !TRI.callPreservedPrefixSize(V.RegOff, V.Size, Format) &&
           llvm::none_of(
               Func.Params,
               [&](const HighParam &Param) {
                 return Param.RegOff == V.RegOff;
               });
  };
  struct Candidate {
    HighStmt *Statement;
    int64_t Offset;
    uint16_t Bytes;
    /// The convention gives the stored register no value at entry.
    bool Undefined;
  };
  std::vector<Candidate> Candidates;
  bool AnyUndefined = false;
  walkStmts(Func.Body, [&](HighStmt &S) {
    if (!Budget || S.Kind != StmtKind::Store || !S.StoreAddr || !S.StoreVal ||
        S.StoreVal->Kind != ExprKind::Var || !S.StoreVal->Type ||
        S.StoreVal->Type->Size != S.StoreVal->Var.Size ||
        S.MemoryOrdering != NdMemoryOrdering::None ||
        S.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        !S.Body.empty() || !S.ElseBody.empty() || !S.Cases.empty() ||
        !S.DefaultBody.empty() || !S.EHClauseBodies.empty() ||
        Assigned.count(varKey(S.StoreVal->Var)))
      return;
    const bool Undefined = isUndefinedIncomingScratch(
        TRI, Architecture, Convention, Arguments.Registers, S.StoreVal->Var);
    if (!Undefined && !UnknownScratch(S.StoreVal->Var))
      return;
    --Budget;
    // Where the convention gives the stored bits no value, the function's
    // own frame bytes below the entry stack pointer held none before the
    // store and hold none after it.
    const uint16_t Bytes = S.StoreVal->Type->Size;
    const auto At = high_detail::frameAddressOffset(
        S.StoreAddr, Func, Architecture, Budget, 0, &Aliases);
    if (At && Bytes && *At >= -Func.FrameSize && *At < 0 &&
        uint64_t(Bytes) <= uint64_t(-*At)) {
      Candidates.push_back({&S, *At, Bytes, Undefined});
      AnyUndefined |= Undefined;
    }
  });
  if (Candidates.empty() || !Budget)
    return;
  // A read of those bytes must still fail where it uses the value, rather
  // than read a slot nothing wrote: keep each store the function reads back.
  std::set<int64_t> ReadBytes;
  const auto RecordRead = [&](int64_t At, uint64_t Bytes) {
    if (Bytes > Budget) {
      Budget = 0;
      return;
    }
    Budget -= Bytes;
    for (uint64_t I = 0; I < Bytes; ++I)
      ReadBytes.insert(At + int64_t(I));
  };
  HighExprSet Seen;
  if (AnyUndefined)
    walkStmts(Func.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &Root) {
        std::vector<const HighExpr *> Pending{Root.get()};
        if (S.Kind == StmtKind::Store && &Root == &S.StoreAddr &&
            high_detail::frameAddressOffset(S.StoreAddr, Func, Architecture,
                                            Budget, 0, &Aliases))
          Pending.clear();
        while (!Pending.empty() && Budget) {
          const HighExpr *E = Pending.back();
          Pending.pop_back();
          if (!E || !Seen.insert(E).second)
            continue;
          --Budget;
          if (E->Kind == ExprKind::Load && E->Type && E->Operands.size() == 1)
            if (const auto At = high_detail::frameAddressOffset(
                    E->Operands[0], Func, Architecture, Budget, 0, &Aliases)) {
              RecordRead(*At, E->Type->Size);
              continue;
            }
          // A stack slot's value, or its address, which may be read through.
          const HighExpr *Slot =
              E->Kind == ExprKind::Addr && E->Operands.size() == 1
                  ? E->Operands[0].get()
                  : E;
          if (Slot && Slot->Kind == ExprKind::Var &&
              Slot->Var.Kind == MedVar::Stack && Slot->Var.StackOff < 0) {
            RecordRead(Slot->Var.StackOff, Slot->Var.Size);
            continue;
          }
          for (const auto &Operand : E->Operands)
            Pending.push_back(Operand.get());
        }
      });
    });
  if (!Budget)
    return;
  // Only a read that can still see the stored value reads it back: one that
  // every path reaches after an overwrite does not. Every proof reads the
  // function as it is, so the stores go together afterwards.
  std::optional<FrameStoreObservers> Observers;
  std::vector<HighStmt *> Unobserved;
  for (const auto &[S, At, Bytes, Undefined] : Candidates) {
    if (!Undefined ||
        ReadBytes.lower_bound(At) != ReadBytes.lower_bound(At + Bytes)) {
      if (!Observers)
        Observers.emplace(Func, Architecture);
      if (!Observers->unobserved(*S, At, Bytes, Undefined))
        continue;
    }
    Unobserved.push_back(S);
  }
  for (HighStmt *S : Unobserved) {
    // Preserve a possible branch destination at the eliminated write.
    const va_t Address = S->Addr;
    *S = HighStmt{};
    S->Kind = StmtKind::Block;
    S->Addr = Address;
  }
}

//===----------------------------------------------------------------------===//
// Consecutive dead store elimination
//===----------------------------------------------------------------------===//

void elimConsecutiveDeadStores(std::vector<HighStmt> &Stmts) {
  Stmts.erase(std::remove_if(Stmts.begin(), Stmts.end(),
                             [](const HighStmt &S) {
                               return S.Kind == StmtKind::Nop &&
                                      (!S.Addr || S.Addr == InvalidVA);
                             }),
              Stmts.end());

  for (size_t I = 0; I + 1 < Stmts.size(); ++I) {
    auto &CurrStmt = Stmts[I];
    auto &NextStmt = Stmts[I + 1];
    if (CurrStmt.Kind != StmtKind::Assign || NextStmt.Kind != StmtKind::Assign)
      continue;
    if (CurrStmt.Val && CurrStmt.Val->hasOrderedMemoryAccess())
      continue;
    if (!CurrStmt.Dst || !NextStmt.Dst)
      continue;
    if (CurrStmt.Dst->Kind != ExprKind::Var ||
        NextStmt.Dst->Kind != ExprKind::Var)
      continue;

    const auto &Current = CurrStmt.Dst->Var;
    const auto &Next = NextStmt.Dst->Var;
    // Physical registers are reused by distinct SSA values. Equal right-hand
    // sides do not make those destinations one mutable local, and a widening
    // conversion does not make the narrow and wide values interchangeable.
    const bool SameVariable =
        Current.Kind == Next.Kind && Current.TheArch == Next.TheArch &&
        Current.Id == Next.Id && Current.SSAVer == Next.SSAVer &&
        Current.RenameTag == Next.RenameTag && Current.Size == Next.Size &&
        Current.RegOff == Next.RegOff;
    if (!SameVariable || !CurrStmt.Val || !NextStmt.Val)
      continue;

    bool NextUsesCurr = false;
    HighExprSet Seen;
    std::function<void(const ExprPtr &)> CheckRef = [&](const ExprPtr &E) {
      if (!E || NextUsesCurr || !Seen.insert(E.get()).second)
        return;
      if (E->Kind == ExprKind::Var && E->Var == CurrStmt.Dst->Var)
        NextUsesCurr = true;
      for (auto &Op : E->Operands)
        CheckRef(Op);
    };
    if (NextStmt.Val)
      CheckRef(NextStmt.Val);

    if (NextUsesCurr)
      continue;

    bool HasEffect = false;
    std::vector<const HighExpr *> Pending{CurrStmt.Val.get()};
    HighExprSet EffectSeen;
    while (!Pending.empty()) {
      const auto *Expression = Pending.back();
      Pending.pop_back();
      if (!Expression || !EffectSeen.insert(Expression).second)
        continue;
      HasEffect |= Expression->Kind == ExprKind::Call ||
                   Expression->Kind == ExprKind::Store;
      for (const auto &Operand : Expression->Operands)
        Pending.push_back(Operand.get());
    }
    if (CurrStmt.Val->Kind == ExprKind::Call) {
      CurrStmt.Kind = StmtKind::Call;
      CurrStmt.CallExpr = CurrStmt.Val;
      CurrStmt.Dst = nullptr;
      CurrStmt.Val = nullptr;
    } else if (HasEffect) {
      CurrStmt.Kind = StmtKind::ExprStmt;
      CurrStmt.Dst = nullptr;
    } else {
      Stmts.erase(Stmts.begin() + static_cast<long>(I));
      --I;
    }
  }

  for (auto &S : Stmts) {
    elimConsecutiveDeadStores(S.Body);
    elimConsecutiveDeadStores(S.ElseBody);
    for (auto &C : S.Cases)
      elimConsecutiveDeadStores(C.Body);
    elimConsecutiveDeadStores(S.DefaultBody);
  }
}

} // namespace neverd
