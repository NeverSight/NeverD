//===- Limits.h - Tunable analysis limits and thresholds ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Central repository for analysis limits, heuristic thresholds, and
/// configurable constants used across the decompilation pipeline.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIMITS_H
#define NEVERD_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd {
namespace limits {

//===----------------------------------------------------------------------===//
// Binary loading
//===----------------------------------------------------------------------===//

/// Upper bound on the bss-style zero-fill expansion of a single loaded
/// segment.  A segment's in-memory size (ELF p_memsz, Mach-O vmsize, COFF
/// VirtualSize) is attacker-controlled and can sit near the 64-bit maximum;
/// materializing it verbatim would attempt a multi-terabyte allocation and
/// abort the process.  Segments requesting more than this keep only their
/// file-backed bytes — reads into the un-materialized tail are already handled
/// gracefully by the Segment bounds checks.  The cap is far larger than any
/// real .bss, so legitimate binaries are unaffected.
constexpr uint64_t kMaxSegmentZeroFill = 1ull << 30; // 1 GiB

//===----------------------------------------------------------------------===//
// Jump-table resolution
//===----------------------------------------------------------------------===//

/// Hard upper bound on jump-table entries.  Tables exceeding this are
/// almost certainly false positives.
constexpr uint32_t kMaxJumpTableEntries = 4096;

/// Aggregate retained-state/query budget for one occurrence-level jump-table
/// evidence phase.  Modulo inference partitions this total into fixed proposal,
/// structural-retention, direct, and replay accounts whose sum remains this
/// value; the resolver's expression traversal separately shares one instance
/// across its complete structural-query batch.  Candidate enumeration is
/// input-controlled, so every retained item and query allocation is prepaid.
constexpr uint32_t kMaxJumpTableEvidenceWork = 4096;

/// Stage-local allowance for authenticating a stack-materialized jump table.
/// Exact Fold/emulator accounting for PIC relocatable O0 local-table functions
/// exceeds the generic four-KiB allowance; sixteen KiB is the next power-of-two
/// bound.
/// Every stack-table candidate in one immutable resolver graph shares it.
constexpr uint32_t kMaxJumpTableStackEvidenceWork = 16384;

/// Local syntax allowance while correlating path-qualified guard aliases.
/// A later O0 state-machine dispatch exceeds eight KiB while every visit
/// continues to debit the candidate-wide aggregate account.  Sixteen KiB is
/// the next power-of-two boundary that completes the supported case.
constexpr uint32_t kMaxJumpTableGuardAliasEvidenceWork = 16384;

/// Local symbolization allowance for an ordinary unsigned-bound query.  The
/// largest supported later O0 state-machine dispatch exceeds the generic
/// occurrence allowance while remaining well below one complete value-match
/// batch.  Every visit also debits the candidate-wide aggregate account.
constexpr uint32_t kMaxJumpTableBoundSymbolEvidenceWork = 65536;

/// Local symbolization allowance for an exact finite-coordinate query.  Such
/// a query may symbolize the expanded candidate graph before enumerating at
/// most 64 coordinates.  The largest supported 64-way expanded selector uses
/// about 68 Ki work; retain the next power of two.  Every visit also debits the
/// candidate-wide aggregate account, so this allowance cannot multiply
/// whole-CFG work across queries.
constexpr uint32_t kMaxJumpTableFiniteSetSymbolEvidenceWork = 131072;

/// Per-core proposal allowance inside one mask-domain fixed point.  Retained
/// LowIR, coordinates and proposal batches also debit the aggregate account
/// below; this smaller ceiling prevents one recursive core from monopolizing
/// it while accommodating the largest supported real computed-goto core.
constexpr uint32_t kMaxJumpTableMaskCoreEvidenceWork = 524288;

/// Per-query-batch structural match allowance.  Every node/comparison also
/// debits the caller's aggregate account when one is supplied; this local cap
/// keeps legacy non-fixed-point callers bounded as well.
constexpr uint32_t kMaxJumpTableValueMatchEvidenceWork = 65536;

/// Mask-domain proof batches run on the expanded candidate graph and can
/// compare several selector occurrences in one transaction.  The largest
/// supported 128-way expanded fixed-point replay uses about 291 Ki work in one
/// batch; retain the next power of two.  Every unit continues to debit the
/// same candidate-wide aggregate account.
constexpr uint32_t kMaxJumpTableMaskMatchEvidenceWork = 524288;

/// Target- and address-role certificates batch every feasible transform and
/// reaching-value alternative in one immutable candidate graph.  Exact
/// accounting for the largest supported O0 large-switch role consumes about
/// 323 Ki work in one batch, so retain the next power of two locally.  Every
/// unit also debits the candidate-wide aggregate account below.
constexpr uint32_t kMaxJumpTableRoleMatchEvidenceWork = 524288;

/// Whole-object consumer audits can compare every byte occurrence in a
/// physical pointer object against every candidate consumer.  Exact graph,
/// memo, string and container-lifetime accounting makes the largest supported
/// x64/AArch64 initializer audit consume about 154 Ki work, while the largest
/// supported O0 12-way expanded object-escape audit consumes about 539 Ki.
/// An i386 PIC 140-way switch with every final target exposed consumes about
/// 9.3 Mi work in the same immutable audit.  Retain the next power of two
/// locally.  The work still debits the same candidate-wide aggregate account.
constexpr uint32_t kMaxJumpTableConsumerAuditMatchEvidenceWork = 16777216;

/// Aggregate allowance for completing one function's exact i386 GOT-base
/// models.  Completion performs whole-graph value matching; the largest
/// supported 762-instruction O0 switch consumes about 377 Ki, so retain the
/// next power of two.  Test overrides may still select a smaller fail-closed
/// boundary.
constexpr uint32_t kMaxI386GOTModelEvidenceWork = 1048576;

/// Candidate-local exact GOTOFF reaching proof.  An O0 140-way i386 expanded
/// graph consumes about 1.50 MiB after occurrence and cache bookkeeping.  This
/// allowance is reserved from, and refunds its unused tail to, the candidate
/// aggregate account; it is not a fresh per-query budget.
constexpr uint32_t kMaxI386GOTOFFProposalEvidenceWork = 2097152;

/// Structural-symbolization allowance shared by the exact unsigned-modulo
/// recipe queries for one candidate.  The largest supported O0 five-way frame
/// relay consumes about 140 Ki work; retain the next power of two.  Expression
/// visits also debit the candidate-wide aggregate account, so this local
/// ceiling cannot multiply whole-CFG work across query batches.
constexpr uint32_t kMaxJumpTableModuloRecipeSymbolEvidenceWork = 262144;

/// Aggregate allowance for one jump-table candidate in a resolver stage.
/// Target/address roles, modulo/mask domains, every candidate-graph snapshot,
/// recursive core proof, and precise-before-upper-bound replay all debit this
/// one balance.  Exact ordered-container and lifetime accounting for the
/// largest supported O0 large-switch/jump-table transaction consumes about 101
/// million units after exact target-role certificate reuse; the next
/// power-of-two ceiling preserves bounded headroom without granting fresh
/// per-phase or per-round allowances.
constexpr uint32_t kMaxJumpTableMaskFixedPointEvidenceWork = 134217728;

/// Aggregate allowance for one transactional multi-candidate resolver stage.
/// A real function can contain several exact branch occurrences that consume
/// the same physical table (peeled loops and computed-goto dispatch relays are
/// common examples).  Each occurrence remains independently capped by
/// kMaxJumpTableMaskFixedPointEvidenceWork; this larger, still finite account
/// retains four-candidate headroom so the stage can validate a complete
/// sibling batch before committing it.
constexpr uint32_t kMaxJumpTableProposalStageEvidenceWork = 536870912;
static_assert(uint64_t{kMaxJumpTableProposalStageEvidenceWork} >=
              uint64_t{kMaxJumpTableMaskFixedPointEvidenceWork} * 4);

/// Aggregate allowance for proving that one authenticated Med jump-table
/// target load is consumed exclusively by its recovered terminal branch.
/// The proof walks a forward SSA use closure and otherwise could rescan an
/// attacker-controlled function once for every derived value.  Exhaustion
/// keeps the ordinary relocation mirror instead of suppressing any slot.
constexpr uint32_t kMaxJumpTableTerminalUseEvidenceWork = 16777216;

/// Maximum recursive depth while reconstructing one exact guard expression.
/// The shared evidence-work budget bounds total graph size; this separate
/// ceiling prevents a single adversarial linear chain from exhausting the C++
/// call stack before the work counter can fail the proof closed.
constexpr uint32_t kMaxJumpTableGuardExpressionDepth = 64;

/// Target/address-role and mask fixed-point value reconstruction can cross
/// several independently authenticated loop back edges in one expanded O0
/// dispatch graph.  A 3-machine x64 selector reaches 65 distinct exact states
/// before closing its memoized cycles; retain the next power-of-two depth while
/// every visit still debits the candidate-wide graph-work account.  Generic
/// guard/domain expression proofs keep the stricter limit above.
constexpr uint32_t kMaxJumpTableExpandedResolverDepth = 128;

/// Deterministic resource ceilings for the exact bit-domain query used to
/// validate a reconstructed jump-table guard.  Exhaustion means "no proof"
/// and therefore rejects that guard; it never falls back to sampled values.
constexpr uint64_t kMaxJumpTableGuardSolverConflicts = uint64_t(1) << 14;
constexpr uint64_t kMaxJumpTableGuardSolverPropagations = uint64_t(1) << 20;
constexpr uint64_t kMaxJumpTableGuardSolverWatchVisits = uint64_t(1) << 22;
constexpr size_t kMaxJumpTableGuardSolverGates = size_t(1) << 18;

/// Minimum number of resolved targets for a table to be accepted.
constexpr uint32_t kMinJumpTableEntries = 2;

/// Maximum byte distance from function entry at which a jump-table
/// target is still considered valid.
constexpr uint64_t kMaxJumpTargetDistance = 0x100000; // 1 MiB

/// Maximum consecutive duplicate targets before we stop reading an
/// unbounded table.
constexpr int kMaxDuplicateRun = 3;

/// Maximum number of guard COND_BRs to walk backward through.
constexpr int kMaxGuardBranches = 4;

/// Maximum depth of backward slicing through CFG predecessors when
/// searching for guard bounds.
constexpr int kMaxGuardPredDepth = 3;

/// Maximum operations to trace backward when slicing for table base.
constexpr int kMaxSliceDepth = 32;

/// Maximum invalid/skipped entries tolerated when reading a bounded table.
constexpr int kMaxSkippedEntries = 4;

/// Maximum NdOp operations to scan backward looking for a guard comparison
/// when no CFG predecessor info is available.
constexpr int kMaxGuardScanOps = 64;

/// Maximum jump-table entry size in bytes (pointer width ceiling).
constexpr uint16_t kMaxEntryBytes = 8;

/// Maximum shift amount that can imply an entry-size multiplier
/// (1<<kMaxShiftForEntrySize == kMaxEntryBytes).
constexpr uint64_t kMaxShiftForEntrySize = 3;

/// Minimum bytes of data that must be readable at a target address for
/// the sanity checker to accept it.
constexpr uint32_t kMinTargetDataBytes = 4;

/// Maximum number of CFG predecessor paths to walk when attempting a
/// dual-path (default-value) jump-table recovery.
constexpr int kMaxDualPathPreds = 4;

/// Maximum number of quasi-copy operations to follow when tracing a
/// switch variable through COPY/AND/OR/ZEXT/SEXT/SUBBYTES chains.
constexpr int kMaxQuasiCopyDepth = 16;

/// Maximum predecessor blocks to consider when checking for unrolled
/// (duplicated) guard COND_BRs across multiple incoming paths.
constexpr int kMaxUnrolledGuardPreds = 8;

/// Maximum predecessor blocks to traverse when collecting path ops
/// for cross-block emulation.
constexpr int kMaxPathEmulationDepth = 4;

/// Maximum total ops to collect across blocks for emulation paths.
constexpr int kMaxPathEmulationOps = 256;

/// Maximum normalization base value for switch-variable subtraction.
/// Values beyond this are unlikely to be legitimate case-base offsets.
constexpr int64_t kMaxNormBase = 0x10000;

/// Maximum normalization shift for switch-variable right-shift.
constexpr uint32_t kMaxNormShift = 5;

/// Minimum instruction alignment for ARM/AArch64 targets.
/// x86 has no alignment requirement (set to 1).
constexpr uint32_t kMinInsnAlignARM = 2;
constexpr uint32_t kMinInsnAlignAArch64 = 4;
constexpr uint32_t kMinInsnAlignX86 = 1;

/// Maximum number of multi-stage recovery attempts.  Each stage re-analyzes
/// unresolved branches and every table whose range used a whole-CFG proof.
/// The extra headroom lets nested target discovery reach a fixed point; an
/// unfinished proof-dependent table is discarded when this budget expires.
constexpr int kMaxMultiStageRetries = 16;

/// Maximum number of LOAD records tracked during a single emulated
/// path evaluation.  Exceeding this indicates runaway emulation.
constexpr int kMaxLoadRecords = 256;

/// Maximum number of entries in the emulator's write-back store.
/// Limits memory consumption when emulating long op sequences.
constexpr int kMaxEmulatorStoreEntries = 64;

/// Minimum proportion of valid (executable, aligned) targets for a
/// candidate table to pass sanity checking (0–100).
constexpr int kMinValidTargetPercent = 75;

/// Maximum distance between consecutive table entries before the
/// target sequence is considered broken (heuristic for sparse tables).
constexpr uint64_t kMaxConsecutiveEntryGap = 0x100000;

/// Threshold for CircleRange size above which the guard analysis
/// assumes the switch variable is non-negative (positive range only).
/// Ranges larger than this are likely full-width artifacts.
constexpr uint64_t kPositiveRangeThreshold = 0x10000;

/// Full range of a 1-byte switch variable (2^8).  A guard that spans
/// this entire range without an explicit comparison is rejected.
constexpr uint64_t kByteVarFullRange = 256;

/// Maximum bit position to scan when extracting a stride from an AND
/// mask.  Must be ≥ the widest register bit-width.
constexpr uint32_t kMaxStrideScanBits = 64;

/// Maximum recursion depth when decomposing the `quotient * N` back-multiply
/// of a `switch(x % N)` remainder into its shift/add/sub terms to read N.
constexpr int kMaxModuloDecompDepth = 24;

/// Maximum recursion depth when tracing a value back to the stack pointer (or
/// forward to a stack-pointer write) to recognise a dynamic `alloca` / VLA. The
/// SP threads through a long copy/sub-register/extend chain before the
/// subtract.
constexpr int kMaxStackPtrTraceDepth = 24;

//===----------------------------------------------------------------------===//
// Function detection
//===----------------------------------------------------------------------===//

/// Maximum instructions to decode when verifying a candidate function entry.
constexpr int kMaxVerifyInsns = 64;

/// Maximum address distance to consider a debug symbol as "overlapping"
/// with an already-detected function.
constexpr uint64_t kMaxOverlapDistance = 0x10000;

//===----------------------------------------------------------------------===//
// Expression tree / IR limits
//===----------------------------------------------------------------------===//

/// Maximum recursion depth for expression inlining.
constexpr int kMaxExprDepth = 500;

/// Maximum SSA nodes to process in a single function.
constexpr int kMaxSSANodes = 10000;

/// Maximum estimated stack frame size.
constexpr int64_t kMaxFrameSize = 16 * 1024 * 1024; // 16 MiB

//===----------------------------------------------------------------------===//
// Backend / code generation
//===----------------------------------------------------------------------===//

/// Target MedIR op count for one LLVM emission shard.
///
/// Peak lift memory is dominated by the shards in flight at once: each holds a
/// private LLVMContext and its slice in the emitter's pre-mem2reg form (an
/// alloca plus load/store per temporary, several times the size of the
/// optimized IR).  Bounding a shard's slice therefore bounds that transient
/// emission component to roughly (worker threads) x (this budget), rather than
/// keeping the whole input's unoptimized LLVM IR resident at once.  Retained
/// Low/MedIR, bitcode, and the final module still scale with the input.  The
/// budget is large enough that per-shard setup and extra link steps stay noise.
constexpr uint64_t kMaxShardOps = 8000;

/// Addresses below this threshold are not considered valid global data
/// references.
constexpr uint64_t kMinGlobalDataAddr = 0x1000;

/// Maximum bytes to scan ahead when looking for a string literal.
constexpr uint32_t kMaxStringScanLen = 4096;

/// Maximum bytes of constant data to embed inline in LLVM IR globals.
/// Applies to read-only segment data (jump tables, vtables, etc.)
/// that must travel with the code for ASLR-safe binary rewriting.
constexpr size_t kMaxEmbeddedDataLen = 4096;

/// Maximum bytes for a SINGLE cohesive embedded global — one whole rodata run
/// or executable-segment literal pool rebuilt by embedRodataRun /
/// embedExecSegmentRun and GEP'd into for every access.  This is the smallest
/// possible form: the per-constant fallback (each copy bounded by
/// kMaxEmbeddedDataLen) duplicates the run O(N) times, far larger (e.g. an 8 KB
/// ARM32 NEON pool fell back to ~2 MB of overlapping copies).  The cap is
/// therefore generous and only guards against an abnormally huge segment.
constexpr size_t kMaxSingleGlobalEmbedLen = 1u << 20; // 1 MiB

/// Maximum number of bytes a GOTOFF-folded rodata table base may precede the
/// segment that holds its symbol.  clang's switch-to-lookup-table indexed by
/// the unbiased case value folds the base to `table - min_case*stride`; a sane
/// `min_case*stride` is well under this, so a larger backward distance is taken
/// as a coincidental integer rather than a genuine table anchor.
constexpr uint64_t kMaxRodataAnchorBackDistance = 0x10000;

//===----------------------------------------------------------------------===//
// Structuring / SSA
//===----------------------------------------------------------------------===//

/// Sentinel variable ID used for synthesized temporaries that must not
/// collide with real SSA IDs.
constexpr int kPhiCondTempId = 99999;

/// Base ID for renamed variables in HighVarRename to avoid collision
/// with real SSA IDs.
constexpr int kVarRenameIdBase = 50000;

//===----------------------------------------------------------------------===//
// Calling-convention / ABI recovery
//===----------------------------------------------------------------------===//

/// Maximum number of arguments recovered per call site and parameters per
/// function (register-passed plus stack-passed).  Counted in pointer-size
/// slots, so a 32-bit ABI splits each 8-byte `double`/`long long` into two
/// slots: 32 slots covers up to ~16 double arguments.  Functions taking more
/// arguments than this are rare; the cap bounds the recovery scans.
constexpr int kMaxCallArgs = 32;

//===----------------------------------------------------------------------===//
// Variadic (...) ABI recovery
//===----------------------------------------------------------------------===//

/// x86-64 SysV `va_start` packs the GP- and FP-register save-area offsets into
/// one 64-bit word ((fp_offset << 32) | gp_offset) stored up front.  gp_offset
/// is a named-integer-register count (0..48, step 8); fp_offset starts past the
/// GP save area (48) and counts named FP-register args (48..176, step 16).
/// Recognising this word marks a variadic prologue.
constexpr uint64_t kX64VaGpOffsetMax = 48; ///< 6 GP arg registers * 8
constexpr uint64_t kX64VaGpOffsetStep = 8;
constexpr uint64_t kX64VaFpOffsetMin = 48;  ///< FP save area follows the GP one
constexpr uint64_t kX64VaFpOffsetMax = 176; ///< 48 + 8 XMM registers * 16
constexpr uint64_t kX64VaFpOffsetStep = 16;

/// Largest plausible byte offset above the entry stack pointer at which the
/// variadic overflow (incoming-stack) area can begin; bounds the search for the
/// va_list overflow pointer so an unrelated SP-derived store is not taken for
/// it.
constexpr int64_t kVariadicOverflowBaseMax = 256;

/// Extra bytes reserved above frame_end (beyond the spilled overflow params) so
/// a wide va_arg read (e.g. an ARM `vld1` straddling the save/overflow
/// boundary) stays inside the alloca frame.
constexpr int64_t kVariadicOverflowSlop = 64;

//===----------------------------------------------------------------------===//
// Function detection
//===----------------------------------------------------------------------===//

/// Minimum chunk size (bytes) for parallel function scanning.  The scan
/// spawns workerThreadCount() workers, each claiming chunks of at least this
/// size, so a small binary stays effectively single-threaded regardless of the
/// core count.
constexpr size_t kMinFuncScanChunk = 64 * 1024;

/// Minimum number of detected candidates before the entry-verification trial
/// decode is spread across worker threads.  Below this the per-thread decoder
/// setup outweighs the work, so the check stays single-threaded.
constexpr size_t kMinParallelVerify = 512;

//===----------------------------------------------------------------------===//
// C output formatting
//===----------------------------------------------------------------------===//

/// Threshold below which integer constants are printed in decimal
/// rather than hexadecimal in decompiled C output.
constexpr uint64_t kDecimalConstThreshold = 4096;

/// Default MXCSR value (x86 SSE control/status register).
constexpr uint64_t kDefaultMXCSR = 0x1F80;

} // namespace limits
} // namespace neverd

#endif // NEVERD_LIMITS_H
