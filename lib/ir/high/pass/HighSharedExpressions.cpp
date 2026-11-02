//===- HighSharedExpressions.cpp - Name values a statement repeats --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A HighIR expression is a DAG: a value inlined where several operations
/// read it is one node with several parents, and the C writer prints it at
/// every parent.  Lane-wise vector code shares its values level after level,
/// so one statement could print megabytes.  A large value a statement reads
/// more than once is assigned to a fresh local right before the statement,
/// which then reads the local.  Integer arithmetic on locals and constants
/// cannot fault or have an effect, so it may run earlier and outside a
/// branch of the statement.  A memory read moves only out of a statement
/// with no effect of its own before its end, where the read always runs.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"

#include "llvm/ADT/STLExtras.h"

#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

namespace neverd {
namespace {

constexpr uint64_t kPartNodes = 64;

/// Evaluating \p E cannot fault or have an effect.
bool effectFree(const HighExpr &E) {
  if (E.IntrinsicId != Intrinsic::None || !E.IntrinsicOutputs.empty() ||
      E.IndirectTarget || E.MemoryOrdering != NdMemoryOrdering::None ||
      E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  switch (E.Kind) {
  case ExprKind::Const:
    return true;
  case ExprKind::Var:
  case ExprKind::Phi:
    return E.Operands.empty() &&
           (E.Var.Kind == MedVar::Reg || E.Var.Kind == MedVar::Temp ||
            E.Var.Kind == MedVar::Param);
  case ExprKind::Cast:
  case ExprKind::BitCast:
    return E.Type && E.Type->Kind == NdTypeKind::Int;
  case ExprKind::UnaryOp:
    switch (E.Op) {
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
  case ExprKind::BinOp:
    switch (E.Op) {
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
    case NdOp::CONCAT:
    case NdOp::SELECT:
    case NdOp::INT_CARRY:
    case NdOp::INT_SOVF:
    case NdOp::INT_SBOR:
      return true;
    case NdOp::SUBBYTES:
      return E.Operands.size() == 2 && E.Operands[1] &&
             E.Operands[1]->Kind == ExprKind::Const;
    default:
      return false;
    }
  default:
    return false;
  }
}

/// A plain read of memory, which may fault, so it moves only from where it
/// always runs.
bool plainLoad(const HighExpr &E) {
  return E.Kind == ExprKind::Load && E.IntrinsicId == Intrinsic::None &&
         E.IntrinsicOutputs.empty() && !E.IndirectTarget &&
         E.Operands.size() == 1 && E.Type &&
         E.MemoryOrdering == NdMemoryOrdering::None &&
         E.MemoryAddressSpace == NdMemoryAddressSpace::Default;
}

/// Evaluating \p E may write memory or have another effect.
bool hasEffect(const HighExpr &E) {
  return E.Kind == ExprKind::Call || E.Kind == ExprKind::Store ||
         E.IntrinsicId != Intrinsic::None || !E.IntrinsicOutputs.empty() ||
         E.MemoryOrdering != NdMemoryOrdering::None ||
         E.MemoryAddressSpace != NdMemoryAddressSpace::Default;
}

/// The expression slots of \p S that the statement evaluates once, before
/// any effect of its own.  A loop condition runs again on every pass.
std::vector<ExprPtr *> evaluatedOnce(HighStmt &S) {
  switch (S.Kind) {
  case StmtKind::Assign:
    return {&S.Val};
  case StmtKind::Store:
    return {&S.StoreAddr, &S.StoreVal};
  case StmtKind::Return:
    return {&S.RetVal};
  case StmtKind::If:
  case StmtKind::IfElse:
    return {&S.Cond};
  case StmtKind::Switch:
    return {&S.SwitchExpr};
  case StmtKind::Call:
    return {&S.CallExpr};
  default:
    return {};
  }
}

class Namer {
public:
  explicit Namer(HighFunc &Func) : Func(Func) {
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
          S.GotoTarget != InvalidVA)
        Targets.insert(S.GotoTarget);
      for (const HighEHClause &Clause : S.EHClauses)
        Targets.insert(Clause.HandlerVA);
      forEachExpr(S, [&](const ExprPtr &E) { note(E); });
      if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var)
        ++Definitions[varKey(S.Dst->Var)];
    });
    // Optional address scans together visit at most one function's worth of
    // distinct expression nodes, independently of nested conditional depth.
    AddressBudget = Noted.size();
  }

