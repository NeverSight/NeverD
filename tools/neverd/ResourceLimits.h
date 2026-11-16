//===- ResourceLimits.h - Resource limits and defaults -----*- C++ -*-===//
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

#ifndef NEVERD_TOOLS_NEVERD_RESOURCELIMITS_H
#define NEVERD_TOOLS_NEVERD_RESOURCELIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::cli {

// tools/neverd/cmd/NeverDCmdStrings.cpp
/// Functions one neverd_string_refs_json call decodes.
inline constexpr int kNeverDCmdStringsReferencePageFunctions = 1024;

// tools/neverd/cmd/NeverDCmdTables.cpp
// Every page, in function entry order.
inline constexpr int kNeverDCmdTablesFunctionsPerPage = 4096;

// tools/neverd/cmd/NeverDCmdXrefs.cpp
/// Functions one neverd_code_refs_json call decodes, and slots one
/// neverd_pointer_refs_json call reads.
inline constexpr int kNeverDCmdXrefsReferencePageFunctions = 1024;
inline constexpr int kNeverDCmdXrefsReferencePageSlots = 65536;

// tools/neverd/mobile/MobileIOS.cpp
inline constexpr size_t kMobileIOSChunk = 64 * 1024;

// tools/neverd/mobile/MobileIOSInternal.h
// Internal same-executable protocol. Control files are temporary workspace
// artifacts; published output accounting is carried explicitly without renewal.
inline constexpr uint64_t kMobileIOSInternalWorkerRequestByteLimit = 4096;

// tools/neverd/cmd/NeverDCmdExport.cpp
inline constexpr int kNeverDCmdExportRunExportBytesLimit = 32 * 1024 * 1024;

// tools/neverd/cmd/NeverDCmdPipeline.cpp
inline constexpr int
    kNeverDCmdPipelineValidateSourcePatchArgumentsPatchCompilerTimeoutLimit =
        3600;
inline constexpr int kNeverDCmdPipelineRunDecompileSizeLimit = 128 * 1024;
inline constexpr int kNeverDCmdPipelineRunDecompileBufferGetBufferSizeLimit =
    128 * 1024;
/// Bound diagnostic output while checking every loaded function.
inline constexpr int kDecompileCheckMaxFailureSamples = 32;
inline constexpr size_t kDecompileCheckMaxReasonBytes = 180;
/// Report progress and return freed allocator pages once per completed batch.
inline constexpr uint64_t kDecompileCheckReportFunctions = 1000;
/// Also report slow batches after this many seconds.
inline constexpr int kDecompileCheckReportSeconds = 30;

// tools/neverd/cmd/NeverDCmdSearch.cpp
inline constexpr int kNeverDCmdSearchRunSearchMaxResultsLimit = 4096;

// tools/neverd/cmd/NeverDCmdTypeFit.cpp
inline constexpr int kNeverDCmdTypeFitRunTypesFitTypesLimit = 500;

// tools/neverd/cmd/NeverDCmdWeb.cpp
inline constexpr int kNeverDCmdWebRunWebPagePageSelected = 128;
inline constexpr int kNeverDCmdWebRunWebPagePageDefault = 128;
inline constexpr int kNeverDCmdWebRunWebChunkMinArgument2 = 65536;

// tools/neverd/cmd/NeverDCmdWebAssessment.cpp
inline constexpr int kNeverDCmdWebAssessmentRunWebAssessmentBytesLimit =
    32 * 1024 * 1024;

// tools/neverd/mobile/MobileArchive.cpp
inline constexpr int kMobileArchiveExtractEntryBytesMinArgument1 = 65536;
inline constexpr int kMobileArchiveExtractEntryInputMinArgument1 = 65536;
inline constexpr int kMobileArchiveReadDirectoryFooterStartMinArgument2 = 65557;
inline constexpr int kMobileArchiveReadDirectoryLengthLimit = 44;
inline constexpr int kMobileArchiveReadDirectoryDirectorySizeCursorLimit = 46;

// tools/neverd/mobile/MobileCommon.cpp
inline constexpr int kMobileCommonOutputFileWriteCountMinArgument1 = 65536;
inline constexpr int kMobileCommonOutputFileWriteWrittenMinArgument1 = 65536;

