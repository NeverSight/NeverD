//===- NeverDCAPIPersist.h - C API persisted user edits -----------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-address annotations and function renames, each persisted to a JSON
/// file in the analyzed binary's project folder, `<input>.neverd/`.
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

// ===--------------------------------------------------------------------===//
// The project folder: `<input>.neverd/` beside the input
// ===--------------------------------------------------------------------===//

/// The loaded input's project folder and which build of the input its files
/// describe: {"folder","input":{"name","size","sha256"},"recorded":{"name",
/// "size","sha256"} or null,"matches"}.  "recorded" is the project.json the
/// folder keeps, null before the project has one; "matches" is false when it
/// names another build, whose addresses may not be this one's.  NULL with
/// neverd_last_error() when no file is loaded.
NEVERD_API const char *neverd_project_json(neverd_session_t Sess);
/// Records in the project folder's project.json that its files describe the
/// loaded build of the input.  Each project file the C API writes records it;
/// a writer of its own calls this after it writes.  0 on success, -1 with
/// neverd_last_error().
NEVERD_API int neverd_project_record(neverd_session_t Sess);
NEVERD_API int neverd_annotations_load(neverd_session_t Sess);

// ===--------------------------------------------------------------------===//
// Symbol renaming (persisted to JSON sidecar file)
// ===--------------------------------------------------------------------===//

NEVERD_API int neverd_rename_func(neverd_session_t Sess, const char *OldName,
                                  const char *NewName);
/// 0 when \p Name may name \p Addr, else -1 with neverd_last_error saying why.
/// A name has at most 4096 bytes and no spaces or control characters, is no
/// automatic name such as `sub_1234` or `qword_3FB8`, and leads to one
/// address: another address's function, user name or symbol (one the user
/// has not renamed) may not have it.  An import's name may name a local copy,
/// as a PDB or a map file names one.  An empty name, which takes the user's
/// name away, passes.  neverd_rename_addr and neverd_rename_func refuse what
/// this refuses; a front end asks it before it names an address itself.
NEVERD_API int neverd_name_check(neverd_session_t Sess, neverd_va_t Addr,
                                 const char *Name);
/// Give \p Addr the user's name \p Name, or take the user's name away when
/// \p Name is NULL or empty.  The name replaces a function's, a symbol's or
/// an automatic name wherever the session names the address: the function
/// list, neverd_symbols_json() and neverd_data_symbols_json() (a row of its
/// own where no symbol names the address) and decompiled C, where it names
/// image data.  A name neverd_name_check refuses is refused.  Saved at once
/// to the renames sidecar; returns 0, or -1 with neverd_last_error.
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
/// State what the function at \p Entry is, as IDA's Edit function does, by
/// the JSON object \p Row: "noreturn" (it never returns, so every call to it
/// ends its block), "library" and "static" (notes analysis does not read),
/// each true or false, "color" ("#RRGGBB", the color it shows in), and
/// "type", its C declaration as IDA's Y sets it, such as
/// "int f(struct _DEVICE *Device, int Count)".  A declaration must read
/// against the local types, declare one function that is not variadic, and
/// use a calling convention NeverD places the image's parameters by; the
/// decompiler then prints its parameters and return, binding each parameter
/// to the register or stack slot the convention passes it in, ahead of the
/// debug information, and the function's callers see it.  A key with null
/// hands the attribute back to analysis; absent keys keep theirs.  A change
/// of "noreturn" restarts analysis and finds the functions again; one of
/// "type" only drops the decompiled text.  The statements go when the
/// function is deleted.  Returns 0, or -1 with neverd_last_error;
/// neverd_functions_save keeps the edit.
NEVERD_API int neverd_func_edit(neverd_session_t Sess, neverd_va_t Entry,
                                const char *Row);
/// What the Edit function dialog shows of the function at \p Entry, without
/// analyzing the whole program: {"addr","name","start","end","noreturn",
/// "library","static","color","purged_bytes","type","stated"}.  "end" is
/// past its last byte; each attribute is the user's statement, else
/// analysis' answer, else null when analysis has none; "purged_bytes" is the
/// arguments x86's `ret imm` pops (null elsewhere); "type" is the user's
/// declaration or null; "stated" holds the user's statements.
/// Null with neverd_last_error when no function starts there.  Free with
/// neverd_free_string.
NEVERD_API const char *neverd_func_attributes_json(neverd_session_t Sess,
                                                   neverd_va_t Entry);

