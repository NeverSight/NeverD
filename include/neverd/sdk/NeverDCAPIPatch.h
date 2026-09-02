//===- NeverDCAPIPatch.h - C API patching and rewrite pipeline ----*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Everything that produces a new binary or measures the machinery that
/// does: the patch entry points and their transform-pass toggles, the
/// one-shot lift / patch / decompile pipeline operations, the EVM and
/// Solana SBF target configuration those consult, and the benchmarks.
///
/// All returned strings are heap-allocated via strdup(); callers must
/// free them with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_PATCH_H
#define NEVERD_SDK_CAPI_PATCH_H

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
// Patch operations
// ===--------------------------------------------------------------------===//

/// Native patch installation strategy used by binary-sanitizer-v1.  The enum
/// supplies names only; the public carrier is fixed-width even when a consumer
/// is compiled with -fshort-enums.
enum neverd_sanitize_strategy {
  NEVERD_SANITIZE_STRATEGY_SECTION = 0,
  NEVERD_SANITIZE_STRATEGY_INPLACE = 1
};
typedef uint32_t neverd_sanitize_strategy_t;

/// Stable terminal status for one sanitizer publication transaction.
enum neverd_sanitize_status {
  NEVERD_SANITIZE_STATUS_OK = 0,
  NEVERD_SANITIZE_STATUS_INVALID_ARGUMENT = 1,
  NEVERD_SANITIZE_STATUS_INVALID_SESSION = 2,
  NEVERD_SANITIZE_STATUS_NOT_LOADED = 3,
  NEVERD_SANITIZE_STATUS_UNSUPPORTED_TARGET = 4,
  NEVERD_SANITIZE_STATUS_PIPELINE_FAILED = 5,
  NEVERD_SANITIZE_STATUS_INCOMPLETE_COVERAGE = 6,
  NEVERD_SANITIZE_STATUS_HUNT_INCOMPLETE = 7,
  NEVERD_SANITIZE_STATUS_METADATA_INVALID = 8,
  NEVERD_SANITIZE_STATUS_PLAN_INCOMPLETE = 9,
  NEVERD_SANITIZE_STATUS_GUARD_FAILED = 10,
  NEVERD_SANITIZE_STATUS_IO_FAILED = 11,
  NEVERD_SANITIZE_STATUS_PATCH_FAILED = 12,
  NEVERD_SANITIZE_STATUS_RECEIPT_MISMATCH = 13,
  NEVERD_SANITIZE_STATUS_RELOAD_FAILED = 14,
  NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED = 15,
  NEVERD_SANITIZE_STATUS_PUBLISH_FAILED = 16,
  NEVERD_SANITIZE_STATUS_SIGNATURE_UNSUPPORTED = 17,
  NEVERD_SANITIZE_STATUS_SIGNING_FAILED = 18,
  NEVERD_SANITIZE_STATUS_PUBLISH_INDETERMINATE = 19,
  NEVERD_SANITIZE_STATUS_PUBLISHED_INCOMPLETE = 20
};
/// Fixed-width ABI carrier; the enum above supplies only named constants.
typedef uint32_t neverd_sanitize_status_t;

/// Public outcome of the one allowed namespace publication attempt.  A
/// successful authenticated no-change receipt is NOT_PUBLISHED, not failure.
enum neverd_sanitize_publication_outcome {
  NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_ATTEMPTED = 0,
  NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_PUBLISHED = 1,
  NEVERD_SANITIZE_PUBLICATION_OUTCOME_PUBLISHED = 2,
  NEVERD_SANITIZE_PUBLICATION_OUTCOME_INDETERMINATE = 3
};
typedef uint32_t neverd_sanitize_publication_outcome_t;

enum neverd_sanitize_publication_namespace {
  NEVERD_SANITIZE_PUBLICATION_NAMESPACE_NONE = 0,
  NEVERD_SANITIZE_PUBLICATION_NAMESPACE_CREATE_EXCLUSIVE = 1,
  NEVERD_SANITIZE_PUBLICATION_NAMESPACE_NO_CHANGE = 2
};
typedef uint32_t neverd_sanitize_publication_namespace_t;

