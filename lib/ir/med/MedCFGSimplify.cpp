//===- MedCFGSimplify.cpp - CFG simplification for MedIR ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Removes trivial jump-only basic blocks from the MedIR CFG and
/// renumbers the remaining blocks.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/LowToMed.h"

#include <algorithm>
#include <map>

namespace neverd {

void LowToMedConverter::simplifyCfg(MedFunc &Func) {
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (int I = 0; I < static_cast<int>(Func.Blocks.size()); ++I) {
      auto &Blk = Func.Blocks[I];
      if (Blk.Succs.size() != 1)
        continue;
      // Exception edges name the exact block where native unwinding transfers
      // control.  Removing either endpoint without rewriting that independent
      // graph can make an invoke skip the landing-pad prologue, so retain such
      // blocks.  The final renumbering below still remaps all retained edges.
      if (!Blk.ExceptionalPreds.empty() || !Blk.ExceptionalSuccs.empty())
        continue;

      bool IsTrivial = true;
      for (auto &Op : Blk.Ops) {
        if (Op.Opcode != NdOp::BRANCH && Op.Opcode != NdOp::NOP) {
          IsTrivial = false;
          break;
        }
      }
      if (!IsTrivial)
        continue;
      if (Blk.Id == 0)
        continue;

      int Target = Blk.Succs[0];
      if (Target == Blk.Id)
        continue;
      if (Target < 0 || Target >= static_cast<int>(Func.Blocks.size()))
        continue;

      std::vector<int> PredsCopy = Blk.Preds;

      for (int PredId : PredsCopy) {
        if (PredId < 0 || PredId >= static_cast<int>(Func.Blocks.size()))
          continue;
        auto &Pred = Func.Blocks[PredId];
        for (auto &S : Pred.Succs) {
          if (S == Blk.Id)
            S = Target;
        }
        Func.Blocks[Target].Preds.push_back(PredId);

        for (auto &Op : Pred.Ops) {
          if ((Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) &&
              Op.NumInputs >= 1 && Op.Inputs[0].isConst()) {
            if (!Func.Blocks[Target].Ops.empty()) {
              va_t TrivialAddr = Blk.Ops.empty() ? 0 : Blk.Ops.front().Addr;
              if (Op.Inputs[0].ConstVal == TrivialAddr) {
                Op.Inputs[0] = MedVar::makeConst(
                    Func.Blocks[Target].Ops.front().Addr, Op.Inputs[0].Size);
              }
            }
          }
        }
      }

      auto &TPreds = Func.Blocks[Target].Preds;
      TPreds.erase(std::remove(TPreds.begin(), TPreds.end(), Blk.Id),
                   TPreds.end());

      // A jump-table dispatch reaches this trivial block through INDIR_BR,
      // whose case targets are carried as JumpTable metadata rather than a
      // constant branch operand, so the predecessor-operand rewrite above
      // misses them. Redirect those targets too — otherwise the emitter cannot
      // map the removed block's address back to a switch case (it would drop
      // the whole switch).
      if (!Blk.Ops.empty() && !Func.Blocks[Target].Ops.empty()) {
        va_t TrivialAddr = Blk.Ops.front().Addr;
        va_t NewAddr = Func.Blocks[Target].Ops.front().Addr;
        if (TrivialAddr != NewAddr)
          for (auto &JT : Func.JumpTables)
            for (auto &Tgt : JT.Targets)
              if (Tgt == TrivialAddr)
                Tgt = NewAddr;
      }

      Blk.Ops.clear();
      Blk.Succs.clear();
      Blk.Preds.clear();
      Changed = true;
    }
  }

  // Remove empty blocks and renumber
  std::vector<MedBlock> NewBlocks;
  std::map<int, int> OldToNew;
  for (auto &Blk : Func.Blocks) {
    if (Blk.Ops.empty() && Blk.Phis.empty() && Blk.Id != 0)
      continue;
    int NewId = static_cast<int>(NewBlocks.size());
    OldToNew[Blk.Id] = NewId;
    Blk.Id = NewId;
    NewBlocks.push_back(std::move(Blk));
  }
  for (auto &Blk : NewBlocks) {
    std::vector<int> RemappedSuccs;
    RemappedSuccs.reserve(Blk.Succs.size());
    for (int S : Blk.Succs) {
      auto It = OldToNew.find(S);
      if (It != OldToNew.end())
        RemappedSuccs.push_back(It->second);
    }
    Blk.Succs = std::move(RemappedSuccs);
    Blk.Preds.clear();
    Blk.ExceptionalPreds.clear();
    for (ExceptionalEdge &Edge : Blk.ExceptionalSuccs) {
      if (Edge.BlockId < 0)
        continue;
      auto It = OldToNew.find(Edge.BlockId);
      Edge.BlockId = It != OldToNew.end() ? It->second : -1;
    }
  }
  // Preds are derived data. Rebuild them from the retained successor edges so
  // removing an empty block cannot leave a stale predecessor ID or make the
  // two edge directions disagree.
  for (const auto &Blk : NewBlocks)
    for (int S : Blk.Succs)
      NewBlocks[S].Preds.push_back(Blk.Id);
  // Exceptional predecessors are likewise derived from the successor graph.
  // Rebuild them after renumbering so SSA recognizes every landing pad as an
  // independent root and the LLVM emitter targets the prologue block itself.
  for (const auto &Blk : NewBlocks)
    for (const ExceptionalEdge &Edge : Blk.ExceptionalSuccs)
      if (Edge.BlockId >= 0 &&
          Edge.BlockId < static_cast<int>(NewBlocks.size())) {
        ExceptionalEdge Pred = Edge;
        Pred.BlockId = Blk.Id;
        auto &Preds = NewBlocks[Edge.BlockId].ExceptionalPreds;
        if (std::find(Preds.begin(), Preds.end(), Pred) == Preds.end())
          Preds.push_back(Pred);
      }
  Func.Blocks = std::move(NewBlocks);
}

} // namespace neverd
