//===- RegistrationSEHJumpStack.cpp - Callback jump arguments -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/X86/X86RegistrationSEHJump.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

namespace neverd {

std::optional<X86RegistrationSEHJumpStack>
getX86RegistrationSEHJumpStack(const llvm::Function &Callback) {
  std::optional<X86RegistrationSEHJumpStack> Result;
  const auto &Layout = Callback.getParent()->getDataLayout();
  size_t Work = 0;
  for (const auto &Block : Callback)
    for (const auto &I : Block) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      const auto *Marker =
          I.getMetadata(windows_eh_md::RegistrationRootAttachment);
      if (!Marker)
        continue;
      if (Marker->getNumOperands() != 2)
        return std::nullopt;
      const auto *Kind =
          llvm::mdconst::dyn_extract<llvm::ConstantInt>(Marker->getOperand(0));
      if (!Kind || !Kind->getType()->isIntegerTy(8))
        return std::nullopt;
      if (Kind->getZExtValue() != 1)
        continue;
      const auto *Offset =
          llvm::mdconst::dyn_extract<llvm::ConstantInt>(Marker->getOperand(1));
      const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
      const auto *Value =
          Store ? llvm::dyn_cast<llvm::PtrToIntInst>(Store->getValueOperand())
                : nullptr;
      const auto *Top = Value ? llvm::dyn_cast<llvm::GetElementPtrInst>(
                                    Value->getPointerOperand())
                              : nullptr;
      const auto *Slot =
          Top ? llvm::dyn_cast<llvm::AllocaInst>(Top->getPointerOperand())
              : nullptr;
      llvm::APInt Actual(32, 0);
      const auto Bytes = Slot ? Slot->getAllocationSize(Layout) : std::nullopt;
      if (Result || !Offset || !Offset->getType()->isIntegerTy(64) || !Value ||
          !Value->getType()->isIntegerTy(32) || !Top ||
          !Top->accumulateConstantOffset(Layout, Actual) || !Slot ||
          Slot->getFunction() != &Callback || !Slot->isStaticAlloca() ||
          Slot->getAddressSpace() || !Bytes || Bytes->isScalable() ||
          Offset->getZExtValue() != Actual.getZExtValue() ||
          !Offset->getZExtValue() ||
          Offset->getZExtValue() > Bytes->getFixedValue())
        return std::nullopt;
      Result = X86RegistrationSEHJumpStack{Slot, Offset->getZExtValue()};
    }
  return Result;
}

} // namespace neverd
