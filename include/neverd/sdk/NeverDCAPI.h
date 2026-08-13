//===- NeverDCAPI.h - C API for NeverD decompiler -------------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pure C API that wraps NeverD's C++ Pipeline/Loader/Decoder/Emitter.
/// Used by CLI, Qt GUI, and third-party plugins via libneverd shared library.
///
/// All returned strings are heap-allocated via strdup(); callers must
/// free them with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_H
#define NEVERD_SDK_CAPI_H

#include <stddef.h>

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

typedef void *neverd_session_t;
typedef unsigned long long neverd_va_t;
/// A Solana slot. Distinct from an address because it orders time rather than
/// memory: it is what decides which runtime gates had been activated yet.
typedef unsigned long long neverd_slot_t;

typedef enum neverd_output_language {
#define NEVERD_OUTPUT_LANGUAGE(NAME, VALUE, SPELLING, DISPLAY_NAME)            \
  NEVERD_OUTPUT_##NAME = (VALUE),
#include "neverd/OutputLanguages.def"
} neverd_output_language_t;

// ===--------------------------------------------------------------------===//
// Session lifecycle
// ===--------------------------------------------------------------------===//

NEVERD_API neverd_session_t neverd_session_create(void);
NEVERD_API void neverd_session_destroy(neverd_session_t Sess);
NEVERD_API int neverd_session_load(neverd_session_t Sess, const char *Path);
NEVERD_API int neverd_session_is_loaded(neverd_session_t Sess);

/// Run the full analysis pipeline (lift → optimize → decompile).
/// Call after neverd_session_load() to pre-compute analysis data.
/// Returns 1 on success, 0 on failure.  Thread-safe if called once.
NEVERD_API int neverd_session_analyze(neverd_session_t Sess);

NEVERD_API const char *neverd_session_file_path(neverd_session_t Sess);
NEVERD_API const char *neverd_session_arch_name(neverd_session_t Sess);
NEVERD_API const char *neverd_session_format_name(neverd_session_t Sess);
NEVERD_API int neverd_session_is_64bit(neverd_session_t Sess);
/// Return the target word size (32, 64, or 256), or 0 when unloaded.
NEVERD_API int neverd_session_bitness(neverd_session_t Sess);

// ===--------------------------------------------------------------------===//
// Function list
// ===--------------------------------------------------------------------===//

NEVERD_API int neverd_func_count(neverd_session_t Sess);
NEVERD_API neverd_va_t neverd_func_entry(neverd_session_t Sess, int Idx);
NEVERD_API int neverd_func_size(neverd_session_t Sess, int Idx);
NEVERD_API const char *neverd_func_name(neverd_session_t Sess, int Idx);

// ===--------------------------------------------------------------------===//
// Disassembly (returns JSON array)
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_disasm_json(neverd_session_t Sess,
                                          neverd_va_t Addr, int MaxInsns);

// ===--------------------------------------------------------------------===//
// Decompilation
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_decompile(neverd_session_t Sess,
                                        neverd_va_t FuncEntry);

// ===--------------------------------------------------------------------===//
// Multi-stage IR
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_ir_low(neverd_session_t Sess,
                                     neverd_va_t FuncEntry);
NEVERD_API const char *neverd_ir_med(neverd_session_t Sess,
                                     neverd_va_t FuncEntry);
NEVERD_API const char *neverd_ir_high(neverd_session_t Sess,
                                      neverd_va_t FuncEntry);
NEVERD_API const char *neverd_ir_llvm(neverd_session_t Sess,
                                      neverd_va_t FuncEntry);

// ===--------------------------------------------------------------------===//
// Function lookup helpers
// ===--------------------------------------------------------------------===//

NEVERD_API int neverd_func_find_by_name(neverd_session_t Sess,
                                        const char *Name);
NEVERD_API int neverd_func_find_by_addr(neverd_session_t Sess,
                                        neverd_va_t Addr);

// ===--------------------------------------------------------------------===//
// Raw bytes
// ===--------------------------------------------------------------------===//

NEVERD_API int neverd_read_bytes(neverd_session_t Sess, neverd_va_t Addr,
                                 unsigned char *Buf, int Size);

// ===--------------------------------------------------------------------===//
// Info panels (return JSON)
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_imports_json(neverd_session_t Sess);
NEVERD_API const char *neverd_exports_json(neverd_session_t Sess);
NEVERD_API const char *neverd_segments_json(neverd_session_t Sess);
NEVERD_API const char *neverd_strings_json(neverd_session_t Sess,
                                           int MinLength);
NEVERD_API const char *neverd_xrefs_to_json(neverd_session_t Sess,
                                            neverd_va_t Addr);
