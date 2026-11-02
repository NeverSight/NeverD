//===- HighPointerTypeInference.cpp - Source pointer types ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Infer the record pointer types parameters transport without assigning one
/// type to a physical frame slot shared by several value lifetimes.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/HighPointerTypeInference.h"

#include "HighFrameAddress.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighSourceFlow.h"

#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {
namespace {
using Identity = HighSourceLocalIdentity;

bool sameType(const TypeRef &Left, const TypeRef &Right) {
  return Left == Right || (Left && Right && equalSourceTypes(Left, Right));
}

/// A pointer to a record (a structure or union), the only source type a
/// parameter gives its copies. Other pointers, and the types of calls,
/// constants, addresses and casts, keep their machine carriers.
bool recordPointer(const TypeRef &Type) {
  return Type && Type->Kind == NdTypeKind::Ptr && Type->Pointee &&
         Type->Pointee->Kind == NdTypeKind::Struct && !Type->Pointee->IsEnum;
}

struct TypeState {
  std::map<Identity, TypeRef> Pointers;
  std::map<Identity, int64_t> Addresses;
  std::map<int64_t, TypeRef> Memory;

  size_t size() const {
    return Pointers.size() + Addresses.size() + Memory.size();
  }
};

struct NodeEffects {
  std::vector<ExprPtr> Expressions;
  std::vector<MedVar> Writes;
  bool ClobbersMemory = false;
};

class PointerTypes {
  const HighFunc &Function;
  llvm::ArrayRef<TypeRef> ParameterTypes;
  Arch Architecture;
  uint16_t PointerBytes;
  size_t Budget = limits::ir::kHighPointerTypeMaxOperations;
  bool Complete = true;
  ExprPtr StackPointer;
  std::map<int64_t, ExprPtr> OffsetExpressions;
  std::set<Identity> AddressTaken;
  std::map<Identity, TypeRef> Candidates;
  std::set<Identity> Rejected;

  bool spend(size_t Count = 1) {
    if (Count > Budget) {
      Complete = false;
      Budget = 0;
      return false;
    }
    Budget -= Count;
    return true;
  }

  bool scalar(const MedVar &Variable) const {
    return Variable.Kind != MedVar::Stack &&
           !isSyntheticEntryStackPointer(Variable, Function, Architecture) &&
           Variable.Size == PointerBytes &&
           !AddressTaken.count(highSourceLocalIdentity(Variable));
  }

  NodeEffects effects(const HighSourceFlowNode &Node) {
    NodeEffects Result;
    std::vector<ExprPtr> Pending;
    if (Node.Test)
      Pending.push_back(Node.Test);
    else if (Node.Statement) {
      forEachRhsExpr(*Node.Statement,
                     [&](const ExprPtr &Root) { Pending.push_back(Root); });
      if (Node.Statement->Dst && Node.Statement->Dst->Kind != ExprKind::Var &&
          Node.Statement->Dst->Kind != ExprKind::Phi)
        Pending.push_back(Node.Statement->Dst);
    }
    HighExprSet Seen;
    while (!Pending.empty() && spend()) {
      ExprPtr Expression = Pending.back();
      Pending.pop_back();
      if (!Expression || !Seen.insert(Expression.get()).second)
        continue;
      Result.Expressions.push_back(Expression);
      if (Expression->Kind == ExprKind::Call ||
          Expression->Kind == ExprKind::Store ||
          Expression->IntrinsicId != Intrinsic::None ||
          Expression->MemoryOrdering != NdMemoryOrdering::None ||
          Expression->MemoryAddressSpace != NdMemoryAddressSpace::Default)
        Result.ClobbersMemory = true;
      for (const MedVar &Output : Expression->IntrinsicOutputs)
        Result.Writes.push_back(Output);
      if (Expression->Kind == ExprKind::Addr)
        for (const ExprPtr &Operand : Expression->Operands)
          if (Operand && (Operand->Kind == ExprKind::Var ||
                          Operand->Kind == ExprKind::Phi))
            AddressTaken.insert(highSourceLocalIdentity(Operand->Var));
      if (Expression->Kind == ExprKind::Var &&
          isSyntheticEntryStackPointer(Expression->Var, Function, Architecture))
        StackPointer = Expression;
      Expression->forEachChildExpr(
          [&](const ExprPtr &Child) { Pending.push_back(Child); });
    }
    return Result;
  }

