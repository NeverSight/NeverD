//===- Limits.h - Interactive patch limits and defaults --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource ceilings of the interactive byte patch overlay, its assembler and
/// its exported files.  Encodings (NOP sequences, branch forms) stay with
/// their architecture models in PatchActions.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PATCH_LIMITS_H
#define NEVERD_PATCH_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::patch {

// The overlay.

/// Bytes one apply may write: a fill over a large function, not a file.
inline constexpr uint64_t kMaxApplyBytes = 1u << 20;
/// Bytes the overlay of one project may hold changed at once.
inline constexpr uint64_t kMaxPatchedBytes = 4u << 20;
/// Separate runs of changed bytes one project may hold.
inline constexpr size_t kMaxPatchRows = 65536;
/// Size of the patches.json a load reads: two hex digits for each original
/// and current byte, plus each row's fields.
inline constexpr uint64_t kMaxPatchesFileBytes =
    kMaxPatchedBytes * 4 + kMaxPatchRows * 256;
/// Widest relocation field the loaders record: a slot that starts this many
/// bytes before a write may still reach into it.
inline constexpr unsigned kMaxRelocationFieldBytes = 8;

// Previews.

/// Instructions a preview displays on either side of a change. This does
/// not limit validation of the complete byte range.
inline constexpr unsigned kMaxPreviewInstructions = 64;
/// Instructions a NOP fill may cover after its first one.
inline constexpr unsigned kMaxFillInstructions = 65536;

// The assembler.

/// Text one assembly request may hold.
inline constexpr size_t kMaxAssemblyTextBytes = 4096;
/// Symbols (frame variables and names) one request may define.
inline constexpr size_t kMaxAssemblySymbols = 4096;
/// Length of one symbol name.
inline constexpr size_t kMaxAssemblySymbolBytes = 1024;

// Export.

/// Largest input retained for interactive replay and exclusive export.
inline constexpr uint64_t kMaxInputFileBytes = 1ull << 30;

/// Bytes copied per read while an export replays the overlay onto a copy of
/// the input file.
inline constexpr size_t kExportCopyChunkBytes = 1u << 20;

} // namespace neverd::limits::patch

#endif // NEVERD_PATCH_LIMITS_H
