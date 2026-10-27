//===- Limits.h - Binary comparison limits and defaults ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Heuristic thresholds, candidate bounds and work budgets of the binary
/// comparison.  The features themselves (what a shape or an MD index is) stay
/// with the code that computes them; only the values a caller could tune live
/// here.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_LIMITS_H
#define NEVERD_DIFF_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::diff {

/// Select a qualified same-ISA profile from the loaded inputs.
inline constexpr const char *kDefaultNativeProfile = "native-same-isa-v1";

// Optional source inspection reuses SDK source-page byte/line ceilings.
// The caller may set the additional native instruction allowance to zero.
inline constexpr uint64_t kDefaultSourceNativeInstructions = 4096;
inline constexpr size_t kMaxSourceReasonBytes = 4096;
inline constexpr uint64_t kDefaultSourceAlignmentWork = 50000000;
// Combined UTF-8 text and serialized line evidence retained for a source pair.
// Zero in the request removes this additional allowance, not emitter limits.
inline constexpr uint64_t kDefaultSourceAlignmentBytes = 64 * 1024 * 1024;

// On-demand lifted scalar validation; proof defaults belong to Analysis.
inline constexpr uint64_t kDefaultValidationNativeInstructions = 2048;
inline constexpr uint64_t kMaxValidationNativeInstructions = 16384;
inline constexpr uint64_t kMaxValidationWork = 1u << 30;

//===----------------------------------------------------------------------===//
// Function correspondence
//===----------------------------------------------------------------------===//

/// Fewest instructions a function needs before its mnemonic multiset (the
/// prime product) alone may pair it: shorter bodies share it too often.
inline constexpr uint32_t kMinPrimeProductInstructions = 8;
/// Fewest flow-graph edges before the flow-graph MD index alone may pair two
/// functions; a graph without edges has none.
inline constexpr uint32_t kMinMDIndexEdges = 2;
/// Fewest distinct scalar constants before their set alone may pair two
/// functions.
inline constexpr uint32_t kMinConstantSetSize = 2;
/// Fewest distinct imports a function calls before that set alone may pair
/// it.
inline constexpr uint32_t kMinImportCallSetSize = 2;
/// Fewest instructions before a function's normalized body alone may pair
/// it.  Below this a body such as `ret` or a jump to an import is shared by
/// many functions and only context tells them apart.
inline constexpr uint32_t kMinShapeInstructions = 3;
/// Largest group of functions sharing one feature that a drill-down splits by
/// the next features.  Larger groups stay unresolved for context to split.
inline constexpr size_t kMaxDrillDownGroup = 4096;
/// Best quick-scored candidates each unmatched function proposes for exact
/// measurement. Proposals from both directions are measured together.
inline constexpr unsigned kDefaultMaxCandidates = 16;
/// Ceiling a caller may raise kDefaultMaxCandidates to.
inline constexpr unsigned kMaxCandidates = 256;
/// Most unmatched pairs the approximate stage scores all of; beyond it,
/// only sketch buckets and neighbors propose candidates.
inline constexpr size_t kMaxExhaustiveCandidatePairs = 1u << 16;
/// Most functions one approximate-match bucket may hold before it is dropped
/// as uninformative (a shingle every function shares).
inline constexpr size_t kMaxSketchBucket = 64;
/// Least similarity an approximate pairing needs by default.
inline constexpr double kDefaultMinimumSimilarity = 0.55;
/// Least similarity a pairing from one remaining caller or callee needs:
/// lower than the global threshold because its context already agrees.
inline constexpr double kDefaultMinimumContextSimilarity = 0.35;
/// How far the best candidate must lead the second before an approximate
/// pairing is accepted; closer candidates and exact ties stay ambiguous.
inline constexpr double kDefaultAmbiguityMargin = 0.02;
/// Rounds of caller/callee propagation after the global passes.
inline constexpr unsigned kMaxPropagationRounds = 64;
/// Dense storage threshold for call-sequence anchor alignment. Larger ranges
/// retain exact optimal anchors using linear score frontiers.
inline constexpr uint64_t kMaxCallAlignmentCells = 1u << 20;

//===----------------------------------------------------------------------===//
// Structural deltas
//===----------------------------------------------------------------------===//

