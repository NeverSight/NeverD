//===- NeverDCAPIDevirtualize.h - Interpreter recovery C API ----*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_SDK_CAPI_DEVIRTUALIZE_H
#define NEVERD_SDK_CAPI_DEVIRTUALIZE_H

#include "neverd/sdk/NeverDCAPITypes.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Context hint relative to the entry value of RSP. It never assumes the slot
/// is initialized or non-aliasing. bytes must be in [1,8].
typedef struct neverd_devirtualize_frame_slot_v1 {
  int64_t offset;
  uint16_t bytes;
  uint16_t reserved;
} neverd_devirtualize_frame_slot_v1;

/// Experimental, bounded x64 interpreter specialization. Control registers
/// select context separation; they never supply concrete entry input values.
/// v1/v2/v3 recovery enables automatic control-state discovery. Its
/// ordinary finite projections do not create context keys. Repeated unresolved
/// memory dependencies may additionally separate proven incoming constants;
/// no extra guest-memory reads or multivalue edge partitions are introduced.
/// Manual hints remain optional context keys; discovery never binds inputs to
/// samples. Refinement restarts share global work and proof budgets.
/// Use full x64 GPR names, e.g. "r10". Null options select default budgets.
/// struct_size must cover this complete v1 structure; future tails are ignored.
typedef struct neverd_devirtualize_options_v1 {
  size_t struct_size;
  const char *const *control_registers;
  size_t control_register_count;
  const neverd_devirtualize_frame_slot_v1 *control_frame_slots;
  size_t control_frame_slot_count;
  uint32_t max_nodes;
  uint32_t max_contexts_per_address;
  uint64_t max_operations;
  int use_llvm;
  int no_opt;
  uint32_t reserved;
} neverd_devirtualize_options_v1;

/// Version 2 adds an explicit control-state refinement budget. Zero the whole
/// structure and set base.struct_size = sizeof(neverd_devirtualize_options_v2).
/// base.struct_size must cover the complete v2 structure; future tails are
/// ignored. Both reserved fields must remain zero. max_control_refinements
/// equal to zero selects the default of 16; other values set a positive limit.
/// v1 entry points continue to ignore this entire extension, including
/// reserved.
typedef struct neverd_devirtualize_options_v2 {
  neverd_devirtualize_options_v1 base;
  uint32_t max_control_refinements;
  uint32_t reserved;
} neverd_devirtualize_options_v2;

/// Version 3 adds explicit control-field and solver-query budgets. Zero the
/// structure and set base.base.struct_size to its complete size. Zero limits
/// select the existing defaults (16 fields and 4096 queries). All reserved
/// fields must be zero. v1/v2 entry points ignore this extension, including
/// reserved; v3 entry points ignore future tails. Larger budgets authorize
/// more bounded work, not partial results or stronger semantic assumptions.
typedef struct neverd_devirtualize_options_v3 {
  neverd_devirtualize_options_v2 base;
  uint32_t max_control_fields;
  uint32_t max_solver_queries;
  uint32_t reserved;
} neverd_devirtualize_options_v3;

typedef enum neverd_devirtualize_flag_v4 {
  NEVERD_DEVIRTUALIZE_V4_DISABLE_CONTROL_DISCOVERY = 1,
  NEVERD_DEVIRTUALIZE_V4_HAS_ENTRY_FRAME_BOUNDS = 2
} neverd_devirtualize_flag_v4;

/// Version 4 adds bounded transfer chaining, optional discovery suppression,
/// and an explicit numeric entry-RSP domain for the machine-state API only.
/// Zero the structure and set base.base.base.struct_size to its complete size.
/// max_chained_transfers == 0 disables chaining; reaching the limit falls back
/// to ordinary CFG boundaries. All operations and proofs remain budgeted.
/// Only the declared flag bits are accepted. All older reserved fields remain
/// zero; v1/v2/v3 entry points ignore this entire extension. v4 ignores future
/// tails. Without HAS_ENTRY_FRAME_BOUNDS, both endpoint fields must be zero.
/// With it, entry_frame_begin < entry_frame_end declares that entry RSP plus
/// every signed offset in [begin,end) fits in [0,UINT64_MAX] without wrapping.
/// This is an unchecked caller precondition, not a concrete stack address or
/// memory accessibility, initialization or nonalias evidence. The emitted C
/// and optional report retain the exact bounds and premise. It does not enable
/// native equivalence proofs or change the existing source execution profile.
typedef struct neverd_devirtualize_options_v4 {
  neverd_devirtualize_options_v3 base;
  uint32_t max_chained_transfers;
  uint32_t flags;
  int64_t entry_frame_begin;
  int64_t entry_frame_end;
} neverd_devirtualize_options_v4;

