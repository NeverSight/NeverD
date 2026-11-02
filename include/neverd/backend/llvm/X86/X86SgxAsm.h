//===- X86SgxAsm.h - Owned SGX assembly completion ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Save rAX before collecting CF/ZF in AL/AH. MOV and SETcc preserve flags,
/// need no stack scratch or LAHF feature, and leave rCX/rDX untouched.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86SGXASM_H
#define NEVERD_BACKEND_LLVM_X86_X86SGXASM_H

#include "neverd/ir/intrinsics/X86/X86Sgx.h"

#include <array>
#include <optional>
#include <string>

namespace neverd {

struct X86SgxAsm {
  std::string Text;
  std::string Constraints;
  uint16_t WordBytes;
  std::array<const char *, 3> OutputConstraints = {"=&r", "={bx}", "={ax}"};
  std::array<const char *, 4> InputConstraints = {"{ax}", "1", "{cx}", "{dx}"};
};

inline std::optional<X86SgxAsm> x86SgxAsm(Intrinsic Id, Arch A) {
  if (!isX86SgxIntrinsic(Id) || (A != Arch::X86 && A != Arch::X64))
    return std::nullopt;
  X86SgxAsm P{std::string(Id == Intrinsic::X86Encls ? "encls" : "enclv") +
                  (A == Arch::X64 ? "\n\tmovq %rax,$0" : "\n\tmovl %eax,$0") +
                  "\n\tsetc %al\n\tsetz %ah",
              {},
              static_cast<uint16_t>(A == Arch::X64 ? 8 : 4)};
  for (const auto *C : P.OutputConstraints)
    P.Constraints += std::string(C) + ',';
  for (const auto *C : P.InputConstraints)
    P.Constraints += std::string(C) + ',';
  P.Constraints += "~{flags},~{memory}";
  return P;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86SGXASM_H