enum neverd_sanitize_publication_guarantee {
  NEVERD_SANITIZE_PUBLICATION_GUARANTEE_NAMESPACE_ATOMIC = 1u << 0,
  NEVERD_SANITIZE_PUBLICATION_GUARANTEE_DESTINATION_CREATE_EXCLUSIVE = 1u << 1,
  NEVERD_SANITIZE_PUBLICATION_GUARANTEE_COMPARE_AND_SWAP = 1u << 2,
  NEVERD_SANITIZE_PUBLICATION_GUARANTEE_CRASH_DURABLE = 1u << 3
};
typedef uint32_t neverd_sanitize_publication_guarantees_t;

enum neverd_sanitize_publication_operand_binding {
  NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_NONE = 0,
  NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_ACCESS_CONTROL_CONFINED_DISTINCT_CREDENTIALS =
      1,
  NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_KERNEL_HELD_OBJECT = 2
};
typedef uint32_t neverd_sanitize_publication_operand_binding_t;

enum { NEVERD_SANITIZE_PUBLICATION_ABI_VERSION = 1 };

/// Append-only strict sanitizer options.  Zero-valued budgets select bounded
/// engine defaults.  `struct_size == sizeof(size_t)` is the smallest valid
/// prefix and selects section patching plus all defaults.
typedef struct neverd_sanitize_options_v1 {
  size_t struct_size;
  uint32_t strategy;
  uint32_t max_paths;
  uint32_t max_steps;
  uint32_t max_loop;
  uint64_t solver_conflicts;
  uint32_t max_call_depth;
  uint32_t max_summary_iterations;
} neverd_sanitize_options_v1;

/// Size-gated binary-sanitizer-v1 result.  The caller initializes
/// `struct_size`; the library preserves it and clears only reached later
/// fields at the start of every call.  No field owns heap memory.
typedef struct neverd_sanitize_result_v1 {
  size_t struct_size;
  int ok;
  neverd_sanitize_status_t status;
  uint32_t plan_version;
  uint64_t findings;
  uint64_t guarded_sites;
  uint64_t guarded_functions;
  uint64_t unsupported_sites;
  uint64_t patched_functions;
  uint64_t code_size;
  uint64_t trampoline_count;
  neverd_sanitize_publication_outcome_t publication_outcome;
  uint32_t publication_receipt_version;
  uint32_t publication_receipt_complete;
  neverd_sanitize_publication_namespace_t publication_namespace_disposition;
  neverd_sanitize_publication_guarantees_t publication_guarantee_flags;
  neverd_sanitize_publication_operand_binding_t publication_operand_binding;
} neverd_sanitize_result_v1;

/// Version of the append-only publication receipt contract implemented by this
/// library.  Callers that require authenticated publication must probe this
/// symbol before invoking neverd_session_sanitize so an older library cannot
/// mutate the namespace before the ABI mismatch is discovered.
NEVERD_API uint32_t neverd_sanitize_publication_abi_version(void);

/// Stable non-owned spelling for a sanitizer status.  Unknown values return
/// "invalid".
NEVERD_API const char *
neverd_sanitize_status_name(neverd_sanitize_status_t Status);

/// Analyze and patch the binary already loaded in `Sess`, then publish to
/// `OutputPath` under the receipt contract above.  Returns one only after a
/// complete authenticated publication (or authenticated no-change).  Every
/// failure returns zero and records an actionable message in
/// neverd_last_error().  Existing destinations are never replaced by v1, but
/// PUBLISH_INDETERMINATE and PUBLISHED_INCOMPLETE mean a newly created
/// destination may exist and must be inspected by the caller.
NEVERD_API int
neverd_session_sanitize(neverd_session_t Sess, const char *OutputPath,
                        const neverd_sanitize_options_v1 *Options,
                        neverd_sanitize_result_v1 *Result);

