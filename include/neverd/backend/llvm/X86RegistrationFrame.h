//===- X86RegistrationFrame.h - Runtime root lowering -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86REGISTRATIONFRAME_H
#define NEVERD_BACKEND_LLVM_X86REGISTRATIONFRAME_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/IR/IRBuilder.h"

namespace neverd {

/// Lower a checked source runtime root using the logical entry ESP. Callback
/// ESP remains a distinct seed for the native callback outlining owner.
llvm::Value *emitX86RegistrationRoot(const MedFunc &Func, const MedOp &Op,
                                     llvm::Value *EntrySP,
                                     llvm::IRBuilder<> &Builder);

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86REGISTRATIONFRAME_H
