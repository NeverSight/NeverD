//===- AArch64ImmediateMemoryBase.h - Source base admission -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Distinguish runtime GPR values from image coordinates before body filtering.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_AARCH64IMMEDIATEMEMORYBASE_H
#define NEVERD_LIB_BACKEND_AARCH64IMMEDIATEMEMORYBASE_H

#include "neverd/ir/high/AArch64PAuthLoad.h"
#include "neverd/ir/high/AArch64WritebackMemory.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLoad.h"
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/MedPrologueSaves.h"
#include "neverd/loader/AArch64MemoryImage.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <type_traits>
#include <vector>

namespace neverd {
namespace aarch64_memory_base_detail {
enum class Step { Scalar, Runtime, Derived, Unsupported };

/// Every dependency must remain in runtime/scalar coordinates. A cycle alone
/// supplies no entry value. Image-derived joins and arithmetic need a separate
/// all-arms span owner; they cannot borrow a runtime parameter's permission.
template <typename Value, typename Key, typename Expand>
bool runtimeValue(Value Root, Key key, Expand expand) {
  std::vector<Value> Work{Root};
  std::set<std::decay_t<decltype(key(Root))>> Seen;
  bool HasRuntime = false;
  while (!Work.empty()) {
    const auto Current = Work.back();
    Work.pop_back();
    if (!Seen.insert(key(Current)).second)
      continue;
    switch (expand(Current, Work)) {
    case Step::Runtime:
      HasRuntime = true;
      break;
    case Step::Unsupported:
      return false;
    case Step::Scalar:
    case Step::Derived:
      break;
    }
  }
  return HasRuntime;
}
/// Raw SP has only parameter roots, complete copies/joins and the completed
/// base of original SP memory operations. Every dependency must reach a root;
/// an unrelated uninitialized cycle cannot borrow another arm's parameter.
template <typename Value, typename Key, typename Expand>
bool rawSPValue(Value Root, Key KeyOf, Expand ExpandValue) {
  using Identity = std::decay_t<decltype(KeyOf(Root))>;
  std::map<Identity, std::vector<Identity>> Users;
  std::set<Identity> Seen, Rooted;
  std::vector<Value> Work{Root};
  while (!Work.empty()) {
    const auto Current = Work.back();
    Work.pop_back();
    const auto K = KeyOf(Current);
    if (!Seen.insert(K).second)
      continue;
    std::vector<Value> Inputs;
    const auto State = ExpandValue(Current, Inputs);
    if (State == Step::Runtime)
      Rooted.insert(K);
    else if (State != Step::Derived || Inputs.empty())
      return false;
    for (const auto &Input : Inputs) {
      Users[KeyOf(Input)].push_back(K);
      Work.push_back(Input);
    }
  }
  std::vector<Identity> Proven(Rooted.begin(), Rooted.end());
  while (!Proven.empty()) {
    const auto K = Proven.back();
    Proven.pop_back();
    const auto It = Users.find(K);
    if (It != Users.end())
      for (const auto &User : It->second)
        if (Rooted.insert(User).second)
          Proven.push_back(User);
  }
  return Rooted.size() == Seen.size();
}
} // namespace aarch64_memory_base_detail

class AArch64ImmediateMedBases {
  using Key = std::tuple<int, int, int, uint16_t>;
  const MedFunc &Function;
  std::map<Key, const MedOp *> Definitions;
  std::map<Key, const PhiNode *> Phis;
  std::set<Key> Parameters;
  bool SkippedSSA;
  static Key key(const MedVar &Value) {
    return {Value.Kind, Value.Id, Value.SSAVer, Value.Size};
  }

public:
  explicit AArch64ImmediateMedBases(const MedFunc &Function)
      : Function(Function), SkippedSSA(Function.SkippedSSA) {
    for (const auto &Parameter : Function.Params)
      Parameters.insert(key(Parameter));
    for (const auto &Block : Function.Blocks) {
      for (const auto &Op : Block.Ops) {
        const auto Define = [&](const MedVar &Value) {
          if (Value.Id < 0 || !Value.Size)
            return;
          const auto [It, Inserted] = Definitions.emplace(key(Value), &Op);
          if (!Inserted)
            It->second = nullptr;
        };
        Define(Op.Output);
        if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
            Op.Inputs[0].isConst() &&
            isAArch64WritebackAuxiliaryLoad(
                static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) &&
            aarch64WritebackMemoryMedShapeIsValid(Op))
          for (const auto &Output : Op.IntrinsicOutputs)
            Define(Output);
      }
      for (const auto &Phi : Block.Phis) {
        const auto [It, Inserted] = Phis.emplace(key(Phi.Output), &Phi);
        if (!Inserted)
          It->second = nullptr;
      }
    }
  }