NEVERD_API int neverd_patch_from_ir(neverd_session_t Sess, const char *IRText,
                                    int Strategy, const char *OutputPath);
NEVERD_API int neverd_patch_from_c(neverd_session_t Sess, const char *CText,
                                   neverd_va_t FuncAddr,
                                   const char *OutputPath);

/// Enable/disable the instruction-substitution pass for subsequent
/// patch operations.  When enabled, every patch entry point (from-ir, from-c,
/// full) runs the IR pass before codegen, replacing integer add/sub/and/or/xor
/// with equivalent instruction sequences.  \p Rounds (>=1) repeats the pass.
/// This is a demo-level sample obfuscation transform; more passes will follow.
NEVERD_API void neverd_set_inst_substitution(neverd_session_t Sess, int Enable,
                                             int Rounds);

/// Number of operators substituted by the most recent patch (0 if disabled).
NEVERD_API int neverd_patch_substitution_count(neverd_session_t Sess);

/// Enable/disable the constant-encryption pass for subsequent patch
/// operations.  When enabled, every patch entry point (from-ir, from-c, full)
/// runs the IR pass before codegen, replacing integer constant operands of
/// binary operators / comparisons with run-time-decrypted values.  This is a
/// demo-level sample obfuscation transform; more passes will follow.
NEVERD_API void neverd_set_constant_encryption(neverd_session_t Sess,
                                               int Enable);

/// Number of constant operands encrypted by the most recent patch (0 if
/// disabled).
NEVERD_API int neverd_patch_constant_encryption_count(neverd_session_t Sess);

/// Enable/disable the opaque-predicate pass for subsequent patch operations.
/// When enabled, every patch entry point (from-ir, from-c, full) runs the IR
/// pass before codegen, guarding basic blocks behind an always-true predicate
/// the backend cannot fold.  This is a demo-level sample obfuscation transform;
/// more passes will follow.
NEVERD_API void neverd_set_opaque_predicate(neverd_session_t Sess, int Enable);

/// Number of opaque predicates inserted by the most recent patch (0 if
/// disabled).
NEVERD_API int neverd_patch_opaque_predicate_count(neverd_session_t Sess);

/// Enable/disable the control-flow flattening pass for subsequent patch
/// operations.  When enabled, every patch entry point (from-ir, from-c, full)
/// runs the IR pass before codegen, rebuilding each function's control-flow
/// graph as a dispatcher loop.  This is a demo-level sample obfuscation
/// transform; more passes will follow.
NEVERD_API void neverd_set_control_flow_flattening(neverd_session_t Sess,
                                                   int Enable);

/// Number of basic blocks flattened by the most recent patch (0 if disabled).
NEVERD_API int
neverd_patch_control_flow_flattening_count(neverd_session_t Sess);

/// Enable/disable the bogus-control-flow pass for subsequent patch operations.
/// When enabled, every patch entry point (from-ir, from-c, full) runs the IR
/// pass before codegen, growing a dead, opaque-guarded fake control-flow
/// sub-graph around each basic block.  This is a demo-level sample obfuscation
/// transform; more passes will follow.
NEVERD_API void neverd_set_bogus_control_flow(neverd_session_t Sess,
                                              int Enable);

/// Number of basic blocks given a bogus sub-graph by the most recent patch
/// (0 if disabled).
NEVERD_API int neverd_patch_bogus_control_flow_count(neverd_session_t Sess);

/// Enable/disable the indirect-branch pass for subsequent patch operations.
/// When enabled, every patch entry point (from-ir, from-c, full) runs the IR
/// pass before codegen, rewriting two-way conditional branches into
/// table-driven `indirectbr`.  This is a demo-level sample obfuscation
/// transform; more passes will follow.
NEVERD_API void neverd_set_indirect_branch(neverd_session_t Sess, int Enable);

