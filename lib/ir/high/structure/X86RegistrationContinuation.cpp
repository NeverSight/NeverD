//===- X86RegistrationContinuation.cpp - Shared PE32 resume tails --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep runtime resume components outside a synchronously protected prefix.
//===----------------------------------------------------------------------===//
#include "X86RegistrationTry.h"

#include "neverd/Limits.h"

#include <map>

namespace neverd {
std::optional<std::set<int>> registrationResumeClosure(const MedFunc &Med,
                                                       size_t &Work) {
  if (!Med.RegistrationStates ||
      !Med.RegistrationStates->CxxContinuationsComplete)
    return std::nullopt;
  std::map<va_t, int> Entries;
  std::map<int, const MedBlock *> Blocks;
  auto Charge = [&](size_t Amount) {
    if (Work > limits::kMaxRegistrationEHStateWork ||
        Amount > limits::kMaxRegistrationEHStateWork - Work)
      return false;
    Work += Amount;
    return true;
  };
  for (const auto &Block : Med.Blocks)
    if (!Charge(1) || !Entries.emplace(Block.StartAddr, Block.Id).second ||
        !Blocks.emplace(Block.Id, &Block).second)
      return std::nullopt;
  std::vector<int> Pending;
  for (const auto &Resume : Med.RegistrationStates->CxxContinuations) {
    const auto Entry = Entries.find(Resume.TargetVA);
    if (!Charge(1) || Entry == Entries.end())
      return std::nullopt;
    Pending.push_back(Entry->second);
  }
  std::set<int> Result;
  while (!Pending.empty()) {
    const int Id = Pending.back();
    Pending.pop_back();
    if (!Charge(1) || !Blocks.count(Id))
      return std::nullopt;
    if (!Result.insert(Id).second)
      continue;
    const auto &Block = *Blocks.at(Id);
    if (!Charge(Block.Succs.size()))
      return std::nullopt;
    Pending.insert(Pending.end(), Block.Succs.begin(), Block.Succs.end());
  }
  return Result;
}
} // namespace neverd
