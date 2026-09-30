//===- HighSourceFlow.cpp - Source flow analysis
//---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/ir/high/HighSourceFlow.h"

#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/bit.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace neverd {
HighSourceLocalIdentity highSourceLocalIdentity(const MedVar &Variable) {
  // Phi cleanup can merge different SSA values/kinds into one emitted local.
  // Unrenamed kinds and SSA values must not borrow each other's definitions.
  if (Variable.RenameTag >= 0)
    return {-1, Variable.RenameTag, 0, 0};
  if (Variable.Kind == MedVar::Stack)
    return {MedVar::Stack, 0, 0, Variable.StackOff};
  if (Variable.Kind == MedVar::RetVal)
    return {MedVar::RetVal, 0, 0, 0};
  return {Variable.Kind, Variable.Id, Variable.SSAVer, 0};
}

bool highSourceFrameBase(const HighFunc &Function, const MedVar &Variable) {
  return Variable.Kind == MedVar::Reg && Variable.SSAVer == 0 &&
         Variable.RenameTag < 0 &&
         (Variable.TheArch == Arch::X64 || Variable.TheArch == Arch::AArch64) &&
         (Function.FrameSize > 0 || Function.FrameHeadroom > 0) &&
         Variable.RegOff == getTargetRegInfo(Variable.TheArch).StackPointer;
}

void HighSourceFlowReport::add(HighSourceFlowIssue Issue, std::string Reason,
                               va_t Address, const HighExpr *Expression,
                               va_t RelatedAddress) {
  constexpr size_t MaxDiagnostics = 4096;
  if (Items.size() < MaxDiagnostics)
    Items.push_back(
        {Issue, std::move(Reason), Address, Expression, RelatedAddress});
  else if (Items.size() == MaxDiagnostics) {
    Items.push_back(
        {HighSourceFlowIssue::Budget,
         "source projection evidence exceeds its diagnostic limit"});
    Complete = false;
  }
}
namespace {
/// This graph models emitted statement control flow, not the earlier native
/// CFG. In particular, source switch arms have implicit breaks, and For has
/// only a condition and body. Goto edges are resolved after all source labels
/// are known, including edges entering or leaving a structured region.
class SourceFlow {
  static constexpr size_t NoNode = std::numeric_limits<size_t>::max();
  static constexpr size_t MaxNodes = 100000;
  static constexpr size_t MaxEdges = 4 * MaxNodes;
  static constexpr size_t MaxSwitchCases = 4096;
  static constexpr size_t MaxLocals = 8192;
  static constexpr size_t MaxOperations = 10000000;
  static constexpr size_t MaxStateWords = 8 * 1024 * 1024; // 64 MiB.
  struct Failure {
    std::string Reason;
    HighSourceFlowIssue Issue;
    va_t Address;
  };
  struct Predicate {
    size_t Local;
    size_t Other;
    uint16_t Width;
    bool Nonzero;
    std::pair<size_t, size_t> identity() const { return {Local, Other}; }
  };
  struct Node {
    std::vector<size_t> Next, Previous, Uses, Writes;
    std::vector<std::optional<Predicate>> EdgeFacts;
    std::vector<std::optional<bool>> EdgeTruth;
    const HighStmt *Statement = nullptr;
    const HighStmt *ExpressionStatement = nullptr;
    ExprPtr Test;
    bool PhiCopy = false;
    std::optional<size_t> Definition;
    bool Returns = false;
    va_t Address = 0;
    std::map<size_t, const HighExpr *> UseExpressions;
  };
  struct Control {
    size_t Break = NoNode, Continue = NoNode;
  };

  const HighFunc &Function;
  HighSourceFlowReport &Diagnostics;
  va_t CurrentAddress = 0;
  std::vector<Node> Nodes;
  std::map<HighSourceLocalIdentity, size_t> Locals;
  std::map<va_t, size_t> Labels;
  std::vector<std::pair<size_t, va_t>> Gotos;
  size_t Operations = 0, EdgeCount = 0, CaseCount = 0;
  std::set<size_t> AddressTaken;
  std::set<const HighStmt *> *DeadCopies;
  // Guard refinement only removes infeasible paths. PHI cleanup uses the
  // complete graph, a superset of every refined path.
  bool Refine = true;
  // Resolve a shared address the way the emitter groups branch entries,
  // rather than requiring one statement per address.
  bool EmitterLabels = false;
  struct Site {
    const std::vector<HighStmt> *List = nullptr;
    size_t Position = 0;
    const HighStmt *Parent = nullptr;
    size_t Order = 0; // Source pre-order.
  };
  std::map<const HighStmt *, Site> Sites;
  std::map<va_t, std::vector<size_t>> Entries;
  // Which PHI copies of a computed value PHI cleanup may erase when unread.
  const std::function<bool(const HighStmt &)> *Erasable = nullptr;

