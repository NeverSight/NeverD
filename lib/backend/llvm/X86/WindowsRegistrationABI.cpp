//===- WindowsRegistrationABI.cpp - PE32 security checker ABI -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/WindowsRegistrationFrame.h"

#include "llvm/IR/Attributes.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

namespace neverd {

bool hasX86RegistrationSecurityCheckABI(const llvm::Function &Function) {
  const auto *Type = Function.getFunctionType();
  return Function.isDeclaration() && Function.hasExternalLinkage() &&
         Function.getAddressSpace() == 0 &&
         !Function.hasDLLImportStorageClass() &&
         Type->getReturnType()->isVoidTy() && !Type->isVarArg() &&
         Type->getNumParams() == 1 && Type->getParamType(0)->isPointerTy() &&
         Type->getParamType(0)->getPointerAddressSpace() == 0 &&
         Function.getCallingConv() == llvm::CallingConv::X86_FastCall &&
         Function.hasParamAttribute(0, llvm::Attribute::InReg);
}

llvm::Function *createX86RegistrationSecurityCheck(llvm::Module &Module,
                                                   llvm::StringRef Name) {
  auto &Context = Module.getContext();
  auto *Type =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                              llvm::PointerType::getUnqual(Context), false);
  auto *Function = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, Name, Module);
  Function->setCallingConv(llvm::CallingConv::X86_FastCall);
  Function->addParamAttr(0, llvm::Attribute::InReg);
  Function->setDSOLocal(true);
  return Function;
}

llvm::Function *createX86RegistrationSecurityCheck(llvm::Module &Module) {
  return createX86RegistrationSecurityCheck(Module, "__security_check_cookie");
}

} // namespace neverd
