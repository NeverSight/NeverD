//===- NeverDCAPISession.h - C API session lifecycle --------------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Creating, loading, and analyzing a session, the image metadata read off it,
/// plus the error, allocation, and version entry points every caller needs.
///
/// All returned strings are heap-allocated via strdup(); callers must
/// free them with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_SESSION_H
#define NEVERD_SDK_CAPI_SESSION_H

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
// Session lifecycle
// ===--------------------------------------------------------------------===//

NEVERD_API neverd_session_t neverd_session_create(void);
NEVERD_API void neverd_session_destroy(neverd_session_t Sess);
/// Returns 1 on success, replacing the image, analysis, and per-image edits.
/// Returns 0 when preparation fails; a previously loaded session remains
/// usable with its existing image, analysis, and edits. See
/// neverd_last_error().
/// Path is UTF-8 on every platform, including Windows.
NEVERD_API int neverd_session_load(neverd_session_t Sess, const char *Path);
/// List the ways neverd_session_load can read the file at \p Path, as a load
/// dialog shows them, without loading it:
/// {"rows":[{"loader","text","processor","bits","endian","loadable",
/// "by_name","reason"?}]} in list order -- the rows a loader asks to lead,
/// the others, then "Binary file" -- with "processor" as
/// neverd_session_arch_name names it ("" when NeverD has none), "by_name" for
/// a row the loader took for the file's name alone (a dialog should not
/// choose it by default) and "reason" saying why a row cannot be loaded;
/// {"rows":[],"error"} when \p Path names no regular file.  Path is UTF-8.
/// Free with neverd_free_string.
NEVERD_API const char *neverd_identify_json(const char *Path);
/// The PDB the PE at \p Path names, and where it is, without loading the PE:
/// {"pdb":null} for an input that names no PDB a symbol store can key -- not
/// a PE, or one without a unique valid CodeView record -- and otherwise
/// {"pdb":{"file","key","recorded","found":{"path","location"}|null,
/// "rejected":[{"path","location","reason"}],"servers":[{"url","path"}]}}.
/// "file" and "key" name it in a symbol store, as
/// <store>/<file>/<key>/<file>, and "recorded" is the path the linker wrote.
/// "found" is the first local copy whose build identity matches the PE's, in
/// the order neverd_session_load() searches: "recorded" (that path),
/// "beside" (the input's directory), "directory" or "store" (of the search
/// path); "rejected" lists the copies before it that belong to another build
/// or cannot be read.  "servers" are the symbol servers to download a
/// missing PDB from, in order: "url" serves it, and "path" is where its store
/// keeps it, which a later load searches.  \p SymbolPath is a search path in
/// _NT_SYMBOL_PATH syntax; NULL reads _NT_ALT_SYMBOL_PATH and
/// _NT_SYMBOL_PATH, and without either searches a store in the user's cache
/// directory filled from Microsoft's public symbol server.  Nothing is
/// downloaded.  {"pdb":null,"error"} when \p Path names no readable file.
/// Paths are UTF-8.  Free with neverd_free_string.
NEVERD_API const char *neverd_pdb_reference_json(const char *Path,
                                                 const char *SymbolPath);