  bool supportsRawSP(const MedVar &Base) const {
    using Step = aarch64_memory_base_detail::Step;
    using Value = std::pair<MedVar, bool>;
    return aarch64_memory_base_detail::rawSPValue(
        Value{Base, false},
        [](const Value &V) { return std::pair(key(V.first), V.second); },
        [&](const Value &V, std::vector<Value> &Work) {
          const auto &[Current, Packed] = V;
          if (Current.isConst() ||
              Current.Size !=
                  (Packed ? kWritebackPairBytes : kAddressMemoryPointerBytes))
            return Step::Unsupported;
          const auto Add = [&](const MedVar &Input, bool Pair = false) {
            Work.emplace_back(Input, Pair);
          };
          if (const auto It = Phis.find(key(Current)); It != Phis.end()) {
            if (SkippedSSA || !It->second || Definitions.count(key(Current)))
              return Step::Unsupported;
            for (const auto &Incoming : It->second->Args)
              Add(Incoming.second, Packed);
            if (It->second->ExceptionalEntry)
              Add(*It->second->ExceptionalEntry, Packed);
            return Step::Derived;
          }
          const auto It = Definitions.find(key(Current));
          if (It == Definitions.end())
            return !Packed && (Current.Kind == MedVar::Param ||
                               Parameters.count(key(Current)))
                       ? Step::Runtime
                       : Step::Unsupported;
          const auto *Def = It->second;
          if (SkippedSSA || !Def)
            return Step::Unsupported;
          if (Def->Opcode == NdOp::COPY && Def->NumInputs == 1 &&
              Def->Inputs[0].Size == Current.Size) {
            Add(Def->Inputs[0], Packed);
            return Step::Derived;
          }
          if (Def->Opcode == NdOp::SELECT && Def->NumInputs == 3) {
            Add(Def->Inputs[1], Packed);
            Add(Def->Inputs[2], Packed);
            return Step::Derived;
          }
          if (!Packed && Def->Opcode == NdOp::SUBBYTES && Def->NumInputs == 2 &&
              Def->Inputs[1].isConst() &&
              Def->Inputs[1].ConstVal == kAddressMemoryPointerBytes &&
              Def->Inputs[0].Size == kWritebackPairBytes) {
            Add(Def->Inputs[0], true);
            return Step::Derived;
          }
          if (aarch64WritebackMemoryMedShapeIsValid(*Def)) {
            const auto Id = static_cast<Intrinsic>(Def->Inputs[0].ConstVal);
            const auto I = *aarch64WritebackMemoryInstruction(
                Def->Inputs[kWritebackWordOperand + 1].ConstVal);
            const bool Completed =
                Packed
                    ? Id == Intrinsic::A64_LoadWriteback
                    : (isAArch64WritebackStoreIntrinsic(Id) ||
                       (isAArch64WritebackAuxiliaryLoad(Id) &&
                        key(Current) ==
                            key(Def->IntrinsicOutputs[aarch64WritebackBaseIndex(
                                Id)])));
            if (I.stackBase() && Completed) {
              Add(Def->Inputs[kWritebackBaseOperand + 1]);
              return Step::Derived;
            }
          }
          if (Packed && aarch64PAuthLoadMedShapeIsValid(*Def) &&
              aarch64PAuthLoadInstruction(Def->Inputs[2].ConstVal)
                  ->stackBase()) {
            Add(Def->Inputs[1]);
            return Step::Derived;
          }
          return Step::Unsupported;
        });
  }

