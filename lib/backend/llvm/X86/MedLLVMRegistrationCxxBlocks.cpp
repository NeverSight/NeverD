//===- MedLLVMRegistrationCxxBlocks.cpp - PE32 copy block owners ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMRegistrationCxxBlocks.h"

#include "neverd/Limits.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"

#include <set>

namespace neverd {
std::optional<std::map<llvm::BasicBlock *, X86RegistrationCatchIdentity>>
registrationCxxPHICopyOwners(
    const std::map<int, X86RegistrationCatchIdentity> &Owners,
    const std::map<int, llvm::BasicBlock *> &OriginalBlocks,
    const std::map<llvm::BasicBlock *, std::pair<int, int>> &CopyEdges) {
  if (OriginalBlocks.size() > limits::kMaxRegistrationEHRecords ||
      CopyEdges.size() > limits::kMaxRegistrationEHRecords)
    return std::nullopt;
  std::set<llvm::BasicBlock *> SourceBlocks;
  for (const auto &[Id, Block] : OriginalBlocks)
    if (!Block || !SourceBlocks.insert(Block).second)
      return std::nullopt;
  std::map<llvm::BasicBlock *, X86RegistrationCatchIdentity> Result;
  for (const auto &[Block, Edge] : CopyEdges) {
    const auto From = OriginalBlocks.find(Edge.first);
    const auto To = OriginalBlocks.find(Edge.second);
    if (!Block || SourceBlocks.count(Block) || From == OriginalBlocks.end() ||
        To == OriginalBlocks.end() ||
        Block->getParent() != From->second->getParent() ||
        Block->getParent() != To->second->getParent() ||
        Block->getUniquePredecessor() != From->second)
      return std::nullopt;
    const auto *Branch =
        llvm::dyn_cast_or_null<llvm::UncondBrInst>(Block->getTerminator());
    if (!Branch || Branch->getSuccessor(0) != To->second)
      return std::nullopt;
    const auto Parent = Owners.find(Edge.first);
    const auto Target = Owners.find(Edge.second);
    if ((Parent == Owners.end()) != (Target == Owners.end()) ||
        (Parent != Owners.end() && Parent->second != Target->second))
      return std::nullopt;
    if (Parent != Owners.end())
      Result.emplace(Block, Parent->second);
  }
  return Result;
}
} // namespace neverd
