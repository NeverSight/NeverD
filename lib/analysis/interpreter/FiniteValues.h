//===- FiniteValues.h - Bounded exhaustive bitvector projection ---*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_INTERPRETER_FINITEVALUES_H
#define NEVERD_ANALYSIS_INTERPRETER_FINITEVALUES_H

#include "neverd/analysis/InterpreterSpecialization.h"

namespace neverd::analysis::detail {

enum class FiniteValueStatus {
  Complete,
  TooManyValues,
  Unknown,
  Invalid,
  QueryBudgetExceeded,
};

struct FiniteValues {
  FiniteValueStatus Status = FiniteValueStatus::Unknown;
  /// Empty on every incomplete result. Complete with no tuples means the
  /// predicate is unsatisfiable, not that its outputs may take any value.
  std::vector<std::vector<uint64_t>> Tuples;
};

/// True only when Value preserves enough independent variable bits to exceed
/// Limit on every reachable path, and none of those variables occurs anywhere
/// in Predicate or any RelatedValues expression. Related roots are inspected
/// directly; constructing a Boolean surrogate could simplify away dependencies.
/// Recognized bit mappings include extracts, concatenations,
/// extensions, complements, and bitwise operations with constant masks. This
/// does not establish that Predicate is satisfiable: optional projection may
/// omit the field, but reachability still needs its own proof. All roots,
/// nodes, expression-bit visits, and operand work share MaxVisited; incomplete
/// walks return false. No expressions are created and no solver is invoked.
bool hasUnconstrainedProjectionInput(
    const symbolic::SymContext &Ctx, symbolic::SymRef Predicate,
    symbolic::SymRef Value, uint32_t Limit, uint64_t MaxVisited,
    llvm::ArrayRef<symbolic::SymRef> RelatedValues = {});

FiniteValues
enumerateFiniteValues(symbolic::SymContext &Ctx, symbolic::SymRef Predicate,
                      llvm::ArrayRef<symbolic::SymRef> Values, uint32_t Limit,
                      const SpecializationOptions &Options, uint64_t &Queries);

} // namespace neverd::analysis::detail

#endif // NEVERD_ANALYSIS_INTERPRETER_FINITEVALUES_H