  [[noreturn]] void
  fail(const char *Reason,
       HighSourceFlowIssue Issue = HighSourceFlowIssue::ControlFlow) {
    throw Failure{Reason, Issue, CurrentAddress};
  }
  void spend(size_t Count = 1) {
    if (Count > MaxOperations - Operations)
      fail("method source-flow analysis exceeds its work limit",
           HighSourceFlowIssue::Budget);
    Operations += Count;
  }
  size_t node() {
    spend();
    if (Nodes.size() == MaxNodes)
      fail("method source-flow graph exceeds its node limit",
           HighSourceFlowIssue::Budget);
    Nodes.emplace_back();
    return Nodes.size() - 1;
  }
  void edge(size_t From, size_t To,
            std::optional<Predicate> Fact = std::nullopt,
            std::optional<bool> Truth = std::nullopt) {
    CurrentAddress = Nodes[From].Address;
    spend();
    if (To == NoNode)
      fail("method has a break or continue outside its source control scope");
    if (EdgeCount == MaxEdges)
      fail("method source-flow graph exceeds its edge limit",
           HighSourceFlowIssue::Budget);
    ++EdgeCount;
    Nodes[From].Next.push_back(To);
    Nodes[From].EdgeFacts.push_back(Fact);
    Nodes[From].EdgeTruth.push_back(Truth);
    Nodes[To].Previous.push_back(From);
  }
  size_t local(const MedVar &Variable) {
    spend();
    const auto Identity = highSourceLocalIdentity(Variable);
    if (auto It = Locals.find(Identity); It != Locals.end())
      return It->second;
    if (Locals.size() == MaxLocals)
      fail("method source-flow graph exceeds its local-value limit",
           HighSourceFlowIssue::Budget);
    return Locals.emplace(Identity, Locals.size()).first->second;
  }
  bool entryValue(const MedVar &Variable) const {
    // The outer source validator still checks every parameter's exact ABI.
    return Variable.Kind == MedVar::Param ||
           highSourceFrameBase(Function, Variable);
  }
  void reads(size_t Index, const ExprPtr &Root) {
    CurrentAddress = Nodes[Index].Address;
    if (!Root)
      return;
    std::vector<std::pair<const HighExpr *, unsigned>> Pending{{Root.get(), 1}};
    std::map<const HighExpr *, unsigned> Seen;
    while (!Pending.empty()) {
      spend();
      auto [Expression, Depth] = Pending.back();
      Pending.pop_back();
      if (Depth > 200)
        fail("method expression exceeds the source projection depth limit",
             HighSourceFlowIssue::Budget);
      auto [It, Fresh] = Seen.emplace(Expression, Depth);
      if (!Fresh && It->second >= Depth)
        continue;
      It->second = Depth;
      if ((Expression->Kind == ExprKind::Var ||
           Expression->Kind == ExprKind::Phi) &&
          !entryValue(Expression->Var)) {
        const size_t Local = local(Expression->Var);
        Nodes[Index].Uses.push_back(Local);
        Nodes[Index].UseExpressions.emplace(Local, Expression);
      }
      // Only a direct address-of-local can expose an emitted C local to a
      // memory write. Its identity must never carry a stable branch fact.
      if (Expression->Kind == ExprKind::Addr)
        for (const auto &Operand : Expression->Operands)
          if (Operand && (Operand->Kind == ExprKind::Var ||
                          Operand->Kind == ExprKind::Phi))
            AddressTaken.insert(local(Operand->Var));
      for (const auto &Output : Expression->IntrinsicOutputs)
        Nodes[Index].Writes.push_back(local(Output));
      spend(Expression->Operands.size());
      for (const auto &Operand : Expression->Operands) {
        if (!Operand)
          fail("method contains a missing expression operand",
               HighSourceFlowIssue::MalformedExpression);
        Pending.emplace_back(Operand.get(), Depth + 1);
      }
      // The function an indirect call runs is read like an operand.
      if (Expression->IndirectTarget) {
        spend(1);
        Pending.emplace_back(Expression->IndirectTarget.get(), Depth + 1);
      }
    }
  }
  static bool terminates(const ExprPtr &Expression) {
    return isTerminatingHighCall(Expression);
  }
  static std::optional<bool> truth(const ExprPtr &Expression) {
    if (!Expression)
      return true; // The C emitter renders a missing loop test as true.
    if (Expression->Kind != ExprKind::Const || !Expression->Type ||
        Expression->Type->Kind != NdTypeKind::Int)
      return std::nullopt;
    return Expression->ConstVal != 0;
  }
  static bool scalarLocal(const ExprPtr &Expression) {
    if (!Expression ||
        (Expression->Kind != ExprKind::Var &&
         Expression->Kind != ExprKind::Phi) ||
        !Expression->Operands.empty() ||
        !Expression->IntrinsicOutputs.empty() ||
        Expression->MemoryOrdering != NdMemoryOrdering::None ||
        Expression->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        !Expression->Type ||
        (Expression->Type->Kind != NdTypeKind::Int &&
         Expression->Type->Kind != NdTypeKind::Ptr) ||
        Expression->Type->Size != Expression->Var.Size ||
        !Expression->Type->Size || Expression->Type->Size > 8)
      return false;
    // Stack storage can be addressed through the synthetic frame. A register,
    // temporary, or parameter denotes an ordinary emitted scalar local.
    return Expression->Var.Kind == MedVar::Reg ||
           Expression->Var.Kind == MedVar::Temp ||
           Expression->Var.Kind == MedVar::Param;
  }
  std::optional<Predicate> predicate(const ExprPtr &Expression) {
    ExprPtr Value = Expression;
    ExprPtr Other;
    bool Nonzero = true;
    if (Value && Value->Kind == ExprKind::BinOp &&
        (Value->Op == NdOp::INT_EQUAL || Value->Op == NdOp::INT_NOTEQUAL) &&
        Value->Operands.size() == 2) {
      auto IsZero = [](const ExprPtr &E) {
        return E && E->Kind == ExprKind::Const && E->ConstVal == 0 && E->Type &&
               E->Type->Kind == NdTypeKind::Int && E->Operands.empty();
      };
      Nonzero = Value->Op == NdOp::INT_NOTEQUAL;
      if (IsZero(Value->Operands[1]))
        Value = Value->Operands[0];
      else if (IsZero(Value->Operands[0]))
        Value = Value->Operands[1];
      else {
        Other = Value->Operands[1];
        Value = Value->Operands[0];
      }
    }
    // An integer cast of unchanged width preserves equality and zero tests.
    // Narrowing, extension and floating conversion do not preserve these
    // facts. Bound the peel so malformed expression cycles remain unknown.
    auto StripIntegerView = [](ExprPtr E) {
      for (unsigned Depth = 0; E && Depth != 16; ++Depth) {
        if (E->Kind != ExprKind::Cast || E->Operands.size() != 1 || !E->Type ||
            !E->CastTo || !E->Operands[0] || !E->Operands[0]->Type ||
            E->Type->Kind != NdTypeKind::Int ||
            E->CastTo->Kind != NdTypeKind::Int ||
            E->Operands[0]->Type->Kind != NdTypeKind::Int ||
            E->Type->Size < 4 || E->Type->Size != E->CastTo->Size ||
            E->Type->Size != E->Operands[0]->Type->Size ||
            !E->IntrinsicOutputs.empty() ||
            E->MemoryOrdering != NdMemoryOrdering::None ||
            E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
          break;
        E = E->Operands[0];
      }
      return E;
    };
    Value = StripIntegerView(Value);
    Other = StripIntegerView(Other);
    if (!scalarLocal(Value) || highSourceFrameBase(Function, Value->Var))
      return std::nullopt;
    size_t Left = local(Value->Var), Right = NoNode;
    if (Other) {
      if (!scalarLocal(Other) || highSourceFrameBase(Function, Other->Var) ||
          Value->Type->Kind != Other->Type->Kind ||
          Value->Type->Size != Other->Type->Size ||
          (Value->Type->Size < 4 &&
           Value->Type->IsSigned != Other->Type->IsSigned))
        return std::nullopt;
      Right = local(Other->Var);
      if (Right < Left)
        std::swap(Left, Right);
    }
    return Predicate{Left, Right, Value->Type->Size, Nonzero};
  }
  void branch(size_t Index, const ExprPtr &Condition, size_t Yes, size_t No) {
    Nodes[Index].Test = Condition;
    reads(Index, Condition);
    auto Known = truth(Condition);
    auto Fact = predicate(Condition);
    if (!Known || *Known)
      edge(Index, Yes, Fact, true);
    if (Fact)
      Fact->Nonzero = !Fact->Nonzero;
    if (!Known || !*Known)
      edge(Index, No, Fact, false);
  }

  size_t block(const std::vector<HighStmt> &Body, size_t Next, Control Scope,
               unsigned Depth) {
    if (Depth > 200)
      fail("method control flow exceeds the source projection depth limit",
           HighSourceFlowIssue::Budget);
    for (auto It = Body.rbegin(); It != Body.rend(); ++It)
      Next = statement(*It, Next, Scope, Depth);
    return Next;
  }
  size_t statement(const HighStmt &Statement, size_t Next, Control Scope,
                   unsigned Depth) {
    CurrentAddress = Statement.Addr;
    const size_t Index = node();
    Nodes[Index].Address = Statement.Addr;
    Nodes[Index].Statement = &Statement;
    Nodes[Index].ExpressionStatement = &Statement;
    if (Statement.Addr && Statement.Addr != InvalidVA) {
      auto [It, Fresh] = Labels.emplace(Statement.Addr, Index);
      if (!Fresh)
        It->second = NoNode; // An emitted goto label must be unique.
      if (EmitterLabels)
        Entries[Statement.Addr].push_back(Index);
    }
    switch (Statement.Kind) {
    case StmtKind::Assign:
      if (!Statement.Dst || !Statement.Val)
        fail("method source assignment is incomplete");
      reads(Index, Statement.Val);
      if (Statement.Dst->Kind == ExprKind::Var ||
          Statement.Dst->Kind == ExprKind::Phi) {
        const size_t Written = local(Statement.Dst->Var);
        Nodes[Index].Writes.push_back(Written);
        if (!entryValue(Statement.Dst->Var))
          Nodes[Index].Definition = Written;
        Nodes[Index].PhiCopy =
            Statement.IsPhiCopy && Statement.Body.empty() &&
            Statement.ElseBody.empty() && Statement.Cases.empty() &&
            Statement.DefaultBody.empty() && scalarLocal(Statement.Dst) &&
            !entryValue(Statement.Dst->Var) &&
            (scalarLocal(Statement.Val) ||
             (Statement.Val->Kind == ExprKind::Const &&
              Statement.Val->Operands.empty() && Statement.Val->Type &&
              Statement.Val->Type->Kind == NdTypeKind::Int));
      } else {
        reads(Index, Statement.Dst);
      }
      if (!terminates(Statement.Val))
        edge(Index, Next);
      break;
    case StmtKind::ExprStmt:
      reads(Index, Statement.Val);
      if (!terminates(Statement.Val))
        edge(Index, Next);
      break;
    case StmtKind::Store:
      if (!Statement.StoreAddr || !Statement.StoreVal)
        fail("method source store is incomplete");
      reads(Index, Statement.StoreAddr);
      reads(Index, Statement.StoreVal);
      edge(Index, Next);
      break;
    case StmtKind::Call:
      if (!Statement.CallExpr)
        fail("method source call is incomplete");
      reads(Index, Statement.CallExpr);
      if (!terminates(Statement.CallExpr))
        edge(Index, Next);
      break;
    case StmtKind::Return:
      reads(Index, Statement.RetVal);
      Nodes[Index].Returns = true;
      break;
    case StmtKind::Nop:
      edge(Index, Next);
      break;
    case StmtKind::Block:
      edge(Index, block(Statement.Body, Next, Scope, Depth + 1));
      break;
    case StmtKind::If:
    case StmtKind::IfElse: {
      if (!Statement.Cond ||
          (Statement.Kind == StmtKind::If && !Statement.ElseBody.empty()))
        fail("method source conditional has no complete branch structure");
      const size_t Yes = block(Statement.Body, Next, Scope, Depth + 1);
      const size_t No = Statement.Kind == StmtKind::IfElse
                            ? block(Statement.ElseBody, Next, Scope, Depth + 1)
                            : Next;
      branch(Index, Statement.Cond, Yes, No);
      break;
    }
    case StmtKind::While:
    case StmtKind::For: {
      const size_t Body =
          block(Statement.Body, Index, {Next, Index}, Depth + 1);
      branch(Index, Statement.Cond, Body, Next);
      break;
    }
    case StmtKind::DoWhile: {
      // A goto to the do statement enters its body, whereas continue and the
      // normal back edge evaluate its trailing condition first.
      const size_t Test = node();
      Nodes[Test].Address = Statement.Addr;
      Nodes[Index].ExpressionStatement = nullptr;
      Nodes[Test].ExpressionStatement = &Statement;
      const size_t Body = block(Statement.Body, Test, {Next, Test}, Depth + 1);
      edge(Index, Body);
      branch(Test, Statement.Cond, Body, Next);
      break;
    }
    case StmtKind::Switch: {
      if (!Statement.SwitchExpr)
        fail("method source switch has no recovered selector");
      reads(Index, Statement.SwitchExpr);
      Nodes[Index].Test = Statement.SwitchExpr;
      std::map<uint64_t, size_t> Cases;
      for (const auto &Case : Statement.Cases) {
        spend();
        if (CaseCount == MaxSwitchCases)
          fail("method source-flow graph exceeds its switch-case limit",
               HighSourceFlowIssue::Budget);
        ++CaseCount;
        const size_t Body =
            block(Case.Body, Next, {Next, Scope.Continue}, Depth + 1);
        if (!Cases.emplace(Case.Value, Body).second)
          fail("method source switch has duplicate case values");
      }
      const size_t Default =
          block(Statement.DefaultBody, Next, {Next, Scope.Continue}, Depth + 1);
      if (Statement.SwitchExpr->Kind == ExprKind::Const) {
        auto It = Cases.find(Statement.SwitchExpr->ConstVal);
        edge(Index, It == Cases.end() ? Default : It->second);
      } else {
        for (const auto &[Value, Body] : Cases)
          edge(Index, Body);
        edge(Index, Default);
      }
      break;
    }
    case StmtKind::Goto:
      Gotos.emplace_back(Index, Statement.GotoTarget);
      break;
    case StmtKind::Break:
      edge(Index, Scope.Break);
      break;
    case StmtKind::Continue:
      edge(Index, Scope.Continue);
      break;
    case StmtKind::SEHTry:
    case StmtKind::CxxTry:
    case StmtKind::ItaniumTry:
      fail("exception-dependent method projection is not supported",
           HighSourceFlowIssue::Exception);
    default:
      fail("method source contains an unsupported statement kind");
    }
    return Index;
  }

  // Partition the emitted CFG by repeated scalar equality tests, including
  // comparison with zero. Facts belong to individual edges, even those with
  // the same destination. A write to either operand kills the relation.
  // Inconsistent widths and escaped locals remain unknown; no memory-value,
  // alias inference is needed. A chain of unique, dominating scalar copies
  // may also carry the same relation to a later test.
  size_t refine(size_t Entry) {
    size_t BranchTests = 0;
    bool HasStoredBooleanTest = false;
    for (const auto &N : Nodes) {
      if (!N.Test || N.Next.size() <= 1)
        continue;
      ++BranchTests;
      auto Condition = N.Test;
      for (unsigned Depth = 0;
           Depth != 8 && Condition &&
           Condition->Kind == ExprKind::UnaryOp &&
           Condition->Op == NdOp::BOOL_NOT &&
           Condition->Operands.size() == 1 &&
           Condition->IntrinsicOutputs.empty() &&
           Condition->MemoryOrdering == NdMemoryOrdering::None &&
           Condition->MemoryAddressSpace == NdMemoryAddressSpace::Default;
           ++Depth)
        Condition = Condition->Operands[0];
      HasStoredBooleanTest |= scalarLocal(Condition);
    }
    if (BranchTests < 2)
      return Entry;
    struct Copy {
      size_t Source;
      NdTypeKind Kind;
      uint16_t Width;
      bool Signed;
      std::vector<size_t> Nodes;
    };
    std::vector<unsigned> WriteCount(Locals.size());
    std::vector<size_t> WriteAt(Locals.size(), NoNode);
    for (size_t I = 0; I < Nodes.size(); ++I)
      for (size_t Written : Nodes[I].Writes)
        if (Written < WriteCount.size()) {
          if (WriteCount[Written]++ == 0)
            WriteAt[Written] = I;
        }
    std::map<std::pair<size_t, size_t>, bool> Dominance;
    size_t DominanceWork = 0;
    auto Dominates = [&](size_t Definition, size_t Use) {
      const auto Key = std::pair{Definition, Use};
      if (auto It = Dominance.find(Key); It != Dominance.end())
        return It->second;
      bool Result = Definition != Use;
      std::vector<uint8_t> Seen(Nodes.size());
      std::vector<size_t> Pending{Entry};
      while (Result && !Pending.empty()) {
        const size_t Current = Pending.back();
        Pending.pop_back();
        if (++DominanceWork > 1000000) {
          Result = false;
          break;
        }
        if (Current == Definition || Seen[Current])
          continue;
        if (Current == Use) {
          Result = false;
          break;
        }
        Seen[Current] = 1;
        Pending.insert(Pending.end(), Nodes[Current].Next.begin(),
                       Nodes[Current].Next.end());
      }
      return Dominance.emplace(Key, Result).first->second;
    };
    // A compiler can emit the same scalar copy on both sides of a join. All
    // writes must copy one stable source, and every path to a comparison must
    // cross one of those writes before the local can borrow its source's fact.
    std::map<size_t, Copy> Copies;
    std::set<size_t> ConflictingCopies;
    for (size_t I = 0; I < Nodes.size(); ++I) {
      const auto *S = Nodes[I].Statement;
      if (!S || S->Kind != StmtKind::Assign || !scalarLocal(S->Dst) ||
          !scalarLocal(S->Val) || entryValue(S->Dst->Var) ||
          entryValue(S->Val->Var) || S->Dst->Type->Kind != S->Val->Type->Kind ||
          S->Dst->Type->Size != S->Val->Type->Size ||
          S->Dst->Type->IsSigned != S->Val->Type->IsSigned ||
          !S->Body.empty() || !S->ElseBody.empty() || !S->Cases.empty() ||
          !S->DefaultBody.empty())
        continue;
      const size_t Destination = local(S->Dst->Var);
      const size_t Source = local(S->Val->Var);
      if (Destination == Source || AddressTaken.count(Destination) ||
          AddressTaken.count(Source) || !WriteCount[Destination] ||
          !WriteCount[Source])
        continue;
      auto [It, Inserted] =
          Copies.try_emplace(Destination, Copy{Source,
                                               S->Dst->Type->Kind,
                                               S->Dst->Type->Size,
                                               S->Dst->Type->IsSigned,
                                               {}});
      if (!Inserted && (It->second.Source != Source ||
                        It->second.Kind != S->Dst->Type->Kind ||
                        It->second.Width != S->Dst->Type->Size ||
                        It->second.Signed != S->Dst->Type->IsSigned))
        ConflictingCopies.insert(Destination);
      else
        It->second.Nodes.push_back(I);
    }
    for (auto It = Copies.begin(); It != Copies.end();)
      if (ConflictingCopies.count(It->first) ||
          It->second.Nodes.size() != WriteCount[It->first] ||
          It->second.Nodes.size() > 8)
        It = Copies.erase(It);
      else
        ++It;
    std::map<std::pair<size_t, size_t>, bool> CopyDominance;
    auto CopyDominates = [&](size_t Local, size_t Use) {
      const auto &Definitions = Copies.at(Local).Nodes;
      if (Definitions.size() == 1)
        return Dominates(Definitions.front(), Use);
      const auto Key = std::pair{Local, Use};
      if (auto It = CopyDominance.find(Key); It != CopyDominance.end())
        return It->second;
      std::vector<uint8_t> Stops(Nodes.size()), Seen(Nodes.size());
      for (size_t Definition : Definitions)
        Stops[Definition] = 1;
      bool Result = true;
      std::vector<size_t> Pending{Entry};
      while (Result && !Pending.empty()) {
        const size_t Current = Pending.back();
        Pending.pop_back();
        if (++DominanceWork > 1000000) {
          Result = false;
          break;
        }
        if (Stops[Current] || Seen[Current])
          continue;
        if (Current == Use) {
          Result = false;
          break;
        }
        Seen[Current] = 1;
        Pending.insert(Pending.end(), Nodes[Current].Next.begin(),
                       Nodes[Current].Next.end());
      }
      return CopyDominance.emplace(Key, Result).first->second;
    };
    std::vector<std::vector<std::optional<Predicate>>> CanonicalFacts;
    // A one-write boolean can preserve a repeated equality even when neither
    // operand was copied. Avoid materializing edge facts for direct tests that
    // have no scalar-copy aliases to canonicalize.
    if (!Copies.empty() || HasStoredBooleanTest) {
      CanonicalFacts.reserve(Nodes.size());
      for (size_t I = 0; I < Nodes.size(); ++I) {
        CanonicalFacts.push_back(Nodes[I].EdgeFacts);
        // The emitter often stores a scalar equality in a one-write boolean
        // local before testing it. Borrow the comparison only when every path
        // to the test passes through that definition.
        if (const auto &Test = Nodes[I].Test) {
          ExprPtr Condition = Test;
          bool Inverted = false;
          for (unsigned Depth = 0;
               Depth != 8 && Condition &&
               Condition->Kind == ExprKind::UnaryOp &&
               Condition->Op == NdOp::BOOL_NOT &&
               Condition->Operands.size() == 1 &&
               Condition->IntrinsicOutputs.empty() &&
               Condition->MemoryOrdering == NdMemoryOrdering::None &&
               Condition->MemoryAddressSpace ==
                   NdMemoryAddressSpace::Default;
               ++Depth) {
            Inverted = !Inverted;
            Condition = Condition->Operands[0];
          }
          if (Condition && scalarLocal(Condition)) {
            const size_t Boolean = local(Condition->Var);
            if (WriteCount[Boolean] == 1 &&
                !AddressTaken.count(Boolean) &&
                Dominates(WriteAt[Boolean], I)) {
              const auto *Definition = Nodes[WriteAt[Boolean]].Statement;
              if (Definition && Definition->Kind == StmtKind::Assign &&
                  scalarLocal(Definition->Dst) &&
                  local(Definition->Dst->Var) == Boolean &&
                  Definition->Val &&
                  Definition->Val->Kind == ExprKind::BinOp &&
                  (Definition->Val->Op == NdOp::INT_EQUAL ||
                   Definition->Val->Op == NdOp::INT_NOTEQUAL) &&
                  Definition->Val->Type &&
                  Definition->Val->Type->Kind == NdTypeKind::Int &&
                  Definition->Val->Type->Size == Condition->Type->Size &&
                  Definition->Dst->Type->Size == Condition->Type->Size &&
                  Definition->Body.empty() &&
                  Definition->ElseBody.empty() &&
                  Definition->Cases.empty() &&
                  Definition->DefaultBody.empty())
                if (auto Relation = predicate(Definition->Val))
                  for (size_t E = 0; E < CanonicalFacts.back().size(); ++E)
                    if (Nodes[I].EdgeTruth[E]) {
                      auto Fact = *Relation;
                      Fact.Nonzero ^= Inverted ^ !*Nodes[I].EdgeTruth[E];
                      CanonicalFacts.back()[E] = Fact;
                    }
            }
          }
        }
        auto Canonical = [&](size_t Local) {
          const size_t Original = Local;
          std::set<size_t> Seen;
          while (Local != NoNode && Seen.insert(Local).second) {
            const auto It = Copies.find(Local);
            if (It == Copies.end())
              return Local;
            if (!CopyDominates(Local, I))
              return Original;
            const size_t Source = It->second.Source;
            if (const auto SourceCopy = Copies.find(Source);
                SourceCopy != Copies.end()) {
              if (SourceCopy->second.Kind != It->second.Kind ||
                  SourceCopy->second.Width != It->second.Width ||
                  SourceCopy->second.Signed != It->second.Signed ||
                  !CopyDominates(Source, I) ||
                  std::any_of(It->second.Nodes.begin(), It->second.Nodes.end(),
                              [&](size_t Definition) {
                                return !CopyDominates(Source, Definition);
                              }))
                return Original;
            } else if (WriteCount[Source] != 1 ||
                       !Dominates(WriteAt[Source], I) ||
                       std::any_of(
                           It->second.Nodes.begin(), It->second.Nodes.end(),
                           [&](size_t Definition) {
                             return !Dominates(WriteAt[Source], Definition);
                           }))
              return Original;
            Local = Source;
          }
          return Original;
        };
        for (auto &Fact : CanonicalFacts.back())
          if (Fact) {
            Fact->Local = Canonical(Fact->Local);
            Fact->Other = Canonical(Fact->Other);
            if (Fact->Other < Fact->Local)
              std::swap(Fact->Local, Fact->Other);
          }
      }
    }
    const auto Facts = [&](size_t I)
        -> const std::vector<std::optional<Predicate>> & {
      return CanonicalFacts.empty() ? Nodes[I].EdgeFacts : CanonicalFacts[I];
    };
    struct Observations {
      size_t Count = 0;
      uint16_t Width = 0;
      bool Consistent = true;
    };
    using Relation = std::pair<size_t, size_t>;
    std::map<Relation, Observations> Tests;
    for (size_t I = 0; I < Nodes.size(); ++I) {
      if (Facts(I).empty() || !Facts(I)[0])
        continue;
      const auto &Fact = *Facts(I)[0];
      auto &Seen = Tests[Fact.identity()];
      Seen.Consistent &= !Seen.Count || Seen.Width == Fact.Width;
      Seen.Width = Fact.Width;
      ++Seen.Count;
    }
    std::map<Relation, uint32_t> Masks;
    std::map<size_t, uint32_t> InvalidatedBy;
    for (const auto &[Pair, Seen] : Tests)
      if (Seen.Count > 1 && Seen.Consistent &&
          !AddressTaken.count(Pair.first) && !AddressTaken.count(Pair.second) &&
          Masks.size() < 32) {
        const uint32_t Mask = uint32_t{1} << Masks.size();
        Masks.emplace(Pair, Mask);
        InvalidatedBy[Pair.first] |= Mask;
        if (Pair.second != NoNode)
          InvalidatedBy[Pair.second] |= Mask;
      }
    if (Masks.empty())
      return Entry;

    struct State {
      size_t Original;
      uint32_t Known, Nonzero;
    };
    std::vector<Node> Refined;
    std::vector<State> States;
    using Key = std::tuple<size_t, uint32_t, uint32_t>;
    std::map<Key, size_t> Indices;
    std::vector<unsigned> Counts(Nodes.size());
    size_t Units = 0, Edges = 0;
    const size_t Limit = std::min(MaxNodes, Nodes.size() * 4 + 256);
    struct ExpansionLimit {};
    auto Add = [&](size_t Original, uint32_t Known, uint32_t Nonzero) {
      // Function exit has no uses or successors and needs no partition.
      if (!Original)
        Known = Nonzero = 0;
      Key K{Original, Known, Nonzero};
      if (auto It = Indices.find(K); It != Indices.end())
        return It->second;
      const auto &N = Nodes[Original];
      Units += 1 + N.Uses.size() + N.Writes.size();
      if (Refined.size() == Limit || Counts[Original] == 32 || Units > 1000000)
        throw ExpansionLimit{};
      ++Counts[Original];
      const size_t Index = Refined.size();
      Indices.emplace(K, Index);
      Refined.push_back(N);
      Refined.back().Next.clear();
      Refined.back().Previous.clear();
      Refined.back().EdgeFacts.clear();
      Refined.back().EdgeTruth.clear();
      States.push_back({Original, Known, Nonzero});
      return Index;
    };
    try {
      Add(0, 0, 0); // Keep the shared fallthrough exit at index zero.
      const size_t NewEntry = Add(Entry, 0, 0);
      for (size_t I = 1; I < States.size(); ++I) {
        const auto S = States[I];
        uint32_t Known = S.Known, Nonzero = S.Nonzero;
        const auto &Original = Nodes[S.Original];
        for (size_t Written : Original.Writes)
          if (auto It = InvalidatedBy.find(Written);
              It != InvalidatedBy.end()) {
            Known &= ~It->second;
            Nonzero &= ~It->second;
          }
        for (size_t E = 0; E < Original.Next.size(); ++E) {
          uint32_t NextKnown = Known, NextNonzero = Nonzero;
          if (auto Fact = Facts(S.Original)[E])
            if (auto It = Masks.find(Fact->identity()); It != Masks.end()) {
              const uint32_t Mask = It->second;
              if ((Known & Mask) && bool(Nonzero & Mask) != Fact->Nonzero)
                continue;
              NextKnown |= Mask;
              if (Fact->Nonzero)
                NextNonzero |= Mask;
              else
                NextNonzero &= ~Mask;
            }
          if (++Edges > MaxEdges)
            throw ExpansionLimit{};
          const size_t Target = Add(Original.Next[E], NextKnown, NextNonzero);
          Refined[I].Next.push_back(Target);
          Refined[I].EdgeTruth.push_back(Original.EdgeTruth[E]);
          Refined[Target].Previous.push_back(I);
        }
      }
      const size_t Words = (Locals.size() + 63) / 64;
      if (Words && Refined.size() > MaxStateWords / Words)
        return Entry;
      Nodes = std::move(Refined);
      return NewEntry;
    } catch (const ExpansionLimit &) {
      // Precision is optional. Discard the whole speculative graph and retain
      // the original conservative analysis when a partition budget is reached.
      return Entry;
    }
  }

  static uint64_t integerMask(unsigned Bytes) {
    return Bytes == 8 ? UINT64_MAX : (uint64_t{1} << (Bytes * 8)) - 1;
  }

  std::optional<uint64_t> indexRange(const ExprPtr &Value,
                                     std::set<size_t> &Dependencies,
                                     unsigned Depth = 0) {
    spend();
    if (!Value || Depth > 32 || !Value->Type ||
        Value->Type->Kind != NdTypeKind::Int || !Value->Type->Size ||
        Value->Type->Size > 8 || Value->IntrinsicId != Intrinsic::None ||
        !Value->IntrinsicOutputs.empty() ||
        Value->MemoryOrdering != NdMemoryOrdering::None ||
        Value->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return std::nullopt;
    const uint64_t Mask = integerMask(Value->Type->Size);
    if (Value->Kind == ExprKind::Const && Value->Operands.empty() &&
        Value->ConstVal <= Mask)
      return Value->ConstVal;
    if (Value->Kind == ExprKind::Var && scalarLocal(Value) &&
        !highSourceFrameBase(Function, Value->Var)) {
      const auto Local = local(Value->Var);
      if (AddressTaken.count(Local))
        return std::nullopt;
      Dependencies.insert(Local);
      return Mask;
    }
    if ((Value->Kind == ExprKind::Cast || Value->Kind == ExprKind::UnaryOp) &&
        Value->Operands.size() == 1) {
      const auto Input =
          indexRange(Value->Operands[0], Dependencies, Depth + 1);
      if (!Input)
        return std::nullopt;
      const auto InputBytes = Value->Operands[0]->Type->Size;
      if (Value->Kind == ExprKind::Cast) {
        if (!Value->CastTo || Value->CastTo->Kind != NdTypeKind::Int ||
            Value->CastTo->Size != Value->Type->Size ||
            InputBytes != Value->Type->Size)
          return std::nullopt;
        return Input;
      }
      if (Value->Type->Size < InputBytes)
        return std::nullopt;
      if (Value->Op == NdOp::INT_ZEXT)
        return Input;
      if (Value->Op == NdOp::INT_SEXT)
        return *Input < (uint64_t{1} << (InputBytes * 8 - 1)) ? *Input : Mask;
      return std::nullopt;
    }
    if (Value->Kind != ExprKind::BinOp || Value->Operands.size() != 2)
      return std::nullopt;
    const auto Left = indexRange(Value->Operands[0], Dependencies, Depth + 1);
    const auto Right = indexRange(Value->Operands[1], Dependencies, Depth + 1);
    if (!Left || !Right || Value->Operands[0]->Type->Size != Value->Type->Size)
      return std::nullopt;
    if (Value->Op == NdOp::INT_LEFT || Value->Op == NdOp::INT_RIGHT) {
      if (Value->Operands[1]->Kind != ExprKind::Const ||
          *Right >= Value->Type->Size * 8U)
        return std::nullopt;
      if (Value->Op == NdOp::INT_RIGHT)
        return *Left >> *Right;
      return *Left <= (Mask >> *Right) ? *Left << *Right : Mask;
    }
    if (Value->Operands[1]->Type->Size != Value->Type->Size)
      return std::nullopt;
    switch (Value->Op) {
    case NdOp::INT_AND:
      return std::min(*Left, *Right);
    case NdOp::INT_ADD:
      return *Left <= Mask - *Right ? *Left + *Right : Mask;
    case NdOp::INT_MULT:
      return !*Right || *Left <= Mask / *Right ? *Left * *Right : Mask;
    case NdOp::INT_SUB:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
      return Mask;
    default:
      return std::nullopt;
    }
  }

  bool purePredicate(const ExprPtr &Test, unsigned Depth = 0) {
    spend();
    if (!Test || Depth > 32 || !Test->Type ||
        Test->Type->Kind != NdTypeKind::Int || !Test->Type->Size ||
        Test->Type->Size > 8 || Test->IntrinsicId != Intrinsic::None ||
        !Test->IntrinsicOutputs.empty() ||
        Test->MemoryOrdering != NdMemoryOrdering::None ||
        Test->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        Test->IndirectTarget)
      return false;
    if (Test->Kind == ExprKind::Const || Test->Kind == ExprKind::Var ||
        Test->Kind == ExprKind::Phi || Test->Kind == ExprKind::Undef)
      return Test->Operands.empty();
    if (Test->Kind != ExprKind::BinOp && Test->Kind != ExprKind::UnaryOp &&
        Test->Kind != ExprKind::Cast && Test->Kind != ExprKind::BitCast &&
        Test->Kind != ExprKind::Field)
      return false;
    if (Test->Kind == ExprKind::BinOp || Test->Kind == ExprKind::UnaryOp)
      switch (Test->Op) {
      case NdOp::NOP:
      case NdOp::LOAD:
      case NdOp::STORE:
      case NdOp::ATOMIC_XCHG:
      case NdOp::ATOMIC_ADD:
      case NdOp::ATOMIC_CMPXCHG:
      case NdOp::BRANCH:
      case NdOp::COND_BR:
      case NdOp::INDIR_BR:
      case NdOp::CALL:
      case NdOp::INDIR_CALL:
      case NdOp::RETURN:
      case NdOp::INTRINSIC:
        return false;
      default:
        break;
      }
    // An unsupported arithmetic expression need not have a known range to
    // preserve the bound from another arm of a compound condition. It only
    // needs to be unable to change any source local while it is evaluated.
    return !Test->Operands.empty() &&
           std::all_of(Test->Operands.begin(), Test->Operands.end(),
                       [&](const ExprPtr &Operand) {
                         return purePredicate(Operand, Depth + 1);
                       });
  }

  // Intersections constrain both predicates. Unions retain every feasible
  // arm; no claim about a short-circuited operand can suppress another arm.
  std::optional<uint64_t> constrainPredicate(const ExprPtr &Test, bool Truth,
                                             const ExprPtr &Index,
                                             uint64_t Upper) {
    spend();
    if (Test->Kind == ExprKind::Const)
      return bool(Test->ConstVal) == Truth ? std::optional<uint64_t>(Upper)
                                           : std::nullopt;
    if (Test->Kind == ExprKind::UnaryOp && Test->Op == NdOp::BOOL_NOT)
      return constrainPredicate(Test->Operands[0], !Truth, Index, Upper);
    if (Test->Kind != ExprKind::BinOp)
      return Upper;
    if (Test->Op == NdOp::BOOL_AND || Test->Op == NdOp::BOOL_OR) {
      const bool Intersect = (Test->Op == NdOp::BOOL_AND) == Truth;
      const auto Left =
          constrainPredicate(Test->Operands[0], Truth, Index, Upper);
      if (Intersect)
        return Left ? constrainPredicate(Test->Operands[1], Truth, Index, *Left)
                    : std::nullopt;
      const auto Right =
          constrainPredicate(Test->Operands[1], Truth, Index, Upper);
      return Left && Right ? std::optional<uint64_t>(std::max(*Left, *Right))
             : Left        ? Left
                           : Right;
    }
    if (Test->Op != NdOp::INT_LESS && Test->Op != NdOp::INT_LESSEQUAL &&
        Test->Op != NdOp::INT_EQUAL && Test->Op != NdOp::INT_NOTEQUAL)
      return Upper;
    for (unsigned Side = 0; Side < 2; ++Side) {
      const auto &Value = Test->Operands[Side];
      const auto &Constant = Test->Operands[1 - Side];
      if (Constant->Kind != ExprKind::Const ||
          !Constant->Type || Constant->Type->Kind != NdTypeKind::Int ||
          !Constant->Type->Size ||
          Constant->Type->Size > Index->Type->Size ||
          !Value->structuralEq(*Index))
        continue;
      const auto Limit = Constant->ConstVal;
      if (Test->Op == NdOp::INT_EQUAL || Test->Op == NdOp::INT_NOTEQUAL) {
        if ((Test->Op == NdOp::INT_EQUAL) == Truth)
          return Limit <= Upper ? std::optional<uint64_t>(Limit) : std::nullopt;
        if (Upper == Limit)
          return Upper ? std::optional<uint64_t>(Upper - 1) : std::nullopt;
        return Upper;
      }
      const bool Inclusive = Test->Op == NdOp::INT_LESSEQUAL;
      const bool UpperEdge = Side == 0 ? Truth : !Truth;
      const bool StrictUpper = Side == 0 ? !Inclusive : Inclusive;
      if (UpperEdge) {
        if (StrictUpper && !Limit)
          return std::nullopt;
        return std::min(Upper, Limit - unsigned(StrictUpper));
      }
      const bool StrictLower = !StrictUpper;
      if (StrictLower ? Upper <= Limit : Upper < Limit)
        return std::nullopt;
    }
    return Upper;
  }

  // Edge truth survives predicate refinement. Effectful compound conditions
  // cannot carry a fact about a value that one of their operands might change.
  std::optional<uint64_t> constrainIndex(const ExprPtr &Test, bool Truth,
                                         const ExprPtr &Index, uint64_t Upper) {
    return purePredicate(Test) ? constrainPredicate(Test, Truth, Index, Upper)
                               : std::optional<uint64_t>(Upper);
  }

  void deadPhiCopies(const std::vector<bool> &Reachable, size_t Words) {
    std::map<const HighStmt *, std::vector<size_t>> Candidates;
    for (size_t I = 0; I < Nodes.size(); ++I)
      if (Reachable[I] && Nodes[I].PhiCopy && Nodes[I].Definition &&
          !AddressTaken.count(*Nodes[I].Definition))
        Candidates[Nodes[I].Statement].push_back(I);
    while (!Candidates.empty()) {
      spend(Nodes.size() * Words);
      std::vector<uint64_t> Live(Nodes.size() * Words, 0);
      std::vector<uint64_t> Incoming(Words);
      std::vector<size_t> Pending;
      std::vector<bool> Queued = Reachable;
      for (size_t I = 0; I < Nodes.size(); ++I)
        if (Reachable[I])
          Pending.push_back(I);
      auto Outputs = [&](size_t Index) {
        std::fill(Incoming.begin(), Incoming.end(), 0);
        for (size_t Next : Nodes[Index].Next) {
          spend(Words + 1);
          for (size_t W = 0; W < Words; ++W)
            Incoming[W] |= Live[Next * Words + W];
        }
      };
      while (!Pending.empty()) {
        spend();
        const size_t Index = Pending.back();
        Pending.pop_back();
        Queued[Index] = false;
        Outputs(Index);
        if (auto Definition = Nodes[Index].Definition)
          Incoming[*Definition / 64] &= ~(uint64_t{1} << (*Definition % 64));
        for (size_t Use : Nodes[Index].Uses) {
          spend();
          Incoming[Use / 64] |= uint64_t{1} << (Use % 64);
        }
        bool Changed = false;
        spend(Words);
        for (size_t W = 0; W < Words; ++W) {
          auto &State = Live[Index * Words + W];
          Changed |= State != Incoming[W];
          State = Incoming[W];
        }
        if (Changed)
          for (size_t Previous : Nodes[Index].Previous) {
            spend();
            if (Reachable[Previous] && !Queued[Previous]) {
              Queued[Previous] = true;
              Pending.push_back(Previous);
            }
          }
      }
      bool Changed = false;
      for (auto It = Candidates.begin(); It != Candidates.end();) {
        bool Dead = true;
        for (size_t Index : It->second) {
          Outputs(Index);
          const size_t Definition = *Nodes[Index].Definition;
          Dead &=
              !(Incoming[Definition / 64] & (uint64_t{1} << (Definition % 64)));
        }
        if (!Dead) {
          ++It;
          continue;
        }
        DeadCopies->insert(It->first);
        for (size_t Index : It->second) {
          Nodes[Index].Uses.clear();
          Nodes[Index].Definition.reset();
        }
        It = Candidates.erase(It);
        Changed = true;
      }
      if (!Changed)
        return;
      // A statement is erased only if dead in EVERY feasible context. Keeping
      // a read in one context must keep its dependencies in all contexts until
      // that entire source statement can be removed.
    }
  }

  // A register's value on entry. Lowering declares one read before any write
  // with a self-assignment; that names the caller's value, not a definition.
  static bool entryRegister(const MedVar &Variable) {
    return Variable.Kind == MedVar::Reg && Variable.SSAVer == 0;
  }

  static bool returnControlRegister(const MedVar &Variable) {
    return Variable.Kind == MedVar::Reg &&
           getTargetRegInfo(Variable.TheArch)
               .isReturnControlReg(Variable.RegOff);
  }

  void site(const std::vector<HighStmt> &Body, const HighStmt *Parent,
            unsigned Depth) {
    if (Depth > 200)
      fail("method control flow exceeds the source projection depth limit",
           HighSourceFlowIssue::Budget);
    for (size_t I = 0; I < Body.size(); ++I) {
      spend();
      const HighStmt &S = Body[I];
      Sites[&S] = {&Body, I, Parent, Sites.size()};
      site(S.Body, &S, Depth + 1);
      site(S.ElseBody, &S, Depth + 1);
      for (const auto &Case : S.Cases)
        site(Case.Body, &S, Depth + 1);
      site(S.DefaultBody, &S, Depth + 1);
    }
  }

  // A PHI copy leading an arm of the conditional that owns \p Address is
  // part of that branch, not a separate entry.
  bool leadingEdgeCopy(const HighStmt &S, const Site &Where,
                       va_t Address) const {
    const HighStmt *Owner = Where.Parent;
    if (!Owner ||
        (Owner->Kind != StmtKind::If && Owner->Kind != StmtKind::IfElse) ||
        !Owner->Cond || Owner->Addr != Address ||
        (Where.List != &Owner->Body && Where.List != &Owner->ElseBody))
      return false;
    for (size_t I = 0; I <= Where.Position; ++I) {
      const HighStmt &Copy = (*Where.List)[I];
      if (Copy.Kind != StmtKind::Assign || !Copy.IsPhiCopy ||
          Copy.Addr != Address || !Copy.Dst || !Copy.Val ||
          Copy.Dst->Kind != ExprKind::Var || !Copy.Body.empty() ||
          !Copy.ElseBody.empty() || !Copy.Cases.empty() ||
          !Copy.DefaultBody.empty() || !Copy.EHClauseBodies.empty())
        return false;
    }
    return &(*Where.List)[Where.Position] == &S;
  }

  // The node a goto to \p Address enters once statements sharing it are
  // grouped: one adjacent run in a single list is entered at its first
  // statement. Anything else is ambiguous.
  size_t emitterEntry(va_t Address) const {
    auto It = Entries.find(Address);
    if (It == Entries.end())
      return NoNode;
    const std::vector<HighStmt> *List = nullptr;
    size_t First = NoNode, Last = 0, Count = 0, Entry = NoNode;
    for (size_t Index : It->second) {
      const HighStmt *S = Nodes[Index].Statement;
      auto Where = Sites.find(S);
      if (!S || Where == Sites.end())
        return NoNode;
      if (leadingEdgeCopy(*S, Where->second, Address))
        continue;
      if (List && Where->second.List != List)
        return NoNode;
      List = Where->second.List;
      ++Count;
      if (Where->second.Position < First) {
        First = Where->second.Position;
        Entry = Index;
      }
      Last = std::max(Last, Where->second.Position);
    }
    return Count && Last - First + 1 == Count ? Entry : NoNode;
  }

  // Side-effect-free PHI copies no emitted path reads, found to a fixed point
  // so a chain of dead joins disappears as a whole. Then the scalar locals
  // joined by the remaining copies that may share one emitted name: each
  // write interferes with every candidate live after it, except the source
  // of a plain copy (both names would hold the same value). Copies merge
  // their classes in source order while no members of the two classes
  // interfere; a class keeps the name of the destination that started it.
  void phiCleanup(size_t Entry, std::set<const HighStmt *> &Dead,
                  std::vector<std::pair<MedVar, MedVar>> &Renames) {
    std::vector<bool> Reachable(Nodes.size());
    std::vector<size_t> Pending{Entry};
    Reachable[Entry] = true;
    for (size_t I = 0; I < Pending.size(); ++I) {
      spend();
      for (size_t Successor : Nodes[Pending[I]].Next)
        if (!Reachable[Successor]) {
          Reachable[Successor] = true;
          Pending.push_back(Successor);
        }
    }
    // Liveness is per local: only the PHI copy operands need it.
    std::vector<size_t> Dense(Locals.size(), NoNode);
    std::vector<size_t> Sparse;
    auto Track = [&](size_t Local) {
      if (Dense[Local] == NoNode) {
        Dense[Local] = Sparse.size();
        Sparse.push_back(Local);
      }
    };
    std::vector<size_t> CopyNodes;
    for (size_t I = 0; I < Nodes.size(); ++I) {
      spend();
      if (!Reachable[I] || !Nodes[I].Definition ||
          AddressTaken.count(*Nodes[I].Definition))
        continue;
      const HighStmt *S = Nodes[I].Statement;
      const bool Computed = S && S->Kind == StmtKind::Assign && S->IsPhiCopy &&
                            S->Body.empty() && S->ElseBody.empty() &&
                            S->Cases.empty() && S->DefaultBody.empty() &&
                            scalarLocal(S->Dst) && Erasable && (*Erasable)(*S);
      if (!Nodes[I].PhiCopy && !Computed)
        continue;
      CopyNodes.push_back(I);
      Track(*Nodes[I].Definition);
      for (size_t Use : Nodes[I].Uses)
        Track(Use);
    }
    if (CopyNodes.empty())
      return;
    const size_t Count = Sparse.size();
    const size_t Words = (Count + 63) / 64;
    if (Nodes.size() > MaxStateWords / Words ||
        Count > MaxStateWords / Words / 2)
      fail("method source-flow analysis exceeds its state memory limit",
           HighSourceFlowIssue::Budget);
    auto Set = [&](std::vector<uint64_t> &Bits, size_t Row, size_t Bit) {
      Bits[Row * Words + Bit / 64] |= uint64_t{1} << (Bit % 64);
    };

    // A copy a jump enters stays, as the label's statement, and keeps reading
    // its source; only the others can be erased.
    std::set<va_t> Entered;
    for (const auto &Jump : Gotos)
      Entered.insert(Jump.second);
    std::vector<bool> IsCopy(Nodes.size());
    for (size_t I : CopyNodes)
      IsCopy[I] = !Entered.count(Nodes[I].Statement->Addr);
    std::vector<uint64_t> Live(Nodes.size() * Words);
    std::vector<uint64_t> Out(Words);
    auto LiveOut = [&](size_t Index) {
      std::fill(Out.begin(), Out.end(), 0);
      for (size_t Next : Nodes[Index].Next) {
        spend(Words + 1);
        for (size_t W = 0; W < Words; ++W)
          Out[W] |= Live[Next * Words + W];
      }
    };
    auto LiveAfter = [&](size_t Local) {
      const size_t Bit = Dense[Local];
      return (Out[Bit / 64] >> (Bit % 64)) & 1;
    };
    // Strong liveness: a copy reads its source only for a destination that
    // is itself still read, so a cycle of copies keeps nothing alive.
    auto Solve = [&] {
      spend(Nodes.size() * Words);
      std::fill(Live.begin(), Live.end(), 0);
      std::vector<bool> Queued = Reachable;
      std::vector<size_t> Work;
      for (size_t I = 0; I < Nodes.size(); ++I)
        if (Reachable[I])
          Work.push_back(I);
      while (!Work.empty()) {
        spend();
        const size_t Index = Work.back();
        Work.pop_back();
        Queued[Index] = false;
        LiveOut(Index);
        const auto Definition = Nodes[Index].Definition;
        const bool Reads =
            !IsCopy[Index] || !Definition || LiveAfter(*Definition);
        if (Definition)
          if (const size_t Killed = Dense[*Definition]; Killed != NoNode)
            Out[Killed / 64] &= ~(uint64_t{1} << (Killed % 64));
        if (Reads)
          for (size_t Use : Nodes[Index].Uses)
            if (const size_t Read = Dense[Use]; Read != NoNode)
              Out[Read / 64] |= uint64_t{1} << (Read % 64);
        bool Changed = false;
        spend(Words);
        for (size_t W = 0; W < Words; ++W) {
          auto &State = Live[Index * Words + W];
          Changed |= State != Out[W];
          State = Out[W];
        }
        if (Changed)
          for (size_t Previous : Nodes[Index].Previous) {
            spend();
            if (Reachable[Previous] && !Queued[Previous]) {
              Queued[Previous] = true;
              Work.push_back(Previous);
            }
          }
      }
    };
    Solve();
    bool Erased = false;
    for (size_t I : CopyNodes) {
      if (!IsCopy[I])
        continue;
      LiveOut(I);
      if (LiveAfter(*Nodes[I].Definition))
        continue;
      // The copy is erased, so it neither reads nor writes any more.
      Dead.insert(Nodes[I].Statement);
      Nodes[I].Uses.clear();
      Nodes[I].Writes.clear();
      Nodes[I].Definition.reset();
      Erased = true;
    }
    if (Erased)
      Solve();

    std::vector<unsigned> Definitions(Locals.size());
    // A byte insert keeps a register's other bytes only because the machine
    // did. Left as its own value, dead-byte narrowing can drop them; joined
    // into a web they would become a real multi-byte value.
    std::vector<bool> ByteInsert(Locals.size());
    for (size_t I = 0; I < Nodes.size(); ++I)
      if (Reachable[I] && Nodes[I].Definition) {
        ++Definitions[*Nodes[I].Definition];
        const HighStmt *S = Nodes[I].Statement;
        if (S && S->Kind == StmtKind::Assign && S->Val &&
            S->Val->Kind == ExprKind::BinOp && S->Val->Op == NdOp::CONCAT)
          ByteInsert[*Nodes[I].Definition] = true;
      }
    struct Copy {
      size_t Position, Destination, Source;
    };
    std::vector<Copy> Copies;
    std::map<size_t, MedVar> Names;
    for (size_t I : CopyNodes) {
      if (!Nodes[I].Definition)
        continue;
      const HighStmt &S = *Nodes[I].Statement;
      auto Where = Sites.find(&S);
      if (Where == Sites.end() || !scalarLocal(S.Val) ||
          entryValue(S.Val->Var) || S.Val->Var.Size != S.Dst->Var.Size ||
          entryRegister(S.Dst->Var) || entryRegister(S.Val->Var) ||
          returnControlRegister(S.Dst->Var) ||
          returnControlRegister(S.Val->Var))
        continue;
      const size_t Destination = *Nodes[I].Definition;
      const size_t Source = local(S.Val->Var);
      if (Destination == Source || Dense[Source] == NoNode ||
          AddressTaken.count(Source) || !Definitions[Source] ||
          ByteInsert[Source] || ByteInsert[Destination])
        continue;
      Names.emplace(Destination, S.Dst->Var);
      Names.emplace(Source, S.Val->Var);
      Copies.push_back({Where->second.Order, Destination, Source});
    }
    if (Copies.empty())
      return;
    std::sort(Copies.begin(), Copies.end(), [](const Copy &A, const Copy &B) {
      return A.Position < B.Position;
    });

    std::vector<uint64_t> Interference(Count * Words, 0);
    for (size_t I = 0; I < Nodes.size(); ++I) {
      if (!Reachable[I] || Nodes[I].Writes.empty())
        continue;
      LiveOut(I);
      size_t Moved = NoNode;
      if (const HighStmt *S = Nodes[I].Statement;
          S && S->Kind == StmtKind::Assign && S->Val &&
          (S->Val->Kind == ExprKind::Var || S->Val->Kind == ExprKind::Phi) &&
          S->Val->Operands.empty() && !entryValue(S->Val->Var))
        Moved = local(S->Val->Var);
      for (size_t Written : Nodes[I].Writes) {
        const size_t Target = Dense[Written];
        if (Target == NoNode)
          continue;
        for (size_t W = 0; W < Words; ++W)
          for (uint64_t Bits = Out[W]; Bits; Bits &= Bits - 1) {
            spend();
            const size_t Other = W * 64 + llvm::countr_zero(Bits);
            if (Other == Target || Sparse[Other] == Moved)
              continue;
            Set(Interference, Target, Other);
            Set(Interference, Other, Target);
          }
      }
    }

    // A web whose only definitions are unknown values or copies within the
    // webs carries no value of its own. Joining two such webs changes no
    // name worth reading; it only hides which path left the value unknown.
    std::vector<bool> Real(Count);
    for (size_t I = 0; I < Nodes.size(); ++I) {
      if (!Reachable[I] || !Nodes[I].Definition)
        continue;
      const size_t Defined = Dense[*Nodes[I].Definition];
      if (Defined == NoNode)
        continue;
      const HighStmt *S = Nodes[I].Statement;
      const HighExpr *Value =
          S && S->Kind == StmtKind::Assign && S->Val ? S->Val.get() : nullptr;
      bool Known = !Value || Value->Kind != ExprKind::Undef;
      if (Known && Value &&
          (Value->Kind == ExprKind::Var || Value->Kind == ExprKind::Phi) &&
          Value->Operands.empty() && !entryValue(Value->Var))
        Known = Dense[local(Value->Var)] == NoNode;
      if (Known)
        Real[Defined] = true;
    }

    std::vector<size_t> Parent(Count);
    std::vector<uint64_t> Members(Count * Words, 0);
    for (size_t K = 0; K < Count; ++K) {
      Parent[K] = K;
      Set(Members, K, K);
    }
    auto Find = [&](size_t K) {
      while (Parent[K] != K)
        K = Parent[K] = Parent[Parent[K]];
      return K;
    };
    for (const Copy &C : Copies) {
      spend(Words);
      const size_t Root = Find(Dense[C.Destination]);
      const size_t Joined = Find(Dense[C.Source]);
      if (Root == Joined || (!Real[Root] && !Real[Joined]))
        continue;
      bool Conflict = false;
      for (size_t W = 0; W < Words && !Conflict; ++W)
        Conflict = Interference[Root * Words + W] & Members[Joined * Words + W];
      if (Conflict)
        continue;
      Parent[Joined] = Root;
      Real[Root] = true;
      for (size_t W = 0; W < Words; ++W) {
        Interference[Root * Words + W] |= Interference[Joined * Words + W];
        Members[Root * Words + W] |= Members[Joined * Words + W];
      }
    }
    for (size_t K = 0; K < Count; ++K)
      if (const size_t Root = Find(K); Root != K)
        Renames.emplace_back(Names.at(Sparse[K]), Names.at(Sparse[Root]));
  }

  std::optional<size_t> build() {
    node(); // Node zero is the emitted function's fallthrough exit.
    size_t Entry = block(Function.Body, 0, {}, 1);
    CurrentAddress = 0;
    bool MissingTarget = false;
    for (const auto &[Index, Address] : Gotos) {
      size_t Target = NoNode;
      if (Address && Address != InvalidVA) {
        if (EmitterLabels)
          Target = emitterEntry(Address);
        else if (auto It = Labels.find(Address); It != Labels.end())
          Target = It->second;
      }
      if (Target == NoNode) {
        MissingTarget = true;
        Diagnostics.Complete = false;
        Diagnostics.add(HighSourceFlowIssue::ControlFlow,
                        "method source goto has no unique emitted target",
                        Nodes[Index].Address, nullptr, Address);
      } else {
        edge(Index, Target);
      }
    }
    // Unknown edges invalidate reachability and must-defined conclusions.
    // The outer validator can still inventory independent expressions.
    if (MissingTarget)
      return std::nullopt;
    return Refine ? refine(Entry) : Entry;
  }

  void analyze(bool NeedsReturn) {
    const auto Built = build();
    if (!Built)
      return;
    const size_t Entry = *Built;
    std::vector<bool> Reachable(Nodes.size());
    std::vector<size_t> Pending{Entry};
    Reachable[Entry] = true;
    for (size_t I = 0; I < Pending.size(); ++I) {
      spend();
      const size_t Index = Pending[I];
      for (size_t Successor : Nodes[Index].Next) {
        spend();
        if (!Reachable[Successor]) {
          Reachable[Successor] = true;
          Pending.push_back(Successor);
        }
      }
    }
    if (Reachable[0] && (NeedsReturn || Function.DoesNotReturn))
      Diagnostics.add(
          HighSourceFlowIssue::ControlFlow,
          "method source has a reachable fallthrough exit without a return");
    for (size_t Index : Pending)
      if (Function.DoesNotReturn && Nodes[Index].Returns)
        Diagnostics.add(
            HighSourceFlowIssue::ControlFlow,
            "method source returns despite its noreturn declaration",
            Nodes[Index].Address);

    // Must-defined is a greatest fixed point: entry starts empty, all other
    // states start at top, and predecessor intersections only remove facts.
    // No facts are imported from unreachable code or a skipped loop body.
    const size_t Words = (Locals.size() + 63) / 64;
    CurrentAddress = 0;
    if (Words && Nodes.size() > MaxStateWords / Words)
      fail("method source-flow analysis exceeds its state memory limit",
           HighSourceFlowIssue::Budget);
    if (DeadCopies) {
      deadPhiCopies(Reachable, Words);
      return;
    }
    spend(Nodes.size() * Words);
    std::vector<uint64_t> States(Nodes.size() * Words, ~uint64_t{0});
    std::vector<uint64_t> Incoming(Words);
    std::vector<bool> Queued = Reachable;
    auto inputs = [&](size_t Index) {
      CurrentAddress = Nodes[Index].Address;
      std::fill(Incoming.begin(), Incoming.end(),
                Index == Entry ? 0 : ~uint64_t{0});
      for (size_t Predecessor : Nodes[Index].Previous) {
        spend();
        if (!Reachable[Predecessor])
          continue;
        spend(Words);
        for (size_t Word = 0; Word < Words; ++Word)
          Incoming[Word] &= States[Predecessor * Words + Word];
      }
    };
    while (!Pending.empty()) {
      spend();
      const size_t Index = Pending.back();
      Pending.pop_back();
      Queued[Index] = false;
      inputs(Index);
      if (auto Definition = Nodes[Index].Definition)
        Incoming[*Definition / 64] |= uint64_t{1} << (*Definition % 64);
      bool Changed = false;
      spend(Words);
      for (size_t Word = 0; Word < Words; ++Word) {
        auto &State = States[Index * Words + Word];
        Changed |= State != Incoming[Word];
        State = Incoming[Word];
      }
      if (Changed)
        for (size_t Successor : Nodes[Index].Next) {
          spend();
          if (!Queued[Successor]) {
            Queued[Successor] = true;
            Pending.push_back(Successor);
          }
        }
    }
    std::set<std::pair<const HighStmt *, size_t>> Reported;
    for (size_t Index = 0; Index < Nodes.size(); ++Index) {
      spend();
      if (!Reachable[Index])
        continue;
      inputs(Index);
      for (size_t Use : Nodes[Index].Uses) {
        spend();
        if (!(Incoming[Use / 64] & (uint64_t{1} << (Use % 64))) &&
            Reported.emplace(Nodes[Index].Statement, Use).second)
          Diagnostics.add(
              HighSourceFlowIssue::DefiniteAssignment,
              "method reads a local value before it is defined on every "
              "reaching source path",
              Nodes[Index].Address, Nodes[Index].UseExpressions.at(Use));
      }
    }
  }

public:
  SourceFlow(const HighFunc &Function, HighSourceFlowReport &Diagnostics,
             std::set<const HighStmt *> *DeadCopies = nullptr)
      : Function(Function), Diagnostics(Diagnostics), DeadCopies(DeadCopies) {}

  void bounds(const std::vector<HighSourceUnsignedRangeQuery> &Queries,
              std::vector<std::optional<uint64_t>> &Result) {
    try {
      if (Queries.empty() || Queries.size() > 128 ||
          Function.StructuredExceptionRegions ||
          Function.UnstructuredExceptionRegions)
        return;
      const auto Entry = build();
      if (!Entry || !Diagnostics.Complete || !Diagnostics.Items.empty())
        return;
      for (size_t Q = 0; Q < Queries.size(); ++Q) {
        const auto &[Statement, Value] = Queries[Q];
        std::set<size_t> Dependencies;
        const auto Initial = indexRange(Value, Dependencies);
        if (!Statement || !Initial)
          continue;
        spend(Nodes.size());
        std::vector<std::optional<uint64_t>> Incoming(Nodes.size());
        std::vector<bool> Queued(Nodes.size());
        std::vector<size_t> Pending{*Entry};
        Incoming[*Entry] = *Initial;
        Queued[*Entry] = true;
        while (!Pending.empty()) {
          spend();
          const auto I = Pending.back();
          Pending.pop_back();
          Queued[I] = false;
          auto Upper = *Incoming[I];
          for (auto Written : Nodes[I].Writes) {
            spend();
            if (Dependencies.count(Written))
              Upper = *Initial;
          }
          for (size_t E = 0; E < Nodes[I].Next.size(); ++E) {
            spend();
            auto NextUpper = std::optional<uint64_t>(Upper);
            if (auto Truth = Nodes[I].EdgeTruth[E])
              NextUpper = constrainIndex(Nodes[I].Test, *Truth, Value, Upper);
            if (!NextUpper)
              continue;
            const auto Next = Nodes[I].Next[E];
            if (!Incoming[Next] || *Incoming[Next] < *NextUpper) {
              Incoming[Next] = *NextUpper;
              if (!Queued[Next]) {
                Queued[Next] = true;
                Pending.push_back(Next);
              }
            }
          }
        }
        for (size_t I = 0; I < Nodes.size(); ++I) {
          spend();
          if (Nodes[I].ExpressionStatement == Statement && Incoming[I]) {
            auto Upper = *Incoming[I];
            // A query describes an occurrence within a statement, not an
            // evaluation order among its expressions. A write in that same
            // statement can precede the queried read.
            for (auto Written : Nodes[I].Writes) {
              spend();
              if (Dependencies.count(Written))
                Upper = *Initial;
            }
            Result[Q] = Result[Q] ? std::max(*Result[Q], Upper) : Upper;
          }
        }
      }
    } catch (const Failure &) {
      // A partial set of proofs must not survive exhaustion or malformed IR.
      std::fill(Result.begin(), Result.end(), std::nullopt);
    }
  }

  void graph(HighSourceFlowGraph &Result) {
    try {
      const auto Entry = build();
      if (!Entry)
        return;
      Result.Entry = *Entry;
      Result.Nodes.reserve(Nodes.size());
      for (auto &N : Nodes)
        Result.Nodes.push_back(
            {N.Statement, N.Test, std::move(N.Next), std::move(N.EdgeTruth)});
    } catch (const Failure &Error) {
      Diagnostics.Complete = false;
      Diagnostics.add(Error.Issue, Error.Reason, Error.Address);
    }
  }

  void collect(bool NeedsReturn) {
    try {
      analyze(NeedsReturn);
    } catch (const Failure &Error) {
      Diagnostics.Complete = false;
      Diagnostics.add(Error.Issue, Error.Reason, Error.Address);
    }
  }

  void cleanup(const std::function<bool(const HighStmt &)> &CanErase,
               std::set<const HighStmt *> &Dead,
               std::vector<std::pair<MedVar, MedVar>> &Renames) {
    try {
      Erasable = &CanErase;
      Refine = false;
      EmitterLabels = true;
      site(Function.Body, nullptr, 1);
      if (const auto Entry = build())
        phiCleanup(*Entry, Dead, Renames);
    } catch (const Failure &Error) {
      Dead.clear();
      Renames.clear();
      Diagnostics.Complete = false;
      Diagnostics.add(Error.Issue, Error.Reason, Error.Address);
    }
  }
};

} // namespace
HighSourceFlowGraph buildHighSourceFlowGraph(const HighFunc &Function) {
  HighSourceFlowGraph Result;
  SourceFlow(Function, Result.Diagnostics).graph(Result);
  return Result;
}
std::vector<std::optional<uint64_t>> highSourceUnsignedUpperBounds(
    const HighFunc &Function,
    const std::vector<HighSourceUnsignedRangeQuery> &Queries) {
  std::vector<std::optional<uint64_t>> Result(Queries.size());
  HighSourceFlowReport Diagnostics;
  SourceFlow(Function, Diagnostics).bounds(Queries, Result);
  return Result;
}
HighSourceFlowReport analyzeHighSourceFlow(const HighFunc &Function,
                                           bool NeedsReturn) {
  HighSourceFlowReport Result;
  SourceFlow(Function, Result).collect(NeedsReturn);
  return Result;
}
HighSourcePhiCleanup
highSourcePhiCleanup(const HighFunc &Function,
                     const std::function<bool(const HighStmt &)> &Erasable) {
  HighSourcePhiCleanup Result;
  HighSourceFlowReport Diagnostics;
  SourceFlow(Function, Diagnostics)
      .cleanup(Erasable, Result.DeadCopies, Result.Renames);
  return Result;
}
bool eliminateHighDeadPhiCopies(HighFunc &Function) {
  std::vector<HighStmt *> Statements;
  std::vector<std::pair<HighStmt *, unsigned>> Pending;
  for (auto &S : Function.Body)
    Pending.emplace_back(&S, 1);
  bool HasPhiCopy = false;
  while (!Pending.empty()) {
    const auto [S, Depth] = Pending.back();
    Pending.pop_back();
    if (Depth > 200 || Statements.size() == 100000)
      return false;
    Statements.push_back(S);
    HasPhiCopy |= S->Kind == StmtKind::Assign && S->IsPhiCopy;
    auto Append = [&](auto &Body) {
      for (auto &Child : Body)
        Pending.emplace_back(&Child, Depth + 1);
    };
    Append(S->Body);
    Append(S->ElseBody);
    Append(S->DefaultBody);
    for (auto &Case : S->Cases)
      Append(Case.Body);
  }
  if (!HasPhiCopy)
    return false;
  HighSourceFlowReport Report;
  std::set<const HighStmt *> Dead;
  SourceFlow(Function, Report, &Dead).collect(false);
  if (!Report.Complete || Dead.empty())
    return false;
  for (auto *S : Statements)
    if (Dead.count(S)) {
      const va_t Address = S->Addr;
      *S = HighStmt{};
      S->Kind = StmtKind::Nop;
      S->Addr = Address; // A goto may still target this source label.
    }
  return true;
}
} // namespace neverd
