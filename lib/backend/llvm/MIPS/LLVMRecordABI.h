//===- LLVMRecordABI.h - MIPS C record call attributes ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_LLVM_MIPS_LLVMRECORDABI_H
#define NEVERD_LIB_BACKEND_LLVM_MIPS_LLVMRECORDABI_H

#include "LLVMIntegerResult.h"

#include "neverd/backend/llvm/LLVMSourceCallValue.h"
#include "neverd/ir/SourceABI.h"

#include "llvm/IR/DerivedTypes.h"

namespace neverd::mips {

inline llvm::AttributeList
recordCallAttributes(llvm::LLVMContext &Context, llvm::AttributeList Attrs,
                     const SourceFunctionTypeHint &Signature,
                     llvm::ArrayRef<SourceABIParameter> Pieces) {
  Attrs = Attrs.addFnAttribute(Context, LLVMMIPSRecordCall);
  const bool Indirect = Signature.ReturnLocation.Kind ==
                        SourceABICarrierKind::IndirectResultPointer;
  if (Indirect) {
    auto *Object = llvm::ArrayType::get(llvm::Type::getInt8Ty(Context),
                                        Signature.ReturnType->Size);
    Attrs = Attrs.addParamAttribute(
        Context, 0, llvm::Attribute::getWithStructRetType(Context, Object));
    Attrs = Attrs.addParamAttribute(
        Context, 0,
        llvm::Attribute::getWithAlignment(
            Context, llvm::Align(Signature.ReturnType->Alignment)));
  } else if (!Signature.ReturnComponents.empty())
    Attrs = Attrs.addRetAttribute(Context, llvm::Attribute::InReg);
  else if (Signature.ReturnType->Kind == NdTypeKind::Int)
    Attrs = integerResultAttributes(Context, Attrs, Signature.ReturnType->Size,
                                    Signature.ReturnType->IsSigned,
                                    Signature.Architecture);
  const auto Arguments = llvmMIPSRecordArguments(Context, Signature, Pieces);
  for (const auto &[I, Argument] : llvm::enumerate(Arguments)) {
    if (!Argument.PieceCount)
      continue;
    const auto &P =
        Signature.Parameters[Pieces[Argument.FirstPiece].ParameterIndex];
    const unsigned Index = I + unsigned(Indirect);
    if (Argument.InReg)
      Attrs = Attrs.addParamAttribute(Context, Index, llvm::Attribute::InReg);
    else if (P.Type->Kind == NdTypeKind::Int) {
      if (P.Type->IsSigned)
        Attrs =
            Attrs.addParamAttribute(Context, Index, LLVMSourceSignedParameter);
      Attrs = Attrs.addParamAttribute(Context, Index,
                                      P.Type->IsSigned || P.Type->Size == 4
                                          ? llvm::Attribute::SExt
                                          : llvm::Attribute::ZExt);
    }
  }
  return Attrs;
}
} // namespace neverd::mips
#endif