/// The DWARF of the ELF or Mach-O image at \p Path, found without loading the
/// image: {"dwarf":null} for another format, for an image without DWARF, or
/// for one whose debug information is never read (SBF), and otherwise
/// {"dwarf":{"path","location"}}.  "path" is the file that holds it, and
/// "location" is where neverd_session_load() reads it from, in its order,
/// which is GDB's: "input" (the image's own sections), "dsym" (a .dSYM bundle
/// beside a Mach-O image), "build-id" (the file /usr/lib/debug/.build-id
/// keeps under the image's build ID), "debuglink" (the file the image's
/// .gnu_debuglink names, of the CRC it records) or "beside" (a split .debug
/// file beside the image, matched by name).  A companion that records another
/// build ID than the image is passed over.  {"dwarf":null,"error"} when
/// \p Path names no readable file.  Paths are UTF-8.  Free with
/// neverd_free_string.
NEVERD_API const char *neverd_dwarf_reference_json(const char *Path);
/// The linker MAP beside the input at \p Path -- <stem>.map or <name>.map --
/// and how it holds against the image, without loading the image:
/// {"map":null} when there is none, when it names no function, or for an
/// input whose debug information is never read (EVM, SBF), and otherwise
/// {"map":{"path","dialect","functions","data","outside","match","reason"?,
/// "stamp"?,"section"?,"names"}}.  "dialect" is the linker format: "msvc"
/// (link.exe and lld-link /MAP, also Borland's and IDA's MAPs), "lld-link"
/// (/lldmap), "ld.lld", "gnu-ld" or "ld64".  "functions" and "data" count
/// its names and "names" holds its first function names, at most eight.
/// "match" says how it holds: "verified" (it records the image's build
/// stamp), "consistent" (it places every section the image also has where
/// the image has it), "unverified" (it states nothing to hold against the
/// image) or "mismatch", with "reason": "stamp" ("stamp":{"map","image"}
/// differ), "section" ("section":{"segment","name","map":{"address","size"},
/// "image":{...}} places one elsewhere) or "outside" ("outside" of its
/// functions, most of them, lie outside the image's code).  "stamp" is also
/// there for any MAP that records one.  neverd_session_load() reads the MAP
/// only when no PDB or DWARF describes the image, and never one that
/// mismatches it.  {"map":null,"error"} when \p Path names no readable file.
/// Paths are UTF-8.  Free with neverd_free_string.
NEVERD_API const char *neverd_map_reference_json(const char *Path);
/// The static symbol table of the image at \p Path, which strip removes,
/// found without loading the image: {"symbols":null} for an image without
/// one, for an object file, which is never read without its table, and for
/// a format that has none, and otherwise
/// {"symbols":{"table","count","names"}}.  "table" names it: ".symtab" for
/// ELF and SBF, "COFF symbol table" for a PE, "LC_SYMTAB" for the Mach-O
/// symbols an image does not export.  "count" is the defined names a load
/// reads from it, and "names" its first function names, at most eight, the
/// global ones first.
/// {"symbols":null,"error"} when \p Path names no readable file.  UTF-8.
/// Free with neverd_free_string.
NEVERD_API const char *neverd_symbol_table_reference_json(const char *Path);
/// Read the input of the next neverd_session_load() as \p OptionsJson says
/// instead of by its header: {"loader":"binary","processor":"x86"|"x86_64"|
/// "arm"|"thumb"|"aarch64","base","offset","size","entry"?,"platform"?}
/// reads it as a binary file -- the bytes from offset on (size 0 for the
/// rest of the file) as that processor's code at base, starting at entry
/// (base when absent); numbers are integers or hexadecimal strings.  The
/// platform whose conventions the code follows decides how it decompiles:
/// "sysv" (System V and AAPCS), "windows" or "darwin"; "auto", the default,
/// reads it from the code -- where calls put their first argument, what
/// prologues preserve, the thread block and system calls the code reaches --
/// and assumes System V when the code shows too little.  {"loader":"evm"}
/// reads a file as EVM bytecode, and {"loader":"auto"} as its header or
/// contents say, even when `<input>.neverd/load.json` says otherwise;
/// NULL drops the request, so a load reads that file when there is one.
/// Returns 0, or -1 with neverd_last_error.
NEVERD_API int neverd_session_set_load_options(neverd_session_t Sess,
                                               const char *OptionsJson);
/// How the loaded image was read, in neverd_session_set_load_options JSON;
/// "{}" before a load.  A binary file's platform comes with
/// "platform_source": "detected" or "user", and with "platform_evidence",
/// what detection read it from.  Free with neverd_free_string.
NEVERD_API const char *neverd_session_load_options_json(neverd_session_t Sess);
/// Keep how the image was read in `<input>.neverd/load.json` for later loads,
/// or remove that file when the header was read.  Returns 0, or -1 with
/// neverd_last_error.
NEVERD_API int neverd_load_options_save(neverd_session_t Sess);
NEVERD_API int neverd_session_is_loaded(neverd_session_t Sess);

