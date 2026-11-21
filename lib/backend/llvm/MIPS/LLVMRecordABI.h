//===- LLVMRecordABI.h - MIPS C record call attributes ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_LLVM_MIPS_LLVMRECORDABI_H
#define NEVERD_LIB_BACKEND_LLVM_MIPS_LLVMRECORDABI_H

#include "LLVMIntegerResult.h"

#include "neverd/ir/SourceABI.h"

namespace neverd::mips {

inline llvm::AttributeList
recordCallAttributes(llvm::LLVMContext &Context, llvm::AttributeList Attrs,
                     const SourceFunctionTypeHint &Signature,
                     llvm::ArrayRef<SourceABIParameter> Pieces) {
  Attrs = Attrs.addFnAttribute(Context, LLVMMIPSRecordCall);
  if (!Signature.ReturnComponents.empty())
    Attrs = Attrs.addRetAttribute(Context, llvm::Attribute::InReg);
  else if (Signature.ReturnType->Kind == NdTypeKind::Int)
    Attrs = integerResultAttributes(Context, Attrs, Signature.ReturnType->Size,
                                    Signature.ReturnType->IsSigned,
                                    Signature.Architecture);
  for (unsigned I = 0; I < Pieces.size(); ++I) {
    const auto &P = Pieces[I];
    if (!Signature.Parameters[P.ParameterIndex].Components.empty())
      Attrs = Attrs.addParamAttribute(Context, I, llvm::Attribute::InReg);
    else if (P.Type->Kind == NdTypeKind::Int) {
      if (P.Type->IsSigned)
        Attrs = Attrs.addParamAttribute(Context, I, LLVMSourceSignedParameter);
      Attrs = Attrs.addParamAttribute(Context, I,
                                      P.Type->IsSigned || P.Type->Size == 4
                                          ? llvm::Attribute::SExt
                                          : llvm::Attribute::ZExt);
    }
  }
  return Attrs;
}
} // namespace neverd::mips
#endif
