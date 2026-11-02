//===- HighVariableEdits.cpp - Private, proven local source variants
//-------===//
#include "neverd/ir/high/HighVariableEdits.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceVariableFlow.h"
#include "neverd/ir/SourceVariableIdentity.h"
#include "neverd/ir/high/HighSourceFlow.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <set>

using namespace neverd;
namespace {
bool scalar(const TypeRef &Type) {
  return Type && Type->Size && Type->Size <= 16 &&
         (Type->Kind == NdTypeKind::Int || Type->Kind == NdTypeKind::Float ||
          Type->Kind == NdTypeKind::Ptr ||
          (Type->Kind == NdTypeKind::Struct && Type->IsEnum));
}
std::string typeKey(const TypeRef &Type, unsigned Depth = 0) {
  if (!Type ||
      Depth > ::neverd::limits::ir::kHighVariableEditsTypeKeyDepthLimit)
    return "unknown";
  std::string Key = std::to_string(unsigned(Type->Kind)) + ":" +
                    std::to_string(Type->Size) + ":" +
                    std::to_string(Type->IsSigned) + ":" + Type->SourceName;
  if (Type->Kind == NdTypeKind::Ptr)
    Key += ":" + typeKey(Type->Pointee, Depth + 1);
  return Key;
}
std::string flowTypeKey(const TypeRef &Type) {
  // Integer signedness is a use-site view. Rewritten reads keep an explicit
  // cast back to that view; floating and pointer interpretations stay distinct.
  return Type && Type->Kind == NdTypeKind::Int
             ? "integer:" + std::to_string(Type->Size)
             : typeKey(Type);
}
struct Variable {
  MedVar Value;
  TypeRef Type;
  std::optional<int64_t> Slot;
};
struct Projection {
  const HighFunc &Function;
  HighSourceFlowGraph Control;
  SourceVariableFlow Flow;
  std::map<std::string, unsigned> Ids;
  std::vector<Variable> Variables;
  std::map<const HighStmt *, size_t> StatementNodes;
  std::map<std::string, int64_t> FrameAliases;
  bool FrameUnsafe = false;
  int MaxId = 0;
  size_t Visits = 0;
  explicit Projection(const HighFunc &Function)
      : Function(Function), Control(buildHighSourceFlowGraph(Function)) {
    if (!Control.Diagnostics.Complete || !Control.Diagnostics.Items.empty() ||
        Control.Nodes.size() >
            ::neverd::limits::ir::
                kHighVariableEditsProjectionControlNodesSizeLimit ||
        Function.StructuredExceptionRegions ||
        Function.UnstructuredExceptionRegions) {
      Flow.Refusal = "Variable associations require complete ordinary control "
                     "flow without exception entries";
      return;
    }
    // Admit only single-definition aliases in the straight-line entry
    // prefix. They dominate every later use and cannot hide a loop cursor.
    std::map<std::string, unsigned> Definitions;
    walkStmts(Function.Body, [&](const HighStmt &S) {
      if (S.Dst &&
          (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi)) {
        ++Definitions[highSourceVariableId(S.Dst->Var)];
        MaxId = std::max(MaxId, S.Dst->Var.Id);
      }
    });
    for (const auto &S : Function.Body) {
      if (S.Kind != StmtKind::Assign && S.Kind != StmtKind::Store &&
          S.Kind != StmtKind::Call && S.Kind != StmtKind::Nop)
        break;
      if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var) {
        const auto Id = highSourceVariableId(S.Dst->Var);
        if (Definitions[Id] == 1)
          if (auto Disp = displacement(S.Val))
            FrameAliases[Id] = *Disp;
      }
    }
    Flow.Entry = Control.Entry;
    Flow.Nodes.resize(Control.Nodes.size());
    std::map<const HighExpr *, const HighStmt *> Tests;
    walkStmts(Function.Body, [&](const HighStmt &S) {
      if (S.Cond)
        Tests.emplace(S.Cond.get(), &S);
    });
    for (size_t N = 0; N < Control.Nodes.size(); ++N) {
      const auto &Node = Control.Nodes[N];
      Flow.Nodes[N].Successors = Node.Successors;
      if (Node.Statement) {
        if (!StatementNodes.emplace(Node.Statement, N).second)
          Flow.Refusal =
              "A source statement has more than one evaluation context";
        statement(*Node.Statement, N);
      }
      if (Node.Test) {
        read(Node.Test, N);
        if (!Node.Statement)
          if (auto It = Tests.find(Node.Test.get()); It != Tests.end())
            StatementNodes.emplace(It->second, N);
      }
      for (auto *Values : {&Flow.Nodes[N].Reads, &Flow.Nodes[N].Writes}) {
        std::sort(Values->begin(), Values->end());
        Values->erase(std::unique(Values->begin(), Values->end()),
                      Values->end());
      }
    }
    if (Flow.Variables.size() >
        ::neverd::limits::ir::
            kHighVariableEditsProjectionFlowVariablesSizeLimit) {
      Flow.Refusal = "Too many source values for an interactive association";
      return;
    }
    size_t ProofWork = 0;
    for (unsigned A = 0; A < Variables.size(); ++A)
      if (Variables[A].Slot) {
        if (FrameUnsafe)
          refuse(A, "Frame storage has an escaping, aliased or dynamically "
                    "indexed address");
        for (unsigned B = 0; B < A; ++B)
          if (Variables[B].Slot) {
            if (++ProofWork > ::neverd::limits::ir::
                                  kHighVariableEditsProjectionProofWorkLimit) {
              Flow.Refusal = "The storage alias proof exceeded its budget";
              return;
            }
            const int64_t ABegin = *Variables[A].Slot,
                          BBegin = *Variables[B].Slot;
            if (ABegin < BBegin + Flow.Variables[B].Bytes &&
                BBegin < ABegin + Flow.Variables[A].Bytes) {
              refuse(A, "The stack byte interval overlaps another access or a "
                        "partial write");
              refuse(B, "The stack byte interval overlaps another access or a "
                        "partial write");
            }
          }
      }
  }
  void refuse(unsigned V, std::string Why) {
    Flow.Variables[V].Eligible = false;
    if (Flow.Variables[V].Refusal.empty())
      Flow.Variables[V].Refusal = std::move(Why);
  }
  unsigned value(const MedVar &V, const TypeRef &Type) {
    const std::string Id = highSourceVariableId(V);
    if (auto It = Ids.find(Id); It != Ids.end()) {
      if (Flow.Variables[It->second].Bytes != V.Size ||
          typeKey(Variables[It->second].Type) != typeKey(Type))
        refuse(It->second,
               "The local has incompatible scalar views or partial writes");
      return It->second;
    }
    const unsigned Index = Variables.size();
    Ids[Id] = Index;
    Variables.push_back({V, Type, {}});
    Flow.Variables.push_back(
        {Id, flowTypeKey(Type), V.Size, V.Kind == MedVar::Param});
    MaxId = std::max(MaxId, V.Id);
    if (!scalar(Type) || V.Size != Type->Size ||
        highSourceFrameBase(Function, V) ||
        (V.Kind != MedVar::Reg && V.Kind != MedVar::Temp &&
         V.Kind != MedVar::Stack && V.Kind != MedVar::Param))
      refuse(Index, "This value is composite, partial, a frame base or a "
                    "runtime-defined value");
    return Index;
  }
  std::optional<int64_t> displacement(const ExprPtr &E,
                                      unsigned Depth = 0) const {
    if (!E ||
        Depth > ::neverd::limits::ir::kHighVariableEditsDisplacementDepthLimit)
      return {};
    if ((E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) &&
        highSourceFrameBase(Function, E->Var))
      return 0;
    if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi)
      if (auto It = FrameAliases.find(highSourceVariableId(E->Var));
          It != FrameAliases.end())
        return It->second;
    if (E->Kind == ExprKind::Cast && E->Operands.size() == 1 &&
        E->Operands[0] && E->Type && E->Operands[0]->Type &&
        E->Type->Size == E->Operands[0]->Type->Size)
      return displacement(E->Operands[0], Depth + 1);
    if (E->Kind != ExprKind::BinOp || E->Operands.size() != 2 ||
        !E->Operands[1] || E->Operands[1]->Kind != ExprKind::Const ||
        (E->Op != NdOp::INT_ADD && E->Op != NdOp::INT_SUB))
      return {};
    auto Base = displacement(E->Operands[0], Depth + 1);
    if (!Base)
      return {};
    int64_t Delta = static_cast<int64_t>(E->Operands[1]->ConstVal);
    if (E->Operands[1]->Type && E->Operands[1]->Type->Size == 4)
      Delta = static_cast<int32_t>(Delta);
    if (Delta == std::numeric_limits<int64_t>::min())
      return {};
    if (E->Op == NdOp::INT_SUB)
      Delta = -Delta;
    if ((Delta > 0 && *Base > std::numeric_limits<int64_t>::max() - Delta) ||
        (Delta < 0 && *Base < std::numeric_limits<int64_t>::min() - Delta))
      return {};
    return *Base + Delta;
  }
  bool frame(const ExprPtr &E, unsigned Depth = 0) const {
    if (!E || Depth > ::neverd::limits::ir::kHighVariableEditsFrameDepthLimit)
      return Depth > ::neverd::limits::ir::kHighVariableEditsFrameDepthLimit;
    if (E->Kind == ExprKind::Load)
      return false;
    if ((E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) &&
        (highSourceFrameBase(Function, E->Var) ||
         FrameAliases.count(highSourceVariableId(E->Var))))
      return true;
    bool Found = false;
    E->forEachChildExpr(
        [&](const ExprPtr &C) { Found |= frame(C, Depth + 1); });
    return Found;
  }
  std::optional<unsigned> slot(const ExprPtr &Address, const TypeRef &Type,
                               NdMemoryOrdering Ordering,
                               NdMemoryAddressSpace Space) {
    const auto Disp = displacement(Address);
    if (!Disp) {
      if (frame(Address))
        FrameUnsafe = true;
      return {};
    }
    // Entry arguments and the return address are externally observable storage.
    if (*Disp >= 0 || !scalar(Type) ||
        *Disp > -static_cast<int64_t>(Type->Size) || Function.FrameSize < 0 ||
        Function.FrameHeadroom < 0 ||
        Function.FrameSize >
            std::numeric_limits<int64_t>::max() - Function.FrameHeadroom ||
        *Disp < -static_cast<int64_t>(Function.FrameSize +
                                      Function.FrameHeadroom)) {
      FrameUnsafe = true;
      return {};
    }
    const std::string Id = "stack:" + std::to_string(*Disp);
    unsigned Index;
    if (auto It = Ids.find(Id); It != Ids.end()) {
      Index = It->second;
      if (!Variables[Index].Slot)
        refuse(Index, "Logical and address-based stack storage overlap");
      if (Flow.Variables[Index].Bytes != Type->Size ||
          (Variables[Index].Type->Kind == NdTypeKind::Float) !=
              (Type->Kind == NdTypeKind::Float))
        refuse(
            Index,
            "The slot has partial accesses or mixed floating-point bit views");
    } else {
      Index = Variables.size();
      Ids[Id] = Index;
      const TypeRef Carrier = Type->Kind == NdTypeKind::Float
                                  ? Type
                                  : NdType::makeInt(Type->Size, false);
      Variables.push_back({{}, Carrier, *Disp});
      Flow.Variables.push_back({Id, flowTypeKey(Carrier), Type->Size});
    }
    if (Ordering != NdMemoryOrdering::None ||
        Space != NdMemoryAddressSpace::Default)
      refuse(Index, "Volatile, atomic and non-default-address-space storage "
                    "cannot be split or mapped");
    return Index;
  }
  void read(const ExprPtr &E, size_t N, unsigned Depth = 0) {
    if (!E)
      return;
    if (++Visits > ::neverd::limits::ir::kHighVariableEditsReadVisitsLimit ||
        Depth > ::neverd::limits::ir::kHighVariableEditsReadDepthLimit) {
      Flow.Refusal = "The source expression edit budget was exceeded";
      return;
    }
    if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) {
      if (highSourceFrameBase(Function, E->Var) ||
          FrameAliases.count(highSourceVariableId(E->Var)))
        FrameUnsafe = true;
      Flow.Nodes[N].Reads.push_back(value(E->Var, E->Type));
      return;
    }
    if (E->Kind == ExprKind::Load && E->Operands.size() == 1) {
      if (auto V = slot(E->Operands[0], E->Type, E->MemoryOrdering,
                        E->MemoryAddressSpace)) {
        Flow.Nodes[N].Reads.push_back(*V);
        return;
      }
    }
    if (E->Kind == ExprKind::Addr) {
      unsigned EscapeVisits = 0;
      std::function<void(const ExprPtr &, unsigned)> Escape =
          [&](const ExprPtr &C, unsigned Level) {
            if (!C)
              return;
            if (++EscapeVisits >
                    ::neverd::limits::ir::
                        kHighVariableEditsReadEscapeEscapeVisitsLimit ||
                Level > ::neverd::limits::ir::kMaxVariableEditNesting) {
              Flow.Refusal = "The address escape proof exceeded its budget";
              return;
            }
            if (C->Kind == ExprKind::Load && !C->Operands.empty() &&
                frame(C->Operands[0]))
              FrameUnsafe = true;
            if (C->Kind == ExprKind::Var || C->Kind == ExprKind::Phi)
              refuse(value(C->Var, C->Type),
                     "The variable's address is observed");
            C->forEachChildExpr(
                [&](const ExprPtr &Child) { Escape(Child, Level + 1); });
          };
      Escape(E, 0);
    }
    if (frame(E))
      FrameUnsafe = true;
    for (const auto &Output : E->IntrinsicOutputs)
      refuse(value(Output, NdType::makeInt(Output.Size)),
             "An intrinsic defines this value implicitly");
    E->forEachChildExpr([&](const ExprPtr &C) { read(C, N, Depth + 1); });
  }
  std::optional<unsigned> wholeValue(const ExprPtr &E) {
    if (!E)
      return {};
    if (E->Kind == ExprKind::Cast && E->Operands.size() == 1 &&
        E->Operands[0] && E->Type && E->Operands[0]->Type &&
        E->Type->Size == E->Operands[0]->Type->Size &&
        E->Type->Kind == NdTypeKind::Int &&
        E->Operands[0]->Type->Kind == NdTypeKind::Int)
      return wholeValue(E->Operands[0]);
    if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi)
      return value(E->Var, E->Type);
    if (E->Kind == ExprKind::Load && E->Operands.size() == 1)
      return slot(E->Operands[0], E->Type, E->MemoryOrdering,
                  E->MemoryAddressSpace);
    return {};
  }
  void statement(const HighStmt &S, size_t N) {
    auto &Node = Flow.Nodes[N];
    if (S.Kind == StmtKind::Assign && S.Dst &&
        (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi)) {
      if (FrameAliases.count(highSourceVariableId(S.Dst->Var)))
        return;
      const unsigned D = value(S.Dst->Var, S.Dst->Type);
      Node.Writes.push_back(D);
      read(S.Val, N);
      unsigned UnknownVisits = 0;
      std::function<bool(const ExprPtr &, unsigned)> Unknown =
          [&](const ExprPtr &E, unsigned Depth) {
            if (++UnknownVisits >
                    ::neverd::limits::ir::
                        kHighVariableEditsStatementUnknownUnknownVisitsLimit ||
                Depth > ::neverd::limits::ir::
                            kHighVariableEditsStatementUnknownDepthLimit)
              return true;
            if (!E)
              return true;
            if (E->Kind == ExprKind::Undef ||
                E->Kind == ExprKind::EntryRegister)
              return true;
            bool Result = false;
            E->forEachChildExpr([&](const ExprPtr &Child) {
              Result |= Unknown(Child, Depth + 1);
            });
            return Result;
          };
      if (Unknown(S.Val, 0))
        refuse(D, "The local has an unknown or runtime-defined reaching value");
      if (auto V = wholeValue(S.Val))
        Node.Copy = {{D, *V}};
      return;
    }
    if (S.Kind == StmtKind::Store && S.StoreVal) {
      if (auto D = slot(S.StoreAddr, S.StoreVal->Type, S.MemoryOrdering,
                        S.MemoryAddressSpace)) {
        Node.Writes.push_back(*D);
        read(S.StoreVal, N);
        if (auto V = wholeValue(S.StoreVal))
          Node.Copy = {{*D, *V}};
        return;
      }
    }
    forEachExpr(S, [&](const ExprPtr &E) { read(E, N); });
  }
};
ExprPtr viewOf(MedVar Variable, const TypeRef &Carrier, const TypeRef &View) {
  ExprPtr Value = HighExpr::makeVar(Variable, Carrier);
  if (typeKey(Carrier) == typeKey(View))
    return Value;
  auto Cast = std::make_shared<HighExpr>();
  Cast->Kind = ExprKind::Cast;
  Cast->Type = View;
  Cast->CastTo = View;
  Cast->Operands = {Value};
  return Cast;
}
} // namespace

