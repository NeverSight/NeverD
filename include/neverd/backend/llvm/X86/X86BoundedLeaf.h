//===- X86BoundedLeaf.h - Native bounded leaf interfaces -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One indivisible restore/instruction/capture window, with explicit EFLAGS
/// clobbering. The fallback retains the native intrinsic's exact value ABI.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86BOUNDEDLEAF_H
#define NEVERD_BACKEND_LLVM_X86_X86BOUNDEDLEAF_H

#include "neverd/ir/intrinsics/X86/X86BoundedLeaf.h"

#include <string>
#include <string_view>

namespace neverd {
inline std::string x86BoundedLeafNativeName(Intrinsic Id, Arch A) {
  if (!isX86BoundedLeafIntrinsic(Id) || (A != Arch::X86 && A != Arch::X64))
    return {};
  return std::string(isX86EncluLeaf(Id) ? "llvm.x86.enclu"
                                        : "llvm.x86.getsec") +
         ".bounded.state" + (A == Arch::X86 ? "32" : "");
}
inline constexpr std::string_view X86BoundedLeafAsmMarker =
    "# neverd-bounded-leaf-state-v1:";
inline constexpr const char *X86BoundedLeafAsmConstraints =
    "={ax},={bx},={cx},={dx},={di},0,1,2,3,4,~{si},~{flags},~{memory}";

inline std::string x86BoundedLeafAsm(Intrinsic Id, Arch A) {
  if (!isX86BoundedLeafIntrinsic(Id) || (A != Arch::X86 && A != Arch::X64))
    return {};
  const std::string Name = isX86EncluLeaf(Id) ? "enclu" : "getsec";
  const std::string Save = A == Arch::X64 ? "movq %rax,%rsi" : "movl %eax,%esi";
  const std::string Restore =
      A == Arch::X64 ? "movq %rsi,%rax" : "movl %esi,%eax";
  return std::string(X86BoundedLeafAsmMarker) + Name + "\n\t" + Save +
         "\n\tandl $$" + std::to_string(X86ArithmeticFlagsMask) +
         ",%edi\n\tmovl %edi,%eax\n\taddb $$127,%al\n\tsahf\n\t" + Restore +
         "\n\t" + Name + "\n\t" + Save +
         "\n\tlahf\n\tseto %al\n\tmovw %ax,%di\n\t" + Restore + "\n\tandw $$" +
         std::to_string(X86ArithmeticFlagsMask) + ",%di";
}
} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86BOUNDEDLEAF_H
