//===- RegistrationCatchBlocks.cpp - PE32 catch invocation ownership ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/backend/llvm/X86RegistrationCatch.h"
#include "neverd/ir/med/MedIR.h"

namespace neverd {
std::optional<std::map<int, X86RegistrationCatchIdentity>>
projectX86RegistrationCatchBlocks(const MedFunc &Function) {
  if (!Function.ExceptionMetadata || !Function.ExceptionMetadata->Cxx ||
      Function.ExceptionMetadata->Cxx->TryBlocks.empty() ||
      Function.ExceptionMetadata->Cxx->TryBlocks.size() >
          limits::kMaxRegistrationEHRecords ||
      !Function.RegistrationStates || !Function.RegistrationStates->Complete ||
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
  std::map<int, X86RegistrationCatchIdentity> Owners;
  size_t Work = 0;
  for (uint32_t Region = 0; Region < Tries.size(); ++Region) {
    const auto &Try = Tries[Region];
    if (Try.Handlers.empty() ||
        Try.Handlers.size() > limits::kMaxRegistrationEHRecords)
      return std::nullopt;
    for (uint32_t Index = 0; Index < Try.Handlers.size(); ++Index) {
      const X86RegistrationCatchIdentity Identity{Region, Index};
      const auto Entry = Entries.find(Try.Handlers[Index].HandlerVA);
      if (Entry == Entries.end())
        return std::nullopt;
      std::vector<int> Pending{Entry->second};
      while (!Pending.empty()) {
        const int Id = Pending.back();
        Pending.pop_back();
        if (++Work > limits::kMaxRegistrationEHStateWork || !Blocks.count(Id))
          return std::nullopt;
        const auto [Owner, Inserted] = Owners.emplace(Id, Identity);
        if (!Inserted) {
          if (Owner->second != Identity)
            return std::nullopt;
          continue;
        }
        const auto &Block = *Blocks.at(Id);
        const auto State = States.find({Block.StartAddr, Block.EndAddr});
        if (State == States.end() || !State->second->Reached ||
            !State->second->CallbackOnly || State->second->Unknown ||
            State->second->CxxMinimumTryLevel != Try.TryHigh + 1)
          return std::nullopt;
        // Catch returns transfer through the CRT. Only ordinary edges stay in
        // the callback invocation; exceptional continuation edges do not.
        Pending.insert(Pending.end(), Block.Succs.begin(), Block.Succs.end());
      }
    }
  }
  for (const auto &Block : Function.Blocks) {
    const auto State = States.find({Block.StartAddr, Block.EndAddr});
    if (State == States.end() || !State->second->Reached)
      continue;
    if (State->second->CallbackOnly != bool(Owners.count(Block.Id)))
      return std::nullopt;
    for (int Next : Block.Succs)
      if (Owners.count(Next) &&
          (!Owners.count(Block.Id) || Owners.at(Block.Id) != Owners.at(Next)))
        return std::nullopt;
  }
  return Owners;
}
} // namespace neverd