NEVERD_API const char *neverd_xrefs_from_json(neverd_session_t Sess,
                                              neverd_va_t Addr);

// ===--------------------------------------------------------------------===//
// CFG graph (returns JSON: nodes + edges)
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_cfg_json(neverd_session_t Sess,
                                       neverd_va_t FuncEntry);

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
// Error handling
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_last_error(neverd_session_t Sess);

// ===--------------------------------------------------------------------===//
// Memory management
// ===--------------------------------------------------------------------===//

NEVERD_API void neverd_free_string(const char *Str);

// ===--------------------------------------------------------------------===//
// Session metadata
// ===--------------------------------------------------------------------===//

NEVERD_API unsigned long long neverd_session_file_size(neverd_session_t Sess);
NEVERD_API neverd_va_t neverd_session_base_addr(neverd_session_t Sess);
NEVERD_API neverd_va_t neverd_session_entry_addr(neverd_session_t Sess);
NEVERD_API int neverd_session_segment_count(neverd_session_t Sess);
NEVERD_API int neverd_session_section_count(neverd_session_t Sess);
NEVERD_API int neverd_session_import_count(neverd_session_t Sess);
NEVERD_API int neverd_session_export_count(neverd_session_t Sess);
NEVERD_API int neverd_session_symbol_count(neverd_session_t Sess);

NEVERD_API const char *neverd_hex_dump(neverd_session_t Sess, neverd_va_t Addr,
                                       int Size);

// ===--------------------------------------------------------------------===//
// Annotations (per-address user comments, persisted to JSON sidecar file)
// ===--------------------------------------------------------------------===//

NEVERD_API void neverd_annotation_set(neverd_session_t Sess, neverd_va_t Addr,
                                      const char *Text);
NEVERD_API void neverd_annotation_remove(neverd_session_t Sess,
                                         neverd_va_t Addr);
NEVERD_API const char *neverd_annotation_get(neverd_session_t Sess,
                                             neverd_va_t Addr);
NEVERD_API const char *neverd_annotations_json(neverd_session_t Sess);
NEVERD_API int neverd_annotations_save(neverd_session_t Sess);
NEVERD_API int neverd_annotations_load(neverd_session_t Sess);

// ===--------------------------------------------------------------------===//
// Binary diff (function-level comparison between two sessions)
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_diff_functions(neverd_session_t SessA,
                                             neverd_session_t SessB);
NEVERD_API const char *neverd_diff_decompile(neverd_session_t SessA,
                                             neverd_va_t EntryA,
                                             neverd_session_t SessB,
                                             neverd_va_t EntryB);

// ===--------------------------------------------------------------------===//
// Symbol renaming (persisted to JSON sidecar file)
// ===--------------------------------------------------------------------===//

NEVERD_API int neverd_rename_func(neverd_session_t Sess, const char *OldName,
                                  const char *NewName);
NEVERD_API const char *neverd_renames_json(neverd_session_t Sess);
NEVERD_API int neverd_renames_save(neverd_session_t Sess);
NEVERD_API int neverd_renames_load(neverd_session_t Sess);

// ===--------------------------------------------------------------------===//
// Call graph (function-level call relationships)
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_callgraph_json(neverd_session_t Sess);

// ===--------------------------------------------------------------------===//
// Address resolution
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_resolve_addr(neverd_session_t Sess,
                                           neverd_va_t Addr);

// ===--------------------------------------------------------------------===//
// Byte pattern / string search across all segments
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_search_bytes(neverd_session_t Sess,
                                           const unsigned char *Pattern,
                                           int PatternLen, int MaxResults);
NEVERD_API const char *neverd_search_string(neverd_session_t Sess,
                                            const char *Pattern,
                                            int CaseSensitive, int MaxResults);

// ===--------------------------------------------------------------------===//
// Sections / Symbols / Relocations / Headers / Entry points / Dashboard
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_sections_json(neverd_session_t Sess);
NEVERD_API const char *neverd_symbols_json(neverd_session_t Sess);
NEVERD_API const char *neverd_relocs_json(neverd_session_t Sess);
NEVERD_API const char *neverd_headers_json(neverd_session_t Sess);
NEVERD_API const char *neverd_entrypoints_json(neverd_session_t Sess);
NEVERD_API const char *neverd_dashboard_json(neverd_session_t Sess);

// ===--------------------------------------------------------------------===//
// FLIRT signature matching
// ===--------------------------------------------------------------------===//

NEVERD_API int neverd_apply_signatures(neverd_session_t Sess,
                                       const char *SigDir);