/// Add the native function entries the function detector finds in the loaded
/// image alone -- call targets, prologues and format tables, without lifting
/// anything -- to the function list.  An added function has an automatic name
/// and no size, and stays listed until the next load; the full pipeline may
/// list more.  Idempotent; returns the function count, or -1 with an error.
NEVERD_API int neverd_session_discover_functions(neverd_session_t Sess);

/// Run the full analysis pipeline (lift → optimize → decompile).
/// Call after neverd_session_load() to pre-compute analysis data.
/// Returns 1 on success, 0 on failure.  Thread-safe if called once.
NEVERD_API int neverd_session_analyze(neverd_session_t Sess);

/// Return the caller's input path encoded as UTF-8.
NEVERD_API const char *neverd_session_file_path(neverd_session_t Sess);
/// Return the SHA-256 of the input file's bytes as loaded, as 64 lowercase
/// hexadecimal digits; empty when nothing is loaded or the file cannot be
/// read.  The loader's own hash costs nothing; otherwise the file is hashed
/// once per load.  Free with neverd_free_string.
NEVERD_API const char *neverd_session_input_sha256(neverd_session_t Sess);
NEVERD_API const char *neverd_session_arch_name(neverd_session_t Sess);
NEVERD_API const char *neverd_session_format_name(neverd_session_t Sess);
NEVERD_API int neverd_session_is_64bit(neverd_session_t Sess);
/// Return the target word size (32, 64, or 256), or 0 when unloaded.
NEVERD_API int neverd_session_bitness(neverd_session_t Sess);

// ===--------------------------------------------------------------------===//
// Debug information
//
// Loading a binary also looks for the debug information that belongs to it —
// PDB for PE, DWARF (in-image, .dSYM, or a split .debug file) for ELF and
// Mach-O, then a linker MAP — and the names it finds populate the symbol table
// the rest of the API reads.  The three setters below adjust that search and
// only take effect on the next neverd_session_load(); the two queries report
// what the last load settled on.
// ===--------------------------------------------------------------------===//

/// Load debug symbols from \p Path instead of searching for a companion file.
/// The named file is authoritative: neverd_session_load() fails if it cannot be
/// read or holds no function symbols.  Pass NULL or "" to resume searching.
/// Path is UTF-8 on every platform.
NEVERD_API void neverd_session_set_pdb_path(neverd_session_t Sess,
                                            const char *Path);

/// neverd_session_set_pdb_path() with an explicit trust choice for the next
/// load.  With UseWithoutIdentity == 1 the named PDB may supply analysis
/// facts for a PE that records no CodeView identity, as
/// neverd_session_load_pdb_ex() allows for a loaded PE; machine, section
/// layout and any present identity are still checked, and the identity stays
/// unverified.  0 is the strict policy of neverd_session_set_pdb_path().
/// Returns 1, or 0 with neverd_last_error for any other UseWithoutIdentity.
NEVERD_API int neverd_session_set_pdb_path_ex(neverd_session_t Sess,
                                              const char *Path,
                                              int UseWithoutIdentity);

/// Inspect a manually selected PDB for the loaded PE. Returns owned JSON:
/// {"verified":bool,"path","name_count","names":[{"address","name",
/// "function":bool}]}. Names are returned only for an unverified import;
/// authenticated inspection returns their count without a large name payload.
/// A missing CodeView identity permits inspection without authorizing analysis.
/// Present identities,
/// machine and section layout are checked; mismatch or malformed input fails
/// with NULL and neverd_last_error. This does not change the session, function
/// inventory, types, ABI or analysis evidence. Caller frees.
NEVERD_API const char *neverd_session_inspect_pdb_json(neverd_session_t Sess,
                                                       const char *Path);

