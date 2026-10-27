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

#ifndef NEVERD_BACKEND_LIMITS_H
#define NEVERD_BACKEND_LIMITS_H

#include "neverd/ir/Limits.h"

#include <cstddef>
#include <cstdint>

namespace neverd::limits::backend {

// lib/backend/c/CSourceRecorder.cpp
inline constexpr size_t kCSourceRecorderCSourceRecorderPrepareHighSourcesWork =
    250000;
inline constexpr size_t
    kCSourceRecorderCSourceRecorderPrepareHighSourcesBudget = 4096;
inline constexpr size_t kCSourceRecorderCSourceRecorderPrepareLLVMSourcesWork =
    250000;

// lib/backend/c/HighC/HighCEmitter.cpp
/// The longest string C promises to translate (C11 5.2.4.1).
inline constexpr uint64_t kHighCEmitterImageStringMaxBytes = 4095;

// lib/backend/c/LLVMC/LLVMCExprWriter.cpp
inline constexpr unsigned kLLVMCExprWriterCheckConstantFoldBudgetMaxWork = 256;
inline constexpr unsigned kLLVMCExprWriterCheckConstantFoldBudgetMaxDepth = 32;
inline constexpr unsigned kLLVMCExprWriterLLVMCWriterIsNormalizedBooleanBudget =
    64;
inline constexpr int kLLVMCExprWriterLLVMCWriterFoldImmediateImmediateFoldWork =
    64;
inline constexpr uint64_t kLLVMCExprWriterMaxInterior = 256;

// lib/backend/c/LLVMC/LLVMCFuncWriter.cpp
inline constexpr unsigned
    kLLVMCFuncWriterLLVMCWriterPlanExpressionMaterializationWork = 64;
inline constexpr unsigned kLLVMCFuncWriterCanSinkCallBudget = 4096;
inline constexpr size_t kLLVMCFuncWriterMaxEHAnnotationRows = 4096;
inline constexpr size_t kLLVMCFuncWriterMaxEHAnnotationChars = 256 * 1024;
inline constexpr size_t kLLVMCFuncWriterMaxEHAnnotationStringChars = 4096;
inline constexpr unsigned kLLVMCFuncWriterLLVMCWriterUsersAreDeadCopiesWork =
    64;

// lib/backend/c/LLVMC/LLVMCMemoryUpdates.cpp
inline constexpr unsigned kLLVMCMemoryUpdateAddressWork = 128;
inline constexpr unsigned kLLVMCMemoryUpdateValueWork = 128;

// lib/backend/c/LLVMC/LLVMCScalarExpressions.cpp
inline constexpr unsigned
    kLLVMCScalarExpressionsLLVMCWriterScalarExpressionTextWork = 128;

// lib/backend/c/LLVMC/LLVMCStmtWriter.cpp
inline constexpr size_t kLLVMCStmtWriterFreezeHasDefinedCProjectionBudget =
    65536;
inline constexpr unsigned kLLVMCStmtWriterMaxForwardDepth = 8;
inline constexpr unsigned kLLVMCStmtWriterMaxForwardCost = 32;
inline constexpr size_t kLLVMCStoredValueGraphWork = 8 * 1024 * 1024;
inline constexpr unsigned kLLVMCStoredValueTraversalWork = 4096;
inline constexpr unsigned
    kLLVMCStmtWriterLLVMCWriterAllocaStoredValueBeforeWork = 32;

// lib/backend/c/dialect/CSyntaxParser.cpp
/// Nesting deeper than this is refused rather than read on a stack it could
/// exhaust; emitted lane assembly reaches about 200.
inline constexpr unsigned kCSyntaxParserMaxNesting = 1024;

// lib/backend/c/pass/LLVMC/LLVMCBitTests.cpp
inline constexpr unsigned kLLVMCBitTestsQueryWork = 512;

// lib/backend/c/pass/LLVMC/LLVMCBitTests.h
inline constexpr unsigned kLLVMCBitTestsWorkLimit = 262144;

// lib/backend/c/pass/LLVMC/LLVMCCommonBranches.cpp
inline constexpr size_t kReturnEdgeDistributionMaxBlocks = 4096;
inline constexpr size_t kLLVMCCommonBranchesMaxEdges = 4096;
// Bound graph construction, repeated dominance queries, PHI scans and output
// size independently of source size. Exhaustion retains the remaining CFG.
inline constexpr size_t kCommonBranchFactoringMaxBlocks = 4096;
inline constexpr size_t kLLVMCCommonBranchesFactorCommonBranchTestsMaxWork =
    65536;
inline constexpr unsigned kLLVMCCommonBranchesMaxJoins = 64;

// lib/backend/c/pass/LLVMC/LLVMCLoopPhases.cpp
inline constexpr size_t kLLVMCLoopPhasesFactorLoopPhiExpressionsMaxWork = 65536;
inline constexpr unsigned kLLVMCLoopPhasesMaxChanges = 64;

// lib/backend/c/pass/LLVMC/LLVMCStoreForwarding.cpp
inline constexpr unsigned kLLVMCStoreForwardingAnalyzeStoreForwardingBudget =
    1024;

// lib/backend/c/render/CTypeFormat.cpp
// Characters shown before the text is cut; UTF-8 continuation bytes
// belong to the character before them.
inline constexpr unsigned kCTypeFormatMaxCharacters = 64;
/// A string that does not end within this many bytes is not read. Literal
/// replacement keeps the complete string; only comments use the display cap.
inline constexpr size_t kCTypeFormatStringWindow = 4096;

// lib/backend/codegen/COFF/COFFExceptionPatch.cpp
inline constexpr uint32_t kCOFFExceptionPatchMaxGeneratedLanguageRecords =
    1u << 16;

// lib/backend/codegen/COFF/COFFPatchObject.cpp
inline constexpr uint64_t kCOFFPatchObjectMaxStorage = 64 * 1024 * 1024;

// lib/backend/codegen/ELF/ELFExceptionPatch.cpp
inline constexpr uint64_t kELFExceptionPatchDecodeBudget = 64 * 1024 * 1024;

// lib/backend/codegen/MachO/MachOPatchObject.cpp
inline constexpr uint64_t kMachOPatchObjectMaxStorage = 64 * 1024 * 1024;

// lib/backend/codegen/MachO/MachOSigner.cpp
inline constexpr unsigned kMachOSignerCodesignTimeoutSeconds = 30;
inline constexpr size_t kMachOSignerMaximumDetailLength = 256;

// lib/backend/codegen/PatchObject.cpp
inline constexpr uint64_t kPatchObjectMaxImageSize = 64 * 1024 * 1024;

// lib/backend/llvm/LLVMSourceMap.cpp
inline constexpr size_t
    kLLVMSourceMapLLVMSourceMapRefreshExpressionOriginsBudget = 250000;

// lib/backend/llvm/X86/MedLLVMX86GlobalData.cpp
inline constexpr int
    kMedLLVMX86GlobalDataMedLLVMEmitterI386WalkedPointerDerefBudget = 256;

// lib/backend/llvm/data/MedLLVMConstClass.cpp
inline constexpr int kMedLLVMConstClassReachesSegBaseBudget = 512;
inline constexpr int kWritableSelectPeerCycleWork = 256;
inline constexpr int kSymbolizedSelectPeerWork = 256;
inline constexpr int kMedLLVMConstClassReachesBitwiseBudget = 256;

// lib/backend/llvm/data/MedLLVMConstUse.cpp
inline constexpr int kMedLLVMConstUseReachesBudget = 256;
inline constexpr int kMedLLVMConstUseReachesMemAddrBudget = 256;
inline constexpr int kMedLLVMConstUseReachesOperationBudget = 256;

// lib/backend/llvm/data/MedLLVMConstWalkedBase.cpp
inline constexpr int kMedLLVMConstWalkedBaseReachesConstBudget = 512;
inline constexpr int kMedLLVMConstWalkedBaseWalkBudget = 512;
inline constexpr int
    kMedLLVMConstWalkedBaseMedLLVMEmitterIsInductionRodataStringBaseBudget =
        256;

// lib/backend/llvm/data/MedLLVMFailureSnapshot.h
inline constexpr size_t kMedLLVMFailureSnapshotRawLimit = 16 * 1024 * 1024;

// lib/backend/llvm/data/MedLLVMIndexedGlobal.cpp
inline constexpr int
    kMedLLVMIndexedGlobalMedLLVMEmitterTryResolveInductionGlobalPtrBudget = 256;
inline constexpr int kMedLLVMIndexedGlobalClassifyIndexedBaseModelBudget = 256;

// lib/backend/llvm/data/MedLLVMLiteralTable.cpp
inline constexpr size_t kMedLLVMLiteralTableScanNarrowSourcesBudget = 8192;
inline constexpr size_t
    kMedLLVMLiteralTableMedLLVMEmitterTryResolveSelectMergeTableBudget = 128;

// lib/backend/llvm/data/MedLLVMSpillPredicate.cpp
// Frame offsets are small; reject any range whose bounds would not fit.
inline constexpr int64_t kMedLLVMSpillPredicateCoverageLimit = int64_t{1} << 40;

// lib/backend/llvm/data/MedLLVMSymbolizedPtr.cpp
inline constexpr int
    kMedLLVMSymbolizedPtrMedLLVMEmitterReloadsSymbolizedWritablePtrBudget = 256;
inline constexpr int
    kMedLLVMSymbolizedPtrMedLLVMEmitterAddrHasSymbolizedSegConstBudget = 256;

// lib/backend/llvm/data/MedLLVMWritableData.cpp
inline constexpr int kMedLLVMWritableDataComputeBudget = 4096;
inline constexpr int kMedLLVMWritableDataI386WritableBlendAddrBudget = 64;

// lib/backend/llvm/resolve/MedLLVMAddrResolve.cpp
inline constexpr unsigned
    kMedLLVMAddrResolveMedLLVMEmitterValueIsGuardedScalarOffsetBudget = 256;
inline constexpr int kMedLLVMAddrResolveScalarRecurrenceHasInitializerBudget =
    8192;
inline constexpr int kMedLLVMAddrResolveProveClosedScalarGraphBudget = 8192;
inline constexpr size_t
    kMedLLVMAddrResolveMedLLVMEmitterPhiHasPureForwardingCycleBudget = 4096;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterUniqueDataAddressOwnerBudget = 4096;

// lib/backend/llvm/resolve/MedLLVMCodePtrResolve.cpp
inline constexpr int kMedLLVMCodePtrResolveLeafBudget = 128;
inline constexpr int kMedLLVMCodePtrResolveComputeBudget = 4096;
inline constexpr unsigned kMedLLVMCodePtrResolveMergeBudget = 256;

// lib/backend/llvm/resolve/MedLLVMVarAccess.cpp
inline constexpr int
    kMedLLVMVarAccessMedLLVMEmitterSummarizeConstantProvenanceBudget = 512;
inline constexpr int kMedLLVMVarAccessMedLLVMEmitterGetRawSegmentOffsetBudget =
    256;

// lib/backend/c/HighC/HighCExprBinOp.cpp
inline constexpr int kHighCExprBinOpCountBelowDepthLimit = 8;
inline constexpr int
    kHighCExprBinOpHighCWriterRenderBinOpOperandsPeelZeroViewDepthLimit = 6;
inline constexpr int
    kHighCExprBinOpHighCWriterRenderBinOpOperandsPeelViewDepthLimit = 8;
inline constexpr int
    kHighCExprBinOpHighCWriterRenderBinOpOperandsDisplayIntDepthLimit = 6;

// lib/backend/c/HighC/HighCExprWriter.cpp
inline constexpr int
    kHighCExprWriterHighCWriterRenderUnaryOpSameScalarPeelDepthLimit = 6;
inline constexpr int
    kHighCExprWriterHighCWriterVaStartUnknownOperandDepthLimit = 8;
inline constexpr int
    kHighCExprWriterHighCWriterIsCtorSourceExprRecRecDepthLimit = 8;
inline constexpr int kHighCExprWriterHighCWriterIndirectCalleeStrDepthLimit = 2;
inline constexpr int kHighCExprWriterHighCWriterCondStrImplScalarOfDepthLimit =
    6;
inline constexpr int
    kHighCExprWriterHighCWriterPreferGreaterIfElseCondPeelPeelDepthLimit = 6;
inline constexpr int
    kHighCExprWriterHighCWriterInvertCondStrImplLooksBoolDepthLimit = 8;
inline constexpr int
    kHighCExprWriterHighCWriterInvertCondStrImplPeelScalarDepthLimit = 6;

// lib/backend/c/HighC/HighCFuncWriter.cpp
inline constexpr int
    kHighCFuncWriterHighCWriterCollectFieldLoadForwardGuardAliasNoteDepthLimit =
        8;
inline constexpr int kHighCFuncWriterPeelCxxThrowDisplayDepthLimit = 8;
inline constexpr int
    kHighCFuncWriterHighCWriterFoldSignedJleCondsSameScalarPeelDepthLimit = 6;
inline constexpr int
    kHighCFuncWriterHighCWriterCollectValueForwardRegionDominatedByDepthLimit =
        64;
inline constexpr int
    kHighCFuncWriterHighCWriterCollectValueForwardCollectDepthLimit = 64;
inline constexpr int
    kHighCFuncWriterHighCWriterCollectValueForwardFoldWritesDepthLimit = 64;
inline constexpr int
    kHighCFuncWriterHighCWriterCollectValueForwardAccessesMemoryDepthLimit = 64;
inline constexpr int kHighCFuncWriterHighCWriterCollectValueForwardDepthLimit =
    64;

// lib/backend/c/HighC/HighCSourceCalls.cpp
inline constexpr int
    kHighCSourceCallsHighCWriterRenderSourceCallExprObjcMsgSendHintNilTerminatedObjectsSizeLimit =
        ::neverd::limits::ir::kMaxObjCSentinelObjects;
inline constexpr int
    kHighCSourceCallsHighCWriterRenderSourceCallExprHintByteCountLimit = 65536;
inline constexpr int
    kHighCSourceCallsHighCWriterRenderSourceCallExprSignatureParametersSizeLimit =
        ::neverd::limits::ir::kMaxSourceABIParameters;
inline constexpr int
    kHighCSourceCallsHighCWriterRenderSourceCallExprFormatAlternativeFormatAddressesSizeLimit =
        64;

// lib/backend/c/LLVMC/LLVMCEmitter.cpp
inline constexpr int kLLVMCEmitterPromoteCallResultHomesDepthLimit = 8;

// lib/backend/c/LLVMC/LLVMCExprWriter.cpp
inline constexpr int kLLVMCExprWriterLLVMCWriterImageDataVAActiveSizeLimit = 64;
inline constexpr int
    kLLVMCExprWriterLLVMCWriterDiscoverSyntheticFrameOffFrameOffDepthLimit = 8;
inline constexpr int
    kLLVMCExprWriterLLVMCWriterDiscoverSyntheticFrameScanEndMinArgument2 = 4096;
inline constexpr int kLLVMCExprWriterLLVMCWriterPeelPointerOffsetSeenSizeLimit =
    64;
inline constexpr int kLLVMCExprWriterPeelMulScaleSeenSizeLimit = 64;
inline constexpr int kLLVMCExprWriterLLVMCWriterTypedIndexAccessSeenSizeLimit =
    64;

// lib/backend/c/LLVMC/LLVMCFuncWriter.cpp
inline constexpr int
    kLLVMCFuncWriterLLVMCWriterPlanExpressionMaterializationFitsFitsDepthLimit =
        16;
inline constexpr int kLLVMCFuncWriterCollectSlotMentionsDepthLimit = 6;
inline constexpr int
    kLLVMCFuncWriterLLVMCWriterCollectOmittedInlinedImmediatesGuardLimit = 32;
inline constexpr int
    kLLVMCFuncWriterLLVMCWriterCollectOmittedUnknownsGuardLimit = 32;
inline constexpr int
    kLLVMCFuncWriterLLVMCWriterPrintedCallArgLimitKeepExtraAddressShapedDepthLimit =
        4;
inline constexpr int
    kLLVMCFuncWriterLLVMCWriterMarkRedundantPhiCopiesGuardLimit = 32;
inline constexpr int
    kLLVMCFuncWriterLLVMCWriterExclusiveSkipElseIfCollectArmInSizeLimit = 160;
inline constexpr int
    kLLVMCFuncWriterLLVMCWriterExclusiveSkipElseIfWalkDepthLimit = 5;
inline constexpr int
    kLLVMCFuncWriterLLVMCWriterRegionBeforeSkipTargetInSizeLimit = 48;

// lib/backend/c/LLVMC/LLVMCScalarExpressions.cpp
inline constexpr int
    kLLVMCScalarExpressionsLLVMCWriterScalarExpressionTextRenderDepthLimit = 32;
inline constexpr int
    kLLVMCScalarExpressionsLLVMCWriterScalarConditionalUpdateIndependentWorkLimit =
        128;

// lib/backend/c/LLVMC/LLVMCScalarRegions.cpp
inline constexpr int kLLVMCScalarRegionsNormalJoinWorkLimit = 65536;
inline constexpr int kLLVMCScalarRegionsEdgeWorkLimit = 65536;
inline constexpr int kLLVMCScalarRegionsSequenceDepthLimit = 64;
inline constexpr int kLLVMCScalarRegionsSequenceWorkLimit = 65536;
inline constexpr int kLLVMCScalarRegionsBuildFnSizeLimit = 1024;
inline constexpr int kLLVMCScalarRegionsBuildInstructionsLimit = 16384;
inline constexpr int
    kLLVMCScalarRegionsLLVMCWriterCoalesceScalarPhiNamesPhisSizeLimit = 128;
inline constexpr int
    kLLVMCScalarRegionsLLVMCWriterCoalesceScalarPhiNamesReadsWorkLimit =
        1048576;
inline constexpr int
    kLLVMCScalarRegionsLLVMCWriterCoalesceScalarPhiNamesReadsDepthLimit = 64;
inline constexpr int
    kLLVMCScalarRegionsLLVMCWriterCoalesceScalarPhiNamesWorkLimit = 1048576;
inline constexpr int
    kLLVMCScalarRegionsLLVMCWriterTryWriteScalarRegionsConfinedScopeWorkLimit =
        65536;

// lib/backend/c/LLVMC/LLVMCStmtWriter.cpp
inline constexpr int
    kLLVMCStmtWriterLLVMCWriterAllocaStoredValueBeforeStableDepthLimit = 8;
inline constexpr int kLLVMCStmtWriterLLVMCWriterWriteInstructionImplDepthLimit =
    8;
inline constexpr int
    kLLVMCStmtWriterLLVMCWriterWriteExclusiveArmCommonFromLeftSizeLimit = 64;
inline constexpr int kLLVMCStmtWriterLLVMCWriterImportCalleeCNameDepthLimit = 6;

// lib/backend/c/LLVMC/LLVMCVariableEdits.cpp
inline constexpr int kLLVMCVariableEditsProjectionNodesSizeLimit = 20000;
inline constexpr int kLLVMCVariableEditsProjectionProofWorkLimit = 2000000;
inline constexpr int kLLVMCVariableEditsProjectionSeenSizeLimit = 20000;

// lib/backend/c/pass/LLVMC/LLVMCBitTests.cpp
inline constexpr int kLLVMCBitTestsGetDepthLimit = 64;

// lib/backend/c/pass/LLVMC/LLVMCFrameAliases.h
inline constexpr int kLLVMCFrameAliasesPeelSyntheticFramesWalkWalkVisitsLimit =
    128;

// lib/backend/c/pass/LLVMC/LLVMCLoopPhases.cpp
inline constexpr int kLLVMCLoopPhasesFactorLoopPhiExpressionsFunctionSizeLimit =
    4096;

// lib/backend/c/pass/LLVMC/LLVMCScalarLoopRecovery.cpp
inline constexpr int kLLVMCScalarLoopRecoveryHasScalarLoopFSizeLimit = 1024;
inline constexpr int
    kLLVMCScalarLoopRecoveryRecoverScalarLoopsScanWorkMinArgument1 = 65536;

// lib/backend/c/render/CTypeFormat.cpp
inline constexpr unsigned kCTypeFormatDeclarationToCRemaining = 4096;

// lib/backend/c/render/X86/LLVMCIntrinsicRenderX86.cpp
inline constexpr int kLLVMCIntrinsicRenderX86SegmentedLoadDepthLimit = 4;

// lib/backend/codegen/COFF/COFFRegistrationCxx.cpp
inline constexpr int
    kCOFFRegistrationCxxGetCheckedCOFFRegistrationCxxTableReceiptMaxStateLimit =
        128;

// lib/backend/codegen/COFF/COFFRegistrationCxxStateProof.cpp
inline constexpr int
    kCOFFRegistrationCxxStateProofValidateCxxGeneratedStatesMaxStateLimit = 128;

// lib/backend/codegen/COFF/COFFRegistrationFrameBits.cpp
inline constexpr int
    kCOFFRegistrationFrameBitsTruncationMayDependOnFrameQueryActiveSizeLimit =
        128;

// lib/backend/codegen/COFF/COFFRegistrationFrameProof.cpp
inline constexpr int
    kCOFFRegistrationFrameProofValidateFramePrivacyLocateDepthLimit = 128;
inline constexpr int
    kCOFFRegistrationFrameProofValidateFramePrivacyLocateImplDepthLimit = 128;

// lib/backend/codegen/COFF/COFFRegistrationImageProof.cpp
inline constexpr int
    kCOFFRegistrationImageProofValidateSymbolicImagePointersEvaluateDepthLimit =
        128;
inline constexpr int
    kCOFFRegistrationImageProofValidateSymbolicImagePointersAddEvaluateAddValuesSizeLimit =
        32;
inline constexpr int
    kCOFFRegistrationImageProofValidateSymbolicImagePointersEvaluateNumericBudgetExceededKnownSizeLimit =
        32;

// lib/backend/codegen/COFF/COFFRegistrationSEHNullAccess.cpp
inline constexpr int
    kCOFFRegistrationSEHNullAccessBindSEHNullReadsSourceZeroDepthLimit = 32;
inline constexpr int
    kCOFFRegistrationSEHNullAccessBindSEHNullReadsIsZeroDepthLimit = 32;

// lib/backend/codegen/COFF/COFFRegistrationSEHScopeState.cpp
inline constexpr int
    kCOFFRegistrationSEHScopeStateBindDynamicSEHStateResolveDepthLimit = 32;

// lib/backend/codegen/ELF/ELFSourceDependencies.cpp
inline constexpr int kELFSourceDependenciesReadLayoutStringsShSizeLimit =
    64 * 1024 * 1024;
inline constexpr int kELFSourceDependenciesAppendStorageSizeLimit =
    256 * 1024 * 1024;
inline constexpr int kELFSourceDependenciesVerifyRelSizeLimit =
    64 * 1024 * 1024;

// lib/backend/codegen/MachO/MachODependencyState.cpp
inline constexpr int
    kMachODependencyStateCompleteMachOChainedSegmentTableCountLimit = 65536;

// lib/backend/codegen/MachO/MachOExportLayout.cpp
inline constexpr int kMachOExportLayoutReadMachOExportLayoutNameSizeLimit =
    65536;
inline constexpr int kMachOExportLayoutWriteMachOExportLayoutNodesSizeLimit =
    1000000;

// lib/backend/codegen/MachO/MachOLayoutGrowth.cpp
#define NEVERD_MACHO_FRAME_LIMIT(Type, Name, Value)                            \
  inline constexpr Type Name = Value;
#include "neverd/backend/codegen/MachO/MachOFrameRelocation.def"
inline constexpr int kMachOLayoutGrowthMoveTextSectionCapacityLimit =
    64 * 1024 * 1024;
inline constexpr int kMachOLayoutGrowthDeclareMachOTextMetadataSizeLimit =
    32 * 1024 * 1024;
inline constexpr int
    kMachOLayoutGrowthReserveMachOSourceUnwindAdditionalEHBytesLimit =
        32 * 1024 * 1024;
inline constexpr int
    kMachOLayoutGrowthReserveMachONativePatchStorageCallbackCountLimit = 65536;
inline constexpr int
    kMachOLayoutGrowthReserveMachONativePatchStorageImportFunctionCountLimit =
        4096;

// lib/backend/codegen/PatchObject.cpp
inline constexpr int kPatchObjectReadELFObjectSectionsSizeLimit = 65536;
inline constexpr int kPatchObjectReadELFObjectSymbolsSizeLimit = 1024 * 1024;

// lib/backend/codegen/SourcePatchCallbackDwarf.cpp
inline constexpr int kSourcePatchCallbackDwarfEncodeRulesRememberDepthLimit =
    64;

// lib/backend/codegen/SourcePatchCallbackMachine.cpp
inline constexpr int kSourcePatchCallbackMachineDeriveAllocateNMinArgument2 =
    256;
inline constexpr int kSourcePatchCallbackMachineDeriveAllocateMinArgument2 =
    256;

// lib/backend/codegen/SourcePatchDependencies.cpp
inline constexpr int kSourcePatchDependenciesAppendCOFFImportsSizeLimit =
    16 * 1024 * 1024;
inline constexpr int kMaxSourcePatchDependencies = 4096;
inline constexpr int
    kSourcePatchDependenciesPrepareSourcePatchDependenciesRequestsSizeLimit =
        kMaxSourcePatchDependencies;
inline constexpr int
    kSourcePatchDependenciesPrepareSourcePatchDependenciesRequestLibrarySizeLimit =
        4096;
inline constexpr int
    kSourcePatchDependenciesPrepareSourcePatchDependenciesRequestSymbolSizeLimit =
        4096;

// lib/backend/codegen/SourcePatchPreparation.cpp
inline constexpr int
    kSourcePatchPreparationPrepareSourcePatchCallbacksAvailableMinArgument1 =
        64;

// lib/backend/llvm/X86/MedLLVMRegistrationCallResult.cpp
inline constexpr int
    kMedLLVMRegistrationCallResultUnusedRegistrationCallResultPlanVisitDepthLimit =
        64;

// lib/backend/llvm/X86/MedLLVMX86GlobalData.cpp
inline constexpr int
    kMedLLVMX86GlobalDataMedLLVMEmitterI386PeeledInitStoreAddrPeelInitInductionPhiTracesToSegEntryDepthLimit =
        8;
inline constexpr int
    kMedLLVMX86GlobalDataMedLLVMEmitterI386PeeledInitStoreAddrPeelInitInductionPhiTracesToUnrollStepDepthLimit =
        8;

// lib/backend/llvm/X86/RegistrationCxxUnwind.cpp
inline constexpr int
    kRegistrationCxxUnwindProjectX86RegistrationCxxUnwindMapSizeLimit = 128;
inline constexpr int
    kRegistrationCxxUnwindProjectX86RegistrationCxxUnwindCxxTryBlocksSizeLimit =
        64;

// lib/backend/llvm/X86/WindowsRegistrationCallbackPlan.cpp
inline constexpr int kWindowsRegistrationCallbackPlanHasBlockAddressDepthLimit =
    256;
inline constexpr int
    kWindowsRegistrationCallbackPlanPrepareCallbackInspectDepthLimit = 256;

// lib/backend/llvm/data/MedLLVMFailureSnapshot.h
inline constexpr int kMedLLVMFailureSnapshotJsonSSizeLimit = 4096;
inline constexpr int kMedLLVMFailureSnapshotSeconds = 2;
inline constexpr int kMedLLVMFailureSnapshotTakeRecordsLimit = 20000;
inline constexpr int kMedLLVMFailureSnapshotTypeDepthLimit = 8;

// lib/backend/llvm/data/MedLLVMIndexedGlobal.cpp
inline constexpr int
    kMedLLVMIndexedGlobalMedLLVMEmitterTryResolveInductionGlobalPtrDepthLimit =
        8;
inline constexpr int
    kMedLLVMIndexedGlobalMedLLVMEmitterTryResolveInductionGlobalPtrLosesMappedAddressBitsDepthLimit =
        16;
inline constexpr int
    kMedLLVMIndexedGlobalMedLLVMEmitterTryResolveInductionGlobalPtrForwardedPhiDepthLimit =
        64;
inline constexpr int
    kMedLLVMIndexedGlobalMedLLVMEmitterTryResolveInductionGlobalPtrRecoverSelectionArmDepthLimit =
        16;
inline constexpr int
    kMedLLVMIndexedGlobalMedLLVMEmitterTryResolveIndexedGlobalPtrIsBoundedScalarLookupDepthLimit =
        16;

// lib/backend/llvm/data/MedLLVMLiteralTable.cpp
inline constexpr int
    kMedLLVMLiteralTableMedLLVMEmitterTryResolveSelectBaseLitTablePeelPeelDepthLimit =
        8;
inline constexpr int
    kMedLLVMLiteralTableMedLLVMEmitterTryResolveSelectBaseLitTableArmPtrDepthLimit =
        8;
inline constexpr int
    kMedLLVMLiteralTableMedLLVMEmitterTryResolveSelectMergeTableIsPureFrameAddressDepthLimit =
        16;
inline constexpr size_t kMedLLVMLiteralTableRemainingAuditNodes = 8192;
inline constexpr int
    kMedLLVMLiteralTableMedLLVMEmitterTryResolveSelectMergeTableAffineRootDepthLimit =
        16;
inline constexpr int
    kMedLLVMLiteralTableMedLLVMEmitterTryResolveSelectMergeTableConditionSetDepthLimit =
        24;
inline constexpr int
    kMedLLVMLiteralTableMedLLVMEmitterTryResolveSelectMergeTablePredicatedValueDepthLimit =
        24;
inline constexpr int
    kMedLLVMLiteralTableMedLLVMEmitterTryResolveSelectMergeTablePredicatedTableBaseDepthLimit =
        16;
inline constexpr int
    kMedLLVMLiteralTableMedLLVMEmitterTryResolveSelectMergeTablePlanPlanDepthLimit =
        32;

// lib/backend/llvm/data/MedLLVMSpillPredicate.cpp
inline constexpr int
    kMedLLVMSpillPredicateMedLLVMEmitterFrameAccessesProvenDisjointAffineStepToSeenSizeLimit =
        64;
inline constexpr int
    kMedLLVMSpillPredicateMedLLVMEmitterFrameAccessesProvenDisjointCorrelatedRangeCountDecrementLimit =
        4096;

// lib/backend/llvm/data/MedLLVMSymbolizedPtr.cpp
inline constexpr int
    kMedLLVMSymbolizedPtrMedLLVMEmitterTryResolvePointerArgIsDataTreeDepthLimit =
        16;
inline constexpr int
    kMedLLVMSymbolizedPtrMedLLVMEmitterTryResolvePointerArgBuildDataTreeDepthLimit =
        16;

// lib/backend/llvm/data/MedLLVMWritableData.cpp
inline constexpr int
    kMedLLVMWritableDataMedLLVMEmitterTryResolveWritableDataPointerAddressModelDepthLimit =
        32;

// lib/backend/llvm/resolve/MedLLVMAddrResolve.cpp
inline constexpr int
    kMedLLVMAddrResolveTraceIndexedLaneConstantTraceLaneConstImplTraceDepthLimit =
        32;
inline constexpr int
    kMedLLVMAddrResolveAnalyzeIndexedPointerLaneWalkUnconstrainedDepthLimit =
        64;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterTraceControlConstEvalEvalDepthLimit = 32;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterValueIsGuardedScalarOffsetScalarSourceDepthLimit =
        32;
inline constexpr int kMedLLVMAddrResolveRemainingFrameRootNodes = 8192;
inline constexpr int kMedLLVMAddrResolveRemainingFrameDomainReachNodes = 8192;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterValueIsStableAddressOffsetImplSummarizeFrameDomainReachDepthLimit =
        128;
inline constexpr int kMedLLVMAddrResolveRemainingScalarReachNodes = 8192;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterValueIsStableAddressOffsetImplScalarValueReachesDepthLimit =
        128;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterValueIsStableAddressOffsetImplProveClosedScalarGraphVisitPathSizeLimit =
        512;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterValueIsStableAddressOffsetImplProveUncachedDepthLimit =
        128;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterValueIsStableAddressOffsetImplProveDepthLimit =
        128;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterPureReadOnlyBaseIdentityZeroExtendedNarrowConstantIdentityDepthLimit =
        16;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterPureReadOnlyBaseIdentityDepthLimit = 32;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterPhiIncomingIsRecurrentImplProveRecurrenceDepthLimit =
        32;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterPhiIncomingIsRecurrentImplCollectAliasesDepthLimit =
        32;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterPhiHasPureForwardingCycleForwardedPhiDepthLimit =
        64;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterIsMaskedSelectOrIsBooleanValueDepthLimit =
        8;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterTraceTableBaseConstDepthLimit = 8;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterClassifyPointerTableLoadRolesDepthLimit =
        16;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterClassifyPointerTableLoadRolesRecurrenceDeltaDepthLimit =
        32;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterClassifyPointerTableLoadRolesAnalyzeAddressDepthLimit =
        32;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterClassifyPointerTableLoadRolesGuardedIterationCountDepthLimit =
        16;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterClassifyPointerTableLoadRolesGuardedIterationCountScalarConstantDepthLimit =
        16;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterRecoverAbsoluteDataPointerLoadIdentitiesDepthLimit =
        8;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterRecoverRelativeDataPointerTargetsDepthLimit =
        8;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterRecoverRelativeDataPointerTargetsRecoverOffsetLoadDepthLimit =
        8;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterCollectIndexedGlobalBaseImplDepthLimit = 8;
inline constexpr int
    kMedLLVMAddrResolveMedLLVMEmitterCollectLiteralPoolBaseDepthLimit = 8;

// lib/backend/llvm/resolve/MedLLVMCodePtrResolve.cpp
inline constexpr int
    kMedLLVMCodePtrResolveMedLLVMEmitterTryResolveCodePtrTablePtrProveAddressRoleDepthLimit =
        128;
inline constexpr int
    kMedLLVMCodePtrResolveMedLLVMEmitterTryResolveCodeAddressValueExactFunctionMaterializationHasOccurrenceEvidenceDepthLimit =
        32;

// lib/backend/llvm/resolve/MedLLVMFrameResolve.cpp
inline constexpr int
    kMedLLVMFrameResolveMedLLVMEmitterCanonicalFrameSlotKeyAffineDeltaToDepthLimit =
        64;

// lib/backend/llvm/resolve/MedLLVMScalarProof.h
inline constexpr int
    kMedLLVMScalarProofProvesUnsignedMagicRemainderScaleFactorDepthLimit = 8;

// lib/backend/llvm/resolve/MedLLVMVarAccess.cpp
inline constexpr int kMedLLVMVarAccessMedLLVMEmitterGetVarVSizeSlotLimit = 64;

// lib/backend/swift/HighSwiftEmitter.cpp
inline constexpr int kHighSwiftEmitterSwiftTypeDepthLimit = 8;
inline constexpr int kHighSwiftEmitterIsSelfDepthLimit = 200;
inline constexpr int kHighSwiftEmitterContainsSelfDepthLimit = 200;
inline constexpr int kHighSwiftEmitterExpressionDepthLimit = 200;
inline constexpr int kHighSwiftEmitterBlockDepthLimit = 200;

// lib/backend/c/HighC/HighCExprWriter.cpp
inline constexpr int
    kHighCExprWriterHighCWriterCollectUnknownOnlyNamesGuardLimit = 8;
inline constexpr int
    kHighCExprWriterHighCWriterCollectCtorSourceNamesGuardLimit = 8;

// lib/backend/c/HighC/X86/HighCCallSetup.cpp
inline constexpr int kHighCCallSetupWalkRunSizeLimit = 8;

// lib/backend/c/LLVMC/LLVMCFuncWriter.cpp
inline constexpr int kLLVMCFuncWriterLLVMCWriterCollectTypedHomesGuardLimit = 6;
inline constexpr int kLLVMCFuncWriterLLVMCWriterSingleEntryUncondTailStepLimit =
    4;
inline constexpr int kLLVMCFuncWriterLLVMCWriterAssignSelectElseIfStepLimit = 3;
inline constexpr int kLLVMCFuncWriterAttemptCondsSizeLimit = 4;
inline constexpr int kLLVMCFuncWriterStraightReturnChainOutSizeLimit = 4;
inline constexpr int
    kLLVMCFuncWriterLLVMCWriterExclusiveSkipElseIfDoneHeadersSizeLimit = 4;
inline constexpr int
    kLLVMCFuncWriterLLVMCWriterExclusiveSkipElseIfCondsSizeLimit = 4;
inline constexpr int kLLVMCFuncWriterLLVMCWriterPrivateJoinElseIfStepLimit = 4;
inline constexpr int
    kLLVMCFuncWriterLLVMCWriterRegionBeforeSkipTargetSkipChainSizeLimit = 4;

// lib/backend/c/LLVMC/LLVMCScalarRegions.cpp
inline constexpr int kLLVMCScalarRegionsChainTestsSizeLimit = 15;

// lib/backend/c/LLVMC/LLVMCStmtWriter.cpp
inline constexpr int kLLVMCStmtWriterLLVMCWriterLeftoverPackedCallResultILimit =
    12;
inline constexpr int kLLVMCStmtWriterLLVMCWriterPriorUnknownStoreToLookedLimit =
    12;
inline constexpr int
    kLLVMCStmtWriterLLVMCWriterStoreIsLeftoverHomeUndefLookedLimit = 12;
inline constexpr int
    kLLVMCStmtWriterLLVMCWriterCollectLeftoverNarrowCallStoresLookedLimit = 32;
inline constexpr int
    kLLVMCStmtWriterLLVMCWriterImpliedPointerSuccessorStepLimit = 16;
inline constexpr int kLLVMCStmtWriterPrintReturnChainChainSizeLimit = 4;
inline constexpr int kLLVMCStmtWriterPrintReturnChainBackSizeLimit = 8;

// lib/backend/c/pass/HighC/HighCVoidAnalysis.cpp
inline constexpr int kHighCVoidAnalysisAnalyzeUnusedAssignsGuardLimit = 8;

// lib/backend/c/pass/LLVMC/LLVMCDeadStoreAnalysis.cpp
inline constexpr int kLLVMCDeadStoreAnalysisAnalyzeDeadFrameStoresGuardLimit =
    8;

// lib/backend/codegen/CompiledImage.cpp
inline constexpr int kCompiledImageCompileImageForPatchImplIterLimit = 8;

// lib/backend/codegen/MachO/MachOExportLayout.cpp
inline constexpr int kMachOExportLayoutWriteMachOExportLayoutPassLimit = 64;

// lib/backend/llvm/data/MedLLVMFailureSnapshot.h
inline constexpr int kMedLLVMFailureSnapshotScalarOffsetRejectionSlotLimit = 64;
inline constexpr int kMedLLVMFailureSnapshotCaptureAttemptLimit = 64;

// lib/backend/llvm/data/MedLLVMIndexedGlobal.cpp
inline constexpr int kMedLLVMIndexedGlobalLoadsFromDataPtrTableDLimit = 8;
inline constexpr int
    kMedLLVMIndexedGlobalMedLLVMEmitterTryResolveRuntimeIndexedReadOnlyTablePeelLimit =
        8;

// lib/backend/llvm/data/MedLLVMSpillPredicate.cpp
inline constexpr int kMedLLVMSpillPredicateRecurrenceEdgeExcludesGuardLimit = 8;

// lib/backend/llvm/resolve/MedLLVMCodePtrResolve.cpp
inline constexpr int
    kMedLLVMCodePtrResolveMedLLVMEmitterJumpTableForLoadStepLimit = 8;
inline constexpr int kMedLLVMCodePtrResolveReloadRecursStepLimit = 16;

// lib/backend/llvm/data/MedLLVMSpillPredicate.cpp
inline constexpr int kMaxSpillPredicateDepth = 64;

// lib/backend/c/HighC/HighCWindowsThrow.cpp
inline constexpr int kMaxNullThrowArgumentDepth = 8;

// lib/backend/llvm/emit/MedLLVMCall.cpp
inline constexpr int kMaxConstantCallIdentityDepth = 8;

// lib/backend/llvm/emit/MedLLVMEmitter.cpp
inline constexpr int kMaxReturnedBlockAddressDepth = 4;

// lib/backend/llvm/resolve/MedLLVMAddrResolve.cpp
inline constexpr int kMaxGuardedScalarOffsetDepth = 16;

// lib/backend/c/LLVMC/LLVMCEmitter.cpp
inline constexpr int kMaxLLVMCNamePropagationRounds = 8;

// lib/backend/codegen/ELF/ELFSourceDependencies.cpp
inline constexpr int kMaxELFDependencyDynamicBytes = 1024 * 1024;

// lib/backend/codegen/ELF/ELFSourceDependencies.cpp
inline constexpr int kMaxELFDependencySymbols = 1024 * 1024;

// lib/backend/codegen/ELF/ELFSourceDependencies.cpp
inline constexpr int kMaxELFDependencyRelocationBytes = 64 * 1024 * 1024;

// lib/backend/codegen/SourcePatchPreparation.cpp
inline constexpr int kMaxSourcePatchPrefixBytes = 1024 * 1024;

// lib/backend/codegen/PatchObject.cpp
inline constexpr int kMaxPatchObjectAlignment = 1024 * 1024;

// lib/backend/codegen/MachO/MachOHeaderGrowth.cpp
inline constexpr int kMaxMachOHeaderGrowthBytes = 1024 * 1024;

// lib/backend/codegen/MachO/MachOLayoutGrowth.cpp
inline constexpr int kMaxMachOAdditionalUnwindFunctions = 65536;

// lib/backend/codegen/MachO/MachOLayoutGrowth.cpp
inline constexpr int kMaxMachONativePatchFrames = 32 * 1024 * 1024;

// lib/backend/codegen/COFF/COFFPatchObject.cpp
inline constexpr int kMaxCOFFPatchObjectSymbols = 1024 * 1024;

// lib/backend/codegen/SourcePatchCallbackMachine.cpp
inline constexpr int kMaxSourceCallbackCopyBytes = 1024 * 1024;

// lib/backend/llvm/emit/MedLLVMEmitter.cpp
inline constexpr int kMaxUnknownFrameDisplacement = 65536;

// lib/backend/llvm/data/MedLLVMFailureSnapshot.h
inline constexpr int kMaxFailureSnapshotRootBytes = 4096;

} // namespace neverd::limits::backend

#endif // NEVERD_BACKEND_LIMITS_H
