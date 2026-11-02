//===- X86MemoryAddressAsm.h - Data address carriers --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Assembly address operands that cannot overwrite compiler-owned frame bases.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86MEMORYADDRESSASM_H
#define NEVERD_BACKEND_LLVM_X86_X86MEMORYADDRESSASM_H

namespace neverd {

struct X86MemoryAddressAsm {
  const char *LLVMConstraint;
  const char *GNUConstraint;
};

/// Callers have already validated the mode and address width. AX is not a
/// frame/realignment/PIC base. Address16 needs a legal DS-default base: DI also
/// avoids i386's reserved ESI realignment and EBX GOT registers.
constexpr X86MemoryAddressAsm x86MemoryAddressAsm(unsigned AddressBits) {
  return AddressBits == 16 ? X86MemoryAddressAsm{"{di}", "D"}
                           : X86MemoryAddressAsm{"{ax}", "a"};
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86MEMORYADDRESSASM_H
