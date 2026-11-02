//===- NeverDCAPIQuery.h - C API image queries and graphs ---------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Read-only interrogation of a loaded image: the info-panel and header
/// tables, control-flow and call graphs, address resolution, byte/string
/// search, and function-level diffing between two sessions.
///
/// All returned strings are heap-allocated via strdup(); callers must
/// free them with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_QUERY_H
#define NEVERD_SDK_CAPI_QUERY_H

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
// Info panels (return JSON)
// ===--------------------------------------------------------------------===//

/// Imports as [{"module","name","ordinal","iat_addr","stubs"}].  "iat_addr"
/// is the format-native data slot; "stubs" lists the executable veneers (ELF
/// PLT entries, Mach-O stubs, import thunks) known to forward to the import,
/// in ascending address order.  "c_name", present when it differs from
/// "name", is what C code calls the import: the symbol without the underscore
/// its format adds (Mach-O, i386 COFF).
NEVERD_API const char *neverd_imports_json(neverd_session_t Sess);
NEVERD_API const char *neverd_exports_json(neverd_session_t Sess);
/// Exact pointer slots that hold an imported symbol's address, from every
/// format's binding metadata (ELF GLOB_DAT and JUMP_SLOT GOT entries, PE IAT
/// entries, Mach-O bindings): [{"addr","name","addend"}] in address order,
/// with "c_name" as for neverd_imports_json.  Slots whose metadata names
/// conflicting symbols are omitted.
NEVERD_API const char *neverd_import_slots_json(neverd_session_t Sess);
/// The unwind record whose code range contains \p Address, as
/// {"begin","end","encoding","personality"?,"language_data"}, or null.
/// "begin" and "end" bound the code the record describes, "encoding" names
/// its format ("dwarf-fde", "x64-unwind-v1", ...), "personality" is the
/// resolved personality routine when one is known, and "language_data" is
/// true when the record carries decoded language-specific data.
/// AArch64 additionally returns "return_address" with the exact "query_pc"
/// (no return-PC subtraction), "dwarf_state_at_pc" and "compact_recipe".
/// Either source is null when absent, otherwise an object with "status":
/// known, unknown, unsupported, malformed, out_of_range or resource_limit;
/// an optional "diagnostic" explains unavailable results. Known results have
/// "kind": unsigned, sp or sp_and_pc. Signed kinds also have "key": a or b;
/// sp_and_pc adds "recipe": hint_pacm or non_hint_pauth_lr, and "signing_pc".
/// Addresses use the same hexadecimal-string convention as begin/end.
/// A compact recipe describes the completed prologue, not whether the return
/// address is already signed at query_pc. It never replaces a failed DWARF
/// state query. These static results do not authenticate or unwind a pointer.
NEVERD_API const char *neverd_unwind_frame_json(neverd_session_t Sess,
                                                neverd_va_t Address);
/// The parts of functions that lie apart from their bodies, as
/// [{"begin","end","function"}] in address order: code that unwind records
/// chained to a function's own cover past a gap in its body, where compilers
/// move cold paths out of line.  "function" is the entry of the function
/// each part belongs to.
NEVERD_API const char *neverd_function_chunks_json(neverd_session_t Sess);
NEVERD_API const char *neverd_segments_json(neverd_session_t Sess);
/// Text strings in the image's initialized data that is not code: C strings
/// in ASCII or UTF-8, UTF-16 or UTF-32 strings in either byte order, and C
/// strings in legacy code pages, each ending in a zero code unit.  Also the
/// literals symbols spell (neverd_symbols_json's "literal") where the image
/// holds the bytes the name spells: wherever they lie -- a kernel keeps them
/// among its code -- however short they are, whatever characters they hold
/// and whichever encodings are searched, since the symbol says they are text.
/// \p OptionsJson is NULL for the defaults or an object with optional
/// "encodings" (names from neverd_string_encodings_json(); default those it
/// marks "default"), "preferred" (a legacy code page that reads a C string
/// first and so wins where other code pages read it as text too; searched
/// whether or not "encodings" names it; without it the code pages read in
/// the order neverd_string_encodings_json() lists them) and "min_length"
/// (display columns, a wide East Asian character counting two, 1 to 1024,
/// default 4).  Returns
/// [{"addr","length","chars","encoding","literal"?,"value"},...] in address
/// order, where "length" counts bytes without the terminator, "literal" is
/// true for a literal a symbol spells, which wins over text a scan reads in
/// its bytes, and "value" is UTF-8; or NULL with neverd_last_error() for
/// malformed options.
NEVERD_API const char *neverd_strings_ex_json(neverd_session_t Sess,
                                              const char *OptionsJson);