/// Number of conditional branches converted to indirect branches by the most
/// recent patch (0 if disabled).
NEVERD_API int neverd_patch_indirect_branch_count(neverd_session_t Sess);

/// Enable/disable the indirect-call pass for subsequent patch operations.
/// When enabled, every patch entry point (from-ir, from-c, full) runs the IR
/// pass before codegen, rewriting direct calls to defined functions into
/// position-independent indirect calls through an opaque function pointer.
/// This is a demo-level sample obfuscation transform; more passes will follow.
NEVERD_API void neverd_set_indirect_call(neverd_session_t Sess, int Enable);

/// Number of direct calls converted to indirect calls by the most recent patch
/// (0 if disabled).
NEVERD_API int neverd_patch_indirect_call_count(neverd_session_t Sess);

/// Enable/disable the mixed-boolean-arithmetic pass for subsequent patch
/// operations.  When enabled, every patch entry point (from-ir, from-c, full)
/// runs the IR pass before codegen, injecting a provably-zero MBA term into
/// every integer add/sub/mul/and/or/xor result.  This is a demo-level sample
/// obfuscation transform; more passes will follow.
NEVERD_API void neverd_set_mba(neverd_session_t Sess, int Enable);

/// Number of operators wrapped with an MBA term by the most recent patch
/// (0 if disabled).
NEVERD_API int neverd_patch_mba_count(neverd_session_t Sess);

/// Enable/disable the indirect global-variable pass for subsequent patch
/// operations.  When enabled, every patch entry point (from-ir, from-c, full)
/// runs the IR pass before codegen, rewriting direct references to defined
/// globals into position-independent indirect addresses through an opaque
/// pointer.  This is a demo-level sample obfuscation transform; more passes
/// will follow.
NEVERD_API void neverd_set_indirect_global(neverd_session_t Sess, int Enable);

/// Number of global-variable references made indirect by the most recent patch
/// (0 if disabled).
NEVERD_API int neverd_patch_indirect_global_count(neverd_session_t Sess);

/// Enable/disable the value-laundering pass for subsequent patch operations.
/// When enabled, every patch entry point (from-ir, from-c, full) runs the IR
/// pass before codegen, routing integer (scalar / integer-vector) instruction
/// results through a volatile stack slot and redirecting their uses to the
/// reloaded value.  This is a demo-level sample obfuscation transform; more
/// passes will follow.
NEVERD_API void neverd_set_value_launder(neverd_session_t Sess, int Enable);

/// Number of values laundered by the most recent patch (0 if disabled).
NEVERD_API int neverd_patch_value_launder_count(neverd_session_t Sess);

/// Enable/disable the constant-pooling pass for subsequent patch operations.
/// When enabled, integer constant operands of binary operators / comparisons
/// are moved into a pass-created read-only global pool and fetched at run time
/// through an opaque index.  This is a demo-level sample obfuscation transform;
/// more passes will follow.
NEVERD_API void neverd_set_constant_pooling(neverd_session_t Sess, int Enable);

/// Number of constant operands pooled by the most recent patch (0 if disabled).
NEVERD_API int neverd_patch_constant_pooling_count(neverd_session_t Sess);

/// Enable/disable the bit-masking pass for subsequent patch operations.  When
/// enabled, every patch entry point (from-ir, from-c, full) runs the IR pass
/// before codegen, replacing integer (scalar / integer-vector) results with the
/// bitwise identity `(x & m) | (x & ~m)` where the two masks come from
/// independent volatile slots (so the backend cannot fold them away).  This is
/// a demo-level sample obfuscation transform; more passes will follow.
NEVERD_API void neverd_set_bit_masking(neverd_session_t Sess, int Enable);

/// Number of values bit-masked by the most recent patch (0 if disabled).
NEVERD_API int neverd_patch_bit_masking_count(neverd_session_t Sess);

