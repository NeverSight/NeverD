//===- RegistrationSEHCallback.cpp - SEH callback CFG ownership ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keeps callback ownership independent of whether an invocation returns.

#include "neverd/Limits.h"
#include "neverd/ir/med/X86/X86RegistrationCallback.h"
#include "neverd/ir/med/X86/X86RegistrationSEHParentExit.h"

#include <set>

namespace neverd {

std::optional<std::map<int, std::pair<va_t, bool>>>
registrationSEHCallbackOwners(const MedFunc &Function) {
  if (!Function.ExceptionMetadata ||
      !Function.ExceptionMetadata->Registration ||
      Function.ExceptionMetadata->Cxx || !Function.RegistrationStates ||
      Function.SkippedSSA || !Function.RegistrationStates->Complete ||
      !Function.RegistrationStates->CallbackStatesComplete ||
      !Function.RegistrationStates->RegistrationLifetimeComplete)
    return std::nullopt;
  const auto &States = Function.RegistrationStates->Blocks;
  const auto &Scopes = Function.ExceptionMetadata->Registration->Scopes;
  if (States.size() != Function.Blocks.size() ||
      States.size() > limits::kMaxRegistrationEHRecords ||
      Scopes.size() > limits::kMaxRegistrationEHRecords)
    return std::nullopt;
  std::map<int, const RegistrationBlockState *> ById;
  for (const auto &State : States)
    if (!ById.emplace(State.BlockId, &State).second)
      return std::nullopt;
  std::map<int, const MedBlock *> Blocks;
  std::map<va_t, int> Entries;
  for (const auto &Block : Function.Blocks) {
    const auto State = ById.find(Block.Id);
    if (!Blocks.emplace(Block.Id, &Block).second ||
        !Entries.emplace(Block.StartAddr, Block.Id).second ||
        State == ById.end() || State->second->Unknown ||
        State->second->Range.Begin != Block.StartAddr ||
        State->second->Range.End != Block.EndAddr)
      return std::nullopt;
  }
  std::set<std::pair<va_t, bool>> Roots;
  for (const auto &Scope : Scopes)
    Roots.emplace(Scope.IsFinally ? Scope.HandlerVA : Scope.FilterVA,
                  Scope.IsFinally);
  const auto ParentExits = getRegistrationSEHParentExits(Function);
  if (!ParentExits)
    return std::nullopt;
  std::map<int, std::pair<va_t, bool>> Result;
  size_t Work = 0;
  for (const auto &Root : Roots) {
    const auto Entry = Entries.find(Root.first);
    if (Entry == Entries.end() || !ById.at(Entry->second)->CallbackOnly)
      return std::nullopt;
    std::vector<int> Pending{Entry->second};
    while (!Pending.empty()) {
      const int Id = Pending.back();
      Pending.pop_back();
      if (++Work > limits::kMaxRegistrationEHStateWork || !Blocks.count(Id))
        return std::nullopt;
      const auto [It, New] = Result.emplace(Id, Root);
      if (!New) {
        if (It->second != Root)
          return std::nullopt;
        continue;
      }
      const auto *State = ById.at(Id);
      if (State->Reached && !State->CallbackOnly)
        return std::nullopt;
      if (const auto Exit = ParentExits->find(Id); Exit != ParentExits->end()) {
        if (!Root.second || Exit->second.EntryVA != Root.first)
          return std::nullopt;
        continue;
      }
      const auto &Successors = Blocks.at(Id)->Succs;
      Work += Successors.size();
      if (Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      Pending.insert(Pending.end(), Successors.begin(), Successors.end());
    }
  }
  for (const auto &State : States)
    if (State.CallbackOnly && !Result.count(State.BlockId))
      return std::nullopt;
  return Result;
}

} // namespace neverd