/// One page of neverd_strings_ex_json(): the strings that start at or after
/// \p FirstAddr, at most \p MaxRows of them (clamped to 1..65536, but never
/// splitting the strings that start at one address), as {"strings":[rows as
/// that function lists them],"next_addr":"0x..."|null}.  The image is scanned
/// once per options, so reading every page costs about one call of that
/// function and no result grows with the image.  NULL with
/// neverd_last_error() for malformed options.
NEVERD_API const char *neverd_strings_page_json(neverd_session_t Sess,
                                                const char *OptionsJson,
                                                neverd_va_t FirstAddr,
                                                int MaxRows);

/// The string that starts at \p Addr, as neverd_strings_ex_json() reads one
/// with \p OptionsJson (NULL for the default encodings), except that a single
/// character is enough when the options name no "min_length".  Returns
/// {"addr","length","chars","encoding","value"} as that function's rows, and
/// "unit", the bytes of the encoding's code unit (and so of the terminator),
/// or NULL with neverd_last_error() when no string starts there.
NEVERD_API const char *neverd_string_at(neverd_session_t Sess, neverd_va_t Addr,
                                        const char *OptionsJson);
/// The encodings neverd_strings_ex_json() can search:
/// [{"name","spelling","unit","default","legacy"},...], where "spelling" is
/// how a listing names the encoding (empty for plain ASCII), "unit" the bytes
/// of one code unit, "default" whether a search without options uses it and
/// "legacy" whether it is an 8-bit code page.
NEVERD_API const char *neverd_string_encodings_json(void);
/// The instructions of up to \p MaxFunctions native functions, from
/// \p FirstEntry in entry order, that refer to the text of a string the
/// options (as neverd_strings_ex_json() takes them) find.  A reference into
/// a string reads its text from the first character that starts at or after
/// that byte, and counts only while at least the minimum length of characters
/// remains there.  An instruction that reads or writes a slot the loader
/// relocated, whose pointer reaches such text, refers to it through the slot.
/// Returns {"refs":[["from","to","string",text_offset,"kind",
/// "via"|null,"instruction"],...],"next_entry":"0x..."|null,
/// "function_count":int}: "to" is the referenced byte, "string" the start of
/// the string holding it, text_offset where its text begins in that string's
/// UTF-8 "value", "kind" read, write or offset, "via" the slot, and
/// "instruction" the referring instruction as neverd_disasm_json() spells
/// it.  Functions decode as for
/// neverd_code_refs_json(), and MaxFunctions is clamped to 1..4096.  NULL
/// with neverd_last_error() for malformed options or an EVM or SBF image.
/// This query never starts analysis.
NEVERD_API const char *neverd_string_refs_json(neverd_session_t Sess,
                                               const char *OptionsJson,
                                               neverd_va_t FirstEntry,
                                               int MaxFunctions);
/// \p Text (UTF-8) with every character case-folded by Unicode simple case
/// folding, so that a filter matches text regardless of case in any script.
/// Bytes that are not UTF-8 stay as they are.  NULL for NULL.
NEVERD_API const char *neverd_fold_case(const char *Text);
/// \p Name as a listing reads it: an Itanium or Microsoft C++, Rust, Swift or
/// D mangled name as its language spells it, the way function identities'
/// "display_name" is (C++ template arguments equal to their defaults dropped,
/// a legacy Rust hash dropped), also with one leading underscore more (a
/// Mach-O symbol or an ELF PLT entry's name), and any other name as it is.
/// NULL for NULL.
NEVERD_API const char *neverd_demangle(const char *Name);
/// \p Size bytes decoded in \p Encoding (a name or alias of
/// neverd_string_encodings_json()) for display, one cell per byte:
/// {"cells":[...]}, where a character's first byte holds its text (UTF-8), a
/// later byte of it "", and a byte that decodes to nothing shown (a control,
/// an unmapped byte) null.  Wide encodings decode whole units from the first
/// byte.  Returns NULL for an unknown encoding or a size outside 0-65536.
NEVERD_API const char *neverd_decode_text_json(const unsigned char *Bytes,
                                               int Size, const char *Encoding);
