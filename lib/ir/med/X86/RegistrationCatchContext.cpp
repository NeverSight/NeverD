//===- RegistrationCatchContext.cpp - PE32 invocation ownership ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Project checked catch identities into current MedIR blocks and parents.
#include "neverd/Limits.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86RegistrationCallback.h"

namespace neverd {
std::optional<std::map<int, RegistrationCatchIdentity>>
registrationCatchBlocks(const MedFunc &Function) {
  if (!Function.ExceptionMetadata ||
      !Function.ExceptionMetadata->Registration ||
      Function.ExceptionMetadata->Encoding !=
          ExceptionEncoding::X86CxxFuncInfo ||
      Function.ExceptionMetadata->ParseStatus !=
          ExceptionParseStatus::Complete ||
      !Function.ExceptionMetadata->Cxx ||
      !Function.ExceptionMetadata->Cxx->hasValidStateGraph() ||
      Function.ExceptionMetadata->Cxx->TryBlocks.empty() ||
      Function.ExceptionMetadata->Cxx->TryBlocks.size() >
          limits::kMaxRegistrationEHRecords ||
      !Function.RegistrationStates || !Function.RegistrationStates->Complete ||
      !Function.RegistrationStates->CallbackStatesComplete ||
      !Function.RegistrationStates->CxxContinuationsComplete ||
      !Function.RegistrationStates->RegistrationLifetimeComplete ||
      Function.Blocks.size() > limits::kMaxRegistrationEHRecords ||
      Function.RegistrationStates->Blocks.size() >
          limits::kMaxRegistrationEHRecords)
    return std::nullopt;
  const auto &Tries = Function.ExceptionMetadata->Cxx->TryBlocks;
  std::map<int, const MedBlock *> Blocks;
  std::map<va_t, int> Entries;
  std::map<std::pair<va_t, va_t>, const RegistrationBlockState *> States;
  for (const auto &Block : Function.Blocks)
    if (!Blocks.emplace(Block.Id, &Block).second ||
        !Entries.emplace(Block.StartAddr, Block.Id).second)
      return std::nullopt;
  for (const auto &State : Function.RegistrationStates->Blocks)
    if (!States
             .emplace(std::make_pair(State.Range.Begin, State.Range.End),
                      &State)
             .second)
      return std::nullopt;
  std::map<int, RegistrationCatchIdentity> Owners;
  std::map<RegistrationCatchIdentity, std::vector<RegistrationCatchIdentity>>
      Contexts;
  size_t Work = 0;
  for (const auto &Block : Function.Blocks) {
    const auto Found = States.find({Block.StartAddr, Block.EndAddr});
    if (Found == States.end() || !Found->second->Reached)
      continue;
    const auto &State = *Found->second;
    if (State.Unknown || (!State.CallbackOnly && !State.CxxCatchStacks.empty()))
      return std::nullopt;
    if (!State.CallbackOnly)
      continue;
    if (State.CxxCatchStacks.size() != 1 || State.CxxCatchStacks[0].empty())
      return std::nullopt;
    const auto &Stack = State.CxxCatchStacks[0];
    for (const auto &[Region, Clause] : Stack)
      if (++Work > limits::kMaxRegistrationEHStateWork ||
          Region >= Tries.size() || Clause >= Tries[Region].Handlers.size())
        return std::nullopt;
    const auto Identity = Stack.back();
    const auto [Context, New] = Contexts.emplace(Identity, Stack);
    if ((!New && Context->second != Stack) ||
        State.CxxMinimumTryLevel != Tries[Identity.first].TryHigh + 1)
      return std::nullopt;
    Owners.emplace(Block.Id, Identity);
  }
  for (uint32_t Region = 0; Region < Tries.size(); ++Region) {
    const auto &Try = Tries[Region];
    if (Try.Handlers.empty() ||
        Try.Handlers.size() > limits::kMaxRegistrationEHRecords)
      return std::nullopt;
    for (uint32_t Index = 0; Index < Try.Handlers.size(); ++Index) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      const auto Entry = Entries.find(Try.Handlers[Index].HandlerVA);
      const RegistrationCatchIdentity Identity{Region, Index};
      if (Entry == Entries.end() || !Owners.count(Entry->second) ||
          Owners.at(Entry->second) != Identity)
        return std::nullopt;
      const auto &Stack = Contexts.at(Identity);
      for (size_t Depth = 0; Depth + 1 < Stack.size(); ++Depth) {
        const auto Parent = Contexts.find(Stack[Depth]);
        if (++Work > limits::kMaxRegistrationEHStateWork ||
            Parent == Contexts.end() || Parent->second.size() != Depth + 1 ||
            !std::equal(Parent->second.begin(), Parent->second.end(),
                        Stack.begin()))
          return std::nullopt;
        const auto &Outer = Tries[Stack[Depth].first];
        if (Try.TryLow <= Outer.TryHigh + 1 || Try.CatchHigh > Outer.CatchHigh)
          return std::nullopt;
      }
    }
  }
  for (const auto &Block : Function.Blocks) {
    const auto Owner = Owners.find(Block.Id);
    for (int Next : Block.Succs) {
      if (++Work > limits::kMaxRegistrationEHStateWork || !Blocks.count(Next))
        return std::nullopt;
      const auto Target = Owners.find(Next);
      if ((Owner == Owners.end()) != (Target == Owners.end()) ||
          (Owner != Owners.end() && Owner->second != Target->second))
        return std::nullopt;
    }
  }
  for (const auto &Resume : Function.RegistrationStates->CxxContinuations) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return std::nullopt;
    const RegistrationCatchIdentity Identity{Resume.TryIndex,
                                             Resume.CatchIndex};
    const auto Context = Contexts.find(Identity);
    const auto Entry = Entries.find(Resume.TargetVA);
    if (Context == Contexts.end() || Entry == Entries.end())
      return std::nullopt;
    const auto Target = Owners.find(Entry->second);
    const auto &Stack = Context->second;
    if (Stack.size() == 1) {
      if (Target != Owners.end() || Resume.SavedCallbackVA)
        return std::nullopt;
    } else {
      const auto Parent = Stack[Stack.size() - 2];
      if (Target == Owners.end() || Target->second != Parent ||
          (Function.ExceptionMetadata->Registration->hasCxxCallbackStack() &&
           !Resume.SavedCallbackVA) ||
          (Resume.SavedCallbackVA &&
           Resume.SavedCallbackVA !=
               Tries[Parent.first].Handlers[Parent.second].HandlerVA))
        return std::nullopt;
    }
  }
  return Owners;
}

std::optional<std::vector<std::optional<RegistrationCatchIdentity>>>
registrationCatchParents(const MedFunc &Function) {
  if (!registrationCatchBlocks(Function))
    return std::nullopt;
  const auto &Tries = Function.ExceptionMetadata->Cxx->TryBlocks;
  std::vector<std::optional<RegistrationCatchIdentity>> Parents(Tries.size());
  size_t Work = 0;
  for (uint32_t Region = 0; Region < Tries.size(); ++Region) {
    bool First = true;
    for (const auto &Handler : Tries[Region].Handlers) {
      const RegistrationBlockState *Entry = nullptr;
      for (const auto &State : Function.RegistrationStates->Blocks) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return std::nullopt;
        if (State.Range.Begin == Handler.HandlerVA)
          Entry = &State;
      }
      if (!Entry || Entry->CxxCatchStacks.size() != 1)
        return std::nullopt;
      const auto &Stack = Entry->CxxCatchStacks[0];
      const auto Parent = Stack.size() > 1
                              ? std::optional(Stack[Stack.size() - 2])
                              : std::nullopt;
      if (!First && Parents[Region] != Parent)
        return std::nullopt;
      Parents[Region] = Parent;
      First = false;
    }
  }
  return Parents;
}
} // namespace neverd