  std::optional<int64_t> address(const ExprPtr &Expression,
                                 const TypeState &State) {
    const auto Alias = [&](const MedVar &Variable) -> ExprPtr {
      auto It = State.Addresses.find(highSourceLocalIdentity(Variable));
      if (It == State.Addresses.end() || !StackPointer)
        return nullptr;
      auto [Cached, Fresh] = OffsetExpressions.try_emplace(It->second);
      if (Fresh) {
        const uint64_t Bits = PointerBytes == 4 ? uint64_t(uint32_t(It->second))
                                                : uint64_t(It->second);
        Cached->second =
            HighExpr::makeBinop(NdOp::INT_ADD, StackPointer,
                                HighExpr::makeConst(Bits, PointerBytes));
      }
      return Cached->second;
    };
    auto Result = high_detail::frameAddressOffset(
        Expression, Function, Architecture, Budget, 0, Alias);
    if (!Budget)
      Complete = false;
    return Result;
  }

  std::optional<int64_t> slot(const ExprPtr &Expression, uint16_t Bytes,
                              const TypeState &State) {
    if (!Bytes || Function.FrameSize <= 0)
      return std::nullopt;
    auto Offset = address(Expression, State);
    if (!Offset || *Offset >= 0 || *Offset < -Function.FrameSize ||
        uint64_t(-*Offset) < Bytes)
      return std::nullopt;
    return Offset;
  }

  TypeRef pointer(const ExprPtr &Expression, const TypeState &State,
                  unsigned Depth = 0) {
    if (!spend() || !Expression || !Expression->Type ||
        Expression->Type->Size != PointerBytes ||
        (Expression->Type->Kind != NdTypeKind::Int &&
         Expression->Type->Kind != NdTypeKind::Ptr))
      return nullptr;
    if (Depth > limits::ir::kHighPointerTypeMaxExpressionDepth) {
      Complete = false;
      return nullptr;
    }
    const auto &E = *Expression;
    if (E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) {
      auto It = State.Pointers.find(highSourceLocalIdentity(E.Var));
      return It == State.Pointers.end() ? nullptr : It->second;
    }
    if (E.Kind == ExprKind::Load) {
      if (E.Operands.size() != 1 ||
          E.MemoryOrdering != NdMemoryOrdering::None ||
          E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return nullptr;
      auto Offset = slot(E.Operands[0], PointerBytes, State);
      if (!Offset)
        return nullptr;
      auto It = State.Memory.find(*Offset);
      return It == State.Memory.end() ? nullptr : It->second;
    }
    // A conversion to another pointer type changes the source type: only
    // a view of the same bits as the type they hold continues it.
    if ((E.Kind == ExprKind::Cast || E.Kind == ExprKind::BitCast) &&
        E.Operands.size() == 1 && E.Operands[0] && E.Operands[0]->Type &&
        E.Operands[0]->Type->Size == PointerBytes &&
        (E.Type->Kind == NdTypeKind::Int || E.Type->Kind == NdTypeKind::Ptr) &&
        (E.Operands[0]->Type->Kind == NdTypeKind::Int ||
         E.Operands[0]->Type->Kind == NdTypeKind::Ptr)) {
      const TypeRef Held = pointer(E.Operands[0], State, Depth + 1);
      return E.Type->Kind != NdTypeKind::Ptr || sameType(Held, E.Type)
                 ? Held
                 : nullptr;
    }
    // Arithmetic on integer pointer bits is byte arithmetic. A nonzero offset
    // cannot retain the same record pointee, nor become scaled C arithmetic.
    if (E.Kind == ExprKind::BinOp && E.Operands.size() == 2 &&
        (E.Op == NdOp::INT_ADD || E.Op == NdOp::INT_SUB) && E.Operands[1] &&
        E.Operands[1]->Kind == ExprKind::Const && !E.Operands[1]->ConstVal)
      return pointer(E.Operands[0], State, Depth + 1);
    return nullptr;
  }

  void forget(TypeState &State, const MedVar &Variable) {
    const auto Key = highSourceLocalIdentity(Variable);
    State.Pointers.erase(Key);
    State.Addresses.erase(Key);
  }

  void write(TypeState &State, const ExprPtr &Address, const ExprPtr &Value,
             NdMemoryOrdering Ordering, NdMemoryAddressSpace Space) {
    const uint16_t Bytes = Value && Value->Type ? Value->Type->Size : 0;
    const auto Offset = address(Address, State);
    int64_t End = 0;
    if (!Bytes || !Offset || llvm::AddOverflow(*Offset, int64_t(Bytes), End) ||
        Ordering != NdMemoryOrdering::None ||
        Space != NdMemoryAddressSpace::Default) {
      State.Memory.clear();
      return;
    }
    // A known write to an incoming home or saved-register slot above the
    // private frame cannot overwrite disjoint private bytes. It supplies no
    // initialized private value, but is not an unknown aliasing write either.
    const bool Private = Function.FrameSize > 0 &&
                         *Offset >= -Function.FrameSize && *Offset < 0 &&
                         End <= 0;
    TypeRef Stored =
        Private && Bytes == PointerBytes ? pointer(Value, State) : nullptr;
    for (auto It = State.Memory.begin(); It != State.Memory.end();) {
      if (It->first < End && *Offset < It->first + PointerBytes)
        It = State.Memory.erase(It);
      else
        ++It;
    }
    if (Stored)
      State.Memory[*Offset] = Stored;
  }

  void observe(const MedVar &Variable, const TypeRef &Type) {
    if (Variable.Kind == MedVar::Param)
      return;
    const auto Key = highSourceLocalIdentity(Variable);
    if (!scalar(Variable) || !Type || Type->Kind != NdTypeKind::Ptr ||
        Type->Size != Variable.Size) {
      Rejected.insert(Key);
      return;
    }
    auto [It, Fresh] = Candidates.try_emplace(Key, Type);
    if (!Fresh && !sameType(It->second, Type))
      Rejected.insert(Key);
  }

  TypeState transfer(const HighSourceFlowNode &Node, const NodeEffects &Effects,
                     TypeState State, bool Observe = false) {
    if (!spend(State.size() + Effects.Expressions.size() + 1))
      return {};
    if (Effects.ClobbersMemory)
      State.Memory.clear();
    for (const MedVar &Output : Effects.Writes) {
      forget(State, Output);
      if (Observe)
        observe(Output, nullptr);
    }
    if (Observe)
      for (const ExprPtr &Expression : Effects.Expressions)
        if (Expression->Kind == ExprKind::Var ||
            Expression->Kind == ExprKind::Phi)
          observe(Expression->Var, pointer(Expression, State));
    if (!Node.Statement)
      return State;
    const HighStmt &S = *Node.Statement;
    if (S.Kind == StmtKind::Store)
      write(State, S.StoreAddr, S.StoreVal, S.MemoryOrdering,
            S.MemoryAddressSpace);
    if (S.Kind != StmtKind::Assign || !S.Dst)
      return State;
    if (S.Dst->Kind == ExprKind::Load) {
      if (S.Dst->Operands.size() == 1)
        write(State, S.Dst->Operands[0], S.Val, S.Dst->MemoryOrdering,
              S.Dst->MemoryAddressSpace);
      else
        State.Memory.clear();
      return State;
    }
    if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi) {
      State.Memory.clear();
      return State;
    }
    const TypeRef Type = S.Dst->Type && (S.Dst->Type->Kind == NdTypeKind::Int ||
                                         S.Dst->Type->Kind == NdTypeKind::Ptr)
                             ? pointer(S.Val, State)
                             : nullptr;
    const auto Offset = address(S.Val, State);
    forget(State, S.Dst->Var);
    if (scalar(S.Dst->Var)) {
      const auto Key = highSourceLocalIdentity(S.Dst->Var);
      if (Type)
        State.Pointers[Key] = Type;
      if (Offset)
        State.Addresses[Key] = *Offset;
    }
    if (Observe)
      observe(S.Dst->Var, Type);
    return State;
  }

