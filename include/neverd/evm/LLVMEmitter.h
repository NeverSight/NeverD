//===- LLVMEmitter.h - EVM to LLVM i256 backend ---------------*- C++ -*-===//

#ifndef NEVERD_EVM_LLVMEMITTER_H
#define NEVERD_EVM_LLVMEMITTER_H

#include "neverd/evm/EVMIR.h"

#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include <memory>
#include <string>

namespace llvm {
class LLVMContext;
} // namespace llvm

namespace neverd::evm {

struct LLVMEmitterOptions {
  std::string ModuleName = kDefaultLLVMModuleName.str();
  std::string FunctionName = kDefaultExecutionFunctionName.str();
  bool EmitTraceHooks = true;
};

llvm::Expected<std::unique_ptr<llvm::Module>>
emitLLVM(const EVMProgram &Program, llvm::LLVMContext &Context,
         LLVMEmitterOptions Options = {});

std::string emitLLVMText(const llvm::Module &Module);

} // namespace neverd::evm

#endif // NEVERD_EVM_LLVMEMITTER_H
