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

/// Address room the ELF loader leaves after an undefined data symbol of a
/// relocatable object, past the furthest offset a relocation states: code
/// reaches a field of an undefined struct at an offset no relocation states
/// when it adds the offset to the address a GOT entry holds, and that address
/// must not be another extern's.
constexpr uint64_t kObjectExternDataReach = 0x100;
/// The furthest stated offset past one undefined data symbol the room covers.
constexpr uint64_t kMaxObjectExternStatedReach = 0x10000;

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

/// Local allowance for constructing and replaying one precise guard expression
/// batch.  An ARM32 auto-vectorized reduction followed by an eight-way switch
/// uses about 13.3 KiB after exact syntax, query, and replay accounting;
/// sixteen KiB is the next power-of-two boundary.  Every visit also debits the
/// candidate-wide aggregate account.
constexpr uint32_t kMaxJumpTableGuardExpressionEvidenceWork = 16384;

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
/// largest supported O0 large-switch/jump-table transaction consumes
/// 143,404,495 units through final consumer-audited address-role replay.  The
/// 160 Mi-unit ceiling leaves 24,367,665 units of bounded headroom without
/// granting fresh per-phase or per-round allowances.
constexpr uint32_t kMaxJumpTableMaskFixedPointEvidenceWork = 167772160;
/// One jump-table candidate's whole evidence account.  Large kernel functions
/// (thousands of instructions with cold chunks) can spend most of the mask
/// fixed-point allowance on inventory prepayment alone, so the candidate keeps
/// a larger allowance for its remaining guard, role and claim proofs.
/// The MSVC two-level table in ntoskrnl 0x1406216C0 (97 cases, index table in
/// PAGE) needs about 320 million units; its proof runs in well under a second,
/// the units being conservative container-work prepayments.
constexpr uint32_t kMaxJumpTableCandidateEvidenceWork = 536870912;
static_assert(kMaxJumpTableCandidateEvidenceWork >=
              kMaxJumpTableMaskFixedPointEvidenceWork);

/// Aggregate allowance for one transactional multi-candidate resolver stage.
/// A real function can contain several exact branch occurrences that consume
/// the same physical table (peeled loops and computed-goto dispatch relays are
/// common examples).  Each occurrence remains independently capped by
/// kMaxJumpTableCandidateEvidenceWork; this larger, still finite account
/// retains four-candidate headroom so the stage can validate a complete
/// sibling batch before committing it.
constexpr uint32_t kMaxJumpTableProposalStageEvidenceWork = 2147483648u;
static_assert(uint64_t{kMaxJumpTableProposalStageEvidenceWork} >=
              uint64_t{kMaxJumpTableCandidateEvidenceWork} * 4);

/// Aggregate allowance for proving that one authenticated Med jump-table
/// target load is consumed exclusively by its recovered terminal branch.
/// The proof walks a forward SSA use closure and otherwise could rescan an
/// attacker-controlled function once for every derived value.  Exhaustion
/// keeps the ordinary relocation mirror instead of suppressing any slot.
constexpr uint32_t kMaxJumpTableTerminalUseEvidenceWork = 16777216;

/// Cumulative expression visits in one pointer recurrence query, including
/// initializer and alias proofs. A depth cap alone cannot bound a branching
/// SSA graph. Exhaustion grants no recurrence evidence.
constexpr uint32_t kMaxPointerRecurrenceEvidenceWork = 65536;

/// Maximum recursive depth while reconstructing one exact guard expression.
/// The shared evidence-work budget bounds total graph size; this separate
/// ceiling prevents a single adversarial linear chain from exhausting the C++
/// call stack before the work counter can fail the proof closed.
constexpr uint32_t kMaxJumpTableGuardExpressionDepth = 64;

/// Blocks searched straight back through single predecessors for an earlier
/// load of the address a table index loads again.  GCC bounds a switch on a
/// structure field as `cmp dword [rcx], 6; ja default` and loads the index
/// with `mov eax, [rcx]` in the next block.
constexpr uint32_t kMaxJumpTableReloadSearchBlocks = 4;

