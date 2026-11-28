//===- NeverDCAPISafety.h - C API native memory-safety scan -------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Memory-safety analysis over a loaded binary: an audit track that reports
/// heap-lifetime defects (leak, double free, use after free) and uninitialized
/// local stack reads, and a hunt track that reports dangerous-copy overflows
/// with symbolic evidence and candidate input values. Evidence is replayable
/// only when it carries a complete `process-input-v1` plan. The initial plan
/// covers exact literal environment values and the first supported
/// `read(0)`-family standard-input consumption; argv, file, network, custom,
/// and ambiguous inputs remain
/// non-replayable with a reason. Safety call effects are closed-world: unknown
/// or only partially applicable effects produce UNKNOWN. Both tracks run on
/// the format-neutral lifted IR, so PE, ELF, and Mach-O are analysed the same
/// way.
///
/// All returned strings are heap-allocated via strdup(); callers must free them
/// with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_SAFETY_H
#define NEVERD_SDK_CAPI_SAFETY_H

#include "neverd/sdk/NeverDCAPISession.h"

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

typedef enum neverd_scan_analyzers {
  NEVERD_SCAN_BOTH = 0,
  NEVERD_SCAN_AUDIT = 1,
  NEVERD_SCAN_HUNT = 2
} neverd_scan_analyzers;

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
  /// Interprocedural call-edge and attacker-control summary limits.  Zero
  /// selects the engine default.
  unsigned max_call_depth;
  unsigned max_summary_iterations;
} neverd_safety_options;

/// Options for neverd_session_scan_json. Zero initialize and set struct_size.
/// Pointers are borrowed only for the synchronous call.
typedef struct neverd_scan_options {
  size_t struct_size;
  /// NULL selects the engine's default budgets and catalog.
  const neverd_safety_options *analysis;
  /// One neverd_scan_analyzers value. Zero selects both.
  unsigned analyzers;
  neverd_cancel_probe_fn cancel;
  void *context;
} neverd_scan_options;

/// Run the selected static analyses against one lift of the loaded image.
/// Returns an owned neverd.memory-safety-scan v1 report, including partial
/// findings on cancellation. Free it with neverd_free_string().
NEVERD_API const char *
neverd_session_scan_json(neverd_session_t Sess,
                         const neverd_scan_options *Options);

/// Count recovered instructions referring to the image's data section and
/// relocated pointers stored there that point into non-executable data.
/// Returns owned neverd.data-ptr-scan v1 JSON. NULL on failure or cancellation
/// (see the session error). Cancel and Context are borrowed until return.
NEVERD_API const char *
neverd_session_data_ptr_scan_json(neverd_session_t Sess,
                                  neverd_cancel_probe_fn Cancel, void *Context);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_SAFETY_H
