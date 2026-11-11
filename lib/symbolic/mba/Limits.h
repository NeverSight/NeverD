//===- Limits.h - MBA refinement bounds -----------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal bounds for MBA refinement, polynomial recovery and result reuse.
/// These refinements share the caller's existing work and storage budgets.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_MBA_LIMITS_H
#define NEVERD_SYMBOLIC_MBA_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::symbolic::detail::limits {

// Preserve the original deterministic assignment stream in native and AP
// cross-checks, including the shared immutable native prefix.
inline constexpr uint64_t kVerificationSampleSeed = 0x9E3779B97F4A7C15ull;
// A fixed 64 KiB prefix covers the measured 14–20-input native checks at
// both 64 and 256 samples. Larger requests keep the original seeded engine;
// this changes neither the sample count nor the assignment sequence.
inline constexpr std::size_t kVerificationCachedWords = 8192;

/// Residual recovery reuses the small exact Boolean recipes without building
/// the four-input exhaustive table.
inline constexpr unsigned kMaxResidualSynthesisAtoms = 3;

inline constexpr std::size_t kMaxLinearTailTerms = 128;
inline constexpr std::size_t kMaxLinearTailNodes = 32;
inline constexpr unsigned kMinLinearTailSummands = 2;
inline constexpr unsigned kMinLinearTailProducts = 1;
inline constexpr unsigned kMinLinearTailProductDegree = 8;
inline constexpr unsigned kMaxLinearTailInputs = 3;

// Shorter products retain their original spelling for local identities.
inline constexpr std::size_t kMinCompletedProductWalkDegree = 16;
inline constexpr std::size_t kMaxCompletedProductTerms = 128;

inline constexpr std::size_t kMinDelayedCommonAtoms = 4;
inline constexpr unsigned kMinDelayedPolynomialOperands = 2;

inline constexpr std::size_t kMaxPolynomialCofactorTerms = 128;
inline constexpr std::size_t kMaxPolynomialCofactorEdges = 4096;
inline constexpr unsigned kMaxCofactorSearchDegree = 3;
inline constexpr unsigned kMaxCubicCofactorSearchAtoms = 3;
inline constexpr unsigned kMinQuadraticRowAtoms = 4;
inline constexpr unsigned kMaxQuadraticRowAtoms = 8;
inline constexpr std::size_t kMaxQuadraticRowTerms = 8;
inline constexpr unsigned kMBAAbstractionInlineEntries = 16;
// Dense tables need spare buckets before their load factor triggers growth.
inline constexpr unsigned kMBAAbstractionInlineBuckets =
    2 * kMBAAbstractionInlineEntries;
inline constexpr std::size_t kMinCompletedRegionCacheWork = 64;
inline constexpr std::size_t kCompletedMemoEntryBytes = 256;
inline constexpr std::size_t kCompletedMemoStorageDivisor = 8;
inline constexpr std::size_t kMinArithmeticMemoTerms = 3;
inline constexpr std::size_t kMinArithmeticPartitionTerms = 3;
inline constexpr std::size_t kMinArithmeticCommonFactors = 3;
inline constexpr std::size_t kMinArithmeticCommonProductOperands = 4;
inline constexpr std::size_t kMinFlatArithmeticSumTerms = 8;
inline constexpr std::size_t kMinUnitProductOperands = 3;
inline constexpr std::size_t kMaxUnitProductCoordinates = 64;
inline constexpr std::size_t kMaxBooleanBoundaryNodes = 128;
inline constexpr unsigned kMinCanonicalAffineAtoms = 4;
inline constexpr unsigned kMinProductCofactorDegree = 4;
inline constexpr unsigned kMaxPartitionCubeAxes = 3;
inline constexpr unsigned kMinCompressedCommonFactors = 3;
inline constexpr std::size_t kMinParityMemoCapacity = 8;
/// Small parity frontiers need no per-node allocation; keep spare hash buckets.
inline constexpr unsigned kProductParityInlineEntries = 16;
inline constexpr unsigned kProductParityInlineBuckets =
    2 * kProductParityInlineEntries;
inline constexpr unsigned kMinQuadraticCongruenceAtoms = 4;
inline constexpr unsigned kMinPackedPolynomialAtoms = 4;
inline constexpr unsigned kMinPackedPolynomialWidth = 8;

/// Bound exact reciprocal-product proofs within the caller's work budget.
inline constexpr unsigned kMaxReciprocalProductWidth = 128;

