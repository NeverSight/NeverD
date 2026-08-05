//===- ControlFlowFlatteningPass.h - Control-flow flattening ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// L1 (IR-layer) control-flow flattening pass.  Replaces a function's natural
/// control-flow graph with a dispatcher loop: every original block is turned
/// into a case of a central switch driven by a state variable, and each block,
/// instead of branching to its successor, writes the successor's case id to the
/// state variable and jumps back to the dispatcher.  This is the fourth
/// demo-level sample transform (after instruction substitution, constant
/// encryption and opaque predicates) and, like opaque predicates, it rewrites
/// the control-flow graph — but far more aggressively: it adds a switch (which
/// the backend may lower to a jump table in read-only data) and rebuilds every
/// edge, so it is the strongest stress test yet for the rewrite backend's
/// branch / jump-table relocation.
///
/// To keep the flattened graph valid SSA (no original block dominates another
/// once they are all reached through the dispatcher) the pass first spills
/// every PHI node and cross-block value to a stack slot in the entry block
/// (reg2mem-style), exactly the classic flattening recipe.  It is otherwise a
/// pure IR transform producing ordinary machine code + ordinary fixups, fully
/// orthogonal to the relocation/rewrite backend (L1).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_CONTROLFLOWFLATTENINGPASS_H
#define NEVERD_PASS_IR_CONTROLFLOWFLATTENINGPASS_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace neverd {

/// Flattens each function's control-flow graph into a dispatcher loop.
struct ControlFlowFlatteningPass
    : public llvm::PassInfoMixin<ControlFlowFlatteningPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM);

  /// Standalone entry point that does NOT instantiate any PassManager template
  /// (mirrors the other L1 sample passes): callers in a different image than
  /// libneverd can apply the transform without AnalysisKey ODR violations.
  /// Returns the number of basic blocks moved into a dispatcher.
  static unsigned inject(llvm::Module &M);
};

} // namespace neverd

#endif // NEVERD_PASS_IR_CONTROLFLOWFLATTENINGPASS_H