// tools/neverd/mobile/MobileDalvikJava.cpp
inline constexpr int kMobileDalvikJavaLocalProjectionSeenSizeLimit = 128;
inline constexpr int kMobileDalvikJavaRecoverJavaSeenSizeLimit = 128;

// tools/neverd/mobile/MobileDalvikSmali.cpp
inline constexpr int kMobileDalvikSmaliIntegerDigitsSizeLimit = 32;
inline constexpr int kMobileDalvikSmaliPayloadResultKeysSizeLimit = 65535;
inline constexpr int
    kMobileDalvikSmaliInstructionConstClassConstStringConstWideConstMethodTypeCountLimit =
        64;

// tools/neverd/mobile/MobileIOS.cpp
inline constexpr int kMobileIOSValidateWorkerReportSignaturesMinArgument2 =
    32 * 1024 * 1024;

// tools/neverd/mobile/MobileIOSMetadata.cpp
inline constexpr int kMobileIOSMetadataSymbolNameNameMinArgument1 = 16384;
inline constexpr int kMobileIOSMetadataSelectSliceCountLimit = 64;
inline constexpr int
    kMobileIOSMetadataSwiftMetadataAddSymbolT0RawNameSizeLimit = 16384;
inline constexpr int kMobileIOSMetadataSwiftMetadataCommandCountLimit = 100000;
inline constexpr int kMobileIOSMetadataSwiftMetadataSectionSize4Limit = 100000;

// tools/neverd/mobile/MobileIOSObjC.cpp
inline constexpr int kMobileIOSObjCLayoutIvarsSizeLimit = 100000;
inline constexpr int kMobileIOSObjCRenderComparisonWorkLimit = 1000000;
inline constexpr int kMobileIOSObjCObjcTypesESizeLimit = 4096;
inline constexpr int kMobileIOSObjCObjcSourcesDiagnosticBytesLimit = 512;

// tools/neverd/mobile/MobileIOSPlist.cpp
inline constexpr int kMobileIOSPlistAllocationLimitMinArgument2 =
    64 * 1024 * 1024;
inline constexpr int kMobileIOSPlistBinaryDepthLimit = 128;
inline constexpr int kMobileIOSPlistBinaryCountLimit = 100000;
inline constexpr int kMobileIOSPlistXmlDepthLimit = 128;
inline constexpr int kMobileIOSPlistXmlArrayASizeLimit = 100000;
inline constexpr int kMobileIOSPlistParseDataSizeLimit = 16 * 1024 * 1024;

// tools/neverd/mobile/MobileIOSSwift.cpp
inline constexpr int
    kMobileIOSSwiftCoverageCompilerGeneratedFromTypeSourceRepresentationCompilerProjectionKindRuntimeContextNameSourceInitializerDeclarationKindRequiresRuntimeSourceProofESizeLimit =
        128;
inline constexpr int
    kMobileIOSSwiftCoverageRequiresRuntimeSourceProofTextSizeLimit = 8192;
inline constexpr int kMobileIOSSwiftSourcesMinArgument2 = 32 * 1024 * 1024;

// tools/neverd/mobile/MobileIOSSwiftSignatures.cpp
inline constexpr int kMobileIOSSwiftSignaturesTypeDepthLimit = 8;
inline constexpr int kMobileIOSSwiftSignaturesSwiftSignatureMangledSizeLimit =
    8000;

// tools/neverd/mobile/MobileProcess.cpp
inline constexpr int kMobileProcessTailSizeMinArgument2 = 4096;
inline constexpr int kMobileProcessContainsOnlyExitedProcessesBytesLimit =
    16 * 1024 * 1024;
inline constexpr int kMobileProcessPauseMilliseconds = 10;
inline constexpr int kMobileProcessChildCommandSizeLimit = 32767;
inline constexpr int kMobileProcessRunToolDrainDeadlineSeconds = 1;
inline constexpr int kMobileProcessRunToolMilliseconds = 5;
inline constexpr int kMobileProcessRunToolNextScanMilliseconds = 250;