NEVERD_API int neverd_auto_apply_signatures(neverd_session_t Sess,
                                            const char *SigBaseDir);
NEVERD_API int neverd_apply_signature_file(neverd_session_t Sess,
                                           const char *SigPath);
NEVERD_API int neverd_sig_match_count(neverd_session_t Sess);
NEVERD_API const char *neverd_sig_matches_json(neverd_session_t Sess);

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

// ===--------------------------------------------------------------------===//
// Signature generation utilities
// ===--------------------------------------------------------------------===//

/// Compute CRC16 over a byte buffer (FLIRT-compatible algorithm).
NEVERD_API unsigned short neverd_sig_compute_crc16(const unsigned char *Data,
                                                   int Length);

// ===--------------------------------------------------------------------===//
// Plugin management
//
// Wraps PluginManager so tools never reference the internal C++ class.
// ===--------------------------------------------------------------------===//

/// Load all supported native (*.dylib / *.dll / *.so) and Python (*.py)
/// plugins from \p Dir in deterministic canonical-path order.
/// Returns the number of plugins successfully loaded.
NEVERD_API int neverd_plugins_load_dir(neverd_session_t Sess, const char *Dir);

/// Load one plugin file. Returns 1 on success or 0 on failure. The native
/// diagnostic is available through neverd_last_error().
NEVERD_API int neverd_plugins_load_file(neverd_session_t Sess,
                                        const char *Path);

/// Return a JSON array of loaded plugins:
///   [{"name":"…","version":"…","author":"…","description":"…",
///     "type":0,"kind":"python","path":"…"},…]
/// Caller frees with neverd_free_string().
NEVERD_API const char *neverd_plugins_list_json(neverd_session_t Sess);

/// Initialize all loaded plugins (calls each plugin's Init callback).
NEVERD_API void neverd_plugins_init(neverd_session_t Sess);

/// Terminate and unload all loaded plugins.
NEVERD_API void neverd_plugins_term(neverd_session_t Sess);

/// Run a specific plugin by name.  Returns the plugin's return code,
/// or -1 if the plugin was not found.
NEVERD_API int neverd_plugins_run(neverd_session_t Sess, const char *Name,
                                  int Arg);

/// Return the number of currently loaded plugins.
NEVERD_API int neverd_plugins_count(neverd_session_t Sess);

/// Dispatch an event to all loaded plugins.
/// Requires the caller to include NeverDPlugin.h for neverd_event_t.
NEVERD_API void neverd_plugins_dispatch_event(neverd_session_t Sess,
                                              const void *Event);

// ===--------------------------------------------------------------------===//
// Expression simplification
// ===--------------------------------------------------------------------===//

/// Why the simplifier handed back the expression it was given.
///
/// "Unchanged" is several different answers wearing one face, and only some of
/// them say anything about the expression rather than about the budget it was
/// given.  A caller deciding whether to spend more needs to tell them apart.
typedef enum neverd_simplify_outcome {
  /// Nothing in the expression belongs to the algebra the engine works in, so
  /// there was nothing to measure.  Spending more would not help.
  NEVERD_SIMPLIFY_NOT_APPLICABLE = 0,
  /// Measured, and no form shorter than what is already there exists.
  NEVERD_SIMPLIFY_ALREADY_SHORTEST = 1,
  /// More inputs than one measurement can afford, and no split into
  /// independent parts or mask-uniform columns was available.  A larger
  /// `max_atoms` reaches it, at twice the cost per input added.
  NEVERD_SIMPLIFY_TOO_MANY_INPUTS = 2,
  /// The layered walk stopped at `max_work` with regions left unvisited.
  NEVERD_SIMPLIFY_BUDGET_EXHAUSTED = 3,
  /// A shorter form was found, and is in `output`.
  NEVERD_SIMPLIFY_REWRITTEN = 4
} neverd_simplify_outcome_t;

/// What stands behind a rewrite that was made.
typedef enum neverd_simplify_evidence {
  /// Nothing was rewritten.
  NEVERD_SIMPLIFY_EVIDENCE_NONE = 0,
  /// The derivation is exact by construction.  A sample check still ran, but as
  /// a net for a mistake in the derivation rather than as the reason to trust
  /// the result.
  NEVERD_SIMPLIFY_EVIDENCE_DERIVATION = 1,
  /// The rewrite holds only under a condition the derivation cannot establish
  /// on its own, and the sample check is what decided it.  Cutting a word into
  /// mask-uniform columns is the case that arises: the reassembly is exact
  /// unless an arithmetic carry crosses a column boundary.
  NEVERD_SIMPLIFY_EVIDENCE_SAMPLES = 2
} neverd_simplify_evidence_t;

