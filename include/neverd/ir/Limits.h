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

#ifndef NEVERD_IR_LIMITS_H
#define NEVERD_IR_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::ir {

#define NEVERD_A64_SVE_STATE_LIMIT(Name, Value)                                \
  inline constexpr size_t Name = Value;
#include "neverd/ir/med/AArch64/AArch64SVEState.def"

/// Shared HighIR text printing, including structured statement children.
inline constexpr unsigned kMaxHighIRPrintNesting = 200;

/// Maximum workers in statement-region symbolic simplification.
inline constexpr unsigned kMaxHighSymbolicWorkers = 4u;

/// Parameters retained in an explicit native source signature.
inline constexpr int kMaxSourceABIParameters = 64;
/// Leave room for the receiver, selector and terminating nil argument.
inline constexpr int kMaxObjCSentinelObjects = kMaxSourceABIParameters - 3;

} // namespace neverd::limits::ir

namespace neverd {

// include/neverd/ir/low/SEHRuntimeMemory.h
/// Includes the terminating NUL for either image or source-written formats.
inline constexpr size_t kMaxWindowsSEHFormatBytes = 4096;

} // namespace neverd

namespace neverd::limits::ir {

// include/neverd/ir/high/HighSwiftErrorProjection.h
inline constexpr size_t
    kHighSwiftErrorProjectionIsSwiftErrorEntryProjectedBudget = 100000;

// include/neverd/ir/high/MedToHigh.h
inline constexpr unsigned kMedToHighSemanticThreadLimit = 1;

// include/neverd/ir/low/CFGBuilder.h
inline constexpr size_t kCFGBuilderMaxRounds = 8;
inline constexpr size_t kCFGBuilderMaxDependencies = 4096;

// include/neverd/ir/med/MedLibraryRecognition.h
inline constexpr size_t
    kMedLibraryRecognitionRecognizeMedLibraryOperationsMaxWork = 250000;

// include/neverd/ir/med/MedSwitchNorm.h
inline constexpr int kMedSwitchNormMaxAffine = 2;

// lib/ir/AffineFrameState.h
inline constexpr uint64_t kAffineFrameStateMaxAlignment = uint64_t{1} << 16;

// lib/ir/NdTypes.cpp
inline constexpr unsigned kNdTypesNdTypeMakeStructBudget = 4096;
inline constexpr unsigned kNdTypesNdTypeStrBudget = 4096;

// lib/ir/RegisterFlow.cpp
inline constexpr size_t kRegisterFlowQueryRegisterFlowBudget = 1000000;

// lib/ir/SourceParameterPlacement.cpp
/// More scalars than any value a convention passes in registers holds: a
/// value with more is placed by its size alone or not at all.
inline constexpr unsigned kSourceParameterPlacementMaxScalarLeaves = 64;

// lib/ir/SourceStackFrame.cpp
inline constexpr int64_t kSourceStackFrameMaxFrame = 1 << 20;

// lib/ir/SourceVariableFlow.cpp
inline constexpr size_t kSourceVariableFlowMaxNodes = 20000;
inline constexpr size_t kSourceVariableFlowMaxWork = 2000000;
// Shared per-function budget for source-only proofs of unwritten frame bytes.
inline constexpr size_t kUnwrittenSourceFrameReadWork = 262144;
// Shared per-function budget for source-only scratch spill overwrite proofs.
inline constexpr size_t kSourceEntrySpillWork = 262144;

// lib/ir/high/HighSourceFlow.cpp
inline constexpr size_t kHighSourceFlowMaxDiagnostics = 4096;
inline constexpr size_t kHighSourceFlowMaxNodes = 100000;
inline constexpr size_t kHighSourceFlowMaxSwitchCases = 4096;
inline constexpr size_t kHighSourceFlowMaxLocals = 8192;
inline constexpr size_t kHighSourceFlowMaxOperations = 10000000;
inline constexpr size_t kHighSourceFlowMaxStateWords = 8 * 1024 * 1024;

// lib/ir/high/HighSourceInputs.cpp
inline constexpr size_t kHighSourceInputsEntryScalarLoadInputBudget = 4096;

// lib/ir/high/lower/MedToHigh.cpp
inline constexpr size_t kMedToHighMaxBytes = 98304;
inline constexpr unsigned kMedToHighMaxRecords = 4096;

// lib/ir/high/pass/HighCleanup.cpp
inline constexpr size_t kHighCleanupFrameBudget = 100000;

// lib/ir/high/pass/HighDeadStoreElim.cpp
inline constexpr size_t kHighDeadStoreElimNarrowSourceConcatLocalsBudget =
    100000;
inline constexpr size_t kHighDeadStoreElimBudget = 200000;
inline constexpr size_t kHighDeadStoreElimElimUnreadPrivateFrameStoresBudget =
    100000;

// lib/ir/high/pass/HighDemandedBytes.cpp
inline constexpr size_t kHighDemandedBytesVisitBudget = 2000000;
/// How deep a constant mask's concatenations and copies may nest.
inline constexpr unsigned kHighDemandedBytesMaxConstantMaskDepth = 16;

// lib/ir/high/pass/HighExprSimplify.cpp
inline constexpr size_t kHighExprSimplifyIsEffectFreeBudget = 128;
inline constexpr size_t kHighExprSimplifyDropBudget = 128;
inline constexpr size_t kHighExprSimplifyDemandLowBytesBudget = 128;
inline constexpr size_t kHighExprSimplifyDiscardMaskedConcatHighBudget = 128;
inline constexpr size_t kHighExpressionDiscardedConcatWork = 128;
inline constexpr size_t kHighExpressionPointerReadWork = 4096;
inline constexpr size_t kHighExpressionMemoryReadWork = 4096;
inline constexpr size_t kHighExpressionDemandedMaskWork = 64;

// lib/ir/high/pass/HighFrameStoreForwarding.cpp
inline constexpr size_t kHighFrameStoreForwardingMaxForwardValueNodes = 1024;
inline constexpr size_t kHighFrameStoreForwardingMaxForwardExpansionNodes =
    4096;
inline constexpr size_t kHighFrameStoreForwardingFrameWalkBudget = 100000;

// lib/ir/high/pass/HighPrivateFrameCopies.cpp
inline constexpr size_t kHighPrivateFrameCopiesBudget = 1000000;

// lib/ir/high/pass/HighSharedExpressions.cpp
/// A statement prints this many nodes at least before a repeated value is
/// worth a local, and at least this many times its distinct nodes unless it
/// exceeds kMaxPrintedNodes.
inline constexpr uint64_t kHighSharedExpressionsMinPrintedNodes = 256;
inline constexpr uint64_t kHighSharedExpressionsMinRepetition = 2;
/// A repeated value this small stays inline.
inline constexpr uint64_t kHighSharedExpressionsMinNamedNodes = 4;
/// A statement that still prints more nodes after its repeated values have
/// names prints its large movable parts as locals of about this size.
inline constexpr uint64_t kHighSharedExpressionsMaxPrintedNodes = 512;

// lib/ir/high/pass/HighSymSimplify.cpp
/// A leaf or one already-canonical operator cannot become shorter.  Four nodes
/// is the first useful case: `~x + 1` is four and simplifies to `-x`.
inline constexpr size_t kHighSymSimplifyMinInterestingNodes = 4;
/// How much a rewrite has to save before it is worth making.
///
/// The cost is the rendered tree rather than the shared graph, so one unit is a
/// real operator removed from the decompiled expression.  Requiring a strict
/// improvement prevents canonical-form churn without hiding compact identities
/// such as `~x + 1`.
inline constexpr size_t kHighSymSimplifyMinGain = 1;
/// Widest expression this pass hands the engine, in bits.
///
/// The engine itself has no width ceiling — its literals are arbitrary
/// precision, and a 256-bit word is measured exactly like a 32-bit one — so
/// the only reason to have one here is that HighIR records a size for every
/// type, including a struct and an array.  An aggregate is not a machine word,
/// and measuring one would allocate values of its size at every corner to
/// learn nothing.  Past the widest word a machine or a virtual machine
/// actually has, a size is therefore read the way an unreadable one already
/// is: the expression is preserved rather than measured.
inline constexpr uint32_t kHighSymSimplifyMaxWidth = 1024;
inline constexpr size_t kHighSymSimplifyHasOnlyScalarLiteralInputsBudget = 4096;
inline constexpr size_t kHighSymSimplifySimplifyOneMaxWork = size_t(1) << 16;

// lib/ir/high/pass/HighVaStartForwarder.cpp
inline constexpr size_t kHighVaStartForwarderAddressBudget = 100000;

// lib/ir/high/structure/HighCFSimplify.cpp
inline constexpr unsigned kHighCFSimplifyMaxRounds = 64;
// Small expression trees need no visited table. A shared or deep expression
// switches the complete count to a DAG walk; only zero, one and many reads
// are distinguished by the single-use rule below.
inline constexpr size_t kHighCFSimplifyMaxTreeVisits = 4096;
inline constexpr unsigned kHighCFSimplifyMaxTreeDepth = 128;
inline constexpr size_t kHighCFSimplifyMaxRegionStmts = 256;

// lib/ir/high/structure/HighLoopRecovery.cpp
inline constexpr size_t kHighLoopRecoveryBudget = 1000000;

// lib/ir/high/structure/ItaniumHandlers.cpp
/// Statements a pad may run before this gives up on it: a pad is a few
/// blocks, and copying it once per clause stays well within this.
inline constexpr unsigned kItaniumHandlersMaxPadStatements = 4096;

// lib/ir/high/structure/X86/X86RegistrationFilter.cpp
inline constexpr size_t kX86RegistrationFilterRegistrationFilterValueBudget =
    256;

// lib/ir/low/CFGBuilder.cpp
// A disposable proof pays for direct-path decodes and trap lookahead alike.
// The base charge includes instruction/LowIR storage and its destruction;
// repeated owner and relocation queries also pay for the image inventories.
inline constexpr size_t kCFGBuilderMaxScratchDecodeAttempts = 1024;
inline constexpr unsigned kCFGBuilderMaxLookaheadInstructions = 32;

// lib/ir/low/X86/CFGBuilderX86Fpu.cpp
inline constexpr size_t kCFGBuilderX86FpuMaxPendingGraphs = 32;
inline constexpr size_t kCFGBuilderX86FpuMaxPendingBytes = 1024 * 1024;
inline constexpr size_t kCFGBuilderX86FpuMaxRefusals = size_t(1) << 18;
inline constexpr size_t kCFGBuilderX86FpuMaxRefusalDependencies = 256;

// lib/ir/low/LowUndefinedEffects.cpp
inline constexpr size_t kLowUndefinedEffectsMaxBufferedBytes = 64 * 1024;

// lib/ir/low/SEHFrameProof.cpp
// The shared work budget bounds total analysis; this also bounds C++ recursion.
inline constexpr size_t kSEHFrameProofMaxCalleeDepth = 16;

// lib/ir/low/SEHRuntimeMemory.cpp
inline constexpr size_t kSEHRuntimeMemoryMaxCalls = 256;
inline constexpr size_t kSEHRuntimeMemoryMaxFormats = 4096;
inline constexpr size_t kSEHRuntimeMemoryLowSEHRuntimeMemoryDigestMaxBytes =
    65536;

// lib/ir/low/SourceBooleanResultProof.h
inline constexpr size_t kSourceBooleanResultProofMaxFacts = 4096;

// lib/ir/low/SourceFrameAnalysis.cpp
inline constexpr int64_t kSourceFrameAnalysisMaxFrame = 1 << 20;
inline constexpr size_t kSourceFrameAnalysisMaxFacts = 4096;
inline constexpr size_t kSourceFrameAnalysisSourceFrameByValueCopiesBudget =
    262144;

// lib/ir/low/X86/RegistrationABIPrivate.h
inline constexpr unsigned kRegistrationABIPrivateMaxInitializerGraphDepth = 32;
inline constexpr size_t kRegistrationABIPrivateMaxInitializerGraphNodes = 256;
inline constexpr unsigned kRegistrationABIPrivateMaxObjectCallbackDepth = 32;
inline constexpr size_t kRegistrationABIPrivateMaxObjectCallbackNodes = 256;

// lib/ir/low/X86/RegistrationConditionalCallee.cpp
inline constexpr unsigned kRegistrationConditionalCalleeMaxBoundCallCheckDepth =
    4;

// lib/ir/low/jumptable/JumpTableResolverBounds.cpp
// Keep attacker-controlled work in independent accounts so an intentionally
// expensive proposal scan cannot consume the memory needed for final all-path
// replay.  Phase caps overlap, but every charge also debits one shared public
// 4096-unit invocation ceiling; this lets a large real proposal borrow unused
// direct/replay capacity without increasing total work.  The
// exact structural relation in JumpTableResolverSlice also has a per-batch
// symbolization ceiling, while its CFG snapshot, value reconstruction and
// every expression visit debit the same candidate-wide aggregate account as
// target/address roles and mask-domain recovery.
inline constexpr size_t kJumpTableResolverBoundsModuloProposalWork = 3072;
inline constexpr size_t kJumpTableResolverBoundsModuloStructuralRetentionWork =
    1536;
inline constexpr size_t kJumpTableResolverBoundsModuloDirectWork = 512;
inline constexpr size_t kJumpTableResolverBoundsModuloReplayWork = 1024;
// Proposal discovery is an under-approximation: retain at most this
// instruction-prefix worth of ops, prepaid before each append.  Candidate
// scanning charges the same ops a second time; the remaining quarter covers
// compact instruction-span records and bounded reaching-definition/evaluator
// visits.  Structural authentication retains its own independent account, and
// final all-path replay keeps the full 1024-unit reserve below.
inline constexpr size_t kJumpTableResolverBoundsMaxRetainedModuloProposalOps =
    512;

// lib/ir/low/jumptable/JumpTableResolverSlice.cpp
inline constexpr size_t kJumpTableResolverSliceMaxValueBatches = 64;
inline constexpr size_t kJumpTableResolverSliceMaxValueBytes = 8 * 1024 * 1024;
inline constexpr size_t kJumpTableResolverSliceMaxConditionBatches = 64;
inline constexpr size_t kJumpTableResolverSliceMaxConditionBytes = 1024 * 1024;
inline constexpr uint32_t kJumpTableResolverSliceMaxI386GOTOFFSiblingTables = 8;
// An index span far below the address width keeps the interval exact
// under modular address arithmetic.
inline constexpr uint64_t kJumpTableResolverSliceMaxIndexSpan = uint64_t{1}
                                                                << 20;
// Each independently symbolized predecessor arm can need one envelope
// query and one equality query per coordinate; a constant index needs
// only one path-predicate query. Reserve that allowance before its first
// SAT call, but cap the entire expanded proof at 128 possible calls.
// Every actual call also pays the local and candidate evidence accounts
// below. Exceeding either limit fails closed.
inline constexpr size_t kJumpTableResolverSliceMaxFiniteSetSolverQueries = 128;

// lib/ir/low/jumptable/ResolverDefinitionCache.h
inline constexpr size_t kResolverDefinitionCacheCapacity = 4096;

// lib/ir/low/jumptable/ResolverMemoryDefinitionCache.h
inline constexpr size_t kResolverMemoryDefinitionCacheCapacity = 1024;

// lib/ir/med/MedPointerAccesses.cpp
/// How many copies and constant additions an address is followed through.
inline constexpr unsigned kMedPointerAccessesMaxChain = 32;

// lib/ir/med/abi/MedCallingConvFP.cpp
inline constexpr size_t kMedCallingConvFPConstantWork = 65536;
inline constexpr size_t kMedCallingConvFPReadWork = 65536;

// lib/ir/med/flags/MedFlags.cpp
inline constexpr int kMedFlagsFlagDefTracesToFPBudget = 256;

// lib/ir/med/lower/LowToMedCallReturnStruct.cpp
inline constexpr size_t kLowToMedCallReturnStructWindow = 4;

// lib/ir/med/pass/MedConstantPropagation.cpp
inline constexpr size_t kMedConstantPropagationMaxDefinitions = 65536;
inline constexpr size_t
    kMedConstantPropagationPropagateInvariantConstantsBudget = 1000000;

// lib/ir/med/pass/MedImmutableTableScans.cpp
inline constexpr size_t kMedImmutableTableScansMaxEvaluationWork = 65536;
inline constexpr unsigned kMedImmutableTableScansMaxEvaluationDepth = 64;
inline constexpr unsigned kMedImmutableTableScansMaxScanIterations = 4096;
inline constexpr unsigned kMedImmutableTableScansMaxFoldRounds = 4;

// lib/ir/med/pass/MedMutableSource.cpp
inline constexpr size_t kMedMutableSourceMaxBlocks = 4096;
inline constexpr size_t kMedMutableSourceMaxValues = 4096;
inline constexpr size_t kMedMutableSourceMaxOperations = 8 * 1024 * 1024;
inline constexpr size_t kMedMutableSourceMaxBlockValues = 4 * 1024 * 1024;
inline constexpr size_t kMedMutableSourceMaxPropagationWords = 16 * 1024 * 1024;

// lib/ir/med/pass/MedStackAlignment.cpp
inline constexpr size_t kMedStackAlignmentMaxProofSteps = 1 << 16;

// include/neverd/ir/high/HighSwiftErrorProjection.h
inline constexpr int
    kHighSwiftErrorProjectionIsSwiftErrorEntryProjectedDepthLimit = 200;

// include/neverd/ir/low/ImportedMemoryCopy.h
inline constexpr int kImportedMemoryCopyReadItSecondOpsSizeLimit = 32;
inline constexpr int kImportedMemoryCopyReadRecordOpsSizeLimit = 32;

// include/neverd/ir/low/SourceFrameEffects.h
inline constexpr int kSourceFrameEffectsMatchABIValueIdentitySizeLimit = 256;

// include/neverd/ir/med/X86/I386PicAddress.h
inline constexpr int kI386PicAddressFoldI386PicAddressVisitDepthLimit = 16;

// lib/ir/NdTypes.cpp
inline constexpr int kNdTypesLayoutDepthLimit = 16;
inline constexpr int kNdTypesLayoutTFieldsSizeLimit = 64;

// lib/ir/RegisterFlow.cpp
inline constexpr int
    kRegisterFlowQueryRegisterFlowValueUsesDefinitionsFBlocksSizeLimit = 4096;
inline constexpr int kRegisterFlowQueryRegisterFlowCountLimit = 200000;
inline constexpr int kRegisterFlowQueryRegisterFlowNodesSizeLimit = 20000;
inline constexpr int kRegisterFlowQueryRegisterFlowValueSeenSizeLimit = 50000;
inline constexpr int kRegisterFlowQueryRegisterFlowValueOutSitesSizeLimit = 512;
inline constexpr int kRegisterFlowQueryRegisterFlowSSizeLimit = 4096;
inline constexpr int kRegisterFlowQueryRegisterFlowRetainedLimit = 1000000;

// lib/ir/SourceABI.cpp
inline constexpr int kSourceABIEqualTypesDepthLimit = 16;
inline constexpr int kSourceABIEqualTypesLeftParamTypesSizeLimit =
    ::neverd::limits::ir::kMaxSourceABIParameters;
inline constexpr int kSourceABIScalarTypeDepthLimit = 16;
inline constexpr int kSourceABISwiftFixedShapeHintParametersSizeLimit =
    ::neverd::limits::ir::kMaxSourceABIParameters;
inline constexpr unsigned kSourceABIEqualSourceTypesRemaining = 4096;
inline constexpr int kSourceABIValidateSourceABIHintParametersSizeLimit =
    ::neverd::limits::ir::kMaxSourceABIParameters;
inline constexpr int kSourceABIAssignDarwinSourceABIHintParametersSizeLimit =
    ::neverd::limits::ir::kMaxSourceABIParameters;

// lib/ir/SourceCallABI.cpp
inline constexpr int
    kSourceCallABINeverdAssignAnalystCallABISignatureParametersSizeLimit = 32;
inline constexpr int
    kSourceCallABINeverdSourceCallVariadicStateDefinedWorkLimit = 20000;
inline constexpr int
    kSourceCallABINeverdSourceCallStackArgumentsDefinedWorkLimit = 20000;
inline constexpr int
    kSourceCallABINeverdSourceCallArgumentsDefinedGraphWorkLimit = 20000;
inline constexpr int
    kSourceCallABINeverdSourceCallArgumentsDefinedDefinedWorkLimit = 20000;

// lib/ir/SourceStackFrame.cpp
inline constexpr int kSourceStackFrameAnalyzeSourceStackFrameFBlocksSizeLimit =
    4096;
inline constexpr int
    kSourceStackFrameAnalyzeSourceStackFramePolicyPointsSizeLimit = 256;
inline constexpr int
    kSourceStackFrameAnalyzeSourceStackFramePolicyVariablesSizeLimit = 256;
inline constexpr int
    kSourceStackFrameAnalyzeSourceStackFrameOperationCountLimit = 200000;
inline constexpr int kSourceStackFrameAnalyzeSourceStackFrameRunRunWorkLimit =
    1000000;

// lib/ir/SourceVariableFlow.cpp
inline constexpr int kSourceVariableFlowNeverdSplitSourceVariableCountLimit =
    512;
inline constexpr int
    kSourceVariableFlowNeverdSplitSourceVariableWordsFlowNodesSizeLimit =
        4 * 1024 * 1024;

// lib/ir/high/HighSourceFlow.cpp
inline constexpr int kHighSourceFlowReadsDepthLimit = 200;
inline constexpr int kHighSourceFlowBlockDepthLimit = 200;
inline constexpr int kHighSourceFlowRefineDominatesDominanceWorkLimit = 1000000;
inline constexpr int kHighSourceFlowRefineCopyDominatesDominanceWorkLimit =
    1000000;
inline constexpr int kHighSourceFlowRefineCopyRootDepthLimit = 64;
inline constexpr int kHighSourceFlowRefineCopyRootCopyWorkLimit = 100000;
inline constexpr int kHighSourceFlowRefineMasksSizeLimit = 32;
inline constexpr int kHighSourceFlowIndexRangeDepthLimit = 32;
inline constexpr int kHighSourceFlowPurePredicateDepthLimit = 32;
inline constexpr int kHighSourceFlowCarriesFrameAddressDepthLimit = 32;
inline constexpr int kHighSourceFlowBoundsQueriesSizeLimit = 128;
inline constexpr int kHighSourceFlowEliminateHighDeadPhiCopiesDepthLimit = 200;
inline constexpr int
    kHighSourceFlowEliminateHighDeadPhiCopiesStatementsSizeLimit = 100000;

// lib/ir/high/HighSourceInputs.cpp
inline constexpr int kHighSourceInputsPureEntryValueDepthLimit = 16;
inline constexpr int kHighSourceInputsEntryLoadDepthLimit = 16;
inline constexpr int
    kHighSourceInputsEntryScalarLoadInputFunctionBodySizeLimit = 256;
inline constexpr int kHighSourceInputsEntryScalarLoadInputScanScanDepthLimit =
    32;

// lib/ir/high/HighVariableEdits.cpp
inline constexpr int kHighVariableEditsTypeKeyDepthLimit = 8;
inline constexpr int kHighVariableEditsProjectionControlNodesSizeLimit = 20000;
inline constexpr int kHighVariableEditsProjectionFlowVariablesSizeLimit = 8192;
inline constexpr int kHighVariableEditsProjectionProofWorkLimit = 2000000;
inline constexpr int kHighVariableEditsDisplacementDepthLimit = 32;
inline constexpr int kHighVariableEditsFrameDepthLimit = 64;
inline constexpr int kHighVariableEditsReadVisitsLimit = 1000000;
inline constexpr int kHighVariableEditsReadDepthLimit = 200;
inline constexpr int kHighVariableEditsReadEscapeEscapeVisitsLimit = 100000;
inline constexpr int kHighVariableEditsStatementUnknownUnknownVisitsLimit =
    100000;
inline constexpr int kHighVariableEditsStatementUnknownDepthLimit = 200;

// lib/ir/high/lower/CallArgCollection.cpp
inline constexpr int
    kCallArgCollectionMedToHighConverterCollectCallArgsInlineUniqueLoadVarsDepthLimit =
        8;

// lib/ir/high/lower/X86/CallArgCollectionWin64.cpp
inline constexpr int
    kCallArgCollectionWin64ResolvePassThroughParamsIsConstLikeDepthLimit = 8;
inline constexpr int
    kCallArgCollectionWin64ResolvePassThroughParamsIsSlotDepthLimit = 8;

// lib/ir/high/lower/MedToHigh.cpp
inline constexpr int kMedToHighHighConversionTraceMedNameSizeLimit = 256;
inline constexpr int kMedToHighHighWalkWalkDepthLimit = 32;
inline constexpr int kMedToHighConverterSourceBitSliceDepthLimit = 40;
inline constexpr int kMedToHighConverterForceInlineExprDepthLimit = 30;
inline constexpr int kMedToHighConverterForceInlineCallTargetDepthLimit = 16;

// lib/ir/high/pass/HighDeadStoreElim.cpp
inline constexpr int
    kHighDeadStoreElimNarrowSourceConcatLocalsConcatLowPrefixDepthLimit = 16;
inline constexpr int kHighDeadStoreElimNarrowSourceConcatLocalsDepthLimit = 128;
inline constexpr int
    kHighDeadStoreElimNarrowSourceConcatLocalsCopyShapeDepthLimit = 8;
inline constexpr int
    kHighDeadStoreElimNarrowUnreadRegisterBytesLowPrefixDepthLimit = 16;

// lib/ir/high/pass/HighDemandedBytes.cpp
inline constexpr int kHighDemandedBytesShiftAmountsDepthLimit = 4;

// lib/ir/high/pass/HighExprSimplify.cpp
inline constexpr int kHighExprSimplifyZeroOrOneDepthLimit = 8;
inline constexpr int kHighExprSimplifySameIntegerValuePeelPeelDepthLimit = 4;

// lib/ir/high/pass/HighFlowOracle.cpp
inline constexpr int kHighFlowOracleReportHighFlowOracleReturnsDepthLimit = 8;

// lib/ir/high/pass/HighFrameAddress.h
inline constexpr int kHighFrameAddressOffsetDepthLimit = 32;

// lib/ir/high/pass/HighPrivateFrameCopies.cpp
inline constexpr int kHighPrivateFrameCopiesFrameDerivedDepthLimit = 64;
inline constexpr int kHighPrivateFrameCopiesCollectDepthLimit = 200;
inline constexpr int kHighPrivateFrameCopiesInspectVisitDepthLimit = 200;
inline constexpr int kHighPrivateFrameCopiesReplaceVisitDepthLimit = 200;

// lib/ir/high/pass/HighSymSimplify.cpp
inline constexpr int kHighSymSimplifyDefinitionInputsCostLimit = 4096;
inline constexpr int kHighSymSimplifyDefinitionInputsSizeLimit = 256;
inline constexpr int
    kHighSymSimplifyStatementRegionsClassifyWeightMinArgument2 = 4096;
inline constexpr int
    kHighSymSimplifyStatementRegionsSimplifyDefinitionsSizeLimit = 256;
inline constexpr int kHighSymSimplifyStatementRegionsStatementCountLimit = 1024;
inline constexpr int
    kHighSymSimplifyStatementRegionsFinishTotalWeightMinArgument1 = 4096;

// lib/ir/high/pass/HighTypeInference.cpp
inline constexpr int
    kHighTypeInferenceMedToHighConverterInferTypesUnwrapDepthLimit = 8;

// lib/ir/high/pass/HighVaStartForwarder.cpp
inline constexpr int kHighVaStartForwarderSpilledValueDepthLimit = 8;
inline constexpr int
    kHighVaStartForwarderRecognizeVaStartForwarderFrameValueDepthLimit = 64;

// lib/ir/high/structure/HighCFSimplify.cpp
inline constexpr int kHighCFSimplifyIsPureValueDepthLimit = 16;
inline constexpr int kHighCFSimplifyIsTailValueDepthLimit = 32;
inline constexpr int kHighCFSimplifyEvaluateWithDepthLimit = 32;
inline constexpr int kHighCFSimplifyReduceSingleUseGotosFoldableFastDepthLimit =
    32;
inline constexpr int
    kHighCFSimplifyReduceSingleUseGotosFoldableSharedDepthLimit = 32;
inline constexpr int
    kHighCFSimplifyMedToHighConverterSimplifyControlFlowFuncBodySizeLimit = 512;

// lib/ir/high/structure/HighCFSimplifyIfElse.cpp
inline constexpr int kHighCFSimplifyIfElseFoldDepthLimit = 64;
inline constexpr int kHighCFSimplifyIfElsePeelCondDepthLimit = 8;
inline constexpr int kHighCFSimplifyIfElsePeelFieldPathDepthLimit = 8;
inline constexpr int
    kHighCFSimplifyIfElseIsNegatedCallCondFromPrefixStepsLimit = 8;
inline constexpr int
    kHighCFSimplifyIfElseSinkJoinDefaultAssignCountStmtTreePrevLimit = 96;
inline constexpr int kHighCFSimplifyIfElseStructureIfElseListBodySizeLimit = 64;

// lib/ir/high/structure/HighLoopRecovery.cpp
inline constexpr int
    kHighLoopRecoveryDetectAndConvertLoopsMaxLoopPassesSelected = 2;
inline constexpr int
    kHighLoopRecoveryDetectAndConvertLoopsMaxLoopPassesDefault = 4;

// lib/ir/high/structure/ItaniumHandlers.cpp
inline constexpr int kItaniumHandlersPeelViewsDepthLimit = 8;
inline constexpr int kItaniumHandlersBaseAndOffsetDepthLimit = 8;
inline constexpr int kItaniumHandlersEvaluateDepthLimit = 16;
inline constexpr int kItaniumHandlersRewriteDepthLimit = 64;
inline constexpr int kItaniumHandlersReadsVariableDepthLimit = 64;
inline constexpr int kItaniumHandlersReadsExceptionStateDepthLimit = 64;
inline constexpr int kItaniumHandlersReadsMemoryAtDepthLimit = 64;
inline constexpr int kItaniumHandlersReadByReadsSpillDepthLimit = 64;
inline constexpr int kItaniumHandlersDropSelectorSpillsVisitDepthLimit = 64;
inline constexpr int kItaniumHandlersForEachCallSiteDepthLimit = 64;

// lib/ir/low/CFGBuilder.cpp
inline constexpr int
    kCFGBuilderBuildOnceRelocationFreeProofStillMatchesOccurrenceArithmeticProofSizeLimit =
        32;

// lib/ir/low/X86/CFGBuilderX86Fpu.cpp
inline constexpr int
    kCFGBuilderX86FpuCFGBuilderWaitForX87CallGraphWaitersForTestingSeconds = 5;
inline constexpr int kCFGBuilderX86FpuPrepareCallsBytesLimit = 4096;

// lib/ir/low/GuardedZeroSelect.cpp
inline constexpr int kGuardedZeroSelectZeroTestOfDepthLimit = 8;

// lib/ir/low/SEHRuntimeMemory.cpp
inline constexpr int
    kSEHRuntimeMemoryLowSEHRuntimeMemoryDigestCallModuleSizeLimit = 256;
inline constexpr int
    kSEHRuntimeMemoryLowSEHRuntimeMemoryDigestCallNameSizeLimit = 256;

// lib/ir/low/SourceBooleanResultProof.h
inline constexpr size_t kSourceBooleanResultProofRemaining = 262144;
inline constexpr int
    kSourceBooleanResultProofProveSourceBooleanResultNormalizationFunctionBlocksSizeLimit =
        256;
inline constexpr int
    kSourceBooleanResultProofProveSourceBooleanResultNormalizationCallsSizeLimit =
        8192;
inline constexpr int
    kSourceBooleanResultProofProveSourceBooleanResultNormalizationCountLimit =
        8192;

// lib/ir/low/SourceFrameAnalysis.cpp
inline constexpr size_t kSourceFrameAnalysisRemaining = 262144;
inline constexpr size_t kSourceFrameStorageObservationPointers = 8;
inline constexpr size_t kSourceFrameTextBytes = 1024;
inline constexpr size_t kSourceFrameTextObservations = 32;
inline constexpr int kSourceFrameAnalysisTransferWriteOutputBytesLimit = 256;
inline constexpr int kSourceFrameAnalysisFrameGraphCountLimit = 16384;
inline constexpr int
    kSourceFrameAnalysisRestoresNativeSourceStateImplCountLimit = 16384;
inline constexpr int
    kSourceFrameAnalysisSourceFrameCallArgumentStorageBytesLimit = 4096;
inline constexpr int
    kSourceFrameAnalysisObservesTerminalNativeSourceStateBlockOpsSizeLimit =
        262144;
inline constexpr int
    kSourceFrameAnalysisPreservesNativeSourceLeafStateCountLimit = 16384;
inline constexpr size_t
    kSourceFrameAnalysisPreservesNativeSourceLeafStateRemaining = 262144;
inline constexpr int
    kSourceFrameAnalysisSourceFrameCompleteOutputBytesFunctionBlocksSizeLimit =
        128;

// lib/ir/low/X86/RegistrationCalleeIndex.cpp
inline constexpr int
    kRegistrationCalleeIndexRegistrationCallCalleeIndexContractsTargetsSizeLimit =
        256;
inline constexpr int
    kRegistrationCalleeIndexRegistrationCallCalleeIndexContractsCacheSizeLimit =
        256;
inline constexpr int
    kRegistrationCalleeIndexRegistrationCallCalleeIndexCleanupContractsCleanupCacheSizeLimit =
        256;

// lib/ir/low/X86/RegistrationCalleeStack.cpp
inline constexpr int kRegistrationCalleeStackProveCacheSizeLimit = 256;
inline constexpr int
    kRegistrationCalleeStackRegistrationCallCalleeIndexStackContractsTargetsSizeLimit =
        256;

// lib/ir/low/X86/RegistrationCallerABI.cpp
inline constexpr int
    kRegistrationCallerABIHasRegistrationCallFrameABITargetsSizeLimit = 256;
inline constexpr int
    kRegistrationCallerABIHasRegistrationCallFrameABIExaminedSizeLimit = 256;
inline constexpr int
    kRegistrationCallerABIHasRegistrationCallFrameABIPendingSizeLimit = 256;

// lib/ir/low/X86/RegistrationLocalUnwind.cpp
inline constexpr int
    kRegistrationLocalUnwindRegistrationCallCalleeIndexLocalUnwindContractsSeenSizeLimit =
        256;

// lib/ir/low/X86/RegistrationNestedFinallyFrame.cpp
inline constexpr int
    kRegistrationNestedFinallyFrameRegistrationStateSolverRunSEHNestedFinallyFrameDepthLimit =
        16;

// lib/ir/low/X86/RegistrationSEHControl.cpp
inline constexpr int
    kRegistrationSEHControlRegistrationCallCalleeIndexSehControlContractsSeenSizeLimit =
        64;

// lib/ir/low/X86/RegistrationStateContracts.cpp
inline constexpr int
    kRegistrationStateContractsRegistrationStateSolverInitializeContractsLocalUnwindsSizeLimit =
        256;
inline constexpr int
    kRegistrationStateContractsRegistrationStateSolverInitializeContractsStacksSizeLimit =
        256;
inline constexpr int
    kRegistrationStateContractsRegistrationStateSolverInitializeContractsCalleesSizeLimit =
        256;

// lib/ir/low/X86/RegistrationStateSEHCall.cpp
inline constexpr int
    kRegistrationStateSEHCallRegistrationStateSolverSehCallContinuationSeenSizeLimit =
        64;

// lib/ir/low/X86/RegistrationThrowABI.cpp
inline constexpr int
    kRegistrationThrowABIGetCheckedX86RegistrationThrowCalleeABIInitializersSizeLimit =
        256;

// lib/ir/low/jumptable/JumpTableResolverBounds.cpp
inline constexpr unsigned
    kJumpTableResolverBoundsCFGBuilderInferBoundsFromMaskWithAbsoluteProofExactFiniteCoordinatesForTargetsQueryCapacityMinArgument2 =
        64u;

// lib/ir/low/jumptable/JumpTableResolverEmu.cpp
inline constexpr unsigned
    kJumpTableResolverEmuCFGBuilderTryEmulatedResolutionMinArgument2 = 64u;

// lib/ir/low/jumptable/JumpTableResolverGroup.cpp
inline constexpr int
    kJumpTableResolverGroupCFGBuilderRecoverGuardedJumpTableGroupSectionSizeLimit =
        256;

// lib/ir/low/jumptable/JumpTableResolverSlice.cpp
inline constexpr int kJumpTableResolverSliceCanRetainInputSizeLimit = 8192;
inline constexpr int kJumpTableResolverSliceCanRetainBBlockStartsSizeLimit =
    8192;
inline constexpr int
    kJumpTableResolverSliceCanRetainBResolvedTableInfoSizeLimit = 1024;
inline constexpr size_t kJumpTableResolverSliceCanRetainRemaining =
    8 * 1024 * 1024;
inline constexpr int
    kJumpTableResolverSliceCFGBuilderTableLoadAddressesMatchRoleExactRelocationFreeAddressProofOccurrenceArithmeticProofSizeLimit =
        32;

// lib/ir/low/jumptable/JumpTableResolverStack.cpp
inline constexpr int
    kJumpTableResolverStackCFGBuilderResolveStackMaterializedTableSourceTraceValToConstSrcDepthLimit =
        4;

// lib/ir/low/jumptable/JumpTableResolverTargets.cpp
inline constexpr unsigned
    kJumpTableResolverTargetsCFGBuilderReadTableEntriesMinArgument2 = 64u;

// lib/ir/low/jumptable/ResolverPointLookup.h
inline constexpr int kResolverPointLookupFindPointsSizeLimit = 64;

// lib/ir/med/MedComLibraryRecognition.cpp
inline constexpr int kMedComLibraryRecognitionValueDepthLimit = 64;
inline constexpr int kMedComLibraryRecognitionStackAddressDepthLimit = 32;
inline constexpr int kMedComLibraryRecognitionWalkPBlocksSizeLimit = 32;
inline constexpr int kMedComLibraryRecognitionVerifyFBlocksSizeLimit = 32;
inline constexpr int kMedComLibraryRecognitionInlineRegionCollectDepthLimit =
    64;
inline constexpr int kMedComLibraryRecognitionInlineRegionUnobservedDepthLimit =
    32;

// lib/ir/med/MedDispatchSlots.cpp
inline constexpr int kMedDispatchSlotsFindTableSwitchIndexStepsLimit = 256;

// lib/ir/med/MedLibraryRecognition.cpp
inline constexpr int kMedLibraryRecognitionCanonicalDepthLimit = 64;
inline constexpr int kMedLibraryRecognitionUnsignedFitsDepthLimit = 32;
inline constexpr int kMedLibraryRecognitionLiteralDepthLimit = 32;
inline constexpr int kMedLibraryRecognitionMatchDepthLimit = 128;
inline constexpr int kMedLibraryRecognitionPredicateCarrierDepthLimit = 32;
inline constexpr int kMedLibraryRecognitionUnobservedDepthLimit = 32;
inline constexpr int kMedLibraryRecognitionSameLowResultDepthLimit = 16;
inline constexpr int kMedLibraryRecognitionComparisonDepthLimit = 32;
inline constexpr int
    kMedLibraryRecognitionNeverdRecognizeMedLibraryOperationsResultMatchesSizeLimit =
        128;

// lib/ir/med/X86/RegistrationCallResult.cpp
inline constexpr int
    kRegistrationCallResultHasUnusedX86RegistrationCallResultSeenSizeLimit =
        256;

// lib/ir/med/abi/MedABIPassSupport.cpp
inline constexpr int kMedABIPassSupportReduceAddrDepthLimit = 128;
inline constexpr int kMedABIPassSupportResolveIndirectTargetAddrDepthLimit = 16;
inline constexpr int kMedABIPassSupportPeelCopyDefIndexDepthLimit = 16;
inline constexpr int kMedABIPassSupportResolveIndirectTargetArgIdxDepthLimit =
    16;
inline constexpr int kMedABIPassSupportAuthenticatedStackOffsetDepthLimit = 128;
inline constexpr int kMedABIPassSupportPreservesSpillAcrossCallDepthLimit = 16;
inline constexpr int kMedABIPassSupportStackPtrDeltaAtDepthLimit = 128;
inline constexpr int kMedABIPassSupportDerivesFromFrameRegDepthLimit = 64;
inline constexpr int kMedABIPassSupportBuildCallSpOffsetsDepthLimit = 128;
inline constexpr int kMedABIPassSupportRelStackOffDepthLimit = 128;
inline constexpr int kMedABIPassSupportUniquePredNonNullGuardDepthLimit = 8;

// lib/ir/med/abi/MedCallingConv.cpp
inline constexpr int kMedCallingConvDetectStackParamsTraceOffDepthLimit = 64;
inline constexpr int
    kMedCallingConvDetectIndirectResultParamDerivesFromIRRDepthLimit = 32;

// lib/ir/med/abi/MedCallingConvFP.cpp
inline constexpr int kMedCallingConvFPDetectFPParamsShiftAmountsDepthLimit = 4;
inline constexpr int kMedCallingConvFPDetectFPParamsConstantBitsDepthLimit = 16;

// lib/ir/med/abi/X86/MedCallingConvX86.cpp
inline constexpr int
    kMedCallingConvX86DetectCdeclStackParamsTraceOffDepthLimit = 4096;

// lib/ir/med/abi/MedVariadic.cpp
inline constexpr int kMedVariadicEntrySpDeltaDepth0Default = 64;

// lib/ir/med/abi/AArch64/MedVariadicDarwin.cpp
inline constexpr int
    kMedVariadicDarwinHasDarwinVariadicPrologueThruCopyDepthLimit = 32;
inline constexpr int
    kMedVariadicDarwinHasDarwinVariadicPrologueOvfDeltaDepthLimit = 32;

// lib/ir/med/lower/LowToMed.cpp
inline constexpr int
    kLowToMedConverterNeutralizeStackProbeCallsConstAddrDepthLimit = 8;

// lib/ir/med/lower/LowToMedSourceCalls.cpp
inline constexpr int
    kLowToMedSourceCallsLowToMedConverterBindSourceCallsParametersSizeLimit =
        64;

// lib/ir/med/pass/MedSourceParameterUses.cpp
inline constexpr int
    kMedSourceParameterUsesObservedMedSourceEntryBytesObserveNodesSizeLimit =
        65536;
inline constexpr size_t
    kMedSourceParameterUsesObservedMedSourceEntryBytesRemaining = 262144;

// lib/ir/med/pass/MedStackAlignment.cpp
inline constexpr int kMedStackAlignmentResolveDepthLimit = 64;

// lib/ir/med/pass/MedTypePass.cpp
inline constexpr int kMedTypePassFpReturnElemSizeDepthLimit = 16;
inline constexpr int kMedTypePassScalarFPLoadElemSizeDepthLimit = 16;
inline constexpr int kMedTypePassX87ReturnElemSizeDepthLimit = 16;
inline constexpr int kMedTypePassIsReturnRegRestoreDepthLimit = 8;
inline constexpr int kMedTypePassIntRetEffWidthRecDepthLimit = 8;
inline constexpr int kMedTypePassDefinedLowBytesRecDepthLimit = 16;
inline constexpr int kMedTypePassInferReturnTypeStepsLimit = 16;

// include/neverd/ir/med/MedSwitchNorm.h
inline constexpr int kMedSwitchNormPeelAffineSwitchVarHopLimit = 64;

// lib/ir/high/lower/CallArgCollection.cpp
inline constexpr int kCallArgCollectionConsiderStorePeelLimit = 4;
inline constexpr int kCallArgCollectionExprFromWindowValuePeelLimit = 8;
inline constexpr int kCallArgCollectionAttachProducingLoadPeelLimit = 8;
inline constexpr int kCallArgCollectionAttachProducingLoadFromWindowsPeelLimit =
    8;

// lib/ir/high/lower/MedToHigh.cpp
inline constexpr int kMedToHighMedToHighConverterReduceLateGotosRoundLimit = 8;

// lib/ir/high/lower/NdOpCallIndLowering.cpp
inline constexpr int
    kNdOpCallIndLoweringMedToHighConverterResolveCallIndTargetHopLimit = 4;

// lib/ir/high/pass/HighDCE.cpp
inline constexpr int kHighDCEPreDCEPreIterLimit = 8;
inline constexpr int kHighDCEFollowingHasObservableWorkKBeginLimit = 32;
inline constexpr int kHighDCEIterativeDCEOuterLimit = 6;
inline constexpr int kHighDCEIterativeDCEIterLimit = 10;
inline constexpr int kHighDCEMedToHighConverterEliminateDeadStmtsIterLimit = 10;

// lib/ir/high/structure/HighCFSimplify.cpp
inline constexpr int kHighCFSimplifyDuplicateSmallReturnTailsRoundLimit = 4;
inline constexpr int kHighCFSimplifyReduceSingleUseGotosAppliedLimit = 512;

// lib/ir/high/structure/HighCFSimplifyIfElse.cpp
inline constexpr int
    kHighCFSimplifyIfElseDropImpliedFallthroughCondsSkippedLimit = 32;
inline constexpr int kHighCFSimplifyIfElseFoldJoinValueGotoChainArmsSizeLimit =
    8;
inline constexpr int kHighCFSimplifyIfElseVisitSinksLimit = 32;
inline constexpr int kHighCFSimplifyIfElseInvertExternalSkipGotoKNextILimit =
    32;
inline constexpr int kHighCFSimplifyIfElseFoldSameTargetSkipGotoKNextILimit =
    32;
inline constexpr int kHighCFSimplifyIfElseStructureIfElseListSinksLimit = 32;

// lib/ir/low/CFGBuilder.cpp
inline constexpr int kCFGBuilderCFGBuilderNextKnownFunctionEntryILimit = 16;

// lib/ir/low/X86/RegistrationConditionalCall.cpp
inline constexpr int
    kRegistrationConditionalCallHasConditionalCallFrameOffsetLimit = 128;

// lib/ir/low/X86/RegistrationConditionalCallee.cpp
inline constexpr int kRegistrationConditionalCalleeProveBoundSEHCalleeArgLimit =
    128;

// lib/ir/med/MedDispatchSlots.cpp
inline constexpr int kMedDispatchSlotsSkipPassThroughHopLimit = 256;

// lib/ir/med/lower/LowToMed.cpp
inline constexpr int
    kLowToMedLowToMedConverterNeutralizeStackProbeCallsGuardLimit = 8;

// lib/ir/med/pass/MedPropagation.cpp
inline constexpr int kMaxMedCopyPropagationRounds = 10;
inline constexpr int kMaxMedFoldPropagationRounds = 5;

// lib/ir/med/pass/MedTypePass.cpp
inline constexpr int kMedTypePassFpReturnElemSizeStepLimit = 16;
inline constexpr int kMedTypePassIsStackPtrDLimit = 8;
inline constexpr int kMedTypePassIsPopAdvanceDLimit = 8;
inline constexpr int kMedTypePassViewRootDLimit = 16;

// lib/ir/high/pass/HighDCE.cpp
inline constexpr int kMaxHighDCEReadDepth = 64;

// lib/ir/high/HighSourceFlow.cpp
inline constexpr int kMaxSourceFlowIntegerViewDepth = 16;

// lib/ir/high/HighSourceFlow.cpp
inline constexpr int kMaxSourceFlowNegationDepth = 8;

// lib/ir/high/pass/HighPrivateFrameCopies.cpp
inline constexpr int kMaxPrivateFrameLeafDepth = 16;

// lib/ir/high/pass/HighPrivateFrameCopies.cpp
inline constexpr int kMaxPrivateFrameCopyPasses = 64;

// lib/ir/med/abi/MedCallingConv.cpp
inline constexpr int kMaxCallingConventionUseRounds = 64;

// lib/ir/med/pass/MedTypePass.cpp
inline constexpr int kMaxTypeInferencePredecessorHops = 4;

// lib/ir/low/X86/RegistrationCalleeStack.cpp
inline constexpr int kMaxReturningStackProofDepth = 32;

// lib/loader/ObjC/ObjCBlockCallHints.cpp
inline constexpr int kMaxSourceFrameDisplacement = 1048576;

// lib/sdk/capi/ObjCSourceBindings.h
inline constexpr int kMaxSourceObjectBytes = 1048576;

// lib/ir/low/SourceBooleanResultProof.h
inline constexpr int kMaxBooleanProofBlockBytes = 32768;

// lib/ir/high/HighSourceFlow.cpp
inline constexpr int kMaxSourceFlowUnits = 1000000;

// lib/loader/ObjC/ObjCCallHints.cpp
inline constexpr int kMaxSourceStackArgumentBytes = 4096;

// lib/ir/NdTypes.cpp
inline constexpr int kMaxNaturalStructBytes = 4096;

// lib/ir/high/structure/HighCFSimplify.cpp
inline constexpr int kMaxIfElseNestedStatements = 1536;

// lib/ir/high/lower/MedToHigh.cpp
inline constexpr int kSlowMedToHighReportMilliseconds = 1000;

// lib/ir/high/pass/HighSymSimplify.cpp
inline constexpr int kMinParallelHighSymbolicWeight = 4096;

// lib/ir/high/structure/HighCFSimplifyIfElse.cpp
inline constexpr int kMaxIfElseCandidateVisits = 128;

// lib/ir/low/SourceFrameAnalysis.cpp
inline constexpr int kMaxBorrowedOutputBytes = 256;

// lib/ir/high/HighVariableEdits.cpp
inline constexpr int kMaxVariableEditNesting = 200;

} // namespace neverd::limits::ir

#endif // NEVERD_IR_LIMITS_H
