//===- RegistrationSEHStateStore.cpp - Checked callback states --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shares exact source-store binding and scope reachability across backends.

#include "neverd/Limits.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86/X86RegistrationSEHCall.h"
#include "neverd/ir/med/X86/X86RegistrationSEHStateStore.h"

#include "llvm/ADT/STLExtras.h"

#include <map>
#include <set>
#include <tuple>

namespace neverd {
namespace {

struct StateStoreSource {
  const RegistrationChainInfo *Chain = nullptr;
  std::map<int, const MedBlock *> Blocks;
  std::map<int, const RegistrationBlockState *> States;
  std::map<va_t, const RegistrationTryLevelStore *> Stores;
};

std::optional<StateStoreSource> collectSource(const MedFunc &Function) {
  if (Function.SkippedSSA || !Function.ExceptionMetadata ||
      !Function.ExceptionMetadata->Registration ||
      Function.ExceptionMetadata->Cxx ||
      Function.ExceptionMetadata->ParseStatus !=
          ExceptionParseStatus::Complete ||
      !Function.RegistrationStates)
    return std::nullopt;
  const auto &EH = *Function.ExceptionMetadata;
  const auto &States = *Function.RegistrationStates;
  const bool EH4 = EH.Encoding == ExceptionEncoding::X86ScopeTableEH4;
  if ((!EH4 && EH.Encoding != ExceptionEncoding::X86ScopeTableEH3) ||
      !States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete || !States.SEHStateStoresComplete)
    return std::nullopt;
  StateStoreSource Result;
  Result.Chain = &*EH.Registration;
  const auto &Chain = *Result.Chain;
  if (!Chain.TryLevelOffset || !Chain.SeededTryLevel ||
      *Chain.SeededTryLevel != (EH4 ? -2 : -1) ||
      Chain.Scopes.size() > limits::kMaxRegistrationEHRecords)
    return std::nullopt;
  size_t Work = 0;
  for (const auto &State : States.Blocks)
    if (++Work > limits::kMaxRegistrationEHStateWork || State.Unknown ||
        !Result.States.emplace(State.BlockId, &State).second)
      return std::nullopt;
  for (const auto &Block : Function.Blocks) {
    const auto State = Result.States.find(Block.Id);
    if (++Work > limits::kMaxRegistrationEHStateWork ||
        !Result.Blocks.emplace(Block.Id, &Block).second ||
        State == Result.States.end() || Block.StartAddr >= Block.EndAddr ||
        State->second->Range.Begin != Block.StartAddr ||
        State->second->Range.End != Block.EndAddr)
      return std::nullopt;
  }
  if (Result.Blocks.size() != Result.States.size())
    return std::nullopt;
  for (const auto &Store : Chain.TryLevelStores)
    if (++Work > limits::kMaxRegistrationEHStateWork ||
        Store.StoreVA >= Store.EndVA ||
        !Result.Stores.emplace(Store.StoreVA, &Store).second)
      return std::nullopt;
  return Result;
}

} // namespace

std::optional<std::vector<X86RegistrationSEHStateStoreBinding>>
getX86RegistrationSEHStateStores(const MedFunc &Function,
                                 bool IncludeParentStores) {
  const auto Source = collectSource(Function);
  if (!Source)
    return std::nullopt;
  const auto &Chain = *Source->Chain;
  auto ValidLevel = [&](int32_t Level) {
    return Level == *Chain.SeededTryLevel ||
           (Level >= 0 && uint32_t(Level) < Chain.Scopes.size());
  };
  using Key = std::tuple<int, va_t, int>;
  std::map<Key, const MedOp *> Operations;
  size_t Work = 0;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      if (Op.Opcode == NdOp::STORE && Op.OriginSeq >= 0 &&
          !Operations.emplace(Key{Block.Id, Op.Addr, Op.OriginSeq}, &Op).second)
        return std::nullopt;
    }
  std::map<va_t, X86RegistrationSEHStateStoreBinding> Bound;
  std::set<Key> Seen;
  for (const auto &Store : Function.RegistrationStates->SEHStateStores) {
    const auto State = Source->States.find(Store.BlockId);
    const auto Native = Source->Stores.find(Store.Address);
    const Key Identity{Store.BlockId, Store.Address, Store.OpSeq};
    if (++Work > limits::kMaxRegistrationEHStateWork ||
        State == Source->States.end() || Store.OpSeq < 0 ||
        !Seen.insert(Identity).second || Store.Address >= Store.EndAddress ||
        Store.Address < State->second->Range.Begin ||
        Store.EndAddress > State->second->Range.End)
      return std::nullopt;
    if (Native != Source->Stores.end()) {
      if (Store.EndAddress != Native->second->EndVA ||
          Store.Width != Native->second->Width ||
          Store.Value != uint32_t(Native->second->Level))
        return std::nullopt;
    } else if (State->second->CallbackOnly || Store.Width != 4 ||
               Store.EndAddress != State->second->Range.End)
      return std::nullopt;
    if (!State->second->CallbackOnly && !IncludeParentStores &&
        Native != Source->Stores.end())
      continue;
    const auto Operation = Operations.find(Identity);
    if (Operation == Operations.end())
      return std::nullopt;
    const auto &Op = *Operation->second;
    if ((Store.Width != 1 && Store.Width != 2 && Store.Width != 4) ||
        Store.Transitions.empty() || Op.Opcode != NdOp::STORE || Op.Dead ||
        Op.NumInputs != 2 || Op.Inputs.size() < 2 ||
        (Op.Inputs[0].Size != 4 && Op.Inputs[0].Size != 8) ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        Op.MemoryOrdering != NdMemoryOrdering::None ||
        (State->second->CallbackOnly && !Op.Inputs[1].isConst()) ||
        Op.Inputs[1].Size != Store.Width)
      return std::nullopt;
    const uint32_t Mask =
        Store.Width == 4 ? UINT32_MAX : (uint32_t(1) << (8 * Store.Width)) - 1;
    if (Store.Value > Mask || (Op.Inputs[1].isConst() &&
                               (Op.Inputs[1].ConstVal & Mask) != Store.Value))
      return std::nullopt;
    std::set<int32_t> TargetLevels;
    std::set<std::pair<int32_t, int32_t>> Transitions;
    for (const auto &Transition : Store.Transitions) {
      if (++Work > limits::kMaxRegistrationEHStateWork ||
          !ValidLevel(Transition.first) || !ValidLevel(Transition.second) ||
          uint32_t(Transition.second) !=
              ((uint32_t(Transition.first) & ~Mask) | Store.Value) ||
          !Transitions.insert(Transition).second)
        return std::nullopt;
      TargetLevels.insert(Transition.second);
    }
    if (!State->second->CallbackOnly && !IncludeParentStores)
      continue;
    if (!Bound
             .emplace(
                 Store.Address,
                 X86RegistrationSEHStateStoreBinding{
                     &Op, &Store, {TargetLevels.begin(), TargetLevels.end()}})
             .second)
      return std::nullopt;
  }
  // Loader observations inside a callback must each retain one checked store.
  // Parent stores remain the ordinary block-state protocol's responsibility.
  for (const auto &[Address, Store] : Source->Stores) {
    unsigned Owners = 0;
    for (const auto &[Id, State] : Source->States) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      if (!State->CallbackOnly || Address < State->Range.Begin ||
          Address >= State->Range.End)
        continue;
      if (++Owners != 1 || Store->EndVA > State->Range.End ||
          !Bound.count(Address) || Bound.at(Address).Source->BlockId != Id)
        return std::nullopt;
    }
  }
  std::vector<X86RegistrationSEHStateStoreBinding> Result;
  uint64_t NextWriteID = 1;
  for (auto &[Address, Binding] : Bound) {
    Binding.FirstWriteID = NextWriteID;
    NextWriteID += Binding.TargetLevels.size();
    Result.push_back(Binding);
  }
  return Result;
}

