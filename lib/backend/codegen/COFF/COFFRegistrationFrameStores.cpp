//===- COFFRegistrationFrameStores.cpp - Private PE32 spill definitions --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Skip unrelated instructions only after proving that all uses of a private
/// allocation are direct memory accesses. Aliased allocations keep the full
/// reaching-store walk in the frame proof.
//===----------------------------------------------------------------------===//

#include "COFFRegistrationFrameStores.h"

#include "neverd/Limits.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <bit>

namespace neverd::coff_registration {
namespace {
bool charge(size_t &Work, size_t Count = 1) {
  if (Work > limits::kMaxRegistrationEHStateWork ||
      Count > limits::kMaxRegistrationEHStateWork - Work)
    return false;
  Work += Count;
  return true;
}
} // namespace

bool RegistrationFrameStores::build(
    llvm::ArrayRef<const llvm::Function *> Functions, size_t &Work) {
  Stores.clear();
  AllStores.clear();
  Positions.clear();
  for (const auto *Function : Functions)
    for (const auto &Block : *Function) {
      unsigned Position = 0;
      for (const auto &I : Block) {
        if (!charge(Work))
          return false;
        Positions.try_emplace(&I, Position++);
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I))
          AllStores[&Block].push_back(Store);
        const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&I);
        if (!Slot || !Slot->isStaticAlloca())
          continue;
        bool Direct = true;
        for (const auto &Use : Slot->uses()) {
          if (!charge(Work))
            return false;
          if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Use.getUser());
              Load && Use.getOperandNo() == Load->getPointerOperandIndex())
            continue;
          if (const auto *Store =
                  llvm::dyn_cast<llvm::StoreInst>(Use.getUser());
              Store && Use.getOperandNo() == Store->getPointerOperandIndex())
            continue;
          Direct = false;
          break;
        }
        if (Direct)
          Stores.try_emplace(Slot);
      }
    }
  for (auto &[Root, Blocks] : Stores) {
    for (const auto *User : Root->users()) {
      if (!charge(Work))
        return false;
      if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User))
        Blocks[Store->getParent()].push_back(Store);
    }
    for (auto &[Block, Definitions] : Blocks) {
      // Sorting user-list order costs at most N log N comparisons.
      if (!charge(Work, Definitions.size() *
                            (std::bit_width(Definitions.size()) + 1)))
        return false;
      llvm::sort(Definitions, [&](const auto *Left, const auto *Right) {
        return Positions.at(Left) < Positions.at(Right);
      });
    }
  }
  return true;
}

std::optional<llvm::ArrayRef<const llvm::StoreInst *>>
RegistrationFrameStores::candidates(const llvm::Value *Root,
                                    const llvm::BasicBlock *Block,
                                    const llvm::Instruction *Through,
                                    size_t &Work) const {
  if (!charge(Work))
    return std::nullopt;
  const auto Found = Stores.find(Root);
  const auto &Blocks = Found == Stores.end() ? AllStores : Found->second;
  const auto Definitions = Blocks.find(Block);
  if (!Through || Definitions == Blocks.end())
    return llvm::ArrayRef<const llvm::StoreInst *>{};
  const auto &Candidates = Definitions->second;
  if (!charge(Work, std::bit_width(Candidates.size()) + 1))
    return std::nullopt;
  const auto End = llvm::upper_bound(Candidates, Positions.at(Through),
                                     [&](unsigned Position, const auto *I) {
                                       return Position < Positions.at(I);
                                     });
  return llvm::ArrayRef(Candidates).take_front(End - Candidates.begin());
}
} // namespace neverd::coff_registration
