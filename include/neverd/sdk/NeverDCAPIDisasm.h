//===- NeverDCAPIDisasm.h - C API disassembly and decompilation ---*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-function views of a loaded binary: the recovered function table, raw
/// bytes, disassembly, decompiled C, and each intermediate representation.
///
/// All returned strings are heap-allocated via strdup(); callers must
/// free them with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_DISASM_H
#define NEVERD_SDK_CAPI_DISASM_H

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
// Function list
// ===--------------------------------------------------------------------===//

/// Native queries expose loader symbols without starting analysis. After an
/// explicit or lazy analysis succeeds, the list also includes recovered
/// functions; their insertion can change indices returned before analysis.
NEVERD_API int neverd_func_count(neverd_session_t Sess);
NEVERD_API neverd_va_t neverd_func_entry(neverd_session_t Sess, int Idx);
NEVERD_API int neverd_func_size(neverd_session_t Sess, int Idx);
NEVERD_API const char *neverd_func_name(neverd_session_t Sess, int Idx);

/// Lightweight function presentation without decompilation: {entry,
/// suggested_name?,prototype?,thunk,thunk_target?}. A thunk forwards through
/// a direct unconditional branch with no parameter or stack transformation.
/// User/stated names take precedence over suggestions. NULL on failure.
NEVERD_API const char *neverd_function_presentation_json(neverd_session_t Sess,
                                                         neverd_va_t Entry);

/// Format-owned data records in ascending address order, from FirstAddress:
/// {items:[{address,size,type,fields:[{offset,size,name,role,target}]}],
/// next_address:hex|null}. Currently publishes checked x64 PE RUNTIME_FUNCTION
/// rows, including chain records. Boundary references do not establish entries.
/// MaxItems is clamped to the data-slot query limit. No analysis is started.
NEVERD_API const char *neverd_metadata_items_json(neverd_session_t Sess,
                                                  neverd_va_t FirstAddress,
                                                  int MaxItems);

/// Borrowed native view of the same checked metadata facts. All pointers,
/// including strings, are valid only for the duration of the visitor call.
typedef struct neverd_metadata_field_v1 {
  size_t struct_size;
  unsigned long long offset;
  unsigned long long size;
  neverd_va_t target;
  const char *name;
  const char *role;
} neverd_metadata_field_v1;

typedef struct neverd_metadata_item_v1 {
  size_t struct_size;
  neverd_va_t address;
  unsigned long long size;
  const char *type;
  const neverd_metadata_field_v1 *fields;
  size_t field_count;
} neverd_metadata_item_v1;

/// Return nonzero to continue or zero to stop after this item. The visitor
/// must not throw, block or reenter this session. It may copy borrowed facts.
typedef int (*neverd_metadata_visitor_fn)(void *user_data,
                                          const neverd_metadata_item_v1 *item);

/// Visit at most MaxItems records in address order without JSON conversion.
/// Returns the number visited, or -1 with neverd_last_error on failure.
/// NextAddress receives the first unvisited record, or zero at the end,
/// including when the visitor stops early. No target code or analysis runs.
NEVERD_API int
neverd_visit_metadata_items(neverd_session_t Sess, neverd_va_t FirstAddress,
                            int MaxItems, neverd_metadata_visitor_fn Visitor,
                            void *UserData, neverd_va_t *NextAddress);

// ===--------------------------------------------------------------------===//
// Function lookup helpers
// ===--------------------------------------------------------------------===//

/// Find a listed name or an export alias, preferring the listed name. On
/// Mach-O, also accept the C spelling without its one ABI underscore when
/// neither exact spelling matches. The function list keeps its original names.
/// Return -1 when no listed function matches.
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
// Disassembly (returns JSON array)
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_disasm_json(neverd_session_t Sess,
                                          neverd_va_t Addr, int MaxInsns);