std::optional<std::vector<uint32_t>>
getX86RegistrationSEHRetainedScopes(const MedFunc &Function) {
  const auto Stores = getX86RegistrationSEHStateStores(Function);
  if (!Stores)
    return std::nullopt;
  const auto &Chain = *Function.ExceptionMetadata->Registration;
  std::set<uint32_t> Parent;
  std::set<uint32_t> Callback;
  size_t Work = 0;
  auto Include = [&](std::set<uint32_t> &Set, int32_t Level) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return false;
    while (Level != *Chain.SeededTryLevel) {
      if (++Work > limits::kMaxRegistrationEHStateWork || Level < 0 ||
          uint32_t(Level) >= Chain.Scopes.size())
        return false;
      const int32_t Outer = Chain.Scopes[Level].EnclosingLevel;
      if (Outer != *Chain.SeededTryLevel && (Outer < 0 || Outer >= Level))
        return false;
      if (!Set.insert(Level).second)
        return true;
      Level = Outer;
    }
    return true;
  };
  for (const auto &State : Function.RegistrationStates->Blocks) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return std::nullopt;
    if (!State.CallbackOnly)
      for (int32_t Level : State.levels())
        if (!Include(Parent, Level))
          return std::nullopt;
  }
  for (const auto &Binding : *Stores)
    for (int32_t Level : Binding.TargetLevels)
      if (!Include(Callback, Level))
        return std::nullopt;
  // An ordinary finally invocation can be the only use of a table row. Keep
  // its callback identity even when no source try-level ever activates it.
  // Both returning and nonlocal calls must bind to their checked occurrences;
  // a mere direct target or an otherwise unused table row grants no retention.
  std::set<va_t> Called;
  if (const auto Returns = getRegistrationSEHReturns(Function))
    for (const auto &[Identity, Target] : Returns->ReturningCalls)
      Called.insert(Target);
  if (const auto Nonlocal = getRegistrationSEHNonlocalCalls(Function))
    for (const auto &[Identity, Target] : *Nonlocal)
      Called.insert(Target);
  for (uint32_t I = 0; I != Chain.Scopes.size(); ++I)
    if (Chain.Scopes[I].IsFinally && Called.count(Chain.Scopes[I].HandlerVA) &&
        !Include(Callback, I))
      return std::nullopt;
  std::vector<uint32_t> Result;
  for (uint32_t I = 0; I != Chain.Scopes.size(); ++I) {
    if (Parent.count(I))
      continue;
    if (!Callback.count(I))
      return std::nullopt;
    Result.push_back(I);
  }
  return Result;
}

} // namespace neverd
