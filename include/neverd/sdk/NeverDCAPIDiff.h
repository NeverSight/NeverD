//===- NeverDCAPIDiff.h - Native structural comparison ------------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Additive structural comparison API. Legacy name/text diff APIs retain
/// their contracts. All JSON uses versioned neverd.bindiff schemas and all
/// returned strings are owned allocations freed with neverd_free_string().
/// Similarity/confidence are heuristic; no result proves semantic equivalence.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_DIFF_H
#define NEVERD_SDK_CAPI_DIFF_H

#include "neverd/sdk/NeverDCAPITypes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *neverd_comparison_t;
typedef void (*neverd_comparison_progress_t)(void *Context, const char *Phase,
                                             neverd_va_t Done,
                                             neverd_va_t Total);

NEVERD_API neverd_comparison_t neverd_comparison_create(void);
/// Destroy after run and every concurrent cancel/status call has returned.
NEVERD_API void neverd_comparison_destroy(neverd_comparison_t Comparison);
/// Run once per handle, on the caller's background thread if desired. Sessions
/// must remain alive and exclusively owned for the duration of this call. The
/// completed snapshot owns its data and survives destruction of both sessions.
/// Options is a bounded JSON object (NULL means defaults). Profile defaults to
/// native-same-isa-v1, selecting qualified x86/x64 or AArch64 inputs with the
/// same ISA and ABI. See docs/binary-diff.md for options and evidence limits.
/// Progress runs on this thread; it may request cancellation or read status,
/// but must not destroy the handle or invoke other operations on it.
/// Returns 1 for a published result, including an explicitly partial result,
/// and 0 on failure; last_error describes a failure. Poll status to distinguish
/// requested cancellation, acknowledged stop and retained partial results.
NEVERD_API int
neverd_comparison_run(neverd_comparison_t Comparison, neverd_session_t Primary,
                      neverd_session_t Secondary, const char *Options,
                      neverd_comparison_progress_t Progress, void *Context);
/// Run with exclusive input ownership. Primary and Secondary must be distinct
/// non-NULL slots containing distinct live sessions, with no concurrent users.
/// After admitting a fresh comparison, this call nulls both slots and owns the
/// sessions until comparison destruction, including if later analysis fails.
/// Invalid ownership arguments or an already-started comparison leave the
/// slots unchanged. The caller must not use or destroy transferred handles.
/// This permits fresh selected-function validation without rebuilding native
/// snapshots; it never caches or persists a proof. Other run semantics match
/// neverd_comparison_run. Input load-progress callbacks are cleared on
/// transfer.
NEVERD_API int neverd_comparison_run_owned(
    neverd_comparison_t Comparison, neverd_session_t *Primary,
    neverd_session_t *Secondary, const char *Options,
    neverd_comparison_progress_t Progress, void *Context);
/// Thread-safe, including before run. Does not claim that computation stopped.
NEVERD_API void neverd_comparison_cancel(neverd_comparison_t Comparison);
/// Thread-safe status/progress; never waits for analysis or matching.
NEVERD_API const char *
neverd_comparison_status_json(neverd_comparison_t Comparison);
NEVERD_API const char *
neverd_comparison_last_error(neverd_comparison_t Comparison);
/// The following calls are only accepted after run returns; serialize them on
/// one owner thread. Concurrent cancel/status remain supported.
/// Bounded metadata/counts for overview panes; no whole-image row arrays.
NEVERD_API const char *
neverd_comparison_summary_json(neverd_comparison_t Comparison);
NEVERD_API const char *
neverd_comparison_export_json(neverd_comparison_t Comparison);
/// Bind a saved comparison to these exact snapshots. Reject stale inputs,
/// settings, revisions and identities, leaving the current result intact.
/// Cooperative cancellation also leaves edits, undo/redo and cached pages
/// intact. Returns 0 on stop/failure and 1 only after complete authentication.
/// Each admitted restore starts a new cancellation request lifetime. Status
/// restore_state describes this operation; state in the saved result retains
/// its historical meaning. The JSON parser and individual allocations/copies
/// are indivisible checkpoints, so cancellation is not a hard latency bound.
NEVERD_API int neverd_comparison_restore_json(neverd_comparison_t Comparison,
                                              const char *Saved);
