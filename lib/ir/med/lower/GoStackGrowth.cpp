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

bool elideGoStackGrowth(MedFunc &Func, va_t GrowthStart, va_t SecondCheck) {
  if (Func.Blocks.size() < 3 || Func.Blocks.front().Ops.empty())
    return false;
  MedBlock &Entry = Func.Blocks.front();
  const auto ChecksGrowth = [&](const MedBlock &Block) {
    if (Block.Ops.empty())
      return false;
    const MedOp &Check = Block.Ops.back();
    return Check.Opcode == NdOp::COND_BR && Check.NumInputs >= 2 &&
           Check.Inputs[0].isConst() && Check.Inputs[0].ConstVal == GrowthStart;
  };
  if (!ChecksGrowth(Entry))
    return false;
  const auto At = [&](va_t Start) {
    return llvm::find_if(Func.Blocks, [&](const MedBlock &Block) {
      return Block.StartAddr == Start;
    });
  };
  const auto Growth = At(GrowthStart);
  // The blocks that check, in order, and the body the last one falls into.
  std::vector<MedBlock *> Checks{&Entry};
  auto Body = At(Entry.EndAddr);
  if (SecondCheck != InvalidVA) {
    if (Body == Func.Blocks.end() || Body->StartAddr != SecondCheck ||
        !ChecksGrowth(*Body) || Body->Preds != std::vector<int>{Entry.Id})
      return false;
    Checks.push_back(&*Body);
    Body = At(Body->EndAddr);
  }
  std::vector<int> Checking;
  for (const MedBlock *Check : Checks)
    Checking.push_back(Check->Id);
  std::vector<int> Reaching =
      Growth == Func.Blocks.end() ? std::vector<int>() : Growth->Preds;
  llvm::sort(Checking);
  llvm::sort(Reaching);
  if (Growth == Func.Blocks.end() || Body == Func.Blocks.end() ||
      Growth == Func.Blocks.begin() || Body == Growth ||
      llvm::is_contained(Checks, &*Body) || Reaching != Checking ||
      Growth->Succs != std::vector<int>{Entry.Id} ||
      !Growth->ExceptionalSuccs.empty() || !Growth->ExceptionalPreds.empty())
    return false;
  const int GrowthId = Growth->Id;
  // Each check branches straight to the block after it, the last to the
  // body ...
  for (size_t I = 0; I < Checks.size(); ++I) {
    MedBlock &Next = I + 1 < Checks.size() ? *Checks[I + 1] : *Body;
    MedOp &Check = Checks[I]->Ops.back();
    const MedVar Target =
        MedVar::makeConst(Next.StartAddr, Check.Inputs[0].Size);
    Check.Opcode = NdOp::BRANCH;
    Check.NumInputs = 0;
    Check.addInput(Target);
    Checks[I]->Succs = {Next.Id};
  }
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