/// Version 5 adds checked entry-RSP alignment for machine-state source only
/// and a per-node symbolic-DAG budget. max_symbolic_nodes == 0 retains the
/// default of 262144; other values set a positive resource limit, not a
/// premise. Zero the structure and set base.base.base.base.struct_size to its
/// size. entry_frame_alignment == 0 leaves the domain unrestricted and requires
/// entry_frame_residue == 0. Otherwise alignment must be a power of two and
/// residue smaller than alignment. Generated source returns status 2 before
/// guest accesses or state writes when entry RSP modulo alignment != residue.
/// Accepted roots retain unconstrained high bits. No memory or native-proof
/// guarantee is added. v1/v2/v3/v4 ignore this extension; v5 ignores future
/// tails.
typedef struct neverd_devirtualize_options_v5 {
  neverd_devirtualize_options_v4 base;
  uint32_t entry_frame_alignment;
  uint32_t entry_frame_residue;
  uint32_t max_symbolic_nodes;
} neverd_devirtualize_options_v5;

typedef enum neverd_devirtualize_flag_v6 {
  NEVERD_DEVIRTUALIZE_V6_EXTERNAL_STORES_DISJOINT_ENTRY_FRAME = 1u << 0
} neverd_devirtualize_flag_v6;

/// Version 6 adds an unchecked external-STORE separation precondition.
/// Zero-initialize and set base.base.base.base.base.struct_size to the full
/// size. The flag requires machine source and v4 entry-frame bounds: every
/// external-origin STORE's entire extent must avoid that range. No runtime
/// alias check, LOAD guarantee or separation between external pointers is
/// implied. v1-v5 ignore this extension; v6 ignores future tails.
typedef struct neverd_devirtualize_options_v6 {
  neverd_devirtualize_options_v5 base;
  uint32_t flags;
} neverd_devirtualize_options_v6;

/// Return recovered C only when all reachable control targets are resolved.
/// The contract fixes mapped image bytes and permissions, excludes concurrent
/// mutation and calls, and does not certify binary patching or unwind behavior.
/// PE sessions must be loaded without neverd_session_restrict_function():
/// recovery requires complete image-wide relocation and exception metadata.
/// The ordinary source ABI reconstructs invocation-private frame storage. Every
/// external-origin LOAD/STORE range, including computed external addresses,
/// must be disjoint from both the native frame and reconstructed source
/// storage. Frame addresses cannot escape or affect
/// observable scalar results; private reads require complete initialization.
/// This source-frame relocation precondition does not apply to the explicit
/// machine-state API below, which retains guest memory at its original address.
/// Ordinary ABI returns require every external-origin STORE target range to be
/// disjoint from the entry return-address slot (a caller/environment
/// precondition, also for computed external addresses). Frame-derived writes
/// are checked, and stack pivots, callee-pop returns, and RET-based dispatch
/// remain unsupported. On failure returns null and sets the session error.
/// Report, when nonnull, receives an owned JSON-v1 diagnostic/evidence document
/// even on failure. Free both returned strings with neverd_free_string().
/// Session caches are unaffected; recovery executes in its own transaction.
NEVERD_API const char *
neverd_devirtualize_source_v1(neverd_session_t Session, neverd_va_t Entry,
                              const neverd_devirtualize_options_v1 *Options,
                              const char **Report);

