//===- NeverDCAPIPersist.h - C API persisted user edits -----------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-address annotations and function renames, each persisted to a JSON
/// sidecar file next to the analyzed binary.
///
/// All returned strings are heap-allocated via strdup(); callers must
/// free them with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_PERSIST_H
#define NEVERD_SDK_CAPI_PERSIST_H

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
// Symbol renaming (persisted to JSON sidecar file)
// ===--------------------------------------------------------------------===//

NEVERD_API int neverd_rename_func(neverd_session_t Sess, const char *OldName,
                                  const char *NewName);
/// Give \p Addr the user's name \p Name, or take the user's name away when
/// \p Name is NULL or empty.  The name replaces a function's, a symbol's or
/// an automatic name wherever the session names the address: the function
/// list, neverd_symbols_json() and neverd_data_symbols_json() (a row of its
/// own where no symbol names the address) and decompiled C, where it names
/// image data.  A name has at most 4096 bytes and no spaces or control
/// characters.  Saved at once to the renames sidecar; returns 0, or -1 with
/// neverd_last_error.
NEVERD_API int neverd_rename_addr(neverd_session_t Sess, neverd_va_t Addr,
                                  const char *Name);
NEVERD_API const char *neverd_renames_json(neverd_session_t Sess);
NEVERD_API int neverd_renames_save(neverd_session_t Sess);
NEVERD_API int neverd_renames_load(neverd_session_t Sess);

/// Make \p Entry, an address in executable code where no function starts or
/// one neverd_func_delete removed, the entry of a function.  The user's last
/// edit at an address decides over the image, the detector and analysis.
/// Returns 0, or -1 with neverd_last_error.  Analysis restarts with the new
/// function list; neverd_functions_save keeps the edit.
NEVERD_API int neverd_func_create(neverd_session_t Sess, neverd_va_t Entry);
/// Stop treating the function entry \p Entry as one, whether the image,
/// analysis or neverd_func_create made it; no source makes it one again.
/// Returns 0, or -1 with neverd_last_error.
NEVERD_API int neverd_func_delete(neverd_session_t Sess, neverd_va_t Entry);
/// The user's function edits, [{"addr","state"}] in address order, where
/// "state" is "created" or "deleted".  Free with neverd_free_string.
NEVERD_API const char *neverd_functions_json(neverd_session_t Sess);
/// Write or read the edits as `<input>.neverd-functions.json`, which
/// neverd_session_load reads too.  Return 0, or -1 with neverd_last_error.
NEVERD_API int neverd_functions_save(neverd_session_t Sess);
NEVERD_API int neverd_functions_load(neverd_session_t Sess);

/// Make the bytes from \p Addr the user's data item, given by the JSON object
/// \p Row: {"kind":"byte"|"word"|"dword"|"qword"} for a value of that size,
/// {"kind":"string","encoding":E,"size":N} for a string of N bytes in
/// encoding E (a name of neverd_string_encodings_json()), its zero terminator
/// included, or {"kind":"undefined","size":N} for N bytes shown as bytes,
/// whatever analysis reads there. {"kind":"code","size"?:N} defines one
/// native instruction in file-backed executable bytes, without creating a
/// function. The SDK decodes its size; a supplied N must match it exactly.
/// Invalid bytes or an unknown instruction mode fail. An item replaces the one
/// at its address; undefined bytes it covers give way to it and stay undefined
/// around it, and it may share no byte with another item.  Returns 0, or -1
/// with neverd_last_error.  neverd_items_save keeps the items.
NEVERD_API int neverd_item_set(neverd_session_t Sess, neverd_va_t Addr,
                               const char *Row);
/// Forget the user's item at \p Addr.  Returns 0, or -1 when none starts
/// there.
NEVERD_API int neverd_item_clear(neverd_session_t Sess, neverd_va_t Addr);
/// The user's data items, [{"addr","kind","size","encoding"?}] in address
/// order.  Free with neverd_free_string.
NEVERD_API const char *neverd_items_json(neverd_session_t Sess);
/// Write or read the items as `<input>.neverd-items.json`, which
/// neverd_session_load reads too.  Return 0, or -1 with neverd_last_error.
NEVERD_API int neverd_items_save(neverd_session_t Sess);
NEVERD_API int neverd_items_load(neverd_session_t Sess);

/// Show operand \p Operand (0 to 7) of the instruction at \p Addr as the JSON
/// object \p FormatJson says: {"base": a base of OperandFormats.def ("number"
/// is the listing's own choice, "hex", "decimal", "binary", "char", "offset"),
/// "negate"?: true to change the sign, "invert"?: true to invert the bits}.
/// NULL, or the "number" base unchanged, forgets the operand's format.
/// \p Addr lies in executable code.  Returns 0, or -1 with
/// neverd_last_error.  neverd_operand_formats_save keeps the formats.
NEVERD_API int neverd_operand_format_set(neverd_session_t Sess,
                                         neverd_va_t Addr, int Operand,
                                         const char *FormatJson);
/// The user's operand formats, [{"addr","operands":[{"operand","base",
/// "negate","invert"},...]},...] in address order.  Free with
/// neverd_free_string.
NEVERD_API const char *neverd_operand_formats_json(neverd_session_t Sess);
/// Write or read the formats as `<input>.neverd-operands.json`, which
/// neverd_session_load reads too.  Return 0, or -1 with neverd_last_error.
NEVERD_API int neverd_operand_formats_save(neverd_session_t Sess);
NEVERD_API int neverd_operand_formats_load(neverd_session_t Sess);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_PERSIST_H
