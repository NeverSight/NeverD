//===- NeverDCAPISafety.h - C API memory-safety audit & hunt ------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Memory-safety analysis over a loaded binary: an audit track that reports
/// heap-lifetime defects (leak, double free, use after free) and a hunt track
/// that reports dangerous-copy overflows with a concrete witness.  Both run on
/// the format-neutral lifted IR, so PE, ELF, and Mach-O are analysed the same
/// way.
///
/// All returned strings are heap-allocated via strdup(); callers must free them
/// with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_SAFETY_H
#define NEVERD_SDK_CAPI_SAFETY_H

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

/// Resource limits and catalog overrides for a safety analysis.  Zero the
/// struct, set `struct_size`, and leave any numeric field zero to use the
/// engine default.  The two path fields are optional and, when set, extend the
/// built-in sink and source catalog.
typedef struct neverd_safety_options {
  size_t struct_size;
  unsigned max_paths;
  unsigned max_steps;
  unsigned max_loop;
  unsigned long long solver_conflicts;
  /// Optional catalog specification files (UTF-8 paths); NULL to skip.
  const char *sinks_path;
  const char *sources_path;
} neverd_safety_options;

/// Audit heap-object lifetimes and return an owned JSON report.  The report
/// lists one finding per detected leak, double free, or use after free, each
/// with the callee name, its identity origin, and the call address.  Caller
/// frees the returned string with neverd_free_string().
NEVERD_API const char *
neverd_session_audit_json(neverd_session_t Sess,
                          const neverd_safety_options *Options);

/// Hunt dangerous-copy overflows and return an owned JSON report.  Each finding
/// carries a verdict (SAFE / UNSAFE / UNKNOWN), a confidence, and — for a proven
/// overflow — the concrete witness that drives it.  Caller frees the returned
/// string with neverd_free_string().
NEVERD_API const char *
neverd_session_hunt_json(neverd_session_t Sess,
                         const neverd_safety_options *Options);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_SAFETY_H