/// Apply an authenticated PDB to the already loaded PE. Returns 1 on success,
/// keeping user names, comments and edits; 0 with neverd_last_error on failure,
/// leaving the existing image and analysis usable. Missing CodeView identity
/// is an error. The selected path becomes the next-load debug search policy.
/// The image is read again, so the PDB's names replace those of a MAP the
/// image was loaded with, as they would had the load named the PDB.
NEVERD_API int neverd_session_load_pdb(neverd_session_t Sess, const char *Path);

/// Apply a manually selected PDB with an explicit trust choice. With
/// UseWithoutIdentity == 1, a missing PE CodeView identity is permitted and
/// the PDB supplies the same supported analysis facts as a matching companion.
/// The caller must obtain user confirmation before setting this flag. Its
/// identity remains unverified; machine, sections and present identities are
/// still checked. 0 keeps the strict policy; other values fail. The selection
/// persists in the session's next-load policy. Same transaction and result
/// contract as neverd_session_load_pdb().
NEVERD_API int neverd_session_load_pdb_ex(neverd_session_t Sess,
                                          const char *Path,
                                          int UseWithoutIdentity);

/// Read the next neverd_session_load()'s linker MAP from \p Path instead of
/// the one beside the binary.  It is read after a PDB or DWARF, whose names
/// outrank its own: its names fill only the addresses they leave unnamed.
/// The named file is authoritative: neverd_session_load() fails if it cannot
/// be read, holds no function names, or contradicts the image (see
/// neverd_map_reference_json).  Pass NULL or "" to resume searching.  UTF-8.
NEVERD_API void neverd_session_set_map_path(neverd_session_t Sess,
                                            const char *Path);

/// neverd_session_set_map_path() with an explicit trust choice for the next
/// load.  With Unchecked == 1 the named MAP is read even when what it records
/// contradicts the image -- another build stamp, a section placed elsewhere,
/// functions outside the image's code -- which the caller must have had the
/// user confirm.  0 is the policy of neverd_session_set_map_path().  Returns
/// 1, or 0 with neverd_last_error for any other Unchecked.
NEVERD_API int neverd_session_set_map_path_ex(neverd_session_t Sess,
                                              const char *Path, int Unchecked);

/// The linker MAP at \p MapPath read against the loaded image, as
/// neverd_map_reference_json() reports one, without changing the session.
/// NULL with neverd_last_error when it cannot be read or names no function.
/// Caller frees.
NEVERD_API const char *neverd_session_inspect_map_json(neverd_session_t Sess,
                                                       const char *MapPath);

/// Load the linker MAP at \p Path into the loaded image beside its other
/// debug information: its names fill the addresses a PDB or DWARF leaves
/// unnamed, and take the place of an earlier MAP's.  With Unchecked == 1 a
/// MAP that contradicts the image loads too, which the caller must have had
/// the user confirm; 0 refuses it, and other values fail.  Returns 1,
/// keeping user names, comments and edits; 0 with neverd_last_error,
/// leaving the existing image and analysis usable.  The MAP becomes part of
/// the next load's debug search policy.
NEVERD_API int neverd_session_load_map(neverd_session_t Sess, const char *Path,
                                       int Unchecked);

/// Read the next neverd_session_load()'s DWARF from \p Path instead of
/// finding it: a split .debug file, an unstripped copy of the image, or a
/// .dSYM bundle or the DWARF file in one.  Its machine must be the image's,
/// and a build ID or UUID both record must agree; otherwise, or when it
/// cannot be read or holds no DWARF, neverd_session_load() fails.  Only an
/// ELF or Mach-O image has one.  Pass NULL or "" to resume finding it.
/// UTF-8.
NEVERD_API void neverd_session_set_dwarf_path(neverd_session_t Sess,
                                              const char *Path);

/// Load the DWARF at \p Path, a file as neverd_session_set_dwarf_path()
/// takes, into the loaded ELF or Mach-O image in place of the DWARF it has;
/// a MAP's names give way to its own.  Same transaction and result contract
/// as neverd_session_load_map().
NEVERD_API int neverd_session_load_dwarf(neverd_session_t Sess,
                                         const char *Path);

