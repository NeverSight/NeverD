//===- LLVMAArch64AuthenticatedCall.h - Call operand bundles ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate an LLVM authenticated call before source normalization or output.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_AARCH64_LLVMAARCH64AUTHENTICATEDCALL_H
#define NEVERD_BACKEND_LLVM_AARCH64_LLVMAARCH64AUTHENTICATEDCALL_H

#include "neverd/Common.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

#include <optional>
#include <stdexcept>

namespace neverd {

struct LLVMAArch64AuthenticatedCall {
  unsigned Key;
  const llvm::Value *Modifier;
};

inline std::optional<LLVMAArch64AuthenticatedCall>
llvmAArch64AuthenticatedCall(const llvm::CallBase &Call, Arch TargetArch) {
  std::optional<llvm::OperandBundleUse> Bundle;
  for (unsigned I = 0; I < Call.getNumOperandBundles(); ++I) {
    auto Candidate = Call.getOperandBundleAt(I);
    if (Candidate.getTagName() != kAArch64PAuthCallBundle)
      continue;
    if (Bundle)
      throw std::invalid_argument(kAArch64PAuthMalformedCall);
    Bundle = Candidate;
  }
  if (!Bundle)
    return std::nullopt;
  if (TargetArch != Arch::AArch64 || Call.getNumOperandBundles() != 1 ||
      Bundle->Inputs.size() != 2 || !Call.isIndirectCall() ||
      Call.isInlineAsm() ||
      !Call.getCalledOperand()->getType()->isPointerTy() ||
      Call.getCalledOperand()->getType()->getPointerAddressSpace() != 0)
    throw std::invalid_argument(kAArch64PAuthMalformedCall);
  if (const auto *Instruction = llvm::dyn_cast<llvm::CallInst>(&Call);
      Instruction && Instruction->isMustTailCall())
    throw std::invalid_argument(kAArch64PAuthMalformedCall);
  const auto *Key = llvm::dyn_cast<llvm::ConstantInt>(Bundle->Inputs[0]);
  if (!Key || !Key->getType()->isIntegerTy(kAArch64PAuthKeyBytes * 8) ||
      Key->getZExtValue() > kAArch64PAuthKeyIB ||
      !Bundle->Inputs[1]->getType()->isIntegerTy(kAArch64PAuthPointerBytes * 8))
    throw std::invalid_argument(kAArch64PAuthMalformedCall);
  return LLVMAArch64AuthenticatedCall{
      static_cast<unsigned>(Key->getZExtValue()), Bundle->Inputs[1]};
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_AARCH64_LLVMAARCH64AUTHENTICATEDCALL_H
