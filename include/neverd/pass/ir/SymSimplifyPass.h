//===- SymSimplifyPass.h - Semantic simplification of LLVM IR ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Collapses mixed boolean-arithmetic and related integer obfuscation on LLVM
/// IR by measuring what a computation does rather than matching its shape.
///
/// This is the counterpart to the HighIR simplifier: the same engine, run one
/// layer earlier, so the recovered form is what the rest of the optimizer and
/// every downstream IR sees.  It works over integer expression trees and the
/// branch conditions built from them; it does not touch memory or anything it
/// cannot read exactly, so it composes with the ordinary optimization pipeline
/// instead of standing apart from it.
///
/// A branch condition is worth measuring for one reason: an opaque predicate is
/// a branch built so that one side is unreachable, with the condition dressed
/// up as arithmetic so that nothing reading its shape can tell.  Measuring the
/// condition turns it into the constant it always was.  Removing the side that
/// constant makes unreachable is left to the SimplifyCFG that follows in the
/// pipeline, which already does exactly that and does it better.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_SYMSIMPLIFYPASS_H
#define NEVERD_PASS_IR_SYMSIMPLIFYPASS_H

#include "llvm/ADT/APInt.h"
#include "llvm/IR/PassManager.h"

#include <optional>

namespace llvm {
class Function;
class Value;
} // namespace llvm

namespace neverd {

/// Function attribute NeverD's L1 obfuscation passes stamp on every definition
/// they rewrite.  SymSimplifyPass skips any function carrying it: this pass
/// measures away exactly the mixed boolean-arithmetic that obfuscation injects,
/// so running it on obfuscated IR would take the obfuscation straight back off.
/// The stamp is what lets an obfuscate-then-patch pipeline reuse one module
/// without the simplifier and the obfuscator undoing each other.
inline constexpr char kObfuscatedFnAttr[] = "neverd-obfuscated";

/// Rewrites integer expressions into the shortest sequence computing the same
/// value, using measurement rather than pattern matching.
///
/// Belongs in the same function pass manager as InstCombine, and immediately
/// after it: the canonical form InstCombine leaves is the cleanest thing to
/// measure, and the compact result of a measurement is the cleanest thing for
/// another InstCombine to finish.  Neither reaches the other's fixed point
/// alone -- an expression mixing `+ - *` with `& | ^ ~` blocks every rule
/// InstCombine can state, and the arithmetic residue this pass leaves is what
/// InstCombine folds best.
struct SymSimplifyPass : public llvm::PassInfoMixin<SymSimplifyPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);

  /// Standalone entry point that instantiates no PassManager template, so a
  /// caller in a different image than libneverd can apply the transform without
  /// tripping an AnalysisKey ODR violation (mirrors the L1 obfuscation passes).
  /// Returns the number of expression roots rewritten.
  static unsigned simplify(llvm::Function &F);

  /// The value \p V always holds, when measuring it says it holds one.
  ///
  /// Offered on its own because deciding a branch is not the only place a value
  /// that cannot vary is worth spotting.  A flattened function computes the
  /// number of the block to run next, and an obfuscator hides that number the
  /// same way it hides everything else -- as arithmetic that no amount of
  /// constant folding sees through, but that measuring collapses. Answering
  /// with the number is what lets the control-flow recovery thread the jump.
  static std::optional<llvm::APInt> constantValueOf(llvm::Value *V);
};

} // namespace neverd

#endif // NEVERD_PASS_IR_SYMSIMPLIFYPASS_H