/// Same transaction, with coordinator-thread progress. The callback may cancel
/// or read status, but must not destroy the handle or invoke other operations.
/// No user callback runs after the new result is committed. A saved timeout
/// does not impose a deadline on this restore.
NEVERD_API int neverd_comparison_restore_json_with_progress(
    neverd_comparison_t Comparison, const char *Saved,
    neverd_comparison_progress_t Progress, void *Context);
/// Page a side's inventory. Query: side (primary/secondary), kind
/// (all/matched/changed/uncertain/ambiguous/unmatched), search, offset, limit,
/// sort and descending. Optional ambiguous_with is a function entry on the
/// opposite side: page every retained ambiguity candidate for that source,
/// rather than only its inventory preview. A valid source with no ambiguity has
/// no rows. Overlapping groups contribute their deduplicated direct candidates.
/// The returned ambiguity object identifies the source, supporting steps and
/// total before search/paging; inventory rows expose candidate_steps. These
/// candidates are hypotheses, not accepted matches. Search/sort/paging apply to
/// the target side as usual. Addresses are hexadecimal strings. Function
/// projections use name for the captured display name and evidence_name for the
/// extracted matching name. Search accepts both; name sorting uses the display
/// name. Display annotations do not change snapshot or report identity. Unknown
/// options, missing source entries and out-of-range pages fail.
NEVERD_API const char *
neverd_comparison_functions_json(neverd_comparison_t Comparison,
                                 const char *Query);
/// One function and its counterpart: blocks, edges, aligned instructions,
/// original bytes/operands/targets, heuristic evidence and explicit coverage.
/// Delta computation is cooperatively cancellable and shares the page cache.
/// Full serialization has a finite instruction allowance; prefer pages.
NEVERD_API const char *
neverd_comparison_function_json(neverd_comparison_t Comparison, int Secondary,
                                neverd_va_t Address);
/// Paged aligned instructions. Query: side, address, offset, limit, timeout_ms,
/// refresh. Includes function metadata and only the block pairs touched by
/// this page. A cancelled/timed-out delta has computation_complete:false and
/// no fabricated unchanged rows. Its exact query remains cached until refresh.
NEVERD_API const char *
neverd_comparison_function_page_json(neverd_comparison_t Comparison,
                                     const char *Query);
/// One side's ordinary source page from comparison-owned inputs. Requires a
/// completed native capture through run_owned; original paths are never
/// reopened. Query: side, address, representation (c/llvmc/source/cpp/rust/go),
/// offset, limit, max_native_instructions (0 removes this additional
/// allowance), timeout_ms and refresh. Reuses the SDK source emitter and its
/// page budgets. Every page carries input revision/hash and explicit
/// availability/coverage. Synthetic rows remain unmapped; comparison_anchors
/// are original instructions present in this snapshot. Optional
/// listing_row/native_change refer to a retained completed instruction
/// alignment, never a source-line alignment. Cancel/deadline checks run between
/// selected-function analysis and rendering, not inside synchronous pipeline
/// work. Stopped requests publish no source text. Source projection does not
/// establish semantic equivalence or patch presence.
NEVERD_API const char *
neverd_comparison_source_page_json(neverd_comparison_t Comparison,
                                   const char *Query);
