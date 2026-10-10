//===- X86RegistrationFrame.h - Runtime root lowering -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86REGISTRATIONFRAME_H
#define NEVERD_BACKEND_LLVM_X86REGISTRATIONFRAME_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/IR/IRBuilder.h"

#include <utility>

namespace neverd {

/// Reserve the checked aligned allocation and entry-save area. Returns the
/// entry-SP offset and allocation alignment; headroom is added by the caller.
std::pair<uint64_t, uint64_t> x86RegistrationFrameStorage(const MedFunc &Func,
                                                          uint64_t EntrySP,
                                                          uint64_t Alignment);

/// Lower a checked source runtime root using the logical entry ESP. Callback
/// ESP remains a distinct seed for the native callback outlining owner.
llvm::Value *emitX86RegistrationRoot(const MedFunc &Func, const MedOp &Op,
                                     llvm::Value *EntrySP,
                                     llvm::IRBuilder<> &Builder);

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86REGISTRATIONFRAME_H
