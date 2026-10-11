//===- LowNoReturn.cpp - LowIR terminating-path proof --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/LowNoReturn.h"

#include "neverd/ir/intrinsics/IntrinsicTermination.h"

#include <map>
#include <set>
#include <vector>

namespace neverd {

bool isArchitecturalNoReturn(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs < 1 ||
      !Op.Inputs[0].isConst())
    return false;
  return intrinsicNeverReturns(
      static_cast<Intrinsic>(Op.Inputs[0].Offset),
      Op.NumInputs >= 2 && Op.Inputs[1].isConst()
          ? std::optional<uint64_t>(Op.Inputs[1].Offset)
          : std::nullopt);
}

bool lowFunctionNeverReturns(const LowFunc &Func, Arch TheArch) {
  std::map<int, const LowBlock *> BlocksById;
  const LowBlock *Entry = nullptr;
  for (const LowBlock &Block : Func.Blocks) {
    BlocksById.emplace(Block.Id, &Block);
    if (!Entry && Block.StartAddr == Func.Entry)
      Entry = &Block;
  }
  if (!Entry)
    return false;
  std::vector<const LowBlock *> Pending{Entry};
  std::set<int> Visited{Entry->Id};
  bool SawTerminator = false;
  while (!Pending.empty()) {
    const LowBlock &Block = *Pending.back();
    Pending.pop_back();
    // Each instruction in order: a return ends the proof, a call marked as
    // not returning or an architectural trap ends the path.
    bool PathEnds = false;
    for (const LowInstructionBoundary &Insn : Block.InstructionBoundaries) {
      if (isUnconditionalNoReturn(Insn)) {
        PathEnds = true;
        break;
      }
      for (uint64_t K = Insn.FirstOp;
           K < Insn.FirstOp + Insn.OpCount && K < Block.Ops.size(); ++K) {
        if (Block.Ops[K].Opcode == NdOp::RETURN)
          return false;
        if (isArchitecturalNoReturn(Block.Ops[K]))
          PathEnds = true;
      }
      if (PathEnds)
        break;
    }
    // Without instruction boundaries no call can be told apart as final.
    if (!Block.hasInstructionBoundaries())
      for (const LowOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::RETURN)
          return false;
    // No-return applies to the ordinary call continuation. The call can
    // still throw into a handler that returns from this enclosing function.
    std::vector<int> Next;
    for (const ExceptionalEdge &Edge : Block.ExceptionalSuccs) {
      // A handler outside the lifted body may return normally.
      if (Edge.BlockId < 0)
        return false;
      Next.push_back(Edge.BlockId);
    }
    if (PathEnds)
      SawTerminator = true;
    else
      Next.insert(Next.end(), Block.Succs.begin(), Block.Succs.end());
    // A path that stops without a terminator (an unresolved jump, a decode
    // failure) proves nothing.
    if (Next.empty() && !PathEnds)
      return false;
    for (int Id : Next) {
      auto It = BlocksById.find(Id);
      if (It == BlocksById.end())
        return false;
      if (Visited.insert(Id).second)
        Pending.push_back(It->second);
    }
  }
  return SawTerminator;
}

} // namespace neverd