inline constexpr std::size_t kMinResidueProductFactors = 4;
inline constexpr unsigned kMinResidueShiftedFactors = 2;
inline constexpr std::size_t kMaxResidueProductFactors = 512;
inline constexpr std::size_t kMaxResidueBaseTerms = 16;
inline constexpr std::size_t kMaxResidueProductSummands = 128;
inline constexpr unsigned kMaxAffineResidueDepth = 8;
inline constexpr unsigned kMinAffineResidueScaledFactors = 1;
inline constexpr std::size_t kMaxAffineResidueEdges = 64;
inline constexpr unsigned kMaxAffineResidueTableBits = 8;
// Equal halves eliminate quadratic high terms at byte precision. Six-bit
// products retain two-bit prefixes, avoiding twice as many prefix groups.
inline constexpr unsigned kResidueEqualHalfBits = 8;
// An isolated top bit can be accumulated separately while the low product
// uses the three-bit odd-unit reading.
inline constexpr unsigned kMaxIsolatedProductLowBits = 3;
// Squaring a recursive mixed product of six or more observed bits can grow
// its decision graph. Preserve its complete odd factors across that square;
// smaller recursive products keep their inexpensive complete decision words.
inline constexpr unsigned kMinResidueRetainedSquareBits = 7;

/// Stable low-word results share small pages instead of per-word hash nodes.
inline constexpr std::size_t kResidueWordPageEntries = 16;
/// Pairing signatures keep stable inline keys in small, prepaid pages.
inline constexpr std::size_t kResidueSignaturePageEntries = 16;
/// Keep optional multiple-residual recovery from starving the surrounding DAG.
inline constexpr std::size_t kMaxMultipleResidueWork = 49152;
/// Direct decision proofs still spend the caller's existing cumulative budget.
inline constexpr std::size_t kMaxResidueDecisionWork = 49152;
inline constexpr std::size_t kMaxResidueDecisionNodes = 4096;
inline constexpr unsigned kMaxResidueDecisionDepth = 64;
inline constexpr unsigned kMaxResidueDecisionInputs = 3;
inline constexpr unsigned kResidueDecisionPriorityBits = 4;
inline constexpr unsigned kMinResidueDecisionPairingBits = 6;
inline constexpr unsigned kMinResidueDecisionWidePairingBits = 6;
// Admission probe for partial blocks; every matched pair still needs a proof.
inline constexpr std::size_t kResidueDecisionPartialPairingDivisor = 2;
inline constexpr unsigned kResidueDecisionPolynomialFirstBits = 6;
inline constexpr std::size_t kResidueDecisionPolynomialFirstNodes = 512;
// LLVM dense maps start with 64 buckets. Residue caches prepay each complete
// bucket allocation before insertion, including this initial table.
inline constexpr std::size_t kResidueMinDenseMapBuckets = 64;
// Eight-byte Boolean buckets dominate rehashing in deep carry proofs. Skip
// one growth generation, funding the full table from the same storage budget.
inline constexpr std::size_t kResiduePackedMapGrowth = 4;
inline constexpr unsigned kMaxResidueDecisionSynthesisInputs = 6;
inline constexpr unsigned kMaxUnconditionedResidueDecisionBits = 3;
inline constexpr std::size_t kMaxResidueDecisionPolynomialTerms = 256;
inline constexpr unsigned kMaxResiduePrefixBits = 16;
inline constexpr unsigned kMaxResiduePrefixHintDepth = 2;
inline constexpr unsigned kMaxResiduePrefixHintNodes = 16;
inline constexpr std::size_t kMaxResidueWitnessWork = 1024;

// lib/symbolic/mba/SymBitwise.cpp
/// The most inputs the exhaustive search will ever tabulate.
///
/// Four inputs is 65536 functions and a relaxation over every pair of them;
/// five would be four billion recipes, which is not a longer wait but a failed
/// allocation.  Clamping a larger request rather than refusing it keeps a
/// caller that asked for more from losing the optimality it could have had.
inline constexpr unsigned kSymBitwiseMaxTabulatedAtoms = 4;
// Packing care/value pairs for six inputs needs a 4096-bit membership table.
// Larger prime-cover rounds keep the sparse hash-based representation.
inline constexpr unsigned kSymBitwiseDirectImplicantAtoms = 6;

// lib/symbolic/mba/SymMBA.cpp
// This runs only on a completed answer. A local builder rule would change
// earlier search choices, while a second region search would repeat them.
inline constexpr size_t kSymMBADetailCompleteComplementarySumsMaxNodes = 64;
inline constexpr size_t kSymMBAMaxEdges = 128;

// lib/symbolic/mba/SymMBAArithmetic.cpp
inline constexpr size_t kSymMBAArithmeticMaxTerms = 128;
inline constexpr size_t kSymMBAArithmeticMaxEdges = 4096;

