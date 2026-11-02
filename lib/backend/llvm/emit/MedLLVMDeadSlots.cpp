//===- MedLLVMDeadSlots.cpp - Private SSA storage cleanup -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Remove unobserved private slots and entry seeds overwritten before all uses.
///
//===----------------------------------------------------------------------===//

#include "MedLLVMDeadSlots.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Transforms/Utils/Local.h"

namespace neverd {
namespace {
void discardPrivateIdentityStores(llvm::AllocaInst &Slot) {
  llvm::SmallVector<llvm::StoreInst *, 4> Redundant;
  for (auto *User : Slot.users()) {
    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(User)) {
      if (!Load->isSimple() || Load->hasMetadata() ||
          Load->getPointerOperand() != &Slot)
        return;
      continue;
    }
    auto *Store = llvm::dyn_cast<llvm::StoreInst>(User);
    if (!Store || !Store->isSimple() || Store->hasMetadata() ||
        Store->getPointerOperand() != &Slot ||
        Store->getValueOperand() == &Slot)
      return;
    auto *Load = llvm::dyn_cast<llvm::LoadInst>(Store->getValueOperand());
    if (Load && Load->getPointerOperand() == &Slot && Load->hasOneUse() &&
        Load->getNextNode() == Store)
      Redundant.push_back(Store);
  }
  // Entry register copies can become a private load/store identity cycle.
  // Neither operation observes the value once the copy's real users are dead.
  // Adjacency excludes an intervening write; every user above must be a plain
  // direct access, so escaping, ordered and source-annotated slots stay intact.
  for (auto *Store : Redundant) {
    auto *Load = llvm::cast<llvm::LoadInst>(Store->getValueOperand());
    Store->eraseFromParent();
    Load->eraseFromParent();
  }
}
} // namespace

void discardOverwrittenMedLLVMSeeds(llvm::ArrayRef<llvm::StoreInst *> Seeds) {
  if (Seeds.empty())
    return;
  auto &Function = *Seeds.front()->getFunction();
  llvm::DominatorTree Dominators(Function);
  for (auto *Seed : Seeds) {
    auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(Seed->getPointerOperand());
    if (!Slot || Seed->getParent() != &Function.getEntryBlock() ||
        Slot->getParent() != &Function.getEntryBlock() || !Seed->isSimple() ||
        Seed->hasMetadata() || Slot->hasMetadata())
      continue;

    llvm::StoreInst *Definition = nullptr;
    bool Private = true;
    for (auto *User : Slot->users()) {
      if (User == Seed)
        continue;
      if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(User)) {
        if (Definition || !Store->isSimple() || Store->hasMetadata() ||
            Store->getPointerOperand() != Slot ||
            Store->getValueOperand() == Slot) {
          Private = false;
          break;
        }
        Definition = Store;
      } else if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(User)) {
        if (!Load->isSimple() || Load->hasMetadata() ||
            Load->getPointerOperand() != Slot) {
          Private = false;
          break;
        }
      } else {
        Private = false;
        break;
      }
    }
    if (!Private || !Definition || !Dominators.dominates(Seed, Definition))
      continue;
    for (auto *User : Slot->users()) {
      if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(User);
          Load && !Dominators.dominates(Definition, Load)) {
        Private = false;
        break;
      }
    }
    if (!Private)
      continue;

    auto *Initial = llvm::dyn_cast<llvm::Instruction>(Seed->getValueOperand());
    Seed->eraseFromParent();
    if (Initial && !Initial->hasMetadata() &&
        llvm::isInstructionTriviallyDead(Initial))
      Initial->eraseFromParent();
  }
}

void discardDeadMedLLVMSlots(llvm::ArrayRef<llvm::AllocaInst *> Slots) {
  llvm::SmallPtrSet<llvm::AllocaInst *, 32> PrivateSlots(Slots.begin(),
                                                         Slots.end());
  llvm::SmallVector<llvm::WeakTrackingVH, 32> Pending(Slots.begin(),
                                                      Slots.end());
  while (!Pending.empty()) {
    auto Handle = Pending.pop_back_val();
    auto *Instruction = llvm::dyn_cast_or_null<llvm::Instruction>(Handle);
    if (!Instruction || Instruction->hasMetadata())
      continue;
    if (auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(Instruction)) {
      if (!PrivateSlots.contains(Slot))
        continue;
      discardPrivateIdentityStores(*Slot);
      // A private slot with only plain stores has no observer. In particular,
      // an unused PHI must not turn an unavailable incoming register into a
      // native stack read when the function is emitted with optnone.
      bool HasObserver = false;
      for (auto *User : Slot->users()) {
        const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User);
        if (!Store || Store->getPointerOperand() != Slot ||
            Store->getValueOperand() == Slot || !Store->isSimple() ||
            Store->hasMetadata()) {
          HasObserver = true;
          break;
        }
      }
      if (HasObserver)
        continue;
      while (!Slot->use_empty()) {
        auto *Store = llvm::cast<llvm::StoreInst>(*Slot->user_begin());
        Pending.emplace_back(Store->getValueOperand());
        Store->eraseFromParent();
      }
      PrivateSlots.erase(Slot);
    } else if (!llvm::isInstructionTriviallyDead(Instruction)) {
      continue;
    }
    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Instruction)) {
      const auto *Slot =
          llvm::dyn_cast<llvm::AllocaInst>(Load->getPointerOperand());
      if (!Slot || !PrivateSlots.contains(Slot))
        continue;
    }
    // A discarded load may make its source slot store-only as well. Weak
    // handles permit repeated candidates without retaining deleted values.
    for (llvm::Value *Operand : Instruction->operand_values())
      Pending.emplace_back(Operand);
    Instruction->eraseFromParent();
  }
}
} // namespace neverd
