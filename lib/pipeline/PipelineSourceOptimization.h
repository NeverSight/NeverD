//===- PipelineSourceOptimization.h - Private source module optimization
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PIPELINE_PIPELINESOURCEOPTIMIZATION_H
#define NEVERD_PIPELINE_PIPELINESOURCEOPTIMIZATION_H

namespace llvm {
class Module;
}

namespace neverd {
class LLVMSourceMap;
struct SourceViewOptions;

/// Optimize a freshly emitted, LLVM-verified module whose only live observers
/// belong to Sources. On failure both are disposable: the caller must rebuild
/// the input before applying the ordinary transactional fallback.
bool tryOptimizeOwnedSourceModule(llvm::Module &Mod, LLVMSourceMap *Sources,
                                  const SourceViewOptions &View);
} // namespace neverd

#endif
