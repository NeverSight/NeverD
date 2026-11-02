//===- RegistrationSEHJumpCallbackFrame.cpp - Nonlocal callback owner -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/backend/llvm/X86/X86RegistrationSEHJump.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicInst.h"

namespace neverd {

const llvm::Value *
getX86RegistrationSEHJumpParentFrame(const llvm::Function &Callback,
                                     const llvm::Function &Parent) {
  if (Callback.arg_size() == 2 && Callback.getReturnType()->isVoidTy() &&
      Callback.getArg(0)->getType()->isIntegerTy(8) &&
      Callback.getArg(1)->getType()->isPointerTy())
    return Callback.getArg(1);
  if (Callback.arg_size() || !Callback.getReturnType()->isIntegerTy(32) ||
      Callback.empty())
    return nullptr;
  const llvm::IntrinsicInst *Result = nullptr;
  size_t Work = 0;
  for (const auto &I : Callback.getEntryBlock()) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return nullptr;
    const auto *Recover = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
    if (!Recover || Recover->getIntrinsicID() != llvm::Intrinsic::eh_recoverfp)
      continue;
    const auto *Runtime =
        Recover->arg_size() == 2
            ? llvm::dyn_cast<llvm::IntrinsicInst>(Recover->getArgOperand(1))
            : nullptr;
    const auto *Depth =
        Runtime && Runtime->arg_size() == 1
            ? llvm::dyn_cast<llvm::ConstantInt>(Runtime->getArgOperand(0))
            : nullptr;
    if (Result || !Runtime || Recover->getArgOperand(0) != &Parent ||
        Runtime->getIntrinsicID() != llvm::Intrinsic::frameaddress || !Depth ||
        !Depth->equalsInt(1) ||
        Runtime->getParent() != &Callback.getEntryBlock() ||
        !Runtime->comesBefore(Recover))
      return nullptr;
    Result = Recover;
  }
  return Result;
}

} // namespace neverd
