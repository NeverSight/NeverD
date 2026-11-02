//===- COFFRegistrationCopySlots.cpp - Recovered scalar storage ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationCopySlots.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/X86/X86RegistrationSEHJump.h"

#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

namespace neverd::coff_registration {

bool RegistrationCopySlots::build(
    const llvm::Function &Parent,
    llvm::ArrayRef<const llvm::Function *> Callbacks, size_t &Work) {
  if (Parent.empty() || Escape || !Aliases.empty() || !Recovered.empty())
    return false;
  for (const auto &Block : Parent)
    for (const auto &I : Block) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
      if (!Call || Call->getIntrinsicID() != llvm::Intrinsic::localescape)
        continue;
      if (Escape || &Block != &Parent.getEntryBlock())
        return false;
      Escape = Call;
      for (const llvm::Value *Argument : Call->args()) {
        const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(Argument);
        if (++Work > limits::kMaxRegistrationEHStateWork || !Slot ||
            !Slot->isStaticAlloca() ||
            Slot->getParent() != &Parent.getEntryBlock() ||
            Slot->getAddressSpace() || Slot->isUsedWithInAlloca())
          return false;
        Aliases.try_emplace(Slot, std::vector<const llvm::Value *>{Slot});
      }
    }
  std::map<const llvm::Function *, const llvm::Value *> Frames;
  for (const auto *Callback : Callbacks) {
    if (++Work > limits::kMaxRegistrationEHStateWork || !Callback ||
        Callback == &Parent || Callback->getParent() != Parent.getParent())
      return false;
    const auto *Frame = getX86RegistrationSEHJumpParentFrame(*Callback, Parent);
    if (!Frame || !Frames.emplace(Callback, Frame).second)
      return false;
  }
  // Include every recovery of this parent's storage, so an unregistered alias
  // cannot hide an observer from the private-slot or frozen-value proof.
  for (const auto &Function : *Parent.getParent())
    for (const auto &Block : Function)
      for (const auto &I : Block) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return false;
        const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
        if (!Call || Call->getIntrinsicID() != llvm::Intrinsic::localrecover ||
            Call->arg_size() != 3 || Call->getArgOperand(0) != &Parent)
          continue;
        const auto Frame = Frames.find(&Function);
        const auto *Index =
            llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(2));
        if (!Escape || Frame == Frames.end() || !Index ||
            !Index->getType()->isIntegerTy(32) ||
            Index->getValue().uge(Escape->arg_size()) ||
            Call->getArgOperand(1) != Frame->second ||
            &Block != &Function.getEntryBlock())
          return false;
        const auto *Slot = llvm::cast<llvm::AllocaInst>(
            Escape->getArgOperand(Index->getZExtValue()));
        Recovered.emplace(Call, Slot);
        Aliases.at(Slot).push_back(Call);
      }
  return true;
}

const llvm::AllocaInst *
RegistrationCopySlots::allocation(const llvm::Value *Value) const {
  if (const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(Value))
    return Slot;
  const auto Found = Recovered.find(Value);
  return Found == Recovered.end() ? nullptr : Found->second;
}

bool RegistrationCopySlots::isStorage(const llvm::Value *Value) const {
  return allocation(Value) != nullptr;
}

bool RegistrationCopySlots::isEscape(const llvm::Value *Value) const {
  return Escape && Value == Escape;
}

llvm::SmallVector<const llvm::Value *, 4>
RegistrationCopySlots::aliases(const llvm::Value *Value) const {
  const auto Found = Aliases.find(allocation(Value));
  if (Found == Aliases.end())
    return {Value};
  return {Found->second.begin(), Found->second.end()};
}

bool RegistrationCopySlots::isPrivate(const llvm::Value *Value,
                                      const llvm::Type *Type, size_t &Work,
                                      bool AllowVolatile) const {
  const auto *Slot = allocation(Value);
  if (!Slot || !Slot->isStaticAlloca() || !Type->isIntegerTy() ||
      Type->getIntegerBitWidth() > 64 || Slot->getAllocatedType() != Type ||
      Slot->getAddressSpace() || Slot->isUsedWithInAlloca())
    return false;
  for (const auto *Alias : aliases(Value))
    for (const auto *User : Alias->users()) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      if (Alias == Slot && Aliases.count(Slot) && isEscape(User))
        continue;
      if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(User)) {
        if (Load->getPointerOperand() != Alias || Load->getType() != Type ||
            (Load->isVolatile() && !AllowVolatile) || Load->isAtomic())
          return false;
      } else if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User)) {
        if (Store->getPointerOperand() != Alias ||
            Store->getValueOperand()->getType() != Type ||
            (Store->isVolatile() && !AllowVolatile) || Store->isAtomic())
          return false;
      } else
        return false;
    }
  return true;
}
} // namespace neverd::coff_registration