/// Options for neverd_disasm_json_ex().
enum {
  /// Add each native instruction's control transfer and constant memory
  /// references, read from the instruction's own LowIR lift.  "flow" is one of
  /// "call", "icall", "jump", "cjump", "ijump" or "ret" and is absent for a
  /// fall-through instruction; it is "unlifted", with no target or references,
  /// for an instruction the lifter cannot model.  "target" is the direct
  /// transfer target when the lift names one.  "refs" lists {"to","kind"} for
  /// constant addresses the instruction reads ("read"), writes ("write") or
  /// takes the address of ("offset"); a read or write adds "size", the bytes
  /// it accesses, when the lift names them.  EVM and SBF rows never carry
  /// these fields.
  NEVERD_DISASM_FLOW = 1u,
  /// Add how each native instruction moves the stack pointer, from the same
  /// lift.  "sp" is the constant the instruction adds to the stack pointer, 0
  /// when it leaves it alone; with "sp_base" it is the constant added to that
  /// register's value before the instruction instead (`leave` is "rbp" plus
  /// 8).  "sp" is null when the lift does not reduce the new stack pointer to
  /// either form, as for an unlifted instruction.  A call leaves the stack
  /// pointer where it was, as the lift models it: the callee pops the return
  /// address, and arguments a callee also pops are not included.  A return
  /// states only what happens before control leaves the function.
  NEVERD_DISASM_STACK = 2u,
  /// Add "operand_bits" for decoded x86/x64 operands, indexed before display
  /// folding. Widths come from the decoder, including the logical width of
  /// an immediate; they are not inferred from a different operand's register.
  NEVERD_DISASM_OPERANDS = 4u
};

/// neverd_disasm_json() with additive row fields selected by \p Options.
/// Rows and their order are identical to neverd_disasm_json().
NEVERD_API const char *neverd_disasm_json_ex(neverd_session_t Sess,
                                             neverd_va_t Addr, int MaxInsns,
                                             unsigned Options);

/// Native instructions recovered for one function, in address order, starting
/// at FirstAddress. Uses the same CFG and recovery checks as the decompiler;
/// skipped bytes are absent, even inside a sized function. Prepares the low
/// function view on demand. Options select the same additive fields as raw
/// disassembly; recovered direct branch targets replace instruction-local
/// indirect targets. Raw neverd_disasm_json[_ex] remains a linear decode.
/// Returns {"instructions":[...],"next_address":"0x..."|null,
/// "lift_complete":bool}. Completeness describes reachable instruction lifting,
/// not proof that every indirect target is known. MaxInsns is clamped to
/// 1..4096. NULL with an error for non-native images or unavailable analysis.
NEVERD_API const char *neverd_function_disasm_json(neverd_session_t Sess,
                                                   neverd_va_t Entry,
                                                   neverd_va_t FirstAddress,
                                                   int MaxInsns,
                                                   unsigned Options);

/// Direct references of up to \p MaxFunctions native functions whose entries
/// are at or above \p FirstEntry, in ascending entry order, from the same
/// LowIR lift as NEVERD_DISASM_FLOW.  The address cursor stays valid when lazy
/// analysis adds a function to the list between calls.  A function with a
/// known size is decoded across its whole extent; one without a size is the
/// code its own direct jumps reach from its entry, short of another listed
/// function's entry (on 32-bit ARM, up to its first function terminator).
/// Returns
/// {"refs":[["from","to","kind"],...],"next_entry":"0x..."|null,
/// "function_count":int}, where kind is "call", "jump", "cjump", "read",
/// "write" or "offset", or "icall"/"ijump" for a call or jump through a pointer
/// slot the loader relocated to code: such a reference names the slot's
/// pointer as loaded, which the program may still overwrite unless the slot
/// is read-only after relocation.  A "read" or "write" row has a fourth
/// element, the bytes it accesses, when the lift names them:
/// ["from","to","read",8].  MaxFunctions is clamped to 1..4096.  EVM and
/// SBF images return NULL with an error, because their analyzers own a
/// different instruction model.  This query never starts analysis.
NEVERD_API const char *neverd_code_refs_json(neverd_session_t Sess,
                                             neverd_va_t FirstEntry,
                                             int MaxFunctions);

