//===- LLVMX86SHA.h - Native SHA vector boundaries ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Checks the LLVM SHA-256 value contract before using its native C API.

#ifndef NEVERD_BACKEND_LLVM_X86_LLVMX86SHA_H
#define NEVERD_BACKEND_LLVM_X86_LLVMX86SHA_H

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsX86.h"

namespace neverd {

inline const char *llvmX86SHA256CName(const llvm::CallBase &Call) {
  const auto *Callee = Call.getCalledFunction();
  if (!Callee || Call.hasOperandBundles() ||
      Call.getCallingConv() != llvm::CallingConv::C ||
      Call.getFunctionType() != Callee->getFunctionType())
    return nullptr;
  const char *Name = nullptr;
  unsigned Arguments = 2;
  switch (Callee->getIntrinsicID()) {
  case llvm::Intrinsic::x86_sha256msg1:
    Name = "_mm_sha256msg1_epu32";
    break;
  case llvm::Intrinsic::x86_sha256msg2:
    Name = "_mm_sha256msg2_epu32";
    break;
  case llvm::Intrinsic::x86_sha256rnds2:
    Name = "_mm_sha256rnds2_epu32";
    Arguments = 3;
    break;
  default:
    return nullptr;
  }
  auto *Vector =
      llvm::FixedVectorType::get(llvm::Type::getInt32Ty(Call.getContext()), 4);
  if (Call.arg_size() != Arguments || Call.getType() != Vector)
    return nullptr;
  for (const auto &Argument : Call.args())
    if (Argument->getType() != Vector)
      return nullptr;
  return Name;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_LLVMX86SHA_H