  template <typename InstructionT>
  bool supports(const BinaryImage &Image, const InstructionT &Instruction,
                const MedVar &Base) const {
    MedVar Constant = Base;
    std::set<Key> Copies;
    while (!SkippedSSA && !Constant.isConst() &&
           Copies.insert(key(Constant)).second) {
      const auto It = Definitions.find(key(Constant));
      const auto *Def = It == Definitions.end() ? nullptr : It->second;
      if (!Def || Def->Opcode != NdOp::COPY || Def->NumInputs != 1 ||
          Def->Inputs[0].Size != Constant.Size)
        break;
      Constant = Def->Inputs[0];
    }
    if (Constant.isConst())
      return bool(aarch64ImmediateMemoryImageSpan(
          Image, Instruction, Constant.ConstVal, Constant.Provenance,
          Constant.AddressOwnerVA));
    using Step = aarch64_memory_base_detail::Step;
    return aarch64_memory_base_detail::runtimeValue(
        Base,
        [](const MedVar &Value) {
          return std::tuple(Value.Kind, Value.Id, Value.SSAVer, Value.Size,
                            Value.ConstVal, Value.Provenance,
                            Value.AddressOwnerVA);
        },
        [&](const MedVar &Value, std::vector<MedVar> &Work) {
          if (Value.isConst())
            return Value.Provenance == ConstantAddressProvenance::Scalar
                       ? Step::Scalar
                       : Step::Unsupported;
          const bool Parameter =
              Value.Kind == MedVar::Param || Parameters.count(key(Value)) ||
              (Value.Kind == MedVar::Reg &&
               std::any_of(Function.Params.begin(), Function.Params.end(),
                           [&](const MedVar &Parameter) {
                             return Parameter.RegOff == Value.RegOff &&
                                    isEntryLiveInValue(Function, Value,
                                                       Parameter.RegOff);
                           }));
          if (const auto It = Phis.find(key(Value)); It != Phis.end()) {
            if (!It->second)
              return Step::Unsupported;
            for (const auto &Incoming : It->second->Args)
              Work.push_back(Incoming.second);
            if (It->second->ExceptionalEntry)
              Work.push_back(*It->second->ExceptionalEntry);
            return Parameter ? Step::Runtime : Step::Derived;
          }
          const auto It = Definitions.find(key(Value));
          if (It == Definitions.end())
            return Parameter ? Step::Runtime : Step::Unsupported;
          const auto *Def = It->second;
          if (!Def || Def->NumInputs > Def->Inputs.size())
            return Step::Unsupported;
          if (Def->Opcode == NdOp::LOAD || Def->Opcode == NdOp::CALL)
            return Step::Runtime;
          if (Def->Opcode == NdOp::INTRINSIC && Def->NumInputs &&
              Def->Inputs[0].isConst()) {
            const auto Id = static_cast<Intrinsic>(Def->Inputs[0].ConstVal);
            if ((isAArch64AddressMemoryIntrinsic(Id) &&
                 !isAArch64AddressStoreIntrinsic(Id)) ||
                isAArch64PAuthLoadIntrinsic(Id) ||
                isAArch64WritebackMemoryIntrinsic(Id) || Def->NumInputs == 1)
              return Step::Runtime;
          }
          const unsigned First =
              Def->Opcode == NdOp::SELECT || Def->Opcode == NdOp::INTRINSIC ? 1
                                                                            : 0;
          for (unsigned I = First; I < Def->NumInputs; ++I)
            Work.push_back(Def->Inputs[I]);
          return Parameter || Def->Opcode == NdOp::INTRINSIC ? Step::Runtime
                                                             : Step::Derived;
        });
  }
};