// tools/neverd/NeverDCLIOptions.cpp
/// Default for --max-func. See the option description for sentinel meanings.
inline constexpr int kDefaultMaxFunc = 0;
/// Default for --vm-max-nodes. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultVMMaxNodes = 4096;
/// Default for --vm-max-contexts. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultVMMaxContexts = 64;
/// Default for --vm-max-operations. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultVMMaxOperations = 262144;
/// Default for --param. See the option description for sentinel meanings.
inline constexpr int kDefaultTypesParam = -1;
/// Default for --limit. See the option description for sentinel meanings.
inline constexpr int kDefaultTypesLimit = 10;
/// Default for --size. See the option description for sentinel meanings.
inline constexpr int kDefaultHexSize = 256;
/// Default for --max-paths. See the option description for sentinel meanings.
inline constexpr int kDefaultSymbolicMaxPaths = 64;
/// Default for --max-steps. See the option description for sentinel meanings.
inline constexpr unsigned kDefaultSymbolicMaxSteps = 1u << 16;
/// Default for --max-block-visits. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultSymbolicMaxBlockVisits = 3;
/// Default for --max-paths. See the option description for sentinel meanings.
inline constexpr int kDefaultSafetyMaxPaths = 0;
/// Default for --max-steps. See the option description for sentinel meanings.
inline constexpr int kDefaultSafetyMaxSteps = 0;
/// Default for --max-loop. See the option description for sentinel meanings.
inline constexpr int kDefaultSafetyMaxLoop = 0;
/// Default for --max-call-depth. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultSafetyMaxCallDepth = 0;
/// Default for --max-summary-iterations. See the option description for
/// sentinel meanings.
inline constexpr int kDefaultSafetyMaxSummaryIterations = 0;
/// Default for --solver-conflicts. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultSafetySolverConflicts = 0;
/// Default for --max-paths. See the option description for sentinel meanings.
inline constexpr int kDefaultPathsMaxPaths = 32;
/// Default for --size. See the option description for sentinel meanings.
inline constexpr int kDefaultItemSize = 1;
/// Default for --element-size. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultItemElementSize = 1;
/// Default for --count. See the option description for sentinel meanings.
inline constexpr int kDefaultItemCount = 0;
/// Default for --boundary. See the option description for sentinel meanings.
inline constexpr int kDefaultItemBoundary = 16;
/// Default for --operand. See the option description for sentinel meanings.
inline constexpr int kDefaultOperandIndex = 0;
/// Default for --delta. See the option description for sentinel meanings.
inline constexpr int kDefaultOperandDelta = 0;
/// Default for --width. See the option description for sentinel meanings.
inline constexpr int kDefaultSimplifyWidth = 32;
/// Default for --max-atoms. See the option description for sentinel meanings.
inline constexpr int kDefaultSimplifyMaxAtoms = 0;
/// Default for --max-work. See the option description for sentinel meanings.
inline constexpr int kDefaultSimplifyMaxWork = 0;
/// Default for --verify-samples. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultSimplifyVerifySamples = 0;
/// Default for --max-cost. See the option description for sentinel meanings.
inline constexpr int kDefaultSimplifyMaxCost = 0;
/// Default for --max-samples. See the option description for sentinel meanings.
inline constexpr int kDefaultSimplifyMaxSamples = 0;
/// Default for --max-leaves. See the option description for sentinel meanings.
inline constexpr int kDefaultSimplifyMaxLeaves = 0;
/// Default for --max-constants. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultSimplifyMaxConstants = 0;
/// Default for --stochastic-slots. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultSimplifyStochasticSlots = 0;
/// Default for --stochastic-restarts. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultSimplifyStochasticRestarts = 0;
/// Default for --stochastic-iterations. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultSimplifyStochasticIterations = 0;
/// Default for --solver-max-conflicts. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultSimplifySolverMaxConflicts = 0;
/// Default for --solver-max-propagations. See the option description for
/// sentinel meanings.
inline constexpr int kDefaultSimplifySolverMaxPropagations = 0;
/// Default for --solver-max-watch-visits. See the option description for
/// sentinel meanings.
inline constexpr int kDefaultSimplifySolverMaxWatchVisits = 0;
/// Default for --solver-timeout-ms. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultSimplifySolverTimeoutMs = 0;
/// Default for --max-rounds. See the option description for sentinel meanings.
inline constexpr int kDefaultOptimizeIRMaxRounds = 0;
/// Default for --synthesis-max-cost. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultOptimizeIRSynthesisMaxCost = 0;
/// Default for --synthesis-max-samples. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultOptimizeIRSynthesisMaxSamples = 0;
/// Default for --synthesis-verify-samples. See the option description for
/// sentinel meanings.
inline constexpr int kDefaultOptimizeIRSynthesisVerifySamples = 0;
/// Default for --synthesis-max-work. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultOptimizeIRSynthesisMaxWork = 0;
/// Default for --synthesis-max-leaves. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultOptimizeIRSynthesisMaxLeaves = 0;
/// Default for --synthesis-max-constants. See the option description for
/// sentinel meanings.
inline constexpr int kDefaultOptimizeIRSynthesisMaxConstants = 0;
/// Default for --synthesis-stochastic-slots. See the option description for
/// sentinel meanings.
inline constexpr int kDefaultOptimizeIRSynthesisStochasticSlots = 0;
/// Default for --synthesis-stochastic-restarts. See the option description for
/// sentinel meanings.
inline constexpr int kDefaultOptimizeIRSynthesisStochasticRestarts = 0;
/// Default for --synthesis-stochastic-iterations. See the option description
/// for sentinel meanings.
inline constexpr int kDefaultOptimizeIRSynthesisStochasticIterations = 0;
/// Default for --solver-max-conflicts. See the option description for sentinel
/// meanings.
inline constexpr int kDefaultOptimizeIRSolverMaxConflicts = 0;
/// Default for --solver-max-propagations. See the option description for
/// sentinel meanings.
inline constexpr int kDefaultOptimizeIRSolverMaxPropagations = 0;
/// Default for --solver-max-watch-visits. See the option description for
/// sentinel meanings.
inline constexpr int kDefaultOptimizeIRSolverMaxWatchVisits = 0;
/// Default for --generation. See the option description for sentinel meanings.
inline constexpr int kDefaultTranslateObjectGeneration = 0;