/// Target/address-role and mask fixed-point value reconstruction can cross
/// several independently authenticated loop back edges in one expanded O0
/// dispatch graph.  A 3-machine x64 selector reaches 65 distinct exact states
/// before closing its memoized cycles; retain the next power-of-two depth while
/// every visit still debits the candidate-wide graph-work account.  Generic
/// guard/domain expression proofs keep the stricter limit above.
constexpr uint32_t kMaxJumpTableExpandedResolverDepth = 128;

/// Exact target/address-role proofs for vector-reduction switches can cross
/// 129 nested value states before memoized loop aliases close.  The lexical
/// target-chain search and authenticated address-role replay use the next
/// power-of-two depth while continuing to debit the same candidate-wide work
/// account.  Whole-function speculative transform discovery retains the
/// stricter expanded limit above so unrelated vector operations cannot consume
/// the candidate budget before the local target chain is considered.
constexpr uint32_t kMaxJumpTableLargeExpressionRoleResolverDepth = 256;
static_assert(kMaxJumpTableLargeExpressionRoleResolverDepth >=
              kMaxJumpTableExpandedResolverDepth);

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

/// Most statements a nested early exit may copy onto the paths that do not
/// take it, instead of jumping over them.
constexpr size_t kMaxSkippedCopyStatements = 6;

/// Most nested ifs an early exit may sit under for its skipped tails to be
/// copied onto the paths that do not take it.
constexpr size_t kMaxSkippedCopyDepth = 6;

/// Most pure statements ahead of a return that a jump to them may copy in
/// place of the jump.
constexpr size_t kMaxReturnTailStatements = 5;

/// Most pure statements in a block that a jump to it may copy in place of
/// the jump when the block ends by jumping forward.
constexpr size_t kMaxJumpTailStatements = 4;

/// Most decision blocks one compare-tree switch may absorb.  The binary
/// search behind the largest sparse kernel switch stays well below this.
constexpr size_t kMaxCompareTreeBlocks = 512;

/// Most selector values one compare-tree case target may receive.  A target
/// reached for more values is a range test, not a list of case labels.
constexpr uint64_t kMaxCompareTreeValuesPerTarget = 8;

/// Fewest case targets besides the default for a compare tree to become a
/// switch; fewer read better as if/else.
constexpr size_t kMinCompareTreeTargets = 3;

/// Most case labels one compare-tree switch may produce.
constexpr size_t kMaxCompareTreeCases = 1024;

/// Deepest definition chain followed to express a compared value as the
/// switch selector plus a constant.
constexpr int kMaxCompareTreeEvalDepth = 32;

//===----------------------------------------------------------------------===//
// Function detection
//===----------------------------------------------------------------------===//

/// Maximum instructions to decode when verifying a candidate function entry.
constexpr int kMaxVerifyInsns = 64;

/// Maximum address distance to consider a debug symbol as "overlapping"
/// with an already-detected function.
constexpr uint64_t kMaxOverlapDistance = 0x10000;

/// Callee register-effect summaries lift callees that the pipeline did not
/// already lift (a single-function decompile lifts only its own body).  A
/// callee deeper than this many calls, or past this many extra lifts, keeps
/// the ABI clobber set.
constexpr int kMaxCallEffectCalleeDepth = 4;
constexpr size_t kMaxCallEffectExtraLifts = 256;
/// All-path predecessor stack-store evidence for one call argument setup.
/// Exhaustion supplies no partial proof.
constexpr size_t kMaxCallSetupStackProofWork = 4096;
/// A no-return proof for an internal callee lifts it, and its own proofs
/// lift their callees in turn; one this many proofs deep counts as returning.
constexpr unsigned kMaxNoReturnProofDepth = 4;
/// One x87 call-effect proof must close every returning path and callee under
/// these independent limits. Incomplete proofs grant no stack or return fact.
constexpr unsigned kMaxX87CallProofDepth = 16;
constexpr size_t kMaxX87CallProofFunctions = 128;
constexpr size_t kMaxX87CallProofWork = 262144;
/// Alignment no-ops between a call and the next function are fewer bytes
/// than the widest function alignment compilers use (64); a longer run is
/// not taken as padding.
constexpr size_t kMaxAlignmentPaddingBytes = 64;
/// A callee whose code range holds no return instruction is worth a no-return
/// proof even where no padding follows the call.  Functions that never return
/// are small error helpers; a larger range is not decoded for the check.
constexpr uint64_t kMaxNoReturnScreenBytes = 0x4000;
/// Revisits of one block before a callee's incoming-stack-read summary widens
/// a still-changing stack offset (a pointer stepped around a loop) to unknown.
constexpr unsigned kMaxStackOffsetJoinVisits = 8;

