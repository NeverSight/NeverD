//===- LLVMWin64CallStackProvider.h - CALL input signature ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Identify the source projection's original Win64 CALL stack word provider.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_LLVMWIN64CALLSTACKPROVIDER_H
#define NEVERD_BACKEND_LLVM_X86_LLVMWIN64CALLSTACKPROVIDER_H

#include "llvm/IR/Function.h"

namespace neverd::llvm_win64_call_stack {
inline constexpr llvm::StringLiteral Name("__neverd_source_win64_call_stack");
inline constexpr llvm::StringLiteral Attribute("neverd.win64.call-stack-input");

inline bool isProvider(const llvm::Function &F) {
  return F.isDeclaration() && F.getName().starts_with(Name) &&
         F.hasFnAttribute(Attribute) && !F.isVarArg() && F.arg_size() == 2 &&
         F.getReturnType()->isIntegerTy(64) &&
         F.getArg(0)->getType()->isIntegerTy(64) &&
         F.getArg(1)->getType()->isIntegerTy(32);
}
} // namespace neverd::llvm_win64_call_stack

#endif // NEVERD_BACKEND_LLVM_X86_LLVMWIN64CALLSTACKPROVIDER_H
