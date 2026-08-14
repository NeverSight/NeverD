//===- NeverDCAPISynth.h - Proof-gated synthesis C API ---------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Versioned C bindings for expression synthesis backed by an equivalence
/// proof.  Search telemetry and proof telemetry use separate counters because
/// they measure different work.
///
/// Zero an options or result struct and set `struct_size` before use.  Release
/// every result, including an error result, with
/// neverd_synthesize_result_dispose().  Release JSON strings with
/// neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_SYNTH_H
#define NEVERD_SDK_CAPI_SYNTH_H

#include "neverd/sdk/NeverDCAPITypes.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#ifdef NEVERD_EXPORTS
#define NEVERD_API __declspec(dllexport)
#else
#define NEVERD_API __declspec(dllimport)
#endif
#else
#define NEVERD_API __attribute__((visibility("default")))
#endif

/// Final verification disposition.  A concrete sample may establish
/// Different without a solver query; samples can never establish Equivalent.
/// These values are append-only.
typedef enum neverd_proof_status {
  NEVERD_PROOF_NOT_RUN = 0,
  NEVERD_PROOF_EQUIVALENT = 1,
  NEVERD_PROOF_DIFFERENT = 2,
  NEVERD_PROOF_UNKNOWN = 3,
  /// The proof question itself was malformed.  This is a terminal,
  /// fail-closed disposition rather than a resource-budget result.
  NEVERD_PROOF_INVALID = 4
} neverd_proof_status_t;

/// Why synthesis did or did not return a shorter expression.  These values
/// are append-only and match the semantic simplifier's public contract.
typedef enum neverd_synthesis_outcome {
  NEVERD_SYNTHESIS_NOT_APPLICABLE = 0,
  NEVERD_SYNTHESIS_ALREADY_SHORTEST = 1,
  NEVERD_SYNTHESIS_TOO_MANY_INPUTS = 2,
  NEVERD_SYNTHESIS_SEARCH_BUDGET_EXHAUSTED = 3,
  NEVERD_SYNTHESIS_COUNTEREXAMPLE = 4,
  NEVERD_SYNTHESIS_PROOF_INCOMPLETE = 5,
  NEVERD_SYNTHESIS_REWRITTEN = 6
} neverd_synthesis_outcome_t;

/// Stable, non-owned spellings.  An out-of-range value returns "invalid".
NEVERD_API const char *neverd_proof_status_name(neverd_proof_status_t Status);
NEVERD_API const char *
neverd_synthesis_outcome_name(neverd_synthesis_outcome_t Outcome);

/// Grammar, search, and solver resources for one synthesis request.
///
/// Zero fields choose bounded library defaults.  `exhaustive != 0` is the
/// explicit opt-in that removes search-work, solver, and native expression
/// parser resource ceilings; the grammar fields remain the caller's
/// description of the expressions worth searching.  Memory-safety bounds and
/// the symbolic IR's representational limits still apply.
typedef struct neverd_synthesize_options {
  size_t struct_size;
  /// Width of leaves without an explicit `#bits` suffix.  Zero means 32.
  unsigned width;
  /// Largest enumerated candidate, measured in grammar nodes.
  size_t max_cost;
  /// Sample points used to distinguish candidates during discovery.
  size_t max_samples;
  /// Independent sample points allowed to reject a candidate before proof.
  unsigned verify_samples;
  /// Candidate-work ceiling shared by enumeration and stochastic search.
  size_t max_work;
  /// Maximum independent leaves admitted to the search grammar.
  unsigned max_leaves;
  /// Maximum literals admitted to the search grammar.
  unsigned max_constants;
  /// Straight-line program slots available to stochastic search.
  unsigned stochastic_slots;
  /// Independent stochastic attempts.
  unsigned stochastic_restarts;
  /// Mutations attempted per stochastic restart.
  size_t stochastic_iterations;
  /// SAT conflict ceiling.  Zero takes the bounded C API default.
  uint64_t solver_max_conflicts;
  /// SAT propagation ceiling.  Zero takes the bounded C API default.
  uint64_t solver_max_propagations;
  /// SAT watched-clause visit ceiling.  Zero takes the bounded C API default.
  uint64_t solver_max_watch_visits;
  /// Remove parser, search-work, and solver ceilings explicitly.
  int exhaustive;
} neverd_synthesize_options;

/// Typed result from one proof-gated synthesis request.
typedef struct neverd_synthesize_result {
  size_t struct_size;
  /// Zero only when the expression could not be parsed.
  int ok;
  const char *error;
  size_t error_offset;
  /// Canonical input and committed output.  They are equal on every refusal.
  const char *input;
  const char *output;
  int changed;
  size_t cost_before;
  size_t cost_after;
  unsigned inputs;
  /// Grammar cost of the accepted candidate, or zero when none was accepted.
  size_t candidate_cost;
  neverd_synthesis_outcome_t outcome;
  neverd_proof_status_t proof_status;
  uint64_t search_work;
  uint64_t proof_queries;
  uint64_t proof_conflicts;
  uint64_t proof_propagations;
  uint64_t proof_watch_visits;
  /// Canonical JSON for a final solver refutation; null otherwise.
  const char *counterexample_json;
} neverd_synthesize_result;

/// Search for a shorter expression and commit it only after the built-in
/// solver proves equivalence.  A refutation, incomplete proof, or exhausted
/// budget returns the input unchanged.  Parse errors are reported through
/// `Result`; non-zero is reserved for unusable result arguments.
NEVERD_API int neverd_synthesize_expr(const char *Expr,
                                      const neverd_synthesize_options *Options,
                                      neverd_synthesize_result *Result);

/// Release every owned string reached by `Result->struct_size`.  Safe on a
/// zeroed result and safe to call twice.
NEVERD_API void
neverd_synthesize_result_dispose(neverd_synthesize_result *Result);

/// JSON-v1 adapter over neverd_synthesize_expr().  The root object contains
/// `schemaVersion: 1`.  Caller releases the returned string with
/// neverd_free_string().
NEVERD_API const char *
neverd_synthesize_expr_json_v1(const char *Expr,
                               const neverd_synthesize_options *Options);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_SYNTH_H
