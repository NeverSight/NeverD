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

#include "neverd/ir/MIPS/SourceRecordABI.h"
#include "neverd/ir/SourceABI.h"

#include "llvm/ADT/SmallVector.h"
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
    const auto Members = sourceComplexComponentBytes(Source)
                             ? sourceValueMembers(Source)
                             : sourceAggregateMembers(Source);
    if (Members.empty())
      return nullptr;
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

struct LLVMMIPSRecordArgument {
  llvm::Type *Type;
  unsigned FirstPiece;
  /// Zero for an unused o32 word inserted before an aligned aggregate.
  unsigned PieceCount;
  bool InReg;
};

/// Clang coerces each record word into a separate inreg argument. An aligned
/// o32 scalar pair instead occupies a single LLVM argument of its source type.
inline llvm::SmallVector<LLVMMIPSRecordArgument, 8>
llvmMIPSRecordArguments(llvm::LLVMContext &Context,
                        const SourceFunctionTypeHint &Hint,
                        llvm::ArrayRef<SourceABIParameter> Pieces) {
  llvm::SmallVector<LLVMMIPSRecordArgument, 8> Result;
  unsigned O32Slot =
      Hint.ReturnLocation.Kind == SourceABICarrierKind::IndirectResultPointer;
  for (unsigned I = 0; I < Pieces.size();) {
    const size_t Parameter = Pieces[I].ParameterIndex;
    const auto &P = Hint.Parameters[Parameter];
    if (isMIPSO32(Hint.Architecture) &&
        (!I || Pieces[I - 1].ParameterIndex != Parameter)) {
      const unsigned Start = mips::alignedO32ArgumentSlot(O32Slot, P.Type);
      // A scalar i64/double retains its natural LLVM alignment. Flattening a
      // record into inreg i32 operands removes that alignment from the type.
      if (Start != O32Slot && P.Type->Kind == NdTypeKind::Struct)
        Result.push_back({llvm::Type::getInt32Ty(Context), I, 0, false});
      O32Slot = Start + (P.Type->Size + 3u) / 4u;
    }
    if (mips::isSplitRecordScalar(P)) {
      Result.push_back(
          {llvmSourceCallValueType(Context, P.Type, true), I, 2, false});
      I += 2;
    } else {
      Result.push_back({llvmMIPSRecordPieceType(Context, Pieces[I], Hint), I, 1,
                        !P.Components.empty()});
      ++I;
    }
  }
  return Result;
}

/// A MIPS record result has one LLVM element for each physical register.
/// Nested floating members and pointers may return through the GPR bank.
inline llvm::Type *
llvmSourceCallResultType(llvm::LLVMContext &Context,
                         const SourceFunctionTypeHint &Hint) {
  if (Hint.MIPSRecords &&
      Hint.ReturnLocation.Kind == SourceABICarrierKind::IndirectResultPointer)
    return llvm::Type::getVoidTy(Context);
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