/// The jump tables whole-program analysis recovered in up to
/// \p MaxFunctions analyzed functions from \p FirstEntry in entry order.
/// Returns {"switches":[{"function","jump","load","table","entry_size",
/// "stride","storage":[["0x...",entry_size,stride,slots],...],"form",
/// "targets":[["target",index,slot],...]},...],"next_entry":"0x..."|null}:
/// "jump" is the dispatch, "load" the instruction that reads the table,
/// "table" the address of its slot 0 and "storage" the runs of slots it owns.
/// Each target carries the index that selects it, the value the dispatch
/// indexes the table with (JumpTable::caseLabel; a switch value the
/// decompiler shows may add a constant to it), and its physical slot, at
/// "table" plus the slot times "stride" (null when the table maps none).
/// "form" says how every such slot stores its target: "absolute",
/// "table_relative" (the table's address plus the entry) or "image_relative"
/// (the image base plus the entry); it is null for a table laid out
/// otherwise.  Unknown addresses are null.  MaxFunctions is clamped to
/// 1..4096.  NULL with an error until neverd_session_analyze has analyzed the
/// whole image.
NEVERD_API const char *neverd_switches_json(neverd_session_t Sess,
                                            neverd_va_t FirstEntry,
                                            int MaxFunctions);

/// The pointers the loader stored in up to \p MaxSlots relocated data slots at
/// or above \p FirstSlot, in ascending slot order: each slot refers to its
/// pointer's target with kind "offset".  Returns
/// {"refs":[["from","to","offset"],...],"next_slot":"0x..."|null}.
/// MaxSlots is clamped to 1..65536.
NEVERD_API const char *neverd_pointer_refs_json(neverd_session_t Sess,
                                                neverd_va_t FirstSlot,
                                                int MaxSlots);

/// Whether \p Address lies in a data slot the loader relocated to hold a
/// pointer.  Returns 1 and stores the slot's first address in \p Slot and the
/// pointer in \p Target (either may be NULL), or returns 0.
NEVERD_API int neverd_pointer_at(neverd_session_t Sess, neverd_va_t Address,
                                 neverd_va_t *Slot, neverd_va_t *Target);

// ===--------------------------------------------------------------------===//
// Decompilation
// ===--------------------------------------------------------------------===//

/// Prepare one function's analysis without emitting source. A fresh or
/// previously restricted session analyzes the requested entry; an existing
/// whole-image analysis is retained. ARM mode and native exception-handler
/// discovery follow the decompiler's checks. Returns 1 when analysis succeeds,
/// or 0 with neverd_last_error on failure (also 0 for a null session).
/// Individual IR/source views can still refuse an unavailable representation.
/// This does not make mutable session operations safe to call concurrently.
NEVERD_API int neverd_prepare_function(neverd_session_t Sess,
                                       neverd_va_t FuncEntry);

/// Prepare the requested view with the same entry and recovery checks.
/// Native "low", "med", "llvm" and "llvmc" postpone HighIR construction,
/// preserving shared CFG/ABI/exception analysis. A later source or "high" view
/// completes HighIR on demand. Native "cfg" prepares the control-flow graphs
/// alone (neverd_cfg_json); any other view of the function continues that
/// analysis on demand.
/// Other valid representations keep ordinary preparation; unknown names fail.
/// Returns 1 on success or 0 with neverd_last_error on failure.
NEVERD_API int neverd_prepare_function_view(neverd_session_t Sess,
                                            neverd_va_t FuncEntry,
                                            const char *Representation);

