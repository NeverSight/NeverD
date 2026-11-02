//===- LLVMScalarEquivalence.h - Bounded scalar equivalence -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_LLVMSCALAREQUIVALENCE_H
#define NEVERD_ANALYSIS_LLVMSCALAREQUIVALENCE_H

#include "neverd/analysis/LLVMScalarFunctionModel.h"
#include "neverd/analysis/Limits.h"

namespace neverd::analysis {

enum class LLVMScalarEquivalenceStatus {
  Proved,
  Unsupported,
  Unproved,
  BudgetExceeded,
  Cancelled,
};

struct LLVMScalarControlBit {
  unsigned Argument = 0;
  unsigned Bit = 0;
  auto operator<=>(const LLVMScalarControlBit &) const = default;
};

struct LLVMScalarEquivalenceLimits {
  /// Applied separately to each read-only model construction.
  LLVMInterpreterModelLimits Model;
  unsigned MaxControlBits = 12;
  uint64_t MaxPartitions = 4096;
  uint64_t MaxBlockVisits = 4096;
  uint64_t MaxSymbolicNodes = 1048576;
  /// Shared by both executions, partition restarts, decision DAG normalization
  /// and dependency queries. Local symbolic-query ceilings remain separate.
  uint64_t MaxWork = limits::analysis::kLLVMScalarEquivalenceDefaultWork;
};

struct LLVMScalarEquivalenceResult {
  LLVMScalarEquivalenceStatus Status = LLVMScalarEquivalenceStatus::Unproved;
  LLVMScalarInputDomain InputDomain = LLVMScalarInputDomain::RequireNoUndef;
  std::string Diagnostic;
  std::vector<LLVMScalarControlBit> ControlBits;
  uint64_t CompletedPartitions = 0;
  uint64_t Attempts = 0;
  uint64_t Work = 0;
  /// A work charge exceeded MaxWork. Reaching the limit exactly, or refusing
  /// another local resource limit, does not set this flag.
  bool WorkLimitExceeded = false;
};

/// Prove complete return-value equality and defined termination for the
/// admitted pure scalar LLVM domain. Both models use the existing LLVM
/// importer and SymExec. Discover input bits controlling branches or source
/// definedness; exhaust all their combinations and retain every remaining
/// input bit symbolically. Return expressions must agree exactly in NeverD's
/// shared expression algebra. No sampling or external SMT is used.
/// A query using the same Function object models and executes it once per
/// partition, retaining every admission, definedness and termination check.
/// Unresolved control/definedness decisions receive a bounded bottom-up DAG
/// normalization before input-bit discovery. Intermediate data writes stay
/// symbolic; final equality first tries identity/bounded facts, then normalizes
/// the demanded return DAGs. All execution, normalization and retry work shares
/// one budget. This neither replays execution nor assumes that a source
/// annotation is true.
///
/// A result other than Proved authorizes no rewrite; Unproved need not mean
/// inequivalent. Every limit is finite. Unsupported instructions, effects,
/// attributes and input contracts fail explicitly through the model builder.
/// Reports describe these supplied functions at this call, not a persistent
/// native/ABI certificate. Neither function nor its module is modified.
LLVMScalarEquivalenceResult
checkLLVMScalarEquivalence(const llvm::Function &Original,
                           const llvm::Function &Candidate,
                           const LLVMScalarEquivalenceLimits &Limits = {},
                           const LLVMScalarModelOptions &Options = {});

} // namespace neverd::analysis

#endif
