//===- LLVMPrivateFrameAccesses.h - Lifted stack addresses ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_LLVMPRIVATEFRAMEACCESSES_H
#define NEVERD_ANALYSIS_LLVMPRIVATEFRAMEACCESSES_H

namespace llvm {
class Module;
}
namespace neverd {
namespace frame_access {
#define LLVM_FRAME_VALUE(Name, Value) inline constexpr unsigned Name = Value;
#include "neverd/analysis/LLVMPrivateFrameAccesses.def"
} // namespace frame_access

/// Restore pointer provenance for bounded constant accesses to a lifted
/// function's private frame before LLVM promotes the underlying storage.
bool canonicalizePrivateFrameAccesses(llvm::Module &Module);
} // namespace neverd

#endif