class AArch64ImmediateHighBases {
  const HighFunc &Function;
  using Definition = std::pair<const HighStmt *, const HighExpr *>;
  VarKeyMap<std::vector<Definition>> Definitions;
  bool HasAddressTaken = false;
  mutable std::optional<HighSourceFlowGraph> Flow;

  bool dominates(const HighStmt &Definition, const HighStmt &Use) const {
    if (&Definition == &Use)
      return false;
    if (!Flow)
      Flow = buildHighSourceFlowGraph(Function);
    if (!Flow->Diagnostics.Complete || !Flow->Diagnostics.Items.empty())
      return false;
    std::optional<size_t> Defined, Used;
    for (size_t I = 0; I < Flow->Nodes.size(); ++I) {
      if (Flow->Nodes[I].Statement == &Definition)
        Defined = I;
      if (Flow->Nodes[I].Statement == &Use)
        Used = I;
    }
    if (!Defined || !Used)
      return false;
    std::vector<std::pair<size_t, bool>> Work{{Flow->Entry, false}};
    std::set<std::pair<size_t, bool>> Seen;
    bool ReachesUse = false;
    while (!Work.empty()) {
      auto [At, DefinedOnPath] = Work.back();
      Work.pop_back();
      DefinedOnPath |= At == *Defined;
      if (!Seen.insert({At, DefinedOnPath}).second)
        continue;
      if (At == *Used) {
        if (!DefinedOnPath)
          return false;
        ReachesUse = true;
      }
      for (const auto Next : Flow->Nodes[At].Successors)
        Work.push_back({Next, DefinedOnPath});
    }
    return ReachesUse;
  }

  /// Source PHIs become mutable copies. Follow all reaching definitions at
  /// this use, stopping each backward path at its most recent assignment.
  bool reachingDefinitions(const MedVar &Variable, const HighStmt *Use,
                           std::vector<Definition> &Reaching,
                           bool &EntryValue) const {
    if (!Use)
      return false;
    if (!Flow)
      Flow = buildHighSourceFlowGraph(Function);
    if (!Flow->Diagnostics.Complete || !Flow->Diagnostics.Items.empty())
      return false;
    std::vector<std::vector<size_t>> Predecessors(Flow->Nodes.size());
    std::set<size_t> Reachable;
    std::vector<size_t> Work{Flow->Entry};
    while (!Work.empty()) {
      const auto At = Work.back();
      Work.pop_back();
      if (!Reachable.insert(At).second)
        continue;
      for (const auto Next : Flow->Nodes[At].Successors) {
        Predecessors[Next].push_back(At);
        Work.push_back(Next);
      }
    }
    std::optional<size_t> Used;
    for (const auto At : Reachable)
      if (Flow->Nodes[At].Statement == Use) {
        if (Used)
          return false;
        Used = At;
      }
    if (!Used)
      return false;
    EntryValue = *Used == Flow->Entry;
    Work = Predecessors[*Used];
    const auto It = Definitions.find(varKey(Variable));
    std::set<size_t> Seen;
    while (!Work.empty()) {
      const auto At = Work.back();
      Work.pop_back();
      if (!Seen.insert(At).second)
        continue;
      bool Defined = false;
      if (It != Definitions.end())
        for (const auto &Definition : It->second)
          if (Definition.first == Flow->Nodes[At].Statement) {
            if (Defined)
              return false;
            Defined = true;
            Reaching.push_back(Definition);
          }
      if (Defined)
        continue;
      EntryValue |= At == Flow->Entry;
      Work.insert(Work.end(), Predecessors[At].begin(), Predecessors[At].end());
    }
    return true;
  }

public:
  explicit AArch64ImmediateHighBases(const HighFunc &Function)
      : Function(Function) {
    std::vector<const HighExpr *> Work;
    walkStmts(Function.Body, [&](const HighStmt &Statement) {
      if (Statement.Kind == StmtKind::Assign && Statement.Dst &&
          Statement.Val &&
          (Statement.Dst->Kind == ExprKind::Var ||
           Statement.Dst->Kind == ExprKind::Phi))
        Definitions[varKey(Statement.Dst->Var)].push_back(
            {&Statement, Statement.Val.get()});
      const auto DefineOutputs = [&](const ExprPtr &Value) {
        if (Value && isAArch64WritebackAuxiliaryLoad(Value->IntrinsicId) &&
            aarch64WritebackMemoryHighShapeIsValid(*Value))
          for (const auto &Output : Value->IntrinsicOutputs)
            Definitions[varKey(Output)].push_back({&Statement, Value.get()});
      };
      DefineOutputs(Statement.Val);
      DefineOutputs(Statement.CallExpr);
      forEachExpr(Statement,
                  [&](const ExprPtr &Value) { Work.push_back(Value.get()); });
    });
    std::set<const HighExpr *> Seen;
    while (!Work.empty()) {
      const auto *Value = Work.back();
      Work.pop_back();
      if (!Value || !Seen.insert(Value).second)
        continue;
      HasAddressTaken |= Value->Kind == ExprKind::Addr;
      Value->forEachChildExpr(
          [&](const ExprPtr &Child) { Work.push_back(Child.get()); });
    }
  }

