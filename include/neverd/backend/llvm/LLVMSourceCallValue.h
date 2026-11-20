//===- LLVMSourceCallValue.h - Source call value types ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Canonical LLVM value types for validated scalar and direct Darwin record
/// calls, and flat MIPS record carriers. Producers and C projections share
/// the corresponding target grouping contract.
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
  if (isSourceAArch64VectorType(Source))
    return llvm::FixedVectorType::get(llvm::Type::getInt8Ty(Context),
                                      Source->Size);
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
    const auto Members = sourceValueMembers(Source);
    const bool Floating = Members.front().Type->Kind == NdTypeKind::Float ||
                          isSourceAArch64VectorType(Members.front().Type);
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

/// On n32/n64 each direct record argument contributes one LLVM scalar per
/// ABI piece. An integer-class field travels through an integer even when
/// the source member is a floating value or a pointer.
inline llvm::Type *llvmMIPSRecordPieceType(llvm::LLVMContext &Context,
                                           const SourceABIParameter &Piece,
                                           const SourceFunctionTypeHint &Hint) {
  if (Hint.Parameters[Piece.ParameterIndex].Components.empty())
    return llvmSourceCallValueType(Context, Piece.Type, true);
  if (Piece.Location.Kind == SourceABICarrierKind::FloatingRegister ||
      (Piece.Location.Kind == SourceABICarrierKind::Stack &&
       Piece.Type->Kind == NdTypeKind::Float && Piece.Type->Size == 8))
    return llvmSourceCallValueType(Context, Piece.Type, true);
  return llvm::IntegerType::get(Context, Piece.Location.ValueBytes * 8);
}

/// A MIPS record result has one LLVM element for each physical register.
/// Nested floating members and pointers may return through the GPR bank.
inline llvm::Type *
llvmSourceCallResultType(llvm::LLVMContext &Context,
                         const SourceFunctionTypeHint &Hint) {
  if (!Hint.MIPSRecords || Hint.ReturnComponents.empty())
    return llvmSourceCallValueType(Context, Hint.ReturnType, false);
  std::vector<llvm::Type *> Fields;
  for (const auto &Piece : Hint.ReturnComponents) {
    if (Piece.Kind == SourceABICarrierKind::FloatingRegister)
      Fields.push_back(Piece.ValueBytes == 4
                           ? llvm::Type::getFloatTy(Context)
                           : llvm::Type::getDoubleTy(Context));
    else
      Fields.push_back(llvm::IntegerType::get(Context, Piece.ValueBytes * 8));
  }
  return llvm::StructType::get(Context, Fields);
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_LLVMSOURCECALLVALUE_H
