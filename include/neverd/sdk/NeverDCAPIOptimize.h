//===- NeverDCAPIOptimize.h - Transactional LLVM C API --------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Versioned C bindings for parsing and transactionally optimizing textual
/// LLVM IR with the same pipeline used by NeverD's lifted modules.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_OPTIMIZE_H
#define NEVERD_SDK_CAPI_OPTIMIZE_H

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

/// Observable reason module optimization stopped.  Values are append-only.
typedef enum neverd_optimization_stop {
  NEVERD_OPTIMIZATION_STABLE = 0,
  NEVERD_OPTIMIZATION_CYCLE_DETECTED = 1,
  NEVERD_OPTIMIZATION_BUDGET_EXHAUSTED = 2,
  NEVERD_OPTIMIZATION_VERIFICATION_FAILED = 3,
  NEVERD_OPTIMIZATION_INPUT_INVALID = 4
} neverd_optimization_stop_t;

/// Pipeline shape.  DEFAULT keeps zero-initialized options on Deep mode.
typedef enum neverd_optimization_mode {
  NEVERD_OPTIMIZATION_MODE_DEFAULT = 0,
  NEVERD_OPTIMIZATION_MODE_CONSERVATIVE = 1,
  NEVERD_OPTIMIZATION_MODE_THIN = 2,
  NEVERD_OPTIMIZATION_MODE_DEEP = 3
} neverd_optimization_mode_t;

/// LLVM's standard optimization levels.  DEFAULT selects O2.
typedef enum neverd_llvm_optimization_level {
  NEVERD_LLVM_OPTIMIZATION_DEFAULT = 0,
  NEVERD_LLVM_OPTIMIZATION_O0 = 1,
  NEVERD_LLVM_OPTIMIZATION_O1 = 2,
  NEVERD_LLVM_OPTIMIZATION_O2 = 3,
  NEVERD_LLVM_OPTIMIZATION_O3 = 4
} neverd_llvm_optimization_level_t;

/// Stable, non-owned spelling.  An out-of-range value returns "invalid".
NEVERD_API const char *
neverd_optimization_stop_name(neverd_optimization_stop_t Stop);

/// Controls one textual LLVM IR optimization transaction.
///
/// Zero-initialized options select Deep/O2, a bounded convergence budget, and
/// disabled synthesis.  Synthesis uses the same grammar/search/solver field
/// meanings as neverd_synthesize_options.  `exhaustive != 0` explicitly
/// removes convergence, synthesis-work, and solver resource ceilings.
/// Synthesis fields require `enable_synthesis != 0`; synthesis itself is
/// incompatible with Conservative mode.  Invalid combinations fail with
/// InputInvalid before parsing or transforming the module.
typedef struct neverd_optimize_llvm_ir_options {
  size_t struct_size;
  neverd_optimization_mode_t mode;
  neverd_llvm_optimization_level_t llvm_level;
  unsigned max_rounds;
  int enable_synthesis;
  size_t synthesis_max_cost;
  size_t synthesis_max_samples;
  unsigned synthesis_verify_samples;
  size_t synthesis_max_work;
  unsigned synthesis_max_leaves;
  unsigned synthesis_max_constants;
  unsigned synthesis_stochastic_slots;
  unsigned synthesis_stochastic_restarts;
  size_t synthesis_stochastic_iterations;
  uint64_t solver_max_conflicts;
  uint64_t solver_max_propagations;
  uint64_t solver_max_watch_visits;
  int exhaustive;
} neverd_optimize_llvm_ir_options;

/// Result and aggregate telemetry from the committed module.
typedef struct neverd_optimize_llvm_ir_result {
  size_t struct_size;
  /// Zero for invalid input or a rejected optimized candidate.
  int ok;
  const char *error;
  size_t error_line;
  size_t error_column;
  /// Printed committed module.  Null when options or textual IR are invalid
  /// before a parsed module exists; transaction failures return the unchanged
  /// committed input module.
  const char *output_ir;
  int changed;
  neverd_optimization_stop_t stop;
  uint64_t functions_visited;
  /// Maximum semantic rounds reached by any visited function.
  unsigned rounds;
  uint64_t semantic_rewrites;
  uint64_t search_work;
  uint64_t proof_queries;
  uint64_t proof_conflicts;
  uint64_t proof_propagations;
  uint64_t proof_watch_visits;
} neverd_optimize_llvm_ir_result;

/// Parse textual LLVM IR, optimize a same-context clone, and print only the
/// committed module.  Parse failure reports InputInvalid with no output IR.
/// Non-zero is reserved for unusable result arguments.
NEVERD_API int
neverd_optimize_llvm_ir(const char *IR,
                        const neverd_optimize_llvm_ir_options *Options,
                        neverd_optimize_llvm_ir_result *Result);

/// Release every owned string reached by `Result->struct_size`.  Safe twice.
NEVERD_API void
neverd_optimize_llvm_ir_result_dispose(neverd_optimize_llvm_ir_result *Result);

/// JSON-v1 adapter over neverd_optimize_llvm_ir().  The root object contains
/// `schemaVersion: 1`.  Caller releases the result with neverd_free_string().
NEVERD_API const char *
neverd_optimize_llvm_ir_json_v1(const char *IR,
                                const neverd_optimize_llvm_ir_options *Options);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_OPTIMIZE_H
