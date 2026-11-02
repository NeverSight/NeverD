//===- X86X87ImportCall.h - Native CRT x87 call adapter -----------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_LLVM_X86_X86X87IMPORTCALL_H
#define NEVERD_BACKEND_LLVM_X86_X86X87IMPORTCALL_H

#include "llvm/Support/Error.h"

namespace llvm {
class Function;
}

namespace neverd {
/// Build a private adapter with binary80 operands followed by the actual native
/// target pointer. Runtime supplies the checked external identity and ABI.
llvm::Expected<llvm::Function *>
createX87ImportCallAdapter(llvm::Function &Runtime, unsigned Arguments);
} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86X87IMPORTCALL_H