/// Explicit x64 machine-state source recovery. The returned function takes a
/// pointer to 17 aligned uint64_t words: RAX, RCX, RDX, RBX, RSP, RBP, RSI,
/// RDI, R8..R15, RFLAGS. It returns uint64_t status; only zero certifies
/// success. Output state is captured before the final native RET address pop.
/// State storage must not alias any guest memory; guest addresses use the
/// original mappings. A little-endian 64-bit host is required.
/// External-origin STORE ranges, including computed addresses, must remain
/// disjoint from the entry return-address slot, as in the default contract.
///
/// Fixed execution profile: CPL3/IOPL0, shadow stacks disabled, no asynchronous
/// events, normal nonfaulting execution. Entry TF/RF/VM/AC/VIF/VIP must be zero
/// and reserved flag bits canonical; executed POPFQ images must keep TF/AC
/// clear. Generated source checks flag-profile violations with a nonzero
/// status. Invalid executions may already have memory effects; no rollback is
/// promised. Internal direct near calls, exhaustively finite register- or
/// memory-indirect near calls, and finite internal returns are supported.
/// Memory calls require a canonical unsegmented r/m64 encoding and a proved
/// target load before the return-address push; unknown or sampled writable
/// slot contents do not establish a target. Exception dispatch and fault/unwind
/// behavior are outside this profile. Other v1 options, report ownership and
/// failure rules match the API above.
NEVERD_API const char *neverd_devirtualize_machine_source_v1(
    neverd_session_t Session, neverd_va_t Entry,
    const neverd_devirtualize_options_v1 *Options, const char **Report);

/// The ordinary source contract and string/report ownership match source_v1.
/// Null options select defaults, including 16 control-state refinements.
NEVERD_API const char *
neverd_devirtualize_source_v2(neverd_session_t Session, neverd_va_t Entry,
                              const neverd_devirtualize_options_v2 *Options,
                              const char **Report);

/// The machine-state ABI, execution profile and ownership match
/// machine_source_v1; only the accepted options structure changes.
NEVERD_API const char *neverd_devirtualize_machine_source_v2(
    neverd_session_t Session, neverd_va_t Entry,
    const neverd_devirtualize_options_v2 *Options, const char **Report);

/// The source contracts and ownership match the corresponding v1/v2 APIs.
/// Null options select unchanged defaults. Reports record effective budgets.
NEVERD_API const char *
neverd_devirtualize_source_v3(neverd_session_t Session, neverd_va_t Entry,
                              const neverd_devirtualize_options_v3 *Options,
                              const char **Report);
NEVERD_API const char *neverd_devirtualize_machine_source_v3(
    neverd_session_t Session, neverd_va_t Entry,
    const neverd_devirtualize_options_v3 *Options, const char **Report);

/// The contracts and ownership match the corresponding v1/v2/v3 APIs. Null
/// options preserve their defaults. The ordinary source API rejects explicit
/// entry-frame bounds; use machine_source_v4 for that numeric precondition.
NEVERD_API const char *
neverd_devirtualize_source_v4(neverd_session_t Session, neverd_va_t Entry,
                              const neverd_devirtualize_options_v4 *Options,
                              const char **Report);
NEVERD_API const char *neverd_devirtualize_machine_source_v4(
    neverd_session_t Session, neverd_va_t Entry,
    const neverd_devirtualize_options_v4 *Options, const char **Report);

/// Contracts and ownership match v4. Ordinary source rejects entry alignment.
NEVERD_API const char *
neverd_devirtualize_source_v5(neverd_session_t Session, neverd_va_t Entry,
                              const neverd_devirtualize_options_v5 *Options,
                              const char **Report);
NEVERD_API const char *neverd_devirtualize_machine_source_v5(
    neverd_session_t Session, neverd_va_t Entry,
    const neverd_devirtualize_options_v5 *Options, const char **Report);

/// Contracts and ownership match v5. Ordinary source rejects frame separation.
NEVERD_API const char *
neverd_devirtualize_source_v6(neverd_session_t Session, neverd_va_t Entry,
                              const neverd_devirtualize_options_v6 *Options,
                              const char **Report);
NEVERD_API const char *neverd_devirtualize_machine_source_v6(
    neverd_session_t Session, neverd_va_t Entry,
    const neverd_devirtualize_options_v6 *Options, const char **Report);

#ifdef __cplusplus
}
#endif
#endif
