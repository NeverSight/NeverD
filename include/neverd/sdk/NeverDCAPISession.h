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
NEVERD_API void neverd_session_set_pdb_path(neverd_session_t Sess,
                                            const char *Path);

/// Linker MAP counterpart of neverd_session_set_pdb_path().
NEVERD_API void neverd_session_set_map_path(neverd_session_t Sess,
                                            const char *Path);

/// Pass 0 to analyze the image alone, ignoring any debug file beside it.
/// The escape hatch for a stale companion file that names functions worse than
/// the image itself does.  Enabled by default.
NEVERD_API void neverd_session_set_debug_info_enabled(neverd_session_t Sess,
                                                      int Enabled);

/// Which loader supplied the session's debug symbols: "dwarf", "pdb", "map",
/// or "none".  Caller frees.
NEVERD_API const char *neverd_session_debug_info_kind(neverd_session_t Sess);

/// Path of the file the debug symbols came from, empty when there are none.
/// Caller frees.
NEVERD_API const char *neverd_session_debug_info_path(neverd_session_t Sess);

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