/// Exact textual line alignment of an accepted function pair, collected from
/// complete comparison-owned source renderings. Query: side, address,
/// representation, offset, limit, max_native_instructions, max_work,
/// max_source_bytes, include_prelude, timeout_ms and refresh. Zero removes each
/// additional work/byte/instruction allowance. Pages reuse one retained pair;
/// mapping edits retire it. Stopped computations have no classified rows and
/// require refresh to retry the same settings. Rows retain original source
/// coordinates and checked instruction anchors. Changed rows pair adjacent
/// insertions/deletions for display only; no statement correspondence or
/// semantic equivalence is established. Ordinary source inspection remains
/// independently available. Cancellation is cooperative, with checks between
/// synchronous analysis/rendering and within line collection/alignment.
NEVERD_API const char *
neverd_comparison_source_difference_json(neverd_comparison_t Comparison,
                                         const char *Query);
/// Bounded CFG topology for a viewer: side, address, node_limit, edge_limit.
/// Returns explicit failure when the caller's graph allowance is exceeded.
/// Also accepts timeout_ms and refresh for shared delta computation. A stopped
/// delta retains native topology with not_compared nodes, never unchanged.
NEVERD_API const char *
neverd_comparison_graph_json(neverd_comparison_t Comparison, const char *Query);
/// Ranked local change observations for one accepted function pair. Query:
/// side, address, context (all or an observation context name), max_rows,
/// max_observations, max_work, timeout_ms, refresh, offset, limit. SSA-specific
/// facts require retained graphs; missing/ambiguous graphs preserve native
/// observations. Aligned native rows also anchor changed SSA operands at
/// unchanged instructions; unique direct synthetic operands name their native
/// anchor nodes without claiming original addresses. Deeper expressions are
/// outside this profile. Rankings and dependency context are review heuristics,
/// not vulnerability, fix-presence or semantic-validation verdicts.
NEVERD_API const char *
neverd_comparison_observations_json(neverd_comparison_t Comparison,
                                    const char *Query);
/// Fresh conditional scalar validation of an accepted function pair. Query:
/// side, address, max_native_instructions, max_work, timeout_ms. Requires two
/// live sessions; their native snapshots are rebound before lifting (no SSA
/// recapture). No session or LLVM object is retained. The x64 profile compares
/// integer returns for defined bitvector inputs at identical GPR positions.
/// Proved is model equivalence only; unproved does not mean inequivalent.
/// Every request reruns; saved comparisons contain no trusted proof verdicts.
NEVERD_API const char *neverd_comparison_scalar_validation_json(
    neverd_comparison_t Comparison, neverd_session_t Primary,
    neverd_session_t Secondary, const char *Query);
/// Same bounded model query using the exclusive sources accepted by run_owned.
/// Requires both native input snapshots to have completed. The original loaded
/// inputs remain authoritative even if their source paths later change. Each
/// query freshly lifts/proves the selected pair; it does not re-read or rebind
/// the complete binaries. Reports distinguish this mode from live revalidation.
NEVERD_API const char *
neverd_comparison_owned_scalar_validation_json(neverd_comparison_t Comparison,
                                               const char *Query);
/// Append one explicit manual assessment. Fields: primary/secondary entries,
/// case, author, note, reference, assessment, expected_head. The pair must be
/// accepted now. History is independent of mapping undo and computed evidence.
NEVERD_API int
neverd_comparison_review_record_json(neverd_comparison_t Comparison,
                                     const char *Record);
/// Page manual review history: optional case, side/address filter, latest
/// (latest event per case/function pair), offset and limit. Rows include
/// current pair binding separately from whether this is the latest assessment.
/// Input identities, global head and explicit manual/unvalidated source are
/// retained.
NEVERD_API const char *
neverd_comparison_reviews_json(neverd_comparison_t Comparison,
                               const char *Query);
/// Paginated region hypotheses, requested with run option regions:true (or
/// a bounded region settings object). Query: offset, limit, optional kind.
/// With region_id, returns that hypothesis's anchor page instead.
/// With all_anchors:true, pages all anchors, including unclassified partial
/// evidence; this is exclusive with region_id. These are
/// overlapping explanations of shared native runs, not accepted function pairs
/// or semantic certificates. Every location names its snapshot revision.
NEVERD_API const char *
neverd_comparison_regions_json(neverd_comparison_t Comparison,
                               const char *Query);
