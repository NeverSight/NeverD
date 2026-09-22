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

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <functional>
#include <set>
#include <unordered_set>

namespace neverd {

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
    case ExprKind::Cast:
    case ExprKind::UnaryOp:
      if (E->Type->Kind != NdTypeKind::Int || E->Operands.size() != 1 ||
          !E->Operands[0] || !E->Operands[0]->Type ||
          E->Operands[0]->Type->Kind != NdTypeKind::Int)
        return false;
      if (E->Kind == ExprKind::UnaryOp &&
          ((E->Op != NdOp::INT_ZEXT && E->Op != NdOp::INT_SEXT) ||
           E->Type->Size < E->Operands[0]->Type->Size))
        return false;
      break;
    case ExprKind::BinOp:
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
      } else {
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
  struct Candidate {
    uint16_t Bytes = 0;
    uint16_t CarrierBytes = 0;
    bool Valid = true;
    std::vector<HighStmt *> Definitions;
    std::set<HighStmt *> PrefixDefinitions;
    std::vector<ExprPtr *> Uses;
  };
  VarKeyMap<Candidate> Candidates;
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
    const bool ConcatShape =
        DestinationShape && V->Kind == ExprKind::BinOp &&
        V->Op == NdOp::CONCAT && V->Type && V->Type->Kind == NdTypeKind::Int &&
        V->Type->Size == CarrierBytes && V->Operands.size() == 2 &&
        V->Operands[0] && V->Operands[1] && V->Operands[0]->Type &&
        V->Operands[1]->Type && V->Operands[1]->Type->Kind == NdTypeKind::Int &&
        (V->Operands[1]->Type->Size == 4 ||
         (CarrierBytes == 16 && V->Operands[1]->Type->Size == 8)) &&
        V->Operands[0]->Type->Size + V->Operands[1]->Type->Size == CarrierBytes;
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
    if ((!ConcatShape && !ExtensionShape && !CopyShape) ||
        !discardableIntegerValue(
            ConcatShape ? V->Operands[0] : V, Budget)) {
      C.Valid = false;
      continue;
    }
    if (ConcatShape) {
      const auto Width = V->Operands[1]->Type->Size;
      C.Valid &= !C.Bytes || C.Bytes == Width;
      C.Bytes = Width;
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
        C.Valid &= (Prefix || ProvenCopy) && Plain(*E) && E->Operands.empty() &&
                   E->Type && E->Type->Kind == NdTypeKind::Int &&
                   E->Type->Size == C.CarrierBytes &&
                   E->Var.Size == C.CarrierBytes && E->Var.RenameTag < 0 &&
                   (E->Var.Kind == MedVar::Reg || E->Var.Kind == MedVar::Temp);
        C.Uses.push_back(Slot);
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
  for (auto &[Key, C] : Candidates)
    if (C.Valid)
      for (auto *Slot : C.Uses)
        Narrow(*Slot, C.Bytes);
  for (auto &[Key, C] : Candidates) {
    if (!C.Valid)
      continue;
    for (auto *S : C.Definitions) {
      Narrow(S->Dst, C.Bytes);
      S->Val = C.PrefixDefinitions.count(S)
                   ? frameValuePrefix(S->Val, C.Bytes)
                   : S->Val->Operands[1];
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
  Stmts.erase(
      std::remove_if(Stmts.begin(), Stmts.end(),
                     [](const HighStmt &S) { return S.Kind == StmtKind::Nop; }),
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
