//===- X86SgxNative.h - Native SGX interface names -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Match the full natural-width register and compact CF/ZF native interface.
/// Leaf dependencies and preserved flags remain owned by the shared lifter.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86SGXNATIVE_H
#define NEVERD_BACKEND_LLVM_X86_X86SGXNATIVE_H

#include "neverd/ir/intrinsics/X86/X86Sgx.h"

#include <string>
#include <string_view>

namespace neverd {
inline std::string x86SgxNativeName(Intrinsic Id, Arch A) {
  if (!isX86SgxIntrinsic(Id) || (A != Arch::X86 && A != Arch::X64))
    return {};
  return std::string(Id == Intrinsic::X86Encls ? "llvm.x86.encls"
                                               : "llvm.x86.enclv") +
         (A == Arch::X86 ? "32" : "");
}
inline bool isX86SgxNativeName(std::string_view Name) {
  return Name == "llvm.x86.encls" || Name == "llvm.x86.enclv" ||
         Name == "llvm.x86.encls32" || Name == "llvm.x86.enclv32";
}
} // namespace neverd
#endif // NEVERD_BACKEND_LLVM_X86_X86SGXNATIVE_H
