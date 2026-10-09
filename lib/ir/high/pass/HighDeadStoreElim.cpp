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
///   - Source-local CONCAT values with unobserved upper bytes
///
/// See also:
///   HighDCE.cpp         — main DCE orchestration, iterative liveness DCE
///   HighCopyProp.cpp    — copy propagation and alias resolution
///   HighDCEDetail.h     — shared declarations
///
//===----------------------------------------------------------------------===//

#include "HighDCEDetail.h"
#include "HighFrameAddress.h"

#include "neverd/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <functional>
#include <set>
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

// Only remove or slice values whose evaluation cannot read memory, trap or
// call another function. Unknown bits may be discarded only when their exact
// bytes have no reads; this never supplies a replacement bit value.
bool discardableIntegerValue(const ExprPtr &Root, size_t &Budget) {
  std::vector<const HighExpr *> Pending{Root.get()};
  std::unordered_set<const HighExpr *> Seen;
  while (!Pending.empty()) {
    const auto *E = Pending.back();
    Pending.pop_back();
    if (!E || !Budget)
      return false;
    if (!Seen.insert(E).second)
      continue;
    --Budget;
    if (!E->Type || !E->Type->Size || E->IntrinsicId != Intrinsic::None ||
        !E->IntrinsicOutputs.empty() ||
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
      if (isTrapFreeFloatOp(E->Op) && E->Operands.size() == 2 &&
          E->Operands[0] && E->Operands[1])
        break;
      if (E->Type->Kind != NdTypeKind::Int || E->Operands.size() != 2 ||
          !E->Operands[0] || !E->Operands[0]->Type ||
          E->Operands[0]->Type->Kind != NdTypeKind::Int || !E->Operands[1] ||
          !E->Operands[1]->Type ||
          E->Operands[1]->Type->Kind != NdTypeKind::Int)
        return false;
      if (E->Op == NdOp::CONCAT) {
        if (E->Type->Size !=
            E->Operands[0]->Type->Size + E->Operands[1]->Type->Size)
          return false;
      } else if (E->Op == NdOp::SUBBYTES) {
        if (E->Operands[1]->Kind != ExprKind::Const ||
            E->Operands[1]->ConstVal > E->Operands[0]->Type->Size ||
            E->Type->Size >
                E->Operands[0]->Type->Size - E->Operands[1]->ConstVal)
          return false;
      } else if (!isTrapFreeIntegerBinOp(E->Op)) {
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

bool harmlessIntegerValue(const ExprPtr &Root, size_t &Budget) {
  std::vector<const HighExpr *> Pending{Root.get()};
  std::unordered_set<const HighExpr *> Seen;
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
  size_t Budget = 100000;
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
    for (unsigned Depth = 0; Value && Depth < 16; ++Depth) {
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
    if (!Budget-- || Depth > 128)
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
      for (unsigned Depth = 0; Depth <= 8; ++Depth) {
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
    if (!Budget-- || Depth > 128 || !E)
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
  constexpr size_t kBudget = 200000;
  size_t Budget = kBudget;
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
    Budget = kBudget;
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
  Budget = kBudget;
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
    for (unsigned Depth = 0; Depth < 16; ++Depth) {
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

void elimUnreadPrivateFrameStores(HighFunc &Func, Arch Architecture) {
  if (Func.FrameSize <= 0 || Architecture == Arch::Unknown ||
      Func.StructuredExceptionRegions || Func.UnstructuredExceptionRegions)
    return;
  const auto &TRI = getTargetRegInfo(Architecture);
  if (TRI.PointerSize != 4 && TRI.PointerSize != 8)
    return;
  size_t Budget = 100000;
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
  std::unordered_set<const HighStmt *> AliasStatements;
  for (const auto &S : Func.Body) {
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
    AliasStatements.insert(&S);
  }
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
  std::unordered_set<const HighExpr *> Seen;
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
        std::unordered_set<const HighExpr *> BoundedFrameInputs;
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
    std::unordered_set<const HighExpr *> Seen;
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
    std::unordered_set<const HighExpr *> EffectSeen;
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