  /// Source variables are mutable. A single textual definition is not enough:
  /// the source-flow owner must prove it dominates this exact use, and an
  /// address-taken local must not acquire a constant-value certificate.
  const HighExpr *imageConstant(const HighExpr &Base,
                                const HighStmt *Use) const {
    const HighExpr *Value = &Base;
    std::set<const HighExpr *> Seen;
    while (Seen.insert(Value).second) {
      if (Value->Kind == ExprKind::Const)
        return Value;
      if ((Value->Kind == ExprKind::Cast || Value->Kind == ExprKind::BitCast) &&
          Value->Type && Value->Type->Size == kAddressMemoryPointerBytes &&
          Value->Operands.size() == 1 && Value->Operands.front() &&
          Value->Operands.front()->Type &&
          Value->Operands.front()->Type->Size == kAddressMemoryPointerBytes &&
          (Value->Type->Kind == NdTypeKind::Int ||
           Value->Type->Kind == NdTypeKind::Ptr) &&
          (Value->Operands.front()->Type->Kind == NdTypeKind::Int ||
           Value->Operands.front()->Type->Kind == NdTypeKind::Ptr)) {
        Value = Value->Operands.front().get();
        continue;
      }
      if (HasAddressTaken || !Use ||
          (Value->Kind != ExprKind::Var && Value->Kind != ExprKind::Phi))
        return nullptr;
      const auto It = Definitions.find(varKey(Value->Var));
      if (It == Definitions.end() || It->second.size() != 1 ||
          !dominates(*It->second.front().first, *Use))
        return nullptr;
      Use = It->second.front().first;
      Value = It->second.front().second;
    }
    return nullptr;
  }