// lib/symbolic/mba/SymMBAFactor.cpp
inline constexpr size_t kProductCofactorMaxTerms = 128;
inline constexpr size_t kProductCofactorMaxEdges = 2048;
inline constexpr size_t kProductCofactorMinCompletionTerms = 6;
inline constexpr size_t kStructuralFactorMaxTerms = 64;
inline constexpr size_t kStructuralFactorMaxEdges = 256;

// lib/symbolic/mba/SymMBA.cpp
inline constexpr int kSymMBAProjectBitWindowDepthLimit = 32;
inline constexpr int kSymMBAFoldConditionalControlsSeenSizeLimit = 64;
inline constexpr int kSymMBATryFastComplementarySumOptsMaxTableBytesLimit =
    4096;
inline constexpr int
    kSymMBADetailCompleteComplementarySumsOptsMaxTableBytesLimit = 4096;

// lib/symbolic/mba/SymMBAAbstract.cpp
inline constexpr int kSymMBAAbstractToMBAWidenBitwiseDepthLimit = 32;

// lib/symbolic/mba/SymMBAArithmetic.cpp
inline constexpr int
    kSymMBAArithmeticSolveNestedProductCofactorsFrontierSizeVisitsLimit = 2048;
inline constexpr int
    kSymMBAArithmeticSolveNestedProductCofactorsFrontierSizeDepthLimit = 32;
inline constexpr int
    kSymMBAArithmeticSolveNestedProductCofactorsOriginalSizeLimit = 128;
inline constexpr int
    kSymMBAArithmeticSolveNestedProductCofactorsDistributedSizeLimit = 128;

// lib/symbolic/mba/SymMBAMaskedSum.cpp
inline constexpr int kSymMBAMaskedSumSolveWeightedMasksOptsMaxTableBytesLimit =
    4096;

// lib/symbolic/mba/SymMBAPoly.cpp
inline constexpr int kSymMBAPolyExpandOverMintermsCheckExtremesTermsSizeLimit =
    128;
inline constexpr int
    kSymMBAPolyExpandOverMintermsCheckExtremesTermFactorsSizeLimit = 64;

// lib/symbolic/mba/SymMBARegion.cpp
inline constexpr int kSymMBARegionFoldZeroProductNodesSizeLimit = 128;
inline constexpr unsigned kSymMBARegionSolveExpandedProductsRemaining = 128;
inline constexpr int kSymMBARegionSolveExpandedProductsInspectDepthLimit = 2;

// lib/symbolic/mba/SymMBA.cpp
inline constexpr int kMaxProjectedBitWindowOperands = 32;
inline constexpr int kSymMBATryFastComplementarySumCtxNumOperandsALimit = 8;

// lib/symbolic/mba/SymMBAArithmetic.cpp
inline constexpr int kSymMBAArithmeticNormalizeCtxNumOperandsRLimit = 2;
inline constexpr int
    kSymMBAArithmeticReduceDistributionProofCtxNumOperandsBeforeLimit = 6;
inline constexpr int
    kSymMBAArithmeticSolveNestedProductCofactorsCtxNumOperandsViewLimit = 16;

// lib/symbolic/mba/SymMBARegion.cpp
inline constexpr int kSymMBARegionSolveRegionCtxNumOperandsELimit = 128;
inline constexpr int kSymMBARegionFoldZeroProductCtxNumOperandsRootLimit = 65;
inline constexpr int kMaxExpandedProductSumOperands = 64;
inline constexpr int kSymMBARegionFoldProductParityCtxNumOperandsRootLimit = 32;

// lib/symbolic/mba/SymWideArithmetic.cpp
inline constexpr int kSymWideArithmeticRecoverSplitWordArithmeticSampleLimit =
    8;

// lib/symbolic/mba/SymMBAMaskedSum.cpp
inline constexpr int kMaxMaskedSumViewDepth = 64;

// lib/symbolic/mba/SymMBARegion.cpp
inline constexpr int kMaxProductParityEdges = 128;

// lib/symbolic/mba/SymMBAArithmetic.cpp
inline constexpr int kMaxNestedCofactorPowerEdges = 2048;

// lib/symbolic/mba/SymMBARegion.cpp
inline constexpr int kMaxProductParityInputs = 128;

// lib/symbolic/mba/SymMBAArithmetic.cpp
inline constexpr int kMaxNestedProductFrontierTerms = 128;

// lib/symbolic/mba/SymMBA.cpp
inline constexpr int kMaxConditionalControlEdges = 128;

} // namespace neverd::symbolic::detail::limits

#endif // NEVERD_SYMBOLIC_MBA_LIMITS_H
