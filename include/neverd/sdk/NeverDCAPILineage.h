//===- NeverDCAPILineage.h - Offline version graphs -----------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Additive offline graph API over retained comparison reports. Returned
/// strings are owned and must be released with neverd_free_string().
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_LINEAGE_H
#define NEVERD_SDK_CAPI_LINEAGE_H

#include "neverd/sdk/NeverDCAPITypes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *neverd_lineage_t;
typedef void (*neverd_lineage_progress_t)(void *Context, const char *Phase,
                                          neverd_va_t Done, neverd_va_t Total);

NEVERD_API neverd_lineage_t neverd_lineage_create(void);
/// Destroy only after every operation, status and cancellation call returns.
NEVERD_API void neverd_lineage_destroy(neverd_lineage_t Lineage);
/// Thread-safe during an operation. Each admitted operation starts a fresh
/// cancellation lifetime; a late request cannot undo a published replacement.
NEVERD_API void neverd_lineage_cancel(neverd_lineage_t Lineage);
/// Thread-safe and nonblocking with respect to graph computation. Operation
/// outcome is distinct from loaded_revision and historical source states.
NEVERD_API const char *neverd_lineage_status_json(neverd_lineage_t Lineage);
NEVERD_API const char *neverd_lineage_last_error(neverd_lineage_t Lineage);

/// Serialize all following operations on one owner thread. Callbacks may only
/// cancel or read status/error; they must not destroy or reenter the handle.
/// Replace with a strict neverd.bindiff.lineage document. Options is a JSON
/// object or NULL: max_source_bytes, max_nodes, max_edges, max_work,
/// timeout_ms. Both prior and candidate collections and retained trace records
/// are charged during replacement. Source bytes are retained text, not process
/// RSS. The saved reader has separate semantic verification allowances.
/// Cancellation is cooperative: parser/allocation/copy steps are indivisible
/// checkpoints. Returns 1 only on publication, 0 on refusal/stop. The old
/// collection and trace remain available after failure. This never
/// authenticates native files.
NEVERD_API int neverd_lineage_load_json(neverd_lineage_t Lineage,
                                        const char *Document,
                                        const char *Options,
                                        neverd_lineage_progress_t Progress,
                                        void *Context);
/// Additive optional export. Edit is a bounded JSON object containing the exact
/// revision and upsert/remove/parent_updates arrays. Existing release upserts
/// may omit parents to preserve them. The complete candidate is validated and
/// published atomically; failure retains the old collection and trace. Options
/// and callback rules are the same as load_json. Upgrades persistence to v2.
NEVERD_API int neverd_lineage_edit_releases_json(
    neverd_lineage_t Lineage, const char *Edit, const char *Options,
    neverd_lineage_progress_t Progress, void *Context);
NEVERD_API const char *neverd_lineage_summary_json(neverd_lineage_t Lineage);
/// Query requires kind and exact revision; see docs/binary-diff.md. Pages are
/// bounded by the shared row/byte policy. Trace pages reuse a single complete
/// projection by source and revision, independent of offset, limit and kind.
/// Current work and timeout allowances are still honored on a cache hit.
NEVERD_API const char *
neverd_lineage_query_json(neverd_lineage_t Lineage, const char *Query,
                          neverd_lineage_progress_t Progress, void *Context);
/// Complete persistence/export strings may exceed IPC frame limits. Workers
/// must write them locally and return bounded metadata, not send them as pages.
NEVERD_API const char *
neverd_lineage_export_json(neverd_lineage_t Lineage,
                           neverd_lineage_progress_t Progress, void *Context);
/// Exact retained source bytes by report digest; not a reserialized report.
NEVERD_API const char *
neverd_lineage_report_json(neverd_lineage_t Lineage, const char *Report,
                           neverd_lineage_progress_t Progress, void *Context);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_LINEAGE_H