  /// The record pointer type a read of an incoming parameter carries, which
  /// alone seeds the entry state; null for any other expression.
  TypeRef parameterPointer(const HighExpr &Expression) const {
    if (Expression.Kind != ExprKind::Var ||
        Expression.Var.Kind != MedVar::Param || Expression.Var.SSAVer != 0 ||
        Expression.Var.RenameTag >= 0 || Expression.Var.Size != PointerBytes)
      return nullptr;
    // HighIR parameter IDs are logical positions. MedIndex retains the
    // native carrier's ID only for binding declarations by ABI location.
    const int Index = Expression.Var.Id;
    if (Index < 0 || size_t(Index) >= Function.Params.size())
      return nullptr;
    const TypeRef Type = ParameterTypes.empty() ? Function.Params[Index].Type
                                                : ParameterTypes[Index];
    return recordPointer(Type) && Type->Size == PointerBytes ? Type : nullptr;
  }

  /// Whether a statement reads such a parameter. The flow graph's nodes
  /// evaluate only expressions of these statements, so without one the entry
  /// state is empty. This spends no budget: it only spares the graph.
  bool readsRecordPointerParameter() const {
    bool Found = false;
    HighExprSet Seen;
    walkStmts(Function.Body, [&](const HighStmt &Statement) {
      forEachExpr(Statement, [&](const ExprPtr &Root) {
        std::vector<const HighExpr *> Pending{Root.get()};
        while (!Found && !Pending.empty()) {
          const HighExpr *Expression = Pending.back();
          Pending.pop_back();
          if (!Expression || !Seen.insert(Expression).second)
            continue;
          Found = parameterPointer(*Expression) != nullptr;
          Expression->forEachChildExpr(
              [&](const ExprPtr &Child) { Pending.push_back(Child.get()); });
        }
      });
    });
    return Found;
  }