/// Paired original instructions of one region anchor. Query: anchor (global
/// index from the anchor page), offset, limit, optional region_id to require
/// membership in that hypothesis. Does not mutate
/// the accepted one-to-one mapping or hide uncovered function instructions.
NEVERD_API const char *
neverd_comparison_region_page_json(neverd_comparison_t Comparison,
                                   const char *Query);
/// SSA region hypotheses from flow_regions:true or a bounded settings object.
/// Query: offset, limit, optional kind (relationship filter); region_id pages
/// its anchors; all_anchors:true includes unclassified partial evidence. With
/// anchor (global index), returns paired nodes and original instructions, or
/// coordinates:"phi_blocks" for verified PHI block pairs. Optional region_id
/// requires membership. Settings fragments:true selects finite shared fragments
/// with explicit depth and frontier_count. coordinates:"frontier" pages their
/// uninspected input definitions; these bindings are not matched code and do
/// not contribute to coverage. Legacy neighborhood recipes remain unchanged.
/// Mutually exclusive selectors and invalid coordinates
/// fail clearly. Correspondence is finite structural evidence, not equivalence.
NEVERD_API const char *
neverd_comparison_flow_regions_json(neverd_comparison_t Comparison,
                                    const char *Query);
/// Bounded SSA observations from run option dataflow:true or settings object.
/// Settings profile: med-ssa-v2 (default), llvm-ssa-v1 (optimized integer
/// SSA), llvm-parameter-ssa-v1 (eligible native low-bit parameter views), or
/// llvm-progress-ssa-v1 (parameter views and independently proved progress),
/// llvm-progress-ssa-v2 (also single-entry multi-block counter components),
/// llvm-progress-ssa-v3 (also nested single-entry counter components),
/// llvm-progress-ssa-v4 (also shared-counter branches and compiler latches),
/// llvm-progress-ssa-v5 (also affine guards and wrapping counter cycles),
/// llvm-progress-ssa-v6 (also parameter-relative equality exit guards), or
/// llvm-progress-ssa-v7 (also pure input-derived expression bounds).
/// Schema 11 adds proof:invariant_modular; the complete expression must have
/// no PHI/freeze/cyclic dependency and all guards must share its basis.
/// Schema 10 also records proof:parameter_modular, derived from shared
/// parameter-bound exits. It does not prove an iteration count or normal
/// return. Schema 6 witnesses add header to block
/// (latch)/counter/update/compare. Schema 8 counts loops, physical backedges
/// and consuming guards separately. Its witness block is the guard block; latch
/// names the backedge source. Guard-node pages retain every witness and
/// authoritative complete totals. Schema 7 retains those fields for every
/// proved nesting layer. Only latch-node pages carry witnesses; v1/schema5 and
/// v2/schema6 remain unchanged. Schema 4 retains parameter source/view widths,
/// optional native site and elision counts; paged nodes expose only their
/// referenced parameter records. Parameter rows consume the same retained-item
/// allowances as nodes; node counts/coverage still count operations. LLVM
/// schema 3 retains exact semantics and plural {address,sequence} origins;
/// unsupported effects/types remain unavailable with reasons. Region nodes
/// return plural originals rather than inventing a singular native origin.
/// Query: side, kind (functions/nodes/blocks), offset, limit; nodes/blocks also
/// require address. kind:accesses additionally selects a call node with call,
/// and accepts max_records, max_work, timeout_ms and refresh. It observes exact
/// local SSA address relationships to recovered arguments under med-ssa-v2;
/// caller reads/writes do not establish callee effects, execution order or
/// complete alias information. RETURN operand paths follow equal-width COPY/PHI
/// edges only; a RETURN input can be a control target rather than an ABI
/// result. The graph's gaps remain visible. One graph index is cached across
/// calls/pages; stopped inspections require refresh. Concurrent cancel/status
/// are supported. Unavailable functions return their explicit reason. Saved
/// graphs bind to fresh native pipeline evidence; hashes are not equivalence.
NEVERD_API const char *
neverd_comparison_dataflow_json(neverd_comparison_t Comparison,
                                const char *Query);