/// Pass 0 to analyze the image alone, ignoring any debug file beside it.
/// The escape hatch for a stale companion file that names functions worse than
/// the image itself does.  Enabled by default.
NEVERD_API void neverd_session_set_debug_info_enabled(neverd_session_t Sess,
                                                      int Enabled);

/// Pass nonzero for whole-program decompilation (neverd_decompile_all() and
/// neverd_decompile_all_ex()) to simplify the source symbolically, as the
/// "symbolic" source option of neverd_ir_view_ex_json() does for one
/// function, with every function's whole-program evidence: it removes
/// mixed Boolean-arithmetic expressions and other identities the machine
/// computes.  Off by default, where source keeps the machine's expressions;
/// lifting and patching are unaffected.
NEVERD_API void neverd_session_set_source_symbolic(neverd_session_t Sess,
                                                   int Enabled);

/// Pass 0 to load a PE without the PDB it names: the next
/// neverd_session_load() searches for none, though a PDB named with
/// neverd_session_set_pdb_path() still loads and other debug information is
/// still found.  For a user who declined the PDB, or chose one to load after
/// the image.  Enabled by default.
NEVERD_API void neverd_session_set_pdb_search(neverd_session_t Sess,
                                              int Enabled);

/// Pass 0 to load an ELF or Mach-O image without its DWARF, from its own
/// sections or a companion file: the next neverd_session_load() reads none,
/// though other debug information is still found.  For a user who declined
/// it.  Enabled by default.
NEVERD_API void neverd_session_set_dwarf_search(neverd_session_t Sess,
                                                int Enabled);

/// Pass 0 to load an image without the linker MAP beside it: the next
/// neverd_session_load() reads none, though a MAP named with
/// neverd_session_set_map_path() still loads and other debug information is
/// still found.  For a user who declined it.  Enabled by default.
NEVERD_API void neverd_session_set_map_search(neverd_session_t Sess,
                                              int Enabled);

/// Pass 0 to load an image without its static symbol table, as if it had
/// been stripped: the next neverd_session_load() reads only the symbols
/// dynamic linking needs (see neverd_symbol_table_reference_json).  For a
/// user who distrusts the names in it.  An object file cannot load so.
/// Enabled by default.
NEVERD_API void neverd_session_set_symbol_table(neverd_session_t Sess,
                                                int Enabled);

/// Search the directories and symbol stores of \p SymbolPath, in
/// _NT_SYMBOL_PATH syntax, for the PDB the next neverd_session_load() finds,
/// instead of the environment's; NULL returns to the environment's.  Servers
/// in it are not contacted.  UTF-8.
NEVERD_API void neverd_session_set_symbol_path(neverd_session_t Sess,
                                               const char *SymbolPath);

/// Limit the next `neverd_session_load()` PE exception-table decode to this
/// function entry.  Catch funclets named by that frame are still decoded.
/// Pass 0 to clear.  Analysis of a different entry after load decodes that
/// entry on demand.  Changing the restriction after an analysis ran discards
/// that analysis, so a later query (including neverd_session_analyze())
/// recomputes it for the new restriction instead of reusing stale results.
NEVERD_API void neverd_session_restrict_function(neverd_session_t Sess,
                                                 neverd_va_t Entry);

/// Assert the instruction state of one AArch32 function entry on subsequent
/// loads. Use this for binaries whose ARM/Thumb mode metadata is missing.
/// The entry is an untagged mapped address (including address zero in an
/// object file). Conflicting symbols, mappings, relocations, CPU constraints,
/// or an undecodable entry make neverd_session_load() fail. Repeating a call
/// replaces that entry's assertion; CLEAR removes it. Returns 0 for an invalid
/// mode or session, otherwise 1. Other architectures reject a nonempty set.
enum neverd_arm_function_mode {
  NEVERD_ARM_FUNCTION_MODE_CLEAR = 0,
  NEVERD_ARM_FUNCTION_MODE_ARM = 1,
  NEVERD_ARM_FUNCTION_MODE_THUMB = 2,
};
NEVERD_API int neverd_session_set_arm_function_mode(neverd_session_t Sess,
                                                    neverd_va_t Entry,
                                                    int Mode);