/// The C declaration of the function at \p Entry as IDA's Y edits it: the
/// user's ("type", neverd_func_edit), else the signature the decompiler
/// prints for it, read against the local types:
/// {"entry","stated","declaration","name","return","variadic",
///  "parameters":[{"name","type","location"}],"problems"}.  "declaration"
/// is the text as it reads; each parameter's "location" is the register
/// ("rcx") or entry stack slot ("stack+0x28") its calling convention passes
/// it in, absent where NeverD has no rules; "problems" ([{"line","column",
/// "message"}]) say why a declaration does not read, as the user's may no
/// longer after a local type it names goes, and are absent otherwise.
/// \p OptionsJson (may be null) asks instead for
/// - {"declaration":"..."}: that declaration read the same way, a check
///   before it is stated that changes nothing;
/// - {"parameter":N,"type":"T"}: the function's declaration with its Nth
///   parameter (from 0, as printed) of type T, as Convert to struct * makes
///   it; T may name the parameter too ("struct _KAPC *Apc").  This one is
///   an error, not "problems", when the function's declaration or T does
///   not read, and on i386 when the function takes parameters in registers,
///   which NeverD's declarations do not place.
/// Null with neverd_last_error when no function starts at \p Entry or the
/// request is malformed.  Free with neverd_free_string.
NEVERD_API const char *neverd_func_type_json(neverd_session_t Sess,
                                             neverd_va_t Entry,
                                             const char *OptionsJson);
/// The user's function edits, [{"addr","state"?,attributes...}] in address
/// order, where "state" is "created" or "deleted" and the attributes are
/// neverd_func_edit's.  Free with neverd_free_string.
NEVERD_API const char *neverd_functions_json(neverd_session_t Sess);
/// Write or read the edits as `<input>.neverd/functions.json`, which
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
/// Write or read the items as `<input>.neverd/items.json`, which
/// neverd_session_load reads too.  Return 0, or -1 with neverd_last_error.
NEVERD_API int neverd_items_save(neverd_session_t Sess);
NEVERD_API int neverd_items_load(neverd_session_t Sess);

/// Show operand \p Operand (0 to 7) of the instruction at \p Addr as the JSON
/// object \p FormatJson says: {"base": a base of OperandFormats.def ("number"
/// is the listing's own choice, "hex", "decimal", "binary", "char", "offset",
/// "struct"), "negate"?: true to change the sign, "invert"?: true to invert
/// the bits}.  A structure offset ("struct", IDA's T) adds "type", a
/// structure or union of the program's local types, and "delta"?, how many
/// bytes into the type the register points: the number lands delta bytes
/// further in (neverd_local_type_member_json), and 0 with delta 2 shows as
/// `[rcx+(_KAPC.Size-2)]`.  A structure offset neither negates nor
/// inverts.  NULL, or the "number" base unchanged, forgets the
/// operand's format.  \p Addr lies in executable code.  Returns 0, or -1
/// with neverd_last_error.  neverd_operand_formats_save keeps the formats.
NEVERD_API int neverd_operand_format_set(neverd_session_t Sess,
                                         neverd_va_t Addr, int Operand,
                                         const char *FormatJson);
/// The user's operand formats, [{"addr","operands":[{"operand","base",
/// "negate","invert","type"?,"delta"?},...]},...] in address order, "type"
/// and "delta" a structure offset's.  Free with neverd_free_string.
NEVERD_API const char *neverd_operand_formats_json(neverd_session_t Sess);
/// Write or read the formats as `<input>.neverd/operands.json`, which
/// neverd_session_load reads too.  Return 0, or -1 with neverd_last_error.
NEVERD_API int neverd_operand_formats_save(neverd_session_t Sess);
NEVERD_API int neverd_operand_formats_load(neverd_session_t Sess);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_PERSIST_H
