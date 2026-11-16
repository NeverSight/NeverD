//===- LLVMIntegerResult.h - MIPS source integer return attributes ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_MIPS_LLVMINTEGERRESULT_H
#define NEVERD_BACKEND_LLVM_MIPS_LLVMINTEGERRESULT_H

#include "neverd/backend/llvm/LLVMSourceTypeHints.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/IR/Attributes.h"

namespace neverd::mips {
inline llvm::AttributeList integerResultAttributes(llvm::LLVMContext &C,
                                                   llvm::AttributeList Attrs,
                                                   unsigned Bytes, bool Signed,
                                                   Arch A) {
  if (Bytes < getTargetRegInfo(A).FullRegWidth)
    Attrs =
        Attrs.addRetAttribute(C, Signed || Bytes == 4 ? llvm::Attribute::SExt
                                                      : llvm::Attribute::ZExt);
  return Signed ? Attrs.addFnAttribute(C, LLVMSourceSignedResult) : Attrs;
}
} // namespace neverd::mips

#endif
