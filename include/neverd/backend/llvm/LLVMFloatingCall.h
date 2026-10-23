//===- LLVMFloatingCall.h - Checked floating call types --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// LLVM carriers of a checked source floating signature. AAPCS64 HFA
/// parameters use arrays: a literal LLVM struct does not preserve their
/// all-or-stack allocation when the floating register bank is exhausted.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_LLVMFLOATINGCALL_H
#define NEVERD_BACKEND_LLVM_LLVMFLOATINGCALL_H

#include "neverd/ir/FloatingCallArguments.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/IR/DerivedTypes.h"

#include <utility>
#include <vector>

namespace neverd {

inline llvm::Type *floatingCallElementType(llvm::LLVMContext &Context,
                                           unsigned Bytes) {
  return Bytes == 4 ? llvm::Type::getFloatTy(Context)
                    : llvm::Type::getDoubleTy(Context);
}

/// The signature has already passed floatingCallArguments or its bound
/// descriptor validator. Preserve record groups even when all are in registers.
inline llvm::FunctionType *
floatingCallType(llvm::LLVMContext &Context,
                 const FloatingCallArguments &Signature) {
  std::vector<llvm::Type *> Scalars;
  for (unsigned Bytes : Signature.ParameterBytes)
    Scalars.push_back(floatingCallElementType(Context, Bytes));
  for (const auto &Parameter : Signature.Scalars)
    Scalars.push_back(
        Parameter.Floating ? floatingCallElementType(Context, Parameter.Bytes)
        : Parameter.Pointer
            ? static_cast<llvm::Type *>(llvm::PointerType::getUnqual(Context))
            : llvm::Type::getIntNTy(Context, Parameter.Bytes * 8));
  std::vector<llvm::Type *> Parameters;
  if (Signature.ParameterElements.empty()) {
    Parameters = std::move(Scalars);
  } else {
    size_t At = 0;
    for (unsigned Count : Signature.ParameterElements) {
      Parameters.push_back(Count ? llvm::ArrayType::get(Scalars[At], Count)
                                 : Scalars[At]);
      At += Count ? Count : 1;
    }
  }
  llvm::Type *Return = floatingCallElementType(Context, Signature.ReturnBytes);
  if (Signature.ReturnElements) {
    llvm::Type *Element = floatingCallElementType(
        Context, Signature.ReturnBytes / Signature.ReturnElements);
    Return = llvm::StructType::get(
        Context, std::vector<llvm::Type *>(Signature.ReturnElements, Element));
  }
  return llvm::FunctionType::get(Return, Parameters, false);
}

/// A raw LLVM parameter array whose C value wrapper has the same AAPCS64 HFA
/// classification, including whole-record stack placement. No nested records,
/// vector lanes, padding or unusual element precision is admitted here.
inline bool isAAPCS64HFAArray(llvm::Type *Type) {
  const auto *Array = llvm::dyn_cast<llvm::ArrayType>(Type);
  return Array && Array->getNumElements() &&
         Array->getNumElements() <=
             getTargetRegInfo(Arch::AArch64).FPReturnRegs.size() &&
         (Array->getElementType()->isFloatTy() ||
          Array->getElementType()->isDoubleTy());
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_LLVMFLOATINGCALL_H