/// `--func` may attach out-of-line catch/unwind pdata, but a malformed or
/// merged runtime-function range must not import that owner's entire EH
/// graph.  Real MSVC methods stay well below 1 MiB.
constexpr uint64_t kMaxOnlyFunctionEHOwnerSize = 0x100000;

//===----------------------------------------------------------------------===//
// Expression tree / IR limits
//===----------------------------------------------------------------------===//

/// Maximum recursion depth for expression inlining.
constexpr int kMaxExprDepth = 500;

/// Maximum SSA nodes to process in a single function.
constexpr int kMaxSSANodes = 10000;

/// Skip HighIR control-flow structuring when MedIR exceeds this many blocks.
/// Flattened or obfuscated functions stay as goto skeletons.
constexpr size_t kMaxStructurableMedBlocks = 1024;

/// Skip HighIR structuring when MedIR exceeds this many ops.  Twice the
/// per-function SSA budget: structuring is cheaper to attempt than to finish
/// on a megafunction, while Med type inference still uses \c kMaxSSANodes.
constexpr size_t kMaxStructurableMedOps =
    static_cast<size_t>(kMaxSSANodes) * 2u;

/// Build SSA and run the MedIR optimizations for functions up to this many
/// operations.  HighIR structuring keeps its own, smaller limit; a larger
/// SSA function is still lowered to statements, block by block.
constexpr size_t kMaxSSAFunctionOps = 400000;

/// Maximum estimated stack frame size.
constexpr int64_t kMaxFrameSize = 16 * 1024 * 1024; // 16 MiB

/// Operations, graph edges and retained-word visits in the necessary AArch64
/// indirect-tail frame guard. Exhaustion retains the original indirect branch.
constexpr size_t kMaxIndirectTailFrameWork = 262144;

/// Joins at one block after which a stack-offset bound that is still moving
/// (a stack pointer that drifts around a loop) becomes unbounded.
constexpr unsigned kStackOffsetWideningJoins = 8;

/// How many stores before a call to scan for stack-passed arguments.
constexpr int kCallArgStoreScanWindow = 12;

/// How many copies, extensions and constant adjustments a store address
/// before a call is followed back to the pointer it is made from.
constexpr int kCallArgStoreAddressDepth = 32;

/// How many definitions a register an indirect call goes through is
/// followed back to the slot it was loaded from.
constexpr int kCallTargetSlotDepth = 16;

/// AArch64 calls with the complete x0-x7 prefix can have an integer argument
/// at [sp]. Materializing eight constants may expand into several MedIR ops per
/// register, so keep a larger but finite window once the full bank itself
/// proves that stack overflow is ABI-plausible.
constexpr int kAArch64FullBankCallArgStoreScanWindow = 64;

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

/// Graph expansions for one scalar-offset provenance proof of a table address.
/// Exhaustion leaves the proof undecided, and a memoized closed-graph proof may
/// still decide it.
constexpr int kMaxScalarOffsetProofNodes = 16384;

