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
