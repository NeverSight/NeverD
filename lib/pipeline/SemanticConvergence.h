//===- SemanticConvergence.h - Observable semantic fixed point -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal convergence driver shared by the semantic and deep LLVM function
/// optimizers.  It exposes bounded work and exact cycle detection to focused
/// tests without making either policy part of the public Pipeline surface.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_SEMANTICCONVERGENCE_H
#define NEVERD_LIB_PIPELINE_SEMANTICCONVERGENCE_H

#include "neverd/pass/ir/simplify/SymSimplifyPass.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstdint>
#include <string>

namespace neverd {

/// One completed optimization round and its post-round structural hash.
struct ConvergenceRound {
  bool Changed = false;
  SymSimplifyResult Semantic;
  uint64_t StructuralHash = 0;
};

using RunRoundFn = llvm::function_ref<ConvergenceRound()>;
using SnapshotFn = llvm::function_ref<std::string()>;

/// Run until stable, an exact repeated state, or a caller-owned finite budget.
/// Zero means unlimited.  The driver retains one exact checkpoint only.
FunctionOptimizationResult driveSemanticConvergence(unsigned MaxRounds,
                                                    RunRoundFn RunRound,
                                                    SnapshotFn Snapshot);

/// Merge one final function result into a module result exactly once.
void mergeFunctionOptimizationResult(OptimizationResult &Module,
                                     const FunctionOptimizationResult &Func);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_SEMANTICCONVERGENCE_H
