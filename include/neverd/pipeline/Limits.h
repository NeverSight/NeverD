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

// lib/pipeline/PipelineCallAbi.cpp
/// The callees next in a restricted run's signature read
/// (omittedDirectCalleeSignatures) that are lifted and converted together on
/// the pipeline workers before the read takes them in queue order. Each holds
/// a whole conversion until its turn; the breadth-first levels of a selection
/// rarely exceed this, so it bounds that memory without serializing them.
inline constexpr size_t kPipelineCallAbiCalleeReadAhead = 64;
/// The callees a restricted run reads at most for the signatures its calls
/// pass (omittedDirectCalleeSignatures), its direct callees first; each read
/// callee is converted, probed and recovered with the run.  Callees past the
/// first few rarely bound a selection's calls: on the 300-function ntoskrnl
/// 19041.6456 sample, reading 16 left all 600 decompiles as reading 256 did
/// and 8 changed three, and 32 left the EH corpus, the switch corpus and the
/// whole units as they were too.
inline constexpr size_t kPipelineCallAbiMaxOmittedCalleeReads = 32;
/// Whether a replica's callee prefill goes on to solve the signatures of the
/// bodies it read image-wide (stepGlobalCalleeSignatures), which runs then
/// install instead of probing and recovering those callees again around
/// their own selection.  On the 300-function ntoskrnl 19041.6456 sample the
/// callees a run reads at 32 reads end with the summaries and recoveries an
/// unbounded read gives them, which is what the solve gives a callee it
/// read every callee of.
inline constexpr bool kPipelineCallAbiGlobalSignatures = true;
/// Bodies one step of that solve probes or recovers at least, on the
/// pipeline's workers: about as long as a decompile of a median function.
inline constexpr size_t kPipelineCallAbiGlobalSignatureStepBodies = 256;
/// Rounds of forwarder propagation the solve takes at most; past them it
/// gives no body a signature and runs read every callee themselves.
inline constexpr size_t kPipelineCallAbiGlobalSignatureMaxRounds = 64;
/// Fewest such callees worth a worker batch. Each is a whole function's lift
/// and MedIR conversion, so two already outweigh starting the workers.
inline constexpr size_t kPipelineCallAbiMinParallelCalleeReads = 2;
/// Fewest kept bodies without a probe their run's summaries hold that the
/// forwarder fixed point probes together on the workers before it starts
/// (propagateForwardedCallArities).  Each probe recovers a copy of one body;
/// a few cost less than starting the workers and copying their summaries,
/// as a revisit's or a small selection's usually are.
inline constexpr size_t kPipelineCallAbiMinSpeculativeProbes = 8;

// lib/pipeline/PipelineCallAbiDetail.h
inline constexpr size_t kPipelineCallAbiDetailMaxProbes = 16;
inline constexpr size_t kPipelineCallAbiDetailMaxConversions = 4;
/// The omitted-callee memo's bodies take up to a twelfth of the host's
/// physical memory (OmittedCalleeMemo::maxBytes), between these bounds.
inline constexpr size_t kPipelineCallAbiDetailMaxBytes = size_t{2048} << 20;
inline constexpr uint64_t kPipelineCallAbiDetailMaxBytesCeiling = uint64_t{4096}
                                                                  << 20;
inline constexpr uint64_t kPipelineCallAbiDetailMemoryShare = 12;

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

// lib/pipeline/PipelineSourceInline.cpp
// Source call arguments can contain a short chain of ABI carrier casts.
inline constexpr unsigned kSourceInlineOperandDepth = 16;
// An interactive source view can include the root and its EH continuations.
inline constexpr size_t kSourceInlineRoots = 8;
// Recover only a small set of complete support bodies on this lazy path.
inline constexpr size_t kSourceInlineCallees = 4;
// Exclude large helpers before expansion; cap aggregate duplicated IR too.
inline constexpr size_t kSourceInlineBodyOperations = 1024;
inline constexpr size_t kSourceInlineExpansionOperations = 16384;

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
