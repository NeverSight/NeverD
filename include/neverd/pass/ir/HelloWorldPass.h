//===- HelloWorldPass.h - Test pass for LLVM module ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A trivial LLVM module pass that injects a hello_world function.
/// Useful as a minimal end-to-end test of the pipeline.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_HELLOWORLDPASS_H
#define NEVERD_PASS_IR_HELLOWORLDPASS_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace neverd {

struct HelloWorldPass : public llvm::PassInfoMixin<HelloWorldPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM);

  /// Standalone entry point that constructs its own analysis infrastructure.
  /// Call this from tools that should NOT instantiate PassManager templates
  /// in their own TU (avoids AnalysisKey ODR violations across dylib/exe).
  static void inject(llvm::Module &M);
};

} // namespace neverd

#endif // NEVERD_PASS_IR_HELLOWORLDPASS_H
