//===- RegistrationStateBranch.cpp - Proven registration CFG choices ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationFrameBranch.h"
#include "RegistrationStateSolver.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd::registration_state {

std::optional<va_t>
knownFrameBranchTarget(const LowBlock &Block, const FrameTransfer &Transfer,
                       llvm::function_ref<std::optional<int>(va_t)> BlockIdAt) {
  if (Block.Succs.size() != 2 || Block.Ops.empty())
    return std::nullopt;
  const auto &Branch = Block.Ops.back();
  if (Branch.Opcode != NdOp::COND_BR || Branch.NumInputs != 2 ||
      !Branch.Inputs[0].isConst())
    return std::nullopt;
  const auto Condition = Transfer.read(Branch.Inputs[1]);
  const auto Taken = BlockIdAt(Branch.Inputs[0].Offset);
  const auto Fallthrough = BlockIdAt(Block.EndAddr);
  if (!Condition.Constant || Condition.MayBeFrame || !Taken || !Fallthrough ||
      Taken == Fallthrough || !llvm::is_contained(Block.Succs, *Taken) ||
      !llvm::is_contained(Block.Succs, *Fallthrough))
    return std::nullopt;
  return *Condition.Constant ? Branch.Inputs[0].Offset : Block.EndAddr;
}

std::optional<va_t>
RegistrationStateSolver::knownSuccessor(const LowBlock &Block,
                                        const FrameTransfer &Transfer) const {
  return knownFrameBranchTarget(
      Block, Transfer, [&](va_t Address) -> std::optional<int> {
        const auto Found = Entries.find(Address);
        return Found == Entries.end()
                   ? std::nullopt
                   : std::optional<int>{Function.Blocks[Found->second].Id};
      });
}

} // namespace neverd::registration_state
