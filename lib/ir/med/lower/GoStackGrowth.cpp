//===- GoStackGrowth.cpp - A Go function's stack-growth path --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Go's runtime grows a goroutine's stack and restarts the function that
/// needed it (src/runtime/stack.go, newstack): the growth path is the stack
/// probe of Go code, which the decompiled body need not repeat.
///
//===----------------------------------------------------------------------===//

#include "GoStackGrowth.h"

#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>

namespace neverd {

bool elideGoStackGrowth(MedFunc &Func, va_t GrowthStart) {
  if (Func.Blocks.size() < 3 || Func.Blocks.front().Ops.empty())
    return false;
  MedBlock &Entry = Func.Blocks.front();
  MedOp &Check = Entry.Ops.back();
  if (Check.Opcode != NdOp::COND_BR || Check.NumInputs < 2 ||
      !Check.Inputs[0].isConst() || Check.Inputs[0].ConstVal != GrowthStart)
    return false;
  const auto Growth = llvm::find_if(Func.Blocks, [&](const MedBlock &Block) {
    return Block.StartAddr == GrowthStart;
  });
  const auto Body = llvm::find_if(Func.Blocks, [&](const MedBlock &Block) {
    return Block.StartAddr == Entry.EndAddr;
  });
  if (Growth == Func.Blocks.end() || Body == Func.Blocks.end() ||
      Growth == Func.Blocks.begin() || Body == Growth ||
      Growth->Preds != std::vector<int>{Entry.Id} ||
      Growth->Succs != std::vector<int>{Entry.Id} ||
      !Growth->ExceptionalSuccs.empty() || !Growth->ExceptionalPreds.empty())
    return false;
  const int GrowthId = Growth->Id;
  const int BodyId = Body->Id;
  // The entry branches straight to the body ...
  const MedVar Target =
      MedVar::makeConst(Body->StartAddr, Check.Inputs[0].Size);
  Check.Opcode = NdOp::BRANCH;
  Check.NumInputs = 0;
  Check.addInput(Target);
  Entry.Succs = {BodyId};
  // ... and nothing starts it again.  Block ids are positions, so the growth
  // block is emptied, as simplifyCfg empties a block, for its renumbering to
  // drop.
  llvm::erase(Entry.Preds, GrowthId);
  Growth->Ops.clear();
  Growth->Phis.clear();
  Growth->Succs.clear();
  Growth->Preds.clear();
  return true;
}

} // namespace neverd
