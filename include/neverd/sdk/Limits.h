//===- Limits.h - SDK resource limits and defaults ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SDK request bounds, retained-state limits and default work budgets.
/// Keep SDK policy values here; binary layouts and protocol constants belong
/// to their owning models. Shared pipeline limits live in neverd/Limits.h.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_LIMITS_H
#define NEVERD_SDK_LIMITS_H

#include "neverd/backend/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/loader/Limits.h"
#include "neverd/support/Limits.h"

#include <cstddef>
#include <cstdint>

namespace neverd::sdk {

// Session-owned analysis image additions.
inline constexpr size_t kMaxAnalysisSectionBytes = 64u << 20;
inline constexpr size_t kMaxAnalysisSections = 128;
inline constexpr size_t kMaxAnalysisSectionStateBytes = 144u << 20;
inline constexpr size_t kMaxAnalysisSectionDescriptionBytes = 1u << 20;
inline constexpr unsigned kMaxAnalysisSectionJsonDepth = 64;
inline constexpr size_t kMaxAnalysisSectionNameBytes = 512;
inline constexpr size_t kMaxAnalysisSectionFunctions = 4096;

/// Lines admitted by one C/IR view page, shared with transport adapters.
inline constexpr int kMaxCodeViewPageLines = 2048;
inline constexpr int kDecodeBenchmarkMaxRepetitions = 100;
/// Maximum wait between cancellation probes while joining an owned discovery.
inline constexpr unsigned kFunctionDiscoveryPollMilliseconds = 5;

// Query paging and decoding.

/// Instructions read from a function without a recorded size. Bounds a
/// corrupt or missing extent, including the function's reachable jump paths.
inline constexpr uint64_t kMaxUnsizedInstructions = 65536;
inline constexpr int kMaxFunctionsPerPage = 4096;
inline constexpr int kMaxSwitchFunctionsPerPage = 4096;
inline constexpr int kMaxDataSlotsPerQuery = 65536;
inline constexpr int kMaxSourceEntriesPerQuery = 65536;
inline constexpr int kMaxStringRowsPerPage = 65536;
inline constexpr int kMaxDecodedTextBytes = 65536;
inline constexpr size_t kStringSearchWindowBytes = 65536;
inline constexpr unsigned kDefaultStringMinLength =
    ::neverd::limits::support::kDefaultStringMinLength;
inline constexpr size_t kShortInstructionByteColumnWidth = 18;
inline constexpr int kHexDumpBytesPerLine = 16;
inline constexpr size_t kMaxSearchOptionsBytes = 32768;
inline constexpr size_t kMaxSearchPatternBytes = 4096;
inline constexpr int64_t kDefaultSearchResults = 256;
inline constexpr int64_t kMaxSearchResults = 4096;
inline constexpr uint64_t kMaxOwnedResourceStringBytes = 64 * 1024 * 1024;

// Call paths and trees.

inline constexpr unsigned kMaxCallPathDepth = 64;
inline constexpr unsigned kDefaultCallPathDepth = 16;
inline constexpr unsigned kDefaultCallTreeDepth = 1;
inline constexpr unsigned kMaxCallPaths = 1000;
inline constexpr unsigned kDefaultCallPaths = 32;
inline constexpr unsigned kMaxCallGraphListed = 100000;
inline constexpr unsigned kDefaultCallGraphListed = 4096;
inline constexpr unsigned kMaxCallPathWaypoints = 64;
/// Steps before one segment's simple-path enumeration reports incomplete.
inline constexpr uint64_t kCallPathStepBudget = 4'000'000;

// Local types and persisted user edits.

inline constexpr size_t kMaxTypeFitAccesses = 4096;
inline constexpr size_t kMaxTypeFitTextBytes = 256;
inline constexpr int64_t kMaxTypeFitCandidates = 500;
inline constexpr int64_t kMaxTypeFitAccessBytes = 1 << 16;
inline constexpr unsigned kMaxLocalTypeConversionDepth = 16;
/// Admission and persistence use the same bound for function declarations.
inline constexpr size_t kMaxFunctionDeclarationBytes = 4096;
inline constexpr size_t kMaxUserNameBytes = 4096;
inline constexpr int kMaxFormattedOperands = 8;
inline constexpr size_t kMaxOperandTypeNameBytes = 512;
inline constexpr size_t kMaxCallSnapshotOperations = 200000;
inline constexpr uint64_t kMaxCallSnapshotBytes = 16 * 1024 * 1024;
inline constexpr size_t kMaxCallVariadicTypes = 32;
inline constexpr size_t kMaxSourceOptionsBytes = 256 * 1024;
inline constexpr size_t kMaxSourceEditIdBytes = 256;
inline constexpr size_t kMaxSourceSnapshotBytes = 128;
inline constexpr size_t kMaxSourceVariableTypes = 256;
inline constexpr size_t kMaxSourceVariableAssociations = 64;
inline constexpr size_t kMaxSourceCallEdits = 32;
inline constexpr size_t kMaxSourceExpressionSnapshotNodes = 100000;
inline constexpr uint64_t kMaxSourceExpressionSnapshotBytes = 16 * 1024 * 1024;
inline constexpr size_t kMaxStackFramePoints = 256;
inline constexpr size_t kMaxStackFrameVariables = 256;
inline constexpr size_t kMaxStackVariableNameBytes = 128;
inline constexpr size_t kMaxStackFrameOptionsBytes = 128 * 1024;
inline constexpr size_t kMaxRegisterFlowOptionsBytes = 4096;
inline constexpr size_t kMaxRegisterFlowBlocks = 4096;
inline constexpr size_t kMaxRegisterFlowInstructions = 20000;
inline constexpr size_t kMaxEnumTypeNameBytes = 512;
inline constexpr size_t kMaxEnumValues = 4096;
inline constexpr size_t kMaxSwiftMethodSelections = 65536;
inline constexpr unsigned kMaxSourcePatchEntryPadding = 1024;

// Retained views and diagnostic output.

inline constexpr size_t kMaxFunctionLlvmModules = 8;
inline constexpr size_t kMaxFunctionSources = 8;
inline constexpr size_t kMaxSourceProjectionDiagnostics = 4096;
inline constexpr size_t kMaxNativeSourceDependencyCalls = 1000000;
inline constexpr size_t kMaxLLVMCommentSections = 6;
inline constexpr size_t kMaxLLVMCommentReaches = 16;
inline constexpr size_t kLLVMCommentWidth = 78;
inline constexpr size_t kDebugFileSampleNames = 8;

// Symbolic C API defaults.

inline constexpr unsigned kDefaultSymbolicWidth = 32;
inline constexpr unsigned kDefaultOptimizationRounds = 8;
/// The C surface is bounded by default even when the C++ solver is not.
/// Callers may explicitly select exhaustive work through their options.
inline constexpr uint64_t kDefaultSolverMaxConflicts = uint64_t{1} << 18;
inline constexpr uint64_t kDefaultSolverMaxPropagations = uint64_t{1} << 24;
inline constexpr uint64_t kDefaultSolverMaxWatchVisits = uint64_t{1} << 26;
inline constexpr uint32_t kDefaultSynthesisTimeoutMs = 1000;

// Incremental immediate search.

inline constexpr size_t kImmediateSearchStepInstructions = 4096;
inline constexpr size_t kImmediateSearchStepBytes = 65536;
inline constexpr size_t kMaxImmediateSearchGaps = 64;
inline constexpr size_t kMaxImmediateSearchOperands = 2000000;
inline constexpr uint64_t kMaxImmediateSearchCodeBytes = 512ULL * 1024 * 1024;

// Decode benchmark sampling.

/// Cap timed input so large images still give a quick representative probe.
inline constexpr uint64_t kDecodeBenchmarkBytes = 16ull << 20;
inline constexpr size_t kDecodeBenchmarkChunkBytes = 64ull << 10;

// Native source projection.

/// HighIR expression visits in ordinary, evaluated and bounded value walks.
inline constexpr size_t kSourceExpressionWork = 100000;
inline constexpr size_t kSourceEvaluationWork = 1000000;
inline constexpr size_t kSourceValueWork = 4096;
inline constexpr size_t kSourceSliceWork = 128;
inline constexpr size_t kSourceFrameWork = 10000;
inline constexpr size_t kSourceConstructorWork = 262144;
inline constexpr size_t kSourceSetterWork = 1U << 18;
inline constexpr size_t kSwiftOnceSliceWork = 1024;
inline constexpr size_t kSwiftOnceValueWork = 256;
inline constexpr size_t kSourceBindingSmallWork = 256;
inline constexpr size_t kSourceBindingCallWork = 1000;
inline constexpr size_t kSourceBindingOccurrenceWork = 16384;
inline constexpr size_t kSourceBindingFieldWork = 65536;
inline constexpr size_t kSourceMetadataScanWork = 1000000;
inline constexpr size_t kMaxBlockSourceWork = 4000000;
inline constexpr size_t kMaxBlockSourceFacts = 262144;
inline constexpr size_t kMaxSourceBindingExports = 262144;
inline constexpr size_t kMaxSourceBindingDescriptors = 8;

/// Swift mangling reader profiles for a short identity and a full binding.
inline constexpr size_t kSwiftIdentityInputBytes =
    ::neverd::limits::loader::kSwiftIdentityInputBytes;
inline constexpr size_t kSwiftIdentityNodes =
    ::neverd::limits::loader::kSwiftIdentityNodes;
inline constexpr unsigned kSwiftIdentityDepth =
    ::neverd::limits::loader::kSwiftIdentityDepth;
inline constexpr size_t kSwiftIdentityMemoryBytes =
    ::neverd::limits::loader::kSwiftIdentityMemoryBytes;
inline constexpr size_t kSwiftIdentityOperations =
    ::neverd::limits::loader::kSwiftIdentityOperations;
inline constexpr size_t kSwiftBindingInputBytes =
    ::neverd::limits::loader::kSwiftBindingInputBytes;
inline constexpr size_t kSwiftBindingNodes =
    ::neverd::limits::loader::kSwiftBindingNodes;
inline constexpr unsigned kSwiftBindingDepth =
    ::neverd::limits::loader::kSwiftBindingDepth;
inline constexpr size_t kSwiftBindingMemoryBytes =
    ::neverd::limits::loader::kSwiftBindingMemoryBytes;
inline constexpr size_t kSwiftBindingOperations =
    ::neverd::limits::loader::kSwiftBindingOperations;

// lib/sdk/SourcePatchCompiler.cpp
inline constexpr int kDefaultSourcePatchTimeoutSeconds = 30;

// lib/sdk/capi/FunctionAuditReason.h
inline constexpr int kFunctionAuditAddressPreviewCount = 4;

// lib/sdk/capi/NeverDCAPI.cpp
inline constexpr int kNeverDCAPINeverdSessionInspectPdbJsonTextSizeLimit =
    32 * 1024 * 1024;

// lib/sdk/capi/NeverDCAPIBenchDecode.cpp
inline constexpr double kDecodeBenchmarkDurationMilliseconds = 200.0;

// lib/sdk/capi/NeverDCAPIDecompile.cpp
inline constexpr int kNeverDCAPIDecompileWriteSourcePageFullSizeLimit =
    32 * 1024 * 1024;
inline constexpr int kNeverDCAPIDecompileWriteSourcePageCountLimit = 4096;
inline constexpr int kNeverDCAPIDecompileReadSourceOptionsPassesSizeLimit = 4;
inline constexpr int kNeverDCAPIDecompileNeverdIrViewExJsonLimit =
    kMaxCodeViewPageLines;
inline constexpr int kNeverDCAPIDecompileNeverdIrViewJsonLimit =
    kMaxCodeViewPageLines;

// lib/sdk/capi/NeverDCAPIDevirtualize.cpp
inline constexpr int kMaxSourceProjectionDepth = 200;
inline constexpr int
    kNeverDCAPIDevirtualizeSourceOptionsControlFrameSlotCountLimit = 64;

// lib/sdk/capi/NeverDCAPIDisasm.cpp
inline constexpr int kNeverDCAPIDisasmNeverdDisasmJsonExLimitDefault = 256;
inline constexpr int kMaxFunctionDisasmPage = 4096;

// lib/sdk/capi/NeverDCAPIInfo.cpp
inline constexpr int kNeverDCAPIInfoNeverdResolveAddrLenLimit = 256;

// lib/sdk/capi/NeverDCAPIObjC.cpp
inline constexpr int kNeverDCAPIObjCObjcMethodsJSONDepthLimit = 16;

// lib/sdk/capi/NeverDCAPIPersist.cpp
inline constexpr int kMaxDataItemArrayElements = 1048576;
inline constexpr int kNeverDCAPIPersistParseDataItemArrayTypeItemTypeSizeLimit =
    512;
inline constexpr int kMaxDataItemAlignment = 1048576;

// lib/sdk/capi/NeverDCAPIRegisterFlow.cpp
inline constexpr int kNeverDCAPIRegisterFlowNeverdRegisterFlowJsonCountLimit =
    200000;

// lib/sdk/capi/NeverDCAPISourceEdits.cpp
inline constexpr int
    kNeverDCAPISourceEditsNeverdSdkHighVariableSnapshotStatementsLimit = 100000;
inline constexpr int
    kNeverDCAPISourceEditsNeverdSourceTypeJsonRequestTextSizeLimit = 32 * 1024;

// lib/sdk/capi/NeverDCAPISourcePatch.cpp
inline constexpr int kMaxSourcePatchInputBytes = 64 * 1024 * 1024;
inline constexpr int kMaxSourcePatchMappings = 65536;
inline constexpr int kMaxSourcePatchCompilerArguments = 256;
inline constexpr int kMaxSourcePatchBindings = 65536;
inline constexpr int kMaxSourcePatchDependencies =
    ::neverd::limits::backend::kMaxSourcePatchDependencies;
inline constexpr int kMaxSourcePatchTimeoutSeconds = 3600;

// lib/sdk/capi/NeverDCAPISwift.cpp
inline constexpr int kNeverDCAPISwiftNeverdSwiftMethodsJsonInputSizeLimit =
    32 * 1024 * 1024;

// lib/sdk/capi/ObjCBlockSources.h
inline constexpr int kObjCBlockSourcesEvalDepthLimit = 128;
inline constexpr int kObjCBlockSourcesNoEscapeActiveSizeLimit = 16;
inline constexpr int
    kObjCBlockSourcesStackBlocksBorrowsDisjointValueCopyStateCallBorrowsDisjointValueCopyCopyBytesLimit =
        4096;
inline constexpr int kObjCBlockSourcesDiscoverObjCBlockSourcesDepthLimit = 16;

// lib/sdk/capi/ObjCByValueCopySources.h
inline constexpr int kObjCByValueCopySourcesSameExpressionDepthLimit = 64;
inline constexpr int
    kObjCByValueCopySourcesReplayCurrentFunctionISecondDecodedInstructionCountLimit =
        1024;
inline constexpr int
    kObjCByValueCopySourcesReplayCurrentFunctionOptionsOnlyFunctionEntriesSizeLimit =
        32;

// lib/sdk/capi/ObjCConstantObjectSources.h
inline constexpr int
    kObjCConstantObjectSourcesRenderObjCConstantObjectHelpersObjectsSizeLimit =
        4096;

// lib/sdk/capi/ObjCForwardedInitializerSources.h
inline constexpr int
    kObjCForwardedInitializerSourcesCompleteCallerLowLowBlocksSizeLimit = 8;
inline constexpr int
    kObjCForwardedInitializerSourcesCompleteCallerLowBlockOpsSizeLimit = 256;

// lib/sdk/capi/ObjCImmutableStringCallbackSources.h
inline constexpr int
    kObjCImmutableStringCallbackSourcesSameExpressionDepthLimit = 12;

// lib/sdk/capi/ObjCNativeSwiftReceiverSources.h
inline constexpr int kObjCNativeSwiftReceiverSourcesResolveDepthLimit = 64;
inline constexpr int kObjCNativeSwiftReceiverSourcesReceiverDepthLimit = 64;
inline constexpr int kObjCNativeSwiftReceiverSourcesSameArgumentDepthLimit = 64;

// lib/sdk/capi/ObjCProfileStorage.h
inline constexpr int kObjCProfileStorageDATALlvmPrfCntsSectionSizeLimit =
    16 * 1024 * 1024;

// lib/sdk/capi/ObjCReadOnlyScalarSources.h
inline constexpr int kObjCReadOnlyScalarSourcesLoadPlanConstantConstValLimit =
    65536;
inline constexpr int kObjCReadOnlyScalarSourcesLoadPlanScaleConstValLimit =
    65536;
inline constexpr int
    kObjCReadOnlyScalarSourcesReadOnlyLoadPlansQueriesSizeLimit = 128;
inline constexpr int kMaxReadOnlyScalarEntries = 4096;
inline constexpr int kMaxReadOnlyScalarBytes = 65536;
inline constexpr int kObjCReadOnlyScalarSourcesReadOnlyLoadPlansPExtentLimit =
    65536;
inline constexpr int
    kObjCReadOnlyScalarSourcesReadOnlyLoopBytePlansConstantDepthLimit = 4;
inline constexpr int
    kObjCReadOnlyScalarSourcesReadOnlyLoopBytePlansInspectBodySizeLimit =
        100000;
inline constexpr int
    kObjCReadOnlyScalarSourcesReadOnlyLoopBytePlansInspectIterationsLimit =
        1365;

// lib/sdk/capi/ObjCResumeSource.h
inline constexpr int
    kObjCResumeSourceProveObjCResumeOnlySourceEHCodeRangeEnd4FunctionEntryLimit =
        16384;
inline constexpr int
    kObjCResumeSourceProveObjCResumeOnlySourceEHItaniumCallSitesSizeLimit = 129;

// lib/sdk/capi/ObjCSentinelStack.h
inline constexpr int
    kObjCSentinelStackSentinelPrivateStackLoadsReadReadDepthLimit = 64;

// lib/sdk/capi/ObjCSourceBindings.h
inline constexpr int
    kObjCSourceBindingsFormatAddressesFormatAlternativeFormatAddressesSizeLimit =
        64;
inline constexpr int kObjCSourceBindingsProvenSourcePointerValueDepthLimit = 64;
inline constexpr int kObjCSourceBindingsMayCarryPrivateFrameAddressDepthLimit =
    64;
inline constexpr int
    kObjCSourceBindingsProvenPrivateFramePointerLoadsCollectDepthLimit = 64;
inline constexpr int
    kObjCSourceBindingsProvenPrivateFramePointerLoadsCandidatesSizeLimit = 16;
inline constexpr int
    kObjCSourceBindingsProvenPrivateFramePointerLoadsGraphNodesSizeLimit =
        100000;
inline constexpr unsigned
    kObjCSourceBindingsProvenPrivateFramePointerLoadsSafeEnumerationCountLimit =
        (1u << 16);
inline constexpr int
    kObjCSourceBindingsProvenFastEnumerationObjectLoadsCollectDepthLimit = 64;
inline constexpr int
    kObjCSourceBindingsProvenFastEnumerationObjectLoadsCandidatesSizeLimit = 8;
inline constexpr int
    kObjCSourceBindingsProvenFastEnumerationObjectLoadsGraphNodesSizeLimit =
        100000;
inline constexpr unsigned
    kObjCSourceBindingsProvenFastEnumerationObjectLoadsValidEnumerationCountLimit =
        (1u << 16);
inline constexpr unsigned
    kObjCSourceBindingsProvenFastEnumerationObjectLoadsSafeDictionaryCountLimit =
        (1u << 16);
inline constexpr int
    kObjCSourceBindingsProvenBlockClassCaptureLoadsFromCaptureDepthLimit = 64;
inline constexpr int
    kObjCSourceBindingsProvenBlockClassCaptureLoadsCollectDepthLimit = 64;
inline constexpr int
    kObjCSourceBindingsProvenBlockClassCaptureLoadsCandidatesSizeLimit = 16;
inline constexpr int
    kObjCSourceBindingsProvenBlockClassCaptureLoadsGraphNodesSizeLimit = 100000;
inline constexpr int
    kObjCSourceBindingsProvenBlockClassCaptureLoadsContextDerivedDepthLimit =
        64;
inline constexpr int kObjCSourceBindingsProvenSourceInteger64ValueDepthLimit =
    64;
inline constexpr int
    kObjCSourceBindingsProvenLocalizedFormatInteger64TypeLiteralAddressDepthLimit =
        64;
inline constexpr int
    kObjCSourceBindingsProvenLocalizedFormatInteger64TypeResolveDepthLimit = 64;
inline constexpr int kObjCSourceBindingsConstantBorrowedByteCountDepthLimit =
    16;
inline constexpr int
    kObjCSourceBindingsConstantBorrowedByteCountEvaluateCurrentDepthLimit = 16;
inline constexpr int kObjCSourceBindingsConstantBorrowedByteCountLimit =
    1024 * 1024;
inline constexpr int
    kObjCSourceBindingsConstantObjectTableEntriesByteCountLimit = 65536;
inline constexpr int
    kObjCSourceBindingsSwiftExportedNominalDescriptorValidDeclarationEnumClassStructureDepthLimit =
        8;
inline constexpr int
    kObjCSourceBindingsSwiftSingletonDescriptorForwardedByNativeHelperSwiftGetSingletonMetadataDepthLimit =
        4;
inline constexpr int
    kObjCSourceBindingsSwiftImportedTypedefDescriptorMatchesComponentILimit =
        256;
inline constexpr int
    kObjCSourceBindingsSwiftPrivateDescriptorFieldPathCountLimit = 4096;
inline constexpr int
    kObjCSourceBindingsSwiftDescriptorTypeRecipeMatchesReadLabelLengthLimit =
        63;
inline constexpr int
    kObjCSourceBindingsSwiftDescriptorTypeRecipeMatchesSameSameDepthLimit = 64;
inline constexpr int kObjCSourceBindingsSwiftTypeMetadataPairProofLengthLimit =
    128;
inline constexpr int
    kObjCSourceBindingsSwiftTypeMetadataPairProofRebuiltSizeLimit = 256;
inline constexpr int
    kObjCSourceBindingsSwiftForwardedTypeMetadataPairsScanScanDepthLimit = 64;
inline constexpr int
    kObjCSourceBindingsSwiftLocalWitnessGlobalsSameTypeDepthLimit = 64;
inline constexpr int
    kObjCSourceBindingsSwiftWitnessCacheAddressHintResolveDepthLimit = 64;
inline constexpr int
    kObjCSourceBindingsSwiftStaticOptionalSelfStorageHintComponentLengthLimit =
        1024;
inline constexpr int
    kObjCSourceBindingsSwiftStaticStringStorageSymbolIdentifierEnumClassStructureDepthLimit =
        8;
inline constexpr int
    kObjCSourceBindingsSwiftInlineSmallStringStorePairSameAddressDepthLimit = 4;
inline constexpr int kObjCSourceBindingsSwiftStaticStringStorageHintCountLimit =
    32;
inline constexpr int
    kObjCSourceBindingsSwiftStaticRootObjectUseFunctionBodySizeLimit = 128;
inline constexpr int
    kObjCSourceBindingsLocalStorageAccessHintAddressBaseAddrLimit = 1024 * 1024;
inline constexpr int kObjCSourceBindingsExactParameterValueDepthLimit = 8;
inline constexpr int kObjCSourceBindingsSelectedStorageSeedsValuesSizeLimit =
    64;
inline constexpr int
    kObjCSourceBindingsSelectedStorageSeedsContainsVarDepthLimit = 64;
inline constexpr int kObjCSourceBindingsSelectedStorageSeedsOffsetDepthLimit =
    16;
inline constexpr int
    kObjCSourceBindingsSelectedStorageSeedsOffsetConstantConstValLimit = 4096;
inline constexpr int
    kObjCSourceBindingsSelectedStorageSeedsSafeExpressionDepthLimit = 64;
inline constexpr int
    kObjCSourceBindingsNativeScalarStorageArgumentExtentAnalyzeCallDepthLimit =
        32;
inline constexpr int
    kObjCSourceBindingsClassReferenceAddressHintFoundSecondNameSizeLimit = 1024;
inline constexpr int
    kObjCSourceBindingsSwiftPrivateWitnessIdentityIdentifierNodeTextSizeLimit =
        128;
inline constexpr int
    kObjCSourceBindingsSwiftPrivateWitnessIdentityRecordsSize4Limit = 65536;
inline constexpr int
    kObjCSourceBindingsConstantAddressIntegerCurrentDepthLimit = 128;
inline constexpr int kObjCSourceBindingsConstantAddressDepthLimit = 128;
inline constexpr int kObjCSourceBindingsMergedIvarOffsetPlansResolveDepthLimit =
    64;
inline constexpr int kObjCSourceBindingsMergedIvarOffsetPlansVisitDepthLimit =
    128;
inline constexpr int
    kObjCSourceBindingsSwiftLiteralStorageWordEvaluateDepthLimit = 16;
inline constexpr int
    kObjCSourceBindingsSwiftWitnessUndefDescriptorGraphNodesSizeLimit = 4096;
inline constexpr int
    kObjCSourceBindingsSwiftWitnessUndefDescriptorScanScanDepthLimit = 64;
inline constexpr int
    kObjCSourceBindingsSwiftWitnessUndefDescriptorResolveDepthLimit = 64;
inline constexpr int
    kObjCSourceBindingsBindObjCSourceReferencesMetadataScanCompleteMetadataCoordinatesSizeLimit =
        64;
inline constexpr int kObjCSourceBindingsBindObjCSourceReferencesWorkSizeLimit =
    16384;
inline constexpr int
    kObjCSourceBindingsBindObjCSourceReferencesScanFormatsTraceDepthLimit = 64;
inline constexpr int
    kObjCSourceBindingsBindObjCSourceReferencesPureConstantCopyPureConstantDepthLimit =
        16;
inline constexpr int
    kObjCSourceBindingsObjcSourceCallBoundGlobalAddressDepthLimit = 64;
inline constexpr int
    kObjCSourceBindingsObjcSourceCallBoundObjectIdentityDepthLimit = 64;
inline constexpr int kObjCSourceBindingsObjcSourceCallBoundMatchesDepthLimit =
    64;
inline constexpr int
    kObjCSourceBindingsObjcSourceCallBoundMatchesCandidateDepthLimit = 64;
inline constexpr int kObjCSourceBindingsObjcSourceCallBoundDepthLimit = 32;
inline constexpr int kObjCSourceBindingsObjcSourceCallBoundCanonicalDepthLimit =
    16;

// lib/sdk/capi/ObjCSourceInputs.h
inline constexpr int
    kObjCSourceInputsSnapshotObjCEntryInputsFunctionBodySizeLimit = 256;
inline constexpr int
    kObjCSourceInputsSnapshotObjCEntryInputsScanScanDepthLimit = 32;

// lib/sdk/capi/ObjCSourceProjection.h
inline constexpr int kMaxSourceProjectionParameters =
    ::neverd::limits::ir::kMaxSourceABIParameters;
inline constexpr size_t kMaxSourceProjectionParametersSize =
    static_cast<size_t>(::neverd::limits::ir::kMaxSourceABIParameters);

// lib/sdk/capi/ObjCSuperGetterSources.h
inline constexpr int kObjCSuperGetterSourcesCompleteLowBlockOpsSizeLimit = 256;

// lib/sdk/capi/ObjCSwiftOnceSources.h
inline constexpr int kObjCSwiftOnceSourcesParameterDepthLimit = 16;
inline constexpr int kObjCSwiftOnceSourcesUnknownScalarViewDepthLimit = 16;
inline constexpr int
    kObjCSwiftOnceSourcesDiscardableNativeContextReadDepthLimit = 16;
inline constexpr int
    kObjCSwiftOnceSourcesSharedOnceTailViewFunctionBodySizeLimit = 32;
inline constexpr int
    kObjCSwiftOnceSourcesSharedOnceTailViewTailTailBodySizeLimit = 16;
inline constexpr int
    kObjCSwiftOnceSourcesObjcClassMetadataAccessorContractPlainDepthLimit = 16;
inline constexpr int kObjCSwiftOnceSourcesGetterContractAliasOriginDepthLimit =
    64;
inline constexpr int kObjCSwiftOnceSourcesGetterContractLoadOriginDepthLimit =
    64;
inline constexpr int kObjCSwiftOnceSourcesGetterContractScanScanDepthLimit =
    128;
inline constexpr int kObjCSwiftOnceSourcesAddressorContractPlainDepthLimit = 16;
inline constexpr int
    kObjCSwiftOnceSourcesObjcGetterThunkContractPlainDepthLimit = 16;
inline constexpr int
    kObjCSwiftOnceSourcesObjcEffectfulThunkContractVisitDepthLimit = 128;
inline constexpr int
    kObjCSwiftOnceSourcesDiscardableCallbackReturnVisitDepthLimit = 16;
inline constexpr int kObjCSwiftOnceSourcesRetainedCallbackReturnDepthLimit = 16;
inline constexpr int
    kObjCSwiftOnceSourcesOnceContextLivenessViewCopyCopyDepthLimit = 128;
inline constexpr int
    kObjCSwiftOnceSourcesNestedCallbackContractVisitDepthLimit = 128;
inline constexpr int
    kObjCSwiftOnceSourcesNestedCallbackContractVisitContractInitializersSizeLimit =
        32;
inline constexpr int
    kObjCSwiftOnceSourcesIndependentCallbackChainVerifyDepthLimit = 16;
inline constexpr int kObjCSwiftOnceSourcesCopyContractFlowNodesSizeLimit = 512;
inline constexpr int kObjCSwiftOnceSourcesCopyContractFindCallsDepthLimit = 64;
inline constexpr int kObjCSwiftOnceSourcesCopyContractResolveDepthLimit = 64;
inline constexpr int kObjCSwiftOnceSourcesCopyContractScanScanDepthLimit = 64;
inline constexpr int
    kObjCSwiftOnceSourcesBindSwiftOnceSourceReferencesCopyCopyDepthLimit = 128;
inline constexpr int
    kObjCSwiftOnceSourcesProjectSwiftOnceNestedCallbacksDepthLimit = 16;

// lib/sdk/capi/ObjCSynchronizedFrameSource.h
inline constexpr int
    kObjCSynchronizedFrameSourceProveObjCSynchronizedPrivateFrameNormalExitEntryLimit =
        16384;
inline constexpr int
    kObjCSynchronizedFrameSourceProveObjCSynchronizedPrivateFrameBytesLimit =
        512;
inline constexpr int
    kObjCSynchronizedFrameSourceProjectObjCSynchronizedFrameReceiverFrameOffsetDepthLimit =
        32;
inline constexpr int
    kObjCSynchronizedFrameSourceProjectObjCSynchronizedFrameReceiverFrameOffsetDeltaLimit =
        8192;
inline constexpr int
    kObjCSynchronizedFrameSourceProjectObjCSynchronizedFrameReceiverFrameOffsetResultLimit =
        8192;
inline constexpr int
    kObjCSynchronizedFrameSourceProjectObjCSynchronizedFrameReceiverIsReceiverDepthLimit =
        32;

// lib/sdk/capi/ObjCSynchronizedSource.h
inline constexpr int
    kObjCSynchronizedSourceObjcSynchronizedNonCallGapEndBeginLimit = 16384;
inline constexpr int
    kObjCSynchronizedSourceObjcSynchronizedInterleavedRangesSitesSizeLimit =
        129;
inline constexpr int
    kObjCSynchronizedSourceProveObjCSynchronizedRegisterReceiverCleanupRegionEndFunctionEntryLimit =
        16384;
inline constexpr int
    kObjCSynchronizedSourceObjcSynchronizedNormalSuffixValidLandingEntryLimit =
        16384;
inline constexpr int
    kObjCSynchronizedSourceProveObjCSynchronizedInterleavedCleanupRegionsFrontLandingFunctionEntryLimit =
        16384;
inline constexpr int
    kObjCSynchronizedSourceProveObjCSynchronizedSequentialCleanupLandingFunctionEntryLimit =
        16384;
inline constexpr int
    kObjCSynchronizedSourceProveObjCSynchronizedSplitResumeCleanupEHItaniumCallSitesSizeLimit =
        129;
inline constexpr int
    kObjCSynchronizedSourceProveObjCSynchronizedSplitResumeCleanupLandingFunctionEntryLimit =
        16384;
inline constexpr int
    kObjCSynchronizedSourceObjcSynchronizedSourceBodyLimitMinArgument2 =
        16 * 1024 * 1024;
inline constexpr int
    kObjCSynchronizedSourceObjcSynchronizedSuspendRuntimeCallsCountLimit = 256;
inline constexpr int
    kObjCSynchronizedSourceObjcSynchronizedUnlockReturnsNextExitEndLimit =
        16 * 1024 * 1024;
inline constexpr int
    kObjCSynchronizedSourceAddObjCSynchronizedSequentialCleanupLifetimesSizeLimit =
        16;
inline constexpr int
    kObjCSynchronizedSourceAddObjCSynchronizedSequentialCleanupProofNormalEnterCallsSizeLimit =
        8;
inline constexpr int
    kObjCSynchronizedSourceAddObjCSynchronizedSequentialCleanupProofNormalExitCallsSizeLimit =
        16;
inline constexpr int
    kObjCSynchronizedSourceAddObjCSynchronizedReceiverCleanupExitsSizeLimit =
        256;

// lib/sdk/capi/ObjCUnwindSource.h
inline constexpr int kObjCUnwindSourceObjcUnwindScalarSliceDepthLimit = 32;

// lib/sdk/capi/ResourceHandles.cpp
inline constexpr int kResourceHandlesReportErrorLengthMinArgument2 = 65536;

// lib/sdk/capi/SourceExpressionIdentity.h
inline constexpr int
    kSourceExpressionIdentitySameStructuredSourceBodyCompareDepthLimit = 64;

// lib/sdk/capi/SourceReceiptOccurrences.h
inline constexpr int
    kSourceReceiptOccurrencesOrdinarySourceReceiptOccurrencesAppendDepthLimit =
        64;

// lib/sdk/capi/SourceSwiftConsumedInputProjection.h
inline constexpr int
    kSourceSwiftConsumedInputProjectionSameExpressionDepthLimit = 64;
inline constexpr int
    kSourceSwiftConsumedInputProjectionValidCurrentLowDecodedInstructionCountLimit =
        256;

// lib/sdk/capi/SourceSwiftOpaqueValueProjection.h
inline constexpr int kSourceSwiftOpaqueValueProjectionSameExpressionDepthLimit =
    64;
inline constexpr int
    kSourceSwiftOpaqueValueProjectionValidLowDecodedInstructionCountLimit = 256;
inline constexpr int kSourceSwiftOpaqueValueProjectionValidHintsSizeLimit = 16;
inline constexpr int
    kSourceSwiftOpaqueValueProjectionValidOptionsOnlyFunctionEntriesSizeLimit =
        5;

// lib/sdk/capi/SourceSwiftValueConstructorProjection.h
inline constexpr int
    kSourceSwiftValueConstructorProjectionMessageCallsExpectedSizeLimit = 32;
inline constexpr int
    kSourceSwiftValueConstructorProjectionSameExpressionDepthLimit = 64;
inline constexpr int
    kSourceSwiftValueConstructorProjectionValidISecondDecodedInstructionCountLimit =
        1024;
inline constexpr int
    kSourceSwiftValueConstructorProjectionValidOptionsOnlyFunctionEntriesSizeLimit =
        32;

// lib/sdk/capi/SwiftABIProjectionPlan.h
inline constexpr int kSwiftABIProjectionPlanTypeDepthLimit = 16;

// lib/sdk/capi/SwiftMangledSourceABI.h
inline constexpr int
    kSwiftMangledSourceABISwiftMangledArrayStringValueInitializerSourceABILowBlocks0OpsSizeLimit =
        4096;

// lib/sdk/capi/SwiftRuntimeSignatures.h
inline constexpr int kSwiftRuntimeSignaturesSameRuntimePropertyTypeDepthLimit =
    8;

// lib/sdk/capi/SwiftSourceSignatures.h
inline constexpr int kSwiftSourceSignaturesIdentifierTextSizeLimit = 1024;
inline constexpr int kSwiftSourceSignaturesTextValueSizeLimit = 65536;
inline constexpr int kSwiftSourceSignaturesTypeDepthLimit = 8;
inline constexpr int kSwiftSourceSignaturesSignatureSymbolSizeLimit = 65536;
inline constexpr int kSwiftSourceSignaturesSignatureParametersSizeLimit = 64;

// lib/sdk/capi/TauriInitializer.cpp
inline constexpr int kTauriInitializerImageSegmentsSizeLimit = 128;

// lib/sdk/capi/ObjCSourceBindings.h
inline constexpr int kObjCSourceBindingsFindDescriptorILimit = 128;

// lib/sdk/capi/ObjCSynchronizedSource.h
inline constexpr int
    kObjCSynchronizedSourceProveObjCSynchronizedRegisterReceiverCleanupRegionBeginAddressLimit =
        32;
inline constexpr int
    kObjCSynchronizedSourceProveObjCSynchronizedInterleavedCleanupRegionsFrontBeginAddressLimit =
        128;
inline constexpr int
    kObjCSynchronizedSourceObjcSynchronizedSavedLocalArgumentEndBeginLimit =
        128;

inline constexpr int kMaxDataItemArrayElementBytes = 65536;
inline constexpr int kMaxDataItemArrayBytes = 64 * 1024 * 1024;
// lib/sdk/capi/SourceExpressionIdentity.h
inline constexpr int kMaxSourceExpressionIdentityDepth = 128;

// lib/sdk/capi/SourceSwiftValueConstructorProjection.h
inline constexpr int kMaxSwiftValueConstructorRounds = 16;

// lib/sdk/capi/ObjCByValueCopySources.h
inline constexpr int kMaxObjCByValueCopyRounds = 16;

// lib/sdk/capi/ObjCSourceBindings.h
inline constexpr int kMaxSwiftSymbolicTypeBytes = 128;

// lib/sdk/capi/ObjCSourceBindings.h
inline constexpr int kMaxSwiftSymbolicTypeReferenceOffset =
    kMaxSwiftSymbolicTypeBytes - 5;

// lib/sdk/capi/ObjCSynchronizedSource.h
inline constexpr int kMaxSynchronizedSourceTokenBytes = 128;

// lib/sdk/capi/ProgramMain.cpp
inline constexpr unsigned kProgramMainEntryInstructions = 64;

} // namespace neverd::sdk

#endif // NEVERD_SDK_LIMITS_H