/// Force the original code-section name used by subsequent patch operations.
/// Pass the section that holds the code to patch when it is not the canonical
/// ".text"/"__text" — e.g. a binary processed by a packer/protector that
/// renamed it (VMProtect ".vmp0", UPX "UPX1", Themida, randomised names).
/// Pass NULL or "" to clear the override and use the format default.  Applies
/// to in-place rewriting (all formats) and to COFF/Mach-O section-mode
/// patching; ELF section-mode patching is segment-based and ignores it.
NEVERD_API void neverd_set_text_section(neverd_session_t Sess,
                                        const char *Name);

NEVERD_API const char *neverd_patch_output_path(neverd_session_t Sess);
NEVERD_API unsigned long long neverd_patch_code_size(neverd_session_t Sess);
NEVERD_API int neverd_patch_trampoline_count(neverd_session_t Sess);

// ===--------------------------------------------------------------------===//
// High-level pipeline operations (lift / patch / decompile)
//
// These wrap Pipeline + Codegen + Emitter so that consumer TUs (CLI, GUI)
// never instantiate LLVM PassManager templates — avoiding weak_def symbol
// duplication across shared-library boundaries.
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_lift_module(neverd_session_t Sess,
                                          const char *InputPath, int NoOpt,
                                          int MaxFunctions);

/// Lift a native binary → LLVM IR → relocatable object in one call. EVM
/// bytecode is rejected because LLVM target object emission has no EVM ABI.
/// Returns 0 on success. Use neverd_roundtrip_* getters for results.
NEVERD_API int neverd_lift_to_obj(neverd_session_t Sess, const char *InputPath,
                                  int NoOpt, int MaxFunctions);
NEVERD_API const char *neverd_roundtrip_ir(neverd_session_t Sess);
NEVERD_API const unsigned char *
neverd_roundtrip_obj(neverd_session_t Sess, unsigned long long *OutLen);
NEVERD_API int neverd_roundtrip_func_count(neverd_session_t Sess);
NEVERD_API const char *neverd_roundtrip_func_name(neverd_session_t Sess,
                                                  int Index);
NEVERD_API unsigned long long
neverd_roundtrip_func_offset(neverd_session_t Sess, int Index);
NEVERD_API unsigned long long neverd_roundtrip_func_size(neverd_session_t Sess,
                                                         int Index);
NEVERD_API int neverd_roundtrip_func_param_count(neverd_session_t Sess,
                                                 int Index);
NEVERD_API const char *neverd_lift_dump(neverd_session_t Sess,
                                        const char *InputPath, int Level,
                                        int MaxFunctions);
NEVERD_API int neverd_patch_full(neverd_session_t Sess, const char *InputPath,
                                 const char *OutputPath, int Strategy,
                                 int NoOpt, int InjectHello, int RunNop,
                                 int MaxFunctions);
NEVERD_API const char *neverd_decompile_all(neverd_session_t Sess,
                                            const char *InputPath,
                                            int UseLlvmRoute, int NoOpt,
                                            int MaxFunctions);
/// Decompile with an explicit output language. Solidity is supported for EVM
/// inputs and Rust is supported for Solana SBF inputs; unsupported
/// input/language combinations return an actionable error.
NEVERD_API const char *
neverd_decompile_all_ex(neverd_session_t Sess, const char *InputPath,
                        neverd_output_language_t Language, int NoOpt,
                        int MaxFunctions);
/// Configure EVM analysis for subsequent session and high-level operations.
NEVERD_API void neverd_evm_set_strict(neverd_session_t Sess, int Strict);
NEVERD_API int neverd_evm_set_hardfork(neverd_session_t Sess,
                                       const char *Hardfork);
/// Configure Solana SBF verification and the optional explicit VM version
/// (`auto`, `v0`, `v1`, `v2`, `v3`, or `v4`) for subsequent operations.
NEVERD_API void neverd_sbf_set_strict(neverd_session_t Sess, int Strict);
NEVERD_API int neverd_sbf_set_version(neverd_session_t Sess,
                                      const char *Version);
