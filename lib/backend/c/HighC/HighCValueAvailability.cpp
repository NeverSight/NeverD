//===- HighCValueAvailability.cpp - Availability in scalar C flow --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapt the final structured body and the C writer's flag transfers to the
/// shared availability proof. Forwarded expressions are not scalar identities;
/// an omitted or moved statement cannot supply an observation point.
//===----------------------------------------------------------------------===//

#include "../../../ir/high/pass/HighFlowGraph.h"
#include "HighCWriter.h"

#include "neverd/backend/Limits.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace neverd {
namespace {

std::optional<std::pair<uint8_t, bool>> comparisonRelation(NdOp Op) {
  using namespace c_availability;
  switch (Op) {
  case NdOp::INT_EQUAL:
    return {{Equal, false}};
  case NdOp::INT_NOTEQUAL:
    return {{Less | Greater, false}};
  case NdOp::INT_LESS:
    return {{Less, false}};
  case NdOp::INT_LESSEQUAL:
    return {{Less | Equal, false}};
  case NdOp::INT_SLESS:
    return {{Less, true}};
  case NdOp::INT_SLESSEQUAL:
    return {{Less | Equal, true}};
  default:
    return std::nullopt;
  }
}

/// Keep statement observation offsets while putting an unconditional linear
/// sequence in one proof node. Unrelated calls/statements consume no path
/// depth; conditions, entries, joins, barriers and cycles retain their edges.
void compactFlow(
    c_availability::Graph &Flow,
    llvm::DenseMap<const HighStmt *, c_availability::Point> &Points) {
  using namespace c_availability;
  const size_t Count = Flow.Nodes.size();
  std::vector<size_t> Successors(Count), OnlySuccessor(Count);
  for (size_t I = 0; I < Count; ++I)
    for (const Edge &E : Flow.Nodes[I].Predecessors) {
      ++Successors[E.Predecessor];
      OnlySuccessor[E.Predecessor] = I;
    }
  std::vector<bool> Mergeable(Count), Mapped(Count);
  for (size_t I = 0; I < Count; ++I) {
    const Node &N = Flow.Nodes[I];
    if (N.Entry || N.Barrier || N.Predecessors.size() != 1)
      continue;
    const Edge &E = N.Predecessors.front();
    Mergeable[I] = Successors[E.Predecessor] == 1 && !E.Condition &&
                   E.Copies.empty() && !Flow.Nodes[E.Predecessor].Barrier;
  }
  Graph Compact;
  Compact.Nodes.reserve(Count);
  std::vector<Point> Locations(Count);
  std::vector<size_t> Heads;
  auto Chain = [&](size_t Head) {
    const size_t Group = Compact.Nodes.size();
    Node &N = Compact.Nodes.emplace_back();
    N.Entry = Flow.Nodes[Head].Entry;
    N.Barrier = Flow.Nodes[Head].Barrier;
    Heads.push_back(Head);
    for (size_t I = Head;;) {
      Locations[I] = {Group, N.Assignments.size()};
      Mapped[I] = true;
      N.Assignments.append(Flow.Nodes[I].Assignments);
      if (Successors[I] != 1)
        break;
      const size_t Next = OnlySuccessor[I];
      if (Mapped[Next] || !Mergeable[Next])
        break;
      I = Next;
    }
  };
  for (size_t I = 0; I < Count; ++I)
    if (!Mergeable[I] && !Mapped[I])
      Chain(I);
  // An isolated cycle has no chain head. Retain its closing edge instead of
  // making it an acyclic path or inferring that it is unreachable.
  for (size_t I = 0; I < Count; ++I)
    if (!Mapped[I])
      Chain(I);
  for (const size_t Head : Heads)
    for (const Edge &E : Flow.Nodes[Head].Predecessors) {
      Edge Incoming = E;
      Incoming.Predecessor = Locations[E.Predecessor].Node;
      Compact.Nodes[Locations[Head].Node].Predecessors.push_back(
          std::move(Incoming));
    }
  for (auto &[Statement, At] : Points) {
    const Point Location = Locations[At.Node];
    At = {Location.Node, Location.Before + At.Before};
  }
  Flow = std::move(Compact);
}

} // namespace

