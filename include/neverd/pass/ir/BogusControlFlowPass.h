//===- BogusControlFlowPass.h - Bogus control flow --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// L1 (IR-layer) bogus-control-flow pass.  For every basic block it grows a
/// small *fake* control-flow sub-graph on a dead path: the block is split and
/// its real continuation is guarded by an always-true opaque predicate, while
/// the (statically reachable but dynamically dead) false edge is routed through
/// two bogus blocks that run a self-contained junk computation before rejoining
/// the real code.  This is the fifth demo-level sample transform (after
/// instruction substitution, constant encryption, opaque predicates and
/// control-flow flattening).
///
/// It differs from the opaque-predicate pass in two ways: (1) the dead path is
/// a multi-block fake sub-graph (two bogus blocks with their own conditional
/// branch) rather than a single trivial edge, and (2) those blocks carry a real
/// multi-instruction arithmetic chain (junk) that survives to machine code,
/// inflating the function with never-executed-but-present code.  That makes it
/// a denser stress test for the rewrite backend's branch relocation across many
/// small blocks.
///
/// The junk is seeded only from a value loaded out of a volatile stack slot in
/// the entry block (so it dominates every block) and from constants, then
/// stored back to a volatile slot that is never read.  Because the junk depends
/// on nothing but an entry-dominating value and the result sinks into a dead
/// volatile slot, the transform is valid SSA for *any* input function and is
/// semantics-preserving regardless of the predicate's value (the fake blocks
/// never run, and would be harmless even if they did).
///
/// It is a pure IR transform producing ordinary machine code + ordinary fixups,
/// fully orthogonal to the relocation/rewrite backend (L1).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_BOGUSCONTROLFLOWPASS_H
#define NEVERD_PASS_IR_BOGUSCONTROLFLOWPASS_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace neverd {

/// Grows a bogus (dead, opaque-guarded) control-flow sub-graph around each
/// basic block.
struct BogusControlFlowPass : public llvm::PassInfoMixin<BogusControlFlowPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM);

  /// Standalone entry point that does NOT instantiate any PassManager template
  /// (mirrors the other L1 sample passes): callers in a different image than
  /// libneverd can apply the transform without AnalysisKey ODR violations.
  /// Returns the number of basic blocks given a bogus sub-graph.
  static unsigned inject(llvm::Module &M);
};

} // namespace neverd

#endif // NEVERD_PASS_IR_BOGUSCONTROLFLOWPASS_H
