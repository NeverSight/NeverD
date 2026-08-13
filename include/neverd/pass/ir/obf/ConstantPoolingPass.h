//===- ConstantPoolingPass.h - Constant pooling pass -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// L1 (IR-layer) constant-pooling pass.  Replaces integer constant operands of
/// binary operators and integer comparisons with a *load from a pass-created
/// read-only global pool*, so the literal constant no longer appears inline in
/// the emitted instruction stream — it lives in read-only data and is fetched
/// at run time through an opaque index.
///
/// For a function with eligible constants the pass materializes one private
/// read-only global array \c neverd_const_pool (\c [N x i64]) holding the
/// distinct constant values (zero-extended to 64 bits).  Each use of a constant
/// \c C of width \c W is rewritten to <tt>trunc(load pool[i])</tt>, where the
/// index \c i is an *opaque* run-time copy of the pool slot produced through a
/// volatile stack slot (a volatile store followed by a volatile load — neither
/// can be eliminated nor value-forwarded, so the backend cannot fold the load
/// back to the constant); the load therefore survives to machine code.  At run
/// time the slot holds the correct index, so the loaded value is exactly \c C.
///
/// This is a deliberately small, demo-level sample transform — distinct from
/// the other samples in one respect that exercises a *new* path through the
/// rewrite backend: it is the first pass that **creates a new global
/// variable**, so it stresses the backend's placement and relocation of
/// pass-introduced read-only data plus run-time-indexed (\c base + i*scale)
/// global addressing. More substantial transforms will be added as separate
/// passes later.
///
/// It is a pure IR transform: it produces ordinary machine code (a read-only
/// global + indexed load) with ordinary fixups, so it is fully orthogonal to
/// the relocation / rewrite backend (L1).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_OBF_CONSTANTPOOLINGPASS_H
#define NEVERD_PASS_IR_OBF_CONSTANTPOOLINGPASS_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace neverd {

/// Moves integer constant operands of binary operators / comparisons into a
/// read-only global pool, loading them at run time through an opaque index.
struct ConstantPoolingPass : public llvm::PassInfoMixin<ConstantPoolingPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM);

  /// Standalone entry point that does NOT instantiate any PassManager template
  /// (mirrors ConstantEncryptionPass::inject): callers in a different image
  /// than libneverd can apply the transform without AnalysisKey ODR violations.
  /// Returns the number of constant operands that were pooled.
  static unsigned inject(llvm::Module &M);
};

} // namespace neverd

#endif // NEVERD_PASS_IR_OBF_CONSTANTPOOLINGPASS_H
