//===- MedLLVMDeadSlots.cpp - Discard unobserved private SSA slots
//---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMDeadSlots.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Transforms/Utils/Local.h"

namespace neverd {
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
