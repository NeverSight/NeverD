//===- BytePatchCAPIHeaderC.c - Frozen byte patch snapshot ABI ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compile the public snapshot contract as C, independently of C++ adapters.
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPIBytePatch.h"

#include <stddef.h>
#include <stdint.h>

typedef const neverd_byte_patch_snapshot_v1 *(*AcquireSnapshot)(
    neverd_session_t);
typedef void (*FreeSnapshot)(const neverd_byte_patch_snapshot_v1 *);

_Static_assert(_Generic(&neverd_byte_patches_snapshot_v1,
                   AcquireSnapshot: 1,
                   default: 0),
               "snapshot acquisition must retain its C signature");
_Static_assert(_Generic(&neverd_byte_patch_snapshot_free_v1,
                   FreeSnapshot: 1,
                   default: 0),
               "snapshot release must retain its C signature");

typedef const neverd_byte_patch_range_snapshot_v1 *(*AcquireRangeSnapshot)(
    neverd_session_t);
typedef void (*FreeRangeSnapshot)(const neverd_byte_patch_range_snapshot_v1 *);

_Static_assert(_Generic(&neverd_byte_patch_ranges_snapshot_v1,
                   AcquireRangeSnapshot: 1,
                   default: 0),
               "range snapshot acquisition must retain its C signature");
_Static_assert(_Generic(&neverd_byte_patch_range_snapshot_free_v1,
                   FreeRangeSnapshot: 1,
                   default: 0),
               "range snapshot release must retain its C signature");

#if SIZE_MAX == UINT64_MAX
_Static_assert(sizeof(neverd_byte_patch_range_v1) == 16,
               "v1 range array stride is frozen");
_Static_assert(offsetof(neverd_byte_patch_range_v1, address) == 0,
               "v1 range address is frozen");
_Static_assert(offsetof(neverd_byte_patch_range_v1, size) == 8,
               "v1 range size is frozen");
_Static_assert(sizeof(neverd_byte_patch_range_snapshot_v1) == 16,
               "v1 range snapshot layout is frozen");
_Static_assert(offsetof(neverd_byte_patch_range_snapshot_v1, range_count) == 0,
               "v1 range snapshot count is frozen");
_Static_assert(offsetof(neverd_byte_patch_range_snapshot_v1, ranges) == 8,
               "v1 range snapshot ranges pointer is frozen");
_Static_assert(sizeof(neverd_byte_patch_row_v1) == 48,
               "v1 row array stride is frozen");
_Static_assert(offsetof(neverd_byte_patch_row_v1, file_offset) == 0,
               "v1 row file offset is frozen");
_Static_assert(offsetof(neverd_byte_patch_row_v1, address) == 8,
               "v1 row address is frozen");
_Static_assert(offsetof(neverd_byte_patch_row_v1, size) == 16,
               "v1 row size is frozen");
_Static_assert(offsetof(neverd_byte_patch_row_v1, original) == 24,
               "v1 row original pointer is frozen");
_Static_assert(offsetof(neverd_byte_patch_row_v1, current) == 32,
               "v1 row current pointer is frozen");
_Static_assert(offsetof(neverd_byte_patch_row_v1, kind) == 40,
               "v1 row kind pointer is frozen");
_Static_assert(sizeof(neverd_byte_patch_snapshot_v1) == 16,
               "v1 snapshot layout is frozen");
_Static_assert(offsetof(neverd_byte_patch_snapshot_v1, row_count) == 0,
               "v1 snapshot row count is frozen");
_Static_assert(offsetof(neverd_byte_patch_snapshot_v1, rows) == 8,
               "v1 snapshot rows pointer is frozen");
#endif
