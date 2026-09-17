//===- FunctionDiscovery.h - Heuristic function start detection -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Format-agnostic heuristic function discovery utilities that operate on
/// BinaryImage.  Used by all three loaders (ELF, COFF, MachO) as a
/// fallback when metadata-based discovery (.eh_frame, .pdata,
/// LC_FUNCTION_STARTS) is unavailable or incomplete.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_FUNCTIONDISCOVERY_H
#define NEVERD_LOADER_FUNCTIONDISCOVERY_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/ProloguePatterns.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

namespace neverd {

/// Binary search through sorted (start, end) intervals to check if \p A
/// falls inside any of them.
inline bool insideInterval(const std::vector<std::pair<va_t, va_t>> &Ivs,
                           va_t A) {
  auto It = std::upper_bound(Ivs.begin(), Ivs.end(),
                             std::make_pair(A, va_t(~va_t(0))));
  if (It == Ivs.begin())
    return false;
  --It;
  return A >= It->first && A < It->second;
}

/// Check if the bytes at offset \p Off in \p Seg look like a function
/// prologue for the given architecture.
bool checkPrologueAtOffset(const Segment &Seg, size_t Off, Arch A);

/// Scan executable segments for import thunk patterns (jmp [rip+disp32]
/// on x86_64, jmp [abs32] on x86, ADRP/LDR/BR x16 on AArch64), map each
/// recognized veneer to its Import, and register it as a function symbol.
/// Shared by all loaders (COFF IAT thunks, ELF PLT stubs).
void scanImportThunks(BinaryImage &Img);

/// Scan executable segments for padding boundaries (int3 / zero runs)
/// and register functions at the first plausible prologue after each gap.
void scanPaddingBoundaries(BinaryImage &Img);

/// Scan read-only data segments for pointer-sized values that point into
/// executable segments at plausible function prologues.
void scanDataFuncPointers(BinaryImage &Img);

/// Run all heuristic function discovery passes and emit the debug summary.
/// Called at the end of every format-specific loader.
//
// LLVM_DEBUG expands DEBUG_TYPE at the point of use, but this inline lives in a
// header that every loader includes *before* it defines its own DEBUG_TYPE, so
// a header-local one is defined here and undefined again below.  This keeps the
// header self-contained and never leaks the macro to the including TU.
#define DEBUG_TYPE "neverd-func-discovery"
inline void runPostLoadDiscovery(BinaryImage &Img,
                                 [[maybe_unused]] llvm::StringRef DebugTag) {
  // A `--func` PE load already has `.pdata` ranges and the requested body.
  // Import-thunk scanning walks every executable byte looking for IAT veneers
  // outside that table; skip it when the caller named the work set.
  if (Img.LoadOnlyFunctionEntries.empty())
    scanImportThunks(Img);
  // x64/ARM PE .pdata is the format-native function table, the same role as
  // Mach-O LC_FUNCTION_STARTS.  Padding and data-pointer scans walk every
  // executable/rodata byte and then linearly probe those ranges; on a
  // 100k-function image that dominates `--func` load.  Full-image PE still
  // needs those scans: catch labels and data-pointer callees can sit outside
  // materialized RUNTIME_FUNCTION bodies.  Skip them only for `--func`.
  const bool RestrictToExceptionDirectory =
      Img.Format == BinaryFormat::COFF &&
      !Img.LoadOnlyFunctionEntries.empty() &&
      (!Img.ExceptionMetadata.Functions.empty() ||
       !Img.COFFPDataRecords.empty() || !Img.KnownCodeRanges.empty());
  if (!RestrictToExceptionDirectory) {
    scanPaddingBoundaries(Img);
    scanDataFuncPointers(Img);
  }
  LLVM_DEBUG(Img.debugDumpSummary(llvm::dbgs(), DebugTag));
}
#undef DEBUG_TYPE

} // namespace neverd

#endif // NEVERD_LOADER_FUNCTIONDISCOVERY_H
