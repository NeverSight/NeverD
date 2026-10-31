//===- X86SvmContext.h - Software-managed SVM state ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native VMEXIT context layout shared with the LLVM/NeverC extension.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X86SVMCONTEXT_H
#define NEVERD_IR_X86_X86SVMCONTEXT_H

#include <cstdint>

namespace neverd {

struct X86SvmContext {
  uint64_t VMCB = 0;
  uint64_t GuestXState = 0;
  uint64_t HostXState = 0;
  uint64_t XStateMask = 0;
  // x86 encoding numbers: RBX, RCX, RDX, RSI, RDI, RBP, R8..R15.
  uint64_t GPR[14] = {};
};

inline constexpr unsigned X86SvmGPRs[] = {3, 1,  2,  6,  7,  5,  8,
                                          9, 10, 11, 12, 13, 14, 15};
static_assert(sizeof(X86SvmContext) == 144);

} // namespace neverd

#endif // NEVERD_IR_X86_X86SVMCONTEXT_H
