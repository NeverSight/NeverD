//===- CFGBuilderBlocks.cpp - Basic-block formation ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Turns the decoded instruction map into basic blocks: cutting blocks at
/// conditional fall-throughs, materializing the blocks between consecutive
/// boundaries, wiring ordinary successor and predecessor edges, and
/// renumbering so the function entry is block 0.  Exceptional edges are added
/// in CFGBuilderException.cpp; see CFGBuilder.cpp for the exploration that
/// produces the instructions.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CFGBuilder.h"

#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <vector>

namespace neverd {

void CFGBuilder::splitBlocks() {
  for (auto &[Addr, Rec] : Insns) {
    if (Rec.IsBranch && Rec.IsCond)
      BlockStarts.insert(Addr + Rec.Size);
  }
}

void CFGBuilder::normalizeEntryBlock(LowFunc &Func) {
  if (Func.Blocks.empty())
    return;

  auto EntryIt = std::find_if(
      Func.Blocks.begin(), Func.Blocks.end(),
      [&](const LowBlock &Blk) { return Blk.StartAddr == Func.Entry; });
  if (EntryIt == Func.Blocks.end())
    llvm::report_fatal_error("CFGBuilder: function entry block is missing");

  const size_t EntryIndex =
      static_cast<size_t>(std::distance(Func.Blocks.begin(), EntryIt));
  const size_t NumBlocks = Func.Blocks.size();
  std::vector<size_t> NewToOld;
  NewToOld.reserve(NumBlocks);
  NewToOld.push_back(EntryIndex);
  for (size_t I = 0; I < NumBlocks; ++I)
    if (I != EntryIndex)
      NewToOld.push_back(I);

  std::vector<int> OldToNew(NumBlocks, -1);
  for (size_t NewId = 0; NewId < NumBlocks; ++NewId) {
    const int OldId = Func.Blocks[NewToOld[NewId]].Id;
    if (OldId < 0 || static_cast<size_t>(OldId) >= NumBlocks ||
        OldToNew[OldId] != -1)
      llvm::report_fatal_error("CFGBuilder: invalid block identity");
    OldToNew[OldId] = static_cast<int>(NewId);
  }

  std::vector<LowBlock> Normalized;
  Normalized.reserve(NumBlocks);
  for (size_t NewId = 0; NewId < NumBlocks; ++NewId) {
    LowBlock Blk = std::move(Func.Blocks[NewToOld[NewId]]);
    auto RemapEdges = [&](std::vector<int> &Edges) {
      for (int &Id : Edges) {
        if (Id < 0 || static_cast<size_t>(Id) >= NumBlocks || OldToNew[Id] < 0)
          llvm::report_fatal_error("CFGBuilder: invalid block edge");
        Id = OldToNew[Id];
      }
    };
    RemapEdges(Blk.Succs);
    RemapEdges(Blk.Preds);
    Blk.Id = static_cast<int>(NewId);
    Normalized.push_back(std::move(Blk));
  }
  Func.Blocks = std::move(Normalized);
}

void CFGBuilder::rebuildBlocks(LowFunc &Func) {
  Func.Blocks.clear();
  Func.JumpTables.clear();

  // A boundary can name an address recursive descent never decoded: the end of
  // a guarded range an exception table declared, or the fall-through of a
  // conditional branch at the edge of the mapped image.  A block there would
  // carry no instruction and so would stand for nothing, so it is dropped.  The
  // entry survives unconditionally — the function is defined by it.
  std::vector<va_t> Starts;
  Starts.reserve(BlockStarts.size());
  for (va_t Start : BlockStarts)
    if (Start == Func.Entry || Insns.count(Start))
      Starts.push_back(Start);
  std::sort(Starts.begin(), Starts.end());

  std::map<va_t, int> AddrToBlock;
  for (size_t I = 0; I < Starts.size(); ++I) {
    LowBlock Blk;
    Blk.Id = static_cast<int>(I);
    Blk.StartAddr = Starts[I];

    va_t End = (I + 1 < Starts.size()) ? Starts[I + 1] : InvalidVA;

    for (auto It = Insns.lower_bound(Starts[I]); It != Insns.end(); ++It) {
      if (It->first >= End)
        break;
      for (auto &Op : It->second.Ops)
        Blk.Ops.push_back(Op);
      Blk.EndAddr = It->first + It->second.Size;
    }

    AddrToBlock[Starts[I]] = Blk.Id;
    Func.Blocks.push_back(std::move(Blk));
  }

  linkSuccessors(Func, AddrToBlock);
  normalizeEntryBlock(Func);
  linkExceptionalSuccessors(Func);
  extractJumpTables(Func);
  fixupFpuStack(Func);
}

//===----------------------------------------------------------------------===//
// fixupFpuStack — the x86/x86-64 x87 stack-pointer (TOP) fixup — is
// architecture-gated, so it is defined in CFGBuilderX86Fpu.cpp following the
// target-dispatch split used by the jump-table detectors.
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// linkSuccessors — wire up block successor/predecessor edges
//===----------------------------------------------------------------------===//

void CFGBuilder::linkSuccessors(LowFunc &Func,
                                const std::map<va_t, int> &AddrToBlock) {
  for (size_t I = 0; I < Func.Blocks.size(); ++I) {
    auto &Blk = Func.Blocks[I];
    if (Blk.Ops.empty())
      continue;

    va_t LastAddr = Blk.Ops.back().Addr;
    auto It = Insns.find(LastAddr);
    if (It == Insns.end())
      continue;
    auto &Rec = It->second;

    if (Rec.IsRet && Rec.IsCond && Rec.IsBranch) {
      auto BIt = AddrToBlock.find(Rec.BranchTarget);
      if (Rec.BranchTarget != InvalidVA && BIt != AddrToBlock.end())
        Blk.Succs.push_back(BIt->second);
    } else if (Rec.IsRet) {
      // Terminal — no successors.
    } else if (Rec.IsBranch && Rec.IsIndirect &&
               !Rec.JumpTableTargets.empty()) {
      for (va_t T : Rec.JumpTableTargets) {
        auto TIt = AddrToBlock.find(T);
        if (TIt != AddrToBlock.end())
          Blk.Succs.push_back(TIt->second);
      }
    } else if (Rec.IsBranch && !Rec.IsIndirect && !Rec.IsCall) {
      if (Rec.IsCond) {
        va_t Fall = Rec.Addr + Rec.Size;
        auto FIt = AddrToBlock.find(Fall);
        if (FIt != AddrToBlock.end())
          Blk.Succs.push_back(FIt->second);
      }
      auto BIt = AddrToBlock.find(Rec.BranchTarget);
      if (Rec.BranchTarget != InvalidVA && BIt != AddrToBlock.end())
        Blk.Succs.push_back(BIt->second);
    } else if (!Rec.IsBranch || Rec.IsCall) {
      // A no-return call (longjmp/abort/exit/...) ends control flow: do not
      // wire a fall-through edge to the next block (the emitter then terminates
      // the block with a dead `ret`, which the noreturn-marked callee folds
      // away).
      if (!Rec.IsNoReturnCall && I + 1 < Func.Blocks.size())
        Blk.Succs.push_back(static_cast<int>(I + 1));
    }

    for (int S : Blk.Succs) {
      if (S >= 0 && S < static_cast<int>(Func.Blocks.size()))
        Func.Blocks[S].Preds.push_back(Blk.Id);
    }
  }
}

// Exceptional successor/predecessor linking is defined in
// CFGBuilderException.cpp.

} // namespace neverd
