//===- LLVMTargetInitialization.cpp - Linked target registry --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Register exactly the backends selected by NeverD's LLVM link contract.
//===----------------------------------------------------------------------===//

#include "neverd/support/LLVMTargetInitialization.h"

#include "llvm/Support/TargetSelect.h"

namespace neverd {

void initializeLLVMCodegenTargets() {
  [[maybe_unused]] static const bool Initialized = [] {
#define NEVERD_LLVM_CODEGEN_TARGET(Name)                                       \
  LLVMInitialize##Name##TargetInfo();                                          \
  LLVMInitialize##Name##Target();                                              \
  LLVMInitialize##Name##TargetMC();                                            \
  LLVMInitialize##Name##AsmPrinter();                                          \
  LLVMInitialize##Name##AsmParser();
#include "neverd/support/LLVMCodegenTargets.def"
    return true;
  }();
}

} // namespace neverd
