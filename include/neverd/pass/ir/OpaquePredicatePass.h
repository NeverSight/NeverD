//===- OpaquePredicatePass.h - Opaque predicate pass ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// L1 (IR-layer) opaque-predicate pass.  Guards each basic block behind an
/// always-true predicate the optimizer cannot fold away, routing the
/// (statically reachable but dynamically dead) false edge through a bogus
/// block.  This is the third demo-level sample transform after
/// InstSubstitutionPass and ConstantEncryptionPass, and the first one that
/// changes the control-flow graph — it exercises the rewrite backend's branch
/// relocation, which the (pure-dataflow) substitution / constant-encryption
/// passes do not.
///
/// The predicate is <tt>((x * (x + 1)) & 1) == 0</tt>, true for every integer
/// x (two consecutive integers always have an even product, even under modular
/// wraparound).  \c x is read from a volatile stack slot so the backend cannot
/// prove the predicate constant.  The bogus (false) edge only performs a
/// volatile store to a dead slot and then rejoins the real continuation, so the
/// transform is semantics-preserving regardless of the predicate's value.
///
/// It is a pure IR transform producing ordinary machine code + ordinary fixups,
/// fully orthogonal to the relocation/rewrite backend (L1).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_OPAQUEPREDICATEPASS_H
#define NEVERD_PASS_IR_OPAQUEPREDICATEPASS_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace neverd {

/// Guards basic blocks behind an always-true opaque predicate.
struct OpaquePredicatePass : public llvm::PassInfoMixin<OpaquePredicatePass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM);

  /// Standalone entry point that does NOT instantiate any PassManager template
  /// (mirrors the other L1 sample passes): callers in a different image than
  /// libneverd can apply the transform without AnalysisKey ODR violations.
  /// Returns the number of opaque predicates inserted.
  static unsigned inject(llvm::Module &M);
};

} // namespace neverd

#endif // NEVERD_PASS_IR_OPAQUEPREDICATEPASS_H
