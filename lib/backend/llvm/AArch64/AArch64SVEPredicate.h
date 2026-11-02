//===- AArch64SVEPredicate.h - Canonical LLVM predicate bridge -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Convert typed predicate results to complete svbool values before storage,
/// and convert back only after reading that canonical representation.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_AARCH64_SVEPREDICATE_H
#define NEVERD_BACKEND_LLVM_AARCH64_SVEPREDICATE_H

#include "neverd/ir/intrinsics/AArch64/AArch64SVEPredicate.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAArch64.h"

namespace neverd::aarch64_sve {

inline llvm::ScalableVectorType *svboolType(llvm::LLVMContext &Context) {
  return llvm::ScalableVectorType::get(llvm::Type::getInt1Ty(Context),
                                       kMinimumPredicateLanes);
}

inline llvm::Value *toSVBool(llvm::IRBuilder<> &Builder, llvm::Value *Value) {
  if (Value->getType() == svboolType(Builder.getContext()))
    return Value;
  auto *Function = llvm::Intrinsic::getOrInsertDeclaration(
      Builder.GetInsertBlock()->getModule(),
      llvm::Intrinsic::aarch64_sve_convert_to_svbool, {Value->getType()});
  return Builder.CreateCall(Function, {Value});
}

inline llvm::Value *fromSVBool(llvm::IRBuilder<> &Builder, llvm::Value *Value,
                               llvm::Type *PredicateType) {
  if (Value->getType() == PredicateType)
    return Value;
  auto *Function = llvm::Intrinsic::getOrInsertDeclaration(
      Builder.GetInsertBlock()->getModule(),
      llvm::Intrinsic::aarch64_sve_convert_from_svbool, {PredicateType});
  return Builder.CreateCall(Function, {Value});
}

} // namespace neverd::aarch64_sve

#endif // NEVERD_BACKEND_LLVM_AARCH64_SVEPREDICATE_H