/// Best-effort exact-name hint for a PE with an explicitly selected PDB.
/// Call after neverd_session_set_pdb_path() and before neverd_session_load().
/// Returns 0 if the name is absent or ambiguous, the files disagree, or the
/// fast lookup is unavailable.  This does not change the session or report an
/// error; callers should load normally and resolve the name in that case.
/// A nonzero result may be passed to neverd_session_restrict_function(), but
/// callers must verify the name after loading and retry without restriction if
/// it no longer resolves to that address.
NEVERD_API neverd_va_t neverd_session_resolve_function_name_before_load(
    neverd_session_t Sess, const char *BinaryPath, const char *Name);

/// Called from `neverd_session_load` on the same thread.  \p phase is a
/// stable token (`image`, `debug`, `ready`).  \p done/\p total are 0 when
/// the step is indeterminate.  \p detail may be empty.  Pass NULL to clear.
typedef void (*neverd_load_progress_fn)(void *user_data, const char *phase,
                                        unsigned long long done,
                                        unsigned long long total,
                                        const char *detail);
NEVERD_API void neverd_session_set_load_progress(neverd_session_t Sess,
                                                 neverd_load_progress_fn Fn,
                                                 void *UserData);

/// Which loader supplied the session's debug symbols: "dwarf", "pdb", "map",
/// or "none".  Caller frees.
NEVERD_API const char *neverd_session_debug_info_kind(neverd_session_t Sess);

/// Path of the file the debug symbols came from, empty when there are none.
/// Caller frees.
NEVERD_API const char *neverd_session_debug_info_path(neverd_session_t Sess);

/// Every file the session's debug information was read from, the PDB or
/// DWARF first: {"enabled","kind","files":[{"kind","path","names",
/// "dialect"?,"match"?,"functions"?}]}.  "kind" is
/// neverd_session_debug_info_kind()'s, "names" counts the names a file
/// published into the image, and a MAP has its "dialect", "match" and
/// "functions" as neverd_map_reference_json() spells them.  Caller frees.
NEVERD_API const char *neverd_session_debug_info_json(neverd_session_t Sess);

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

/// Non-fatal loader metadata diagnostics as [{code,message}]. No input bytes
/// are repaired. Returns [] for an unloaded/null session. Caller frees.
NEVERD_API const char *
neverd_session_load_diagnostics_json(neverd_session_t Sess);
NEVERD_API int neverd_session_segment_count(neverd_session_t Sess);
NEVERD_API int neverd_session_section_count(neverd_session_t Sess);
NEVERD_API int neverd_session_import_count(neverd_session_t Sess);
NEVERD_API int neverd_session_export_count(neverd_session_t Sess);
NEVERD_API int neverd_session_symbol_count(neverd_session_t Sess);

/// \p Size bytes from \p Addr as text, sixteen to a line: the address, the
/// bytes in hexadecimal, and the printable ASCII characters among them.  A
/// byte the image does not map or materialize prints as "??" and a blank,
/// and the lines after the last mapped byte are left out.  NULL when the
/// range maps no byte.
NEVERD_API const char *neverd_hex_dump(neverd_session_t Sess, neverd_va_t Addr,
                                       int Size);
/// neverd_hex_dump() with the text column read in \p TextEncoding, a name or
/// alias of neverd_string_encodings_json(), or ASCII when NULL: a character
/// shows at its first byte in the columns it draws (two for wide East Asian
/// characters), its later bytes filling the rest of its width, and a byte
/// that starts no shown character prints '.'.  Each run of mapped bytes
/// decodes from its first byte.  NULL with an error for an unknown encoding.
NEVERD_API const char *neverd_hex_dump_ex(neverd_session_t Sess,
                                          neverd_va_t Addr, int Size,
                                          const char *TextEncoding);

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

#endif // NEVERD_SDK_CAPI_SESSION_H
