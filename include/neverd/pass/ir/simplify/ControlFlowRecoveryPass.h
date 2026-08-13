//===- ControlFlowRecoveryPass.h - Undo dispatcher-loop flattening -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recovers the control flow that a dispatcher loop was built to hide.
///
/// Control-flow flattening replaces a function's graph with a state variable
/// and one central switch: every original block ends by writing the number of
/// the block that should run next and jumping back to the switch, so the graph
/// becomes a star and the order the blocks actually run in is data rather than
/// structure.  Nothing downstream can read it, which is the point.
///
/// Putting it back does not require recognising the shape.  A predecessor that
/// writes a known number to the slot and then jumps to a block whose only job
/// is to read that slot and switch on it could have jumped to the chosen case
/// directly, and saying so is a fact about stores and loads rather than about
/// obfuscation.  So this is jump threading through a stack slot: it is correct
/// wherever it applies, it happens to dismantle a dispatcher completely, and
/// on an ordinary switch it is the constant propagation anyone would want.
///
/// Emptying the dispatcher is as far as it goes.  Deleting the block it leaves
/// unreachable, and promoting the slots the flattening spilled values into,
/// are what the promotion and SimplifyCFG around it in the pipeline already do.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_SIMPLIFY_CONTROLFLOWRECOVERYPASS_H
#define NEVERD_PASS_IR_SIMPLIFY_CONTROLFLOWRECOVERYPASS_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Function;
} // namespace llvm

namespace neverd {

/// Rewrites a jump into a dispatcher as a jump to the case it would select.
///
/// Runs before promotion, because that is where the state lives in memory: a
/// lifted function keeps everything in stack slots until it is promoted, and so
/// does a flattened one, for the same reason -- once every block is reached
/// only through the dispatcher, no block dominates another and a value crossing
/// a block boundary has nowhere else to travel.  Recovering the graph first is
/// what lets the ordinary promotion that follows put those values back in
/// registers, with the phis the real control flow calls for.
struct ControlFlowRecoveryPass
    : public llvm::PassInfoMixin<ControlFlowRecoveryPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);

  /// Standalone entry point that instantiates no PassManager template, so a
  /// caller in a different image than libneverd can apply the transform without
  /// tripping an AnalysisKey ODR violation (mirrors the L1 obfuscation passes).
  /// Returns the number of edges rewritten.
  static unsigned recover(llvm::Function &F);
};

} // namespace neverd

#endif // NEVERD_PASS_IR_SIMPLIFY_CONTROLFLOWRECOVERYPASS_H