  bool supportsRawSP(const HighExpr &Base, const HighStmt *Use) const {
    using Step = aarch64_memory_base_detail::Step;
    using Value = std::tuple<const HighExpr *, const HighStmt *, bool>;
    return aarch64_memory_base_detail::rawSPValue(
        Value{&Base, Use, false}, [](const Value &V) { return V; },
        [&](const Value &V, std::vector<Value> &Work) {
          const auto &[Current, Statement, Packed] = V;
          if (!Current || !Current->Type ||
              Current->Type->Size !=
                  (Packed ? kWritebackPairBytes : kAddressMemoryPointerBytes))
            return Step::Unsupported;
          const auto Add = [&](const HighExpr *Input, const HighStmt *At,
                               bool Pair = false) {
            Work.emplace_back(Input, At, Pair);
          };
          if (Current->Kind == ExprKind::Var ||
              Current->Kind == ExprKind::Phi) {
            if (HasAddressTaken)
              return Step::Unsupported;
            const auto It = Definitions.find(varKey(Current->Var));
            if (It == Definitions.end())
              return !Packed && Current->Var.Kind == MedVar::Param
                         ? Step::Runtime
                         : Step::Unsupported;
            std::vector<Definition> Reaching;
            bool EntryValue = false;
            if (!reachingDefinitions(Current->Var, Statement, Reaching,
                                     EntryValue) ||
                (EntryValue && (Packed || Current->Var.Kind != MedVar::Param)))
              return Step::Unsupported;
            for (const auto &[DefinedAt, Definition] : Reaching) {
              if (aarch64WritebackMemoryHighShapeIsValid(*Definition) &&
                  isAArch64WritebackAuxiliaryLoad(Definition->IntrinsicId)) {
                const auto Id = Definition->IntrinsicId;
                const auto I = *aarch64WritebackMemoryInstruction(
                    Definition->Operands[kWritebackWordOperand]->ConstVal);
                if (Packed || !I.stackBase() ||
                    varKey(Current->Var) !=
                        varKey(Definition
                                   ->IntrinsicOutputs[aarch64WritebackBaseIndex(
                                       Id)]))
                  return Step::Unsupported;
                Add(Definition->Operands[kWritebackBaseOperand].get(),
                    DefinedAt);
              } else
                Add(Definition, DefinedAt, Packed);
            }
            return EntryValue ? Step::Runtime : Step::Derived;
          }
          if ((Current->Kind == ExprKind::Cast ||
               Current->Kind == ExprKind::BitCast) &&
              Current->Operands.size() == 1 && Current->Operands[0] &&
              Current->Operands[0]->Type &&
              Current->Operands[0]->Type->Size == Current->Type->Size &&
              (Current->Type->Kind == NdTypeKind::Int ||
               Current->Type->Kind == NdTypeKind::Ptr) &&
              (Current->Operands[0]->Type->Kind == NdTypeKind::Int ||
               Current->Operands[0]->Type->Kind == NdTypeKind::Ptr)) {
            Add(Current->Operands[0].get(), Statement, Packed);
            return Step::Derived;
          }
          if (Current->Kind == ExprKind::BinOp && Current->Op == NdOp::SELECT &&
              Current->Operands.size() == 3) {
            Add(Current->Operands[1].get(), Statement, Packed);
            Add(Current->Operands[2].get(), Statement, Packed);
            return Step::Derived;
          }
          if (!Packed && Current->Kind == ExprKind::BinOp &&
              Current->Op == NdOp::SUBBYTES && Current->Operands.size() == 2 &&
              Current->Operands[1] &&
              Current->Operands[1]->Kind == ExprKind::Const &&
              Current->Operands[1]->ConstVal == kAddressMemoryPointerBytes) {
            Add(Current->Operands[0].get(), Statement, true);
            return Step::Derived;
          }
          if (aarch64WritebackMemoryHighShapeIsValid(*Current)) {
            const auto Id = Current->IntrinsicId;
            const auto I = *aarch64WritebackMemoryInstruction(
                Current->Operands[kWritebackWordOperand]->ConstVal);
            if (I.stackBase() &&
                (Packed ? Id == Intrinsic::A64_LoadWriteback
                        : isAArch64WritebackStoreIntrinsic(Id))) {
              Add(Current->Operands[kWritebackBaseOperand].get(), Statement);
              return Step::Derived;
            }
          }
          if (Packed && aarch64PAuthLoadHighShapeIsValid(*Current) &&
              aarch64PAuthLoadInstruction(Current->Operands[1]->ConstVal)
                  ->stackBase()) {
            Add(Current->Operands[0].get(), Statement);
            return Step::Derived;
          }
          return Step::Unsupported;
        });
  }