  template <typename Map, typename Equal>
  bool intersect(Map &Left, const Map &Right, Equal Equals) {
    bool Changed = false;
    if (!spend(Left.size() + 1))
      return false;
    for (auto It = Left.begin(); It != Left.end();) {
      const auto Other = Right.find(It->first);
      if (Other == Right.end() || !Equals(It->second, Other->second)) {
        It = Left.erase(It);
        Changed = true;
      } else
        ++It;
    }
    return Changed;
  }

public:
  PointerTypes(const HighFunc &Function, Arch Architecture,
               llvm::ArrayRef<TypeRef> ParameterTypes)
      : Function(Function), ParameterTypes(ParameterTypes),
        Architecture(Architecture),
        PointerBytes(getTargetRegInfo(Architecture).PointerSize) {}

  HighPointerTypeFacts run() {
    if (!readsRecordPointerParameter())
      return {};
    const auto Graph = buildHighSourceFlowGraph(Function);
    if (!Graph.Diagnostics.Complete || !Graph.Diagnostics.Items.empty() ||
        Graph.Nodes.empty())
      return {};
    std::vector<NodeEffects> Effects;
    Effects.reserve(Graph.Nodes.size());
    for (const auto &Node : Graph.Nodes) {
      Effects.push_back(effects(Node));
      if (!Complete)
        return {};
    }
    TypeState Entry;
    for (const auto &Effect : Effects)
      for (const auto &Expression : Effect.Expressions)
        if (const TypeRef Type = parameterPointer(*Expression);
            Type && scalar(Expression->Var))
          Entry.Pointers[highSourceLocalIdentity(Expression->Var)] = Type;
    if (Entry.Pointers.empty())
      return {};
    std::vector<std::optional<TypeState>> Inputs(Graph.Nodes.size());
    Inputs[Graph.Entry] = Entry;
    size_t StateEntries = Entry.size();
    std::deque<size_t> Pending{Graph.Entry};
    std::vector<bool> Queued(Graph.Nodes.size());
    Queued[Graph.Entry] = true;
    while (!Pending.empty() && Complete) {
      const size_t Index = Pending.front();
      Pending.pop_front();
      Queued[Index] = false;
      auto Output =
          transfer(Graph.Nodes[Index], Effects[Index], *Inputs[Index]);
      for (size_t Successor : Graph.Nodes[Index].Successors) {
        if (!spend(Output.size() + 1))
          break;
        auto &Input = Inputs[Successor];
        bool Changed = !Input;
        if (!Input) {
          Input = Output;
          StateEntries += Input->size();
        } else {
          const size_t Before = Input->size();
          Changed |= intersect(Input->Pointers, Output.Pointers, sameType);
          Changed |= intersect(Input->Memory, Output.Memory, sameType);
          Changed |= intersect(Input->Addresses, Output.Addresses,
                               std::equal_to<int64_t>());
          StateEntries -= Before - Input->size();
        }
        if (StateEntries > limits::ir::kHighPointerTypeMaxStateEntries)
          Complete = false;
        if (Changed && !Queued[Successor]) {
          Pending.push_back(Successor);
          Queued[Successor] = true;
        }
      }
    }
    if (!Complete)
      return {};
    // Commit only after reaching a fixed point. In particular a loop back edge
    // must be included before a first-iteration pointer fact can type a local.
    for (size_t I = 0; I < Graph.Nodes.size() && Complete; ++I)
      transfer(Graph.Nodes[I], Effects[I], Inputs[I].value_or(TypeState()),
               true);
    if (!Complete)
      return {};
    for (const auto &Key : Rejected)
      Candidates.erase(Key);
    return Candidates;
  }
};
} // namespace

HighPointerTypeFacts
inferHighPointerTypes(const HighFunc &Function, Arch Architecture,
                      llvm::ArrayRef<TypeRef> ParameterTypes) {
  if (Architecture == Arch::Unknown ||
      (!ParameterTypes.empty() &&
       ParameterTypes.size() != Function.Params.size()))
    return {};
  bool HasRecordPointer = false;
  for (size_t I = 0; I < Function.Params.size(); ++I)
    HasRecordPointer |= recordPointer(
        ParameterTypes.empty() ? Function.Params[I].Type : ParameterTypes[I]);
  if (!HasRecordPointer)
    return {};
  return PointerTypes(Function, Architecture, ParameterTypes).run();
}
} // namespace neverd
