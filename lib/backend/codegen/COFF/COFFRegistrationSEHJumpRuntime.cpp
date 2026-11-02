//===- COFFRegistrationSEHJumpRuntime.cpp - Nonlocal unwind frame proof --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationSEHJump.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/X86/X86RegistrationSEHJump.h"

#include "llvm/IR/IntrinsicInst.h"

namespace neverd::coff_registration {

llvm::Error
validateSEHJumpUnwindBridge(const llvm::Function &Parent,
                            const RegistrationSEHJumpCapture &Source) {
  using namespace x86_registration;
  const auto *Module = Parent.getParent();
  const auto *Runtime =
      Module->getFunction(getSEHSourceJumpUnwindName(Source.Protocol));
  const auto *Bridge =
      Module->getFunction(getSEHJumpUnwindName(Source.Protocol));
  auto HasABI = [](const llvm::Function *F) {
    return F && F->getReturnType()->isVoidTy() && !F->isVarArg() &&
           F->arg_size() == 1 &&
           F->getFunctionType()->getParamType(0)->isPointerTy() &&
           F->getArg(0)->getType()->getPointerAddressSpace() == 0 &&
           F->getCallingConv() == llvm::CallingConv::X86_StdCall &&
           !F->getAttributes().getParamAttrs(0).hasAttributes() &&
           !F->getAttributes().getRetAttrs().hasAttributes() &&
           F->getMemoryEffects() == llvm::MemoryEffects::unknown() &&
           !F->doesNotReturn() && !F->doesNotThrow() &&
           !F->hasFnAttribute(llvm::Attribute::ReturnsTwice) &&
           !F->hasFnAttribute(llvm::Attribute::NoCallback);
  };
  if (!HasABI(Runtime) || !Runtime->isDeclaration() ||
      !Runtime->hasExternalLinkage() || !HasABI(Bridge) ||
      !Bridge->hasLocalLinkage() || Bridge->size() != 1 ||
      !Bridge->hasFnAttribute(llvm::Attribute::NoInline) ||
      !Bridge->hasFnAttribute(llvm::Attribute::OptimizeNone))
    return rejectIR("native jump unwind helper changed its runtime ABI");
  auto Identity = rewrite_source::getOriginalVA(*Runtime);
  if (!Identity)
    return Identity.takeError();
  auto BridgeIdentity = rewrite_source::getOriginalVA(*Bridge);
  if (!BridgeIdentity)
    return BridgeIdentity.takeError();
  if (!*Identity || **Identity != Source.UnwindVA || *BridgeIdentity)
    return rejectIR("native jump unwind helper changed its source identity");

  const auto &Block = Bridge->getEntryBlock();
  if (Block.size() != 8 || Block.hasAddressTaken())
    return rejectIR("native jump unwind bridge gained unproved effects");
  auto It = Block.begin();
  const auto *Copy = llvm::dyn_cast<llvm::AllocaInst>(&*It++);
  const auto *Memcpy = llvm::dyn_cast<llvm::MemCpyInst>(&*It++);
  const auto *RegistrationAddress =
      llvm::dyn_cast<llvm::GetElementPtrInst>(&*It++);
  const auto *Registration = llvm::dyn_cast<llvm::LoadInst>(&*It++);
  const auto *Frame = llvm::dyn_cast<llvm::GetElementPtrInst>(&*It++);
  const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&*It++);
  const auto *Call = llvm::dyn_cast<llvm::CallInst>(&*It++);
  const auto *Return = llvm::dyn_cast<llvm::ReturnInst>(&*It++);
  auto IsOffset = [](const llvm::GetElementPtrInst *GEP,
                     const llvm::Value *Base, uint64_t Offset) {
    return GEP && GEP->getPointerOperand() == Base &&
           GEP->getSourceElementType()->isIntegerTy(8) &&
           GEP->getNumIndices() == 1 &&
           integer(GEP->getOperand(1), 32) == Offset;
  };
  const auto *Type =
      Copy ? llvm::dyn_cast<llvm::ArrayType>(Copy->getAllocatedType())
           : nullptr;
  if (!Copy || !Type || !Copy->isStaticAlloca() || Copy->isArrayAllocation() ||
      Copy->getAddressSpace() || Copy->getAlign() != llvm::Align(4) ||
      !Type->getElementType()->isIntegerTy(8) ||
      Type->getNumElements() != RegistrationSEHJumpCapture::BufferBytes ||
      !Memcpy || Memcpy->getRawDest() != Copy ||
      Memcpy->getRawSource() != Bridge->getArg(0) || Memcpy->isVolatile() ||
      integer(Memcpy->getLength(), 64) !=
          RegistrationSEHJumpCapture::BufferBytes ||
      Memcpy->getDestAlign() != llvm::Align(4) ||
      Memcpy->getSourceAlign() != llvm::Align(4) ||
      !IsOffset(RegistrationAddress, Bridge->getArg(0), 24) || !Registration ||
      Registration->getPointerOperand() != RegistrationAddress ||
      !Registration->getType()->isPointerTy() ||
      Registration->getType()->getPointerAddressSpace() ||
      Registration->isVolatile() || Registration->isAtomic() ||
      Registration->getAlign() != llvm::Align(4) ||
      !IsOffset(Frame, Registration, 16) || !Store ||
      Store->getPointerOperand() != Copy || Store->getValueOperand() != Frame ||
      Store->isVolatile() || Store->isAtomic() ||
      Store->getAlign() != llvm::Align(4) || !Call ||
      Call->getCalledFunction() != Runtime || Call->arg_size() != 1 ||
      Call->getArgOperand(0) != Copy || Call->getNumOperandBundles() ||
      Call->isTailCall() ||
      Call->getCallingConv() != llvm::CallingConv::X86_StdCall ||
      !Call->getAttributes().isEmpty() || !Return || Return->getReturnValue())
    return rejectIR("native jump unwind bridge changed its private frame copy");
  return llvm::Error::success();
}

} // namespace neverd::coff_registration
