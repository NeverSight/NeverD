//===- SBFLLVMEmitter.h - Solana SBF to LLVM IR backend ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_EMIT_SBFLLVMEMITTER_H
#define NEVERD_SBF_EMIT_SBFLLVMEMITTER_H

#include "neverd/sbf/SBFIR.h"

#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace llvm {
class LLVMContext;
}

namespace neverd::sbf {

/// Selects the syscall callback contract emitted into an LLVM module.
/// Legacy remains the default so an existing host symbol keeps linking.
/// FeatureAware selects the append-only SBFHostABI.def symbol whose explicit
/// RuntimeFeatureMask-width integer precedes the syscall hash.
enum class LLVMRuntimeSyscallABI : uint8_t { Legacy, FeatureAware };

struct LLVMEmitterOptions {
  std::string ModuleName = kModuleName.str();
  std::string FunctionName = kEntryFunctionName.str();
  LLVMRuntimeSyscallABI SyscallABI = LLVMRuntimeSyscallABI::Legacy;
  /// Runtime snapshot embedded in feature-aware syscall invocations. Absence
  /// means SBFProgram::ActiveRuntimeFeatures; explicit None remains empty.
  std::optional<RuntimeFeature> RuntimeFeatures;
};

llvm::Expected<std::unique_ptr<llvm::Module>>
emitLLVM(const SBFProgram &Program, llvm::LLVMContext &Context,
         const LLVMEmitterOptions &Options = {});

std::string emitLLVMText(const llvm::Module &Module);

} // namespace neverd::sbf

#endif // NEVERD_SBF_EMIT_SBFLLVMEMITTER_H
