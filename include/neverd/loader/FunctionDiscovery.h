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
#include "neverd/support/LoadProgress.h"
#include "neverd/support/ProloguePatterns.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <memory>

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

/// Register the initialization and termination code the loader runs (ELF
/// DT_INIT, DT_FINI and the preinit, init and fini arrays, Mach-O
/// initializers) as function symbols: the format names them function entries
/// even when no code in the image calls them.  Their sizes stay unknown.
void registerLoaderRunFunctions(BinaryImage &Img);

/// Scan executable segments for import thunk patterns (jmp [rip+disp32]
/// on x86_64, jmp [abs32] on x86, ADRP/LDR/BR x16 on AArch64), map each
/// recognized veneer to its Import, and register it as a function symbol.
/// Shared by all loaders (COFF IAT thunks, ELF PLT stubs).
void scanImportThunks(BinaryImage &Img);

/// Scan executable segments for padding boundaries (int3 / zero runs)
/// and register functions at the first plausible prologue after each gap.
void scanPaddingBoundaries(BinaryImage &Img, const LoadProgress &Progress = {});

/// Register the hotpatchable entries of a linked x86 PE image (see
/// isX86HotpatchEntryAt).  x86 PE has no function table, and optimized MSVC
/// code packs functions after `ret` without padding, where the padding scan
/// cannot see them.
void scanX86HotpatchEntries(BinaryImage &Img,
                            const LoadProgress &Progress = {});

/// Scan read-only data segments for pointer-sized values that point into
/// executable segments at plausible function prologues.
void scanDataFuncPointers(BinaryImage &Img, const LoadProgress &Progress = {});

/// How an image holds an AArch32 code pointer, which says how much of the
/// instruction set its bit 0 states.
enum class ARMCodePointerUse : uint8_t {
  /// The runtime calls through it, as `blx` does: an ELF DT_INIT or DT_FINI
  /// value, an init, fini or preinit array entry, or an IRELATIVE resolver.
  /// Bit 0 set is Thumb code, clear is ARM code.
  Called,
  /// A relocated or GOT slot, which may hold a data address.  Bit 0 set is
  /// still a Thumb code address, in a section the image marks executable;
  /// clear, it states nothing.
  Stored,
};

/// Record the instruction set that \p Pointer, an AArch32 code pointer of a
/// linked image, states for the code it addresses, as an exact entry that
/// reachable mode discovery follows.  A pointer outside executable code
/// states nothing.  Mapping symbols already decide what they cover, and a
/// called pointer that contradicts them or an exact entry is reported as a
/// load diagnostic instead of recorded.
void recordARMCodePointerMode(BinaryImage &Img, uint64_t Pointer,
                              ARMCodePointerUse Use);

/// Record the modes the stored code pointers of a linked AArch32 image state:
/// the values of its relocated pointer slots and GOT entries (BaseRelocations)
/// once every symbol has stated its own.
void recordARMStoredCodePointerModes(BinaryImage &Img);

/// Reachable ARM/Thumb mode discovery for one image, which keeps what it
/// decoded: evidence read later, the landing pads of the exception tables,
/// extends it without decoding the image again.
class ARMModeDiscovery {
public:
  explicit ARMModeDiscovery(BinaryImage &Image);
  ~ARMModeDiscovery();
  ARMModeDiscovery(const ARMModeDiscovery &) = delete;
  ARMModeDiscovery &operator=(const ARMModeDiscovery &) = delete;

  /// Follow direct ARM/Thumb control-flow edges from exact code entries.
  /// Records only decoded reachable instruction spans; malformed or
  /// conflicting state transitions fail instead of assigning a file-wide
  /// default mode.
  llvm::Error discover();

  /// After the exception tables are read, follow the landing pads of the call
  /// sites decoding reached, in the mode of that code: the unwinder resumes a
  /// frame in the instruction set of its PC.  The last phase: it releases
  /// what the discovery kept.
  llvm::Error followLandingPads();

  /// How many instructions the discovery disassembled: each address once in
  /// each mode a path reached it in, however many passes walk it.
  size_t disassembledInstructions() const { return Disassembled; }

private:
  struct State;
  BinaryImage &Img;
  size_t Disassembled = 0;
  /// What the discovery keeps between its phases, once it decodes.
  std::unique_ptr<State> S;
};

/// ARMModeDiscovery(Img).discover(), for a loader that reads no exception
/// tables afterwards.
llvm::Error discoverARMReachableModes(BinaryImage &Img);

/// Add caller-asserted exact function entry modes after format metadata and
/// relocations have been parsed, before reachable mode discovery. Contradictory
/// assertions and non-executable entries fail the load.
llvm::Error
applyARMFunctionModeHints(BinaryImage &Img,
                          const std::map<va_t, InstructionMode> &Hints);

/// When reachable decoding ran, require every asserted entry to have decoded
/// at least its first instruction in the asserted mode.
llvm::Error
verifyARMFunctionModeHints(const BinaryImage &Img,
                           const std::map<va_t, InstructionMode> &Hints);

/// Correct relocated pointers to literal data inside executable sections when
/// every decoded path through the literal region traps, and no code metadata
/// owns the target. Unknown targets retain their original classification.
void recordRelocatedLiteralTargets(BinaryImage &Img);

/// Run all heuristic function discovery passes and emit the debug summary.
/// Called at the end of every format-specific loader.
//
// LLVM_DEBUG expands DEBUG_TYPE at the point of use, but this inline lives in a
// header that every loader includes *before* it defines its own DEBUG_TYPE, so
// a header-local one is defined here and undefined again below.  This keeps the
// header self-contained and never leaks the macro to the including TU.
#define DEBUG_TYPE "neverd-func-discovery"
inline void runPostLoadDiscovery(BinaryImage &Img,
                                 [[maybe_unused]] llvm::StringRef DebugTag,
                                 const LoadProgress &Progress = {}) {
  Progress.report("load-discovery", 0, 1, "relocated literals");
  recordRelocatedLiteralTargets(Img);
  // Format-stated entries come before every heuristic scan.
  Progress.check();
  Progress.report("load-discovery", 0, 1, "loader entries");
  registerLoaderRunFunctions(Img);
  Progress.check();
  // A `--func` PE load already has `.pdata` ranges and the requested body.
  // Import-thunk scanning walks every executable byte looking for IAT veneers
  // outside that table; skip it when the caller named the work set.
  Progress.report("load-discovery", 0, 1, "import thunks");
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
    // Before the padding scan, which would otherwise record a hotpatch entry
    // after padding as a mere boundary guess.
    Progress.check();
    Progress.report("load-discovery", 0, 1, "hotpatch entries");
    scanX86HotpatchEntries(Img, Progress);
    Progress.check();
    Progress.report("load-discovery", 0, 1, "padding boundaries");
    scanPaddingBoundaries(Img, Progress);
    Progress.check();
    Progress.report("load-discovery", 0, 1, "data function pointers");
    scanDataFuncPointers(Img, Progress);
  }
  Progress.report("load-discovery", 1, 1);
  LLVM_DEBUG(Img.debugDumpSummary(llvm::dbgs(), DebugTag));
}
#undef DEBUG_TYPE

} // namespace neverd

#endif // NEVERD_LOADER_FUNCTIONDISCOVERY_H
