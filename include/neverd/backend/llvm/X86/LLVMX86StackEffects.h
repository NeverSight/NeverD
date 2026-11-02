//===- LLVMX86StackEffects.h - Inline assembly stack footprint -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_LLVMX86STACKEFFECTS_H
#define NEVERD_BACKEND_LLVM_X86_LLVMX86STACKEFFECTS_H

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"

namespace neverd {

inline constexpr char X86StackTemporaryMetadata[] =
    "neverd.x86.stack-temporary";

/// PUSHF/POPF assembly borrows memory below the compiler's stack pointer.
/// Keep that footprint on the instruction: function attributes alone do not
/// follow its body into a caller when LLVM inlines it.
inline void markX86StackTemporary(llvm::CallInst &Call) {
  Call.setMetadata(X86StackTemporaryMetadata,
                   llvm::MDNode::get(Call.getContext(), {}));
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_LLVMX86STACKEFFECTS_H