  template <typename InstructionT>
  bool supports(const BinaryImage &Image, const InstructionT &Instruction,
                const HighExpr &Base, const HighStmt *Use) const {
    if (const auto *Constant = imageConstant(Base, Use))
      return bool(aarch64ImmediateMemoryImageSpan(
          Image, Instruction, Constant->ConstVal, Constant->ConstProvenance,
          Constant->AddressOwnerVA));
    using Step = aarch64_memory_base_detail::Step;
    using Occurrence = std::pair<const HighExpr *, const HighStmt *>;
    return aarch64_memory_base_detail::runtimeValue(
        Occurrence{&Base, Use}, [](const Occurrence &Value) { return Value; },
        [&](const Occurrence &Current, std::vector<Occurrence> &Work) {
          const auto *Value = Current.first;
          const auto Add = [&](const HighExpr *Child) {
            Work.emplace_back(Child, Current.second);
          };
          if (!Value)
            return Step::Unsupported;
          if (Value->Kind == ExprKind::Const)
            return Value->ConstProvenance == ConstantAddressProvenance::Scalar
                       ? Step::Scalar
                       : Step::Unsupported;
          if (Value->Kind == ExprKind::Load ||
              Value->Kind == ExprKind::EntryRegister ||
              (Value->Kind == ExprKind::Call &&
               (Value->IntrinsicId == Intrinsic::None ||
                (isAArch64AddressMemoryIntrinsic(Value->IntrinsicId) &&
                 !isAArch64AddressStoreIntrinsic(Value->IntrinsicId)) ||
                isAArch64PAuthLoadIntrinsic(Value->IntrinsicId) ||
                isAArch64WritebackMemoryIntrinsic(Value->IntrinsicId) ||
                Value->Operands.empty())))
            return Step::Runtime;
          if (Value->Kind == ExprKind::Var || Value->Kind == ExprKind::Phi) {
            const auto It = Definitions.find(varKey(Value->Var));
            if (It != Definitions.end())
              for (const auto &Definition : It->second) {
                if (isAArch64WritebackAuxiliaryLoad(
                        Definition.second->IntrinsicId) &&
                    (!Current.second ||
                     !dominates(*Definition.first, *Current.second)))
                  return Step::Unsupported;
                Work.emplace_back(Definition.second, Definition.first);
              }
            const bool Parameter =
                Value->Var.Kind == MedVar::Param ||
                (Value->Var.Kind == MedVar::Reg && Value->Var.SSAVer == 0 &&
                 std::any_of(Function.Params.begin(), Function.Params.end(),
                             [&](const HighParam &Parameter) {
                               return Parameter.MedIndex == Value->Var.Id &&
                                      Parameter.RegOff != kNoParamReg &&
                                      Parameter.RegOff == Value->Var.RegOff;
                             }));
            if (Parameter)
              return Step::Runtime;
            return It == Definitions.end() ? Step::Unsupported : Step::Derived;
          }
          if (Value->Kind == ExprKind::Undef || Value->Kind == ExprKind::Store)
            return Step::Unsupported;
          if (Value->Kind == ExprKind::Addr && Value->Operands.size() == 1 &&
              Value->Operands.front() &&
              Value->Operands.front()->Kind == ExprKind::Load) {
            if (Value->Operands.front()->Operands.empty() ||
                !Value->Operands.front()->Operands.front())
              return Step::Unsupported;
            Add(Value->Operands.front()->Operands.front().get());
            return Step::Derived;
          }
          const unsigned First = Value->Op == NdOp::SELECT ? 1 : 0;
          for (unsigned I = First; I < Value->Operands.size(); ++I) {
            if (!Value->Operands[I])
              return Step::Unsupported;
            Add(Value->Operands[I].get());
          }
          return Value->Kind == ExprKind::Call ? Step::Runtime : Step::Derived;
        });
  }
};

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_AARCH64IMMEDIATEMEMORYBASE_H