/// Fewest instructions a basic block needs before its byte-exact or
/// normalized body alone may pair it in the global block passes.
inline constexpr uint32_t kMinBlockHashInstructions = 2;
/// Most cells retained in a dense instruction-alignment table. Larger ranges
/// use exact weighted divide-and-conquer with linear score frontiers.
inline constexpr uint64_t kMaxAlignmentCells = 1u << 22;
/// Poll cancellation/deadlines at least this often in instruction/call scores.
inline constexpr size_t kAlignmentPollCells = 1024;

//===----------------------------------------------------------------------===//
// Native region hypotheses
//===----------------------------------------------------------------------===//

inline constexpr unsigned kRegionSeedInstructions = 3;
inline constexpr unsigned kDefaultRegionMinInstructions = 4;
inline constexpr unsigned kDefaultRegionMinRepeatedInstructions = 8;
/// Short repeated runs still qualify when they cover this share of either
/// participating function's decoded non-transfer instructions.
inline constexpr double kRegionMinShortRunFunctionCoverage = 0.5;
inline constexpr unsigned kMaxRegionMinInstructions = 256;
inline constexpr unsigned kDefaultRegionMaxOccurrences = 16;
inline constexpr unsigned kMaxRegionOccurrences = 64;
inline constexpr uint64_t kDefaultRegionMaxSeeds = 1u << 20;
inline constexpr uint64_t kMaxRegionSeeds = 8u << 20;
inline constexpr uint64_t kDefaultRegionMaxWork = 64u << 20;
inline constexpr uint64_t kMaxRegionWork = 1u << 30;
inline constexpr unsigned kDefaultRegionMaxAnchors = 100000;
inline constexpr unsigned kMaxRegionAnchors = 1000000;
inline constexpr unsigned kDefaultRegionMaxHypotheses = 20000;
inline constexpr unsigned kMaxRegionHypotheses = 100000;
inline constexpr unsigned kMaxRegionGroupAnchors = 4096;
inline constexpr unsigned kRegionExtensionInstructions = 4096;
inline constexpr unsigned kRegionPollWork = 1024;
inline constexpr unsigned kMaxRegionIdBytes = 128;
inline constexpr unsigned kRegionFunctionPreview = 16;
inline constexpr double kRegionUniqueSeedWeight = 0.5;
inline constexpr double kRegionIncompleteConfidenceFactor = 0.5;
inline constexpr double kRegionMaxConfidence = 0.95;

// SSA dependency snapshots, neighborhood candidates and slices.
inline constexpr char kMedSSAProfile[] = "med-ssa-v2";
inline constexpr char kLLVMSSAProfile[] = "llvm-ssa-v1";
inline constexpr char kLLVMParameterSSAProfile[] = "llvm-parameter-ssa-v1";
inline constexpr char kLLVMProgressSSAProfile[] = "llvm-progress-ssa-v1";
inline constexpr char kLLVMMultiBlockProgressSSAProfile[] =
    "llvm-progress-ssa-v2";
inline constexpr char kLLVMNestedProgressSSAProfile[] = "llvm-progress-ssa-v3";
inline constexpr char kLLVMBranchProgressSSAProfile[] = "llvm-progress-ssa-v4";
inline constexpr char kLLVMModularProgressSSAProfile[] = "llvm-progress-ssa-v5";
inline constexpr char kLLVMParameterBoundProgressSSAProfile[] =
    "llvm-progress-ssa-v6";
inline constexpr char kLLVMInvariantBoundProgressSSAProfile[] =
    "llvm-progress-ssa-v7";