// tools/neverd/cmd/NeverDCmdWebAssessment.cpp
inline constexpr int kMaxCLIWebAssessmentSelections = 16;

// tools/neverd/mobile/MobileIOSMetadata.cpp
inline constexpr int kMobileIOSMetadataSwiftMetadataILimit = 16384;

// tools/neverd/mobile/MobileIOSObjC.cpp
inline constexpr int kMaxObjCTypeDiagnostics = 8;

// tools/neverd/mobile/MobileProcess.cpp
inline constexpr int kMaxMobilePipeDrainReads = 16;

// tools/neverd/mobile/MobileIOSMetadata.cpp
inline constexpr int kMaxIOSLoadCommands = 100000;

// tools/neverd/mobile/MobileIOSMetadata.cpp
inline constexpr int kMaxIOSSections = 100000;

// tools/neverd/mobile/MobileIOSPlist.cpp
inline constexpr int kMaxPlistObjects = 100000;

// tools/neverd/cmd/NeverDCmdWebBehaviorDiff.cpp
inline constexpr int kMaxWebBehaviorDiffOffset = 16384;

// tools/neverd/cmd/NeverDCmdWebSEA.cpp
inline constexpr int kMaxWebSEAOffset = 32768;

// tools/neverd/cmd/NeverDCmdWebArchive.cpp
inline constexpr int kMaxWebArchiveOffset = 32768;

// tools/neverd/cmd/NeverDCmdWebPackages.cpp
inline constexpr int kMaxWebPackagesOffset = 32768;

// tools/neverd/cmd/NeverDCmdWebPkg.cpp
inline constexpr int kMaxWebPkgOffset = 16384;

// tools/neverd/cmd/NeverDCmdPipeline.cpp
inline constexpr int kMaxPipelineAggregateSourceBytes = 64 * 1024 * 1024;

// tools/neverd/mobile/MobileDalvikJava.cpp
inline constexpr int kMaxDalvikJavaRegisters = 1024;

// tools/neverd/cmd/NeverDCmdWebBehavior.cpp
inline constexpr int kMaxWebBehaviorOffset = 8192;

// tools/neverd/cmd/NeverDCmdWebNexe.cpp
inline constexpr int kMaxWebNexeOffset = 4096;

// tools/neverd/cmd/NeverDCmdWebWails.cpp
inline constexpr int kMaxWebWailsOffset = 4096;

// tools/neverd/cmd/NeverDCmdWebAssessment.cpp
inline constexpr int kMaxWebAssessmentOffset = 8192;

} // namespace neverd::limits::cli

#endif // NEVERD_TOOLS_NEVERD_RESOURCELIMITS_H
