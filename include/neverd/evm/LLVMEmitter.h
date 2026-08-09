//===- LLVMEmitter.h - EVM to LLVM i256 backend ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares lowering of EVM IR to verifier-clean LLVM IR using `i256` words.
///
//===----------------------------------------------------------------------===//

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

/// Controls module/function naming and optional trace-hook emission.
struct LLVMEmitterOptions {
  std::string ModuleName = kDefaultLLVMModuleName.str();
  std::string FunctionName = kDefaultExecutionFunctionName.str();
  bool EmitTraceHooks = true;
};

/// Builds and verifies an LLVM module for the EVM state machine.
llvm::Expected<std::unique_ptr<llvm::Module>>
emitLLVM(const EVMProgram &Program, llvm::LLVMContext &Context,
         const LLVMEmitterOptions &Options = {});

/// Serializes a generated module in canonical LLVM assembly syntax.
std::string emitLLVMText(const llvm::Module &Module);

} // namespace neverd::evm

#endif // NEVERD_EVM_LLVMEMITTER_H