inline constexpr uint32_t kDefaultFlowMaxNodes = 65536;
inline constexpr uint32_t kMaxFlowNodes = 1u << 20;
inline constexpr uint32_t kDefaultFlowMaxBlocks = 1024;
inline constexpr uint32_t kMaxFlowBlocks = 4096;
inline constexpr uint64_t kDefaultFlowMaxWork = 1u << 24;
inline constexpr uint64_t kMaxFlowWork = 1u << 30;
inline constexpr unsigned kDefaultFlowRounds = 4;
inline constexpr unsigned kMaxFlowRounds = 16;
inline constexpr unsigned kFlowPollWork = 1024;
inline constexpr unsigned kMaxFlowIdentityBytes = 4096;
inline constexpr unsigned kMaxFlowOpcodeBytes = 64;
inline constexpr unsigned kMaxFlowSemanticsBytes = 4096;
inline constexpr unsigned kDefaultFlowFunctions = 16;
inline constexpr unsigned kMaxFlowFunctions = 4096;
inline constexpr uint64_t kDefaultFlowTotalNodes = 1u << 18;
inline constexpr uint64_t kMaxFlowTotalNodes = 1u << 22;
inline constexpr unsigned kDefaultFlowNativeInstructions = 4096;
inline constexpr unsigned kMaxFlowNativeInstructions = 65536;
inline constexpr unsigned kMaxFlowReasonBytes = 4096;
// Verified finite SSA neighborhood regions; separate from native runs.
inline constexpr unsigned kDefaultFlowRegionMinNodes = 4;
inline constexpr unsigned kMaxFlowRegionMinNodes = 4096;
inline constexpr unsigned kDefaultFlowRegionMinOrigins = 2;
inline constexpr unsigned kDefaultFlowRegionOccurrences = 8;
inline constexpr unsigned kMaxFlowRegionOccurrences = 64;
inline constexpr uint64_t kDefaultFlowRegionSeeds = 1u << 18;
inline constexpr uint64_t kMaxFlowRegionSeeds = 1u << 22;
inline constexpr uint64_t kDefaultFlowRegionWork = 1u << 26;
inline constexpr uint64_t kMaxFlowRegionWork = uint64_t{1} << 32;
inline constexpr unsigned kDefaultFlowRegionAnchors = 20000;
inline constexpr unsigned kMaxFlowRegionAnchors = 100000;
inline constexpr uint64_t kDefaultFlowRegionPairs = 1u << 20;
inline constexpr uint64_t kMaxFlowRegionPairs = 1u << 24;
inline constexpr unsigned kMaxFlowRegionGroupAnchors = 4096;

// Observed interprocedural call reachability. These bounds include unknown
// transfer sites; they never authorize guessing an indirect target or effect.
inline constexpr unsigned kMaxImpactIndexedFunctions = 1u << 20;
inline constexpr unsigned kMaxImpactSeeds = 4096;
inline constexpr unsigned kDefaultImpactFunctions = 4096;
inline constexpr unsigned kMaxImpactFunctions = 65536;
inline constexpr unsigned kDefaultImpactDepth = 32;
inline constexpr unsigned kMaxImpactDepth = 4096;
inline constexpr unsigned kDefaultImpactSites = 1u << 18;
inline constexpr unsigned kMaxImpactSites = 1u << 22;
inline constexpr uint64_t kDefaultImpactWork = 1u << 24;
inline constexpr uint64_t kMaxImpactWork = 1u << 30;
inline constexpr unsigned kImpactPollWork = 1024;

//===----------------------------------------------------------------------===//
// Scores
//===----------------------------------------------------------------------===//

/// Weights of a matched pair's similarity: aligned instructions, matched
/// blocks and matched edges.  They sum to one.
inline constexpr double kSimilarityInstructionWeight = 0.50;
inline constexpr double kSimilarityBlockWeight = 0.25;
inline constexpr double kSimilarityEdgeWeight = 0.25;
/// Highest similarity a pair with any change may report, so 1.0 always means
/// identical normalized instructions and structure.
inline constexpr double kMaxChangedSimilarity = 0.999;
/// Share of a pair's confidence its callers' and callees' agreement decides.
inline constexpr double kContextConfidenceWeight = 0.25;

//===----------------------------------------------------------------------===//
// Extraction and transport
//===----------------------------------------------------------------------===//

