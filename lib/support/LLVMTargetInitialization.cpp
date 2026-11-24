//===- LLVMTargetInitialization.cpp - Linked target registry --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Register exactly the backends selected by NeverD's LLVM link contract.
//===----------------------------------------------------------------------===//

#include "neverd/support/LLVMTargetInitialization.h"

#include "neverd/ArchSupport.h"

#include "llvm/Support/TargetSelect.h"

namespace neverd {

void initializeLLVMCodegenTargets() {
  [[maybe_unused]] static const bool Initialized = [] {
#define NEVERD_LLVM_CODEGEN_TARGET(Name, Availability, Supports)               \
  LLVMInitialize##Name##TargetInfo();                                          \
  LLVMInitialize##Name##Target();                                              \
  LLVMInitialize##Name##TargetMC();                                            \
  LLVMInitialize##Name##AsmPrinter();                                          \
  LLVMInitialize##Name##AsmParser();
#include "LLVMCodegenTargets.inc"
#undef NEVERD_LLVM_CODEGEN_TARGET
    return true;
  }();
}

bool archCodegenSupported(Arch A) {
#define NEVERD_LLVM_CODEGEN_TARGET(Name, Availability, Supports)               \
  if (Supports)                                                                \
    return true;
#include "LLVMCodegenTargets.inc"
#undef NEVERD_LLVM_CODEGEN_TARGET
  return false;
}

} // namespace neverd