/// Dependency slice: side, address, seeds (bounded node IDs), direction
/// (backward/forward), control (default true), timeout_ms, offset, limit.
/// Bounded work and effect gaps remain visible. Concurrent cancel/status are
/// supported; repeated pages reuse the last exact request's retained slice.
/// refresh:true recomputes it, including a previously stopped request.
NEVERD_API const char *
neverd_comparison_slice_json(neverd_comparison_t Comparison, const char *Query);
/// Observed call reachability, independently of SSA extraction. Query: side,
/// seeds (function address strings), direction (callers/callees), kind
/// (functions/entrypoints/sites/boundaries/frontier/path/effects). path
/// requires witness (a retained function row). All views share offset/limit,
/// max_depth, max_functions, max_sites, max_work, timeout_ms and refresh. One
/// exact request is cached across ordinary page kinds; effects has its own
/// recipe. Restored inputs recompute native facts. Entry traits are observed,
/// never inferred from absence of callers. Global unresolved sites remain
/// visible even without a discovered path to a seed. This is possible call
/// reachability, not proven interprocedural data impact. Concurrent
/// cancel/status are supported; original bytes accompany sites. effects reviews
/// every indexed transfer in a reached caller. It also accepts
/// max_effect_records/max_effect_work and requires optional retained SSA to
/// qualify exact import contracts through Safety. Unknowns stay visible;
/// argument roles and fallible-release conditions do not establish data flow,
/// capacity, must-release, path safety or fix presence. One deadline covers
/// fresh native and effect work; effect inspection completeness is independent
/// of native traversal/discovery coverage. Stopped pages require refresh.
NEVERD_API const char *
neverd_comparison_impact_json(neverd_comparison_t Comparison,
                              const char *Query);
/// Override: {kind: match/unmatch/confirm, primary: "0x...", secondary:
/// "0x..."}. Successful edits are undoable; a new edit discards the redo
/// branch. Every edit starts a fresh cancellation lifetime. An acknowledged
/// stop preserves the result, history and cached views; status edit_state
/// describes the operation independently of the saved comparison's state.
NEVERD_API int neverd_comparison_override_json(neverd_comparison_t Comparison,
                                               const char *Override);
NEVERD_API int neverd_comparison_undo(neverd_comparison_t Comparison);
NEVERD_API int neverd_comparison_redo(neverd_comparison_t Comparison);
/// Optional progress exports for cancellable edit clients. Progress runs on
/// the coordinator thread and may cancel or read status. It ends before commit;
/// do not destroy the handle or invoke other operations from the callback.
/// Staging copies and final publication are indivisible checkpoints.
NEVERD_API int neverd_comparison_override_json_with_progress(
    neverd_comparison_t Comparison, const char *Override,
    neverd_comparison_progress_t Progress, void *Context);
NEVERD_API int
neverd_comparison_undo_with_progress(neverd_comparison_t Comparison,
                                     neverd_comparison_progress_t Progress,
                                     void *Context);
NEVERD_API int
neverd_comparison_redo_with_progress(neverd_comparison_t Comparison,
                                     neverd_comparison_progress_t Progress,
                                     void *Context);
/// Re-extract current inputs and check revision/content/direction against the
/// snapshots. Returns 0 on stale or unsupported input; never silently rebinds.
NEVERD_API int neverd_comparison_validate_inputs(neverd_comparison_t Comparison,
                                                 neverd_session_t Primary,
                                                 neverd_session_t Secondary);

#ifdef __cplusplus
}
#endif
#endif // NEVERD_SDK_CAPI_DIFF_H