/// Prepare later neverd_prepare_function calls for the functions from
/// FirstEntry on, in entry order, at most MaxFunctions (clamped to
/// 1..4096): build the control-flow graphs those calls would otherwise build
/// on demand, for them to reuse, and scan the image's data access widths the
/// first C view reads.  The results of later calls are unchanged,
/// and nothing already prepared is discarded.  Returns
/// {"available":bool,"prepared":int,"full":bool,"next_entry":"0x..."|null}:
/// "prepared" counts the functions the session now holds prepared, "full"
/// says it refused one for lack of room, so further calls prepare no more
/// functions.  They still keep what a decompile reads of calls to the
/// page's functions, so a pass goes on to "next_entry" null either way.
/// "available" is false where calls keep no such state: images other than
/// x86-64 except those whose calls take their arguments from the caller's
/// setup outside i386's regparm convention (AArch64, ARM), for which it
/// reads the functions' signature bodies as their callers' selections read
/// them.  Whatever it says, an image with a C view has its widths
/// scanned, and the function list and the C emitter its first views read
/// prepared.  A function that is not a symbol start keeps its own state
/// beside it.  A PE function's preparation keeps it unless discovering the
/// function's exception handlers adds metadata, which after a full load it
/// does not.  NULL
/// with an error when no binary is loaded or a build fails.  Like every
/// session call, this must not run concurrently with another call on the
/// session.
NEVERD_API const char *neverd_warm_functions_json(neverd_session_t Sess,
                                                  neverd_va_t FirstEntry,
                                                  int MaxFunctions);

NEVERD_API const char *neverd_decompile(neverd_session_t Sess,
                                        neverd_va_t FuncEntry);

/// LLVM-to-C for one native function (`neverd decompile --llvm`).  Only its
/// body is emitted, with the other functions declared, so a function the
/// emitter refuses fails alone.  EVM and SBF have dedicated backends and
/// return empty. Defaults to O2 and control-flow recovery, with optional
/// symbolic expression simplification off. Free with neverd_free_string.
NEVERD_API const char *neverd_decompile_llvm(neverd_session_t Sess,
                                             neverd_va_t FuncEntry);

/// Single-function LLVM-to-C with the same NoOpt policy as
/// neverd_decompile_all(). Pass nonzero to keep value-changing LLVM passes
/// disabled while still promoting the emitter's temporary allocas.
NEVERD_API const char *neverd_decompile_llvm_ex(neverd_session_t Sess,
                                                neverd_va_t FuncEntry,
                                                int NoOpt);

/// Reconstruct Mach-O native C and supported Objective-C method bodies as a
/// schema_version=1 JSON report. MaxFunctions=0 analyzes all discovered native
/// functions. Runtime signatures are source projection hints, not a semantic
/// equivalence certificate. Unsupported and missing methods remain explicit.
/// Returns NULL on failure; free successful results with neverd_free_string.
NEVERD_API const char *neverd_objc_methods_json(neverd_session_t Sess,
                                                size_t MaxFunctions);

/// Run the same Objective-C analysis and source publication checks, omitting
/// native_source and per-method source strings from the JSON result. All
/// coverage, diagnostics and dependency evidence retain the full-mode meaning.
/// The report adds sources_omitted=true. This is not a source-code artifact.
/// Returns NULL on failure; free successful results with neverd_free_string.
NEVERD_API const char *neverd_objc_methods_summary_json(neverd_session_t Sess,
                                                        size_t MaxFunctions);

/// Emit actual Swift bodies from structured source signature hints. The
/// schema_version=1 input must bind each mangled symbol to its image entry.
/// The report retains unsupported methods. NULL indicates an export failure;
/// free successful JSON with neverd_free_string. Source hints are not proof of
/// semantic equivalence or authenticated ABI metadata.
NEVERD_API const char *neverd_swift_methods_json(neverd_session_t Sess,
                                                 const char *SignaturesJson,
                                                 size_t MaxFunctions);

// ===--------------------------------------------------------------------===//
// Multi-stage IR
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_ir_low(neverd_session_t Sess,
                                     neverd_va_t FuncEntry);
NEVERD_API const char *neverd_ir_med(neverd_session_t Sess,
                                     neverd_va_t FuncEntry);
NEVERD_API const char *neverd_ir_high(neverd_session_t Sess,
                                      neverd_va_t FuncEntry);
/// Inspect native lifted LLVM with temporary-slot SSA promotion only. Does
/// not run the LLVMC optimization profile or optional symbolic rewriting;
/// native CFG/ABI/exception recovery still precedes emission. EVM and SBF
/// retain their architecture-specific LLVM emission.
NEVERD_API const char *neverd_ir_llvm(neverd_session_t Sess,
                                      neverd_va_t FuncEntry);

