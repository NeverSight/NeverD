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
NEVERD_API const char *neverd_renames_json(neverd_session_t Sess);
NEVERD_API int neverd_renames_save(neverd_session_t Sess);
NEVERD_API int neverd_renames_load(neverd_session_t Sess);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_PERSIST_H
