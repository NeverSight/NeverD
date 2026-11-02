//===- LLVMX87ValueAsm.h - Atomic x87 value and status ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared LLVM and LLVM-to-C constraints for a native value instruction
/// returning its result and status before any compiler-generated spill.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_LLVMX87VALUEASM_H
#define NEVERD_BACKEND_LLVM_X86_LLVMX87VALUEASM_H

#include "llvm/ADT/StringRef.h"

namespace neverd {

inline llvm::StringRef x87ValueStatusConstraints(bool TwoOperands,
                                                 bool PopsST1 = false) {
  if (!TwoOperands)
    return "=&{st},={ax},0,~{dirflag},~{fpsr},~{flags}";
  return PopsST1 ? "=&{st},={ax},0,{st(1)},~{st(1)},~{dirflag},~{fpsr},~{flags}"
                 : "=&{st},={ax},0,{st(1)},~{dirflag},~{fpsr},~{flags}";
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_LLVMX87VALUEASM_H
