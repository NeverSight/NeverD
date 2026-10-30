//===- Limits.h - Component limits and defaults -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource ceilings, work budgets and defaults owned by this component.
/// Binary layouts and instruction/ABI constants remain with their models.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_LIMITS_H
#define NEVERD_ANALYSIS_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::analysis {

// include/neverd/analysis/BytecodeDecoder.h
inline constexpr size_t BytecodeInstructionByteLimit = 4096;

} // namespace neverd::analysis

namespace neverd::limits::analysis {

// Read-only scalar model construction and equivalence queries.
inline constexpr uint64_t kLLVMScalarStopPollWork = 1024;
inline constexpr uint64_t kLLVMScalarEquivalenceDefaultWork = 16777216;

// include/neverd/analysis/BytecodeSource.h
inline constexpr uint64_t kBytecodeSourceMaxOperations = 10000000;

// include/neverd/analysis/LowIRRefinement.h
inline constexpr uint64_t kLowIRRefinementMaxMetadata = 65536;

// include/neverd/analysis/arch/x86_64/InterpreterMachineState.h
inline constexpr uint64_t kInterpreterMachineStateMaxOperations = 65536;

// include/neverd/analysis/arch/x86_64/InterpreterMachineRegionSource.h
inline constexpr uint64_t kInterpreterMachineRegionChoiceSourceMaxOperations =
    16777216;

// include/neverd/analysis/arch/x86_64/X64ContinuationSource.h
inline constexpr uint64_t kX64ContinuationMaxInstructions = 65536;
inline constexpr uint64_t kX64ContinuationMaxOperations = 16777216;

// lib/analysis/llvm/LLVMScalarLoopBounds.cpp
inline constexpr unsigned kLLVMScalarLoopBoundsMaxValues = 32;
inline constexpr unsigned kLLVMScalarLoopBoundsMaxOperands = 64;
inline constexpr unsigned kLLVMScalarLoopBoundsMaxBounds = 4;

// lib/analysis/llvm/LLVMScalarLoopRecoveryInternal.h
inline constexpr unsigned kLLVMScalarLoopRecoveryInternalMaxReplacementBatch =
    32;

// lib/analysis/llvm/LLVMScalarLoopUnpeeling.cpp
inline constexpr unsigned kLLVMScalarLoopUnpeelingMaxSlots = 4;
inline constexpr unsigned kLLVMScalarLoopUnpeelingMaxValues = 12;
inline constexpr unsigned kLLVMScalarLoopUnpeelingMaxDepth = 6;
inline constexpr unsigned kLLVMScalarLoopUnpeelingMaxVisits = 128;
inline constexpr unsigned kLLVMScalarLoopUnpeelingMaxCuts = 8;

// lib/analysis/arch/x86_64/X64Recovery.cpp
inline constexpr int
    kX64RecoveryValidateRawEnvironmentDeclarationsTextTextSSizeLimit = 4096;

// lib/analysis/bytecode/BytecodeDecoder.cpp
inline constexpr int kMaxBytecodeLookupValues = 65536;
inline constexpr int kBytecodeDecoderValidateLayoutPTemporaryBytesLimit = 65536;
inline constexpr int kMaxBytecodeEncodingOperations = 4096;
inline constexpr int kMaxBytecodeEncodings = 65536;
inline constexpr int kBytecodeDecoderCreateOperationCountLimit = 1000000;

// lib/analysis/bytecode/BytecodeProfile.cpp
inline constexpr int kBytecodeProfileEncodingMatchesSizeLimit = 4096;
inline constexpr int kMaxBytecodeProfileJSONBytes = 64 * 1024 * 1024;

// lib/analysis/core/ConditionalImplication.cpp
inline constexpr int kMaxConditionalImplicationWork = 4194304;
inline constexpr unsigned kMaxConditionalImplicationReachableNodes = 262144U;
inline constexpr int kMaxConditionalImplicationEncodingCost = 4096;
inline constexpr unsigned kConditionalImplicationProveWithEncodingMinArgument2 =
    16384U;

// lib/analysis/core/DomainCoverage.cpp
inline constexpr int kMaxCoverageConjunctionTerms = 64;
inline constexpr int kMaxCoverageResidualGuards = 64;
inline constexpr int kDomainCoverageProvePartitionedCoverageVisitDepthLimit = 8;

// lib/analysis/core/FrameOffsets.cpp
inline constexpr unsigned kFrameOffsetsProveFrameOffsetImplRemaining = 16;

// lib/analysis/core/LowIRUndefinedIndependence.cpp
inline constexpr int
    kLowIRUndefinedIndependenceConditionalLimitsBoundMaxWidthDefault = 256;
inline constexpr int
    kLowIRUndefinedIndependenceBInstructionBoundariesSizeLimit = 32;

// lib/analysis/llvm/LLVMScalarLoopRegions.cpp
inline constexpr unsigned kLLVMScalarLoopRegionsRotateGuardLimitSelected = 6U;
inline constexpr unsigned kLLVMScalarLoopRegionsRotateGuardLimitDefault = 3U;

// lib/analysis/llvm/LLVMScalarSourceRecovery.cpp
inline constexpr int
    kLLVMScalarSourceRecoveryCleanCandidateOptionsMaxFiniteValueWorkMinArgument1 =
        65536;
inline constexpr int
    kLLVMScalarSourceRecoveryCleanCandidateOptionsMaxPredicateWorkMinArgument1 =
        65536;

// lib/analysis/core/DomainCoverage.cpp
inline constexpr int kMaxCoverageArms = 8;

// lib/analysis/core/OrderedQuery.h
inline constexpr int
    kOrderedQueryIsOriginalQueryConjunctCtxNumOperandsPredicateLimit = 64;

// lib/analysis/llvm/LLVMScalarLoopRegions.cpp
inline constexpr int kLLVMScalarLoopRegionsZeroTripNLimit = 8;

// lib/analysis/core/ConditionalImplication.cpp
inline constexpr int kMaxConditionalClosurePasses = 16;

// lib/analysis/bytecode/BytecodeSource.cpp
inline constexpr int kMaxBytecodeRegisterBankBytes = 65536;

} // namespace neverd::limits::analysis

#endif // NEVERD_ANALYSIS_LIMITS_H
