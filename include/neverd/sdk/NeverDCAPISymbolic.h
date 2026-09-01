//===- NeverDCAPISymbolic.h - C API symbolic exploration ----------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded symbolic and concolic execution of native LowIR functions and the
/// resource limits that bound each walk.
///
/// All returned strings are heap-allocated via strdup(); callers must
/// free them with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_SYMBOLIC_H
#define NEVERD_SDK_CAPI_SYMBOLIC_H

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

// ===--------------------------------------------------------------------===//
// Symbolic path exploration
// ===--------------------------------------------------------------------===//

/// Resource limits and output policy for symbolic path exploration.  Zero the
/// struct, set `struct_size`, and leave any numeric field zero to use the
/// engine default.
typedef struct neverd_symbolic_explore_options {
  size_t struct_size;
  unsigned max_paths;
  unsigned max_steps;
  unsigned max_block_visits;
  /// Include the rendered path predicate and unresolved target expression.
  int include_expressions;
} neverd_symbolic_explore_options;

/// Explore one native LowIR function and return an owned JSON report.
///
/// The report distinguishes a complete walk from one stopped by a loop, step,
/// path, or unresolved-indirect-branch bound, and reports every operation
/// conservatively replaced by unknown state, including unsummarised calls and
/// stores through unresolved addresses.  Caller frees the returned string with
/// neverd_free_string().
NEVERD_API const char *
neverd_symbolic_explore_json(neverd_session_t Sess, neverd_va_t FuncEntry,
                             const neverd_symbolic_explore_options *Options);

// ===--------------------------------------------------------------------===//
// LowIR concolic branch flipping v1
// ===--------------------------------------------------------------------===//

/// Maximum number of borrowed seed records accepted by the v1 boundary.
/// This is a validation ceiling, not a promise that every target exposes this
/// many seedable register ranges.
#define NEVERD_LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1 4096u

/// One frozen v1 entry-register seed range.
///
/// `offset` is a byte offset in NeverD's architecture register file, `bytes`
/// is in the inclusive range 1..8, and `value` is the unsigned value of that
/// range in the target byte order.  The value must fit exactly in `bytes`.
/// Ranges in one request must not overlap.  `reserved` must be zero.
///
/// This array element deliberately has no `struct_size`: its v1 stride is
/// frozen.  A differently shaped element requires a future versioned type and
/// entry point.
typedef struct neverd_lowir_concolic_register_seed_v1 {
  uint64_t offset;
  uint64_t value;
  uint32_t bytes;
  uint32_t reserved;
} neverd_lowir_concolic_register_seed_v1;

/// Borrowed inputs and bounded resources for one LowIR concolic-v1 run.
///
/// Zero the struct and set `struct_size`.  A null options pointer, or a field
/// absent from an older append-only prefix, selects the documented bounded
/// default.  A numeric resource field of zero also selects that default;
/// this API has no unbounded or exhaustive mode.  The seed pointer may be null
/// only when `register_seed_count` is zero and remains owned by the caller.
typedef struct neverd_lowir_concolic_options_v1 {
  size_t struct_size;
  const neverd_lowir_concolic_register_seed_v1 *register_seeds;
  size_t register_seed_count;

  unsigned max_steps;
  unsigned max_block_visits;
  unsigned max_loop_iterations;
  unsigned max_flip_attempts;
  unsigned max_candidates;
  /// Must be zero.  This consumes the v1 prefix's trailing 32-bit slot.
  uint32_t reserved;

  uint64_t solver_max_conflicts;
  uint64_t solver_max_propagations;
  uint64_t solver_max_watch_visits;
  uint64_t solver_max_gates;
} neverd_lowir_concolic_options_v1;

/// Follow one exact concrete LowIR trace and attempt bounded, solver-backed
/// conditional-branch flips.  The owned JSON report uses schema version 1 and
/// adapter `lowir-concolic-v1`; it always reports `exhaustive: false`.
///
/// Request validation, lifting failures, incomplete traces, solver limits,
/// projection refusals, and replay failures are represented in the report
/// rather than by an untyped null.  A null result is reserved for allocation
/// failure.  Caller releases every non-null result with neverd_free_string().
NEVERD_API const char *
neverd_lowir_concolic_json_v1(neverd_session_t Sess, neverd_va_t FuncEntry,
                              const neverd_lowir_concolic_options_v1 *Options);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_SYMBOLIC_H
