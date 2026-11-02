//===- NeverDCAPIBytePatch.h - Interactive byte patches ----------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preview, edit, persist, revert and export an input's byte overlay.
/// Strings returned here are freed with neverd_free_string(). Errors return
/// NULL (strings) or -1 (integers), with neverd_last_error() explaining why.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_SDK_NEVERDCAPIBYTEPATCH_H
#define NEVERD_SDK_NEVERDCAPIBYTEPATCH_H
#include "neverd/sdk/NeverDCAPISession.h"
#include "neverd/sdk/NeverDCAPITypes.h"

#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct neverd_byte_patch_options_v1 {
  size_t struct_size;
  neverd_va_t address;
  const char *kind;
  const char *bytes; ///< Hex pairs, for kind=bytes.
  const char *text;  ///< One instruction, for kind=assemble.
  uint64_t value;
  uint64_t size; ///< Whole instruction range, for kind=fill_nops.
  int fill_nops;
  int allow_overlap;
  int dialect; ///< 0=Intel, 1=AT&T.
  const char *
      expected; ///< Optional hex of preview's original bytes; checked at apply.
  const char *symbols; ///< Optional JSON object of explicit name:signed-offset
                       ///< bindings.
} neverd_byte_patch_options_v1;

typedef struct neverd_assemble_options_v1 {
  size_t struct_size;
  neverd_va_t pc;
  int dialect;
  int mode; ///< 0=image mode at pc, 1=ARM, 2=Thumb. Must match image evidence.
  const char *text;
  const char *symbols; ///< Explicit assembly symbols, bounded by the assembly
                       ///< text budget.
} neverd_assemble_options_v1;

/// Frozen v1 layout: raw bytes of one canonical changed run. All pointers are
/// owned by the containing snapshot. Original and Current both contain Size
/// bytes; Kind is a NUL-terminated canonical patch kind.
typedef struct neverd_byte_patch_row_v1 {
  uint64_t file_offset;
  neverd_va_t address;
  size_t size;
  const uint8_t *original;
  const uint8_t *current;
  const char *kind;
} neverd_byte_patch_row_v1;

/// Frozen v1 layout. Rows are ordered by file_offset, with no overlaps.
typedef struct neverd_byte_patch_snapshot_v1 {
  size_t row_count;
  const neverd_byte_patch_row_v1 *rows;
} neverd_byte_patch_snapshot_v1;

/// Frozen v1 layout: one canonical changed range, without byte payloads.
typedef struct neverd_byte_patch_range_v1 {
  neverd_va_t address;
  size_t size;
} neverd_byte_patch_range_v1;

/// Frozen v1 layout. Ranges retain file-offset order, not address order.
typedef struct neverd_byte_patch_range_snapshot_v1 {
  size_t range_count;
  const neverd_byte_patch_range_v1 *ranges;
} neverd_byte_patch_range_snapshot_v1;

NEVERD_API const char *
neverd_byte_patch_preview(neverd_session_t Sess,
                          const neverd_byte_patch_options_v1 *Options);
NEVERD_API const char *
neverd_byte_patch_apply(neverd_session_t Sess,
                        const neverd_byte_patch_options_v1 *Options);
NEVERD_API int neverd_byte_patch_revert(neverd_session_t Sess,
                                        neverd_va_t Address, uint64_t Size);
/// Complete overlay rows, ordered by their file offset (the "addr" key).
NEVERD_API const char *neverd_byte_patches_json(neverd_session_t Sess);
/// Owned immutable inventory, without JSON encoding. A loaded session with no
/// patches returns an empty snapshot. NULL indicates failure. The snapshot and
/// its pointers remain valid across session edits, reloads and destruction.
/// Free exactly once with the matching function from the same loaded SDK.
NEVERD_API const neverd_byte_patch_snapshot_v1 *
neverd_byte_patches_snapshot_v1(neverd_session_t Sess);
/// Release a snapshot and all its storage. NULL is accepted.
NEVERD_API void neverd_byte_patch_snapshot_free_v1(
    const neverd_byte_patch_snapshot_v1 *Snapshot);
/// Owned immutable range inventory, without encoding JSON or copying original
/// and current byte payloads. A loaded empty overlay returns a non-null empty
/// snapshot; NULL indicates failure. Storage survives session edits, reloads
/// and destruction. Acquire and release with the same loaded SDK.
NEVERD_API const neverd_byte_patch_range_snapshot_v1 *
neverd_byte_patch_ranges_snapshot_v1(neverd_session_t Sess);
/// Release the range snapshot and its storage. NULL is accepted.
NEVERD_API void neverd_byte_patch_range_snapshot_free_v1(
    const neverd_byte_patch_range_snapshot_v1 *Snapshot);
/// Canonical changed ranges as [{address,size}], without encoding byte data.
NEVERD_API const char *neverd_byte_patch_ranges_json(neverd_session_t Sess);
/// Applicable machine-byte actions and the current instruction, for menus.
NEVERD_API const char *neverd_byte_patch_context_json(neverd_session_t Sess,
                                                      neverd_va_t Address);
/// Prove a source literal (-1 for a statement) against current machine bytes.
/// Line is zero-based in the exact emitted C/LLVMC text.
NEVERD_API const char *neverd_byte_patch_source_target_json(
    neverd_session_t Sess, neverd_va_t Function, const char *Representation,
    size_t Line, int64_t Literal);
/// Decompile before/after in an isolated session; never changes Sess.
NEVERD_API const char *neverd_byte_patch_source_preview_json(
    neverd_session_t Sess, neverd_va_t Function, const char *Representation,
    const neverd_byte_patch_options_v1 *Options);
/// Compilable HighC, entry mapping and explicit bindings for function
/// rewriting.
NEVERD_API const char *neverd_source_patch_context_json(neverd_session_t Sess,
                                                        neverd_va_t Function);
NEVERD_API int neverd_byte_patches_save(neverd_session_t Sess);
NEVERD_API int neverd_byte_patches_load(neverd_session_t Sess);
NEVERD_API int neverd_byte_patches_export(neverd_session_t Sess,
                                          const char *OutputPath,
                                          int AllowUnsigned);
NEVERD_API const char *
neverd_assemble_json(neverd_session_t Sess,
                     const neverd_assemble_options_v1 *Options);

#ifdef __cplusplus
}
#endif
#endif // NEVERD_SDK_NEVERDCAPIBYTEPATCH_H