/// Expansions for the single retry of a scalar-offset proof that exhausted
/// kMaxScalarOffsetProofNodes and that the closed-graph proof could not decide.
/// The path-sensitive proof re-enters a loop induction PHI once for every
/// vector lane that reads it, since a PHI's result depends on its anchoring
/// context and is never cached.  A NEON base64 encoder (a64o157_b64) needs
/// more than 32768 expansions for its table offsets; keep the next power of
/// two.  Exhaustion of the retry means "no proof": the emitter keeps the raw
/// model or refuses the stale-address fallback.
constexpr int kMaxEscalatedScalarOffsetProofNodes = 65536;
static_assert(kMaxEscalatedScalarOffsetProofNodes >=
              kMaxScalarOffsetProofNodes);

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

/// Bound source-ABI signatures (explicit components, not recovered scans).
constexpr int kMaxBoundSourceCallArgs = 64;

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

/// Minimum piece size (bytes) of an x86 call sweep split across workers.
/// Joining the pieces costs a few instructions a seam, so they can be small.
constexpr size_t kMinFuncScanPiece = 16 * 1024;

/// Pieces an x86 call sweep gives each worker.  Workers claim pieces as they
/// finish, so with several each a slow core holds up only its last piece.
constexpr size_t kFuncScanPiecesPerWorker = 8;

/// Instructions of a binary file's x86 code read for evidence of its
/// platform, and bytes of other code: enough for thousands of call sites,
/// and a bounded share of opening a large file.
constexpr uint64_t kPlatformEvidenceInstructions = 1024 * 1024;
constexpr uint64_t kPlatformEvidenceBytes = 64 * 1024 * 1024;

/// The evidence that decides a binary file's platform: at least this score
/// for the leader, and this many times the runner-up's.  Reading stops early
/// once the leader has the decisive score and ratio.
constexpr uint64_t kPlatformMinimumScore = 4;
/// x86 instructions a linear sweep may read per point of a binary file's
/// platform evidence.  Compiled code sets a call's arguments every few
/// dozen instructions; data the sweep decodes as code scores a hundred
/// times more sparsely, so sparse evidence decides nothing.
constexpr uint64_t kPlatformInstructionsPerScore = 500;
constexpr uint64_t kPlatformMarginRatio = 3;
constexpr uint64_t kPlatformDecisiveScore = 256;
constexpr uint64_t kPlatformDecisiveRatio = 16;

/// Instructions from where a function may start that its prologue spans,
/// for prologue evidence of a binary file's platform, and the alignment
/// compilers start functions at.
constexpr unsigned kPlatformPrologueInstructions = 6;
constexpr uint64_t kPlatformFunctionAlignment = 16;

/// How far beside a binary file's code a call may land and still count as
/// one the code makes, as its text calls the stubs next to it, and the
/// instructions before a call read for the arguments it sets.
constexpr int64_t kPlatformCallReach = 16 * 1024 * 1024;
constexpr size_t kPlatformCallWindow = 8;

/// Minimum number of detected candidates before the entry-verification trial
/// decode is spread across worker threads.  Below this the per-thread decoder
/// setup outweighs the work, so the check stays single-threaded.
constexpr size_t kMinParallelVerify = 512;

/// Minimum Low/Med/High work items before `parallelForEach` spawns 8 MiB
/// workers. `--func` expands unwind ActionVAs so the batch is often 2–20,
/// not 1; those stacks are CLI latency. Below this the caller thread runs
/// the batch (heaviest-first when weighted).
constexpr size_t kMinParallelIRWorkItems = 32;

//===----------------------------------------------------------------------===//
// C output formatting
//===----------------------------------------------------------------------===//

/// Threshold below which integer constants are printed in decimal
/// rather than hexadecimal in decompiled C output.
constexpr uint64_t kDecimalConstThreshold = 4096;

/// Display-only TPI member / enumerator names.  Structs stay small;
/// LF_ENUM string-id tables are sequential and commonly exceed 1k.
constexpr size_t kMaxTpiDisplayFields = 16384;

/// Bytes to read from MSVC `TypeDescriptor::name[]` (RTTI).
constexpr size_t kMaxMsvcTypeDescriptorNameBytes = 256;

/// Copy-forward / frame-alias name chain walks in HighC.
constexpr unsigned kMaxCopyForwardAliasDepth = 8;