/// Return a schema_version=1 page of native Low/Med/High/LLVM IR with
/// instruction anchors, or C/LLVMC with optional library regions.
/// Representation is "low", "med", "high", "llvm", "c", "llvmc", "cpp", "rust",
/// "go" or "source"; dialects spell the HighC page in that language and
/// "source" in the function's own (C++, Rust, Go or C), adding dialect, unread
/// and source_names (where each source name such as core::fmt::write is). Every
/// C, LLVMC and dialect page lists identifier_symbols: the functions the
/// emitted C names by another identifier than their symbol (fini for _fini,
/// dotted_fn for dotted.fn), each as identifier, symbol and address. Dialects
/// are offered for an image with that language's code (neverd_headers_json's
/// language.pseudocode); for another the page is mapping_status
/// "unsupported_representation" with the reason. Offset is an absolute
/// zero-based rendered line and Limit is 1..2048. Text is byte-for-byte the
/// corresponding IR dump slice. C views retain native exception calls and
/// handler definitions, with table metadata in comments, instead of C++
/// try/catch/throw syntax. They require the original exception runtime.
/// Rows carry line, object_id, kind,
/// mapping_status, addresses (hex strings), and origin_seq when available. An
/// instruction anchor identifies the originating instruction, not every
/// contributing instruction after data propagation. Headers, PHIs and
/// synthetic/unbound operations remain unmapped. Unsupported
/// representations/architectures return an explicit mapping_status with empty
/// rows and no text. Errors return NULL and set neverd_last_error. C source
/// pages add library_regions with rule and identity evidence, precise original
/// occurrences. Surviving source statements also carry instruction anchors,
/// checked against canonical LowIR instruction boundaries and sequences. The
/// first row address belongs to the smallest intersecting statement span;
/// declarations and synthetic/unbound statements have no guessed address. These
/// anchors support navigation, not complete expression provenance. Pages retain
/// provenance_complete:false and ordinary source byte for byte. Library regions
/// add foldable and spans. Optional function_identity shares
/// raw/display/linkage names with neverd_resolve_addr; original direct-call
/// regions add callee and callee_identity. Span begin_byte/end_byte are
/// half-open UTF-8 byte offsets in the complete source; byte_offset locates
/// this page in it. An unknown mapping stays unfolded. C source pages also
/// carry prelude {lines, end_byte} when the emitter recorded where the
/// function's definition begins: the whole lines before it (includes, support
/// types and declarations) and the byte where the definition starts. Source
/// text is always complete code, identical to the legacy C emitter. The page
/// text is capped at 2 MiB; Low/Med retain no whole IR string for paging, while
/// C renders one bounded function (at most 32 MiB). Object IDs are stable for a
/// fixed analysis snapshot, not across revisions. Free the result with
/// neverd_free_string().
NEVERD_API const char *neverd_ir_view_json(neverd_session_t Sess,
                                           neverd_va_t FuncEntry,
                                           const char *Representation,
                                           size_t Offset, size_t Limit);

/// Source page with request-local transformations. OptionsJSON is an object:
/// `symbolic` (bool, default false); for llvmc, `llvm_optimization` is basic,
/// O0, O1, O2 (default), O3 or custom. Custom requires `llvm_passes`, an array
/// drawn from instcombine, early-cse, simplifycfg and dce, run in that order.
/// `llvm_control_flow` (bool, default true) independently enables dispatcher
/// recovery for llvmc, including its targeted constant proofs.
/// Unknown fields, invalid combinations and non-source representations fail.
/// Options never alter ordinary views, machine analysis, patching or project
/// state. Ordinary LLVM source uses O2 and control-flow recovery with optional
/// symbolic expression rewriting off.
/// Return an owned UTF-8 string (neverd_free_string), or NULL with last_error
/// set.
NEVERD_API const char *neverd_ir_view_ex_json(neverd_session_t Sess,
                                              neverd_va_t FuncEntry,
                                              const char *Representation,
                                              size_t Offset, size_t Limit,
                                              const char *OptionsJSON);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_DISASM_H