NEVERD_API const char *neverd_strings_json(neverd_session_t Sess,
                                           int MinLength);
NEVERD_API const char *neverd_xrefs_to_json(neverd_session_t Sess,
                                            neverd_va_t Addr);
NEVERD_API const char *neverd_xrefs_from_json(neverd_session_t Sess,
                                              neverd_va_t Addr);

// ===--------------------------------------------------------------------===//
// Sections / Symbols / Relocations / Headers / Entry points / Dashboard
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_sections_json(neverd_session_t Sess);
/// The image's symbols with the user's names applied, as
/// [{"addr","literal"?,"name","size"}].  "literal" is true where the name
/// spells a string literal rather than naming an object, as MSVC names the
/// literals it pools (`??_C@_15OHIPBLFN@?$AAS?$AAA@` is L"SA"); a listing
/// names the string by its text instead.  A name the user gives is never one.
NEVERD_API const char *neverd_symbols_json(neverd_session_t Sess);
/// The symbols of neverd_symbols_json that are not functions.
NEVERD_API const char *neverd_data_symbols_json(neverd_session_t Sess);
NEVERD_API const char *neverd_relocs_json(neverd_session_t Sess);
/// The image's headers.  "language" is the runtime that built the image as
/// {"runtime","version"?,"secondary":[],"evidence":[],"pseudocode":[]}: "c",
/// "c++-itanium", "c++-msvc", "rust", "go", "objective-c", "swift", "delphi",
/// "ada", "d" or "unknown", with the Go release when the image names one;
/// "pseudocode" lists "c", plus "cpp", "rust" or "go" for an image with
/// that language's code.
NEVERD_API const char *neverd_headers_json(neverd_session_t Sess);
NEVERD_API const char *neverd_entrypoints_json(neverd_session_t Sess);
/// An overview of the loaded image: "file" (path, name, format, arch, bits,
/// entry, base, size), "hashes" of the input file ("md5", "sha256" and
/// "crc32" in lowercase hex) and "counts".
NEVERD_API const char *neverd_dashboard_json(neverd_session_t Sess);

// ===--------------------------------------------------------------------===//
// CFG graph (returns JSON: nodes + edges)
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_cfg_json(neverd_session_t Sess,
                                       neverd_va_t FuncEntry);

// ===--------------------------------------------------------------------===//
// Call graph (function-level call relationships)
// ===--------------------------------------------------------------------===//

/// Return an owned JSON object with complete function nodes and call edges.
/// SBF recovery is exact-or-empty under typed host resource budgets: when a
/// budget is exhausted this returns {"nodes":[],"edges":[]} and records the
/// diagnostic for neverd_last_error(). A partial edge relation is never
/// published.
NEVERD_API const char *neverd_callgraph_json(neverd_session_t Sess);