/// How to simplify.  Zero the whole struct, set `struct_size`, then set only
/// what you mean to change: every field left zero takes the engine's default.
///
/// `struct_size` is what lets this grow.  A library newer than its caller reads
/// only the fields the caller's struct actually has, so a plugin compiled
/// against an older header keeps working against a newer libneverd instead of
/// having fields read past the end of what it allocated.
typedef struct neverd_simplify_options {
  size_t struct_size;
  /// Width every leaf without a `#bits` suffix is created at.  Zero means 32.
  unsigned width;
  /// Measure one layer only.  The default is the layered walk, which reaches
  /// inside the subterms a single measurement has to treat as opaque; that is
  /// what obfuscated input needs, and it costs more on input already short.
  int shallow;
  /// Most distinct inputs one measurement may span.  The cost is 2^this, so it
  /// is the dial between reach and time.  Zero takes the default.
  unsigned max_atoms;
  /// Graph nodes the layered walk may measure over before it stops starting new
  /// measurements.  Zero takes the default; (size_t)-1 removes the limit.
  size_t max_work;
  /// Random assignments a rewrite is checked against before it is returned.
  /// Zero takes the default.
  unsigned verify_samples;
  /// Return a rewrite even when it reads worse than what it replaces.  For
  /// measuring the engine, not for using it.
  int allow_growth;
} neverd_simplify_options;

/// What became of one expression.  Zero the struct and set `struct_size` before
/// the call; everything else is written by it.  Release it with
/// neverd_simplify_result_dispose() whatever the outcome.
typedef struct neverd_simplify_result {
  size_t struct_size;
  /// Zero when the expression could not be read, in which case `error` and
  /// `error_offset` say what and where, and nothing else is set.
  int ok;
  const char *error;
  size_t error_offset;
  /// The expression as the engine read it, which is already shorter than what
  /// was written whenever building it folded something.
  const char *input;
  const char *output;
  int changed;
  /// What the expression costs a reader, before and after.
  size_t cost_before;
  size_t cost_after;
  /// Distinct inputs the winning measurement spanned.
  unsigned inputs;
  /// Graph nodes the search measured over: what the answer cost to find, as
  /// opposed to how large the answer is.  This is the number to watch when
  /// `max_work` has to be chosen.
  size_t work;
  neverd_simplify_outcome_t outcome;
  neverd_simplify_evidence_t evidence;
  /// Stable spellings of the two above, for a report meant to be read.
  const char *outcome_name;
  const char *evidence_name;
} neverd_simplify_result;

/// Simplify a bitvector expression written in the engine's infix syntax:
/// C operators throughout, calls for the ones C has no spelling for
/// (`sdiv`, `ashr`, `rol`, `zext`, `extract`, …), and an optional `#bits`
/// suffix on any leaf that leaves the ambient width.
///
/// \p Options may be null, which takes every default.  Returns zero on success,
/// including when the expression did not parse -- that is reported through
/// \p Result rather than as a failure of the call -- and non-zero only when the
/// arguments themselves are unusable.
NEVERD_API int neverd_simplify_expr(const char *Expr,
                                    const neverd_simplify_options *Options,
                                    neverd_simplify_result *Result);

/// Release the strings in \p Result.  Safe on a zeroed struct and safe twice.
NEVERD_API void neverd_simplify_result_dispose(neverd_simplify_result *Result);

/// The JSON spelling of the same thing, for callers that had it before the
/// typed entry point existed and for languages where parsing one string beats
/// declaring a struct.  Both go through one implementation.
///
/// \p Width is the width every leaf without a `#bits` suffix is created at.
/// \p Deep asks for the layered walk.
///
/// Returns a JSON object.  Having simplified:
///   {"ok":true, "input":"…", "output":"…", "changed":true,
///    "costBefore":7, "costAfter":3, "inputs":2, "work":19,
///    "outcome":"rewritten", "evidence":"derivation"}
/// On a syntax error:
///   {"ok":false, "error":"expected ')'", "offset":6}
/// Caller frees with neverd_free_string().
NEVERD_API const char *neverd_simplify_expr_json(const char *Expr,
                                                 unsigned Width, int Deep);

// ===--------------------------------------------------------------------===//
// Version info
// ===--------------------------------------------------------------------===//

/// Full version string, e.g. "NeverD v3389.0.1".  Caller frees.
NEVERD_API const char *neverd_version(void);

/// Project name only, e.g. "NeverD".  Caller frees.
NEVERD_API const char *neverd_project_name(void);

/// Version number only, e.g. "3389.0.1".  Caller frees.
NEVERD_API const char *neverd_version_number(void);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_H
