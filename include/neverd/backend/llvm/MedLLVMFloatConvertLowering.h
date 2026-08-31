//===- MedLLVMFloatConvertLowering.h --------------------------*- C++ -*-===//

#ifndef NEVERD_BACKEND_LLVM_MEDLLVMFLOATCONVERTLOWERING_H
#define NEVERD_BACKEND_LLVM_MEDLLVMFLOATCONVERTLOWERING_H

#include "neverd/Common.h"

namespace llvm {
class IRBuilderBase;
class Module;
class Type;
class Value;
} // namespace llvm

namespace neverd::llvm_detail {

llvm::Value *emitFPToInt(llvm::IRBuilderBase &Builder, llvm::Module &Module,
                         llvm::Value *Source, llvm::Type *DestType,
                         bool IsUnsigned, Arch TargetArch);

llvm::Value *emitIntToFP(llvm::IRBuilderBase &Builder, llvm::Value *Source,
                         llvm::Type *DestType, bool IsUnsigned,
                         Arch TargetArch);

} // namespace neverd::llvm_detail

#endif
