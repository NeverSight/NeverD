//===- LLVMEmitter.h - Solana SBF to LLVM IR backend ----------*- C++ -*-===//

#ifndef NEVERD_SBF_LLVMEMITTER_H
#define NEVERD_SBF_LLVMEMITTER_H

#include "neverd/sbf/SBFIR.h"

#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include <memory>
#include <string>

namespace llvm {
class LLVMContext;
}

namespace neverd::sbf {

struct LLVMEmitterOptions {
  std::string ModuleName = kModuleName.str();
  std::string FunctionName = kEntryFunctionName.str();
};

llvm::Expected<std::unique_ptr<llvm::Module>>
emitLLVM(const SBFProgram &Program, llvm::LLVMContext &Context,
         const LLVMEmitterOptions &Options = {});

std::string emitLLVMText(const llvm::Module &Module);

} // namespace neverd::sbf

#endif // NEVERD_SBF_LLVMEMITTER_H
