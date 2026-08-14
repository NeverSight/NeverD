//===- NeverDCAPISimplify.h - C API expression simplification -----*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bitvector expression simplification: the typed and JSON entry points,
/// their option/result structs, and the outcome/evidence enums.
///
/// Result strings are released with neverd_simplify_result_dispose(); the
/// JSON entry point's string is released with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_SIMPLIFY_H
#define NEVERD_SDK_CAPI_SIMPLIFY_H

#include "neverd/sdk/NeverDCAPITypes.h"

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

// ===--------------------------------------------------------------------===//
// Expression simplification
// ===--------------------------------------------------------------------===//

/// Why the simplifier handed back the expression it was given.
///
/// "Unchanged" is several different answers wearing one face, and only some of
/// them say anything about the expression rather than about the budget it was
/// given.  A caller deciding whether to spend more needs to tell them apart.
typedef enum neverd_simplify_outcome {
  /// Nothing in the expression belongs to the algebra the engine works in, so
  /// there was nothing to measure.  Spending more would not help.
  NEVERD_SIMPLIFY_NOT_APPLICABLE = 0,
  /// Measured, and no form shorter than what is already there exists.
  NEVERD_SIMPLIFY_ALREADY_SHORTEST = 1,
  /// More inputs than one measurement can afford, and no split into
  /// independent parts or mask-uniform columns was available.  A larger
  /// `max_atoms` reaches it, at twice the cost per input added.
  NEVERD_SIMPLIFY_TOO_MANY_INPUTS = 2,
  /// The layered walk or polynomial search stopped at `max_work`.
  NEVERD_SIMPLIFY_BUDGET_EXHAUSTED = 3,
  /// A shorter form was found, and is in `output`.
  NEVERD_SIMPLIFY_REWRITTEN = 4
} neverd_simplify_outcome_t;

/// What stands behind a rewrite that was made.
typedef enum neverd_simplify_evidence {
  /// Nothing was rewritten.
  NEVERD_SIMPLIFY_EVIDENCE_NONE = 0,
  /// The derivation is exact by construction and a separate deterministic
  /// coefficient verifier accepted it.  Sampling, when enabled, is only a
  /// defect net.
  NEVERD_SIMPLIFY_EVIDENCE_DERIVATION = 1,
  /// Reserved for callers or optional backends that explicitly accept a
  /// heuristic result.  The built-in production solvers never return this.
  NEVERD_SIMPLIFY_EVIDENCE_SAMPLES = 2
} neverd_simplify_evidence_t;

/// How to simplify.  Zero the whole struct, set `struct_size`, then set only
/// what you mean to change: every field left zero takes the engine's default.
/// `exhaustive != 0` selects the unlimited MBA work/arity policy and removes
/// the expression parser's nesting and width policy ceilings.  Memory-safety
/// bounds and the symbolic IR's representational limits still apply.
///
/// `struct_size` is what lets this grow.  A library newer than its caller reads
/// only the fields the caller's struct actually has, so a plugin compiled
/// against an older header keeps working against a newer libneverd instead of
/// having fields read past the end of what it allocated.
typedef struct neverd_simplify_options {
  size_t struct_size;
  /// Width every leaf without a `#bits` suffix is created at.  Zero means 32.
  unsigned width;
  /// Measure one layer only.  The default is the layered walk, which reaches
  /// inside the subterms a single measurement has to treat as opaque; that is
  /// what obfuscated input needs, and it costs more on input already short.
  int shallow;
  /// Most distinct inputs one measurement may span.  The cost is 2^this, so it
  /// is the dial between reach and time.  Zero takes the default.
  unsigned max_atoms;
  /// Work budget for the layered walk and combinatorial polynomial search.
  /// Zero takes the default; (size_t)-1 removes the resource budget.
  size_t max_work;
  /// Random assignments a rewrite is checked against before it is returned.
  /// Zero takes the default.
  unsigned verify_samples;
  /// Return a rewrite even when it reads worse than what it replaces.  For
  /// measuring the engine, not for using it.
  int allow_growth;
  /// Remove parser, MBA arity, and MBA work policy ceilings explicitly.  This
  /// takes precedence over `max_atoms` and `max_work`.
  int exhaustive;
} neverd_simplify_options;

/// What became of one expression.  Zero the struct and set `struct_size` before
/// the call; everything else is written by it.  Release it with
/// neverd_simplify_result_dispose() whatever the outcome.
typedef struct neverd_simplify_result {
  size_t struct_size;
  /// Zero when the expression could not be read, in which case `error` and
  /// `error_offset` say what and where, and nothing else is set.
  int ok;
  const char *error;
  size_t error_offset;
  /// The expression as the engine read it, which is already shorter than what
  /// was written whenever building it folded something.
  const char *input;
  const char *output;
  int changed;
  /// What the expression costs a reader, before and after.
  size_t cost_before;
  size_t cost_after;
  /// Distinct inputs the winning measurement spanned.
  unsigned inputs;
  /// Work units consumed by graph traversal, corner measurements, coefficient
  /// verification, and polynomial expansion/search.
  size_t work;
  neverd_simplify_outcome_t outcome;
  neverd_simplify_evidence_t evidence;
  /// Stable spellings of the two above, for a report meant to be read.
  const char *outcome_name;
  const char *evidence_name;
} neverd_simplify_result;

/// Simplify a bitvector expression written in the engine's infix syntax:
/// C operators throughout, calls for the ones C has no spelling for
/// (`sdiv`, `ashr`, `rol`, `zext`, `extract`, …), and an optional `#bits`
/// suffix on any leaf that leaves the ambient width.
///
/// \p Options may be null, which takes every default.  Returns zero on success,
/// including when the expression did not parse -- that is reported through
/// \p Result rather than as a failure of the call -- and non-zero only when the
/// arguments themselves are unusable.
NEVERD_API int neverd_simplify_expr(const char *Expr,
                                    const neverd_simplify_options *Options,
                                    neverd_simplify_result *Result);

/// Release the strings in \p Result.  Safe on a zeroed struct and safe twice.
NEVERD_API void neverd_simplify_result_dispose(neverd_simplify_result *Result);

/// The JSON spelling of the same thing, for callers that had it before the
/// typed entry point existed and for languages where parsing one string beats
/// declaring a struct.  Both go through one implementation.
///
/// \p Width is the width every leaf without a `#bits` suffix is created at.
/// \p Deep asks for the layered walk.
///
/// Returns a JSON object.  Having simplified:
///   {"ok":true, "input":"…", "output":"…", "changed":true,
///    "costBefore":7, "costAfter":3, "inputs":2, "work":19,
///    "outcome":"rewritten", "evidence":"derivation"}
/// On a syntax error:
///   {"ok":false, "error":"expected ')'", "offset":6}
/// Caller frees with neverd_free_string().
NEVERD_API const char *neverd_simplify_expr_json(const char *Expr,
                                                 unsigned Width, int Deep);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_SIMPLIFY_H
