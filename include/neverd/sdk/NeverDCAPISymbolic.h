//===- NeverDCAPISymbolic.h - C API symbolic exploration ----------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded symbolic path exploration of one native LowIR function and the
/// resource limits that bound the walk.
///
/// All returned strings are heap-allocated via strdup(); callers must
/// free them with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_SYMBOLIC_H
#define NEVERD_SDK_CAPI_SYMBOLIC_H

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

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_SYMBOLIC_H
