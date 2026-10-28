//===- LLVMSourceCallValue.h - Source call value types ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Canonical LLVM value types for validated scalar and direct Darwin record
/// calls. Producers and C projections share the same grouping contract.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_LLVMSOURCECALLVALUE_H
#define NEVERD_BACKEND_LLVM_LLVMSOURCECALLVALUE_H

#include "neverd/ir/SourceABI.h"

#include "llvm/IR/DerivedTypes.h"

#include <vector>

namespace neverd {

inline llvm::Type *llvmSourceCallValueType(llvm::LLVMContext &Context,
                                           const TypeRef &Source,
                                           bool Parameter) {
#define LLVM_SOURCE_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "neverd/backend/llvm/LLVMSourceCallValue.def"
  switch (Source->Kind) {
  case NdTypeKind::Void:
    return Parameter ? nullptr : llvm::Type::getVoidTy(Context);
  case NdTypeKind::Int:
    return llvm::Type::getIntNTy(Context, Source->Size * kSourceValueByteBits);
  case NdTypeKind::Ptr:
    return llvm::PointerType::getUnqual(Context);
  case NdTypeKind::Float:
    return Source->Size == kSourceValueFloatBytes
               ? llvm::Type::getFloatTy(Context)
               : llvm::Type::getDoubleTy(Context);
  case NdTypeKind::Struct: {
    const auto Members = sourceAggregateMembers(Source);
    const bool Floating = Members.front().Type->Kind == NdTypeKind::Float;
    auto *Element =
        Floating ? llvmSourceCallValueType(Context, Members.front().Type, true)
                 : llvm::Type::getInt64Ty(Context);
    const unsigned Count = Floating
                               ? Members.size()
                               : (Source->Size + kSourceValueWordBytes - 1) /
                                     kSourceValueWordBytes;
    return Parameter
               ? static_cast<llvm::Type *>(llvm::ArrayType::get(Element, Count))
               : llvm::StructType::get(
                     Context, std::vector<llvm::Type *>(Count, Element));
  }
  default:
    return nullptr;
  }
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_LLVMSOURCECALLVALUE_H