  bool run() { return visit(Func.Body); }

private:
  HighFunc &Func;
  std::set<va_t> Targets;
  std::set<const HighExpr *> Noted;
  int NextId = limits::kVarRenameIdBase;
  int NextTag = 0;
  Arch TheArch = Arch::Unknown;
  bool Changed = false;
  VarKeyMap<unsigned> Definitions;
  size_t AddressBudget = 0;

  void note(const ExprPtr &E) {
    if (!E || !Noted.insert(E.get()).second)
      return;
    if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) {
      NextId = std::max(NextId, E->Var.Id + 1);
      NextTag = std::max(NextTag, E->Var.RenameTag + 1);
      TheArch = E->Var.TheArch;
    }
    for (const MedVar &Output : E->IntrinsicOutputs)
      ++Definitions[varKey(Output)];
    for (const ExprPtr &Operand : E->Operands)
      note(Operand);
    note(E->IndirectTarget);
  }

  ExprPtr freshLocal(TypeRef Type) {
    if (NextTag >= INT16_MAX)
      return nullptr;
    MedVar Local;
    Local.Kind = MedVar::Temp;
    Local.Id = NextId++;
    Local.RenameTag = static_cast<int16_t>(NextTag++);
    Local.Size = Type->Size;
    Local.TheArch = TheArch;
    return HighExpr::makeVar(Local, std::move(Type));
  }

  // A same-width integer/pointer view preserves the address bits. A float
  // conversion or a narrowing is a different computation and stays explicit.
  static const HighExpr *addressBits(const HighExpr *E) {
    while (E && E->Operands.size() == 1 && E->Operands[0] && E->Type &&
           E->Operands[0]->Type &&
           E->Type->Size == E->Operands[0]->Type->Size &&
           (E->Type->Kind == NdTypeKind::Int ||
            E->Type->Kind == NdTypeKind::Ptr) &&
           (E->Operands[0]->Type->Kind == NdTypeKind::Int ||
            E->Operands[0]->Type->Kind == NdTypeKind::Ptr) &&
           (E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast ||
            (E->Kind == ExprKind::UnaryOp &&
             (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT))))
      E = E->Operands[0].get();
    return E;
  }

  /// A guard q!=s also tests ((q^k)-(s^k))!=0. If an address uses q^k plus
  /// a large displacement, name that decoded base when its remaining field
  /// offset is smaller. No value for q, object layout or callee is inferred.
  std::vector<HighStmt> nameGuardedAddress(HighStmt &S) {
    if (!AddressBudget || TheArch == Arch::Unknown || !Targets.empty() ||
        (S.Kind != StmtKind::If && S.Kind != StmtKind::IfElse) || !S.Cond ||
        S.Cond->Kind != ExprKind::BinOp ||
        (S.Cond->Op != NdOp::INT_NOTEQUAL && S.Cond->Op != NdOp::INT_EQUAL) ||
        S.Cond->Operands.size() != 2)
      return {};
    auto Q = S.Cond->Operands[0], Sentinel = S.Cond->Operands[1];
    if (Q && Q->Kind == ExprKind::Const)
      std::swap(Q, Sentinel);
    if (!Q || Q->Kind != ExprKind::Var || !Q->Type || !Sentinel ||
        Sentinel->Kind != ExprKind::Const || !Sentinel->Type ||
        Sentinel->ConstProvenance != ConstantAddressProvenance::Scalar ||
        (Q->Type->Kind != NdTypeKind::Int &&
         Q->Type->Kind != NdTypeKind::Ptr) ||
        (Q->Type->Size != 4 && Q->Type->Size != 8) ||
        Sentinel->Type->Size != Q->Type->Size ||
        Q->Type->Size != getTargetRegInfo(TheArch).PointerSize ||
        (Q->Var.Kind != MedVar::Temp && Q->Var.Kind != MedVar::Param) ||
        Definitions[varKey(Q->Var)] != (Q->Var.Kind == MedVar::Temp ? 1u : 0u))
      return {};
    auto &Arm = S.Cond->Op == NdOp::INT_NOTEQUAL ? S.Body : S.ElseBody;
    if (Arm.empty())
      return {};
    const unsigned Bytes = Q->Type->Size;
    const uint64_t Mask = Bytes == 8 ? UINT64_MAX : UINT32_MAX;
    const auto Magnitude = [&](uint64_t V) {
      V &= Mask;
      return std::min(V, (uint64_t{0} - V) & Mask);
    };
    struct Address {
      ExprPtr Xor;
      uint64_t Key, Offset;
    };
    auto Match = [&](const ExprPtr &Root) -> std::optional<Address> {
      const HighExpr *E = addressBits(Root.get());
      if (!E || E->Kind != ExprKind::BinOp || E->Op != NdOp::INT_ADD ||
          E->Operands.size() != 2 || !E->Type || E->Type->Size != Bytes)
        return std::nullopt;
      auto Xor = E->Operands[0], Offset = E->Operands[1];
      if (Xor && Xor->Kind == ExprKind::Const)
        std::swap(Xor, Offset);
      const auto *Bits = addressBits(Xor.get());
      if (!Bits || Bits->Kind != ExprKind::BinOp || Bits->Op != NdOp::INT_XOR ||
          Bits->Operands.size() != 2 || !Bits->Type ||
          Bits->Type->Size != Bytes || !Offset ||
          Offset->Kind != ExprKind::Const || !Offset->Type ||
          Offset->ConstProvenance != ConstantAddressProvenance::Scalar ||
          Offset->Type->Size != Bytes)
        return std::nullopt;
      auto Input = Bits->Operands[0], Key = Bits->Operands[1];
      if (Input && Input->Kind == ExprKind::Const)
        std::swap(Input, Key);
      const auto *InputBits = addressBits(Input.get());
      if (!InputBits || InputBits->Kind != ExprKind::Var ||
          InputBits->Var != Q->Var || !Key || Key->Kind != ExprKind::Const ||
          Key->ConstProvenance != ConstantAddressProvenance::Scalar ||
          !Key->Type || Key->Type->Size != Bytes)
        return std::nullopt;
      return Address{Xor, Key->ConstVal & Mask, Offset->ConstVal & Mask};
    };
    std::optional<Address> Chosen;
    HighExprSet Seen;
    std::function<void(const ExprPtr &)> Find = [&](const ExprPtr &E) {
      if (!E || Chosen || !AddressBudget || !Seen.insert(E.get()).second ||
          E->SourceCallHint)
        return;
      --AddressBudget;
      if (E->Kind == ExprKind::Load && E->Operands.size() == 1) {
        if (auto At = Match(E->Operands[0])) {
          const uint64_t NullWord = (Sentinel->ConstVal ^ At->Key) & Mask;
          if (NullWord &&
              Magnitude(At->Offset + NullWord) < Magnitude(At->Offset))
            Chosen = std::move(At);
        }
      }
      E->forEachChildExpr(Find);
    };
    // Nested statement scopes have their own visit. Inspect only direct
    // expressions so enclosing guards do not rescan complete descendants.
    for (const HighStmt &T : Arm)
      forEachRhsExpr(T, Find);
    if (!Chosen)
      return {};
    const auto Type = NdType::makeInt(Bytes, false);
    auto Local = freshLocal(Type);
    if (!Local)
      return {};
    const uint64_t NullWord = (Sentinel->ConstVal ^ Chosen->Key) & Mask;
    auto Xor = std::make_shared<HighExpr>(*addressBits(Chosen->Xor.get()));
    Xor->Type = Type;
    auto Base = HighExpr::makeBinop(
        NdOp::INT_SUB, Xor,
        HighExpr::makeConst(NullWord, Bytes,
                            ConstantAddressProvenance::Scalar));
    Base->Type = Type;
    std::unordered_map<const HighExpr *, ExprPtr> Rewritten;
    std::function<ExprPtr(const ExprPtr &)> Rewrite = [&](const ExprPtr &E) {
      if (!E || E->SourceCallHint)
        return E;
      if (auto It = Rewritten.find(E.get()); It != Rewritten.end())
        return It->second;
      auto Copy = std::make_shared<HighExpr>(*E);
      bool Changed = false;
      for (size_t I = 0; I < Copy->Operands.size(); ++I) {
        Copy->Operands[I] = Rewrite(Copy->Operands[I]);
        Changed |= Copy->Operands[I] != E->Operands[I];
      }
      Copy->IndirectTarget = Rewrite(Copy->IndirectTarget);
      Changed |= Copy->IndirectTarget != E->IndirectTarget;
      if (E->Kind == ExprKind::Load && E->Operands.size() == 1)
        if (auto At = Match(E->Operands[0]); At && At->Key == Chosen->Key) {
          auto Address = HighExpr::makeBinop(
              NdOp::INT_ADD, Local,
              HighExpr::makeConst((At->Offset + NullWord) & Mask, Bytes,
                                  ConstantAddressProvenance::Scalar));
          Address->Type = E->Operands[0]->Type;
          Copy->Operands[0] = std::move(Address);
          Changed = true;
        }
      return Rewritten[E.get()] = Changed ? std::move(Copy) : E;
    };
    for (HighStmt &T : Arm)
      forEachRhsExpr(T, [&](ExprPtr &E) { E = Rewrite(E); });
    auto Condition = std::make_shared<HighExpr>(*S.Cond);
    Condition->Operands = {
        Local,
        HighExpr::makeConst(0, Bytes, ConstantAddressProvenance::Scalar)};
    S.Cond = std::move(Condition);
    HighStmt Assign;
    Assign.Kind = StmtKind::Assign;
    Assign.Dst = Local;
    Assign.Val = std::move(Base);
    Assign.KeepsName = true;
    return {std::move(Assign)};
  }

  bool visit(std::vector<HighStmt> &Body) {
    for (size_t I = 0; I < Body.size(); ++I) {
      for (std::vector<HighStmt> *Inner :
           {&Body[I].Body, &Body[I].ElseBody, &Body[I].DefaultBody})
        visit(*Inner);
      for (SwitchCase &Case : Body[I].Cases)
        visit(Case.Body);
      for (std::vector<HighStmt> &Clause : Body[I].EHClauseBodies)
        visit(Clause);
      std::vector<HighStmt> Named = nameGuardedAddress(Body[I]);
      auto Repeated = name(Body[I]);
      Named.insert(Named.end(), std::make_move_iterator(Repeated.begin()),
                   std::make_move_iterator(Repeated.end()));
      if (Named.empty())
        continue;
      // A jump to the statement lands on the first new assignment instead,
      // which takes over its label.
      const va_t Address = Body[I].Addr;
      if (Address != 0 && Address != InvalidVA && Targets.count(Address))
        Named.front().Addr = Address;
      Body.insert(Body.begin() + static_cast<std::ptrdiff_t>(I),
                  std::make_move_iterator(Named.begin()),
                  std::make_move_iterator(Named.end()));
      I += Named.size();
      Changed = true;
    }
    return Changed;
  }

  /// The assignments that name the repeated values of \p S, in the order
  /// they run; \p S reads the names afterwards.
  std::vector<HighStmt> name(HighStmt &S) {
    std::vector<ExprPtr *> Roots = evaluatedOnce(S);
    // How many nodes the statement prints, and how many distinct ones.
    std::unordered_map<const HighExpr *, uint64_t> Printed;
    std::function<uint64_t(const HighExpr *)> Size =
        [&](const HighExpr *E) -> uint64_t {
      if (!E)
        return 0;
      if (auto It = Printed.find(E); It != Printed.end())
        return It->second;
      uint64_t N = 1;
      for (const ExprPtr &Operand : E->Operands)
        N = std::min<uint64_t>(N + Size(Operand.get()), UINT64_MAX / 4);
      N = std::min<uint64_t>(N + Size(E->IndirectTarget.get()), UINT64_MAX / 4);
      Printed[E] = N;
      return N;
    };
    uint64_t Total = 0;
    for (ExprPtr *Root : Roots)
      Total += Size(Root->get());
    if (Total < ::neverd::limits::ir::kHighSharedExpressionsMinPrintedNodes ||
        (Total <= ::neverd::limits::ir::kHighSharedExpressionsMaxPrintedNodes &&
         Total < ::neverd::limits::ir::kHighSharedExpressionsMinRepetition *
                     Printed.size()))
      return {};

    // Work on a copy, so that a statement sharing these nodes keeps them.
    std::unordered_map<const HighExpr *, ExprPtr> Copies;
    std::function<ExprPtr(const ExprPtr &)> Copy =
        [&](const ExprPtr &E) -> ExprPtr {
      if (!E)
        return E;
      if (auto It = Copies.find(E.get()); It != Copies.end())
        return It->second;
      auto C = std::make_shared<HighExpr>(*E);
      Copies[E.get()] = C;
      for (ExprPtr &Operand : C->Operands)
        Operand = Copy(Operand);
      C->IndirectTarget = Copy(C->IndirectTarget);
      return C;
    };
    for (ExprPtr *Root : Roots)
      *Root = Copy(*Root);

    // The nodes evaluated only on some paths through the statement: the
    // right operand of a logical operator and the arms of a selection.
    bool Quiet = true;
    std::set<const HighExpr *> Conditional, Unconditional;
    std::function<void(const HighExpr *, bool)> Reach = [&](const HighExpr *E,
                                                            bool Guarded) {
      if (!E || !(Guarded ? Conditional : Unconditional).insert(E).second)
        return;
      Quiet &= !hasEffect(*E);
      for (size_t I = 0; I < E->Operands.size(); ++I) {
        const bool Arm =
            (E->Kind == ExprKind::BinOp &&
             (E->Op == NdOp::BOOL_AND || E->Op == NdOp::BOOL_OR) && I > 0) ||
            (E->Op == NdOp::SELECT && I > 0);
        Reach(E->Operands[I].get(), Guarded || Arm);
      }
      Reach(E->IndirectTarget.get(), Guarded);
    };
    for (ExprPtr *Root : Roots)
      Reach(Root->get(), false);
    // A destination in memory computes its address within the statement too.
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind != ExprKind::Var &&
        S.Dst->Kind != ExprKind::Phi)
      Reach(S.Dst.get(), false);

    // Each node's readers, and whether it may move before the statement.
    std::unordered_map<const HighExpr *, std::vector<ExprPtr *>> Readers;
    std::unordered_map<const HighExpr *, bool> Free;
    std::vector<ExprPtr> PostOrder;
    std::set<const HighExpr *> Seen;
    std::function<bool(ExprPtr &)> Walk = [&](ExprPtr &E) -> bool {
      if (!E)
        return true;
      Readers[E.get()].push_back(&E);
      if (!Seen.insert(E.get()).second)
        return Free[E.get()];
      bool AllFree = effectFree(*E) ||
                     (Quiet && plainLoad(*E) && !Conditional.count(E.get()));
      for (ExprPtr &Operand : E->Operands)
        AllFree &= Walk(Operand);
      if (E->IndirectTarget) {
        Walk(E->IndirectTarget);
        AllFree = false;
      }
      Free[E.get()] = AllFree;
      PostOrder.push_back(E);
      return AllFree;
    };
    for (ExprPtr *Root : Roots)
      Walk(*Root);

    std::unordered_map<const HighExpr *, size_t> Position;
    for (size_t I = 0; I < PostOrder.size(); ++I)
      Position[PostOrder[I].get()] = I;
    // The new assignments with the position of the value each names.
    std::vector<std::pair<size_t, HighStmt>> Named;
    std::set<const ExprPtr *> RootSlots(Roots.begin(), Roots.end());
    // Assigns \p E to a fresh local, which its readers read instead.
    auto NameValue = [&](const ExprPtr &E) {
      auto Local = freshLocal(E->Type);
      if (!Local)
        return false;
      HighStmt Assign;
      Assign.Kind = StmtKind::Assign;
      Assign.Dst = Local;
      Assign.Val = E;
      Assign.KeepsName = true;
      Named.emplace_back(Position[E.get()], std::move(Assign));
      for (ExprPtr *Use : Readers[E.get()])
        *Use = HighExpr::makeVar(Local->Var, E->Type);
      // The sizes above shrink.
      Printed.clear();
      return true;
    };
    auto Nameable = [&](const ExprPtr &E) {
      return Free[E.get()] && E->Type && E->Kind != ExprKind::Var &&
             E->Kind != ExprKind::Phi && E->Kind != ExprKind::Const &&
             llvm::none_of(Readers[E.get()], [&](const ExprPtr *Use) {
               return RootSlots.count(Use) != 0;
             });
    };
    Printed.clear();
    // Children come before their parents, so a name is assigned before any
    // value reading it.
    for (const ExprPtr &E : PostOrder)
      if (Readers[E.get()].size() >= 2 && Nameable(E) &&
          Size(E.get()) >=
              ::neverd::limits::ir::kHighSharedExpressionsMinNamedNodes &&
          !NameValue(E))
        break;
    uint64_t Remaining = 0;
    for (ExprPtr *Root : Roots)
      Remaining += Size(Root->get());
    if (Remaining > ::neverd::limits::ir::kHighSharedExpressionsMaxPrintedNodes)
      for (const ExprPtr &E : PostOrder)
        if (Readers[E.get()].size() == 1 && Nameable(E) &&
            Size(E.get()) >= kPartNodes && !NameValue(E))
          break;
    // A value a named one reads comes before it in the post order, so this
    // order assigns every name before any assignment reads it.
    llvm::stable_sort(
        Named, [](const auto &A, const auto &B) { return A.first < B.first; });
    std::vector<HighStmt> Ordered;
    for (auto &Entry : Named)
      Ordered.push_back(std::move(Entry.second));
    return Ordered;
  }
};

} // namespace

bool nameRepeatedValues(HighFunc &Func) { return Namer(Func).run(); }

} // namespace neverd
