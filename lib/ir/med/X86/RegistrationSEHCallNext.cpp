//===- RegistrationSEHCallNext.cpp - Nonreturning adjacent finally calls -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Separates an adjacent callback entry from a call whose checked invocation
/// always abandons its return PC through a nonlocal continuation.

#include "RegistrationRoots.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/X86/X86RegistrationSEHCall.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>

namespace neverd {

std::optional<std::map<int, int>>
getRegistrationSEHAdjacentExits(const MedFunc &Func) {
  const auto Returns = getRegistrationSEHReturns(Func);
  const auto Nonlocal = getRegistrationSEHNonlocalCalls(Func);
  if (!Returns || !Nonlocal)
    return std::nullopt;
  std::map<int, int> Result;
  if (Nonlocal->empty())
    return Result;
  const auto &Controls = Func.RegistrationStates->SEHControls;
  if (Controls.size() > limits::kMaxRegistrationEHStateWork ||
      Func.Blocks.size() > limits::kMaxRegistrationEHRecords)
    return std::nullopt;
  std::map<int, const MedBlock *> Blocks;
  for (const auto &Block : Func.Blocks)
    if (!Blocks.emplace(Block.Id, &Block).second)
      return std::nullopt;
  size_t Work = 0;
  std::map<va_t, const MedBlock *> Entries;
  for (const auto &Block : Func.Blocks)
    if (!Entries.emplace(Block.StartAddr, &Block).second)
      return std::nullopt;
  for (const auto &[Identity, Target] : *Nonlocal) {
    const auto [Id, Address, Seq] = Identity;
    if (Returns->ReturningCalls.count(Identity) || !Blocks.count(Id))
      continue;
    auto &Block = *Blocks.at(Id);
    if (Block.EndAddr != Target)
      continue;
    if (Block.Ops.empty() || !Entries.count(Target))
      return std::nullopt;
    const auto &Callback = *Entries.at(Target);
    const auto &Call = Block.Ops.back();
    if (Callback.StartAddr != Target || Call.Addr != Address ||
        Call.OriginSeq != Seq || Call.Opcode != NdOp::CALL ||
        (Block.Succs.empty() ? !Call.DoesNotReturn || !Callback.Preds.empty()
                             : Block.Succs != std::vector<int>{Callback.Id} ||
                                   Callback.Preds != std::vector<int>{Id}))
      return std::nullopt;
    if (Controls.size() > limits::kMaxRegistrationEHStateWork - Work)
      return std::nullopt;
    Work += Controls.size();
    if (llvm::count_if(Controls, [&](const auto &Control) {
          return Control.Control ==
                     RegistrationSEHControlContract::Kind::NearCall &&
                 Control.Address == Address && Control.EndAddress == Target &&
                 Control.Target == Target;
        }) != 1)
      return std::nullopt;
    if (!Result.emplace(Id, Callback.Id).second)
      return std::nullopt;
  }
  return Result;
}

void x86_registration::RegistrationRoots::disconnectAdjacentSEHCalls(
    MedFunc &Func) const {
  const auto Exits = getRegistrationSEHAdjacentExits(Func);
  if (!Exits)
    return;
  std::map<int, MedBlock *> Blocks;
  for (auto &Block : Func.Blocks)
    Blocks.emplace(Block.Id, &Block);
  for (const auto &[Id, Target] : *Exits) {
    auto &Block = *Blocks.at(Id);
    Block.Ops.back().DoesNotReturn = true;
    Block.Succs.clear();
    std::erase(Blocks.at(Target)->Preds, Id);
  }
}

} // namespace neverd
