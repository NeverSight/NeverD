//===- RegistrationReachability.cpp - PE32 source reachability -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Remove source-proven dead predecessors before callback SSA construction.
//===----------------------------------------------------------------------===//

#include "RegistrationRoots.h"

#include "neverd/Limits.h"

#include <set>

namespace neverd::x86_registration {
void RegistrationRoots::removeUnreachedBlocks(MedFunc &Func) const {
  const auto &States = *Low.RegistrationStates;
  if (!States.CallbackStatesComplete || !States.CxxContinuationsComplete ||
      Func.Blocks.size() > limits::kMaxRegistrationEHRecords ||
      States.Blocks.size() > limits::kMaxRegistrationEHRecords)
    return;
  std::map<std::pair<va_t, va_t>, const RegistrationBlockState *> Source;
  for (const auto &State : States.Blocks)
    if (!Source
             .emplace(std::make_pair(State.Range.Begin, State.Range.End),
                      &State)
             .second)
      return;
  std::set<int> Dead;
  std::set<int> Ids;
  for (const auto &Block : Func.Blocks) {
    const auto Found = Source.find({Block.StartAddr, Block.EndAddr});
    if (Found == Source.end() || !Ids.insert(Block.Id).second ||
        !Block.Phis.empty())
      return;
    if (Found->second->Reached)
      continue;
    if (Block.StartAddr == Low.Entry ||
        Low.OrdinaryModuleAnalysisRoots.count(Block.StartAddr))
      return;
    Dead.insert(Block.Id);
  }
  size_t Work = 0;
  for (const auto &Block : Func.Blocks) {
    if (Dead.count(Block.Id))
      continue;
    for (int Next : Block.Succs)
      if (++Work > limits::kMaxRegistrationEHStateWork || !Ids.count(Next) ||
          Dead.count(Next))
        return;
    for (const auto &Edge : Block.ExceptionalSuccs)
      if (++Work > limits::kMaxRegistrationEHStateWork ||
          (Edge.BlockId >= 0 &&
           (!Ids.count(Edge.BlockId) || Dead.count(Edge.BlockId))))
        return;
  }
  // No ordinary or exceptional execution enters these intervals. Clear them
  // for the existing pre-SSA CFG compactor, which owns block renumbering.
  // Removing their edges prevents an unreachable destructor tail from adding
  // a fabricated ordinary predecessor to a live resumed catch.
  for (auto &Block : Func.Blocks) {
    if (Dead.count(Block.Id)) {
      Block.Ops.clear();
      Block.Succs.clear();
      Block.ExceptionalSuccs.clear();
      Block.Preds.clear();
      Block.ExceptionalPreds.clear();
    } else {
      std::erase_if(Block.Preds, [&](int Id) { return Dead.count(Id); });
      std::erase_if(Block.ExceptionalPreds,
                    [&](const auto &Edge) { return Dead.count(Edge.BlockId); });
    }
  }
}
} // namespace neverd::x86_registration
