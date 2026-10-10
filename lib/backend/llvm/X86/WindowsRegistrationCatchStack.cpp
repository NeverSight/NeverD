//===- WindowsRegistrationCatchStack.cpp - PE32 catch scratch bounds ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "WindowsRegistrationFramePrivate.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/X86RegistrationCatchStack.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

namespace neverd {

llvm::Expected<uint32_t> checkX86RegistrationCatchStack(
    llvm::BasicBlock &Entry, llvm::ArrayRef<llvm::BasicBlock *> Blocks,
    llvm::StoreInst &Seed, llvm::ArrayRef<llvm::StoreInst *> SourceStores) {
  using namespace x86_registration;
  if (Blocks.size() > limits::kMaxRegistrationEHRecords ||
      SourceStores.size() > limits::kMaxRegistrationEHStateWork ||
      Seed.getParent() != &Entry || Seed.isAtomic() || Seed.isVolatile() ||
      !Seed.getValueOperand()->getType()->isIntegerTy(32))
    return reject("catch ESP has no exact scalar entry definition");
  auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(Seed.getPointerOperand());
  if (!Slot || !isStaticEntryAlloca(*Slot, *Entry.getParent()) ||
      !Slot->getAllocatedType()->isIntegerTy(32) ||
      !llvm::cast<llvm::ConstantInt>(Slot->getArraySize())->isOne())
    return reject("catch ESP has no private SSA destination");
  std::set<llvm::BasicBlock *> Body(Blocks.begin(), Blocks.end()), Reached;
  if (Body.size() != Blocks.size() || !Body.count(&Entry))
    return reject("catch stack has no unique callback body");
  size_t Work = 0;
  std::vector<llvm::BasicBlock *> Pending{&Entry};
  while (!Pending.empty()) {
    auto *Block = Pending.back();
    Pending.pop_back();
    if (!Reached.insert(Block).second)
      continue;
    if (++Work > limits::kMaxRegistrationEHStateWork || !Body.count(Block) ||
        Block->getParent() != Entry.getParent() || !Block->getTerminator())
      return reject("catch stack control flow is not closed");
    for (auto *Pred : llvm::predecessors(Block))
      if (!Body.count(Pred))
        return reject("ordinary flow enters the catch stack");
    for (auto *Succ : llvm::successors(Block))
      Pending.push_back(Succ);
  }
  if (Reached != Body)
    return reject("catch stack has unreachable body blocks");
  for (auto *User : Slot->users()) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return reject("catch ESP uses exceed the work budget");
    if (User == &Seed || isPrivateSlotInitializer(User, *Slot))
      continue;
    auto *Load = llvm::dyn_cast<llvm::LoadInst>(User);
    if (!Load || !Body.count(Load->getParent()) || Load->isAtomic() ||
        Load->isVolatile() || Load->getType() != Slot->getAllocatedType() ||
        (Load->getParent() == &Entry && !Seed.comesBefore(Load)))
      return reject("catch ESP seed does not dominate its private uses");
  }
  std::set<llvm::StoreInst *> Bridges;
  for (auto *Store : SourceStores)
    if (!Store || !Body.count(Store->getParent()) || Store->isAtomic() ||
        !Bridges.insert(Store).second)
      return reject("catch stack bridge has no exact source store");
  X86RegistrationCallbackFrame Frame;
  Frame.InferStackBounds = true;
  Frame.StackBytes = limits::kMaxRegistrationEHStateWork;
  Frame.StackPointerOffset = Frame.StackBytes;
  std::set<llvm::AllocaInst *> PrivateSlots{Slot};
  if (auto Error =
          checkPrivateStack(Entry.getModule()->getDataLayout(), Entry, Body,
                            {{&Seed, X86RegistrationRootKind::StackPointer}},
                            Frame, PrivateSlots, Work, &Bridges))
    return std::move(Error);
  return Frame.StackBytes;
}

} // namespace neverd
