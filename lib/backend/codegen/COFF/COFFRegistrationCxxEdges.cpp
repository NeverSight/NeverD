//===- COFFRegistrationCxxEdges.cpp - Check PE32 scalar copy edges -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationCxxIRProof.h"

#include "neverd/Limits.h"

#include "llvm/IR/CFG.h"

namespace neverd::coff_registration {
namespace {
bool isPrivateScalarSlot(const llvm::Value *Value, const llvm::Type *Type,
                         size_t &Work) {
  const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(Value);
  if (!Slot || !Slot->isStaticAlloca() || !Type->isIntegerTy() ||
      Type->getIntegerBitWidth() > 64 || Slot->getAllocatedType() != Type ||
      Slot->getAddressSpace())
    return false;
  for (const auto *User : Slot->users()) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return false;
    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(User)) {
      if (Load->getPointerOperand() != Slot || Load->getType() != Type ||
          Load->isVolatile() || Load->isAtomic())
        return false;
    } else if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User)) {
      if (Store->getPointerOperand() != Slot ||
          Store->getValueOperand()->getType() != Type || Store->isVolatile() ||
          Store->isAtomic())
        return false;
    } else
      return false;
  }
  return true;
}

bool hasPrivateScalarCopies(const llvm::BasicBlock &Block, size_t &Work) {
  for (const auto &I : Block) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return false;
    if (I.isTerminator())
      continue;
    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
      if (!isPrivateScalarSlot(Load->getPointerOperand(), Load->getType(),
                               Work))
        return false;
    } else if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
      if (!isPrivateScalarSlot(Store->getPointerOperand(),
                               Store->getValueOperand()->getType(), Work))
        return false;
    } else if (!llvm::isa<llvm::TruncInst, llvm::ZExtInst, llvm::SExtInst>(I))
      return false;
  }
  return true;
}
} // namespace

llvm::Error bindCxxCopyEdges(CxxIRControlProof &Proof) {
  size_t Work = 0;
  for (const auto &Source : Proof.Source.Blocks) {
    const auto Segment = Proof.Segments.find(Source.Id);
    if (Segment == Proof.Segments.end())
      continue;
    const auto *From = Segment->second.Exit->getParent();
    const auto *Term = From->getTerminator();
    if (!llvm::isa<llvm::CondBrInst, llvm::UncondBrInst, llvm::SwitchInst>(
            Term))
      continue;
    std::set<const llvm::BasicBlock *> Expected;
    for (int Successor : Source.Succs) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("C++ copy-edge proof exceeded its work budget");
      if (Proof.Segments.count(Successor))
        Expected.insert(Proof.Segments.at(Successor).Enter->getParent());
    }
    for (const auto *Block : llvm::successors(From)) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("C++ copy-edge proof exceeded its work budget");
      if (Expected.count(Block))
        continue;
      const auto *Branch =
          llvm::dyn_cast<llvm::UncondBrInst>(Block->getTerminator());
      if (!Branch || Block->hasAddressTaken() ||
          Block->getUniquePredecessor() != From ||
          !Expected.count(Branch->getSuccessor(0)) ||
          !hasPrivateScalarCopies(*Block, Work))
        return rejectIR("C++ source edge gained unproved copy instructions");
      const auto [Found, New] =
          Proof.CopyEdges.emplace(Block, Branch->getSuccessor(0));
      if (!New && Found->second != Branch->getSuccessor(0))
        return rejectIR("C++ copy block has conflicting source edges");
      const auto Owner = Proof.SourceCatchOwners.find(Source.Id);
      if (Owner != Proof.SourceCatchOwners.end())
        Proof.Catches.at(Owner->second).Blocks.insert(Block);
    }
  }
  return llvm::Error::success();
}
} // namespace neverd::coff_registration