/// Supply an Anchor IDL document, whose instruction, account, and event names
/// take precedence over the built-in name dictionary when recovering dispatch.
/// Passing NULL or an empty string clears any previously supplied document.
/// Returns 0 and sets the session error when the document cannot be parsed.
NEVERD_API int neverd_sbf_set_idl(neverd_session_t Sess, const char *Json);
/// Select the runtime a recovered Solana program is described against.
///
/// None of this is in the program file. Which gates are on depends on the
/// cluster and the slot, the shape of the input buffer depends on the loader
/// that owns the program, and whether a syscall resolves depends on whether
/// the question is about running the program or about deploying it. Answering
/// without them means answering for whichever chain the defaults name.
///
/// Cluster accepts `mainnet-beta`, `testnet`, `devnet`, or `localnet`; loader
/// accepts `loader-v1` through `loader-v4`; purpose accepts `execution` or
/// `deployment`. Each returns 0 and sets the session error on an unknown name.
NEVERD_API int neverd_sbf_set_cluster(neverd_session_t Sess,
                                      const char *Cluster);
NEVERD_API void neverd_sbf_set_slot(neverd_session_t Sess, neverd_slot_t Slot);
NEVERD_API int neverd_sbf_set_loader(neverd_session_t Sess, const char *Loader);
NEVERD_API int neverd_sbf_set_purpose(neverd_session_t Sess,
                                      const char *Purpose);
NEVERD_API int neverd_inject_hello(neverd_session_t Sess);
NEVERD_API const char *neverd_disasm_text(neverd_session_t Sess,
                                          const char *FuncNameOrAddr,
                                          int Annotate);
NEVERD_API const char *neverd_xrefs_scan(neverd_session_t Sess,
                                         const char *InputPath,
                                         neverd_va_t Target);
NEVERD_API const char *neverd_cfg_dot(neverd_session_t Sess,
                                      const char *InputPath,
                                      const char *FuncNameOrAddr, int Styled);

// ===--------------------------------------------------------------------===//
// Benchmark support (structured pipeline access for timing)
// ===--------------------------------------------------------------------===//

/// Run the full pipeline and return structured JSON benchmark data:
/// {"func_count":N, "import_count":N, "string_count":N,
///  "low_time_ms":N, "med_time_ms":N, "high_time_ms":N,
///  "llvm_time_ms":N, "total_time_ms":N,
///  "functions":[{"name":"...","entry":"0x...","blocks":N,"ops":N},...]}.
/// Caller frees.
NEVERD_API const char *neverd_bench_run(neverd_session_t Sess,
                                        const char *InputPath,
                                        int MaxFunctions);

/// Measure raw instruction-decode throughput over the loaded image's
/// executable segments and return JSON:
///   {"arch":"...","exec_bytes":N,"insns":N,
///    "full_detail_insns_per_sec":N,"light_insns_per_sec":N,
///    "full_detail_mb_per_sec":F,"light_mb_per_sec":F,
///    "detail_off_speedup":F,
///    "mt_threads":N,"mt_full_detail_insns_per_sec":N,
///    "mt_full_detail_mb_per_sec":F,"mt_scaling":F,
///    "aarch64_blscan_words_per_sec":N,"aarch64_blscan_speedup":F}
/// The "full_detail" figures use the operand-detail decode the lift path needs;
/// "light" uses the detail-free classification decode; the "mt_" figures are
/// the multi-threaded aggregate over workerThreadCount() per-thread decoders
/// (the rate a real pipeline decode achieves), with mt_scaling the wall-clock
/// speedup over the single-threaded full-detail pass; the AArch64 fields (when
/// applicable) measure the fixed-width BL scan used for call-target discovery.
/// Requires a binary to be loaded via neverd_session_load().  Caller frees.
NEVERD_API const char *neverd_bench_decode(neverd_session_t Sess);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_PATCH_H