inline constexpr uint64_t kDefaultMaxInstructions = 8u << 20;
inline constexpr uint64_t kMaxInstructions = 32u << 20;
inline constexpr uint64_t kDefaultMaxFunctions = 1u << 20;
inline constexpr uint64_t kDefaultMaxMemoryBytes = 2ull << 30;
inline constexpr uint64_t kMaxOptionsBytes = 1u << 20;
inline constexpr unsigned kProgressIntervalMilliseconds = 100;
inline constexpr unsigned kCancellationPollMilliseconds = 10;
inline constexpr unsigned kDefaultPageSize = 200;
inline constexpr unsigned kMaxPageSize = 4096;
inline constexpr unsigned kDefaultGraphNodeLimit = 20000;
inline constexpr unsigned kDefaultGraphEdgeLimit = 100000;
inline constexpr unsigned kGraphPreviewInstructions = 3;
inline constexpr uint64_t kMaxDetailInstructions = 1u << 20;
inline constexpr unsigned kDecodePageFunctions = 128;
inline constexpr unsigned kMaxDataReferenceBytes = 256;
inline constexpr unsigned kMinStringReferenceBytes = 4;
inline constexpr unsigned kMaxWarnings = 256;
inline constexpr uint64_t kMaxTimeoutMilliseconds = 24ull * 60 * 60 * 1000;

//===----------------------------------------------------------------------===//
// Change observations
//===----------------------------------------------------------------------===//

inline constexpr uint32_t kDefaultObservationRows = 65536;
inline constexpr uint32_t kMaxObservationRows = 1u << 20;
inline constexpr uint32_t kDefaultObservations = 4096;
inline constexpr uint32_t kMaxObservations = 65536;
inline constexpr uint64_t kDefaultObservationWork = 1u << 24;
inline constexpr uint64_t kMaxObservationWork = 1u << 30;
inline constexpr uint64_t kObservationPollInterval = 1024;
inline constexpr unsigned kObservationNativePriority = 10;
inline constexpr unsigned kObservationConstantPriority = 40;
inline constexpr unsigned kObservationWidthPriority = 60;
inline constexpr unsigned kObservationSignednessPriority = 80;
inline constexpr unsigned kObservationPredicatePriority = 90;
inline constexpr unsigned kObservationCallArgumentPriority = 70;
inline constexpr unsigned kObservationReturnPriority = 60;
inline constexpr unsigned kObservationMemoryWritePriority = 80;

//===----------------------------------------------------------------------===//
// Analyst review history
//===----------------------------------------------------------------------===//

inline constexpr uint32_t kMaxReviewRecords = 4096;
inline constexpr uint32_t kReviewDigestHexBytes = 64;
inline constexpr uint32_t kMaxReviewIdentityBytes = 4096;

inline constexpr uint64_t kMaxReviewTextBytes = 4u << 20;
inline constexpr uint32_t kMaxReviewCaseBytes = 256;
inline constexpr uint32_t kMaxReviewAuthorBytes = 256;
inline constexpr uint32_t kMaxReviewNoteBytes = 16384;
inline constexpr uint32_t kMaxReviewReferenceBytes = 2048;

// Import-call contract review. Native indexing uses ImpactOptions separately.
inline constexpr uint32_t kDefaultEffectRecords = 4096;
inline constexpr uint32_t kMaxEffectRecords = 65536;
inline constexpr uint64_t kDefaultEffectWork = 16ull * 1024 * 1024;
inline constexpr uint64_t kMaxEffectWork = 1024ull * 1024 * 1024;
inline constexpr uint64_t kEffectPollWork = 1024;

//===----------------------------------------------------------------------===//
// Persisted comparisons
//===----------------------------------------------------------------------===//

/// Caller-adjustable admission for offline version graphs. These bound source
/// bytes and graph records, not allocator overhead or the saved reader's work.
inline constexpr uint64_t kDefaultLineageSourceBytes = 256ull << 20;
inline constexpr uint32_t kDefaultLineageNodes = 1u << 20;
inline constexpr uint32_t kDefaultLineageEdges = 4u << 20;
inline constexpr uint64_t kDefaultLineageWork = 8 * kDefaultLineageSourceBytes;
/// Bounded projections fit the worker transport; source exports use files.
inline constexpr uint64_t kMaxLineagePageBytes = 8ull << 20;

/// Largest saved comparison read back, in bytes.
inline constexpr uint64_t kMaxSavedComparisonBytes = 1ull << 31;
/// Cooperative file/C-string admission and progress; not a report size limit.
inline constexpr size_t kSavedComparisonChunkBytes = 64 * 1024;
inline constexpr size_t kSavedComparisonProgressRecords = 256;
/// Most overrides one saved comparison may record.
inline constexpr size_t kMaxSavedOverrides = 1u << 20;

} // namespace neverd::limits::diff

#endif // NEVERD_DIFF_LIMITS_H