/// Unwrap Cast/ZExt/SExt when matching a frame address or copy source.
constexpr unsigned kMaxIntegerViewUnwrapDepth = 8;

/// Walk INT_ADD/INT_SUB when recovering a frame displacement.
constexpr unsigned kMaxFrameDisplacementDepth = 32;

/// Fixed-point iterations when growing HighC frame aliases.
constexpr unsigned kMaxFrameAliasFixedPoint = 64;

/// (block, covered-byte mask) states explored while proving that every path to
/// a frame reload initializes each of its bytes.  Exhaustion is unproved.
constexpr size_t kMaxFrameInitializerStates = 4096;

/// Compact unused HighC parameters only when at least this many are unused.
/// A lower threshold drops trailing ABI arguments such as
/// `identity(values, 0)`.
constexpr unsigned kMinUnusedParamsToCompact = 4;

/// Nesting at which HighC expression printing is truncated.  Separate from
/// \c kMaxExprDepth, which bounds IR inlining rather than C text.
constexpr int kMaxCExprPrintDepth = 200;

/// Pointer-chain walk when asking whether a C type contains a function.
constexpr unsigned kMaxCPointerNesting = 16;

/// Largest fixed integer vector represented by a C compiler vector type.
constexpr unsigned kMaxCIntegerVectorBytes = 64;

/// HighC dead-store / store-forward expression walks.
constexpr unsigned kMaxHighCMemoryWalkDepth = 128;

/// Per-expression and aggregate byte budget when inlining forwarded stores
/// into C text (the writer serializes DAGs as trees).
constexpr size_t kMaxHighCForwardedExpressionBytes = 16 * 1024;

/// Borrowed/read-only runtime byte blobs named in HighC source calls.
constexpr uint32_t kMaxSourceCallBorrowedBytes = 1024 * 1024;

/// Skip aggressive if/else folding above this MedIR block count.
constexpr size_t kMaxIfElseStructuringBlocks = 500;

/// HighIR statement count above which loop/if structuring is treated as mega.
constexpr size_t kMaxStructuredHighStmts = 4000;
/// Top-level statement bound for the late goto-reduction phases.  Their
/// scans are near-linear per round; oversized block-by-block functions
/// stay within a few seconds at this size.
constexpr size_t kMaxLateGotoReductionStmts = 400000;

/// Statements before a branch whose assignments are substituted into its
/// condition when comparing it with an earlier branch.  Substitution only
/// exposes equal subexpressions, so a shorter window finds fewer implied
/// conditions but never an unsound one; scanning the whole prefix for every
/// branch was cubic on long bodies.
constexpr size_t kMaxComposedWorkAssigns = 64;

/// HighC value forwarding (folding a single-use temporary into its use)
/// rescans every statement for each candidate.  Larger bodies, which only
/// arise when an oversized function is lowered block by block, keep their
/// temporaries.
constexpr size_t kMaxValueForwardSites = 8192;

/// if/else folding passes for ordinary vs large CFGs.
constexpr int kIfElseStructuringPasses = 10;
constexpr int kIfElseLargeCfgPasses = 3;
/// Newly folded then/else arms already lived in the parent list. Re-drain
/// them with this many passes; the parent still runs kIfElseStructuringPasses
/// and structureIfElseNested already structured existing children.
constexpr int kIfElseNestedArmPasses = 1;

/// x86 registration-chain prologue helper expansion fixed point.
constexpr unsigned kMaxRegistrationEHFixedPoint = 16;

/// Shared limit on x86 registration language-table records and state domains.
constexpr uint32_t kMaxRegistrationEHRecords = 4096;

/// Total registration-state propagation work, including state/edge pairs.
/// Exhaustion invalidates the whole result rather than truncating a domain.
constexpr size_t kMaxRegistrationEHStateWork = 1048576;

/// Default MXCSR value (x86 SSE control/status register).
constexpr uint64_t kDefaultMXCSR = 0x1F80;

} // namespace limits
} // namespace neverd

#endif // NEVERD_LIMITS_H
