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

#ifndef NEVERD_PIPELINE_LIMITS_H
#define NEVERD_PIPELINE_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd {

// include/neverd/pipeline/BytecodeRecovery.h
inline constexpr uint64_t BytecodeRecoveryInputLimit = 64 * 1024 * 1024;

} // namespace neverd

namespace neverd::limits::pipeline {

// lib/pipeline/BytecodeSourceEvidence.cpp
inline constexpr size_t kBytecodeSourceEvidenceBindBytecodeSourceEvidenceWork =
    100000;

// lib/pipeline/NativeSourceHints.cpp
inline constexpr size_t kNativeSourceHintsNativeOutputFrameEffectsBudget =
    262144;
inline constexpr size_t
    kNativeSourceHintsValidateSwiftValueConstructorBindingsBudget = 262144;
inline constexpr size_t
    kNativeSourceHintsValidateSwiftOpaqueValueBindingsBudget = 262144;
inline constexpr size_t
    kNativeSourceHintsValidateSwiftConsumedInputBindingsBudget = 262144;
inline constexpr size_t
    kNativeSourceHintsValidateNativeSwiftReceiverBindingsBudget = 100000;
inline constexpr size_t
    kNativeSourceHintsValidateObjCByValueCopyBindingsBudget = 100000;
inline constexpr size_t
    kNativeSourceHintsValidateSwiftWitnessFrameBindingsBudget = 262144;
inline constexpr size_t kNativeSourceHintsCurrentNativeSwiftErrorCalleeBudget =
    262144;
inline constexpr size_t
    kNativeSourceHintsValidateNativeSwiftErrorBindingsBudget = 262144;
inline constexpr size_t
    kNativeSourceHintsObservedNativeFourDoubleReturnsBudget = 262144;
inline constexpr size_t kNativeSourceHintsObservedNativeFourWordReturnsBudget =
    262144;
inline constexpr size_t kNativeSourceHintsRefineNativeFourWordReturnHintBudget =
    262144;
inline constexpr size_t kNativeSourceHintsByteBudget = 100000;

// lib/pipeline/NativeSourceIntegerPrefixReturn.h
inline constexpr size_t
    kNativeSourceIntegerPrefixReturnHasNativeSourceIntegerPrefixReturnBudget =
        100000;

// lib/pipeline/PipelineCallAbiDetail.h
inline constexpr size_t kPipelineCallAbiDetailMaxProbes = 4;
inline constexpr size_t kPipelineCallAbiDetailMaxConversions = 4;
inline constexpr size_t kPipelineCallAbiDetailMaxBytes = size_t{512} << 20;

// lib/pipeline/PipelineFormattedCalls.cpp
/// The distinct constants a register may hold where paths join, as the scan
/// follows them into a block, and the rounds it joins a function's blocks
/// for before it gives up: a cross-jumped call's format comes from a few
/// paths.
inline constexpr size_t kPipelineFormattedCallsMaxJoinedFormatConstants = 4;
inline constexpr size_t kPipelineFormattedCallsMaxFormatJoinRounds = 8;
inline constexpr uint32_t kPipelineFormattedCallsMaxUnits = 65536;

// lib/pipeline/PipelineHighIR.cpp
inline constexpr size_t kPipelineHighIRSourceWorkBudget = 250000;

// lib/pipeline/PipelineIncomingStack.cpp
inline constexpr size_t kIncomingStackLeafOperations = 256;
inline constexpr size_t kIncomingStackLeafCallees = 32;

// lib/pipeline/PipelineLibraryRecognition.cpp
inline constexpr size_t kPipelineLibraryRecognitionConflictWork = 250000;
inline constexpr size_t
    kPipelineLibraryRecognitionPipelineRecognizeLibrariesWork = 250000;

// lib/pipeline/PipelineLowIR.cpp
// Keep finite exact alternatives for slot-precise LOAD/STORE decisions.
// Larger joins widen to the durable owner-summary lattice above.
inline constexpr size_t kPipelineLowIRMaxExactAddressAlternatives = 16;
// Frame offsets widen the same way; without a bound a frame-walking loop
// adds one offset per fixpoint round.
inline constexpr size_t kPipelineLowIRMaxFrameOffsetAlternatives = 64;
inline constexpr size_t kPipelineLowIRMaxOutgoingStackCells = 64;
inline constexpr size_t kPipelineLowIRMaxBytes = size_t(256) << 20;
// Table/stack proofs can dominate bodies only a few KiB long, so medium
// batches can also use independent decoders; tiny batches stay serial.
inline constexpr uint64_t kPipelineLowIRMinParallelBytes = 4096;
// Decode cost tracks a function's instruction count, which is unknown before
// the recursive-descent build runs.  Candidates are address-sorted, so the
// byte gap to the next entry is a cheap upper-bound proxy for a function's
// size; scheduling the largest gaps first keeps one giant function from
// being claimed last and serializing the tail.  The gap is clamped so an
// outsized cross-segment gap (data between the last function and the segment
// end) does not distort the ordering — it is only a scheduling hint and never
// affects the decoded result.
inline constexpr uint64_t kPipelineLowIRMaxDecodeWeight = 1ull << 20;

// lib/pipeline/PipelineReturnContracts.cpp
// Each function's answer reads its callees' and its callers': repeat
// until no answer changes, within a bound for call-graph cycles.
inline constexpr unsigned kPipelineReturnContractsMaxRounds = 8;
inline constexpr unsigned kPipelineReturnContractsClassifyMaxDepth = 32;

// lib/pipeline/StateSourceBank.cpp
inline constexpr unsigned kStateSourceBankBankOffsetsBudget = 128;

// lib/pipeline/StateSourceRecovery.cpp
inline constexpr unsigned kStateSourceRecoveryResultWriteIsDeadBudget = 4096;
inline constexpr unsigned kStateSourceRecoveryPointerCandidateLoadBudget = 4096;
inline constexpr unsigned kStateSourceRecoveryPointerCandidateMatchesBudget =
    4096;
inline constexpr unsigned kStateSourceRecoveryFinishStateSourceRecoveryBudget =
    4096;

// lib/pipeline/BytecodeRecovery.cpp
inline constexpr int kBytecodeRecoveryIdentifierNameSizeLimit = 128;
inline constexpr int kBytecodeRecoveryRecoverBytecodeRangesSizeLimit = 100000;
inline constexpr uint64_t kBytecodeRecoveryRemainingOperations = 10000000;
inline constexpr uint32_t kBytecodeRecoveryRemainingInstructions = 1000000;
inline constexpr int kBytecodeRecoveryRecoverBytecodeBindingsSizeLimit = 65536;

// lib/pipeline/BytecodeSourceEvidence.cpp
inline constexpr int
    kBytecodeSourceEvidenceBindBytecodeSourceEvidenceNameSizeLimit = 512;
inline constexpr int
    kBytecodeSourceEvidenceBindBytecodeSourceEvidenceSymbolSizeLimit = 512;
inline constexpr int
    kBytecodeSourceEvidenceBindBytecodeSourceEvidenceModuleSizeLimit = 512;
inline constexpr int
    kBytecodeSourceEvidenceBindBytecodeSourceEvidenceSSizeLimit = 512;
inline constexpr int
    kBytecodeSourceEvidenceBindBytecodeSourceEvidenceArgsSizeLimit = 64;
inline constexpr int
    kBytecodeSourceEvidenceBindBytecodeSourceEvidenceASizeLimit = 1024;
inline constexpr int
    kBytecodeSourceEvidenceBindBytecodeSourceEvidenceTextSizeLimit = 4096;

// lib/pipeline/BytecodeStateSourceContract.cpp
inline constexpr int
    kBytecodeStateSourceContractReadBytecodeStateSourceContractGatewaysSizeLimit =
        256;
inline constexpr int
    kBytecodeStateSourceContractReadBytecodeStateSourceContractHexSizeLimit =
        131072;
inline constexpr int
    kBytecodeStateSourceContractReadBytecodeStateSourceContractOGetArrayWritesSizeLimit =
        128;
inline constexpr int
    kBytecodeStateSourceContractReadBytecodeStateSourceContractOGetArrayImportsSizeLimit =
        4096;
inline constexpr int
    kBytecodeStateSourceContractReadBytecodeStateSourceContractExclusionsSizeLimit =
        64;

// lib/pipeline/NativeSourceFloatingReturn.h
inline constexpr size_t kNativeSourceFloatingReturnRemaining = 262144;
inline constexpr int kNativeSourceFloatingReturnGraphFunctionBlocksSizeLimit =
    4096;
inline constexpr int kNativeSourceFloatingReturnGraphBOpsSizeLimit = 65536;
inline constexpr int kNativeSourceFloatingReturnNeedNodesSizeLimit = 4096;

// lib/pipeline/NativeSourceHints.cpp
inline constexpr int kNativeSourceHintsNativeEntryRegistersLowBlocksSizeLimit =
    16384;
inline constexpr size_t kNativeSourceHintsNativeEntryRegistersRemaining =
    262144;
inline constexpr int
    kNativeSourceHintsHasNativeSourceStateContractLowBlocksSizeLimit = 16384;
inline constexpr size_t
    kNativeSourceHintsHasNativeSourceStateContractRemaining = 262144;
inline constexpr int kNativeSourceHintsDefinedReturnPathsCountLimit = 16384;
inline constexpr size_t kNativeSourceHintsDefinedReturnPathsRemaining = 262144;
inline constexpr size_t kNativeSourceHintsNativeRecordReturnRemaining = 262144;
inline constexpr size_t kNativeSourceHintsCompleteIntegerLeafReturnRemaining =
    262144;
inline constexpr size_t
    kNativeSourceHintsCompilerRTPlatformVersionContractRemaining = 262144;
inline constexpr int
    kNativeSourceHintsObservedNativePairReturnsFunctionBlocksSizeLimit = 16384;
inline constexpr size_t kNativeSourceHintsObservedNativePairReturnsRemaining =
    262144;
inline constexpr int
    kNativeSourceHintsObservedNativeFourDoubleReturnsFunctionBlocksSizeLimit =
        16384;
inline constexpr int
    kNativeSourceHintsObservedNativeFourWordReturnsFunctionBlocksSizeLimit =
        16384;
inline constexpr int
    kNativeSourceHintsInferNativeSourceTypeHintMedParamsSizeLimit = 64;
inline constexpr int
    kNativeSourceHintsRefineNativeSourceTypeHintOriginalParametersSizeLimit =
        64;
inline constexpr int kNativeSourceHintsRefineNativeSourceTypeHintDepthLimit =
    128;
inline constexpr size_t kNativeSourceHintsRefineNativeSourceTypeHintRemaining =
    65536;
inline constexpr int
    kNativeSourceHintsRefineNativeSourceTypeHintScanScanDepthLimit = 128;

// lib/pipeline/NativeSourceIntegerPrefixReturn.h
inline constexpr int
    kNativeSourceIntegerPrefixReturnHasNativeSourceIntegerPrefixReturnPendingSizeLimit =
        100000;
inline constexpr int
    kNativeSourceIntegerPrefixReturnHasNativeSourceIntegerPrefixReturnDefinedDepthLimit =
        128;

// lib/pipeline/Pipeline.cpp
inline constexpr int kPipelineRecordLineSizeLimit = 192;
inline constexpr int kPipelineNativePipelineTraceCountLimit = 32;

// lib/pipeline/PipelineHighIR.cpp
inline constexpr int
    kPipelineHighIRPipelineRebuildHighIRAppendSourcesFISizeLimit = 100000;

// lib/pipeline/PipelineLibraryRecognition.cpp
inline constexpr int
    kPipelineLibraryRecognitionReturnedEntryRegisterCountLimit = 4096;
inline constexpr int
    kPipelineLibraryRecognitionReturnedEntryRegisterDepthLimit = 64;
inline constexpr int
    kPipelineLibraryRecognitionPipelineRecognizeLibrariesCallsSizeLimit = 128;

// lib/pipeline/PipelineLowIR.cpp
inline constexpr int kPipelineLowIRComputeCallRegisterEffectsBatchSizeSelected =
    32;
inline constexpr int kPipelineLowIRComputeCallRegisterEffectsBatchSizeDefault =
    8;

// lib/pipeline/PipelineOptimize.cpp
inline constexpr int kPipelineOptimizePrivateFrameDisplacementWorkLimit = 1024;
inline constexpr int kPipelineOptimizeReturnDependsOnProvenLoadSeenSizeLimit =
    512;
inline constexpr int kPipelineOptimizeReturnDependsOnProvenLoadWorkSizeLimit =
    512;

// lib/pipeline/PipelineSEHRuntimeMemory.cpp
inline constexpr int
    kPipelineSEHRuntimeMemoryCollectLowSEHRuntimeMemoryWritableImageRangesSizeLimit =
        256;

// lib/pipeline/PipelineStructReturn.cpp
inline constexpr int kPipelineStructReturnsWholeVectorIsWideVecDepthLimit = 8;

// lib/pipeline/PipelineWideIntReturn.cpp
inline constexpr int kPipelineWideIntReturnResolveCallTargetVADepthLimit = 8;

// lib/pipeline/StateSourceBank.cpp
inline constexpr int kStateSourceBankBankOffsetsResultBankSizeLimit = 32;
inline constexpr int kStateSourceBankEligibleFSizeLimit = 1024;
inline constexpr int kStateSourceBankEligibleFGetInstructionCountLimit = 16384;
inline constexpr int kStateSourceBankForwardTransferWorkLimit = 1048576;
inline constexpr int kStateSourceBankForwardTransferValuesSizeLimit = 512;
inline constexpr int kStateSourceBankRefineBankAccessesBytesLimit = 4096;
inline constexpr int kStateSourceBankRefineBankAccessesMSizeLimit = 4096;
inline constexpr int kStateSourceBankRefineBankAccessesWorkLimit = 65536;

// lib/pipeline/StateSourceRecovery.cpp
inline constexpr int kStateSourceRecoveryIdentifierNameSizeLimit = 128;
inline constexpr int kStateSourceRecoveryNormalReturnConstantDepthLimit = 64;
inline constexpr int kStateSourceRecoveryResultWriteIsDeadCheckDepthLimit = 128;
inline constexpr int kStateSourceRecoveryAdvisoryAddressDepthLimit = 64;
inline constexpr int kStateSourceRecoverySelectorCandidateDepthLimit = 64;
inline constexpr int kStateSourceRecoveryExposeBankLoadSlicesSizeLimit = 4096;
inline constexpr int kStateSourceRecoveryLocalizeBankBytesLimit = 4096;
inline constexpr int kStateSourceRecoveryLocalizeBankFGetInstructionCountLimit =
    16384;
inline constexpr int kStateSourceRecoveryLocalizeBankCallsSizeLimit = 256;
inline constexpr int
    kStateSourceRecoveryLocalizeBankUint64TBytes2CallsSizeReturnsSize1Limit =
        262144;
inline constexpr int kStateSourceRecoveryLocalizeBankWorkLimit = 65536;
inline constexpr int
    kStateSourceRecoveryPrepareStateSourceRecoveryCStateBytesLimit = 65536;
inline constexpr int
    kStateSourceRecoveryPrepareStateSourceRecoveryCGatewaysSizeLimit = 256;
inline constexpr int
    kStateSourceRecoveryPrepareStateSourceRecoveryImageBytesSizeLimit = 65536;
inline constexpr int
    kStateSourceRecoveryPrepareStateSourceRecoveryGImportsSizeLimit = 4096;
inline constexpr int
    kStateSourceRecoveryPrepareStateSourceRecoveryGExcludedArgumentsSizeLimit =
        64;
inline constexpr int
    kStateSourceRecoveryPrepareStateSourceRecoveryGWritesSizeLimit = 128;
inline constexpr int
    kStateSourceRecoveryFinishStateSourceRecoveryCandidateEncodedSizeLimit =
        512;

// lib/pipeline/evidence/ConstantStores.cpp
inline constexpr int
    kConstantStoresProjectNativeConstantStoresImageSymbolsSizeLimit = 65536;
inline constexpr int
    kConstantStoresProjectNativeConstantStoresCResultInstructionsLimit = 32;
inline constexpr int kConstantStoresProjectNativeConstantStoresOpsSizeLimit =
    32;

// lib/pipeline/StateSourceRecovery.cpp
inline constexpr int kStateSourceRecoveryFinishStateSourceRecoveryRoundLimit =
    3;

// lib/pipeline/evidence/ConstantStores.cpp
inline constexpr int kConstantStoresProjectNativeConstantStoresPCStartLimit =
    256;

// lib/pipeline/evidence/ConstantStores.cpp
inline constexpr int kMaxNativeConstantStoreSteps = 1000000;

// lib/pipeline/evidence/ConstantStores.cpp
inline constexpr int kMaxNativeConstantStoreEntryDistance = 65536;

// lib/pipeline/BytecodeStateSourceContract.cpp
inline constexpr int kMaxBytecodeSourceStateOffset = 65536;

// lib/pipeline/evidence/ConstantStores.cpp
inline constexpr int kMaxConstantStoreDisplacement = 4096;

} // namespace neverd::limits::pipeline

#endif // NEVERD_PIPELINE_LIMITS_H