HighCWriter::DeferredAssignment
HighCWriter::deferredAssignment(const HighExpr &Value) const {
  using c_availability::FlagWrite;
  if (knownVoidCall(Value) || isDeferredUnknownInput(&Value))
    return {FlagWrite::Unknown, {}};
  const HighExpr *Source = peelIntegerViewOps(&Value);
  if (Source &&
      (Source->Kind == ExprKind::Var || Source->Kind == ExprKind::Phi)) {
    std::string Name = copyForwardName(varName(Source->Var));
    if (DeferredUnknownFlags.count(Name))
      return {FlagWrite::Copy, std::move(Name)};
  }
  return {FlagWrite::Known, {}};
}

void HighCWriter::buildAvailabilityFlow(const HighFunc &Func) {
  using namespace c_availability;
  if (DeferredUnknownFlags.empty())
    return;
  auto &Flow = AvailabilityFlow.emplace();

  // A name has one C local identity, including different HighIR occurrences
  // of that local. Constant identities include their exact machine width.
  auto Local = [&](llvm::StringRef Name) {
    const size_t Next = AvailabilityValues.size() + 1;
    return AvailabilityValues.try_emplace(Name, Next).first->second;
  };
  for (const auto &[Name, Flag] : DeferredUnknownFlags)
    Local(Name);
  llvm::StringSet<> Addressed;
  std::function<void(const HighExpr &, bool)> InspectExpr;
  InspectExpr = [&](const HighExpr &E, bool Address) {
    Address |= E.Kind == ExprKind::Addr;
    if (Address && (E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi))
      Addressed.insert(varName(E.Var));
    if (E.Kind == ExprKind::Call && libc::isReturnsTwiceFunction(E.CallTarget))
      Flow.Complete = false;
    E.forEachChildExpr([&](const ExprPtr &Child) {
      if (Child)
        InspectExpr(*Child, Address);
    });
  };
  llvm::DenseMap<va_t, llvm::SmallVector<const HighStmt *, 2>> Labels;
  std::function<void(const std::vector<HighStmt> &, va_t)> InspectList;
  InspectList = [&](const std::vector<HighStmt> &Body, va_t Parent) {
    va_t Previous = Parent;
    for (const HighStmt &S : Body) {
      // The writer emits the first occurrence. Branch presentation can
      // reverse distinct occurrences, so each candidate becomes an entry.
      if (S.Addr != Previous && GotoTargets.count(S.Addr))
        Labels[S.Addr].push_back(&S);
      Previous = S.Addr;
      if (S.Kind == StmtKind::SEHTry || S.Kind == StmtKind::CxxTry ||
          !S.EHClauseBodies.empty() || !S.CxxContinuationReturnTargets.empty())
        Flow.Complete = false;
      forEachExpr(S, [&](const ExprPtr &E) {
        if (E)
          InspectExpr(*E, false);
      });
      InspectList(S.Body, S.Addr);
      InspectList(S.ElseBody, 0);
      for (const SwitchCase &C : S.Cases)
        InspectList(C.Body, 0);
      InspectList(S.DefaultBody, 0);
      for (const auto &Clause : S.EHClauseBodies)
        InspectList(Clause, 0);
    }
  };
  InspectList(Func.Body, 0);
  if (!Flow.Complete)
    return;

  HighFlowGraph Structured(Func.Body, isNoReturnCallStmt);
  Flow.Complete = Structured.Complete;
  if (!Flow.Complete)
    return;
  Flow.Nodes.resize(Structured.Nodes.size());
  Flow.Nodes[Structured.Entry].Entry = true;
  for (const auto &[Address, Candidates] : Labels)
    if (Candidates.size() > 1)
      for (const HighStmt *S : Candidates) {
        size_t N = Structured.NodeOf.at(S);
        // The do-while's label enters its body, before the trailing test.
        if (S->Kind == StmtKind::DoWhile)
          N = Structured.Nodes[N].Successors.front();
        Flow.Nodes[N].Entry = true;
      }

  llvm::StringMap<TypeRef> ParameterTypes;
  for (size_t I = 0; I < Func.Params.size(); ++I) {
    MedVar Param;
    Param.Kind = MedVar::Param;
    Param.Id = static_cast<int>(I);
    ParameterTypes.try_emplace(varName(Param), emittedParamType(Func, I));
  }
  for (const auto &[Offset, Slot] : FrameSlots)
    Addressed.insert(Slot.Name);
  llvm::DenseSet<Value> PredicateValues;
  std::vector<std::string> PredicateNames;
  auto Scalar = [&](const HighExpr &E) -> std::optional<Value> {
    if (!E.Type || E.Type->Kind != NdTypeKind::Int || E.Type->IsEnum ||
        !E.Type->Size)
      return std::nullopt;
    if (E.Kind == ExprKind::Const)
      return Local("#" + std::to_string(E.Type->Size) + ":" +
                   std::to_string(E.ConstVal));
    if (E.Kind != ExprKind::Var && E.Kind != ExprKind::Phi)
      return std::nullopt;
    const std::string Name = varName(E.Var);
    if (Addressed.contains(Name) || copyForwardName(Name) != Name ||
        FieldForward.count(Name) || ValueForward.count(Name) ||
        CtorThisForward.count(Name) || CallResultNames.count(Name) ||
        FrameAliases.count(Name) || frameStorageSlotNamed(Name) ||
        CatchAliasTemps.count(Name) || UnknownOnlyNames.count(Name))
      return std::nullopt;
    const auto LocalType = DeclaredCTypes.find(Name);
    const auto ParameterType = ParameterTypes.find(Name);
    const TypeRef Declared =
        LocalType != DeclaredCTypes.end()       ? LocalType->second
        : ParameterType != ParameterTypes.end() ? ParameterType->second
                                                : nullptr;
    if (!Declared || Declared->Kind != NdTypeKind::Int || Declared->IsEnum ||
        Declared->Size != E.Type->Size)
      return std::nullopt;
    const Value ID = Local(Name);
    if (PredicateValues.insert(ID).second)
      PredicateNames.push_back(Name);
    return ID;
  };
  auto Comparison = [&](const HighExpr &Condition) -> std::optional<Predicate> {
    const HighExpr *E = &Condition;
    bool Inverted = false;
    while (E->Kind == ExprKind::UnaryOp && E->Op == NdOp::BOOL_NOT &&
           E->Operands.size() == 1 && E->Operands[0]) {
      Inverted = !Inverted;
      E = E->Operands[0].get();
    }
    if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) {
      const auto V = Scalar(*E);
      if (!V)
        return std::nullopt;
      return Predicate{*V, Local("#" + std::to_string(E->Type->Size) + ":0"),
                       unsigned(E->Type->Size) * 8,
                       uint8_t(Inverted ? Equal : Less | Greater), false};
    }
    if (E->Kind != ExprKind::BinOp || E->Operands.size() != 2 ||
        !E->Operands[0] || !E->Operands[1])
      return std::nullopt;
    const auto Relation = comparisonRelation(E->Op);
    if (!Relation)
      return std::nullopt;
    const auto [Mask, Signed] = *Relation;
    const HighExpr *Left = E->Operands[0].get();
    const HighExpr *Right = E->Operands[1].get();
    if (E->Op == NdOp::INT_EQUAL || E->Op == NdOp::INT_NOTEQUAL) {
      if (Left->Kind == ExprKind::Const && !Left->ConstVal)
        std::swap(Left, Right);
      // The whole subtraction/xor is zero iff its inputs are equal. A
      // truncated difference cannot establish equality of the whole words.
      if (Right->Kind == ExprKind::Const && !Right->ConstVal &&
          Left->Kind == ExprKind::BinOp &&
          (Left->Op == NdOp::INT_SUB || Left->Op == NdOp::INT_XOR) &&
          Left->Type && Right->Type && Left->Type->Size == Right->Type->Size &&
          Left->Operands.size() == 2 && Left->Operands[0] &&
          Left->Operands[1] && Left->Operands[0]->Type &&
          Left->Operands[1]->Type &&
          Left->Operands[0]->Type->Size == Left->Type->Size &&
          Left->Operands[1]->Type->Size == Left->Type->Size) {
        Right = Left->Operands[1].get();
        Left = Left->Operands[0].get();
      }
    }
    const auto L = Scalar(*Left), R = Scalar(*Right);
    if (!L || !R || Left->Type->Size != Right->Type->Size)
      return std::nullopt;
    return Predicate{*L, *R, unsigned(Left->Type->Size) * 8,
                     uint8_t(Inverted ? Any ^ Mask : Mask), Signed};
  };

  // Only branch operands need predicate kills. Unknown payload transport
  // retains every actual flag write, including narrowed integer copies.
  std::vector<std::optional<Predicate>> Conditions(Flow.Nodes.size());
  for (size_t I = 1; I < Structured.Nodes.size(); ++I) {
    const HighStmt &S = *Structured.Nodes[I].Statement;
    if (S.Cond && !Analysis.DeadStmts.count(&S) &&
        (S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse ||
         S.Kind == StmtKind::While || S.Kind == StmtKind::DoWhile ||
         S.Kind == StmtKind::For))
      Conditions[I] = Comparison(*S.Cond);
    // An exact deferred copy can rename a predicate operand. Index its
    // source before collecting writes, including writes earlier in the body.
    // Ordinary assignments only kill facts and introduce no such identity.
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi) &&
        DeferredUnknownFlags.count(varName(S.Dst->Var)) &&
        (S.Val->Kind == ExprKind::Var || S.Val->Kind == ExprKind::Phi))
      Scalar(*S.Val);
  }
  struct RelationAssignment {
    Predicate Comparison;
    Predicate TrueValue;
  };
  // Index candidate definitions once, then follow only predicate dependencies.
  // A stored difference can precede the Boolean comparison that first gives
  // its destination a predicate role; source order is not a proof boundary.
  llvm::StringMap<llvm::SmallVector<size_t, 1>> Definitions;
  for (size_t I = 1; I < Structured.Nodes.size(); ++I) {
    const HighStmt &S = *Structured.Nodes[I].Statement;
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val ||
        (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi))
      continue;
    const auto &E = *S.Val;
    if ((E.Kind == ExprKind::BinOp &&
         (comparisonRelation(E.Op) || E.Op == NdOp::INT_SUB ||
          E.Op == NdOp::INT_XOR)) ||
        (E.Kind == ExprKind::UnaryOp && E.Op == NdOp::BOOL_NOT))
      Definitions[varName(S.Dst->Var)].push_back(I);
  }
  llvm::DenseMap<size_t, RelationAssignment> Relations;
  for (size_t K = 0; K < PredicateNames.size(); ++K) {
    const auto Defined = Definitions.find(PredicateNames[K]);
    if (Defined == Definitions.end())
      continue;
    for (const size_t I : Defined->second) {
      const HighStmt &S = *Structured.Nodes[I].Statement;
      if (stmtSuppressedAsDead(S) || stmtHiddenFromC(S))
        continue;
      const auto Destination = Scalar(*S.Dst);
      if (!Destination)
        continue;
      auto Test = Comparison(*S.Val);
      uint8_t TrueMask = Less | Greater;
      const auto &E = *S.Val;
      if (!Test && E.Kind == ExprKind::BinOp &&
          (E.Op == NdOp::INT_SUB || E.Op == NdOp::INT_XOR) && E.Type &&
          E.Type->Size == S.Dst->Type->Size && E.Operands.size() == 2 &&
          E.Operands[0] && E.Operands[1] && E.Operands[0]->Type &&
          E.Operands[1]->Type && E.Operands[0]->Type->Size == E.Type->Size &&
          E.Operands[1]->Type->Size == E.Type->Size) {
        const auto L = Scalar(*E.Operands[0]), R = Scalar(*E.Operands[1]);
        if (L && R) {
          Test = Predicate{*L, *R, unsigned(E.Type->Size) * 8, Equal, false};
          TrueMask = Equal;
        }
      }
      if (!Test)
        continue;
      const Value Zero = Local("#" + std::to_string(S.Dst->Type->Size) + ":0");
      Relations.try_emplace(I,
                            RelationAssignment{*Test,
                                               {*Destination, Zero,
                                                unsigned(S.Dst->Type->Size) * 8,
                                                TrueMask, false}});
    }
  }
  Flow.Nodes.reserve(Flow.Nodes.size() + 2 * Relations.size());
  for (size_t I = 1; I < Structured.Nodes.size(); ++I) {
    const HighStmt &S = *Structured.Nodes[I].Statement;
    Node &N = Flow.Nodes[I];
    AvailabilityPoints[&S] = {I, 0};
    if (LandingPadLabels.count(S.Addr))
      N.Entry = true;
    // These structural statements are absent from C, rather than a test
    // whose taken body can publish a flag. Bypass them as the writer does.
    if ((S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse) && !S.Cond)
      Flow.Complete = false;
    if (S.Kind == StmtKind::Switch && !S.SwitchExpr)
      Flow.Complete = false;
    if (Analysis.DeadStmts.count(&S) &&
        (!S.Body.empty() || !S.ElseBody.empty() || !S.Cases.empty() ||
         !S.DefaultBody.empty()))
      Flow.Complete = false;
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi)) {
      const std::string Name = varName(S.Dst->Var);
      const auto ID = AvailabilityValues.find(Name);
      if (ID != AvailabilityValues.end()) {
        Assignment A;
        A.Destination = ID->second;
        if (DeferredUnknownFlags.count(Name) && !stmtSuppressedAsDead(S) &&
            !stmtHiddenFromC(S)) {
          const auto Transfer = deferredAssignment(*S.Val);
          A.Availability = Transfer.Availability;
          const HighExpr *Source = peelIntegerViewOps(S.Val.get());
          if (Source &&
              (Source->Kind == ExprKind::Var ||
               Source->Kind == ExprKind::Phi) &&
              copyForwardName(varName(Source->Var)) != varName(Source->Var)) {
            // CopyForward may be erased during emission. It cannot supply
            // a stable availability source for a cross-statement proof.
            A.Availability = FlagWrite::Unknown;
          } else if (Transfer.Availability == FlagWrite::Copy) {
            A.Source = Local(Transfer.Source);
          }
          if ((S.Val->Kind == ExprKind::Var || S.Val->Kind == ExprKind::Phi) &&
              S.Val->Type && S.Dst->Type &&
              S.Val->Type->Size == S.Dst->Type->Size)
            if (auto Copy = Scalar(*S.Val)) {
              A.Source = *Copy;
              A.CopyBits = unsigned(S.Val->Type->Size) * 8;
            }
        }
        N.Assignments.push_back(A);
      }
    }
    // Multi-output intrinsics write ordinary scalar locals too. No
    // previously tested comparison about those locals survives the write.
    forEachRhsExpr(S, [&](const ExprPtr &Root) {
      std::function<void(const HighExpr &)> Outputs = [&](const HighExpr &E) {
        for (const MedVar &Output : E.IntrinsicOutputs) {
          const auto ID = AvailabilityValues.find(varName(Output));
          if (ID != AvailabilityValues.end())
            N.Assignments.push_back(
                {ID->second, std::nullopt, 0, FlagWrite::None});
        }
        E.forEachChildExpr([&](const ExprPtr &Child) {
          if (Child)
            Outputs(*Child);
        });
      };
      if (Root)
        Outputs(*Root);
    });
    if (const auto Relation = Relations.find(I); Relation != Relations.end()) {
      // C evaluates the operands before writing the result. Publish only
      // the exact zero/nonzero relation after the write, retaining the old
      // operands when a later statement changes them. A stored sub/xor
      // supplies equality of whole-width inputs, not its arithmetic value.
      for (const bool Taken : {true, false}) {
        Node Outcome;
        Outcome.Assignments = N.Assignments;
        Edge Test;
        Test.Predecessor = I;
        Test.Condition = Relation->second.Comparison;
        if (!Taken)
          Test.Condition->Allowed ^= Any;
        Outcome.Predecessors.push_back(std::move(Test));
        const size_t OutcomeIndex = Flow.Nodes.size();
        Flow.Nodes.push_back(std::move(Outcome));
        for (const size_t Next : Structured.Nodes[I].Successors) {
          Edge Result;
          Result.Predecessor = OutcomeIndex;
          Result.Condition = Relation->second.TrueValue;
          if (!Taken)
            Result.Condition->Allowed ^= Any;
          Flow.Nodes[Next].Predecessors.push_back(std::move(Result));
        }
      }
      N.Assignments.clear();
      continue;
    }
    for (size_t K = 0; K < Structured.Nodes[I].Successors.size(); ++K) {
      Edge E;
      E.Predecessor = I;
      E.Condition = Conditions[I];
      if (E.Condition && K)
        E.Condition->Allowed ^= Any;
      Flow.Nodes[Structured.Nodes[I].Successors[K]].Predecessors.push_back(
          std::move(E));
    }
  }
  // An unconnected statement can be an entry omitted by structural recovery.
  // Do not use its lack of predecessors as an unreachable-path proof.
  for (Node &N : Flow.Nodes)
    if (N.Predecessors.empty())
      N.Entry = true;
  if (Flow.Complete)
    compactFlow(Flow, AvailabilityPoints);
}

bool HighCWriter::deferredValueAvailable(llvm::StringRef Name) {
  if (!EmittingStmt || !AvailabilityFlow || !AvailabilityFlow->Complete)
    return false;
  const auto Point = AvailabilityPoints.find(EmittingStmt);
  const auto Value = AvailabilityValues.find(Name);
  return Point != AvailabilityPoints.end() &&
         Value != AvailabilityValues.end() &&
         c_availability::availableBefore(*AvailabilityFlow, Point->second,
                                         Value->second);
}

} // namespace neverd