/// Call paths between functions, read from each function's direct
/// references (neverd_code_refs_json) without starting analysis.  Nodes are
/// functions and imports; an import's veneer (a PLT entry, an import thunk)
/// is the import.  A node calls, jumps to the entry of, or transfers through
/// a slot holding another; with "data_refs" it also takes, reads or writes
/// its address.  \p OptionsJson is an object: "waypoints" (2 to 64, each a
/// "0x..." address of a function entry, an import's slot or veneer, or a
/// function's or import's name), visited in order, one segment between each
/// two; "exclude" (nodes no path passes, unless they are waypoints);
/// "max_depth" (edges per segment, 1-64, default 16); "shortest_only"
/// (bool); "data_refs" (bool); "max_paths" (0-1000, default 32) and
/// "max_listed" (1-100000 nodes and edges per segment, default 4096).
/// Returns {"segments":[{"from","to","shortest","node_count","edge_count",
/// "nodes":["0x..."],"edges":[["from","to","kind","site"]],"paths":
/// [["0x...",...]],"complete","reversed"?}],"unconnected":int,"nodes":
/// [{"address","name","kind":"function"|"import","module"?,"library"?}],
/// "max_depth","data_refs"}.  A segment's nodes are those on a path of at
/// most "max_depth" edges (with "shortest_only", of "shortest" edges), in
/// order of distance from "from"; "kind" is "call", "jump", "indirect" or
/// "data"; "site" is the first instruction making the reference.  Paths are
/// simple, shortest first.  Waypoints that connect only the other way round
/// give that segment with "reversed":true; neither way, "shortest" is null
/// and the segment counts in "unconnected".  "complete" is false when
/// "max_listed" or "max_paths" cut a list, or path enumeration exceeded its
/// step budget.  NULL with neverd_last_error() for malformed options, an
/// unknown waypoint, or an EVM or SBF image.
NEVERD_API const char *neverd_call_paths_json(neverd_session_t Sess,
                                              const char *OptionsJson);

/// The functions and imports one function reaches through the same graph
/// as neverd_call_paths_json(), or the functions that reach it.
/// \p OptionsJson is an object: "root" (required, as a waypoint is named);
/// "direction" ("callees", the default, or "callers"); "max_depth" (1-64,
/// default 1); "max_listed" (1-100000, default 4096); "data_refs"; "exclude";
/// and "expand_library" (bool): imports, and unless it is true functions
/// byte signatures name as library code, are listed but not expanded.
/// Returns {"root","direction","max_depth","nodes":[{...,"depth"}],
/// "edges":[["caller","callee","kind","site"]],"depth_limited","complete",
/// "bottom_up"?}: nodes in breadth-first order, "depth_limited" when a node
/// at "max_depth" has more, "complete" false when "max_listed" cut the
/// list.  For callees, "bottom_up" lists the reached functions that expand
/// with each one's callees before it and the root last: the order to
/// decompile them in.  NULL with neverd_last_error() as for
/// neverd_call_paths_json().
NEVERD_API const char *neverd_call_tree_json(neverd_session_t Sess,
                                             const char *OptionsJson);

// ===--------------------------------------------------------------------===//
// Address resolution
// ===--------------------------------------------------------------------===//

/// Exact functions include optional display_name/linkage_name, name/display
/// origins, recognition_state and library_annotations. User/stated names keep
/// precedence. This query does not force native analysis; evidence may be
/// pending.
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
/// Bounded search in address order. Options: kind (bytes/text/immediate),
/// pattern, limit (1..4096), optional exclusive from address, backward and
/// case_sensitive booleans. Returns owned JSON {items,complete,next_address};
/// a non-null next_address resumes after the last item in the same direction.
/// Immediate pages decode native executable ranges in bounded chunks. Repeat
/// the same request while building is true. coverage and undecoded_ranges
/// report skipped ranges; matches do not assert reachability or include memory
/// displacements. Numeric values use the logical operand width.
/// Matches stay within one segment's initialized bytes. Invalid options fail
/// with null and a session error. Legacy search uses the same matcher.
NEVERD_API const char *neverd_search_page(neverd_session_t Sess,
                                          const char *OptionsJSON);

/// Physical register inventory, reaching definitions, uses or static value
/// before PC in Entry's original LowIR. Options: kind and register. Calls,
/// undefined effects and unknown paths remain explicit boundaries. Owned JSON.
NEVERD_API const char *neverd_register_flow_json(neverd_session_t Sess,
                                                 neverd_va_t Entry,
                                                 neverd_va_t PC,
                                                 const char *OptionsJSON);

// ===--------------------------------------------------------------------===//
// Binary diff (function-level comparison between two sessions)
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_diff_functions(neverd_session_t SessA,
                                             neverd_session_t SessB);
NEVERD_API const char *neverd_diff_decompile(neverd_session_t SessA,
                                             neverd_va_t EntryA,
                                             neverd_session_t SessB,
                                             neverd_va_t EntryB);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_QUERY_H