HighVariableEditResult neverd::editHighVariable(const HighFunc &Function,
                                                const HighSourceMap &Sources,
                                                const HighVariableEdit &Edit) {
  HighVariableEditResult Result;
  Projection P(Function);
  if (!P.Flow.Refusal.empty()) {
    Result.Refusal = P.Flow.Refusal;
    return Result;
  }
  const auto From = P.Ids.find(Edit.From), To = P.Ids.find(Edit.To);
  if (From == P.Ids.end() ||
      (Edit.TheKind == HighVariableEdit::Map && To == P.Ids.end())) {
    Result.Refusal =
        "The selected source value is not available in the current function";
    return Result;
  }
  SourceVariableSplit Split;
  if (Edit.TheKind == HighVariableEdit::Map)
    Result.Refusal = canMapSourceVariables(P.Flow, From->second, To->second);
  else {
    Split = splitSourceVariable(P.Flow, From->second);
    Result.Refusal = Split.Refusal;
  }
  if (!Result.Refusal.empty())
    return Result;
  const unsigned Count =
      Edit.TheKind == HighVariableEdit::Map ? 1 : Split.Partitions;
  if (P.MaxId > std::numeric_limits<int>::max() - static_cast<int>(Count) - 1) {
    Result.Refusal = "The source temporary identity space was exhausted";
    return Result;
  }
  const Variable &Original = P.Variables[From->second];
  const Variable &Target = Edit.TheKind == HighVariableEdit::Map
                               ? P.Variables[To->second]
                               : Original;
  std::vector<MedVar> Values(Count);
  for (unsigned I = 0; I < Count; ++I) {
    if (Edit.TheKind == HighVariableEdit::Map && !Target.Slot)
      Values[I] = Target.Value;
    else {
      Values[I].Kind = MedVar::Temp;
      Values[I].Id = ++P.MaxId;
      Values[I].Size = Target.Type->Size;
    }
    Result.Outputs.push_back(highSourceVariableId(Values[I]));
  }
  std::map<const HighExpr *, std::vector<ExprPtr>> Copies;
  std::set<std::pair<va_t, StmtKind>> ChangedStatements;
  Result.Function = Function;
  std::vector<const HighStmt *> Originals;
  walkStmts(Function.Body, [&](const HighStmt &S) { Originals.push_back(&S); });
  size_t Statement = 0;
  walkStmts(Result.Function.Body, [&](HighStmt &S) {
    const HighStmt *Old = Originals.at(Statement++);
    const auto FoundNode = P.StatementNodes.find(Old);
    const size_t Node = FoundNode == P.StatementNodes.end()
                            ? P.Flow.Nodes.size()
                            : FoundNode->second;
    auto Partition = [&](bool Definition) {
      if (Edit.TheKind == HighVariableEdit::Map)
        return 0u;
      if (Node == P.Flow.Nodes.size())
        return SourceVariableSplit::None;
      return Definition ? Split.Definitions[Node] : Split.Uses[Node];
    };
    auto Matches = [&](const ExprPtr &E) {
      if (!E)
        return false;
      if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi)
        return highSourceVariableId(E->Var) == Edit.From ||
               (Edit.TheKind == HighVariableEdit::Map &&
                highSourceVariableId(E->Var) == Edit.To && Target.Slot);
      if (E->Kind == ExprKind::Load && E->Operands.size() == 1) {
        const auto Disp = P.displacement(E->Operands[0]);
        return (Original.Slot && Disp == Original.Slot) ||
               (Edit.TheKind == HighVariableEdit::Map && Target.Slot &&
                Disp == Target.Slot);
      }
      return false;
    };
    std::map<std::pair<const HighExpr *, bool>, ExprPtr> Local;
    std::function<void(ExprPtr &, bool)> Rewrite = [&](ExprPtr &E,
                                                       bool Definition) {
      if (!E)
        return;
      const auto Key = std::make_pair(E.get(), Definition);
      if (auto It = Local.find(Key); It != Local.end()) {
        E = It->second;
        return;
      }
      const HighExpr *Before = E.get();
      ExprPtr Next = std::make_shared<HighExpr>(*E);
      const unsigned Part = Partition(Definition);
      if (Matches(E) && Part != SourceVariableSplit::None) {
        Next = Definition ? HighExpr::makeVar(Values[Part], Target.Type)
                          : viewOf(Values[Part], Target.Type, E->Type);
      } else {
        for (auto &Child : Next->Operands)
          Rewrite(Child, false);
        Rewrite(Next->IndirectTarget, false);
      }
      Local[Key] = Next;
      Copies[Before].push_back(Next);
      E = std::move(Next);
    };
    bool Store = false;
    if (S.Kind == StmtKind::Store && S.StoreVal) {
      const auto Disp = P.displacement(S.StoreAddr);
      Store = (Original.Slot && Disp == Original.Slot) ||
              (Edit.TheKind == HighVariableEdit::Map && Target.Slot &&
               Disp == Target.Slot);
    }
    if (Store && Partition(true) != SourceVariableSplit::None) {
      ExprPtr Value = S.StoreVal;
      Rewrite(Value, false);
      const va_t Address = S.Addr;
      ChangedStatements.emplace(Address, S.Kind);
      S = HighStmt{};
      S.Kind = StmtKind::Assign;
      S.Addr = Address;
      S.Dst = HighExpr::makeVar(Values[Partition(true)], Target.Type);
      S.Val = std::move(Value);
      S.KeepsName = true;
    } else {
      if (S.Kind == StmtKind::Assign && S.Dst &&
          (Matches(S.Dst) ||
           (Edit.TheKind == HighVariableEdit::Map &&
            (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi) &&
            highSourceVariableId(S.Dst->Var) == Edit.To)))
        S.KeepsName = true;
      Rewrite(S.Dst, true);
      forEachRhsExpr(S, [&](ExprPtr &E) { Rewrite(E, false); });
    }
  });
  for (const auto &Observation : Sources) {
    if (Observation.Kind == HighSourceKind::Statement ||
        Observation.Kind == HighSourceKind::Store) {
      // Keep statement observations at their original
      // instruction identity only while the projection retains the statement
      // kind; scalarizing a stack store must not inherit its old store anchor.
      if (Observation.Function == Function.Entry && Observation.StatementKind &&
          !ChangedStatements.contains(
              {Observation.Occurrence.Address, *Observation.StatementKind}))
        Result.Sources.push_back(Observation);
      continue;
    }
    const auto Expression = Observation.Expression.lock();
    if (Expression)
      if (const auto Found = Copies.find(Expression.get());
          Found != Copies.end())
        for (const auto &Copy : Found->second) {
          auto Mapped = Observation;
          Mapped.Expression = Copy;
          Result.Sources.push_back(std::move(Mapped));
        }
  }
  return Result;
}
