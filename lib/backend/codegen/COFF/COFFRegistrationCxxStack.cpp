//===- COFFRegistrationCxxStack.cpp - Catch stack identity ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationCxxIRProof.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/RegistrationFrameAddress.h"

#include "llvm/IR/Dominators.h"

namespace neverd::coff_registration {

static llvm::Error bindCatchStack(CxxIRControlProof &Proof,
                                  const MedFunc &Source,
                                  const llvm::Function &Function,
                                  uint32_t Index) {
  auto &Catch = Proof.Catches[Index];
  const auto &EH = *Source.ExceptionMetadata;
  const bool PrivateStack = EH.Registration->hasCxxCallbackStack();
  const va_t Callback = EH.Cxx->TryBlocks[0].Handlers[Index].HandlerVA;
  size_t Work = 0;
  unsigned Expected = 0;
  for (const auto &Block : Source.Blocks)
    if (Block.StartAddr == Callback)
      for (const auto &Op : Block.Ops) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("C++ callback root replay exceeded its work budget");
        Expected += Op.RegistrationRoot ==
                    MedOp::RegistrationRootKind::CallbackStackPointer;
      }
  const llvm::StoreInst *Seed = nullptr;
  const llvm::AllocaInst *Stack = nullptr;
  for (const auto &Block : Function)
    for (const auto &I : Block) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("C++ callback stack identity exceeded its work budget");
      if (const auto *MD =
              I.getMetadata(windows_eh_md::RegistrationCatchStackAttachment)) {
        const auto Owner = metadataInteger(*MD, 1, 64);
        if (PrivateStack && Owner && *Owner != Callback &&
            llvm::any_of(EH.Cxx->TryBlocks[0].Handlers,
                         [&](const auto &Handler) {
                           return Handler.HandlerVA == *Owner;
                         }))
          continue;
        Stack = llvm::dyn_cast<llvm::AllocaInst>(&I);
        if (!PrivateStack || Catch.Stack || !Stack ||
            MD->getNumOperands() != 2 ||
            metadataInteger(*MD, 0, 64) != Source.Entry ||
            metadataInteger(*MD, 1, 64) != Callback ||
            !Stack->isStaticAlloca() || Stack->getAddressSpace() ||
            Stack->isSwiftError() || Stack->isUsedWithInAlloca() ||
            &Block != &Function.getEntryBlock() || Stack == Proof.Frame.Slot ||
            Stack->getAlign() != llvm::Align(16))
          return rejectIR("C++ callback stack lost its invocation identity");
        Catch.Stack = Stack;
      }
      if (I.getMetadata(windows_eh_md::RegistrationRootAttachment)) {
        if (PrivateStack && &Block != Catch.Pad->getParent() &&
            llvm::any_of(Proof.Catches, [&](const auto &Other) {
              return Other.Pad->getParent() == &Block;
            }))
          continue;
        const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
        if (!PrivateStack || Seed || !Store || Store->isAtomic() ||
            Store->isVolatile() || &Block != Catch.Pad->getParent())
          return rejectIR("C++ callback ESP has no unique runtime definition");
        Seed = Store;
      }
    }
  if (!PrivateStack)
    return llvm::Error::success();
  if (Expected != 1 || !Seed || !Stack)
    return rejectIR("C++ callback stack lost its source ESP binding");
  const auto *Value =
      llvm::dyn_cast<llvm::PtrToIntInst>(Seed->getValueOperand());
  const auto *Address =
      Value
          ? llvm::dyn_cast<llvm::GetElementPtrInst>(Value->getPointerOperand())
          : nullptr;
  const auto *MD = Seed->getMetadata(windows_eh_md::RegistrationRootAttachment);
  const auto Size =
      Stack->getAllocationSize(Function.getParent()->getDataLayout());
  const auto *Destination =
      llvm::dyn_cast<llvm::AllocaInst>(Seed->getPointerOperand());
  if (!Size || Size->isScalable() || !Size->getFixedValue() ||
      Size->getFixedValue() > limits::kMaxRegistrationEHStateWork ||
      MD->getNumOperands() != 2 || metadataInteger(*MD, 0, 8) != 1 ||
      metadataInteger(*MD, 1, 64) != Size->getFixedValue() || !Value ||
      !Value->getType()->isIntegerTy(32) || !Address ||
      Address->getPointerOperand() != Stack ||
      registration_frame::checkedByteGEPOffset(Address) !=
          int64_t(Size->getFixedValue()) ||
      !Destination || !Destination->isStaticAlloca() ||
      Destination->getAddressSpace() ||
      Destination->getParent() != &Function.getEntryBlock() ||
      Destination->getAllocationSize(Function.getParent()->getDataLayout()) !=
          llvm::TypeSize::getFixed(4) ||
      !Destination->getAllocatedType()->isIntegerTy(32))
    return rejectIR("C++ callback ESP changed its private frame address");
  llvm::DominatorTree Dominators(const_cast<llvm::Function &>(Function));
  for (const auto *User : Destination->users()) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return rejectIR("C++ callback ESP uses exceeded their work budget");
    if (User == Seed)
      continue;
    if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User)) {
      const auto *Zero =
          llvm::dyn_cast<llvm::ConstantInt>(Store->getValueOperand());
      if (Store->getParent() == &Function.getEntryBlock() && Zero &&
          Zero->isZero() && Zero->getType()->isIntegerTy(32) &&
          !Store->isVolatile() && !Store->isAtomic() &&
          Store->getPointerOperand() == Destination)
        continue;
    }
    const auto *Load = llvm::dyn_cast<llvm::LoadInst>(User);
    if (!Load || !Catch.Blocks.count(Load->getParent()) || Load->isVolatile() ||
        Load->isAtomic() || !Dominators.dominates(Seed, Load))
      return rejectIR("C++ callback ESP escaped its runtime invocation");
  }
  return llvm::Error::success();
}

llvm::Error bindCxxCatchStack(CxxIRControlProof &Proof, const MedFunc &Source,
                              const llvm::Function &Function) {
  for (uint32_t Index = 0; Index < Proof.Catches.size(); ++Index)
    if (auto Error = bindCatchStack(Proof, Source, Function, Index))
      return Error;
  return llvm::Error::success();
}

} // namespace neverd::coff_registration
